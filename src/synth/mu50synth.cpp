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
void MU50H8SwpWrite(void* pCtx, mu50h8::u64, mu50h8::u16 nAddr, mu50h8::u8 nData)
{
	static_cast<CMU50Synth*>(pCtx)->OnH8SwpWrite(nAddr, nData);
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
	  m_nGain(nGain),
	  m_nVolume(100),
	  m_dResampleRatio(44100.0 / static_cast<double>(nSampleRate)),
	  m_dResampleCarry(0.0),
	  m_bResamplePrimed(false),
	  m_PrevSample{},
	  m_LeftoverSample{},
	  m_NativeScratch{},
	  m_FloatScratch{}
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

void CMU50Synth::OnH8SwpWrite(u16 nAddr, u8 nData)
{
	// Called from AdvanceH8 under m_Lock. Firmware writes that the
	// register core rejects are ignored (should not happen once the
	// wave ROM is loaded).
	if (m_pSynth)
	{
		m_pSynth->write_register(nAddr, nData);
		++m_nDiagSwpWrites;
	}
}

u8 CMU50Synth::OnH8SwpRead(u16 nAddr)
{
	// A failed read returns 0, matching the M2 harness stub behavior.
	u8 nValue = 0;
	if (m_pSynth)
		m_pSynth->read_register(nAddr, nValue);
	return nValue;
}

void CMU50Synth::OnH8Sci1RdrRead()
{
	// Called from the H8's sci_read under m_Lock (inside AdvanceH8
	// during Render). Counts SCI1 RDR reads, normally one per MIDI
	// byte the firmware consumes.
	++m_nDiagSciRead;
}

void CMU50Synth::AdvanceH8(size_t nNativeFrames)
{
	if (!m_pH8 || m_bH8Halted || nNativeFrames == 0)
		return;

	m_dH8CycleCarry += static_cast<double>(nNativeFrames) * H8CyclesPerNativeFrame;
	const u64 nAdvance = static_cast<u64>(m_dH8CycleCarry);
	m_dH8CycleCarry -= static_cast<double>(nAdvance);
	if (nAdvance == 0)
		return;

	const bool bDiag      = CDiagLog::Get().IsEnabled();
	const u64 nStartUs    = bDiag ? CTimer::Get()->GetClockTicks() : 0;
	const u64 nStartInsns = m_pH8->instr_count();
	if (!m_pH8->run_until(m_pH8->cycles() + nAdvance))
	{
		// Sticky: a halted CPU stays halted until a system reset
		// (0xFF) revives it. Should not happen: M2 ran 23.7M
		// instructions through boot with no halt.
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
		m_nHeldNotes      = 0;
		m_bSustain        = false;
		m_bSustainedNotes = false;
		m_Lock.Release();
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
		// CC 120 (All Sound Off) and CC 123 (All Notes Off) on every
		// channel; the firmware handles them like real hardware. The
		// H8 and register core are NOT reset here: the firmware owns
		// the voice state.
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

	// Sounding-note tracking (see IsActive): an All Sound Off
	// clears every held/sustained note.
	m_nHeldNotes      = 0;
	m_bSustain        = false;
	m_bSustainedNotes = false;

	// Reset MIDI monitor
	CSynthBase::AllSoundOff();
}

void CMU50Synth::SetMasterVolume(u8 nVolume)
{
	m_nVolume = nVolume;
	// libmu50 has no volume API: volume is applied as a scale factor
	// in RenderResampled(), together with the per-engine gain.
}

void CMU50Synth::RenderResampled(float* pOutBuffer, size_t nFrames)
{
	// Caller must hold m_Lock. Cubic Catmull-Rom resampler (same
	// 4-tap weights as Nuked's cubic): native 44.1 kHz frames are
	// rendered block by block and interpolated to the output rate.
	// Three native frames carry across blocks: the frame before the
	// boundary (the cubic's prev tap) plus the pair straddling it.
	const double dRatio = m_dResampleRatio;
	const float fScale  = m_nGain * (static_cast<float>(m_nVolume) / 100.0f);

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
		m_dResampleCarry  = dEndPos - static_cast<double>(nConsumed);
		m_bResamplePrimed = true;
		nOutDone += nBlockOut;
	}
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
	// is released, so it never nests under m_Lock. MU50 renders
	// synchronously (no FIFO/ring), so underrun_frames is
	// structurally 0; halted sticks at 1 if the H8 ever stops on an
	// illegal opcode (cleared only by a system reset).
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
			DiagLog.LogLine("window synth=mu50 pct=%llu avg_us=%llu max_us=%llu h8_us=%llu swp_us=%llu h8_insn=%llu underrun_frames=0 halted=%u",
			                static_cast<unsigned long long>(nPct),
			                static_cast<unsigned long long>(nAvgUs),
			                static_cast<unsigned long long>(nMaxUs),
			                static_cast<unsigned long long>(nH8Us),
			                static_cast<unsigned long long>(nSwpUs),
			                static_cast<unsigned long long>(nH8Insns),
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
		}
	}
}
