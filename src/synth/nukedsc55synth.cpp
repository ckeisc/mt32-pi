//
// nukedsc55synth.cpp
//
// mt32-pi - A baremetal MIDI synthesizer for Raspberry Pi
// Copyright (C) 2020-2023 Dale Whinham <daleyo@gmail.com>
//
// This file is part of mt32-pi.
//
// mt32-pi is free software: you can redistribute it and/or modify it under the
// terms of the GNU General Public License as published by the Free Software
// Foundation, either version 3 of the License, or (at your option) any later
// version.
//
// mt32-pi is distributed in the hope that it will be useful, but WITHOUT ANY
// WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
// FOR A PARTICULAR PURPOSE. See the GNU General Public License for more
// details.
//
// You should have received a copy of the GNU General Public License along with
// mt32-pi. If not, see <http://www.gnu.org/licenses/>.
//

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <span>
#include <string>
#include <strings.h>
#include <vector>

#include <circle/cputhrottle.h>
#include <circle/logger.h>
#include <circle/memory.h>
#include <circle/sched/scheduler.h>
#include <circle/synchronize.h>
#include <circle/timer.h>
#include <fatfs/ff.h>

#include "config.h"
#include "diaglog.h"
#include "lcd/ui.h"
#include "synth/nukedsc55synth.h"
#include "synth/fir64k48k.h"
#include "utility.h"

// Nuked-SC55 backend headers are only included here (this file is compiled
// as C++20; see Kernel.mk). The compat shim must come first: it supplies
// the C++23 library functions the bare-metal libstdc++ lacks (see the
// header itself); it is also force-included into the backend build.
#include "nukedcirclecompat.h"

#include "emu.h"
#include "nuked_profile.h"
#include "pcm.h"
#include "rom.h"
#include "split_thread_circle.h"
#include "rom_io.h"
#include "sha256.h"
#include "standard_romsets.h"

LOGMODULE("nukedsc55");

// Dual-core split state (see split_thread_circle.h): owned by the
// synth instance (m_pSplitState), published here while the split is
// active so the backend's dispatch and hooks can reach it. Null
// whenever the split is inactive (single-threaded path, or before
// activation / after teardown) -- the backend then calls PCM_Read /
// PCM_Write directly.
ThreadSplitStateCircle* g_splitT = nullptr;

// Sound Canvas ROM sets live in their own directory alongside the MT-32
// "roms" and "soundfonts" directories (shared with the EmuSC engine; the
// two engines can use the same files on the SD card)
const char* const NukedDisks[] = { "SD", "USB" };
const char NukedROMDirectory[] = "roms/sc55";

// Sanity limits for the ROM directory scan. A complete SC-55 set is five
// files totalling ~3.3 MiB; these bounds leave generous headroom while
// keeping a pathological directory from exhausting the heap.
constexpr size_t MaxROMFiles    = 24;
constexpr size_t MaxROMFileSize = 4 * 1024 * 1024;

// The emulator produces frames at its PCM rate (64 kHz for the SC-55) and
// each Step() advances roughly 500-600 ns of emulated time, so ~30 steps
// are needed per output frame. The step budget gives an order of magnitude
// of headroom over that.
constexpr u64 StepsPerFrameBudget = 300;

// Peak level (in emulator output units, full scale = 2^29) above which the
// synth is considered active. The SC-55 firmware idles with a small DC
// offset (~0.03 of full scale), so the threshold sits above that.
constexpr s32 ActiveThreshold = 1 << 25;

namespace
{

struct TROMFile
{
	CString Name;
	SHA256_Digest Digest;
	std::vector<uint8_t> Data;
};

bool ReadROMFile(const char* pPath, std::vector<uint8_t>& OutData)
{
	FIL File;
	if (f_open(&File, pPath, FA_READ) != FR_OK)
		return false;

	const FSIZE_t nSize = f_size(&File);
	OutData.resize(static_cast<size_t>(nSize));

	size_t nReadTotal = 0;
	bool bOK          = true;
	while (nReadTotal < OutData.size())
	{
		UINT nRead = 0;
		if (f_read(&File, OutData.data() + nReadTotal, static_cast<UINT>(OutData.size() - nReadTotal), &nRead) != FR_OK || nRead == 0)
		{
			bOK = false;
			break;
		}
		nReadTotal += nRead;
	}

	f_close(&File);
	return bOK && nReadTotal == OutData.size();
}

// Scans the ROM directories (SD first, then USB) and hashes every
// plausible file it finds. Files from both disks are pooled: a complete
// set on either disk will match, even if the other holds a partial set.
// Returns false if no usable files were found at all.
bool ScanROMFiles(std::vector<TROMFile>& OutFiles)
{
	for (auto pDisk : NukedDisks)
	{
		CString DirectoryPath;
		DirectoryPath.Format("%s:/%s", pDisk, NukedROMDirectory);

		DIR Dir;
		FILINFO FileInfo;
		FRESULT Result = f_findfirst(&Dir, &FileInfo, DirectoryPath, "*");

		while (Result == FR_OK && *FileInfo.fname)
		{
			if (!(FileInfo.fattrib & (AM_DIR | AM_HID | AM_SYS)) && FileInfo.fsize > 0 && FileInfo.fsize <= MaxROMFileSize && OutFiles.size() < MaxROMFiles)
			{
				CString ROMPath;
				ROMPath.Format("%s/%s", static_cast<const char*>(DirectoryPath), FileInfo.fname);

				TROMFile File;
				if (ReadROMFile(ROMPath, File.Data) && SHA256_HashBytes(File.Data, File.Digest))
				{
					File.Name = ROMPath;
					OutFiles.push_back(std::move(File));
				}
				else
				{
					LOGWARN("Failed to read/hash ROM file %s", static_cast<const char*>(ROMPath));
				}
			}

			Result = f_findnext(&Dir, &FileInfo);
		}

		f_closedir(&Dir);
	}

	return !OutFiles.empty();
}

// Finds a complete romset definition whose ROM hashes all match scanned
// files. MK1 definitions (the SC-55) are preferred, as with the other
// Sound Canvas engine the plain SC-55 set is the reference set; any other
// complete set is accepted as a fallback.
const RomsetDefinition* MatchRomset(std::vector<TROMFile>& Files, TROMFile* pMatchedFiles[ROMLOCATION_COUNT])
{
	const auto Definitions = GetStandardRomsetDefinitions();

	for (int nPass = 0; nPass < 2; ++nPass)
	{
		for (const RomsetDefinition& Definition : Definitions)
		{
			if (nPass == 0 && Definition.romset != Romset::MK1)
				continue;

			bool bComplete = true;
			for (size_t i = 0; i < ROMLOCATION_COUNT; ++i)
				pMatchedFiles[i] = nullptr;

			for (const RomLocation Location : Definition.GetValidLocations())
			{
				const SHA256_Digest& Wanted = Definition.GetHash(Location);
				TROMFile* pMatch            = nullptr;
				for (TROMFile& File : Files)
				{
					if (File.Digest == Wanted)
					{
						pMatch = &File;
						break;
					}
				}

				if (!pMatch)
				{
					bComplete = false;
					break;
				}

				pMatchedFiles[static_cast<size_t>(Location)] = pMatch;
			}

			if (bComplete)
				return &Definition;
		}
	}

	return nullptr;
}

const char* FriendlyModelName(Romset Romset)
{
	switch (Romset)
	{
		case Romset::MK1:
			return "SC-55";
		case Romset::MK2:
			return "SC-55mkII";
		case Romset::SC155:
			return "SC-155";
		case Romset::SC155MK2:
			return "SC-155mkII";
		case Romset::ST:
			return "SC-55ST";
		case Romset::CM300:
			return "CM-300";
		case Romset::SCB55:
			return "SCB-55";
		case Romset::JV880:
			return "JV-880";
		case Romset::RLP3237:
			return "RLP-3237";
		default:
			return "Sound Canvas";
	}
}

} // namespace

