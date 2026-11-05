//
// mu50synth.cpp
//
// mt32-pi - A baremetal MIDI synthesizer for Raspberry Pi
// Copyright (C) 2020-2023 Dale Whinham <daleyo@gmail.com>
//
// This file is part of mt32-pi.
//
// mt32-pi is free software: you can redistribute it and/or modify it under
// the terms of the GNU General Public License as published by the Free Software
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

#include <cstring>
#include <strings.h>
#include <vector>

#include <circle/logger.h>
#include <circle/memory.h>
#include <circle/timer.h>
#include <circle/sched/scheduler.h>
#include <circle/cputhrottle.h>
#include <fatfs/ff.h>

#include "config.h"
#include "diaglog.h"
#include "lcd/ui.h"
#include "synth/mu50synth.h"
#include "synth/mu50h8.h"
#include "utility.h"

// libmu50 headers are only included here (this file is compiled as C++17;
// see Kernel.mk). Unlike libEmuSC, libmu50 needs no Circle compat shim:
// it uses only standard headers (algorithm, array, cstdint, limits,
// memory, string, tuple, utility, vector), no threads, no mutex, and it
// reports errors via return values rather than exceptions.
#include <libmu50/synth.hpp>

LOGMODULE("mu50synth");

// MU50 ROMs live in their own directory alongside the MT-32 "roms" and
// "soundfonts" directories, mirroring the EmuSC layout (roms/sc55).
const char* const MU50Disks[] = { "SD", "USB" };
const char MU50ROMDirectory[] = "roms/yamaha";

// ROM files are classified by size, mirroring the verified mu50 set:
// - Wave ROM is 2 x 2 MiB (xq057c0.ic18 + xq058c0.ic19, concatenated in
//   filename order) or a single 4 MiB file; libmu50 requires exactly 4 MiB
// - Firmware ROMs are 512 KiB each (xr174c0.ic7, yamaha_mu50.bin,
//   xq332e0.bin). Initialize() loads the default BIOS (xr174c0.ic7,
//   the M2-verified one) into the H8 ROM window, falling back to the
//   first filename-sorted candidate.
constexpr FSIZE_t WaveROMPartSize = 2 * 1024 * 1024;
constexpr FSIZE_t WaveROMTotalSize = 4 * 1024 * 1024;
constexpr FSIZE_t FirmwareROMSize = 512 * 1024;
constexpr size_t MaxROMFiles      = 16;
constexpr size_t MaxFirmwareFiles = 8;

namespace
{
bool ROMPathComparator(const CString& LHS, const CString& RHS)
{
	return strcasecmp(static_cast<const char*>(LHS), static_cast<const char*>(RHS)) < 0;
}

// H8 CPU clock (10 MHz) per libmu50 native audio frame (44.1 kHz).
constexpr double H8CyclesPerNativeFrame = 10000000.0 / 44100.0;

// H8 hook targets: the hook address is the SWP00 window offset
// (0x0000-0x07ff), which is exactly libmu50's register address.
// The write hook's cycle is the 10 MHz H8 cycle domain timestamp
// for the W-event FIFO (M1).
void MU50H8SwpWrite(void* pCtx, mu50h8::u64 nCycle, mu50h8::u16 nAddr, mu50h8::u8 nData)
{
	static_cast<CMU50Synth*>(pCtx)->OnH8SwpWrite(nCycle, nAddr, nData);
}

mu50h8::u8 MU50H8SwpRead(void* pCtx, mu50h8::u16 nAddr)
{
	return static_cast<CMU50Synth*>(pCtx)->OnH8SwpRead(nAddr);
}

// SCI1 RDR read by the firmware (see OnH8Sci1RdrRead).
void MU50H8Sci1RdrRead(void* pCtx)
{
	static_cast<CMU50Synth*>(pCtx)->OnH8Sci1RdrRead();
}

}

CMU50Synth::CMU50Synth(unsigned nSampleRate, float nGain)
	: CSynthBase(nSampleRate),
	  m_pSynth(nullptr),
	  m_pH8(nullptr),
	  m_FirmwareROM(),
	  m_dH8CycleCarry(0.0),
	  m_bH8Halted(false),
	  m_nWrittenNativeFrames(0),
	  m_nLastActiveNativeFrame(0),
	  m_nHeldNotes(0),
	  m_bSustain(false),
	  m_bSustainedNotes(false),
	  m_nDiagWindowStartUs(0),
	  m_nDiagCalls(0),
	  m_nDiagTotalUs(0),
	  m_nDiagMaxUs(0),
	  m_nDiagH8Us(0),
	  m_nDiagSwpUs(0),
	  m_nDiagH8Insns(0),
	  m_nDiagOutputFrames(0),
	  m_nDiagMidiQueued(0),
	  m_nDiagSciRead(0),
	  m_nDiagSwpWrites(0),
	  m_nDiagNativePeak(0),
	  m_nDiagWaitUs(0),
	  m_nDiagLastUnderrunFrames(0),
	  m_nDiagLastSwpRenderUs(0),
	  m_nGain(nGain),
	  m_nVolume(100),
	  m_dResampleRatio(44100.0 / static_cast<double>(nSampleRate)),
	  m_dResampleCarry(0.0),
	  m_bResamplePrimed(false),
	  m_PrevSample{},
	  m_LeftoverSample{},
	  m_NativeScratch{},
	  m_FloatScratch{},
	  m_WEventFifo{},
	  m_nWEventFifoCap(kWEventFifoSize - 1),
	  m_SwpShadow{},
	  m_NativeRing{},
	  m_PublishedBlocks{},
	  m_MidiStage{},
	  m_nRingDrops(0),
	  m_nWqFullWaits(0),
	  m_nBlockWaits(0),
	  m_nStageDrops(0),
	  m_nRenderedEnqueued(0),
	  m_nConsumed(0),
	  m_nPublishedCycles(0),
	  m_nUnderrunFrames(0),
	  m_nSwpRenderUs(0),
	  m_nRingHighWater(0),
	  m_nAbortSeq(0),
	  m_nNextPublishSeq(0),
	  m_nLastPublishedSeq(0),
	  m_bLastPublishOk(false),
	  m_bSplitMode(false),
	  m_bSwpActive(true),
	  m_bSwpParkAck(false),
	  m_bAudioBusy(false),
	  m_SwpScratch{},
	  m_Pending{},
	  m_nPendingCount(0)
{
}

CMU50Synth::~CMU50Synth()
{
	// The H8 first: its hooks point back at this synth.
	if (m_pH8)
		delete m_pH8;
	if (m_pSynth)
		delete m_pSynth;
}

bool CMU50Synth::ScanROMs(TROMPaths& OutPaths)
{
	CString WaveCandidates[MaxROMFiles];
	CString BigWaveCandidates[MaxROMFiles];
	CString FirmwareCandidates[MaxFirmwareFiles];
	size_t nWaveCount     = 0;
	size_t nBigWaveCount  = 0;
	size_t nFirmwareCount = 0;

	// Loop over each disk; the first disk with a complete set wins
	for (auto pDisk : MU50Disks)
	{
		nWaveCount = nBigWaveCount = nFirmwareCount = 0;

		CString DirectoryPath;
		DirectoryPath.Format("%s:/%s", pDisk, MU50ROMDirectory);

		DIR Dir;
		FILINFO FileInfo;
		FRESULT Result = f_findfirst(&Dir, &FileInfo, DirectoryPath, "*");

		while (Result == FR_OK && *FileInfo.fname)
		{
			// Ensure not directory, hidden, or system file
			if (!(FileInfo.fattrib & (AM_DIR | AM_HID | AM_SYS)))
			{
				CString ROMPath;
				ROMPath.Format("%s/%s", static_cast<const char*>(DirectoryPath), FileInfo.fname);

				const FSIZE_t nSize = FileInfo.fsize;
				if (nSize == WaveROMPartSize && nWaveCount < MaxROMFiles)
					WaveCandidates[nWaveCount++] = ROMPath;
				else if (nSize == WaveROMTotalSize && nBigWaveCount < MaxROMFiles)
					BigWaveCandidates[nBigWaveCount++] = ROMPath;
				else if (nSize == FirmwareROMSize && nFirmwareCount < MaxFirmwareFiles)
					FirmwareCandidates[nFirmwareCount++] = ROMPath;
			}

			Result = f_findnext(&Dir, &FileInfo);
		}

		f_closedir(&Dir);

		if (nWaveCount == 2 || nBigWaveCount > 0)
			break;
	}

	// Firmware files are enumerated for BIOS selection in Initialize()
	for (size_t i = 0; i < nFirmwareCount; ++i)
	{
		LOGNOTE("MU50 firmware candidate: %s",
		        static_cast<const char*>(FirmwareCandidates[i]));
		OutPaths.FirmwarePaths[i] = FirmwareCandidates[i];
	}
	OutPaths.nFirmwareCount = nFirmwareCount;

	if (nWaveCount == 2)
	{
		// Deterministic order: the 2 MiB parts must be concatenated in
		// filename order (xq057c0.ic18, then xq058c0.ic19)
		Utility::QSort(WaveCandidates, ROMPathComparator, 0, nWaveCount - 1);
		OutPaths.WaveROMPaths[0] = WaveCandidates[0];
		OutPaths.WaveROMPaths[1] = WaveCandidates[1];
		OutPaths.nWaveROMCount   = 2;
	}
	else if (nBigWaveCount > 0)
	{
		if (nBigWaveCount > 1)
			Utility::QSort(BigWaveCandidates, ROMPathComparator, 0, nBigWaveCount - 1);
		OutPaths.WaveROMPaths[0] = BigWaveCandidates[0];
		OutPaths.nWaveROMCount   = 1;
	}
	else
	{
		LOGWARN("Incomplete MU50 wave ROM set in %s (2 MiB parts: %u, 4 MiB files: %u)",
		        MU50ROMDirectory, static_cast<unsigned>(nWaveCount),
		        static_cast<unsigned>(nBigWaveCount));
		return false;
	}

	OutPaths.bValid = true;

	LOGNOTE("MU50 wave ROM set: %u file(s), firmware candidates: %u",
	        static_cast<unsigned>(OutPaths.nWaveROMCount),
	        static_cast<unsigned>(nFirmwareCount));

	return true;
}

