//
// mister.cpp
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

#include <circle/logger.h>

#include "control/mister.h"

LOGMODULE("mistercontrol");

constexpr u8 MisterI2CAddress = 0x45;

CMisterControl::CMisterControl(CI2CMaster* pI2CMaster, TEventQueue& EventQueue)
	: m_pI2CMaster(pI2CMaster),
	  m_pEventQueue(&EventQueue),

	  bMisterActive(false),
	  m_LastSystemStatus{TMisterSynth::Unknown, 0xFF, 0xFF},
	  m_LastMisterStatus{TMisterSynth::Unknown, 0xFF, 0xFF},
	  m_bHW1SC55Selected(false),
	  m_HW1SC55Synth(TSynth::NukedSC55)
{
}

void CMisterControl::Update(const TMisterStatus& SystemStatus)
{
	assert(m_pI2CMaster != nullptr);

	// Read current status from MiSTer
	TMisterStatus MisterStatus;
	if (m_pI2CMaster->Read(MisterI2CAddress, &MisterStatus, sizeof(MisterStatus)) < 0)
	{
		ResetState();
		return;
	}

	//LOGDBG("MiSTer Rx: 0x%02x 0x%02x 0x%02x", static_cast<u8>(MisterStatus.Synth), MisterStatus.MT32ROMSet, MisterStatus.SoundFontIndex);

	// Core was reset or "Reset Hanging Notes" was selected from OSD; turn off all sound
	if (MisterStatus.Synth == TMisterSynth::Mute)
	{
		LOGNOTE("Stopping synth activity");
		EnqueueAllSoundOffEvent();
		WriteConfigToMister(SystemStatus);
		return;
	}

	if (bMisterActive)
	{
		// If the state has been changed by user controls/SysEx, we just need to update the MiSTer status
		if (SystemStatus != m_LastSystemStatus)
		{
			// Write config back to MiSTer
			if (!WriteConfigToMister(SystemStatus))
			{
				ResetState();
				return;
			}

			m_LastSystemStatus = SystemStatus;
		}
		else if (MisterStatus != m_LastMisterStatus)
		{
			// The state has been changed by MiSTer; apply it
			ApplyConfig(MisterStatus, SystemStatus);

			// Write config back to MiSTer
			if (!WriteConfigToMister(MisterStatus))
			{
				ResetState();
				return;
			}

			m_LastMisterStatus = MisterStatus;
		}
	}
	else
	{
		// First valid reply from MiSTer; apply its config
		ApplyConfig(MisterStatus, SystemStatus);

		// Write config back to MiSTer
		if (!WriteConfigToMister(MisterStatus))
			return;

		// Show MiSTer logo
		EnqueueDisplayImageEvent();

		m_LastMisterStatus = MisterStatus;
		bMisterActive = true;
	}
}

TSynth CMisterControl::ResolveSC55Synth() const
{
	// MiSTer FluidSynth no longer means SoundFont literally: it
	// means the current SC-55 engine -- the engine last selected
	// via HW1 (Nuked/EmuSC/SoundFont), or Nuked if none has been
	// selected yet (runtime-only; reset on boot, untouched by
	// MiSTer disconnect).
	if (m_bHW1SC55Selected)
		return m_HW1SC55Synth;
	return TSynth::NukedSC55;
}

void CMisterControl::ApplyConfig(const TMisterStatus& NewStatus, const TMisterStatus& CurrentStatus)
{
	TEvent Event;

	// MiSTer synth mapping: MT32/EmuSC/Nuked are literal. MiSTer
	// FluidSynth (SoundFont) no longer means SoundFont literally:
	// MiSTer can only report Munt/FluidSynth, so FluidSynth means
	// the current SC-55 engine (HW1-selected, or Nuked by default).
	// Any other value (e.g. Unknown) keeps the old catch-all to
	// SoundFont. The event is enqueued only when the RESOLVED
	// target differs from the current synth, so a MiSTer report
	// that resolves to the synth already active is a silent
	// no-op (no "Already active!" notice).
	TSynth TargetSynth;
	if (NewStatus.Synth == TMisterSynth::MT32)
		TargetSynth = TSynth::MT32;
	else if (NewStatus.Synth == TMisterSynth::EmuSC)
		TargetSynth = TSynth::EmuSC;
	else if (NewStatus.Synth == TMisterSynth::NukedSC55)
		TargetSynth = TSynth::NukedSC55;
	else if (NewStatus.Synth == TMisterSynth::SoundFont)
		TargetSynth = ResolveSC55Synth();
	else
		TargetSynth = TSynth::SoundFont;
	TSynth CurrentSynth = TSynth::SoundFont;
	bool bCurrentKnown = true;
	if (CurrentStatus.Synth == TMisterSynth::MT32)
		CurrentSynth = TSynth::MT32;
	else if (CurrentStatus.Synth == TMisterSynth::EmuSC)
		CurrentSynth = TSynth::EmuSC;
	else if (CurrentStatus.Synth == TMisterSynth::NukedSC55)
		CurrentSynth = TSynth::NukedSC55;
	else if (CurrentStatus.Synth == TMisterSynth::SoundFont)
		CurrentSynth = TSynth::SoundFont;
	else
		bCurrentKnown = false;
	if (!bCurrentKnown || TargetSynth != CurrentSynth)
	{
		Event.Type = TEventType::SwitchSynth;
		Event.SwitchSynth.Synth = TargetSynth;
		m_pEventQueue->Enqueue(Event);
	}

	if (NewStatus.MT32ROMSet != CurrentStatus.MT32ROMSet)
	{
		Event.Type = TEventType::SwitchMT32ROMSet;
		Event.SwitchMT32ROMSet.ROMSet = static_cast<TMT32ROMSet>(NewStatus.MT32ROMSet);
		m_pEventQueue->Enqueue(Event);
	}

	if (NewStatus.SoundFontIndex != CurrentStatus.SoundFontIndex)
	{
		Event.Type = TEventType::SwitchSoundFont;
		Event.SwitchSoundFont.Index = NewStatus.SoundFontIndex;
		m_pEventQueue->Enqueue(Event);
	}
}

bool CMisterControl::WriteConfigToMister(const TMisterStatus& NewStatus)
{
	// Write config back to MiSTer
	if (m_pI2CMaster->Write(MisterI2CAddress, &NewStatus, sizeof(NewStatus)) < 0)
	{
		LOGERR("MiSTer write failed");
		return false;
	}

	//LOGDBG("MiSTer Tx: 0x%02x 0x%02x 0x%02x", static_cast<u8>(NewStatus.Synth), NewStatus.MT32ROMSet, NewStatus.SoundFontIndex);
	return true;
}

void CMisterControl::ResetState()
{
	if (bMisterActive)
	{
		// MiSTer has just stopped responding; dispatch an All Sound Off event
		LOGNOTE("MiSTer stopped responding; turning notes off");
		EnqueueAllSoundOffEvent();
		bMisterActive = false;
		m_LastSystemStatus = {TMisterSynth::Unknown, 0xFF, 0xFF};
		m_LastMisterStatus = {TMisterSynth::Unknown, 0xFF, 0xFF};
	}
}

void CMisterControl::EnqueueDisplayImageEvent()
{
	TEvent Event;
	Event.Type = TEventType::DisplayImage;
	Event.DisplayImage.Image = TImage::MisterLogo;
	m_pEventQueue->Enqueue(Event);
}

void CMisterControl::EnqueueAllSoundOffEvent()
{
	TEvent Event;
	Event.Type = TEventType::AllSoundOff;
	m_pEventQueue->Enqueue(Event);
}
