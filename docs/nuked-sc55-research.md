# Nuked-SC55 research notes (2026-10-23 / 2026-10-24)

These notes are the research record behind the basic Nuked-SC55 engine in
this branch. They are deliberately separated into what was measured, what
was ruled out, and what is still open. Anything marked as a follow-up is
NOT in this commit; it is deferred on purpose so the basic engine can land
first.

No ROM files are included anywhere in this repo. SC-55 ROMs are
user-supplied and live in `roms/sc55` on SD or USB. This work was
qualified against the SC-55 v1.21 (MK1) set only.

## 1. What this engine is

- Fourth synth engine beside Munt, FluidSynth, and EmuSC:
  `TSynth::NukedSC55 = 3`, SysEx `F0 7D 03 03 F7`, MiSTer status byte
  `0xA4`, Button 1 cycles engines, Button 2 cycles the H8 engine while
  Nuked is active.
- Backend: jcmoyer/Nuked-SC55, submodule `external/nuked-sc55`, pinned
  `66d12727cc43000d38094aaf2f76dc30cc93f448`. All backend adaptations
  live in `patches/nuked-sc55-0.8.0-circle.patch`; the engine library
  itself is not forked. The backend is GPLv2+.
- Wrapper: `src/synth/nukedsc55synth.{h,cpp}`. It scans `roms/sc55`,
  SHA-256-matches files to ROM slots, unscrables wave ROMs through the
  backend's `LoadRomset()`, and forwards MIDI byte-for-byte (CCs, bank
  select, pitch bend, SysEx all pass through; MIDI filtering was
  exonerated as a sound cause).
- Config: `[nuked]` gain (default 1.0) and oversampling (default on),
  plus `[nuked] h8_engine`. The H8 selector is
  `decoder2` (default) -> `legacy` -> `jit`, in that cycle order.
  The JIT is included in this commit but is NOT the default.
- Diagnostics: `[diagnostics]` writes `SD:/mt32pi-diag.log` (default
  interval 5 s). The init line reports romset, native rate, oversampling,
  H8 engine, heap low-water, and prime-step timing. Profile lines report
  `emu_pct` (Emulator::Step wall time / window wall time, share of one
  core), `mcu_pct` (non-PCM share of emulator time), `pcm_pct` (PCM share
  of emulator time), and `insn_per_sec` (music demand, not engine speed).
  The first profile flush after a long non-Nuked period can report
  millions of calls with `avg_us` near 0 (cumulative counters); ignore
  that first window.

## 2. H8 JIT: correct, but not a performance lever

- Design: per-instruction native AArch64 codelets, so step/peripheral
  interleaving matches the interpreters exactly. Arena 512 KB, cache
  4096 sets x 4 ways, codelets deduplicated by FNV-1a.
- Differential suite (host harness, decoder2 as oracle): directed suite
  52/52; boot 10M steps; E1M1 164,997,213 steps; BELMO 441,939,679 steps;
  every checkpoint and every audio sample bit-identical to decoder2.
  Two flag bugs were caught and fixed by the suite: ADDX sticky-Z, and
  AND/OR/XOR preserve C.
- Hardware result (diag-11, networking off, oversampling off, all three
  engines via Button 2): decoder2 emu ~89.6%, legacy ~90.2%,
  JIT ~89.6% of one core. The JIT gives no CPU gain on the Pi Zero 2 W's
  Cortex-A53. It is kept as a second, correct implementation and as a
  differential oracle, not as an optimization.
- Known landmine, documented for future wrapper edits: backend `mcu.h`
  includes `"config.h"`, and the wrapper translation unit resolves that
  to mt32-pi's own `include/config.h` instead of the backend's generated
  config, so the wrapper sees `mcu_t` without its trailing `icache`
  member. Benign only because `icache` is last and the shipped wrapper
  touches no `mcu_t` members. Future wrapper edits that need the full
  `mcu_t` must include the generated backend config first.

## 3. Boot failure and fix (hardware, 2026-10-23)

Two separate boot problems were found; do not conflate them.

- Kernel-size guard (always check before shipping): Circle's memory map
  puts core/exception stacks and L1 page tables at FIXED addresses
  starting at MEM_KERNEL_END = 0x80000 + KERNEL_MAX_SIZE (0x480000 for
  the 4 MB setting). If `kernel8.elf`'s `_end` reaches that, the
  stacks/page tables land inside the kernel's own BSS and the board dies
  silently. Check `nm kernel8.elf | grep ' _end$'` < `0x480000`.
  `KERNEL_MAX_SIZE` stays 4 MB for image-growth headroom.
