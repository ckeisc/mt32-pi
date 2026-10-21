//
// emuscsynth.cpp
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
#include <string>
#include <strings.h>
#include <vector>

#include <circle/logger.h>
#include <fatfs/ff.h>

#include "config.h"
#include "lcd/ui.h"
#include "synth/emuscsynth.h"
#include "utility.h"

// libEmuSC headers are only included here (this file is compiled as C++17;
// see Kernel.mk). The compat shim must come first: it provides the
// std::mutex that the bare-metal libstdc++ lacks (see the header itself
// for why a no-op mutex is sound in this integration).
#include "emuscirclecompat.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wignored-qualifiers"
#include <emusc/control_rom.h>
#include <emusc/synth.h>
#include <emusc/wave_rom.h>
#pragma GCC diagnostic pop

LOGMODULE("emuscsynth");

// Sound Canvas ROM sets live in their own directory alongside the MT-32
// "roms" and "soundfonts" directories
const char* const EmuSCDisks[] = { "SD", "USB" };
const char EmuSCROMDirectory[] = "roms/sc55";

// ROM files are classified by size, mirroring libEmuSC's own expectations:
// - CPU ROM is always 32 KiB
// - Control (prog) ROM is 256 KiB (SC-55/SCC-1) or 512 KiB (SC-55mkII)
// - Wave ROMs are always a multiple of 1 MiB and are concatenated in
//   filename order (wave1, wave2, wave3, ...)
constexpr FSIZE_t CPURROMSize      = 32 * 1024;
constexpr FSIZE_t ControlROMSize1  = 256 * 1024;
constexpr FSIZE_t ControlROMSize2  = 512 * 1024;
constexpr FSIZE_t WaveROMGranularity = 1024 * 1024;
constexpr size_t MaxWaveROMs       = 4;
constexpr size_t MaxROMFiles       = 16;

namespace
{
bool ROMPathComparator(const CString& LHS, const CString& RHS)
{
	return strcasecmp(static_cast<const char*>(LHS), static_cast<const char*>(RHS)) < 0;
}
}

CEmuSCSynth::CEmuSCSynth(unsigned nSampleRate, float nGain, TSoundMap SoundMap)
	: CSynthBase(nSampleRate),
	  m_pControlROM(nullptr),
	  m_pWaveROM(nullptr),
	  m_pSynth(nullptr),
	  m_nGain(nGain),
	  m_SoundMap(SoundMap),
	  m_nVolume(100),
	  m_ModelName{},
	  m_ROMVersion{}
{
}

CEmuSCSynth::~CEmuSCSynth()
{
	if (m_pSynth)
		delete m_pSynth;

	if (m_pWaveROM)
		delete m_pWaveROM;

	if (m_pControlROM)
		delete m_pControlROM;
}

bool CEmuSCSynth::ScanROMs(TROMPaths& OutPaths)
{
	CString ControlCandidates[MaxROMFiles];
	CString CPUCandidates[MaxROMFiles];
	CString WaveCandidates[MaxROMFiles];
	size_t nControlCount = 0;
	size_t nCPUCount     = 0;
	size_t nWaveCount    = 0;

	// Loop over each disk; the first disk with a complete set wins
	for (auto pDisk : EmuSCDisks)
	{
		nControlCount = nCPUCount = nWaveCount = 0;

		CString DirectoryPath;
		DirectoryPath.Format("%s:/%s", pDisk, EmuSCROMDirectory);

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
				if (nSize == CPURROMSize && nCPUCount < MaxROMFiles)
					CPUCandidates[nCPUCount++] = ROMPath;
				else if ((nSize == ControlROMSize1 || nSize == ControlROMSize2) && nControlCount < MaxROMFiles)
					ControlCandidates[nControlCount++] = ROMPath;
				else if (nSize > 0 && (nSize % WaveROMGranularity) == 0 && nWaveCount < MaxROMFiles)
					WaveCandidates[nWaveCount++] = ROMPath;
			}

			Result = f_findnext(&Dir, &FileInfo);
		}

		f_closedir(&Dir);

		if (nControlCount > 0 && nCPUCount > 0 && nWaveCount > 0)
			break;
	}

	if (nControlCount == 0 || nCPUCount == 0 || nWaveCount == 0)
	{
		LOGWARN("Incomplete SC-55 ROM set in %s (control: %u, cpu: %u, wave: %u)",
		        EmuSCROMDirectory, static_cast<unsigned>(nControlCount), static_cast<unsigned>(nCPUCount), static_cast<unsigned>(nWaveCount));
		return false;
	}

	// Deterministic order: wave ROMs must be supplied to libEmuSC in
	// filename order so that their data is concatenated correctly
	if (nControlCount > 1)
		Utility::QSort(ControlCandidates, ROMPathComparator, 0, nControlCount - 1);
	if (nCPUCount > 1)
		Utility::QSort(CPUCandidates, ROMPathComparator, 0, nCPUCount - 1);
	if (nWaveCount > 1)
		Utility::QSort(WaveCandidates, ROMPathComparator, 0, nWaveCount - 1);

	OutPaths.ControlROMPath = ControlCandidates[0];
	OutPaths.CPUROMPath     = CPUCandidates[0];
	OutPaths.nWaveROMCount  = Utility::Min(nWaveCount, MaxWaveROMs);
	for (size_t i = 0; i < OutPaths.nWaveROMCount; ++i)
		OutPaths.WaveROMPaths[i] = WaveCandidates[i];
	OutPaths.bValid = true;

	LOGNOTE("SC-55 ROM set: control=%s cpu=%s wave=%u file(s)",
	        static_cast<const char*>(OutPaths.ControlROMPath),
	        static_cast<const char*>(OutPaths.CPUROMPath),
	        static_cast<unsigned>(OutPaths.nWaveROMCount));

	return true;
}

