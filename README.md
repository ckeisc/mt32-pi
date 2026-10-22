# mt32-pi — EmuSC fork

This is a fork of **[dwhinham/mt32-pi](https://github.com/dwhinham/mt32-pi)**, based on upstream **v0.13.1**. Full documentation lives in the [upstream README](https://github.com/dwhinham/mt32-pi/blob/master/README.md)

## How this fork differs

It adds **[EmuSC](https://github.com/skjelten/emusc)** (libEmuSC 0.7.0) for Roland Sound Canvas **SC-55** emulation — and EmuSC **replaces FluidSynth**, occupying the SoundFont slot, so it works with unmodified MiSTer cores and MiSTer Main:

- MiSTer sees two synths as before (Munt and "FluidSynth"), but the second slot is EmuSC: selecting FluidSynth in a core OSD or Main's MT32-pi menu (status `0xA2`) runs SC-55 emulation, and the Pi reports EmuSC back as `0xA2`. No SoundFonts are loaded and `[fluidsynth]` settings are ignored
- Also selectable via `default_synth = soundfont` or `emusc`, Button 1 cycling, or SysEx `F0 7D 03 02 F7`; `[emusc]` section in `mt32-pi.cfg` (`gain`, `sound_map`)
- SC-55 ROM dumps (not included) are loaded from `roms/sc55/` and classified by file size; the SC-55 **v1.21** set is the verified one
- Bare-metal fixes found via QEMU boot-testing: Circle patches to libEmuSC in `patches/emusc-0.7.0-circle.patch` (notably, `exit(0)` on unsupported ROMs becomes a thrown exception instead of a silent kernel halt) and the zone allocator's malloc reserve is raised from 32 MB to 96 MB for libEmuSC's decoded sample cache.