- Stale 2 MB Circle library (the true hardware root cause): the 4 MB
  define reached `libs/circle/Config.mk` but the top-level
  `circle-stdlib` target is gated by `.done`, so no library objects were
  rebuilt on a config-only change. The shipped `libcircle.a` still had
  the 2 MB map (`startup64.o` `_start` literal `0x2A0000` instead of
  `0x4A0000`), so stacks/page tables overlapped the image and the board
  died late in an innocent place (MT32 analog gain calls). Fix: after
  ANY `Config.mk` / `DEFINE` change, `make -C lib clean` plus addon
  cleans, rebuild the library, and verify the startup literal
  (`0x4A0000` = 4 MB map, `0x2A0000` = stale 2 MB map).
- JIT BSS fix (in this branch): the JIT's large tables were moved from
  BSS to heap allocation on first JIT use, and the JIT arena was reduced
  from 1 MB to 512 KB, so the fixed kernel's `_end` fits under the
  4 MB map. This is the boot-fix commit on this branch.
- Build staleness (general trap): CMake engine libs (`build-nuked/`,
  `build-emusc/`) do NOT rebuild when submodule sources change, and
  `kernel8.img` does not relink when a lib changes. Force with
  `rm -rf build-<engine>` AND `rm -f kernel8.img kernel8.elf` before
  `make`, and verify via timestamps/objdump, never make's exit code.
  `Kernel.mk` has an explicit OBJS list; new `src/*.cpp` files must be
  added there.

## 4. Heap and the 255 MiB SoundFont

`MallocHeapSize` stays 96 MB so the 255 MiB SoundFont
(`0_SCB55_MKII_Kitrinx.sf2`, exactly 267,859,590 bytes) keeps fitting
(~73 MB headroom on the Lite). Nuked footprint reductions that make
this possible: ROM staging moved rather than copied where possible,
`PurgeRomData()` after load, and the LCD framebuffer in the patch
reduced from 4 MiB (1024x1024) to 832x288. The LCD shrink is the
backend's desktop-frontend pixel canvas only; the MiSTer-side virtual
LCD is separate and unaffected.

## 5. Sound: what was measured

### 5.1 Idle hum: fixed by DC-centering (in this commit)

- Pristine upstream backend rendered on the host (MK1 v1.21, 10 s, no
  MIDI, 64000 Hz): every sample on both channels was exactly +0.031250
  (1/32 = `0x1000000 / 2^29`), the SC-55 DAC mid-scale idle bias, with
  zero AC content. The Pi wrapper faithfully reproduced that DC.
- Pure DC is not directly audible, but a sigma-delta DAC fed constant
  non-zero DC can generate an audible idle tone / limit-cycle
  oscillation whose frequency follows the DC level. That is what was
  heard: the DAC's reaction, not the offset itself.
- Fix (in this commit, `OnSample`): subtract `0.03125` from both
  channels. Hardware-confirmed: the hum is gone. This is sonically
  neutral for music and matches a real SC-55, whose analog output is
  AC-coupled. Centering creates a negative DC for ~2 s during boot
  (centered value of backend output 0 is -0.03125); steady idle is
  exact 0.0.

### 5.2 Pitch: no backend pitch bug

Host sweep through the pristine backend: all 128 GM melodic programs at
note 60 plus drum-kit notes 35-81. Result: 110/128 melodic clean; the
18 flagged cases are all explained by instrument character, composite
hits, or non-pitched categories (for example inharmonic bar partials,
sub-octave drawbars, membrane partials, fifth-stacks, drums in the
melodic bank, and sound effects). Drums 47/47 audible. The backend does
not mistune any melodic instrument. The sweep tested pitch only, not
timbre, envelopes, reverb/chorus, or interpolation character.

### 5.3 Signal path: linear resampler, integer conversion, oversampling

- Path replicated on host: backend float at native rate (64000
  oversampled / 32000 native) -> wrapper linear interpolation to 48000
  -> gain 1.0 -> float-to-signed-integer by raw C cast, no clamp ->
  Circle I2S device. I2S device rate is 47,999.98 Hz for a 48000 Hz
  request (inaudible error).
