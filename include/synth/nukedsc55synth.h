//
// nukedsc55synth.h
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

#ifndef _nukedsc55synth_h
#define _nukedsc55synth_h

#include <circle/types.h>

#include "synth/synthbase.h"
#include "utility.h"

// The Nuked-SC55 backend classes are only forward-declared here, rather than
// including the backend headers: the backend requires C++20, while the
// mt32-pi kernel is built as C++14. Only nukedsc55synth.cpp includes the real
// headers, and it is compiled as C++20 (see Kernel.mk).
class Emulator;
struct RomsetInfo;

class CNukedSC55Synth : public CSynthBase
{
public:
	#define ENUM_NUKEDH8ENGINE(ENUM) \
		ENUM(Decoder2, decoder2)     \
		ENUM(Legacy, legacy)

	CONFIG_ENUM(TH8Engine, ENUM_NUKEDH8ENGINE);

	CNukedSC55Synth(unsigned nSampleRate, float nGain, bool bOversampling, TH8Engine H8Engine);
	virtual ~CNukedSC55Synth() override;

	// Cycle the H8 execution path (Button 2 while this synth is
	// active). Takes effect between instructions; returns the config
	// string name of the newly selected engine.
	const char* CycleH8Engine();
	static const char* H8EngineName(TH8Engine Engine);

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

	const char* GetModelName() const { return m_ModelName; }
	const char* GetROMVersion() const { return m_ROMVersion; }

private:
	// Native-rate frames produced by the emulator are pushed into this ring
	// buffer by the sample callback (which runs inside Render(), under
	// m_Lock, on the audio core) and consumed by the resampler.
	static constexpr size_t RingBufferFrames = 16384;
	static_assert((RingBufferFrames & (RingBufferFrames - 1)) == 0, "Ring buffer size must be a power of two");

	void OnSample(s32 nLeft, s32 nRight);
	size_t RenderFrames(float* pOutBuffer, size_t nFrames);

	Emulator* m_pEmulator;
	RomsetInfo* m_pRomsetInfo;

	float m_nGain;
	bool m_bOversampling;
	TH8Engine m_H8Engine;
	u8 m_nVolume;

	unsigned m_nNativeRate;
	double m_dResampleRatio;	// Native frames consumed per output frame
	double m_dReadPosition;		// Fractional native-frame read position
	u64 m_nWrittenFrames;		// Total native frames produced (callback)
	u64 m_nLastActiveFrame;		// Native frame index of the last above-threshold sample

	// Render timing statistics for the diagnostic log; accumulated in
	// RenderFrames() (audio context, brief stats lock) and flushed
	// periodically from UpdateLCD() when [diagnostics] is enabled.
	struct TRenderStats
	{
		u64 nCalls          = 0;
		u64 nFrames         = 0;
		u64 nTotalUs        = 0;
		u64 nMaxUs          = 0;
		u64 nMisses         = 0;
		u64 nUnderrunFrames = 0;
		u64 nWindowStartUs  = 0;
	};
	TRenderStats m_Stats;
	CSpinLock m_StatsLock;

	// Backend profiling snapshots (see nuked_profile.h in the backend):
	// cumulative counters are read and diffed at each diag flush.
	u64 m_nLastProfileTotalUs = 0;
	u64 m_nLastProfilePCMUs   = 0;
	u64 m_nLastProfileInsn    = 0;

	float m_RingBuffer[RingBufferFrames][2];

	char m_ModelName[16];
	char m_ROMVersion[16];
};

#endif