bool CMU50Synth::ReadFile(const char* pPath, u8* pBuffer, size_t nSize)
{
	FIL File;
	if (f_open(&File, pPath, FA_READ) != FR_OK)
		return false;

	size_t nTotalRead = 0;
	while (nTotalRead < nSize)
	{
		UINT nRead = 0;
		// Read in bounded chunks; f_read's UINT count is 32-bit, but
		// keep each call small for the FatFs buffer
		const UINT nWant = static_cast<UINT>(Utility::Min(nSize - nTotalRead, static_cast<size_t>(65536)));
		if (f_read(&File, pBuffer + nTotalRead, nWant, &nRead) != FR_OK || nRead == 0)
		{
			f_close(&File);
			return false;
		}
		nTotalRead += nRead;
	}

	f_close(&File);
	return nTotalRead == nSize;
}

bool CMU50Synth::Initialize()
{
	TROMPaths ROMPaths;
	if (!ScanROMs(ROMPaths))
		return false;

	// Concatenate the wave ROM parts into the exactly-4 MiB buffer
	// libmu50 requires (it copies the buffer on load, so this vector
	// is freed again below)
	std::vector<u8> WaveROM;
	WaveROM.reserve(WaveROMTotalSize);
	for (size_t i = 0; i < ROMPaths.nWaveROMCount; ++i)
	{
		const size_t nPartSize = (ROMPaths.nWaveROMCount == 1)
			? WaveROMTotalSize
			: WaveROMPartSize;
		const size_t nOldSize = WaveROM.size();
		WaveROM.resize(nOldSize + nPartSize);
		if (!ReadFile(static_cast<const char*>(ROMPaths.WaveROMPaths[i]),
		              WaveROM.data() + nOldSize, nPartSize))
		{
			LOGWARN("MU50 wave ROM read failed: %s",
			        static_cast<const char*>(ROMPaths.WaveROMPaths[i]));
			return false;
		}
	}

	if (WaveROM.size() != WaveROMTotalSize)
	{
		LOGWARN("MU50 wave ROM size mismatch: %u bytes", static_cast<unsigned>(WaveROM.size()));
		return false;
	}

	// The core always renders at its native 44.1 kHz; the engine
	// resamples to the output rate in RenderResampled()
	libmu50::Config LibConfig;
	LibConfig.sample_rate = 44100;
	m_pSynth = new libmu50::Synth(LibConfig);

	const libmu50::Status LoadStatus = m_pSynth->load_wave_rom(WaveROM);
	if (!LoadStatus)
	{
		LOGWARN("libmu50 wave ROM load failed: %s", LoadStatus.message.c_str());
		delete m_pSynth;
		m_pSynth = nullptr;
		return false;
	}

	LOGNOTE("libmu50 %s initialized: wave ROM loaded", libmu50::version());

	// Front end: the real Yamaha firmware on the H8 CPU port. Prefer
	// xr174c0.ic7 (default BIOS 0, the M2-verified one); otherwise the
	// first filename-sorted candidate. The firmware buffer is kept as
	// a member: Mu50H8 holds a pointer to it for its lifetime, so it
	// is filled before H8 creation and never modified after. Firmware
	// must sit on the same disk as the wave set: ScanROMs stops at
	// the first disk with a complete wave set.
	if (ROMPaths.nFirmwareCount == 0)
	{
		LOGWARN("MU50 firmware ROM missing in %s (engine unavailable)", MU50ROMDirectory);
		delete m_pSynth;
		m_pSynth = nullptr;
		return false;
	}
	Utility::QSort(ROMPaths.FirmwarePaths, ROMPathComparator, 0, ROMPaths.nFirmwareCount - 1);
	CString FirmwarePath;
	bool bFirmwareFound = false;
	for (size_t i = 0; i < ROMPaths.nFirmwareCount; ++i)
	{
		const char* pFullPath = static_cast<const char*>(ROMPaths.FirmwarePaths[i]);
		const char* pSlash    = strrchr(pFullPath, '/');
		const char* pBase     = pSlash ? pSlash + 1 : pFullPath;
		if (strcasecmp(pBase, "xr174c0.ic7") == 0)
		{
			FirmwarePath   = ROMPaths.FirmwarePaths[i];
			bFirmwareFound = true;
			break;
		}
	}
	if (!bFirmwareFound)
	{
		LOGWARN("MU50 default BIOS xr174c0.ic7 not found, using fallback: %s",
		        static_cast<const char*>(ROMPaths.FirmwarePaths[0]));
		FirmwarePath = ROMPaths.FirmwarePaths[0];
	}
	m_FirmwareROM.resize(FirmwareROMSize);
	if (!ReadFile(static_cast<const char*>(FirmwarePath),
	              m_FirmwareROM.data(), FirmwareROMSize))
	{
		LOGWARN("MU50 firmware ROM read failed: %s",
		        static_cast<const char*>(FirmwarePath));
		m_FirmwareROM.clear();
		delete m_pSynth;
		m_pSynth = nullptr;
		return false;
	}

	mu50h8::Hooks H8Hooks;
	H8Hooks.ctx       = this;
	H8Hooks.swp_write = MU50H8SwpWrite;
	H8Hooks.swp_read  = MU50H8SwpRead;
	H8Hooks.sci1_rdr_read = MU50H8Sci1RdrRead;
	H8Hooks.midi_tx   = nullptr; // firmware MIDI out is unused by the engine
	H8Hooks.log       = nullptr; // null in the engine (harness prints it)
	m_pH8 = new mu50h8::Mu50H8(m_FirmwareROM.data(), H8Hooks);
	m_pH8->reset();

	LOGNOTE("MU50 H8 front end initialized: firmware %s",
	        static_cast<const char*>(FirmwarePath));

	CDiagLog::Get().LogLine("init synth=mu50 wave=ok firmware=%s h8=ok heap_free_low_kb=%u",
	                        static_cast<const char*>(FirmwarePath),
	                        static_cast<unsigned>(CMemorySystem::Get()->GetHeapFreeSpace(HEAP_LOW) / 1024));

	// Apply any master volume set before initialization completed
	SetMasterVolume(m_nVolume);

	return true;
}

void CMU50Synth::OnH8SwpWrite(u64 nCycle, u16 nAddr, u8 nData)
{
	// Called from AdvanceH8 (audio thread). Push the event into the
	// W-event FIFO in firmware order; the shadow always tracks the
	// last-written value (split-mode read fallback). Firmware writes
	// that the register core rejects are ignored at drain time
	// (should not happen once the wave ROM is loaded).
	// Single-core: a full FIFO drains synchronously (M1 behavior);
	// the drain count is the M2 wq_full_waits counter's M1 form and
	// must stay 0 at E1M1 load (M0 worst block burst 602 < 4095
	// usable). Split: a full FIFO makes the H8 thread WAIT
	// (backpressure, counted once per episode) -- W events are
	// synthesis events, never dropped (unlike Nuked's latch stores).
	if (m_pSynth)
	{
		const TWEvent Event{ nCycle, nAddr, nData };
		if (nAddr < sizeof(m_SwpShadow))
			m_SwpShadow[nAddr] = nData;
		if (m_bSplitMode)
		{
			// Cap-gated backpressure (cap <= 4095 usable; the
			// TryPush full check below is the backstop). Single
			// producer: pending cannot grow between the wait exit
			// and the push, so the push must succeed.
			if (m_WEventFifo.PendingForProducer() >= m_nWEventFifoCap)
			{
				__atomic_add_fetch(&m_nWqFullWaits, 1, __ATOMIC_RELAXED);
				while (m_WEventFifo.PendingForProducer() >= m_nWEventFifoCap)
					mu50spsc::CpuRelax();
			}
			m_WEventFifo.TryPush(Event);
		}
		else
		{
			if (!m_WEventFifo.TryPush(Event))
			{
				DrainWEvents();
				__atomic_add_fetch(&m_nWqFullWaits, 1, __ATOMIC_RELAXED);
				m_WEventFifo.TryPush(Event); // must succeed: drained empty
			}
		}
		++m_nDiagSwpWrites;
	}
}

