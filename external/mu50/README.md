# libmu50 snapshot (mu50 engine)

Verbatim snapshot of the libmu50 standalone MU50 SWP00 core from
`~/workspace/libmu50/` (scaffold time, 2026-11-02), copied here so the
kernel tree is self-contained for the local `feature/mu50-engine-scaffold`
branch. The copied files are NOT modified.

Contents:

- `include/libmu50/synth.hpp` — host-facing API (register-level SWP00 core)
- `src/synth.cpp`, `src/swp00_voice.*`, `src/swp00_meg.*` — the core
- `LICENSE` — MAME BSD-3-Clause (as in libmu50's `licenses/`)

Refresh: re-copy the same files from `~/workspace/libmu50/` (do not edit
the copies in place — fixes belong upstream in libmu50).

VENDERING DECISION REQUIRED BEFORE ANY PUSH: this is a plain snapshot,
not a submodule (libmu50 has no published repo yet). Before this branch
is ever pushed, either publish libmu50 and replace this directory with
a submodule (like external/emusc, external/nuked-sc55), or keep the
snapshot deliberately and record that choice here. ROMs are NEVER part
of this directory — they stay user-supplied under `roms/yamaha/`.
