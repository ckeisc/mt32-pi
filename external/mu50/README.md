# libmu50 snapshot (mu50 engine)

Verbatim snapshot of the libmu50 standalone MU50 SWP00 core from
`~/workspace/libmu50/`, copied here so the kernel tree is self-contained
for the local `feature/mu50-engine-scaffold` branch. The copied files
are NOT modified.

2026-11-03 vendor refresh: libmu50 is now BASED on MAME 0.277's SWP00
(Listening verdict: 0.277 sounds best of 0.277–0.289). The
HEAD-based voice pipeline (with the DPCM loop-skip behavior) is
replaced by 0.277's flat 32-channel core with sequential-by-address
DPCM decode. The HEAD snapshot is preserved in
`~/workspace/libmu50-head-backup/` (and in PR #7's vendored copy).

Contents:

- `include/libmu50/synth.hpp` — host-facing API (register-level SWP00 core)
- `src/synth.cpp`, `src/swp00_0277.*`, `src/swp00_meg.*` — the core
- `LICENSE` — MAME BSD-3-Clause (as in libmu50's `licenses/`)

Refresh: re-copy the same files from `~/workspace/libmu50/` (do not edit
the copies in place — fixes belong upstream in libmu50). The engine
uses only the public Synth API (Config.sample_rate, load_wave_rom,
write_register, read_register, render, version); no HEAD-only Config
toggles (bypass_filter, nearest_interp, wrap_decode) were set by the
engine, so the 0.277 Config (which drops them) is a drop-in.

VENDERING DECISION REQUIRED BEFORE ANY PUSH: this is a plain snapshot,
not a submodule (libmu50 has no published repo yet). Before this branch
is ever pushed, either publish libmu50 and replace this directory with
a submodule (like external/emusc, external/nuked-sc55), or keep the
snapshot deliberately and record that choice here. ROMs are NEVER part
of this directory — they stay user-supplied under `roms/yamaha/`.
