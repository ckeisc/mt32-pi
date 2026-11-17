//
// soundfontsynth.h
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

#ifndef _soundfontsynth_h
#define _soundfontsynth_h

#include <circle/types.h>

#include <fluidsynth.h>

#include "soundfontmanager.h"
#include "synth/fxprofile.h"
#include "synth/synthbase.h"

class CSoundFontSynth : public CSynthBase
{
public:
	CSoundFontSynth(unsigned nSampleRate);
	virtual ~CSoundFontSynth() override;

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

	bool SwitchSoundFont(size_t nIndex);
	size_t GetSoundFontIndex() const { return m_nCurrentSoundFontIndex; }
	CSoundFontManager& GetSoundFontManager() { return m_SoundFontManager; }

	// Background load at boot: Initialize() only resolves the
	// SoundFont and creates the FluidSynth settings; the ~3 s
	// .sf2 parse is done by LoadSoundFont() on Core 1 after boot
	// init, so it does not delay the other engines or the main
	// loop. Until the load finishes the synth renders silence
	// and drops MIDI; selecting it waits for the load (see
	// WaitForLoad). LoadSoundFont() is also used for a runtime
	// re-init (USB rescan), where it loads synchronously.
	// Returns true once the synth is loaded.
	bool LoadSoundFont();

	// Wait (bounded) for a load in progress to finish; returns
	// true if the synth is loaded afterwards. Core 0 only: yields
	// to the (core-0) scheduler while waiting. Returns
	// immediately if no load is pending or in progress.
	bool WaitForLoad(unsigned nTimeoutMillis = 30000);

private:
	bool Reinitialize(const char* pSoundFontPath, const TFXProfile* pFXProfile);

	// Sets m_bLoading around Reinitialize() and records the
	// result in m_bLoaded. Used by LoadSoundFont() and
	// SwitchSoundFont().
	bool ReloadSoundFont(const char* pSoundFontPath, const TFXProfile* pFXProfile);

	// True once the .sf2 is parsed and no load is in progress.
	// Callers hold m_Lock.
	bool IsLoadedLocked() const { return m_bLoaded && !m_bLoading; }

	void ResetMIDIMonitor();
#ifndef NDEBUG
	void DumpFXSettings() const;
#endif
	bool ParseGMSysEx(const u8* pData, size_t nSize);
	bool ParseRolandSysEx(const u8* pData, size_t nSize);
	bool ParseYamahaSysEx(const u8* pData, size_t nSize);

	fluid_settings_t* m_pSettings;
	fluid_synth_t* m_pSynth;

	u8 m_nVolume;
	float m_nInitialGain;

	u16 m_nPercussionMask;
	size_t m_nCurrentSoundFontIndex;

	CSoundFontManager m_SoundFontManager;

	// Background-load state. m_bLoadPending is set by
	// Initialize() when the .sf2 load is deferred; LoadSoundFont()
	// consumes it. m_bLoading is set around every Reinitialize()
	// (background load and font switch) so no other thread calls
	// into FluidSynth while a .sf2 is parsed -- the API is not
	// thread-safe (synth.threadsafe-api is false). All three are
	// only read or written under m_Lock, which also orders them
	// across cores.
	bool m_bLoaded;
	bool m_bLoading;
	bool m_bLoadPending;

	// Resolved at Initialize() time: the manager's list can be
	// rebuilt by a rescan, so the path is copied, not borrowed.
	CString m_PendingSoundFontPath;
	TFXProfile m_PendingFXProfile;

	static void FluidSynthLogCallback(int nLevel, const char* pMessage, void* pUser);
};

#endif
