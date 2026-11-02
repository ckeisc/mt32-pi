# Compiler A/B testing

Checks whether a toolchain change alters what Munt and FluidSynth render. `run.sh` builds `libmt32emu.a` and `libfluidsynth.a` with two Arm GNU Toolchain versions, using the same CMake options and flags as the pim-32 `Makefile` for `BOARD=pi3-64`. It links each pair into a small bare-metal renderer (`abrender.cpp`), runs both renderers under `qemu-system-aarch64` with semihosting for file access, and compares the output sample by sample.

The renderer runs the AArch64 code each toolchain generates for the libraries, so it shows codegen differences. It doesn't model the Pi's timing, caches or multiple cores.

## Needs

- Both toolchains unpacked side by side (as in `BUILDING.md`), e.g. `~/arm-gnu-toolchain-11.3.rel1-x86_64-aarch64-none-elf`
- `cmake`, `patch`, `qemu-system-aarch64` (Ubuntu: `qemu-system-arm`), `python3` with `numpy`
- `make submodules` done in this tree
- For Munt: an MT-32 control ROM and PCM ROM

## Usage

```sh
# FluidSynth: generated GM stress file with GeneralUser GS
scripts/dev/compiler-ab/run.sh --sf2 /path/to/GeneralUser-GS.sf2

# Munt: a game soundtrack with its SysEx setup
scripts/dev/compiler-ab/run.sh --control /roms/MT32_CONTROL.ROM --pcm /roms/MT32_PCM.ROM \
    --sysex /path/to/AMONRA.SYX --midi /path/to/LB2MT.MID
```

Options: `--old` / `--new` toolchain versions (default 11.3.rel1 and 15.2.rel1), `--toolchains DIR` where they are unpacked (default `$HOME`), `--opt FLAGS` to build both libraries with other optimisation flags (e.g. `-O2` as a reference), `--work DIR`. For Munt, `--until SECONDS` stops the render that far into the MIDI file (everything before it is still played, so long soundtracks can be cut to the passage of interest) and `--solo CHANNEL` keeps note-ons on one MIDI channel only, with SysEx and controllers intact. Builds are kept in the work directory and reused. To isolate FluidSynth's effects, run a renderer by hand with a fifth argument `nofx`, `noreverb` or `nochorus` (see the top of `abrender.cpp`).

Renders are 48 kHz stereo float32 with mt32-pi's default settings (Munt: gain 1.0, GOOD resampler; FluidSynth: FDN reverb and chorus as in `config.def`, polyphony 200, gain 0.2). Each event is sent at the sample position of its MIDI time. `cmp_f32.py` reads the renders a second at a time, so full-length soundtracks fit in memory, and prints the share of identical samples, the first difference, peak and RMS difference in dBFS, and the five seconds where the difference is largest relative to the signal.

## Reading the result

Libraries built with `-Ofast` let the compiler reorder floating-point arithmetic, so two toolchains seldom produce bit-identical output. Differences around −100 dBFS and below are rounding. To tell rounding apart from a real change, run again with `--opt -O2`: if the `-O2` builds from both toolchains agree, the `-Ofast` difference comes from floating-point reordering, and the `-O2` render shows which `-Ofast` build is closer to the exact result.

Phase 4.5 results (FluidSynth 2.6.1, GeneralUser GS 2.0.3, generated file, 51 s), GCC 11.3 vs 15.2:

| Build | Max difference | RMS difference |
| --- | --- | --- |
| `-Ofast`, reverb and chorus off | −125 dBFS | −147 dBFS |
| `-Ofast`, chorus only | −119 dBFS | −143 dBFS |
| `-Ofast`, FDN reverb on | −64 dBFS | −89 dBFS |
| `-O2`, everything on | identical | identical |
| 15.2 `-Ofast` vs `-O2` | −112 dBFS | −142 dBFS |
| 11.3 `-Ofast` vs `-O2` | −64 dBFS | −89 dBFS |

The reverb difference is GCC 11.3's `-Ofast` code drifting from the exact result; GCC 15.2's stays close to it.

Munt, Laura Bow 2 (`AMONRA.SYX` then `LB2MT.MID`, the whole 102-minute soundtrack), GCC 11.3 vs 15.2 with `-Ofast`, MT-32 v1.07 ROM: maximum difference −120 dBFS, RMS −156 dBFS; in every second the difference is at least 128 dB below the music. The first 455 s (through measure 188) with the MT-32 v2.04 and CM-32L v1.02 ROMs: maximum −124 dBFS. Channel 6 alone (`--solo 6`; part 5, which plays the uploaded timbre M09 "BanjoLB2" from measure 164) is audible in both builds, about −33 to −39 dBFS per measure, matching to 0.1 dB.