static EmuSC::Synth::SoundMap MapSoundMap(CEmuSCSynth::TSoundMap SoundMap)
{
	switch (SoundMap)
	{
		case CEmuSCSynth::TSoundMap::GSGM:
			return EmuSC::Synth::SoundMap::GS_GM;

		case CEmuSCSynth::TSoundMap::MT32:
			return EmuSC::Synth::SoundMap::MT32;

		case CEmuSCSynth::TSoundMap::GS:
		default:
			return EmuSC::Synth::SoundMap::GS;
	}
}

bool CEmuSCSynth::Initialize()
{
	TROMPaths ROMPaths;
	if (!ScanROMs(ROMPaths))
		return false;

	std::vector<std::string> WavePaths;
	for (size_t i = 0; i < ROMPaths.nWaveROMCount; ++i)
		WavePaths.push_back(static_cast<const char*>(ROMPaths.WaveROMPaths[i]));

	// libEmuSC reports ROM problems by throwing std::string, but other
	// failures (std::bad_alloc and friends) must also degrade to "synth
	// unavailable" here — an uncaught exception would terminate the kernel
	auto FailInit = [this](const char* pMessage)
	{
		LOGERR("libEmuSC failed to load ROMs: %s", pMessage);

		if (m_pSynth)
		{
			delete m_pSynth;
			m_pSynth = nullptr;
		}
		if (m_pWaveROM)
		{
			delete m_pWaveROM;
			m_pWaveROM = nullptr;
		}
		if (m_pControlROM)
		{
			delete m_pControlROM;
			m_pControlROM = nullptr;
		}

		return false;
	};

	try
	{
		m_pControlROM = new EmuSC::ControlRom(static_cast<const char*>(ROMPaths.ControlROMPath),
		                                       static_cast<const char*>(ROMPaths.CPUROMPath));
		m_pWaveROM    = new EmuSC::WaveRom(WavePaths, *m_pControlROM);
		m_pSynth      = new EmuSC::Synth(*m_pControlROM, *m_pWaveROM, MapSoundMap(m_SoundMap));
	}
	catch (const std::string& Error)
	{
		return FailInit(Error.c_str());
	}
	catch (const std::exception& Error)
	{
		return FailInit(Error.what());
	}
	catch (...)
	{
		return FailInit("unknown error");
	}

	m_pSynth->set_audio_format(m_nSampleRate, 2);

	strncpy(m_ModelName, m_pControlROM->model().c_str(), sizeof(m_ModelName) - 1);
	strncpy(m_ROMVersion, m_pControlROM->version().c_str(), sizeof(m_ROMVersion) - 1);

	LOGNOTE("libEmuSC %s initialized: %s control ROM v%s, wave ROM v%s",
	        EmuSC::Synth::version().c_str(), m_ModelName, m_ROMVersion, m_pWaveROM->version().c_str());

	// Apply any master volume set before initialization completed
	SetMasterVolume(m_nVolume);

	return true;
}