CNukedSC55Synth::CNukedSC55Synth(unsigned nSampleRate, float nGain, bool bOversampling, TNukedResampler Resampler)
	: CSynthBase(nSampleRate),
	  m_pSplitState(nullptr),
	  m_bSplitActive(false),
	  m_bPCMIdle(true),
	  m_bSplitWedged(false),
	  m_bSplitEligible(false),
	  m_pEmulator(nullptr),
	  m_pRomsetInfo(nullptr),
	  m_nGain(nGain),
	  m_bOversampling(bOversampling),
	  m_nVolume(100),
	  m_nNativeRate(0),
	  m_dResampleRatio(1.0),
	  m_dReadFrac(0.0),
	  m_nWrittenFrames(0),
	  m_nLastActiveFrame(0),
	  m_SampleA{},
	  m_SampleB{},
	  m_bWindowValid(false),
	  m_bFirActive(false),
	  m_FirHist{},
	  m_FirFed(0),
	  m_FirOutIndex(0),
	  m_FirPhase(0),
	  m_CubicPrev{},
	  m_CubicA{},
	  m_CubicB{},
	  m_CubicNext{},
	  m_bCubicValid(false),
	  m_bCubicActive(false),
	  m_Resampler(Resampler),
	  m_SampleRing{},
	  m_nSampleRingFill(0),
	  m_nSampleRingDrops(0),
	  m_Stats{},
	  m_StatsLock(TASK_LEVEL),
	  m_nHeldNotes(0),
	  m_bSustain(false),
	  m_bSustainedNotes(false),
	  m_ModelName{},
	  m_ROMVersion{}
{
}

const char* CNukedSC55Synth::H8EngineName(TH8Engine Engine)
{
	switch (Engine)
	{
	case TH8Engine::Legacy:
		return "legacy";
	case TH8Engine::JIT:
		return "jit";
	default:
		return "decoder2";
	}
}

namespace
{

H8Engine ToBackendEngine(CNukedSC55Synth::TH8Engine Engine)
{
	switch (Engine)
	{
	case CNukedSC55Synth::TH8Engine::Legacy:
		return H8Engine::Legacy;
	case CNukedSC55Synth::TH8Engine::JIT:
		return H8Engine::JIT;
	default:
		return H8Engine::Decoder2;
	}
}

}

CNukedSC55Synth::~CNukedSC55Synth()
{
	// Park the PCM task before destroying anything it touches. (In
	// practice synths are never deleted mid-session; this covers the
	// init-failure path, where the split is not active yet and this
	// is a no-op.)
	StopSplitPCM();

	if (m_pSplitState)
		delete m_pSplitState;

	if (m_pEmulator)
		delete m_pEmulator;

	if (m_pRomsetInfo)
		delete m_pRomsetInfo;
}

void CNukedSC55Synth::OnSample(s32 nLeft, s32 nRight)
{
	// Activity detection works on the raw emulator output (before the
	// float normalization) so the threshold is in stable units.
	if (nLeft > ActiveThreshold || nLeft < -ActiveThreshold || nRight > ActiveThreshold || nRight < -ActiveThreshold)
		m_nLastActiveFrame = m_nWrittenFrames;

	// Normalize() in the backend maps full scale (2^29) to 1.0f
	// The backend idles at the SC-55 DAC mid-scale bias (0x1000000),
	// i.e. +0.03125 DC. A sigma-delta DAC can turn a constant DC input
	// into an audible idle tone, so center the ring at zero. This matches
	// a real SC-55, whose analog output is AC-coupled, and is sonically
	// neutral for music (verified on hardware: the idle hum is gone).
	// Normalization happens here (at production time), exactly as
	// before, so the resampler in RenderFrames() sees the same float
	// values it always did; only the ring changed (CRingBuffer
	// instead of the masked array, so the callback is safe from any
	// core, with or without m_Lock).
	constexpr float Scale = 1.0f / 536870912.0f;
	constexpr float DCBias = 16777216.0f / 536870912.0f;
	const TSplitSample Frame{ nLeft * Scale - DCBias, nRight * Scale - DCBias };
	// Fill and drop counters are maintained with atomic add/sub:
	// core 3 enqueues here while core 2 dequeues in RenderFrames,
	// and a plain volatile read-modify-write on either side could
	// lose an update (harmless for the loop bound -- see the header
	// -- but needless noise in the diag). The ring itself provides
	// the data ordering; these counters are only a loop bound.
	if (m_SampleRing.Enqueue(Frame))
		__atomic_add_fetch(&m_nSampleRingFill, static_cast<size_t>(1), __ATOMIC_RELAXED);
	else
		m_nSampleRingDrops = m_nSampleRingDrops + 1;
	m_nWrittenFrames = m_nWrittenFrames + 1;
}

bool CNukedSC55Synth::StopSplitPCM()
{
	if (!m_bSplitActive)
		return true;

	// Park the PCM task: it finishes its current batch (bounded:
	// at most 128 ticks, a few ms), then sees the flag, acks idle,
	// and yields until reactivated. The MCU thread is not stepping
	// here (the caller holds m_Lock, as RenderFrames does), so once
	// the PCM task has parked, nothing touches the backend anymore.
	m_bSplitActive = false;
	SplitDMB();
	// Bounded wait (5 s of wall clock): a parked task acks within
	// milliseconds, so a timeout means core 3 is wedged inside a
	// batch. On timeout the split is marked wedged and g_splitT is
	// deliberately NOT cleared: resetting or reactivating under a
	// possibly-live core 3 would corrupt the backend under it (the
	// standalone test kernel showed exactly this failure mode).
	// The only recovery from a wedge is a reboot.
	const u64 nDeadlineUs = CTimer::Get()->GetClockTicks() + 5000000ULL;
	while (!m_bPCMIdle)
	{
		SplitDMB();
		if (CTimer::Get()->GetClockTicks() >= nDeadlineUs)
		{
			m_bSplitWedged = true;
			CDiagLog::Get().LogLine("splitpark_timeout ticks=%llu pcmidle=no fifo_head=%llu fifo_tail=%llu",
			                        static_cast<unsigned long long>(m_pSplitState ? m_pSplitState->ticks : 0),
			                        static_cast<unsigned long long>(m_pSplitState ? m_pSplitState->fifo_head : 0),
			                        static_cast<unsigned long long>(m_pSplitState ? m_pSplitState->fifo_tail : 0));
			return false;
		}
		CScheduler::Get()->Yield();
	}
	SplitDMB();
	g_splitT = nullptr;
	SplitDMB();
	return true;
}

