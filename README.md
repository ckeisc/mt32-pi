# mt32-pi — Nuked SC-55 + EmuSC + MU50 fork

This is a fork of **[dwhinham/mt32-pi](https://github.com/dwhinham/mt32-pi)**, now based on **[pim-32](https://github.com/macroexp/mt32-pi)** v0.17.0, the community-maintained continuation of mt32-pi (this fork was originally based on upstream v0.13.1). Full documentation lives in the [upstream README](https://github.com/dwhinham/mt32-pi/blob/master/README.md)

## How this fork differs

It adds three more synth engines for General MIDI playback, alongside upstream's Munt (MT-32) and FluidSynth (SoundFont): **[EmuSC](https://github.com/skjelten/emusc)** (libEmuSC 0.7.0), **[Nuked-SC55](https://github.com/jcmoyer/Nuked-SC55)** (firmware-level emulation) and Yamaha MU50:

- Nuked SC-55 engine has been optimized to use two cores and can run ok on MT-32pi Lite (Pi Zero 2 W)
- From Mister side, you can still only toggle between Munt and Fluidsynth. When Fluidsynth is selected, you can press button 1 on MT-32pi to switch to Nuked SC-55 engine, EmuSC engine, or MU50 engine.
- Nuked SC-55 engine only tested on 1.21 rom set
- Yamaha MU50 emulation based on MAME code
