//
// synthbase.h
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

#ifndef _synthbase_h
#define _synthbase_h

#include <circle/spinlock.h>
#include <circle/types.h>

#include "lcd/lcd.h"
#include "lcd/ui.h"
#include "midimonitor.h"

class CSynthBase
{
public:
	CSynthBase(unsigned int nSampleRate)
		: m_Lock(TASK_LEVEL),
		  m_nSampleRate(nSampleRate),
		  m_pUI(nullptr)
	{
	}

	virtual ~CSynthBase() = default;

	virtual bool Initialize() = 0;
	virtual void HandleMIDIShortMessage(u32 nMessage) { m_MIDIMonitor.OnShortMessage(nMessage); };
	virtual void HandleMIDISysExMessage(const u8* pData, size_t nSize) = 0;
	virtual bool IsActive() = 0;
	virtual void AllSoundOff() { m_MIDIMonitor.AllNotesOff(); };
	virtual void SetMasterVolume(u8 nVolume) = 0;
	// Fade-in on synth switch: reset the applied volume to silence
	// so the next Render ramps up to the target instead of starting
	// at full volume mid-signal. Default no-op; Nuked and MU50
	// override it (they apply master volume as an output scale).
	// Called by CMT32Pi::SwitchSynth before the new synth becomes
	// current, so the audio thread is not rendering it yet.
	virtual void ResetVolumeRamp() {}
	virtual size_t Render(s16* pOutBuffer, size_t nFrames) = 0;
	virtual size_t Render(float* pOutBuffer, size_t nFrames) = 0;
	// Prefill: step the emulator toward a target output cushion
	// while the DAC queue is full (no room to write), so idle time
	// builds a cushion instead of spinning on zero-frame Render
	// calls. Default is a no-op (synths without a prefillable
	// ring); Nuked overrides it. Returns the ring fill after
	// prefill (native frames), or 0 if not supported.
	// pfnShouldStop (optional) is polled during the prefill; when
	// it returns true the prefill stops early so the caller can
	// Render instead -- this keeps Prefill from holding the
	// audio thread while the DAC queue drains. pStopContext is
	// passed through to the callback.
	virtual size_t Prefill(size_t /* nTargetFill */, bool (* /* pfnShouldStop */)(void*) = nullptr, void* /* pStopContext */ = nullptr) { return 0; }
	virtual void ReportStatus() const = 0;
	virtual void UpdateLCD(CLCD& LCD, unsigned int nTicks) = 0;
	void SetUserInterface(CUserInterface* pUI) { m_pUI = pUI; }

	CSpinLock m_Lock;
	unsigned int m_nSampleRate;
	CMIDIMonitor m_MIDIMonitor;
	CUserInterface* m_pUI;
};

#endif