void CMU50Synth::DrainWEventsUpTo(size_t nSnap)
{
	// Applies pending events in firmware order up to the producer
	// in-index snapshot nSnap. Single-core DrainWEvents() passes the
	// current in-index (drain everything); the SWP thread passes the
	// published block's snapshot (drain exactly that block's writes).
	// Never called concurrently: single-core mode drains on the
	// audio thread, split mode on the SWP thread, modes exclusive.
	while (m_WEventFifo.OutIndex() != nSnap)
	{
		TWEvent Event;
		if (!m_WEventFifo.TryPeek(Event))
			break; // cannot happen: snap is behind in-index
		if (m_pSynth)
			m_pSynth->write_register(Event.nAddr, Event.nData);
		m_WEventFifo.Pop();
	}
}

void CMU50Synth::DrainWEvents()
{
	// Caller must hold m_Lock (single-core, synchronous).
	// Applies every pending event in firmware order. Runs before
	// each block render and before each SWP read, so no read ever
	// observes a stale register.
	DrainWEventsUpTo(m_WEventFifo.InIndex());
}

u8 CMU50Synth::OnH8SwpRead(u16 nAddr)
{
	// Single-core (M1): drain first, then read live register state.
	if (!m_bSplitMode)
	{
		DrainWEvents();
		u8 nValue = 0;
		if (m_pSynth)
			m_pSynth->read_register(nAddr, nValue);
		return nValue;
	}
	// Split: the SWP thread owns live register state, so the audio
	// thread must never read it. Pending-scan newest-wins first
	// (M0: reads are cold, single-address 0x001, always
	// read-your-own-write); the audio-side shadow covers the case
	// where the newest write was already drained (shadow == last
	// written == live for side-effect-free control registers, and
	// the firmware never reads any other address, M0). A failed
	// read returns 0, matching the M2 harness stub behavior.
	TWEvent Event;
	if (m_WEventFifo.FindNewest([nAddr](const TWEvent& E) { return E.nAddr == nAddr; }, Event))
		return Event.nData;
	if (nAddr < sizeof(m_SwpShadow))
		return m_SwpShadow[nAddr];
	return 0;
}

void CMU50Synth::OnH8Sci1RdrRead()
{
	// Called from the H8's sci_read (inside AdvanceH8 during
	// Render, audio thread). Counts SCI1 RDR reads, normally one
	// per MIDI byte the firmware consumes.
	++m_nDiagSciRead;
}

void CMU50Synth::DrainMidiStage()
{
	// Split mode, audio thread only, called at each AdvanceH8.
	// Bytes staged by the MIDI thread land in SCI1 exactly as if
	// the MIDI handler had queued them directly: cycles_ is
	// unchanged since the last advance, so the line-driver edge
	// times (cycles_+320) are identical to single-core timing.
	// Note tracking and the MIDI monitor move to the drain too, so
	// all note state stays on the audio thread.
	TMidiStage Stage;
	while (m_MidiStage.TryPop(Stage))
	{
		if (Stage.nKind == 0)
		{
			if (m_pH8->queue_midi_byte(Stage.nB0))
				++m_nDiagMidiQueued;
		}
		else if (Stage.nKind == 1)
		{
			const u8 nStatus = Stage.nB0;
			const u8 nData1  = Stage.nB1;
			const u8 nData2  = Stage.nB2;
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
			QueueMIDIMessageBytes(nStatus, nData1, nData2);
			CSynthBase::HandleMIDIShortMessage(
				static_cast<u32>(nStatus) | (static_cast<u32>(nData1) << 8) |
				(static_cast<u32>(nData2) << 16));
		}
		else if (Stage.nKind == 2)
		{
			// AllSoundOff: CC 120 + CC 123 on every channel.
			for (u8 nChannel = 0; nChannel < 16; ++nChannel)
			{
				QueueMIDIMessageBytes(static_cast<u8>(0xB0 | nChannel), 120, 0);
				QueueMIDIMessageBytes(static_cast<u8>(0xB0 | nChannel), 123, 0);
			}
			m_nHeldNotes      = 0;
			m_bSustain        = false;
			m_bSustainedNotes = false;
			CSynthBase::AllSoundOff();
		}
	}
}

void CMU50Synth::AdvanceH8(size_t nNativeFrames)
{
	// Split mode: staged MIDI lands before any stepping, even when
	// halted or when this block is empty -- matching single-core,
	// where MIDI handlers queue regardless of halt state.
	if (m_bSplitMode)
		DrainMidiStage();

	if (!m_pH8 || nNativeFrames == 0)
		return;

	if (!m_bH8Halted)
	{
		m_dH8CycleCarry += static_cast<double>(nNativeFrames) * H8CyclesPerNativeFrame;
		const u64 nAdvance = static_cast<u64>(m_dH8CycleCarry);
		m_dH8CycleCarry -= static_cast<double>(nAdvance);
		if (nAdvance > 0)
		{
			const bool bDiag      = CDiagLog::Get().IsEnabled();
			const u64 nStartUs    = bDiag ? CTimer::Get()->GetClockTicks() : 0;
			const u64 nStartInsns = m_pH8->instr_count();
			if (!m_pH8->run_until(m_pH8->cycles() + nAdvance))
			{
				// Sticky: a halted CPU stays halted until a system
				// reset (0xFF) revives it. Should not happen: M2
				// ran 23.7M instructions through boot with no halt.
				m_bH8Halted = true;
				LOGWARN("MU50 H8 halted at pc=0x%08x (illegal opcode); firmware stopped",
				        m_pH8->illegal_pc());
			}
			if (bDiag)
			{
				m_nDiagH8Us += CTimer::Get()->GetClockTicks() - nStartUs;
				m_nDiagH8Insns += m_pH8->instr_count() - nStartInsns;
			}
		}
	}
	// Split mode: publish the block (RELEASE) even when halted --
	// M1 still renders every block after a halt (decay/silence),
	// so the SWP thread must still render it, or the audio thread
	// would wait forever. The snapshot bound makes the SWP drain
	// exact even when the audio core has run ahead: only writes
	// pushed before this publish are applied to this block's
	// render. A full block queue waits (counted); with 16 slots
	// and the SWP core faster than the H8, it never fills in
	// steady state.
	if (m_bSplitMode)
	{
		const u64 nSeq = ++m_nNextPublishSeq;
		const TPublishedBlock Block{ m_WEventFifo.InIndex(), nNativeFrames, nSeq };
		m_bLastPublishOk = true;
		if (!m_PublishedBlocks.TryPush(Block))
		{
			__atomic_add_fetch(&m_nBlockWaits, 1, __ATOMIC_RELAXED);
			while (!m_PublishedBlocks.TryPush(Block))
			{
				// M3: parked mid-publish (sustained parked
				// rendering, e.g. the starve test): skip the
				// publish rather than wedging. The block's W
				// events stay queued (SWP drains them later, so
				// SWP state stays correct); the pull sees
				// bPublished=false and silence-fills without
				// waiting. Never skip for any other reason: a
				// skipped publish while SWP is live would lose
				// the block's audio.
				if (!__atomic_load_n(&m_bSwpActive, __ATOMIC_ACQUIRE))
				{
					m_bLastPublishOk = false;
					break;
				}
				mu50spsc::CpuRelax();
			}
		}
		m_nLastPublishedSeq = nSeq;
		__atomic_store_n(&m_nPublishedCycles, m_pH8->cycles(), __ATOMIC_RELEASE);
	}
}

void CMU50Synth::QueueMIDIMessageBytes(u8 nStatus, u8 nData1, u8 nData2)
{
	// Byte-length rules mirror Nuked: the emulated UART parses the
	// stream like the real hardware, so an extra data byte would
	// corrupt the following message.
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

	for (size_t i = 0; i < nBytes; ++i)
		if (m_pH8->queue_midi_byte(Bytes[i]))
			++m_nDiagMidiQueued;
}