void CNukedSC55Synth::InitSplitState(bool bClearRing)
{
	// Caller guarantees: PCM task parked (or never started) and the
	// MCU thread not stepping (m_Lock held, or pre-activation).
	// Refuse after a park-timeout wedge: the old core-3 batch may
	// still be live, and publishing fresh state under it would
	// corrupt the backend.
	if (m_bSplitWedged)
	{
		CDiagLog::Get().LogLine("splitstate refuse wedged");
		return;
	}
	if (!m_pSplitState)
		m_pSplitState = new ThreadSplitStateCircle;

	m_pSplitState->InitFromPCM(m_pEmulator->GetPCM(), m_pEmulator->GetMCU());

	if (bClearRing)
	{
		// Fresh audio after a reset: the queued frames belong to the
		// pre-reset boot and must not play after the reboot.
		TSplitSample Dummy;
		while (m_SampleRing.Dequeue(Dummy))
		{
		}
		m_nSampleRingFill = 0;
		m_bWindowValid = false;
		m_dReadFrac = 0.0;
	}
	// Activation (bClearRing false) keeps the priming frames: the
	// first RenderFrames() drains the boot silence continuously.

	// Publish order: initialized state, then g_splitT, then the
	// active flag, each separated by a barrier, so the PCM task can
	// never observe the flag before the state it gates on.
	SplitDMB();
	g_splitT = m_pSplitState;
	SplitDMB();
	m_bSplitActive = true;
	SplitDMB();
}

void CNukedSC55Synth::SplitActivate()
{
	// Called on switch-to-Nuked, under m_Lock (see SwitchSynth):
	// no RenderFrames call can be in flight across the transition.
	m_Lock.Acquire();
	if (m_bSplitEligible && !m_bSplitActive && !m_bSplitWedged && m_pEmulator)
		InitSplitState(true);
	m_Lock.Release();
}

void CNukedSC55Synth::SplitDeactivate()
{
	// Called on switch-from-Nuked, under m_Lock: park core 3 so it
	// stops consuming cycles and filling the ring while another
	// synth plays. A timeout leaves the split wedged (and live);
	// nothing further is done with it.
	m_Lock.Acquire();
	if (m_bSplitActive)
		StopSplitPCM();
	m_Lock.Release();
}

void CNukedSC55Synth::LogSplitState(const char* pTag)
{
	if (!CDiagLog::Get().IsEnabled())
		return;
	SplitDMB();
	const bool bActive = m_bSplitActive;
	const bool bIdle = m_bPCMIdle;
	const bool bStateSet = m_pSplitState != nullptr;
	const char* pGlobal = !bStateSet ? "n/a" : (g_splitT == m_pSplitState ? "match" : "mismatch");
	CDiagLog::Get().LogLine("splitstate tag=%s active=%s wedged=%s state=%s gsplit=%s pcmidle=%s model=%s ring=%llu drops=%llu",
	                        pTag,
	                        bActive ? "on" : "off",
	                        m_bSplitWedged ? "yes" : "no",
	                        bStateSet ? "set" : "null",
	                        pGlobal,
	                        bIdle ? "yes" : "no",
	                        m_ModelName,
	                        static_cast<unsigned long long>(m_nSampleRingFill),
	                        static_cast<unsigned long long>(m_nSampleRingDrops));
}


void CNukedSC55Synth::PCMTaskIteration()
{
	if (!m_bSplitActive || !m_pEmulator || !m_pSplitState)
	{
		// Parked: ack the stop handshake (if one is waiting) and idle.
		// This branch always returns, so a stale idle ack (written
		// just before a reactivation) can never be followed by work
		// without a fresh gate read first: the next call re-checks
		// m_bSplitActive, and a stop that already set it false parks
		// the task again instead of letting it render. Conversely,
		// m_bPCMIdle is false throughout any work batch (set below),
		// so a stop waiter can never observe true mid-batch.
		m_bPCMIdle = true;
		SplitDMB();
		CScheduler::Get()->Yield();
		return;
	}

	ThreadSplitStateCircle& S = *m_pSplitState;
	pcm_t& PCM = m_pEmulator->GetPCM();

	// Trim batch chase: core 2 RELEASE-publishes cycles_pub every
	// 8 steps; core 3 renders at most 128 ticks, capped at
	// published-800 (a tick is at most 800 cycles, so no tick ever
	// ends past published -- every register write the tick reads
	// was already published). pcm.cycles is core-3-owned (only
	// PCM_Update advances it), so reading it unlocked is safe.
	const uint64_t nPublished = __atomic_load_n(&S.cycles_pub, __ATOMIC_ACQUIRE);
	const uint64_t nTick = static_cast<uint64_t>(PCM.config.reg_slots + 1) * 25ULL;
	if (nPublished <= PCM.cycles + ThreadSplitStateCircle::MaxTickCycles)
	{
		// Idle: back off 64 yield hints, then yield to the scheduler
		// (the fork has tasks; the minimal kernel spun here).
		S.core3_phase = 1;
		for (unsigned i = 0; i < 64; ++i)
			SplitPause();
		CScheduler::Get()->Yield();
		return;
	}

	m_bPCMIdle = false;
	S.core3_phase = 2;
	const u64 nWorkStartUs = CTimer::Get()->GetClockTicks();

	const uint64_t nBatchEnd = std::min(PCM.cycles + ThreadSplitStateCircle::MaxBatchTicks * nTick,
	                                    nPublished - ThreadSplitStateCircle::MaxTickCycles);
	S.core3_phase = 3;
	PCM_Update(PCM, nBatchEnd);
	S.core3_phase = 4;

	// Publish the render position (the diag line reads this) and
	// the peak lag for the diag line.
	SplitDMB();
	S.pcm_rendered = PCM.cycles;
	const uint64_t nFreshPublished = __atomic_load_n(&S.cycles_pub, __ATOMIC_ACQUIRE);
	const int64_t nLag = static_cast<int64_t>(nFreshPublished - PCM.cycles);
	if (nLag > static_cast<int64_t>(S.max_pcm_lag))
		S.max_pcm_lag = static_cast<uint64_t>(nLag);
	S.pcm_task_us = S.pcm_task_us + CTimer::Get()->GetClockTicks() - nWorkStartUs;
}

