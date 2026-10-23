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

#include <cstring>
#include <span>
#include <string>
#include <strings.h>
#include <vector>

#include <circle/cputhrottle.h>
#include <circle/logger.h>
#include <circle/memory.h>
#include <circle/timer.h>
#include <fatfs/ff.h>

#include "config.h"
#include "diaglog.h"
#include "lcd/ui.h"
#include "synth/nukedsc55synth.h"
#include "utility.h"

// Nuked-SC55 backend headers are only included here (this file is compiled
// as C++20; see Kernel.mk). The compat shim must come first: it supplies
// the C++23 library functions the bare-metal libstdc++ lacks (see the
// header itself); it is also force-included into the backend build.
#include "nukedcirclecompat.h"

#include "emu.h"
#include "pcm.h"
#include "rom.h"
#include "rom_io.h"
#include "sha256.h"
#include "standard_romsets.h"

LOGMODULE("nukedsc55");

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

CNukedSC55Synth::CNukedSC55Synth(unsigned nSampleRate, float nGain, bool bOversampling)
	: CSynthBase(nSampleRate),
	  m_pEmulator(nullptr),
	  m_pRomsetInfo(nullptr),
	  m_nGain(nGain),
	  m_bOversampling(bOversampling),
	  m_nVolume(100),
	  m_nNativeRate(0),
	  m_dResampleRatio(1.0),
	  m_dReadPosition(0.0),
	  m_nWrittenFrames(0),
	  m_nLastActiveFrame(0),
	  m_Stats{},
	  m_StatsLock(TASK_LEVEL),
	  m_RingBuffer{},
	  m_ModelName{},
	  m_ROMVersion{}
{
}

CNukedSC55Synth::~CNukedSC55Synth()
{
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
	constexpr float Scale = 1.0f / 536870912.0f;
	m_RingBuffer[m_nWrittenFrames & (RingBufferFrames - 1)][0] = nLeft * Scale;
	m_RingBuffer[m_nWrittenFrames & (RingBufferFrames - 1)][1] = nRight * Scale;
	++m_nWrittenFrames;
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

	m_pEmulator->SetSampleCallback([](void* pUserData, const AudioFrame<int32_t>& Frame) {
		static_cast<CNukedSC55Synth*>(pUserData)->OnSample(Frame.left, Frame.right);
	}, this);

	m_pEmulator->Reset();

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

	strncpy(m_ModelName, FriendlyModelName(pDefinition->romset), sizeof(m_ModelName) - 1);
	const char* pVersion = strstr(pDefinition->name, "-v");
	strncpy(m_ROMVersion, pVersion ? pVersion + 2 : pDefinition->name, sizeof(m_ROMVersion) - 1);

	LOGNOTE("Nuked-SC55 initialized: romset %s (%s v%s), native rate %u Hz, oversampling %s",
	        pDefinition->name, m_ModelName, m_ROMVersion, m_nNativeRate, m_bOversampling ? "on" : "off");

	CDiagLog::Get().LogLine("init synth=nuked romset=%s rate=%u oversampling=%s heap_free_low_kb=%u",
	                        pDefinition->name, m_nNativeRate, m_bOversampling ? "on" : "off",
	                        static_cast<unsigned>(CMemorySystem::Get()->GetHeapFreeSpace(HEAP_LOW) / 1024));

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
		m_pEmulator->Reset();
		m_pEmulator->PostSystemReset(EMU_SystemReset::GS_RESET);
		m_Lock.Release();
		return;
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

	return (m_nWrittenFrames - m_nLastActiveFrame) < (m_nNativeRate / 2);
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

	// Reset MIDI monitor
	CSynthBase::AllSoundOff();
}

void CNukedSC55Synth::SetMasterVolume(u8 nVolume)
{
	// The emulated firmware owns its internal levels; mt32-pi's master
	// volume is applied as an output scale in Render(), like the gain.
	m_nVolume = nVolume;
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

	// Step the emulator until enough native-rate frames are available
	const double dNeeded = static_cast<double>(static_cast<u64>(m_dReadPosition)) + nFrames * m_dResampleRatio + 2.0;
	u64 nSteps           = 0;
	const u64 nStepBudget = static_cast<u64>(nFrames * m_dResampleRatio + 2.0) * StepsPerFrameBudget;
	while (static_cast<double>(m_nWrittenFrames) < dNeeded && nSteps < nStepBudget)
	{
		m_pEmulator->Step();
		++nSteps;
	}

	for (size_t i = 0; i < nFrames; ++i)
	{
		const u64 nIndex = static_cast<u64>(m_dReadPosition);
		if (nIndex + 1 >= m_nWrittenFrames)
		{
			// Underrun (emulator too slow, or still booting): emit
			// silence and resync to the newest frame rather than
			// replaying stale ring contents.
			pOutBuffer[i * 2]     = 0.0f;
			pOutBuffer[i * 2 + 1] = 0.0f;
			m_dReadPosition       = static_cast<double>(m_nWrittenFrames);
			++nUnderrunFrames;
			continue;
		}

		const float nFrac = static_cast<float>(m_dReadPosition - static_cast<double>(nIndex));
		const float* pA   = m_RingBuffer[nIndex & (RingBufferFrames - 1)];
		const float* pB   = m_RingBuffer[(nIndex + 1) & (RingBufferFrames - 1)];

		pOutBuffer[i * 2]     = (pA[0] + (pB[0] - pA[0]) * nFrac) * nScale;
		pOutBuffer[i * 2 + 1] = (pA[1] + (pB[1] - pA[1]) * nFrac) * nScale;

		m_dReadPosition += m_dResampleRatio;
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
	const u8 nBarHeight = LCD.Height();
	float ChannelLevels[16], PeakLevels[16];
	m_MIDIMonitor.GetChannelLevels(nTicks, ChannelLevels, PeakLevels, 1 << 9);
	CUserInterface::DrawChannelLevels(LCD, nBarHeight, ChannelLevels, PeakLevels, 16, true);

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
			DiagLog.LogLine("render synth=nuked calls=%llu frames=%llu avg_us=%llu max_us=%llu miss_pct=%llu underrun_frames=%llu temp_c=%u",
			                static_cast<unsigned long long>(Stats.nCalls),
			                static_cast<unsigned long long>(Stats.nFrames),
			                static_cast<unsigned long long>(Stats.nTotalUs / Stats.nCalls),
			                static_cast<unsigned long long>(Stats.nMaxUs),
			                static_cast<unsigned long long>(Stats.nMisses * 100 / Stats.nCalls),
			                static_cast<unsigned long long>(Stats.nUnderrunFrames),
			                pThrottle ? pThrottle->GetTemperature() : 0);
		}
	}
}