// Front end: the real Yamaha firmware on the H8 CPU port. Channel and
// system messages are queued byte-by-byte into SCI1 RX; the firmware's
// own MIDI parser consumes them, exactly as on hardware. The H8 line
// driver sends queued bytes at 31250 baud whenever the line is idle,
// and the SCI only samples them once the firmware enables RX, so MIDI
// sent while the firmware is still booting is dropped, as on hardware.
// Note: the H8 MIDI queue is 256 bytes and drops on overrun, so a
// single SysEx longer than that truncates (short control SysExes,
// including XG/GM resets, are unaffected).
void CMU50Synth::HandleMIDIShortMessage(u32 nMessage)
{
	const u8 nStatus = nMessage & 0xFF;
	const u8 nData1  = (nMessage >> 8) & 0xFF;
	const u8 nData2  = (nMessage >> 16) & 0xFF;

	if (!m_pH8)
		return;

	// Handle system reset (0xFF) host-side, like EmuSC: reset the
	// whole front end (firmware reboots and re-initializes the SWP00)
	// rather than forwarding the byte to the firmware parser.
	if (nStatus == 0xFF)
	{
		if (m_bSplitMode)
		{
			// Split: the reset touches audio-owned (H8, resampler)
			// and SWP-owned (registers, ring) state, so it needs
			// both threads quiescent. Park the SWP thread, wait
			// for the audio thread to leave RenderResampledSplit
			// (bounded; a Render call is ~10 ms), then reset with
			// both threads out. On timeout the reset is dropped
			// (logged) rather than racing the audio thread.
			if (!ParkSwp())
			{
				LOGWARN("MU50 split reset dropped: SWP park timeout");
				return;
			}
			unsigned nSpins = 0;
			while (__atomic_load_n(&m_bAudioBusy, __ATOMIC_ACQUIRE) && nSpins < 10000000U)
			{
				mu50spsc::CpuRelax();
				++nSpins;
			}
			if (__atomic_load_n(&m_bAudioBusy, __ATOMIC_ACQUIRE))
			{
				LOGWARN("MU50 split reset dropped: audio busy timeout");
				UnparkSwp();
				return;
			}
			m_pH8->reset();
			if (m_pSynth)
				m_pSynth->reset();
			m_dResampleCarry  = 0.0;
			m_bResamplePrimed = false;
			m_PrevSample[0] = m_PrevSample[1] = 0;
			m_LeftoverSample[0][0] = m_LeftoverSample[0][1] = 0;
			m_LeftoverSample[1][0] = m_LeftoverSample[1][1] = 0;
			m_dH8CycleCarry = 0.0;
			m_bH8Halted     = false;
			m_WEventFifo.Clear();
			m_PublishedBlocks.Clear();
			m_MidiStage.Clear();
			// Drain the ring (SWP parked, audio idle: safe).
			{
				TNativeFrame Dropped;
				while (m_NativeRing.Dequeue(Dropped))
					;
			}
			m_nHeldNotes      = 0;
			m_bSustain        = false;
			m_bSustainedNotes = false;
			UnparkSwp();
			return;
		}
		m_Lock.Acquire();
		m_pH8->reset();
		if (m_pSynth)
			m_pSynth->reset();
		m_dResampleCarry  = 0.0;
		m_bResamplePrimed = false;
		m_PrevSample[0] = m_PrevSample[1] = 0;
		m_LeftoverSample[0][0] = m_LeftoverSample[0][1] = 0;
		m_LeftoverSample[1][0] = m_LeftoverSample[1][1] = 0;
		m_dH8CycleCarry = 0.0;
		m_bH8Halted     = false;
		// FIFO is empty at every Render return by construction
		// (pushes happen only inside AdvanceH8, drained before
		// the block render), but a reset must never leave stale
		// events for the fresh boot; cleared for M2-readiness.
		m_WEventFifo.Clear();
		m_nHeldNotes      = 0;
		m_bSustain        = false;
		m_bSustainedNotes = false;
		m_Lock.Release();
		return;
	}

	if (m_bSplitMode)
	{
		// Stage the message; the audio thread drains it at the next
		// AdvanceH8 (note tracking + monitor update happen there).
		// No m_Lock: the MIDI thread never touches H8 state.
		const TMidiStage Stage{ 1, nStatus, nData1, nData2 };
		if (!m_MidiStage.TryPush(Stage))
			__atomic_add_fetch(&m_nStageDrops, 1, __ATOMIC_RELAXED);
		return;
	}

	// Sounding-note tracking for power management (Nuked pattern):
	// a note held, or held by the sustain pedal, keeps IsActive()
	// true through quiet passages, so the clock cannot drop to
	// power-save mid-performance.
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

	// Serialize MIDI input against Render() on the audio core.
	m_Lock.Acquire();
	QueueMIDIMessageBytes(nStatus, nData1, nData2);
	m_Lock.Release();

	// Update MIDI monitor
	CSynthBase::HandleMIDIShortMessage(nMessage);
}

void CMU50Synth::HandleMIDISysExMessage(const u8* pData, size_t nSize)
{
	if (!m_pH8 || !pData || nSize == 0)
		return;

	// The firmware expects the complete message including the leading
	// 0xF0 and trailing 0xF7, and parses the stream itself.
	if (m_bSplitMode)
	{
		// Stage byte by byte; the audio thread drains them in order.
		for (size_t i = 0; i < nSize; ++i)
		{
			const TMidiStage Stage{ 0, pData[i], 0, 0 };
			if (!m_MidiStage.TryPush(Stage))
				__atomic_add_fetch(&m_nStageDrops, 1, __ATOMIC_RELAXED);
		}
		return;
	}
	m_Lock.Acquire();
	for (size_t i = 0; i < nSize; ++i)
		if (m_pH8->queue_midi_byte(pData[i]))
			++m_nDiagMidiQueued;
	m_Lock.Release();
}

bool CMU50Synth::IsActive()
{
	if (!m_pH8 || !m_pSynth)
		return false;

	// Nuked pattern: audible output within the last 0.5 s of native
	// frames, or a note held / held by the sustain pedal. Counters
	// are read without the lock, as in Nuked (benign races).
	return (m_nWrittenNativeFrames - m_nLastActiveNativeFrame) < (44100 / 2)
	    || m_nHeldNotes > 0 || m_bSustainedNotes;
}

void CMU50Synth::AllSoundOff()
{
	if (m_pH8)
	{
		if (m_bSplitMode)
		{
			// Stage the control entry; the drain queues the 32
			// messages, clears note tracking, and resets the
			// monitor, all on the audio thread.
			const TMidiStage Stage{ 2, 0, 0, 0 };
			if (!m_MidiStage.TryPush(Stage))
				__atomic_add_fetch(&m_nStageDrops, 1, __ATOMIC_RELAXED);
		}
		else
		{
			// CC 120 (All Sound Off) and CC 123 (All Notes Off) on
			// every channel; the firmware handles them like real
			// hardware. The H8 and register core are NOT reset
			// here: the firmware owns the voice state.
			m_Lock.Acquire();
			for (u8 nChannel = 0; nChannel < 16; ++nChannel)
			{
				const u8 SoundOff[3] = { static_cast<u8>(0xB0 | nChannel), 120, 0 };
				const u8 NotesOff[3] = { static_cast<u8>(0xB0 | nChannel), 123, 0 };
				QueueMIDIMessageBytes(SoundOff[0], SoundOff[1], SoundOff[2]);
				QueueMIDIMessageBytes(NotesOff[0], NotesOff[1], NotesOff[2]);
			}
			m_Lock.Release();
		}
	}

	// Sounding-note tracking (see IsActive): an All Sound Off
	// clears every held/sustained note. Split mode: the drain does
	// this (audio thread); doing it here would race the drain.
	// (Without an H8 there is no drain, so clear directly.)
	if (!m_bSplitMode || !m_pH8)
	{
		m_nHeldNotes      = 0;
		m_bSustain        = false;
		m_bSustainedNotes = false;
	}

	// Reset MIDI monitor. Split mode: the drain does this.
	if (!m_bSplitMode || !m_pH8)
		CSynthBase::AllSoundOff();
}

void CMU50Synth::SetMasterVolume(u8 nVolume)
{
	m_nVolume = nVolume;
	// libmu50 has no volume API: volume is applied as a scale factor
	// in RenderResampled(), together with the per-engine gain.
}

