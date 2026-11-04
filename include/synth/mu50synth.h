//
// mu50synth.h
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

#ifndef _mu50synth_h
#define _mu50synth_h

#include <circle/types.h>

#include <vector>

#include "synth/synthbase.h"
#include "utility.h"

// The libmu50 class is only forward-declared here, rather than including
// the libmu50 header: libmu50 requires C++17, while the mt32-pi kernel is
// built as C++14. Only mu50synth.cpp includes the real header, and it is
// compiled as C++17 (see Kernel.mk). This mirrors the EmuSC port's pattern.
namespace libmu50
{
	class Synth;
}

// Same pattern for the H8 CPU port: only mu50synth.cpp includes the
// real header (also C++17, see Kernel.mk).
namespace mu50h8
{
	class Mu50H8;
}

class CMU50Synth : public CSynthBase
{
public:
	CMU50Synth(unsigned nSampleRate, float nGain);
	virtual ~CMU50Synth() override;

	// CSynthBase
	virtual bool Initialize() override;
	virtual void HandleMIDIShortMessage(u32 nMessage) override;
	virtual void HandleMIDISysExMessage(const u8* pData, size_t nSize) override;
	virtual bool IsActive() override;
	virtual void AllSoundOff() override;
	virtual void SetMasterVolume(u8 nVolume) override;
	virtual size_t Render(s16* pOutBuffer, size_t nFrames) override;
	virtual size_t Render(float* pOutBuffer, size_t nFrames) override;
	virtual void ReportStatus() const override;
	virtual void UpdateLCD(CLCD& LCD, unsigned int nTicks) override;

	// H8 hook targets, called from the file-local hook functions in
	// mu50synth.cpp (under m_Lock from Render). Public only because
	// the file-local hooks cannot call private methods.
	void OnH8SwpWrite(u16 nAddr, u8 nData);
	u8 OnH8SwpRead(u16 nAddr);
	// SCI1 RDR read by the firmware (see OnH8Sci1RdrRead).
	void OnH8Sci1RdrRead();

private:
	struct TROMPaths
	{
		CString WaveROMPaths[2];
		size_t nWaveROMCount = 0;
		CString FirmwarePaths[8];
		size_t nFirmwareCount = 0;
		bool bValid           = false;
	};

	bool ScanROMs(TROMPaths& OutPaths);
	bool ReadFile(const char* pPath, u8* pBuffer, size_t nSize);

	// Renders libmu50 native-rate (44.1 kHz) frames into pOutBuffer,
	// resampling to the engine's output rate. Caller must hold m_Lock:
	// this advances the resampler position and uses the scratch buffers.
	void RenderResampled(float* pOutBuffer, size_t nFrames);

	// Advance the H8 by the emulated cycles for nNativeFrames of
	// audio (10 MHz / 44.1 kHz). Caller must hold m_Lock.
	void AdvanceH8(size_t nNativeFrames);

	// Queue one short message's bytes into SCI1 RX (Nuked byte-length
	// rules: 2 bytes for 0xC0/0xD0 channel messages, 3 otherwise;
	// 2 for 0xF1/0xF3, 3 for 0xF2, 1 for other system messages).
	// Caller must hold m_Lock.
	void QueueMIDIMessageBytes(u8 nStatus, u8 nData1, u8 nData2);

	libmu50::Synth* m_pSynth;

	// The H8 CPU port running the real MU50 firmware. It holds a
	// pointer to m_FirmwareROM, which is therefore kept alive for
	// the H8's lifetime and never modified after creation.
	mu50h8::Mu50H8* m_pH8;
	std::vector<u8> m_FirmwareROM;

	// H8 advance state (caller must hold m_Lock)
	double m_dH8CycleCarry; // fractional emulated cycles into the next block
	bool m_bH8Halted;       // sticky: H8 stopped on an illegal opcode

	// Activity tracking for IsActive(), mirroring the Nuked pattern:
	// audible output within the last 0.5 s of native frames, or a
	// held/sustained note, keeps the clock up.
	u64 m_nWrittenNativeFrames;
	u64 m_nLastActiveNativeFrame;
	unsigned m_nHeldNotes;
	bool m_bSustain;
	bool m_bSustainedNotes;

	// Diag window stats: accumulated under m_Lock in Render, flushed
	// in UpdateLCD outside the lock (mirrors the Nuked window line).
	u64 m_nDiagWindowStartUs;
	u64 m_nDiagCalls;
	u64 m_nDiagTotalUs;
	u64 m_nDiagMaxUs;
	u64 m_nDiagH8Us;
	u64 m_nDiagSwpUs;
	u64 m_nDiagH8Insns;
	u64 m_nDiagOutputFrames;
	// Per-window MIDI/SWP diagnostic counters, flushed in UpdateLCD
	// alongside the timing stats (see the second diag line): bytes
	// accepted into the SCI1 queue, SCI1 RDR reads by the firmware,
	// SWP00 register writes, and the peak native SWP sample before
	// resampling (proves non-zero SWP output independent of gain).
	u64 m_nDiagMidiQueued;
	u64 m_nDiagSciRead;
	u64 m_nDiagSwpWrites;
	int m_nDiagNativePeak;

	float m_nGain;
	u8 m_nVolume;

	// Cubic Catmull-Rom resampler state (same 4-tap weights as
	// Nuked's cubic resampler; implemented locally so Nuked's files
	// are untouched).
	double m_dResampleRatio; // native frames per output frame
	double m_dResampleCarry; // fractional native position into the next block
	bool m_bResamplePrimed;  // false until the first block has rendered
	// Three native stereo frames carried across blocks: the frame
	// before the boundary (cubic's prev tap) plus the pair
	// straddling it. Without the prev frame, the first output of
	// each block would lose its fourth tap.
	static constexpr size_t kNativeScratchFrames = 1024;
	s16 m_PrevSample[2];
	s16 m_LeftoverSample[2][2];
	s16 m_NativeScratch[2 * kNativeScratchFrames];
	float m_FloatScratch[1024];
};

#endif