void CEmuSCSynth::HandleMIDIShortMessage(u32 nMessage)
{
	const u8 nStatus = nMessage & 0xFF;
	const u8 nData1  = (nMessage >> 8) & 0xFF;
	const u8 nData2  = (nMessage >> 16) & 0xFF;

	if (!m_pSynth)
		return;

	// Handle system reset (0xFF) like the other synths
	if (nStatus == 0xFF)
	{
		m_Lock.Acquire();
		m_pSynth->reset(MapSoundMap(m_SoundMap));
		m_Lock.Release();
		return;
	}

	// libEmuSC handles all channel messages (including handling of
	// single-data-byte messages) and updates its own GS state. Take our
	// own lock as well (like the other synth engines) so that MIDI input
	// is serialized against Render() on the audio core.
	m_Lock.Acquire();
	m_pSynth->midi_input(nStatus, nData1, nData2);
	m_Lock.Release();

	// Update MIDI monitor
	CSynthBase::HandleMIDIShortMessage(nMessage);
}

void CEmuSCSynth::HandleMIDISysExMessage(const u8* pData, size_t nSize)
{
	if (!m_pSynth || nSize == 0 || nSize > 0xFFFF)
		return;

	// libEmuSC expects the complete message including the leading 0xF0
	// and trailing 0xF7, in a mutable buffer; non-Roland messages are
	// ignored by the library itself. GM/GS reset messages therefore
	// reach the synth exactly as they would real hardware.
	u8 Buffer[1000];
	if (nSize > sizeof(Buffer))
		return;

	memcpy(Buffer, pData, nSize);

	m_Lock.Acquire();
	m_pSynth->midi_input_sysex(Buffer, static_cast<uint16_t>(nSize));
	m_Lock.Release();
}

bool CEmuSCSynth::IsActive()
{
	if (!m_pSynth)
		return false;

	// libEmuSC has no voice-count API; use its per-part peak levels
	m_Lock.Acquire();
	const auto Peaks = m_pSynth->get_parts_last_peak_sample();
	m_Lock.Release();
	for (int nPeak : Peaks)
	{
		if (nPeak != 0)
			return true;
	}

	return false;
}

void CEmuSCSynth::AllSoundOff()
{
	if (m_pSynth)
	{
		m_Lock.Acquire();
		m_pSynth->panic();
		m_Lock.Release();
	}

	// Reset MIDI monitor
	CSynthBase::AllSoundOff();
}

void CEmuSCSynth::SetMasterVolume(u8 nVolume)
{
	m_nVolume = nVolume;

	if (m_pSynth)
	{
		m_Lock.Acquire();
		m_pSynth->set_param(EmuSC::SystemParam::Volume, static_cast<uint8_t>(m_nVolume * 127 / 100));
		m_Lock.Release();
	}
}

size_t CEmuSCSynth::Render(float* pOutBuffer, size_t nFrames)
{
	if (!m_pSynth)
	{
		memset(pOutBuffer, 0, nFrames * 2 * sizeof(float));
		return nFrames;
	}

	m_Lock.Acquire();
	for (size_t i = 0; i < nFrames; ++i)
	{
		float nLeft, nRight;
		m_pSynth->get_next_frame(nLeft, nRight);
		pOutBuffer[i * 2]     = nLeft * m_nGain;
		pOutBuffer[i * 2 + 1] = nRight * m_nGain;
	}
	m_Lock.Release();

	return nFrames;
}

size_t CEmuSCSynth::Render(s16* pOutBuffer, size_t nFrames)
{
	if (!m_pSynth)
	{
		memset(pOutBuffer, 0, nFrames * 2 * sizeof(s16));
		return nFrames;
	}

	m_Lock.Acquire();
	for (size_t i = 0; i < nFrames; ++i)
	{
		float nLeft, nRight;
		m_pSynth->get_next_frame(nLeft, nRight);
		pOutBuffer[i * 2]     = static_cast<s16>(Utility::Clamp(nLeft * m_nGain, -1.0f, 1.0f) * 32767.0f);
		pOutBuffer[i * 2 + 1] = static_cast<s16>(Utility::Clamp(nRight * m_nGain, -1.0f, 1.0f) * 32767.0f);
	}
	m_Lock.Release();

	return nFrames;
}

void CEmuSCSynth::ReportStatus() const
{
	if (m_pUI)
		m_pUI->ShowSystemMessage(m_ModelName[0] ? m_ModelName : "Sound Canvas");
}

void CEmuSCSynth::UpdateLCD(CLCD& LCD, unsigned int nTicks)
{
	const u8 nBarHeight = LCD.Height();
	float ChannelLevels[16], PeakLevels[16];
	m_MIDIMonitor.GetChannelLevels(nTicks, ChannelLevels, PeakLevels, 1 << 9);
	CUserInterface::DrawChannelLevels(LCD, nBarHeight, ChannelLevels, PeakLevels, 16, true);
}