bool CNukedSC55Synth::Initialize()
{
	std::vector<TROMFile> Files;
	if (!ScanROMFiles(Files))
	{
		LOGWARN("No ROM files found in %s", NukedROMDirectory);
		return false;
	}

	TROMFile* pMatchedFiles[ROMLOCATION_COUNT];
	const RomsetDefinition* pDefinition = MatchRomset(Files, pMatchedFiles);
	if (!pDefinition)
	{
		LOGWARN("ROM files in %s do not form a complete known romset", NukedROMDirectory);
		return false;
	}

	// Hand the backend the raw (still scrambled) ROM images; LoadRomset()
	// unscrambles the wave ROMs in place, exactly as it does for files it
	// loads itself. The buffers are moved (not copied) out of the scan
	// results so the images are only held once. The RomsetInfo object
	// itself is kept for the emulator's lifetime, but its rom_data is
	// released again after LoadRoms() (see below).
	m_pRomsetInfo = new RomsetInfo;
	for (size_t i = 0; i < ROMLOCATION_COUNT; ++i)
	{
		if (pMatchedFiles[i])
		{
			m_pRomsetInfo->rom_data[i] = std::move(pMatchedFiles[i]->Data);
			LOGNOTE("ROM %s <- %s", ToCString(static_cast<RomLocation>(i)), static_cast<const char*>(pMatchedFiles[i]->Name));
		}
	}
	Files.clear();
	Files.shrink_to_fit();


	RomLoadStatusSet LoadStatus;
	if (!LoadRomset(*m_pRomsetInfo, &LoadStatus))
	{
		LOGERR("LoadRomset failed for romset %s", pDefinition->name);
		delete m_pRomsetInfo;
		m_pRomsetInfo = nullptr;
		return false;
	}


	m_pEmulator = new Emulator;

	EMU_Options Options;
	Options.lcd_backend = nullptr;
	if (!m_pEmulator->Init(Options))
	{
		LOGERR("Emulator init failed");
		delete m_pEmulator;
		m_pEmulator = nullptr;
		delete m_pRomsetInfo;
		m_pRomsetInfo = nullptr;
		return false;
	}

	m_pEmulator->GetPCM().enable_oversampling = m_bOversampling;


	if (!m_pEmulator->LoadRoms(pDefinition->romset, *m_pRomsetInfo))
	{
		LOGERR("LoadRoms failed for romset %s", pDefinition->name);
		delete m_pEmulator;
		m_pEmulator = nullptr;
		delete m_pRomsetInfo;
		m_pRomsetInfo = nullptr;
		return false;
	}

	m_nNativeRate    = PCM_GetOutputFrequency(m_pEmulator->GetPCM());
	m_dResampleRatio = static_cast<double>(m_nNativeRate) / static_cast<double>(m_nSampleRate);

	ApplyResamplerSelection();
	m_pEmulator->SetSampleCallback([](void* pUserData, const AudioFrame<int32_t>& Frame) {
		static_cast<CNukedSC55Synth*>(pUserData)->OnSample(Frame.left, Frame.right);
	}, this);

	// Apply the configured H8 execution path before reset/priming so
	// the whole boot sequence runs on the selected engine.
	m_pEmulator->SetH8Engine(ToBackendEngine(TH8Engine::Decoder2));

	m_pEmulator->Reset();

	// Enable backend phase profiling only when the diag log is on, so
	// normal operation carries none of its cost. Installed before the
	// priming steps below so the boot sequence is profiled too.
	if (CDiagLog::Get().IsEnabled())
		NukedProfileSetClock([]() -> uint64_t { return CTimer::Get()->GetClockTicks(); });

	// LoadRoms() copies every ROM into the backend's internal buffers
	// (Emulator::LoadRom), nothing references rom_data afterwards, and
	// upstream's own frontend releases it at this point too. Freeing the
	// staging images returns several MB to the kernel heap.
	m_pRomsetInfo->PurgeRomData();

	// Run the firmware through (part of) its power-on sequence now so the
	// module is awake by the time it is selected; any remaining boot time
	// elapses during normal rendering, as it would on real hardware.
	u64 nSteps = 0;
	const u64 nStepBudget = 2000000;
	while (m_nWrittenFrames < m_nNativeRate / 10 && nSteps < nStepBudget)
	{
		m_pEmulator->Step();
		++nSteps;
	}

	m_pEmulator->PostSystemReset(EMU_SystemReset::GS_RESET);

	// Dual-core split activation (MK1 romsets only -- JV880/MK2 keep
	// the single-threaded path regardless of the config flag, and the
	// backend dispatch bypasses them too). Priming above ran
	// single-threaded, so the split state starts from the live pcm_t;
	// the priming frames stay queued for a continuous boot.
	// PostSystemReset queued the GS reset bytes into the UART ring,
	// which the MCU consumes in step order once split stepping starts.
	m_bSplitEligible = pDefinition->romset == Romset::MK1;
	if (m_bSplitEligible)
		InitSplitState(false);

	strncpy(m_ModelName, FriendlyModelName(pDefinition->romset), sizeof(m_ModelName) - 1);
	const char* pVersion = strstr(pDefinition->name, "-v");
	strncpy(m_ROMVersion, pVersion ? pVersion + 2 : pDefinition->name, sizeof(m_ROMVersion) - 1);

	LOGNOTE("Nuked-SC55 initialized: romset %s (%s v%s), native rate %u Hz, oversampling %s",
	        pDefinition->name, m_ModelName, m_ROMVersion, m_nNativeRate, m_bOversampling ? "on" : "off");

	CDiagLog::Get().LogLine("init synth=nuked romset=%s rate=%u oversampling=%s resamp=%s h8_engine=%s sync=%s heap_free_low_kb=%u prime_insn=%llu prime_emu_us=%llu prime_pcm_us=%llu",
	                        pDefinition->name, m_nNativeRate, m_bOversampling ? "on" : "off", m_bFirActive ? "kaiser" : (m_bCubicActive ? "cubic" : "linear"), H8EngineName(TH8Engine::Decoder2),
	                        m_bSplitActive ? "romacclz" : "single",
	                        static_cast<unsigned>(CMemorySystem::Get()->GetHeapFreeSpace(HEAP_LOW) / 1024),
	                        static_cast<unsigned long long>(NukedProfile().nInstructions),
	                        static_cast<unsigned long long>(NukedProfile().nTotalUs),
	                        static_cast<unsigned long long>(NukedProfile().nPCMUs));

	LogSplitState("init");

	// Baseline for the periodic flush: don't charge priming to the
	// first render interval.
	m_nLastProfileTotalUs = NukedProfile().nTotalUs;
	m_nLastProfilePCMUs   = NukedProfile().nPCMUs;
	m_nLastProfileInsn    = NukedProfile().nInstructions;

	return true;
}