// Resample one pulled native block (see the header). Extracted
// verbatim from M1's RenderResampled body; the M1 loop and the
// split loop both call it, so the math cannot drift apart.
double CMU50Synth::ResamplePulledBlock(float* pOutBuffer, size_t nOutDone,
                                       size_t nBlockOut, double dBlockStart,
                                       size_t nNativeRender)
{
	const double dRatio = m_dResampleRatio;
	const float fScale  = m_nGain * (static_cast<float>(m_nVolume) / 100.0f);
	// Activity for IsActive(): any non-zero native sample in this
	// block marks the block's end as the last active frame.
	for (size_t i = 0; i < nNativeRender * 2; ++i)
	{
		if (m_NativeScratch[i] != 0)
		{
			m_nLastActiveNativeFrame = m_nWrittenNativeFrames + nNativeRender;
			break;
		}
	}
	// Window peak of the native SWP output, before resampling
	// and before gain: proves whether the SWP renders non-zero
	// audio independent of the resampler and volume scale.
	// Observe-only: this scan reads m_NativeScratch and changes
	// no audio state.
	for (size_t i = 0; i < nNativeRender * 2; ++i)
	{
		int nSample = m_NativeScratch[i];
		if (nSample < 0)
			nSample = -nSample;
		if (nSample > m_nDiagNativePeak)
			m_nDiagNativePeak = nSample;
	}
	m_nWrittenNativeFrames += nNativeRender;

	// Effective native frame k: primed blocks start with the two
	// carried frames, then the freshly rendered scratch frames;
	// the first block has no carried frames
	auto NativeFrame = [&](size_t nIndex) -> const s16*
	{
		if (m_bResamplePrimed)
		{
			if (nIndex < 2)
				return m_LeftoverSample[nIndex];
			return &m_NativeScratch[(nIndex - 2) * 2];
		}
		return &m_NativeScratch[nIndex * 2];
	};

	// Cubic's prev tap: the frame before nIndex. Primed blocks
	// take it from the carried prev frame; the first block has
	// no previous frame, so it duplicates frame 0 (Nuked's prime
	// behavior — at t = 0 the output is exactly frame 0 anyway).
	auto PrevFrame = [&](size_t nIndex) -> const s16*
	{
		if (nIndex == 0)
			return m_bResamplePrimed ? m_PrevSample : NativeFrame(0);
		return NativeFrame(nIndex - 1);
	};

	for (size_t i = 0; i < nBlockOut; ++i)
	{
		const double dPos = dBlockStart + static_cast<double>(i) * dRatio;
		const size_t nIndex = static_cast<size_t>(dPos);
		const double dFrac  = dPos - static_cast<double>(nIndex);
		const s16* pFrameM1 = PrevFrame(nIndex);
		const s16* pFrame0 = NativeFrame(nIndex);
		const s16* pFrame1 = NativeFrame(nIndex + 1);
		const s16* pFrame2 = NativeFrame(nIndex + 2);
		// Catmull-Rom weights (identical to Nuked's cubic)
		const float t = static_cast<float>(dFrac);
		const float t2 = t * t;
		const float t3 = t2 * t;
		const float w0 = -0.5f * t + t2 - 0.5f * t3;
		const float w1 = 1.0f - 2.5f * t2 + 1.5f * t3;
		const float w2 = 0.5f * t + 2.0f * t2 - 1.5f * t3;
		const float w3 = -0.5f * t2 + 0.5f * t3;
		const float fLeft  = (w0 * pFrameM1[0] + w1 * pFrame0[0] + w2 * pFrame1[0] + w3 * pFrame2[0]) / 32768.0f;
		const float fRight = (w0 * pFrameM1[1] + w1 * pFrame0[1] + w2 * pFrame1[1] + w3 * pFrame2[1]) / 32768.0f;
		pOutBuffer[(nOutDone + i) * 2]     = fLeft * fScale;
		pOutBuffer[(nOutDone + i) * 2 + 1] = fRight * fScale;
	}

	// Carry three frames into the next block: the frame before
	// the boundary (cubic's prev tap) plus the pair straddling
	// it. The prev frame is read first: for small nConsumed it
	// comes from the current leftover pair, which the lines
	// below overwrite.
	const double dEndPos = dBlockStart + static_cast<double>(nBlockOut) * dRatio;
	const size_t nConsumed = static_cast<size_t>(dEndPos);
	const s16* pPrev = NativeFrame(nConsumed > 0 ? nConsumed - 1 : 0);
	const s16 nPrev0 = pPrev[0];
	const s16 nPrev1 = pPrev[1];
	const s16* pLeftover0 = NativeFrame(nConsumed);
	const s16* pLeftover1 = NativeFrame(nConsumed + 1);
	m_PrevSample[0] = nPrev0;
	m_PrevSample[1] = nPrev1;
	m_LeftoverSample[0][0] = pLeftover0[0];
	m_LeftoverSample[0][1] = pLeftover0[1];
	m_LeftoverSample[1][0] = pLeftover1[0];
	m_LeftoverSample[1][1] = pLeftover1[1];
	m_bResamplePrimed = true;
	return dEndPos - static_cast<double>(nConsumed);

}

void CMU50Synth::RenderResampled(float* pOutBuffer, size_t nFrames)
{
	// Split mode: the lag-2 pipeline (no m_Lock; see
	// RenderResampledSplit).
	if (m_bSplitMode)
	{
		RenderResampledSplit(pOutBuffer, nFrames);
		return;
	}

	// Single-core mode, caller must hold m_Lock. Cubic Catmull-Rom
	// resampler (same 4-tap weights as Nuked's cubic): native
	// 44.1 kHz frames are rendered block by block and interpolated
	// to the output rate. Three native frames carry across blocks:
	// the frame before the boundary (the cubic's prev tap) plus the
	// pair straddling it.
	const double dRatio = m_dResampleRatio;

	size_t nOutDone = 0;
	while (nOutDone < nFrames)
	{
		// Cap the block so the native frames it needs fit the scratch
		const size_t nMaxBlockOut = static_cast<size_t>((kNativeScratchFrames - 3) / dRatio);
		const size_t nBlockOut = Utility::Min(nFrames - nOutDone,
		                                      Utility::Max(static_cast<size_t>(1), nMaxBlockOut));

		// Native frames to render: primed blocks render up to one
		// past the new leftover pair (the cubic's next tap); the
		// first block has no carried frames and renders three extra
		// (pair + next tap)
		double dBlockStart;
		size_t nNativeRender;
		if (m_bResamplePrimed)
		{
			dBlockStart   = m_dResampleCarry;
			nNativeRender = static_cast<size_t>(dBlockStart + nBlockOut * dRatio) + 1;
		}
		else
		{
			dBlockStart   = 0.0;
			nNativeRender = static_cast<size_t>(nBlockOut * dRatio) + 3;
		}

		// Advance the firmware by the time this block represents, so
		// its register writes land before the audio that reflects
		// them. The firmware boots lazily over the first seconds of
		// audio (no priming at Initialize, matching hardware power-on
		// behavior); MIDI sent before its SCI init is dropped, as on
		// hardware.
		AdvanceH8(nNativeRender);

		// M1: apply this block's W events before rendering it
		// (identity mode: exactly today's advance-then-render
		// semantics, with the FIFO as the only new step).
		DrainWEvents();

		const bool bDiag = CDiagLog::Get().IsEnabled();
		const u64 nSwpStartUs = bDiag ? CTimer::Get()->GetClockTicks() : 0;
		const libmu50::Status RenderStatus = m_pSynth->render(m_NativeScratch, nNativeRender);
		if (bDiag)
		{
			m_nDiagSwpUs += CTimer::Get()->GetClockTicks() - nSwpStartUs;
		}
		if (!RenderStatus)
		{
			// Should not happen after a successful Initialize();
			// fail safe to silence for the rest of the call
			memset(pOutBuffer + nOutDone * 2, 0, (nFrames - nOutDone) * 2 * sizeof(float));
			return;
		}

		// M1: native sample ring round-trip, mirroring Nuked's
		// per-frame Enqueue/Dequeue. The block is enqueued then
		// dequeued back into the scratch buffer before any use,
		// so the resampler below consumes ring output; any ring
		// error changes PCM and fails the gate. Fill never exceeds
		// one block (<= 1024 frames) against 16384 capacity.
		for (size_t i = 0; i < nNativeRender; ++i)
		{
			const TNativeFrame Frame{ m_NativeScratch[i * 2], m_NativeScratch[i * 2 + 1] };
			if (!m_NativeRing.Enqueue(Frame))
				__atomic_add_fetch(&m_nRingDrops, 1, __ATOMIC_RELAXED);
		}
		for (size_t i = 0; i < nNativeRender; ++i)
		{
			TNativeFrame Frame;
			if (m_NativeRing.Dequeue(Frame))
			{
				m_NativeScratch[i * 2]     = Frame.nLeft;
				m_NativeScratch[i * 2 + 1] = Frame.nRight;
			}
			else
			{
				// Short pull in single-core mode: the block was
				// just enqueued, so this cannot happen without a
				// ring bug -- but if it ever does, it is an
				// underrun (silence inserted at pull), not a ring
				// drop (drops are enqueue-side only). Counted as
				// such since M3; M1 mislabeled it a drop.
				__atomic_add_fetch(&m_nUnderrunFrames, 1, __ATOMIC_RELAXED);
				m_NativeScratch[i * 2]     = 0;
				m_NativeScratch[i * 2 + 1] = 0;
			}
		}

		m_dResampleCarry = ResamplePulledBlock(pOutBuffer, nOutDone, nBlockOut, dBlockStart, nNativeRender);
		nOutDone += nBlockOut;
	}
}

