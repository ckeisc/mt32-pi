//
// emuscsynth.h
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

#ifndef _emuscsynth_h
#define _emuscsynth_h

#include <circle/types.h>

#include "synth/synthbase.h"
#include "utility.h"

// The libEmuSC classes are only forward-declared here, rather than including
// the libEmuSC headers: libEmuSC requires C++17, while the mt32-pi kernel is
// built as C++14. Only emuscsynth.cpp includes the real headers, and it is
// compiled as C++17 (see Kernel.mk).
namespace EmuSC
{
	class ControlRom;
	class Synth;
	class WaveRom;
}

class CEmuSCSynth : public CSynthBase
{
public:
	#define ENUM_EMUSCSOUNDMAP(ENUM) \
		ENUM(GS, gs)                 \
		ENUM(GSGM, gs_gm)            \
		ENUM(MT32, mt32)

	CONFIG_ENUM(TSoundMap, ENUM_EMUSCSOUNDMAP);

	CEmuSCSynth(unsigned nSampleRate, float nGain, TSoundMap SoundMap);
	virtual ~CEmuSCSynth() override;

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
	struct TROMPaths
	{
		CString ControlROMPath;
		CString CPUROMPath;
		CString WaveROMPaths[4];
		size_t nWaveROMCount = 0;
		bool bValid          = false;
	};

	bool ScanROMs(TROMPaths& OutPaths);

	EmuSC::ControlRom* m_pControlROM;
	EmuSC::WaveRom* m_pWaveROM;
	EmuSC::Synth* m_pSynth;

	float m_nGain;
	TSoundMap m_SoundMap;
	u8 m_nVolume;

	char m_ModelName[16];
	char m_ROMVersion[16];
};

#endif
