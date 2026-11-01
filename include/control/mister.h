//
// mister.h
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

#ifndef _mister_h
#define _mister_h

#include <circle/i2cmaster.h>
#include <circle/types.h>

#include "event.h"
#include "misterstatus.h"
#include "synth/synth.h"

class CMisterControl
{
public:
	CMisterControl(CI2CMaster* pI2CMaster, TEventQueue& EventQueue);

	void Update(const TMisterStatus& SystemStatus);

	// Last synth reported by MiSTer (Unknown if none reported yet or
	// MiSTer stopped responding). Drives the Button1 cycle gate.
	TMisterSynth GetLastMisterSynth() const { return m_LastMisterStatus.Synth; }

	// SC-55 engine selected via HW1 (Button1), for the MiSTer
	// FluidSynth reinterpretation (see ApplyConfig). Called only
	// by Button1 switches to Nuked/EmuSC/SoundFont; MT32 switches
	// do not touch it, and MiSTer disconnect does not reset it.
	void SetHW1SC55Synth(TSynth Synth) { m_bHW1SC55Selected = true; m_HW1SC55Synth = Synth; }

private:
	bool WriteConfigToMister(const TMisterStatus& NewStatus);
	void ResetState();
	void ApplyConfig(const TMisterStatus& NewStatus, const TMisterStatus& SystemStatus);
	TSynth ResolveSC55Synth() const;
	void EnqueueDisplayImageEvent();
	void EnqueueAllSoundOffEvent();

	CI2CMaster* m_pI2CMaster;
	TEventQueue* m_pEventQueue;

	bool bMisterActive;
	TMisterStatus m_LastSystemStatus;
	TMisterStatus m_LastMisterStatus;

	// HW1 SC-55 selection (see SetHW1SC55Synth). False until the
	// first Button1 switch to Nuked/EmuSC/SoundFont; the synth
	// value is meaningless until then (ResolveSC55Synth falls
	// back to Nuked).
	bool m_bHW1SC55Selected;
	TSynth m_HW1SC55Synth;
};

#endif