- Linear-interpolation damage vs a windowed-sinc reference:
  64k->48k is mild on music (at most about -0.70 dB at 16-24 kHz, RMS
  0.0931 vs 0.0934); 32k->48k is severe (up to -3.07 dB at 8-16 kHz and
  +8.51 dB spurious image energy at 16-24 kHz). The 32k path also
  produces phantom tones (15 kHz -> 1 kHz phantom at -19.16 dB, vs
  -81.18 dB for the reference).
- Integer conversion cleared by measurement: peaks below 1.0 (no
  clipping), max integer magnitude 8,256,511 < 8,388,607 (2^23 - 1, no
  overflow/wrap), quantization noise about -140 dBFS, truncation DC
  error about -148 dBFS. Both inaudible.
- A/B listening samples (M_E1M1, Pi-linear vs reference-sinc, at both
  native rates) were all judged "way too clean" against the Pi
  experience. Conclusion: the backend raw output is clean AND the
  linear resampler is clean to the ear, even on the worst (32k) path.
  The measured resampler difference is real but is NOT what is heard
  on the Pi. The Pi cause is device starvation (section 7).
- Oversampling is CPU-neutral (code-grounded): the expensive work is
  per PCM tick, not per output sample. Each PCM tick advances
  `pcm.cycles` by a fixed `(reg_slots + 1) * 25`, independent of
  oversampling; oversampling gates only the second `MCU_PostSample`
  and the reported output frequency. Ticks per output frame are
  identical on and off (0.667 ticks/frame both ways). Recommendation:
  oversampling ON (no CPU cost, upstream default, less resampling
  damage). It is not a CPU lever and not the sound fix.

## 6. CPU profile (measured, overnight 2026-10-24)

Full report: `docs/nuked-sc55-cpu-profile-20261024.md` (QEMU TCG plugin,
exact instruction counts, AArch64, `-Ofast -mcpu=cortex-a53`, full
M_E1M1, decoder2; cross-checked with gprof on x86-64; hardware anchor
diag-11).

Headline split (steady-state song interval, 85.3B instructions):
PCM voices/mixing 53.84%, H8 exec/timers/peripherals 35.53%
(timer subsystem 19.41%), H8 dispatch+execution (decoder2) 6.34%, step
scaffolding 1.52%. Reverb/chorus is 1.99%, not a hotspot.

Ranked optimization strategy (measurement-only, NOT implemented in
this commit; see follow-ups):

1. Force-inline `sx20` (7.53%, outlined at ~250M call sites):
   expected gain ~4.5-6%, zero semantic risk.
2. Timer lazy catch-up + arithmetic jump to next match (19.41%):
   expected gain ~10-15%, medium risk, gated on the full differential
   suite passing unchanged.
3. `calc_tv` prologue caching (11.97%): expected gain ~3-4%,
   low-medium risk.
4. Force-inline `PCM_ReadROM` (6.12%): expected gain ~1-2%, zero
   semantic risk.
5. Step-scaffolding inlining (~6.46% combined opportunity): expected
   gain ~1-2%, low risk, only after items 1-4.

Explicitly not worth optimizing: H8 dispatch/JIT/codelets (6.34% total;
even a perfect JIT saves at most ~6.3%, and hardware already proves
it). NEON vectorization of the voice pipeline is a stretch goal only
(large, high-risk rewrite for uncertain gain).

Caveat: QEMU timing does not transfer to the Pi (TCG is far slower
than hardware). What transfers is the instruction mix / relative hot
spots (same sources, same optimization level, same A53 codegen
target). The Pi kernel's render loop could not be profiled under
system QEMU at all: core 0 boots then freezes and cores 1-3 spin in the
secondary-boot stub, a QEMU-side control-plane/device stall, not guest
code. The user-mode + harness path was used instead.

## 7. Device starvation: the Pi sound mechanism (measured)

This is the leading explanation for the Pi's bad Nuked sound, and it is
NOT fixed in this commit.