void CMU50Synth::ResamplePendingFront(float* pOutBuffer)
{
	const TPendingBlock Entry = m_Pending[0];
	// Wait for the SWP thread to enqueue this block's frames.
	// Enqueued counts only successfully enqueued frames, and
	// consumed is stable on this thread during the wait, so the
	// difference cannot overstate the ring fill: the wait succeeds
	// only when the frames are really there. Drops (counted at
	// enqueue) would fail the gate, not hang the wait -- and the
	// consumed count below tracks actual dequeues, so a short pull
	// cannot wedge the next block's wait either.
	// M3: the wait also breaks when the SWP worker parks mid-wait
	// (switch-away race, or 0xFF park before its audio-idle wait):
	// without this, a parked SWP would wedge the audio thread
	// forever. The abort records Entry.nSeq, so the SWP core skips
	// this block's remaining frames on unpark (see
	// EnqueueSwpFrame); the pull below then fills silence for the
	// missing frames and counts them as underruns -- a true
	// ring-empty-at-pull event, and the M3 forced-starvation test
	// exercises exactly this path (park, then Render).
	const bool bDiag = CDiagLog::Get().IsEnabled();
	size_t nGot = 0;
	if (!Entry.bPublished)
	{
		// Never published (publish skipped while parked): no
		// frames will ever arrive for this block, so waiting
		// would wedge. Fill silence + count, no wait. (No abort
		// record needed: the block was never queued, so the SWP
		// core has nothing to skip.)
		for (size_t i = 0; i < Entry.nNativeRender; ++i)
		{
			__atomic_add_fetch(&m_nUnderrunFrames, 1, __ATOMIC_RELAXED);
			m_NativeScratch[i * 2]     = 0;
			m_NativeScratch[i * 2 + 1] = 0;
		}
	}
	else
	{
		const u64 nWaitStartUs = bDiag ? CTimer::Get()->GetClockTicks() : 0;
		while (__atomic_load_n(&m_nRenderedEnqueued, __ATOMIC_ACQUIRE) -
		       __atomic_load_n(&m_nConsumed, __ATOMIC_ACQUIRE) < Entry.nNativeRender)
		{
			if (!__atomic_load_n(&m_bSwpActive, __ATOMIC_ACQUIRE))
			{
				__atomic_store_n(&m_nAbortSeq, Entry.nSeq, __ATOMIC_RELEASE);
				break;
			}
			mu50spsc::CpuRelax();
		}
		if (bDiag)
			m_nDiagWaitUs += CTimer::Get()->GetClockTicks() - nWaitStartUs;
		for (size_t i = 0; i < Entry.nNativeRender; ++i)
		{
			TNativeFrame Frame;
			if (m_NativeRing.Dequeue(Frame))
			{
				m_NativeScratch[i * 2]     = Frame.nLeft;
				m_NativeScratch[i * 2 + 1] = Frame.nRight;
				++nGot;
			}
			else
			{
				// Ring-empty at pull (park-abort shortfall, or the
				// defensive case the wait above was meant to exclude):
				// silence + count. This retires the hardcoded
				// underrun_frames=0 -- every silence insertion here
				// is a counted, real underrun.
				__atomic_add_fetch(&m_nUnderrunFrames, 1, __ATOMIC_RELAXED);
				m_NativeScratch[i * 2]     = 0;
				m_NativeScratch[i * 2 + 1] = 0;
			}
		}
	}
	__atomic_add_fetch(&m_nConsumed, nGot, __ATOMIC_RELAXED);
	// Resample in order (helper updates the frame carry state).
	// Its carry return is discarded: the loop's virtual carry was
	// already advanced analytically with the identical formula.
	ResamplePulledBlock(pOutBuffer, Entry.nOutDone, Entry.nBlockOut,
	                    Entry.dBlockStart, Entry.nNativeRender);
	m_Pending[0] = m_Pending[1];
	--m_nPendingCount;
}

void CMU50Synth::RenderResampledSplit(float* pOutBuffer, size_t nFrames)
{
	// Audio thread, no m_Lock. Lag-2 pipeline: each iteration
	// computes the next block from VIRTUAL resampler state
	// (dCarry/bPrimedVirt -- the real carry member is only written
	// back at the end, and the primed member only by
	// ResamplePulledBlock in resample order), advances the H8 and
	// publishes, then resamples the oldest pending block once two
	// are pending. The SWP thread therefore renders block N while
	// the audio thread advances block N+1: true overlap, with block
	// boundaries identical to single-core (same formulas, same
	// order), so output is bit-identical. At call return every
	// block is resampled and the ring is drained (same invariant
	// as M1's FIFO-empty-at-Render-return).
	__atomic_store_n(&m_bAudioBusy, true, __ATOMIC_RELEASE);
	const double dRatio = m_dResampleRatio;
	double dCarry = m_dResampleCarry;
	bool bPrimedVirt = m_bResamplePrimed;
	size_t nOutDone = 0;
	m_nPendingCount = 0;
	while (nOutDone < nFrames)
	{
		const size_t nMaxBlockOut = static_cast<size_t>((kNativeScratchFrames - 3) / dRatio);
		const size_t nBlockOut = Utility::Min(nFrames - nOutDone,
		                                      Utility::Max(static_cast<size_t>(1), nMaxBlockOut));
		double dBlockStart;
		size_t nNativeRender;
		if (bPrimedVirt)
		{
			dBlockStart   = dCarry;
			nNativeRender = static_cast<size_t>(dBlockStart + nBlockOut * dRatio) + 1;
		}
		else
		{
			dBlockStart   = 0.0;
			nNativeRender = static_cast<size_t>(nBlockOut * dRatio) + 3;
		}
		// Advance + publish (drains staging, steps H8, publishes).
		AdvanceH8(nNativeRender);
		m_Pending[m_nPendingCount].nBlockOut     = nBlockOut;
		m_Pending[m_nPendingCount].dBlockStart   = dBlockStart;
		m_Pending[m_nPendingCount].nNativeRender = nNativeRender;
		m_Pending[m_nPendingCount].nOutDone      = nOutDone;
		m_Pending[m_nPendingCount].nSeq          = m_nLastPublishedSeq;
		m_Pending[m_nPendingCount].bPublished   = m_bLastPublishOk;
		++m_nPendingCount;
		// Advance the virtual state analytically (identical formula
		// to ResamplePulledBlock's carry update).
		const double dEndPos = dBlockStart + static_cast<double>(nBlockOut) * dRatio;
		dCarry = dEndPos - static_cast<double>(static_cast<size_t>(dEndPos));
		bPrimedVirt = true;
		nOutDone += nBlockOut;
		if (m_nPendingCount == 2)
			ResamplePendingFront(pOutBuffer);
	}
	while (m_nPendingCount > 0)
		ResamplePendingFront(pOutBuffer);
	m_dResampleCarry = dCarry;
	__atomic_store_n(&m_bAudioBusy, false, __ATOMIC_RELEASE);
}

void CMU50Synth::SetSplitMode(bool bSplit)
{
	// Contract: call before the SWP thread starts; afterwards the
	// flag is read-only (thread-creation happens-before covers it).
	m_bSplitMode = bSplit;
}

void CMU50Synth::PCMTaskIteration()
{
	// Unconditional-call discipline: the worker loop calls this
	// every iteration; gating is inside. If the call itself were
	// gated on m_bSwpActive, the park ack below would never run
	// and ParkSwp would deadlock (the Nuked wedge).
	if (!m_bSplitMode || !m_pSynth)
		return;
	if (!__atomic_load_n(&m_bSwpActive, __ATOMIC_ACQUIRE))
	{
		__atomic_store_n(&m_bSwpParkAck, true, __ATOMIC_RELEASE);
		// M4: parked branch yields (Nuked precedent) so the core-3
		// loop is scheduler-paced even when the Nuked synth does
		// not exist to pace it.
		CScheduler::Get()->Yield();
		return;
	}
	TPublishedBlock Block;
	if (!m_PublishedBlocks.TryPop(Block))
	{
		// Idle drain: no published block, but the audio thread may
		// be mid-block with a full W FIFO (tiny-FIFO stress, or a
		// pathological block burst over 4095 writes). Draining all
		// pending events here relieves the backpressure -- and it
		// cannot change output: the audio core is at most one block
		// ahead (lag-2), so idle-drained writes always belong to
		// the immediately-next block, whose render happens after
		// its publish anyway; the snapshot drain at pop time then
		// finds nothing new. Without this, a mid-block full FIFO
		// would deadlock (the SWP core could only drain up to a
		// snapshot that does not exist yet).
		DrainWEventsUpTo(m_WEventFifo.InIndex());
		// M4: idle backoff (Nuked precedent: 64 yield hints, then
		// a scheduler yield). The SWP thread spends most of its
		// time here while the audio thread steps the next block;
		// the Yield keeps the core-3 loop from spinning hot, and
		// on a single-task core it returns immediately, so publish
		// latency is unaffected.
		for (unsigned i = 0; i < 64; ++i)
			mu50spsc::CpuRelax();
		CScheduler::Get()->Yield();
		return;
	}
	DrainWEventsUpTo(Block.nFifoSnap);
	const u64 nSwpStartUs = CTimer::Get()->GetClockTicks();
	size_t nDone = 0;
	while (nDone < Block.nFrames)
	{
		const size_t nChunk = Utility::Min(Block.nFrames - nDone, kNativeScratchFrames);
		const libmu50::Status RenderStatus = m_pSynth->render(&m_SwpScratch[0], nChunk);
		if (!RenderStatus)
		{
			// Should not happen after a successful Initialize();
			// fail safe: enqueue silence for the remainder.
			for (size_t i = nDone; i < Block.nFrames; ++i)
			{
				const TNativeFrame Frame{ 0, 0 };
				EnqueueSwpFrame(Block.nSeq, Frame);
			}
			__atomic_add_fetch(&m_nSwpRenderUs, CTimer::Get()->GetClockTicks() - nSwpStartUs, __ATOMIC_RELAXED);
			UpdateRingHighWater();
			return;
		}
		for (size_t i = 0; i < nChunk; ++i)
		{
			const TNativeFrame Frame{ m_SwpScratch[i * 2], m_SwpScratch[i * 2 + 1] };
			EnqueueSwpFrame(Block.nSeq, Frame);
		}
		nDone += nChunk;
	}
	__atomic_add_fetch(&m_nSwpRenderUs, CTimer::Get()->GetClockTicks() - nSwpStartUs, __ATOMIC_RELAXED);
	UpdateRingHighWater();
}