void CNukedSC55Synth::HandleMIDIShortMessage(u32 nMessage)
{
	const u8 nStatus = nMessage & 0xFF;
	const u8 nData1  = (nMessage >> 8) & 0xFF;
	const u8 nData2  = (nMessage >> 16) & 0xFF;

	if (!m_pEmulator)
		return;

	// Handle system reset (0xFF) like the other synths: a full reset of
	// the emulated module, followed by a GS reset once it has booted
	if (nStatus == 0xFF)
	{
		m_Lock.Acquire();
		// Split path: park the PCM task first (it may be mid-render
		// on core 3, outside m_Lock), then reset, then reactivate
		// from the fresh pcm_t with a cleared sample ring. The MCU
		// thread cannot be stepping here: RenderFrames needs the
		// same m_Lock this handler holds. A park timeout leaves the
		// split wedged: the reset is skipped entirely (resetting
		// under a possibly-live core 3 would corrupt the backend)
		// and the synth keeps running on its old state.
		const bool bWasSplit = m_bSplitActive;
		bool bParked = true;
		if (bWasSplit)
			bParked = StopSplitPCM();
		if (bParked)
		{
			if (bWasSplit)
				m_pEmulator->ResetForRestart();
			else
				m_pEmulator->Reset();
			m_pEmulator->PostSystemReset(EMU_SystemReset::GS_RESET);
			if (bWasSplit)
				InitSplitState(true);
		}
		m_nHeldNotes = 0;
		m_bSustain = false;
		m_bSustainedNotes = false;
		m_Lock.Release();
		// Split-state reset log: after the lock
		// is released, so the diag write never nests under
		// m_Lock.
		LogSplitState("reset");
		return;
	}

	// Sounding-note tracking for power management (core 0 only):
	// a note held, or held by the sustain pedal, keeps IsActive()
	// true through quiet passages, so the clock cannot drop to
	// power-save mid-performance. Note-off for a note that was
	// sustained releases the sustain hold only when the pedal is
	// already up; pedal-up releases all sustained notes at once.
	if (nStatus < 0xF0)
	{
		const u8 nType = nStatus & 0xF0;
		if (nType == 0x90 && nData2 > 0)
			++m_nHeldNotes;
		else if (nType == 0x80 || (nType == 0x90 && nData2 == 0))
		{
			if (m_bSustain && m_nHeldNotes > 0)
			{
				--m_nHeldNotes;
				m_bSustainedNotes = true;
			}
			else if (m_nHeldNotes > 0)
				--m_nHeldNotes;
		}
		else if (nType == 0xB0 && nData1 == 64)
		{
			m_bSustain = nData2 >= 64;
			if (!m_bSustain)
				m_bSustainedNotes = false;
		}
	}

	// Post exactly the bytes the message carries; the emulated UART
	// parses the stream like the real hardware, so an extra data byte
	// would corrupt the following message.
	u8 Bytes[3] = { nStatus, nData1, nData2 };
	size_t nBytes;
	if (nStatus < 0xF0)
		nBytes = ((nStatus & 0xE0) == 0xC0) ? 2 : 3;
	else if (nStatus == 0xF1 || nStatus == 0xF3)
		nBytes = 2;
	else if (nStatus == 0xF2)
		nBytes = 3;
	else
		nBytes = 1;

	m_Lock.Acquire();
	m_pEmulator->PostMIDI(std::span<const uint8_t>(Bytes, nBytes));
	m_Lock.Release();

	// Update MIDI monitor
	CSynthBase::HandleMIDIShortMessage(nMessage);
}

void CNukedSC55Synth::HandleMIDISysExMessage(const u8* pData, size_t nSize)
{
	if (!m_pEmulator || nSize == 0)
		return;

	// The emulator expects the complete message including the leading
	// 0xF0 and trailing 0xF7; it parses the stream itself.
	m_Lock.Acquire();
	m_pEmulator->PostMIDI(std::span<const uint8_t>(pData, nSize));
	m_Lock.Release();
}

bool CNukedSC55Synth::IsActive()
{
	if (!m_pEmulator || m_nNativeRate == 0)
		return false;

	// Audible output within the last 0.5 s, or a note held / held
	// by the sustain pedal (a quiet passage or a held note must not
	// let the clock drop to power-save mid-performance; see the
	// tracking in HandleMIDIShortMessage).
	return (m_nWrittenFrames - m_nLastActiveFrame) < (m_nNativeRate / 2)
	    || m_nHeldNotes > 0 || m_bSustainedNotes;
}

void CNukedSC55Synth::AllSoundOff()
{
	if (m_pEmulator)
	{
		// CC 120 (All Sound Off) and CC 123 (All Notes Off) on every
		// channel; the firmware handles them like real hardware.
		m_Lock.Acquire();
		for (u8 nChannel = 0; nChannel < 16; ++nChannel)
		{
			const u8 SoundOff[3] = { static_cast<u8>(0xB0 | nChannel), 120, 0 };
			const u8 NotesOff[3] = { static_cast<u8>(0xB0 | nChannel), 123, 0 };
			m_pEmulator->PostMIDI(std::span<const uint8_t>(SoundOff, sizeof(SoundOff)));
			m_pEmulator->PostMIDI(std::span<const uint8_t>(NotesOff, sizeof(NotesOff)));
		}
		m_Lock.Release();
	}

	// Sounding-note tracking (see IsActive): an All Sound Off
	// clears every held/sustained note.
	m_nHeldNotes = 0;
	m_bSustain = false;
	m_bSustainedNotes = false;

	// Reset MIDI monitor
	CSynthBase::AllSoundOff();
}

void CNukedSC55Synth::SetMasterVolume(u8 nVolume)
{
	// The emulated firmware owns its internal levels; mt32-pi's master
	// volume is applied as an output scale in Render(), like the gain.
	m_nVolume = nVolume;
}

void CNukedSC55Synth::ResetFirState()
{
	memset(m_FirHist, 0, sizeof(m_FirHist));
	m_FirFed = 0;
	m_FirOutIndex = 0;
	m_FirPhase = 0;
}

void CNukedSC55Synth::ApplyResamplerSelection()
{
	// Effective resampler: kaiser runs the FIR only on 64k -> 48k
	// (MK1 oversampling on, 48 kHz output); otherwise it falls back
	// to cubic (general, better than lerp). Cubic selection runs
	// cubic at any rate; linear runs the lerp. All resampler state
	// is reset so the newly active resampler primes fresh; the ring
	// is NOT cleared, so audio continues from the queued native
	// samples and a mid-song switch is nearly seamless.
	m_bFirActive = (m_Resampler == TNukedResampler::Kaiser &&
	                m_nNativeRate == 64000 && m_nSampleRate == 48000);
	m_bCubicActive = !m_bFirActive && (m_Resampler != TNukedResampler::Linear);
	ResetFirState();
	m_bCubicValid = false;
	m_bWindowValid = false;
	m_dReadFrac = 0.0;
}

const char* CNukedSC55Synth::CycleResampler()
{
	// Dev A/B toggle (HW2): advance the selection, re-derive the
	// effective resampler, and reset state -- all under m_Lock, so
	// no RenderFrames call can observe a half-switched resampler.
	m_Lock.Acquire();
	switch (m_Resampler)
	{
	case TNukedResampler::Kaiser:
		m_Resampler = TNukedResampler::Cubic;
		break;
	case TNukedResampler::Cubic:
		m_Resampler = TNukedResampler::Linear;
		break;
	default:
		m_Resampler = TNukedResampler::Kaiser;
		break;
	}
	ApplyResamplerSelection();
	const char* pName = m_bFirActive ? "kaiser" : (m_bCubicActive ? "cubic" : "linear");
	m_Lock.Release();
	return pName;
}

