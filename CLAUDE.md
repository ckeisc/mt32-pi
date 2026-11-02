# Working on pim-32

pim-32 is a community continuation of [dwhinham/mt32-pi](https://github.com/dwhinham/mt32-pi): a bare-metal MT-32 (Munt) and SoundFont (FluidSynth) synth for Raspberry Pi, built on the [Circle](https://github.com/rsta2/circle) framework via circle-stdlib. It is not Linux. "Updating the base" means updating Circle.

**Start every session by reading the punchlist's "Current state / handoff" section:** https://claude.ai/artifact/QZmVguavkG1sY6nDBfaEXs. It says what's released, what's open, and what to do next. Update that section before you finish, and tick or add punchlist items as work completes or new tasks turn up.

## Layout

- `src/`, `include/`: mt32-pi itself. `src/synth/mt32synth.cpp` wraps Munt, `src/synth/soundfontsynth.cpp` wraps FluidSynth, `src/mt32pi.cpp` holds the audio, MIDI and UI tasks (one per core).
- `external/`: submodules. `circle-stdlib` (v21, Circle Step 51.1.1, newlib 4.5.0), `munt` (libmt32emu 2.8.3), `fluidsynth` (2.6.1, built with its `embedded` OS abstraction; `make submodules` also fetches its `gcem` submodule), `inih` (r62).
- `patches/`: local patches applied to submodules by the `Makefile` (`APPLY_PATCH`) and reversed by `make mrproper` (`REVERSE_PATCH`). Patch names carry the submodule version they apply to. Circle needs none since Step 51.1.1: unused USB drivers are left out with `EXCLUDE_USB_*` defines in the `Makefile`.
- `Config.mk`, `Kernel.mk`, `Makefile`: build. `BOARD` is one of `pi2`, `pi3`, `pi3-64` (default; also Pi Zero 2 W), `pi4`, `pi4-64`.
- `sdcard/`: files shipped on the SD card (`config.txt`, `mt32-pi.cfg`).
- `scripts/`: user-facing updater/installer (Python, linted in CI). `scripts/dev/munt-ab/`: Munt A/B render and partial-report tools (see its README).
- `BUILDING.md`: local build setup, including the toolchain download.

## Build

```sh
make BOARD=pi3-64            # kernel8.img; HDMI_CONSOLE=1 shows the log on HDMI
make mrproper                # reverse patches, clean everything (do this before switching branches,
                             # with the old branch still checked out)
```

- **Toolchain: Arm GNU Toolchain 15.2.rel1** (`aarch64-none-elf` and `arm-none-eabi`), as Circle 51.1.1 recommends. Releases up to 0.16.1 used 11.3.rel1 because GCC 15 builds on Circle 45 caused boot failures (ROMs not found, synth init failing), "sync" exception crashes and silent SysEx-uploaded timbres on a Pi Zero 2 W. Phase 4.5 moved to 15.2 after a hardware soak test; if those symptoms come back, compare against an 11.3.rel1 build of the same commit first.
- Munt is built with `-Ofast`. Offline renders show this is bit-for-bit close to `-O2` for Munt 2.7.0 and 2.8.3.
- Each core's stack is 128 KB (`KERNEL_STACK_SIZE`), with no guard. Munt's `doRenderAndConvertStreams` frame is ~98 KB on the audio core, so keep large buffers off that stack.

## Git and GitHub

- Branch from `main`, one pull request per change; one dependency upgrade per commit.
- **Submodules:** after checking out or rebasing, run `git submodule update`. Never `git commit -a` or `git add .` while a submodule is checked out at another branch's version; stage files by name and check `git show --stat HEAD` before pushing. (This once slipped a Munt bump into a changelog commit.)
- **Pull requests:** create and merge them with the REST API (`gh api repos/macroexp/mt32-pi/pulls ...`, `gh api -X PUT .../pulls/N/merge`). GraphQL (`gh pr create`) is blocked from Claude sessions. CI status: `gh api repos/macroexp/mt32-pi/commits/<sha>/check-runs` (the Actions API is blocked).
- **Releases:** Claude sessions can't push tags or create releases. Date the version in `CHANGELOG.md` (`## [X.Y.Z] - YYYY-MM-DD` plus the compare link at the bottom) in its own PR, then the maintainer publishes a release from `main` with tag `vX.Y.Z` in the GitHub UI. CI builds the tag, extracts that version's CHANGELOG section as the notes and attaches `pim-32-X.Y.Z.zip`.
- Version tags must sort above upstream's v0.13.1 for the updater. The device still reports itself as `mt32-pi` (boot log, FTP banner) because the updater parses that.

## CI

`.github/workflows/ci.yml`: lint (black, isort, flake8, shellcheck), builds for pi2, pi3-64, pi4-64 (packaged) plus pi3, pi4 (build check only), and a package job that fetches GeneralUser GS v2.0.3 (pinned by commit and SHA-256). Actions are pinned to tagged versions.

## Testing

- **Hardware (maintainer):** Pi Zero 2 W, FiiO Taishan I²S DAC, SSD1306 I²C OLED, simple buttons, HDMI monitor for `HDMI_CONSOLE=1` builds. `mt32-pi.cfg`: I²S, chunk_size 256, Wi-Fi on, RTP and UDP MIDI on, USB off, rom_set old (MT-32 v1.x ROM), verbose on. MIDI comes from Cakewalk on a PC. The maintainer has SD cards with stock 0.13.1 for comparison and can record audio.
- **Regression test:** Laura Bow 2 (Sierra). Send the setup `AMONRA.SYX`, then play `LB2MT.MID`. Part 5 (MIDI ch 6) uses the uploaded timbre M09 "BanjoLB2"; measures 164-188 are a good dense/sparse stretch. These files are Sierra's and aren't in the repo; ask the maintainer to attach them.
- **Munt changes:** run `scripts/dev/munt-ab/run.sh` (renders old vs new with the maintainer's ROMs; `--report`, `--solo`, `--sysex`).
- **Debug kernels:** branch `debug-midi-log` (never merge it) adds `MIDI_DEBUG_CHANNEL=N` logging, timbre checksums and PCM checksums. Its heavy HDMI logging disturbs audio and causes audible distortion, so never judge sound quality on a debug kernel.
- Always hand the maintainer builds that differ from a known-good build in exactly one respect, and state what that respect is.

## Licence

GPLv3. Keep copyright notices; every binary release ships with its source.