bool CMU50Synth::EnqueueSwpFrame(u64 nSeq, const TNativeFrame& Frame)
{
	// Skip frames of blocks the audio thread aborted (parked
	// mid-wait): the audio thread already consumed silence for
	// them, so enqueuing their tail now would poison the next
	// block's pull (the counters are the sync mechanism, and a
	// skipped frame is never counted as enqueued).
	if (nSeq <= __atomic_load_n(&m_nAbortSeq, __ATOMIC_ACQUIRE))
		return false;
	if (m_NativeRing.Enqueue(Frame))
	{
		__atomic_add_fetch(&m_nRenderedEnqueued, 1, __ATOMIC_RELAXED);
		return true;
	}
	__atomic_add_fetch(&m_nRingDrops, 1, __ATOMIC_RELAXED);
	return false;
}

void CMU50Synth::UpdateRingHighWater()
{
	// SWP-thread only (single writer): sample the ring fill after
	// each rendered block; the max over the window is the split
	// line's max-lag figure. A concurrent UpdateLCD reset just
	// starts a fresh window (diag-grade).
	const u64 nFill = __atomic_load_n(&m_nRenderedEnqueued, __ATOMIC_ACQUIRE) -
	                  __atomic_load_n(&m_nConsumed, __ATOMIC_ACQUIRE);
	if (nFill > __atomic_load_n(&m_nRingHighWater, __ATOMIC_ACQUIRE))
		__atomic_store_n(&m_nRingHighWater, nFill, __ATOMIC_RELAXED);
}

bool CMU50Synth::ParkSwp()
{
	__atomic_store_n(&m_bSwpParkAck, false, __ATOMIC_RELEASE);
	__atomic_store_n(&m_bSwpActive, false, __ATOMIC_RELEASE);
	// The worker loop calls PCMTaskIteration continuously (yielding
	// on idle), so the ack arrives within microseconds; the bound
	// below is a wedge guard, not a timing assumption. Park must be
	// called while the worker thread is running -- parking a joined
	// thread can never ack, and correctly times out.
	for (unsigned nSpins = 0; nSpins < 10000000U; ++nSpins)
	{
		if (__atomic_load_n(&m_bSwpParkAck, __ATOMIC_ACQUIRE))
			return true;
		mu50spsc::CpuRelax();
	}
	return false;
}

void CMU50Synth::UnparkSwp()
{
	__atomic_store_n(&m_bSwpActive, true, __ATOMIC_RELEASE);
}

u64 CMU50Synth::GetWqFullWaits()
{
	return __atomic_load_n(&m_nWqFullWaits, __ATOMIC_ACQUIRE);
}

u64 CMU50Synth::GetRingDrops()
{
	return __atomic_load_n(&m_nRingDrops, __ATOMIC_ACQUIRE);
}

u64 CMU50Synth::GetBlockWaits()
{
	return __atomic_load_n(&m_nBlockWaits, __ATOMIC_ACQUIRE);
}

u64 CMU50Synth::GetStageDrops()
{
	return __atomic_load_n(&m_nStageDrops, __ATOMIC_ACQUIRE);
}

u64 CMU50Synth::GetRenderedEnqueued()
{
	return __atomic_load_n(&m_nRenderedEnqueued, __ATOMIC_ACQUIRE);
}

u64 CMU50Synth::GetConsumed()
{
	return __atomic_load_n(&m_nConsumed, __ATOMIC_ACQUIRE);
}

u64 CMU50Synth::GetUnderrunFrames()
{
	return __atomic_load_n(&m_nUnderrunFrames, __ATOMIC_ACQUIRE);
}

void CMU50Synth::SplitActivate()
{
	if (!m_bSplitMode || !m_pSynth)
		return;
	UnparkSwp();
}

void CMU50Synth::SplitDeactivate()
{
	if (!m_bSplitMode || !m_pSynth)
		return;
	// Called on switch-from-MU50 (core 0, after m_pCurrentSynth
	// already moved: no NEW MU50 Render can start after this
	// call, only a pre-switch load can still be in flight). Park
	// first, then wait for the audio thread to leave Render: an
	// in-flight Render completes promptly because the park-abort
	// in ResamplePendingFront breaks its wait with silence
	// instead of wedging on a parked SWP core. On timeout, unpark
	// and proceed -- the worker stays live but idle-draining
	// (harmless: no MU50 blocks are published while another synth
	// plays), and the in-flight Render completes normally.
	if (!ParkSwp())
	{
		LOGWARN("MU50 split deactivate: SWP park timeout");
		return;
	}
	unsigned nSpins = 0;
	while (__atomic_load_n(&m_bAudioBusy, __ATOMIC_ACQUIRE) && nSpins < 10000000U)
	{
		mu50spsc::CpuRelax();
		++nSpins;
	}
	if (__atomic_load_n(&m_bAudioBusy, __ATOMIC_ACQUIRE))
	{
		LOGWARN("MU50 split deactivate: audio busy timeout, leaving SWP live");
		UnparkSwp();
	}
}

bool CMU50Synth::SplitQueuesQuiescent()
{
	return m_WEventFifo.EmptyForConsumer() &&
	       m_PublishedBlocks.EmptyForConsumer() &&
	       __atomic_load_n(&m_nRenderedEnqueued, __ATOMIC_ACQUIRE) ==
	           __atomic_load_n(&m_nConsumed, __ATOMIC_ACQUIRE);
}

void CMU50Synth::SetWEventFifoCapForTest(size_t nCap)
{
	m_nWEventFifoCap = nCap > 0 ? Utility::Min(nCap, kWEventFifoSize - 1) : 1;
}

size_t CMU50Synth::Render(float* pOutBuffer, size_t nFrames)
{
	if (!m_pSynth)
	{
		memset(pOutBuffer, 0, nFrames * 2 * sizeof(float));
		return nFrames;
	}

	const bool bDiag = CDiagLog::Get().IsEnabled();
	const u64 nStartUs = bDiag ? CTimer::Get()->GetClockTicks() : 0;

	// Split mode: m_Lock narrows to switch/park/diag only (see the
	// header). The audio thread owns H8 + resampler here; diag
	// counters are audio-thread-written and read by UpdateLCD
	// between Render calls, as in Nuked's lock-free pattern.
	if (!m_bSplitMode)
		m_Lock.Acquire();
	if (bDiag)
	{
		if (m_nDiagWindowStartUs == 0)
			m_nDiagWindowStartUs = nStartUs;
		++m_nDiagCalls;
		m_nDiagOutputFrames += nFrames;
	}
	RenderResampled(pOutBuffer, nFrames);
	if (bDiag)
	{
		const u64 nElapsedUs = CTimer::Get()->GetClockTicks() - nStartUs;
		m_nDiagTotalUs += nElapsedUs;
		if (nElapsedUs > m_nDiagMaxUs)
			m_nDiagMaxUs = nElapsedUs;
	}
	if (!m_bSplitMode)
		m_Lock.Release();

	return nFrames;
}

size_t CMU50Synth::Render(s16* pOutBuffer, size_t nFrames)
{
	if (!m_pSynth)
	{
		memset(pOutBuffer, 0, nFrames * 2 * sizeof(s16));
		return nFrames;
	}

	const bool bDiag = CDiagLog::Get().IsEnabled();
	const u64 nStartUs = bDiag ? CTimer::Get()->GetClockTicks() : 0;

	// Split mode: m_Lock narrows to switch/park/diag only (see the
	// float overload).
	if (!m_bSplitMode)
		m_Lock.Acquire();
	if (bDiag)
	{
		if (m_nDiagWindowStartUs == 0)
			m_nDiagWindowStartUs = nStartUs;
		++m_nDiagCalls;
		m_nDiagOutputFrames += nFrames;
	}
	size_t nDone = 0;
	while (nDone < nFrames)
	{
		const size_t nBlock = Utility::Min(nFrames - nDone,
		                                   sizeof(m_FloatScratch) / sizeof(m_FloatScratch[0]) / 2);
		RenderResampled(m_FloatScratch, nBlock);
		for (size_t i = 0; i < nBlock; ++i)
		{
			pOutBuffer[(nDone + i) * 2]     = static_cast<s16>(Utility::Clamp(m_FloatScratch[i * 2], -1.0f, 1.0f) * 32767.0f);
			pOutBuffer[(nDone + i) * 2 + 1] = static_cast<s16>(Utility::Clamp(m_FloatScratch[i * 2 + 1], -1.0f, 1.0f) * 32767.0f);
		}
		nDone += nBlock;
	}
	if (bDiag)
	{
		const u64 nElapsedUs = CTimer::Get()->GetClockTicks() - nStartUs;
		m_nDiagTotalUs += nElapsedUs;
		if (nElapsedUs > m_nDiagMaxUs)
			m_nDiagMaxUs = nElapsedUs;
	}
	if (!m_bSplitMode)
		m_Lock.Release();

	return nFrames;
}