size_t CNukedSC55Synth::RenderFrames(float* pOutBuffer, size_t nFrames)
{
	if (!m_pEmulator)
	{
		memset(pOutBuffer, 0, nFrames * 2 * sizeof(float));
		return nFrames;
	}

	const float nScale = m_nGain * (static_cast<float>(m_nVolume) / 100.0f);

	CDiagLog& DiagLog        = CDiagLog::Get();
	const bool bDiag         = DiagLog.IsEnabled();
	const u64 nStartUs       = bDiag ? CTimer::Get()->GetClockTicks() : 0;
	u64 nUnderrunFrames      = 0;

	m_Lock.Acquire();

	// Frames the streaming resampler below will need for this call:
	// the position advance, plus the interpolation endpoint, the
	// entry slide, and rounding. A couple of spare frames just stay
	// queued for the next call.
	const size_t nNeed = static_cast<size_t>(m_dReadFrac + static_cast<double>(nFrames) * m_dResampleRatio) + 3;
	u64 nSteps           = 0;
	const u64 nStepBudget = static_cast<u64>(nFrames * m_dResampleRatio + 2.0) * StepsPerFrameBudget;

	if (m_bSplitActive && m_pSplitState)
	{
		// Split path (core 2, trim): step only the MCU + peripherals;
		// the PCM ticks run on core 3 and produce frames into the
		// ring. Core 2 NEVER waits on core 3 -- no tick rule, no
		// lookahead bound (those were the old RPC design's
		// back-pressure, and the standalone proof showed they are
		// not needed: the ring fill and the step budget bound the
		// loop, and budget exhaustion with a short ring IS the
		// starvation the diag measures). The exactness trade-off
		// is stated plainly: IRQ timing becomes approximate,
		// bounded by core-3 lag (typically tiny at 1 GHz), and
		// sub-millisecond IRQ jitter is inaudible; the standalone
		// hardware runs sound correct on this loop.
		mcu_t& MCU = m_pEmulator->GetMCU();
		ThreadSplitStateCircle& S = *m_pSplitState;
		const u64 nMCUStartUs = bDiag ? CTimer::Get()->GetClockTicks() : 0;
		unsigned nRingCheckdown = 1;	// first check immediate
		unsigned nPublishCountdown = 0;	// publish every 8 steps
		while (m_nSampleRingFill < nNeed && nSteps < nStepBudget)
		{
			// Ring check every 16 steps (countdown 1 = immediate
			// first check): break once the ring holds enough
			// frames for this call. Overstepping by at most 16
			// steps (~1 frame at ~62.5 steps/frame) is negligible
			// against the 16384-frame ring.
			if (--nRingCheckdown == 0)
			{
				nRingCheckdown = 16;
				if (m_nSampleRingFill >= nNeed)
					break;
			}
			// Fold a pending voice-end IRQ0 request into
			// interrupt_pending (core 3 sets the flag; this is
			// one ACQUIRE byte load when no IRQ is pending).
			SplitFoldIrq0(S, MCU);
			m_pEmulator->StepMCU();
			m_pEmulator->StepPeripherals();
			++nSteps;
			// Publish every 8 steps (RELEASE store): core 3 only
			// renders ticks ending at most published-800, and a
			// tick is at most 800 cycles, so every register write
			// the tick reads was already published (see the
			// header's safety proof). A final flush at loop exit
			// covers a mid-interval break.
			if (++nPublishCountdown >= 8)
			{
				nPublishCountdown = 0;
				__atomic_store_n(&S.cycles_pub, MCU.cycles, __ATOMIC_RELEASE);
				S.publishes = S.publishes + 1;
			}
		}
		if (nPublishCountdown != 0)
		{
			__atomic_store_n(&S.cycles_pub, MCU.cycles, __ATOMIC_RELEASE);
			S.publishes = S.publishes + 1;
		}
		if (bDiag)
			m_nMCUUs += CTimer::Get()->GetClockTicks() - nMCUStartUs;
	}
	else
	{
		// Single-threaded path (split off, or a non-MK1 romset):
		// exactly the old stepping, now feeding the same ring and
		// streaming resampler as the split path.
		while (m_nSampleRingFill < nNeed && nSteps < nStepBudget)
		{
			m_pEmulator->Step();
			++nSteps;
		}
	}

	// Streaming resampler: the window (m_SampleA, m_SampleB) brackets
	// the next output position (m_dReadFrac relative to A). The lerp
	// and the scale are the old resampler's, on the same normalized
	// float values, so a full ring produces identical output. The
	// only behavioral difference is the underrun path: instead of
	// resyncing to the newest frame (skipping stale ring contents),
	// the drain model plays the queued frames in order and emits
	// silence once the ring empties -- it cannot fall behind, so
	// there is nothing stale to skip.
	auto DequeueOne = [&](TSplitSample& Out) -> bool
	{
		if (m_SampleRing.Dequeue(Out))
		{
			__atomic_sub_fetch(&m_nSampleRingFill, static_cast<size_t>(1), __ATOMIC_RELAXED);
			return true;
		}
		return false;
	};

	size_t nDone = 0;
	if (m_bFirActive)
	{
		// Polyphase FIR, 64k -> 48k (L=3 up, M=4 down), Kaiser 125 dB
		// target (measured worst stopband -114.7 dB, passband flat to
		// 20 kHz, transition 20-28 kHz; 66 taps/output/phase, see
		// fir64k48k.h). Output n uses phase p = n % 3 and newest
		// input m = (4n - p) / 3; history is circular (128 slots).
		// Feed native samples from the ring until m_FirFed > m,
		// then dot-product the phase's 66 taps against history.
		// nNeed above already covers FIR consumption (4/3 per output
		// plus margin); the 66-sample history warms up from zeros
		// on the first call (inaudible: the emulator starts silent).
		for (; nDone < nFrames; ++nDone)
		{
			const unsigned nPhase = m_FirPhase;
			const size_t nM = (4 * m_FirOutIndex - nPhase) / 3;
			bool bStarved = false;
			while (m_FirFed <= nM)
			{
				TSplitSample Sample;
				if (!DequeueOne(Sample))
				{
					bStarved = true;
					break;
				}
				const unsigned nSlot = static_cast<unsigned>(m_FirFed & 127u);
				m_FirHist[nSlot * 2] = static_cast<float>(Sample.nLeft);
				m_FirHist[nSlot * 2 + 1] = static_cast<float>(Sample.nRight);
				++m_FirFed;
			}
			if (bStarved)
				break;
			const unsigned nNewest = static_cast<unsigned>(nM & 127u);
			float nAccL = 0.0f;
			float nAccR = 0.0f;
			for (unsigned t = 0; t < 66; ++t)
			{
				const unsigned nSlot = (nNewest - t) & 127u;
				const float nCoeff = kFir64k48k[nPhase][t];
				nAccL += nCoeff * m_FirHist[nSlot * 2];
				nAccR += nCoeff * m_FirHist[nSlot * 2 + 1];
			}
			pOutBuffer[nDone * 2] = nAccL * nScale;
			pOutBuffer[nDone * 2 + 1] = nAccR * nScale;
			++m_FirOutIndex;
			m_FirPhase = (m_FirPhase + 1) % 3;
		}
	}
	else if (m_bCubicActive)
	{
		// Cubic Catmull-Rom (4 taps, general rate): window
		// (prev, A, B, next) brackets the output position
		// (m_dReadFrac relative to A); slides like the lerp
		// window but 4-wide. Prime duplicates A into prev
		// (frac starts at 0, so the first output is exactly A).
		for (; nDone < nFrames; ++nDone)
		{
			if (!m_bCubicValid)
			{
				TSplitSample A, B;
				if (!DequeueOne(A) || !DequeueOne(B))
					break;
				m_CubicPrev = A;
				m_CubicA = A;
				m_CubicB = B;
				if (!DequeueOne(m_CubicNext))
					m_CubicNext = B;
				m_bCubicValid = true;
			}
			bool bStarved = false;
			while (m_dReadFrac >= 1.0)
			{
				m_CubicPrev = m_CubicA;
				m_CubicA = m_CubicB;
				m_CubicB = m_CubicNext;
				if (!DequeueOne(m_CubicNext))
				{
					bStarved = true;
					break;
				}
				m_dReadFrac -= 1.0;
			}
			if (bStarved)
				break;

			const float t = static_cast<float>(m_dReadFrac);
			const float t2 = t * t;
			const float t3 = t2 * t;
			const float w0 = -0.5f * t + t2 - 0.5f * t3;
			const float w1 = 1.0f - 2.5f * t2 + 1.5f * t3;
			const float w2 = 0.5f * t + 2.0f * t2 - 1.5f * t3;
			const float w3 = -0.5f * t2 + 0.5f * t3;
			pOutBuffer[nDone * 2] = (w0 * m_CubicPrev.nLeft + w1 * m_CubicA.nLeft + w2 * m_CubicB.nLeft + w3 * m_CubicNext.nLeft) * nScale;
			pOutBuffer[nDone * 2 + 1] = (w0 * m_CubicPrev.nRight + w1 * m_CubicA.nRight + w2 * m_CubicB.nRight + w3 * m_CubicNext.nRight) * nScale;

			m_dReadFrac += m_dResampleRatio;
		}
	}
	else
	{
		for (; nDone < nFrames; ++nDone)
		{
			if (!m_bWindowValid)
			{
				TSplitSample A, B;
				if (!DequeueOne(A) || !DequeueOne(B))
					break;
				m_SampleA = A;
				m_SampleB = B;
				m_bWindowValid = true;
			}
			bool bStarved = false;
			while (m_dReadFrac >= 1.0)
			{
				m_SampleA = m_SampleB;
				if (!DequeueOne(m_SampleB))
				{
					bStarved = true;
					break;
				}
				m_dReadFrac -= 1.0;
			}
			if (bStarved)
				break;

			const float nFrac = static_cast<float>(m_dReadFrac);
			pOutBuffer[nDone * 2]     = (m_SampleA.nLeft + (m_SampleB.nLeft - m_SampleA.nLeft) * nFrac) * nScale;
			pOutBuffer[nDone * 2 + 1] = (m_SampleA.nRight + (m_SampleB.nRight - m_SampleA.nRight) * nFrac) * nScale;

			m_dReadFrac += m_dResampleRatio;
		}
	}
	if (nDone < nFrames)
	{
		// Underrun (emulator too slow, or still booting): silence for
		// the rest of the call; the next call re-primes from whatever
		// the ring holds then.
		for (size_t j = nDone; j < nFrames; ++j)
		{
			pOutBuffer[j * 2]     = 0.0f;
			pOutBuffer[j * 2 + 1] = 0.0f;
		}
		nUnderrunFrames += nFrames - nDone;
		m_dReadFrac = 0.0;
		m_bWindowValid = false;
		if (m_bFirActive)
			ResetFirState();
		else if (m_bCubicActive)
			m_bCubicValid = false;
	}

	if (bDiag)
	{
		const u64 nElapsedUs = CTimer::Get()->GetClockTicks() - nStartUs;
		const u64 nBudgetUs  = static_cast<u64>(nFrames) * 1000000ULL / m_nSampleRate;

		m_StatsLock.Acquire();
		if (m_Stats.nCalls == 0)
			m_Stats.nWindowStartUs = nStartUs;
		++m_Stats.nCalls;
		m_Stats.nFrames += nFrames;
		m_Stats.nTotalUs += nElapsedUs;
		if (nElapsedUs > m_Stats.nMaxUs)
			m_Stats.nMaxUs = nElapsedUs;
		if (nElapsedUs > nBudgetUs)
			++m_Stats.nMisses;
		m_Stats.nUnderrunFrames += nUnderrunFrames;
		m_StatsLock.Release();
	}

	m_Lock.Release();

	return nFrames;
}

