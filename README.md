# mt32-pi — Nuked SC-55 + EmuSC + MU50 fork

This is a fork of **[dwhinham/mt32-pi](https://github.com/dwhinham/mt32-pi)**, now based on **[pim-32](https://github.com/macroexp/mt32-pi)** v0.17.0, the community-maintained continuation of mt32-pi (this fork was originally based on upstream v0.13.1). Full documentation lives in the [upstream README](https://github.com/dwhinham/mt32-pi/blob/master/README.md)

## How this fork differs

It adds three more synth engines for General MIDI playback, alongside upstream's Munt (MT-32) and FluidSynth (SoundFont): **[EmuSC](https://github.com/skjelten/emusc)** (libEmuSC 0.7.0), **[Nuked-SC55](https://github.com/jcmoyer/Nuked-SC55)** (firmware-level emulation) and Yamaha MU50:

- Nuked SC-55 engine has been optimized to use two cores and can run on the MT-32pi Lite (Pi Zero 2 W). BUT, sustained playback requires active cooling (fan). An MT-32pi Lite without additional cooling would likely encounter heat throttling after 10 minutes of continuous playback. 
- From Mister side, you can still only toggle between Munt and Fluidsynth. When Fluidsynth is selected, you can press button 1 on MT-32pi to switch to Nuked SC-55 engine, EmuSC engine, the original sound font-based engine or the experimental MU50 engine. Press button 1 once to show the current engine and additional press to cycle through all engines.
- Nuked SC-55 engine only tested on 1.21 rom set.
- Yamaha MU50 emulation based on MAME 0.277 code.

## Setup 

- Copy SC-55 rom set into `roms/sc55` folder. Only v1.21 rom set is supported. It should contain 5 files: `rom1`, `rom2`, `waverom1`, `waverom2` and `waverom3`. File name does not matter as the match is based on file hash. Both Nuked SC-55 and EmuSC engines require the rom files to work.
- Copy Yamaha MU50 rom set into `roms/yamaha` folder. File name does matter for mu50 emulation. They need to be `xq057c0.ic18`, `xq058c0.ic19`, `xr174c0.ic7`, `yamaha_mu50.bin` from MAME romset.
- Set "Show Info" to "LCD-Auto (non-FB)" on the core MT32-pi menu to see engine selection notifications.