void CMU50Synth::ReportStatus() const
{
	if (m_pUI)
		m_pUI->ShowSystemMessage("MU50");
}

void CMU50Synth::UpdateLCD(CLCD& LCD, unsigned int nTicks)
{
	const u8 nBarHeight = LCD.Height();
	float ChannelLevels[16], PeakLevels[16];
	m_MIDIMonitor.GetChannelLevels(nTicks, ChannelLevels, PeakLevels, 1 << 9);
	CUserInterface::DrawChannelLevels(LCD, nBarHeight, ChannelLevels, PeakLevels, 16, true);

	// Flush accumulated render timing statistics to the diagnostic
	// log (mirrors the Nuked window line). The stats copy happens
	// under m_Lock, but the log write itself happens after the lock
	// is released, so it never nests under m_Lock. M3: the window
	// line's underrun_frames is now the counted ring-empty pulls
	// (window delta of the cumulative atomic -- the hardcoded 0 is
	// gone); in split mode swp_us is the SWP core's render time
	// delta, and a third split line carries the split counters.
	// halted sticks at 1 if the H8 ever stops on an illegal opcode
	// (cleared only by a system reset).
	CDiagLog& DiagLog = CDiagLog::Get();
	if (DiagLog.IsEnabled())
	{
		const u64 nNowUs = CTimer::Get()->GetClockTicks();

		m_Lock.Acquire();
		const bool bDue = m_nDiagCalls > 0 && m_nDiagWindowStartUs != 0 &&
			nNowUs - m_nDiagWindowStartUs >= static_cast<u64>(DiagLog.IntervalSeconds()) * 1000000ULL;
		u64 nWindowUs = 0, nCalls = 0, nTotalUs = 0, nMaxUs = 0;
		u64 nH8Us = 0, nSwpUs = 0, nH8Insns = 0, nOutputFrames = 0;
		u64 nMidiQueued = 0, nSciRead = 0, nSwpWrites = 0;
		u64 nWaitUs = 0, nUnderrunDelta = 0, nRingHw = 0;
		int nNativePeak = 0;
		if (bDue)
		{
			nWindowUs     = nNowUs - m_nDiagWindowStartUs;
			nCalls        = m_nDiagCalls;
			nTotalUs      = m_nDiagTotalUs;
			nMaxUs        = m_nDiagMaxUs;
			nH8Us         = m_nDiagH8Us;
			nSwpUs        = m_nDiagSwpUs;
			nH8Insns      = m_nDiagH8Insns;
			nOutputFrames = m_nDiagOutputFrames;
			nMidiQueued   = m_nDiagMidiQueued;
			nSciRead      = m_nDiagSciRead;
			nSwpWrites    = m_nDiagSwpWrites;
			nNativePeak   = m_nDiagNativePeak;
			nWaitUs       = m_nDiagWaitUs;
			// Split mode: SWP render time lives in the SWP
			// thread's cumulative atomic (m_nDiagSwpUs is never
			// written by the split Render path); report the
			// window delta, like the other windowed fields.
			if (m_bSplitMode)
			{
				const u64 nSwpNow = __atomic_load_n(&m_nSwpRenderUs, __ATOMIC_ACQUIRE);
				nSwpUs = nSwpNow - m_nDiagLastSwpRenderUs;
				m_nDiagLastSwpRenderUs = nSwpNow;
			}
			const u64 nUnderrunNow = __atomic_load_n(&m_nUnderrunFrames, __ATOMIC_ACQUIRE);
			nUnderrunDelta = nUnderrunNow - m_nDiagLastUnderrunFrames;
			m_nDiagLastUnderrunFrames = nUnderrunNow;
			nRingHw = __atomic_exchange_n(&m_nRingHighWater, 0, __ATOMIC_ACQ_REL);
			m_nDiagWindowStartUs = 0;
			m_nDiagCalls         = 0;
			m_nDiagTotalUs       = 0;
			m_nDiagMaxUs         = 0;
			m_nDiagH8Us          = 0;
			m_nDiagSwpUs         = 0;
			m_nDiagH8Insns       = 0;
			m_nDiagOutputFrames  = 0;
			m_nDiagMidiQueued    = 0;
			m_nDiagSciRead       = 0;
			m_nDiagSwpWrites     = 0;
			m_nDiagNativePeak    = 0;
			m_nDiagWaitUs        = 0;
		}
		const bool bHalted = m_bH8Halted;
		m_Lock.Release();

		if (bDue)
		{
			// pct: output frames written vs frames due in the window
			// at the engine output rate (real-time rate, as in Nuked).
			const u64 nAvgUs = nCalls > 0 ? nTotalUs / nCalls : 0;
			u64 nPct = 0;
			if (nWindowUs > 0 && m_nSampleRate > 0)
			{
				const u64 nDueFrames = nWindowUs * static_cast<u64>(m_nSampleRate) / 1000000ULL;
				if (nDueFrames > 0)
					nPct = nOutputFrames * 100 / nDueFrames;
			}
			DiagLog.LogLine("window synth=mu50 pct=%llu avg_us=%llu max_us=%llu h8_us=%llu swp_us=%llu h8_insn=%llu underrun_frames=%llu halted=%u",
			                static_cast<unsigned long long>(nPct),
			                static_cast<unsigned long long>(nAvgUs),
			                static_cast<unsigned long long>(nMaxUs),
			                static_cast<unsigned long long>(nH8Us),
			                static_cast<unsigned long long>(nSwpUs),
			                static_cast<unsigned long long>(nH8Insns),
			                static_cast<unsigned long long>(nUnderrunDelta),
			                static_cast<unsigned>(bHalted ? 1 : 0));
			// Second diag line (new counters only; the window line
			// above is unchanged): per-window MIDI bytes accepted
			// into the SCI1 queue, SCI1 RDR reads by the firmware,
			// SWP00 register writes, and the peak native SWP sample
			// before resampling/gain. Read against the window line,
			// it separates "MIDI never reaches the firmware"
			// (midi_queued=0) from "firmware reads nothing"
			// (sci_read=0) from "firmware writes no notes"
			// (swp_writes small) from "SWP renders silence"
			// (native_peak=0).
			DiagLog.LogLine("window synth=mu50 diag midi_queued=%llu sci_read=%llu swp_writes=%llu native_peak=%d",
			                static_cast<unsigned long long>(nMidiQueued),
			                static_cast<unsigned long long>(nSciRead),
			                static_cast<unsigned long long>(nSwpWrites),
			                static_cast<int>(nNativePeak));
			// Third diag line, split mode only (M3): the split
			// counters (cumulative, never reset -- wq_full_waits
			// and ring_drops must stay 0 for the whole run),
			// current ring fill + per-window max lag in frames
			// (max fill observed at block-enqueue time), audio
			// wait time, and per-core duty cycles: audio_pct is
			// Render wall time minus SWP-wait spins over the
			// window (the H8 + resample load on core 2), swp_pct
			// is SWP render time over the window (core 3).
			if (m_bSplitMode)
			{
				const u64 nRendered = GetRenderedEnqueued();
				const u64 nConsumed = GetConsumed();
				u64 nAudioPct = 0, nSwpPct = 0;
				if (nWindowUs > 0)
				{
					const u64 nBusy = nTotalUs > nWaitUs ? nTotalUs - nWaitUs : 0;
					nAudioPct = nBusy * 100 / nWindowUs;
					nSwpPct = nSwpUs * 100 / nWindowUs;
				}
				DiagLog.LogLine("window synth=mu50 split wq_full_waits=%llu ring_drops=%llu block_waits=%llu stage_drops=%llu rendered=%llu consumed=%llu quiescent=%u ring_fill=%llu max_lag_frames=%llu wait_us=%llu audio_pct=%llu swp_pct=%llu",
				                static_cast<unsigned long long>(GetWqFullWaits()),
				                static_cast<unsigned long long>(GetRingDrops()),
				                static_cast<unsigned long long>(GetBlockWaits()),
				                static_cast<unsigned long long>(GetStageDrops()),
				                static_cast<unsigned long long>(nRendered),
				                static_cast<unsigned long long>(nConsumed),
				                static_cast<unsigned>(SplitQueuesQuiescent() ? 1 : 0),
				                static_cast<unsigned long long>(nRendered - nConsumed),
				                static_cast<unsigned long long>(nRingHw),
				                static_cast<unsigned long long>(nWaitUs),
				                static_cast<unsigned long long>(nAudioPct),
				                static_cast<unsigned long long>(nSwpPct));
			}
		}
	}
}