size_t CNukedSC55Synth::Render(float* pOutBuffer, size_t nFrames)
{
	return RenderFrames(pOutBuffer, nFrames);
}

size_t CNukedSC55Synth::Render(s16* pOutBuffer, size_t nFrames)
{
	// Render via the float path in chunks that fit a modest stack buffer
	constexpr size_t ChunkFrames = 512;
	float FloatBuffer[ChunkFrames * 2];

	size_t nDone = 0;
	while (nDone < nFrames)
	{
		const size_t nChunk = Utility::Min(nFrames - nDone, ChunkFrames);
		RenderFrames(FloatBuffer, nChunk);

		for (size_t i = 0; i < nChunk * 2; ++i)
			pOutBuffer[nDone * 2 + i] = static_cast<s16>(Utility::Clamp(FloatBuffer[i], -1.0f, 1.0f) * 32767.0f);

		nDone += nChunk;
	}

	return nFrames;
}

void CNukedSC55Synth::ReportStatus() const
{
	if (m_pUI)
		m_pUI->ShowSystemMessage(m_ModelName[0] ? m_ModelName : "Sound Canvas");
}

void CNukedSC55Synth::UpdateLCD(CLCD& LCD, unsigned int nTicks)
{
	// Bars shrink by one row, mirroring the MT32 synth's
	// bars-plus-status layout (the bottom row was the debug
	// heartbeat row, removed: its 16 ms Flip flashed the
	// display). Graphical: rows are 8px, so bars lose 8px;
	// character: rows are cells, so bars lose one row.
	u8 nBarHeight;
	if (LCD.GetType() == CLCD::TType::Character)
		nBarHeight = LCD.Height() - 1;
	else
		nBarHeight = LCD.Height() - 8;
	float ChannelLevels[16], PeakLevels[16];
	m_MIDIMonitor.GetChannelLevels(nTicks, ChannelLevels, PeakLevels, 1 << 9);
	CUserInterface::DrawChannelLevels(LCD, nBarHeight, ChannelLevels, PeakLevels, 16, true);

	// (Debug heartbeat removed: its 16 ms status-row print + Flip
	// flashed the whole display. Core-2/core-3 aliveness now lives
	// in the diag window line's calls/written fields instead.)

	// Flush accumulated render timing statistics to the diagnostic log
	CDiagLog& DiagLog = CDiagLog::Get();
	if (DiagLog.IsEnabled())
	{
		const u64 nNowUs = CTimer::Get()->GetClockTicks();

		m_StatsLock.Acquire();
		const TRenderStats Stats = m_Stats;
		const bool bDue          = Stats.nCalls > 0 && Stats.nWindowStartUs != 0 &&
		                  nNowUs - Stats.nWindowStartUs >= static_cast<u64>(DiagLog.IntervalSeconds()) * 1000000ULL;
		if (bDue)
			m_Stats = TRenderStats{};
		m_StatsLock.Release();

		if (bDue)
		{
			CCPUThrottle* pThrottle = CCPUThrottle::Get();
			// One consolidated window line per interval (standalone-style):
			// engine identity (sync=romacclz/single) + real-time rate + render
			// time + split counters or backend phase split. This replaces
			// the separate render/split/profile lines, halving the f_sync
			// stalls per interval. The resampler lives inside RenderFrames
			// here (unlike the standalone's separate resampler), so its time
			// is inside avg_us/max_us, not a separate field; cycle-based
			// fields are unavailable (CTimer wall clock, no PMU).
			const u64 nWindowUs = nNowUs - Stats.nWindowStartUs;
			const u64 nAvgUs = Stats.nCalls > 0 ? Stats.nTotalUs / Stats.nCalls : 0;
			const u64 nMissPct = Stats.nCalls > 0 ? Stats.nMisses * 100 / Stats.nCalls : 0;
			const unsigned nTempC = pThrottle ? pThrottle->GetTemperature() : 0;
			u64 nPct = 0;
			if (nWindowUs > 0 && m_dResampleRatio > 0.0)
			{
				const u64 nDueFrames = static_cast<u64>(nWindowUs * (static_cast<double>(m_nNativeRate) / m_dResampleRatio) / 1000000.0);
				if (nDueFrames > 0)
					nPct = Stats.nFrames * 100 / nDueFrames;
			}


			if (m_bSplitActive && m_pSplitState)
			{
				// Split diag for the same window: window deltas for
				// the trim counters, peaks since activation as-is.
				// The core-2 MCU time is the stepping time
				// accumulated in RenderFrames; the core-3 PCM time
				// is the PCM task's work time. (The single-threaded
				// profile line is meaningless across cores -- PCM
				// time on core 3 can exceed emulator time on core
				// 2 -- so it is replaced, not supplemented, while
				// the split is active.)
				ThreadSplitStateCircle& S = *m_pSplitState;
				const u64 nFifoFullWaits  = S.fifo_full_waits - m_nLastSplitFifoFullWaits;
				const u64 nFifoDrops      = S.fifo_drops - m_nLastSplitFifoDrops;
				const u64 nPublishes      = S.publishes - m_nLastSplitPublishes;
				const u64 nTicks          = S.ticks - m_nLastSplitTicks;
				const u64 nIrq0Folded     = S.irq0_folded - m_nLastSplitIrq0Folded;
				const u64 nPCMUs          = S.pcm_task_us - m_nLastSplitPCMUs;
				const u64 nMCUUs          = m_nMCUUs;
				const u64 nWritten = m_nWrittenFrames - m_nLastWrittenFrames;
				DiagLog.LogLine("window synth=nuked sync=romacclz pct=%llu avg_us=%llu max_us=%llu miss_pct=%llu underrun_frames=%llu temp_c=%u core2_pct=%llu core3_pct=%llu fifo_full_waits=%llu fifo_hw=%llu fifo_drops=%llu ring_drops=%llu publishes=%llu ticks=%llu irq0_folded=%llu max_lag=%llu phase=%u written=%llu",
				                static_cast<unsigned long long>(nPct),
				                static_cast<unsigned long long>(nAvgUs),
				                static_cast<unsigned long long>(Stats.nMaxUs),
				                static_cast<unsigned long long>(nMissPct),
				                static_cast<unsigned long long>(Stats.nUnderrunFrames),
				                nTempC,
				                static_cast<unsigned long long>(nWindowUs > 0 ? nMCUUs * 100 / nWindowUs : 0),
				                static_cast<unsigned long long>(nWindowUs > 0 ? nPCMUs * 100 / nWindowUs : 0),
				                static_cast<unsigned long long>(nFifoFullWaits),
				                static_cast<unsigned long long>(S.fifo_hw),
				                static_cast<unsigned long long>(nFifoDrops),
				                static_cast<unsigned long long>(m_nSampleRingDrops),
				                static_cast<unsigned long long>(nPublishes),
				                static_cast<unsigned long long>(nTicks),
				                static_cast<unsigned long long>(nIrq0Folded),
				                static_cast<unsigned long long>(S.max_pcm_lag),
				                S.core3_phase,
				                static_cast<unsigned long long>(nWritten));
				m_nLastSplitFifoFullWaits = S.fifo_full_waits;
				m_nLastSplitFifoDrops = S.fifo_drops;
				m_nLastSplitPublishes = S.publishes;
				m_nLastSplitTicks = S.ticks;
				m_nLastSplitIrq0Folded = S.irq0_folded;
				m_nLastSplitPCMUs = S.pcm_task_us;
				m_nLastWrittenFrames = m_nWrittenFrames;
				m_nMCUUs = 0;
			}
			else
			{
				// Backend phase split for the same window: emulator wall
				// share of one core, MCU+peripherals vs PCM share of the
				// emulator's time, and the H8 instruction rate.
				const NukedProfileData& Profile = NukedProfile();
				const u64 nTotalDelta          = Profile.nTotalUs - m_nLastProfileTotalUs;
				const u64 nPCMDelta            = Profile.nPCMUs - m_nLastProfilePCMUs;
				const u64 nInsnDelta           = Profile.nInstructions - m_nLastProfileInsn;
				m_nLastProfileTotalUs          = Profile.nTotalUs;
				m_nLastProfilePCMUs            = Profile.nPCMUs;
				m_nLastProfileInsn             = Profile.nInstructions;

				DiagLog.LogLine("window synth=nuked sync=single pct=%llu avg_us=%llu max_us=%llu miss_pct=%llu underrun_frames=%llu temp_c=%u emu_pct=%llu mcu_pct=%llu pcm_pct=%llu insn_per_sec=%llu",
				                static_cast<unsigned long long>(nPct),
				                static_cast<unsigned long long>(nAvgUs),
				                static_cast<unsigned long long>(Stats.nMaxUs),
				                static_cast<unsigned long long>(nMissPct),
				                static_cast<unsigned long long>(Stats.nUnderrunFrames),
				                nTempC,
				                static_cast<unsigned long long>(nWindowUs > 0 ? nTotalDelta * 100 / nWindowUs : 0),
				                static_cast<unsigned long long>(nTotalDelta > 0 ? (nTotalDelta - nPCMDelta) * 100 / nTotalDelta : 0),
				                static_cast<unsigned long long>(nTotalDelta > 0 ? nPCMDelta * 100 / nTotalDelta : 0),
				                static_cast<unsigned long long>(nWindowUs > 0 ? nInsnDelta * 1000000ULL / nWindowUs : 0));
			}
		}
	}
}
