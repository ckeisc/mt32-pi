# mt32-pi — EmuSC fork

This is a fork of **[dwhinham/mt32-pi](https://github.com/dwhinham/mt32-pi)**, based on upstream **v0.13.1**. Full documentation lives in the [upstream README](https://github.com/dwhinham/mt32-pi/blob/master/README.md)

## How this fork differs

It adds a **third synth engine — [EmuSC](https://github.com/skjelten/emusc)** (libEmuSC 0.7.0) — for Roland Sound Canvas **SC-55** emulation, alongside upstream's Munt (MT-32) and FluidSynth (SoundFont):

- Select EmuSC via `default_synth = emusc`, Button 1 cycling, or SysEx `F0 7D 03 02 F7`; new `[emusc]` section in `mt32-pi.cfg` (`gain`, `sound_map`)
- SC-55 ROM dumps (not included) are loaded from `roms/sc55/` and classified by file size; the SC-55 **v1.21** set is the verified one
- Proposed MiSTer status value `0xA3` for EmuSC (upstream cores currently only know `0xA1`/`0xA2`)
- Bare-metal fixes found via QEMU boot-testing: Circle patches to libEmuSC in `patches/emusc-0.7.0-circle.patch` (notably, `exit(0)` on unsupported ROMs becomes a thrown exception instead of a silent kernel halt) and the zone allocator's malloc reserve is raised from 32 MB to 96 MB for libEmuSC's decoded sample cache.
