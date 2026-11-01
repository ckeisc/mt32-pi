# mt32-pi — Nuked SC-55 + EmuSC fork

This is a fork of **[dwhinham/mt32-pi](https://github.com/dwhinham/mt32-pi)**, based on upstream **v0.13.1**. Full documentation lives in the [upstream README](https://github.com/dwhinham/mt32-pi/blob/master/README.md)

## How this fork differs

It adds two synth engines for Roland Sound Canvas **SC-55** emulation, alongside upstream's Munt (MT-32) and FluidSynth (SoundFont): **[EmuSC](https://github.com/skjelten/emusc)** (libEmuSC 0.7.0) and **[Nuked-SC55](https://github.com/jcmoyer/Nuked-SC55)** (firmware-level emulation):

- Nuked SC-55 engine has been optimized to use two cores and can run ok on MT-32pi Lite (Pi Zero 2 W)
- From Mister side, you can still only toggle between Munt and Fluidsynth. When Fluidsynth is selected, you can press button 1 on MT-32pi to switch to Nuked SC-55 engine or EmuSC engine.
- Nuked SC-55 engine only tested on 1.21 rom set
