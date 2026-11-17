# Host tests for the MU50 engine

Host-side tests for the MU50 engine (PR #15 and later). They build
with the system g++ against the repo's own sources -- no Pi, no
Circle build, no cross toolchain.

## Layout

- `smoke.cpp`, `meg_state.cpp`, `swp00_0277.cpp` -- libmu50 unit
  tests (ported verbatim from the libmu50 test suite; the vendored
  core lives in `external/mu50`, its tests were never in this repo).
- `keyon_test.cpp` -- KeyOnStatus gate (PR #15 Finding 3, review
  item 8): 7 hand-derived golden cases + 4000 seeded random cases
  comparing `libmu50::KeyOnStatus` against a scratch Swp0277Core
  driven directly in the test. See the file header for what the
  random comparison does and does not prove (it guards the register
  mapping; the core formulas are pinned by the goldens and by
  `swp00_0277.cpp`).
- `render_guards.cpp` -- `Synth::render` guards (review item 5):
  7 cases (null output, sample rate, ROM).
- `reg_scan.cpp` -- split-mode register readback scan (Finding 7,
  the 0x2e/0x2f written-mask): 896 never-written voice regs + 66
  write/readback pairs, through the real `CMU50Synth` wrapper.
- `xg_render.cpp` -- full MIDI render harness (single/split,
  filter mode, FIFO cap). Used for the bit-identical gates and the
  tiny-queue stress.
- `meg_offsets.cpp` -- dump the firmware-programmed MEG
  delay-offset table (registers 0x180-0x1ff) after rendering the
  start of a MIDI file. Used to check a proposed reverb-tap fix
  against the delay table before shipping it (PR #15 round 2:
  the inferred right-channel taps failed the check -- see the PR
  body -- so the fix was NOT shipped and this tool is the evidence).
- `compare_raw.py` -- compare two raw s16 stereo PCM files
  (bit-identical check + RMS/max/first-mismatch stats).
- `compare_pcm.py` -- same, for WAV files (libmu50 tool, ported).
- `midi_to_events.py` -- MIDI -> events.txt for `xg_render`.
- `stub/` -- Circle-API shims the engine tier needs (copied from
  the mu50-dualsplit host harnesses' engstub; test support, not
  firmware code).

## Build and run

```
make            # all binaries into build/
make check      # build + run the unit tier + reg_scan
```

`make check` needs the MU50 ROMs for `reg_scan` (wave ROM parts +
firmware `xr174c0.ic7` at `$HOME/roms/yamaha`, found via the
engstub fatfs mapping `SD:/roms/yamaha`). The unit tier needs no
ROMs.

## Render gates (bit-identical split vs single-core)

The gates compare single-core and split renders of the same MIDI
through `xg_render`, with `compare_raw.py`. Events files are NOT
in the repo (they are derived from game MIDIs); generate them with
`midi_to_events.py`:

```
python3 midi_to_events.py <M_E1M1.mid> e1m1_events.txt --seconds 200
python3 midi_to_events.py <MegaMan2-DrWily-Stage1.mid> megaman2_events.txt --seconds 200
./build/xg_render e1m1_events.txt e1m1_single.pcm 100 1 10 0
./build/xg_render e1m1_events.txt e1m1_split.pcm 100 1 10 1
python3 compare_raw.py e1m1_single.pcm e1m1_split.pcm   # must be bit-identical
./build/xg_render megaman2_events.txt mm2_single.pcm 100 1 10 0
./build/xg_render megaman2_events.txt mm2_split.pcm 100 1 10 1
python3 compare_raw.py mm2_single.pcm mm2_split.pcm     # must be bit-identical
```

Arguments: `<events> <out.pcm> <seconds> <filter0277 0|1>
[bootsec=10] [split 0|1] [cap 0]`. `seconds` is song time after
the boot lead-in; `filter0277=1` is the default Route A hybrid.
Gate durations used for PR #15 round 2: 10 s boot + 100 s song.

## Tiny-queue stress

Split mode with a 15-entry W-event FIFO cap (the `tiny` config
from the dual-core harnesses), same E1M1 render as above:

```
./build/xg_render e1m1_events.txt e1m1_tiny.pcm 100 1 10 1 15
python3 compare_raw.py e1m1_single.pcm e1m1_tiny.pcm   # RMS diff + waits in DONE line
```

The stress is a diagnostic, not a bit-identical gate: with a
15-entry cap the producer waits on nearly every push and writes
land at coarser timing, so split diverges from single-core. The
number to watch is the RMS diff (and `waits=` in the DONE line);
see the PR #15 round-2 response for the baselines.