- Diag logs (wall-timed 5.008 s windows, expected 240,384 output frames
  per window): steady windows delivered roughly 143,872-212,736 frames
  (about 69-88% of real time; dense windows about 69%, i.e. ~33k
  frames/s against the DAC's 48k clock, a ~31% shortfall). The emulator
  runs at ~1.15M H8 instructions/s vs ~1.66M needed for real time at
  ~52 instructions/backend-sample: 0.69x real time, matching 33k/48k
  exactly. EmuSC sounds good through the identical device/DAC because
  its render is fast enough to never starve. Host renders sound clean
  because they are offline (no real-time deadline, no DMA, no holes).
- Circle's DMA TX path zero-fills every shortfall with silence
  (`m_NullFrame` in `CSoundBaseDevice::GetChunkInternal`); it never
  repeats stale data. Gaps are one DMA buffer = 128 frames = 2.67 ms,
  at 16-40% duty depending on music density. That reads as
  roughness/mechanical texture, preserves pitch and tempo, and hits
  sustained instruments hardest, matching "only some instruments off"
  as gap-exposure, not a per-instrument bug. Total buffering is only
  ~10.7 ms (256-frame queue + 2x128-frame DMA buffers).
- Single `RenderFrames` calls also stall up to ~20 ms (`max_us` in diag
  logs); during a 20 ms stall the device consumes 960 frames against
  512 frames of buffering, giving a ~14+ ms silence gap on top of the
  2.67 ms texture. A larger chunk size would absorb stall gaps but
  cannot fix a sustained rate deficit.
- Corrections to earlier claims (owned): `underrun_frames = 0` does NOT
  prove the stream is intact. The wrapper underrun fires only when the
  step loop fails to produce enough backend samples for the requested
  chunk; the emulator always catches up within budget, just slowly, so
  wrapper underrun stays 0. Device starvation (DMA queue empty while
  the DAC clock runs) is a different quantity and no counter for it
  exists in the log. Likewise `frames/calls` is a count-weighted mean
  chunk size, not a queue level; the queue oscillates full-to-empty.
- A simulation of periodic 2.67 ms zero holes at 31.1% duty was judged
  "closer" to the Pi sound but not exact. Real gaps are irregular and
  clustered in dense passages and include the ~14 ms stall gaps, so
  the simulation is not expected to match exactly. The decisive hole
  measurement still needs a line capture (Y-splitter, WAV, EmuSC +
  Nuked, same passage), not phone recordings: phone A/B dip analysis
  was inconclusive for holes (room reverb + AAC + iOS voice-memo
  processing bury them, and the two recordings were different
  passages). Level-normalized spectra did show Nuked brighter/harsher
  in every band (+1.8 to +9.8 dB), which is supporting but ambiguous
  (synth timbre vs hole splatter).
- CPU implication: the section 6 optimizations are also a partial
  sound fix (estimated starvation ~31% -> ~17%, delivery ~33k -> ~40k),
  but a full sustained fix needs ~45% more throughput (block-
  compilation JIT or NEON voice work) or acceptance of partial
  improvement. Buffer increases mitigate stall gaps only, never the
  sustained deficit.

## 8. Halt under load/IO contention (NOT fixed in this commit)

Symptom: with WiFi+FTP on, or with the SD sample/MIDI capture kernel
on, the Pi halted a few seconds after switching to Nuked; other engines
did not halt in those sessions. With networking off and capture off,
Nuked ran 154.5 s clean (diag-13, delivery 69-88%, emu 87-90%,
underrun 0 throughout). Networking stays OFF on the hardware for now;
the debug-endpoint idea (WiFi/FTP log access) is parked.

What code investigation ruled out (Nuked-internal):

- No unbounded loop in the MK1/decoder2 Step path: `MCU_Step` has no
  loops; `TIMER_Clock` FRT/TMR deadline loops always advance (stride
  >= 2 for FRT; TMR stride 0 is guarded to deadline UINT64_MAX);
  `PCM_Update`'s tick loop increments unconditionally by
  `(reg_slots+1)*25` with `reg_slots` in [1,32], so never 0.
- No MIDI race: delivery is MainTask-only; IRQ handlers only enqueue
  into a spin-locked ring; PostMIDI (MainTask, under `m_Lock`) and Step
  (AudioTask, under `m_Lock`) are serialized, never concurrent.
  PostMIDI overwrites on full (no block, no growth).
- No gradual exhaustion: no C++ voice allocator (firmware voice mask,
  fixed 32); no steady-state heap allocation in Step/RenderFrames;
  counters are uint64/double (no wrap on a seconds timescale).
- `m_Lock` is a lag vector, not a wedge: RenderFrames holds it for a
  full chunk (~10-20 ms), so MainTask MIDI/switch calls block up to
  that long, but RenderFrames always returns, so the lock is always
  released. No AB-BA deadlock (stats lock is nested inside `m_Lock`
  only, and taken alone by UITask).

Leading account (unproven, needs the card tests below): MainTask stuck
inside a FatFs/SD call, holding the volume lock that in this build has
no effective timeout (`ff_req_grant` always returns 1), freezing
controls/Drain/diag while AudioTask keeps rendering idle/silence.
Capture/FTP/WiFi are amplifiers (more MainTask FatFs load + MainTask
delay), not the root. The root would then live in the shared FatFs/SD
layer, not in Nuked code and not in capture/FTP code. Sampled SD paths
are timeout-bounded and sampled FatFs paths are offset-bounded, so the
exact stuck call is in an unexamined path or a card-state edge case
(the SD card dates from the .3 era and has survived many halts and
card pulls).

Follow-up fixes (deferred, NOT in this commit):

- Nuked-side: (i) MIDI/control deferral: MainTask enqueues MIDI, engine
  switches, and AllSoundOff into a lossy lock-free SPSC FIFO; AudioTask
  drains it under `m_Lock` at the start of RenderFrames; MainTask never
  acquires `m_Lock` again, so MainTask liveness becomes independent of
  backend speed. (ii) Backend loop caps via the `patches/` mechanism
  (TIMER_Clock loops, PCM_Update tick loop, JIT dedupe walk): break,
  log a diag error, continue degraded, never loop forever; caps must
  never trip on correct state. (iii) `m_Lock` scope reduction (release
  before the interp loop and stats). (iv) Keep the RenderFrames step-
  budget + underrun silence + resync path as-is.
- Shared-layer companions (required for a full "never halt" guarantee):
  (a) a real timeout on the FatFs volume lock; (b) move capture drain
  off MainTask onto its own low-priority sacrificial task (rings fill
  and drop, counted, but MainTask/controls/UI/audio stay alive);
  (c) count short `f_write`/`f_sync` results as drops. For a full
  guarantee MainTask must never call FatFs directly, including the
  diag `LogLine` path.
- Also deferred: the `mt32pi.cpp` IntBuffer zero-size bug found during
  capture analysis (`s8 IntBuffer[nQueueSizeFrames * nBytesPerFrame +
  bI2S? 0: 1]` parses as `((...)+bI2S)?0:1`, size 0, so IntBuffer
  aliases FloatBuffer's base and the float capture file is bit-for-bit
  the int32 device samples, showing as NaN when read as float). Device
  values are correct and this is not a halt cause, but it breaks the
  float-vs-s32 capture isolation. Fix: parentheses
  (`+ (bI2S? 0: 1)`). The SD capture feature itself (post-interpolation
  float + post-conversion s32 + MIDI log, MainTask-drained) was built
  and used for diagnosis, then removed from this clean branch; its
  sources are preserved outside the repo for the follow-up.
- Verification still to run (hardware): capture on + Nuked + music
  60 s, no halt, clean stop line on switch away; dense MIDI + capture,
  no halt, only counted FIFO drops; idle Nuked + capture 60 s, no
  halt; host-harness loop-cap trip test (corrupt stride/reg_slots to 0,
  backend must return with an error, not hang); `fsck`/`chkdsk` the SD
  card and a fresh-card A/B (if capture works on a fresh card, card FAT
  state is implicated; if it still halts, it is code/driver); halt
  phenomenology if it still halts (do buttons/LCD respond, does it
  recover without a power cycle, music or idle, which MIDI input).

## 9. Known limitations of this commit

- Not full speed on the Pi Zero 2 W (mt32-pi Lite): dense music runs
  about 0.69x real time (section 7). Sound quality under starvation is
  still under investigation; the section 6 optimizations are expected
  to improve it but not cure it.
- Sound quality still bad without the hum (separate issue, narrowed
  to the shared Nuked path; gain/clipping cleared, resampler cleared
  as the Pi cause by A/B listening, backend pitch cleared by sweep).
- Halt under IO contention is NOT fixed (section 8). Keep networking
  off; do not re-enable WiFi/FTP for log access without the follow-up.
- Oversampling off does not help CPU (section 5.3); keep it on.
- The JIT is a correctness second implementation, not a performance
  lever (section 2). Do not optimize on assumption again: every
  optimization in the follow-up must be tied to a measured share and
  gated on the differential suite plus E1M1 audio bit-identity.
- Still open from before: validating that SC-55 v1.21 is the most
  DOS-game-compatible ROM set.
