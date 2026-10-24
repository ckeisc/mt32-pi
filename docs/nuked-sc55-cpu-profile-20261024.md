# Nuked-SC55 CPU profile — measured evidence (overnight, 2026-10-24)

## Method (what was actually measured, not theorized)

- **Primary: QEMU TCG plugin, exact instruction counts.** Custom plugin
  (`/tmp/plugins/libmyhotblocks_snap.so`, derived from QEMU 8.2 `hotblocks`,
  limit raised, per-CPU tables, plus wall-clock snapshots every 180 s to a
  plain file so a frozen control plane cannot lose the data) run under a
  freshly built user-mode QEMU 8.2.2 (`~/workspace/tools/qemu-user-build/
  qemu-aarch64`) on the AArch64 differential-test harness
  (`jit-test-arm`, backend built `-Ofast -mcpu=cortex-a53` — the Pi Zero 2
  W's core), workload = full M_E1M1, engine = decoder2 (the Pi default).
  Run completed exit 0; golden confirms `steps 164997213` (the known E1M1
  count). Total: 119,358,521,630 guest instructions.
- **Cross-check: gprof** on the native x86-64 harness build, same workload
  (165M steps, 93 s wall). Same ranking, same top functions.
- **Hardware anchor:** Joe's diag-11 log (read from the file):
  JIT steady windows `emu_pct=89-90, mcu_pct=55-63, pcm_pct=36-44,
  insn_per_sec~1.0-1.08M`.
- **NOT measured (rig blocker, see below):** the Pi kernel under system
  QEMU (raspi3b). That path cannot run the render loop at all (see
  "System-QEMU rig blocker").

## Hot-spot table — AArch64 plugin, steady-state song interval
## (snapshot 1 -> snapshot 5, 85,275,377,446 instructions; identical to
## 0.01 pp in the shorter snap-1->snap-2 and snap-1->snap-3 intervals,
## and within 0.1 pp of the full-run cumulative)

| share | function (file:line) | what it does |
|---|---|---|
| 25.77% | PCM_Update, pcm.cpp:576 (self only) | per-tick PCM render: voice loop + final mixing |
| 11.97% | calc_tv, pcm.cpp:317 | voice envelope generator (TVA/TVF/TVP, 3x per voice per tick) |
| 8.62% | TIMER_Clock, mcu_timer.cpp:385 | per-step timer polling loop (3 FRT + TMR deadlines) |
| 7.53% | sx20, pcm.cpp:265 | 20-bit sign extension `(in<<12)>>12` — OUTLINED, not inlined |
| 6.98% | TIMER_ClockFrt, mcu_timer.cpp:331 | free-running timer tick (compare/flag/interrupt checks) |
| 6.12% | PCM_ReadROM, pcm.cpp:31 | wave ROM byte fetch (bank switch + mask + deref), 380M calls |
| 4.94% | MCU_Step, mcu.cpp:927 | per-step orchestration (IRQ, instr, PCM, timers, UART, analog) |
| 3.78% | TIMER_ClockTmr, mcu_timer.cpp:375 | 8-bit timer tick (same shape as Frt) |
| 3.46% | decoder2::FetchDecodeExecuteNext, dispatch.cpp:174 | H8 DISPATCH (fetch+decode+execute dispatch) |
| 2.68% | MCU_Read, mcu.cpp:445 | H8 bus/peripheral reads |
| 1.97% | MCU_Interrupt_Handle, mcu_interrupt.cpp:154 | interrupt prioritization per step |
| 1.56% | eram_pack, pcm.cpp:498 | wave-data repack into ERAM (2-bit exponent format) |
| 1.52% | Emulator::Step, emu.cpp:203 | Step wrapper (profiling hook + MCU_Step call) |
| 1.19% | MCU_Write, mcu.cpp:647 | H8 bus/peripheral writes |
| 1.11% | MCU_ReadInstruction, mcu.cpp:812 | H8 instruction fetch from ROM/RAM |
| 0.99% | MCU_UpdateUART_RX, mcu.cpp:891 | UART receive polling per step |
| rest | each <=0.83% | incl. all I_* H8 execute handlers (largest single: 0.22%) |

## The split (steady-state interval)

- **PCM voices/mixing: 53.84%**
  (PCM_Update body 25.74 + calc_tv 11.99 + sx20 7.53 + ReadROM 6.12
  + eram pack/unpack 2.00 + addclip20 0.28 + multi 0.09 + PCM_Write 0.06)
- **H8 exec / timers / peripherals: 35.53%**
  (timer subsystem 19.41 = Clock loop 8.83 + ClockFrt 6.98 + ClockTmr 3.78
  + remainder; MCU_Step 4.94; MCU_Read 2.68; IRQ_Handle 1.97; MCU_Write 1.19;
  ReadInstruction 1.11; UART_RX 0.99; Analog 0.69; UART_TX 0.55; Read16 0.42;
  SetStatus 0.58; Write16 0.24; IRQ helpers ~0.4)
- **H8 dispatch + execution (decoder2): 6.34%**
  (FetchDecodeExecuteNext 3.46 + all I_* handlers + LoadFromReg/
  LoadFromEA/StoreToReg/GetPageForRegister helpers ~2.9)
- **Step scaffolding (Emulator::Step): 1.52%**
- **Other: 2.77%** (harness loop, NukedProfileClock, OnSample)
- **Reverb/chorus specifically (pcm.cpp:1440-1510): 1.99%** — NOT a hotspot.
  (2 multiplies per voice-tick for sends + a small return path.)
- **Final mixing head (pcm.cpp:575-650): 2.16%.**

## Cross-check (x86 gprof, self-time %; same workload, decoder2)

PCM_Update 42.91, TIMER_Clock 14.18 (Frt/Tmr inlined into it on x86),
MCU_Step 7.46, PCM_ReadROM 4.80, calc_tv 3.69, FetchDecode 2.73,
IRQ_Handle 2.29, Emulator::Step 1.77, MCU_Read 1.48, MCU_UpdateAnalog 1.18,
I_* handlers sum ~7.5. Rollup: PCM ~51.4%, timers+peripherals ~20.8%,
dispatch+exec ~10.2%, step ~9.2%. Same ranking as AArch64; the AArch64
timer share is higher (19.4% vs 14.2%) because Frt/Tmr are NOT inlined
on AArch64 (cortex-a53 codegen) while they are on x86.

## Corrections to prior claims

- The old "host H8 ~94% / PCM ~6%" anchor is SUPERSEDED. It counted
  steps/ticks, not work. Both work-based profiles say PCM ~51-54%,
  H8+timers+peripherals ~36-42% (incl. dispatch), dispatch ~6-10%.
- Pi wall-time split (diag-11: mcu ~62 / pcm ~37) vs backend insn split
  (non-PCM ~46 / PCM ~54) differs by ~15 pp. Explained in part by
  oversampling: diag-11 ran oversampling OFF, the profiles ran ON —
  but see below: oversampling is CPU-neutral per tick, so the residual
  gap is song density (E1M1 vs diag-11's unknown test music) plus
  wall-time vs insn-count measurement (IPC: straight-line PCM code
  vs branchy timer code). The target ranking does not depend on it:
  PCM and timers dominate under both measurements; dispatch is small
  under both.

## Why oversampling is CPU-neutral (code-grounded, explains diag-11)

- Each PCM tick advances `pcm.cycles` by a FIXED `(reg_slots+1)*25`
  (pcm.cpp:1565-1567), independent of oversampling. Ticks per H8 step
  are therefore rate-independent.
- Oversampling gates ONLY the second `MCU_PostSample` in the mixing
  head (pcm.cpp:621) and halves the REPORTED output frequency
  (pcm.cpp:1580-1587), which sets the wrapper's resample ratio.
  Ticks per output frame are identical ON and OFF (2 samples/tick at
  ratio 1.333 vs 1 sample/tick at ratio 0.667).
- Hence diag-11 (OFF) vs diag-7 (ON) showing emu ~90% and pcm ~37-46%
  in both cases is EXPECTED, not a paradox. Oversampling is a fidelity
  choice, not a CPU lever. (Prime-step halving is init-time trivia
  and is not used in this reasoning.)

## RANKED optimization strategy (each item tied to a measured share)

### 1. Force-inline sx20 (and verify addclip20/multi stay inlined)
- **Share: 7.53%** for a 2-instruction function `(in<<12)>>12`
  (pcm.cpp:263-266). It is `inline` in source but appears as an
  outlined function in the profile — the compiler outlined it at its
  ~250M call sites (via addclip20/multi, which ARE inlined, so their
  sx20 calls remain calls). Each call pays branch+link + return for
  2 instructions of work.
- **Expected gain: ~4.5-6% of total backend time** (call overhead on
  ~250M calls; the 2 shifts of work remain, spread into callers).
- **Risk: zero semantic change** (same arithmetic, just inlined).
  Apply `__attribute__((always_inline))` to sx20 (and confirm
  addclip20/multi, currently 0.28%/0.09%, stay inlined).
- **Verify on hardware:** rebuild, run E1M1 differential (audio
  bit-identical required), re-profile: sx20 must vanish from the top
  list; emu_pct should drop ~4-5 points at equal music density.

### 2. Timer lazy catch-up + arithmetic jump to next match
- **Share: 19.41%** (Clock loop 8.83 + Frt 6.98 + Tmr 3.78).
  Per Step, TIMER_Clock (mcu_timer.cpp:375-400) polls 4 deadline
  while-loops even when no tick is due, and each due tick runs the
  full compare/flag/interrupt-check sequence (ClockFrt:331,
  ClockTmr:375). The deadline structure already exists.
- **Mechanism:** (a) fast path: if target_cycles < min(next deadlines),
  return after ONE comparison instead of 4 while-loop setups
  (saves the 8.83% loop share); (b) when a deadline IS reached, advance
  counters arithmetically to the next compare-match/overflow instead
  of iterating tick-by-tick (ticks between matches are pure counter
  increments — the flags/interrupts only matter AT matches).
  Firmware-visible behavior is preserved as long as timers are caught
  up before any firmware register read (MCU_Read path) and before the
  per-Step interrupt check (MCU_Interrupt_Handle) — both are existing
  seams.
- **Expected gain: 10-15% of total backend time** (loop overhead fully,
  plus most per-tick iteration; only true timer events still cost).
- **Risk: MEDIUM — timer timing is music-critical** (tempo, envelopes,
  LFOs derive from these timers). Gate: the existing differential
  suite (boot/E1M1/BELMO state hashes + audio bit-identity) must pass
  unchanged, exactly as the JIT gate did.
- **Verify on hardware:** emu_pct drop at equal music density with
  identical audio output (bit-compare a recorded render).

### 3. calc_tv prologue caching (envelope parameter cache)
- **Share: 11.97%.** calc_tv (pcm.cpp:313-477) runs 3x per voice per
  tick (TVA/TVF/TVP). Its prologue derives speed/target/type from
  `adjust` plus `nfs` — but `adjust` changes only when the firmware
  writes new envelope parameters (rare vs. per-tick calls), while the
  per-tick sum path feeds `volmul` every tick (pcm.cpp:463-472) and
  cannot be skipped.
- **Mechanism:** cache (speed, target, type) per envelope slot keyed by
  (adjust, nfs); recompute only when the key changes. Skips the
  prologue + type switch on most calls; the sum path still runs.
- **Expected gain: ~3-4% of total** (prologue is roughly a third of
  calc_tv; exact fraction to be measured after implementing).
- **Risk: LOW-MEDIUM** — cache invalidation must be exact (any adjust
  or nfs change must invalidate; the key covers both). Gate: same
  differential suite + audio bit-identity.
- **Verify on hardware:** emu_pct drop + bit-identical audio.

### 4. Force-inline PCM_ReadROM
- **Share: 6.12%, 380M calls** (pcm.cpp:30-66). It is a free function
  (NOT marked inline): bank switch on config_reg_3d + address masks
  + dereference, per byte. Voice fetches are sequential, so the bank
  rarely changes within a fetch run, but the per-call overhead is
  paid on every byte.
- **Expected gain: ~1-2%** (call overhead only; the address computation
  remains). Zero semantic risk.
- **Verify:** ReadROM vanishes from the profile; audio bit-identical.

### 5. Step-scaffolding inlining (minor)
- **Share: 6.46%** (MCU_Step 4.94 + Emulator::Step 1.52). Inlining
  Emulator::Step into the wrapper's render loop saves one call per
  step; MCU_Step's own body (cycle accounting, the is_mk1 branches)
  is already minimal.
- **Expected gain: ~1-2%. Risk: low.** Do only after 1-4.

### Stretch only (NOT recommended as primary work)
- Voice-pipeline SIMD/NEON vectorization: the voice work is spread
  across ~1000 lines of PCM_Update with shared accumulators and
  faithful hardware-model data flow (ram1/ram2 buses). Vectorizing
  without changing semantics is a large, high-risk rewrite for an
  uncertain gain. Revisit only if 1-4 land and headroom is still short.

## Explicitly NOT worth optimizing (backed by the measurements)

- **H8 dispatch / JIT / codelets: 6.34% TOTAL on AArch64
  (FetchDecode 3.46 + all handlers/helpers ~2.9%), ~10% on x86.**
  Even a perfect JIT (zero dispatch+exec cost, impossible — native
  code still executes the semantics) saves at most 6.3%. The
  hardware result already proves the point: JIT vs decoder2 vs legacy
  all cost ~90% emu (diag-11: decoder2 89.6, legacy 90.2, jit 89.6 —
  JIT gains ~0%). The JIT was the wrong target, as Joe suspected;
  further dispatch work is chasing 6% with a mechanism already proven
  to yield 0%. The JIT remains valuable as a second CORRECT
  implementation (differential oracle), not as a performance lever.
- **Reverb/chorus specifically: 1.99%.** The sends are 2 multiplies
  per voice-tick plus a small return path (pcm.cpp:1440-1510). There
  is no reverb/chorus hotspot to optimize.
- **Oversampling toggle: CPU-neutral** (see code-grounded proof above;
  diag-11 falsified it as a lever: emu stayed ~90%).
- **Wrapper resampling/ring: bounded by hardware at <=10%** of the
  core (non-emulator share), and linear interpolation is a handful of
  ops per output frame. Not a target.
- **LoadRomset/SHA/unscramble: one-time boot cost** (2.35%/1.07%/
  0.44% in the boot-heavy first snapshot, diluting to <0.5% over a
  full song). Not a steady-state target.
- **JIT_NotifyWrite: 0.16%** (the JIT invalidation hook firing in
  decoder2 mode). Negligible even though it runs in the wrong mode.

## Answers to Joe's 2026-10-24 01:42 questions (for the parent's reply)

- **Does bad sound come from high-but-not-100% CPU? NO — not by
  starvation.** Hardware shows underrun=0 in every window: the audio
  stream is never starved. High CPU = thin headroom (miss_pct in dense
  passages is the symptom), not distortion. The remaining bad sound
  is a fidelity/timbre issue in the shared Nuked path (post-hum-fix),
  separate from CPU. This profile does not explain the timbre issue;
  it rules out starvation as its cause (again, with numbers).
- **Why does the host/QEMU sample sound good while the Pi sounds bad?**
  NOT answered by this profile (the profile measures CPU, not audio
  quality), and the leading candidates remain what they were:
  Pi wrapper linear-interpolation resampling to 48000 (vs host native
  rate float output), gain staging, float->s32 conversion, and the
  I2S DAC/analog chain. The profile DOES bound the wrapper: it is
  <=10% of the core and its resampler is a few ops per frame, so a
  wrapper-side fidelity problem would be in the resampler's MATH
  (linear interpolation dulls/aliases), not its cost. The
  post-interpolation, post-conversion sample capture Joe ordered is
  still the right next test for the sound question (separate from
  this CPU work).

## System-QEMU rig blocker (why the Pi kernel could not be profiled
## under system QEMU, and what was proven instead)

- Under system QEMU (raspi3b, clone kernel, null-sink audio patch so
  the audio task does not block on the unemulated audio DMA),
  snapshots show: **core 0 executes the whole boot (6180 TBs, incl.
  LoadRomset/SHA/EmuSC WaveRom) and then FREEZES (identical counts in
  snap-1 and snap-2, 3 min apart), while cores 1/2/3 spin forever in
  QEMU's own AArch64 secondary-boot stub at 0x300/0x30c** (3-insn
  `wfe; ldr; cbz` loop, ~800M iterations per core per snapshot).
  The stub (raspi.c `write_smpboot64`, SMPBOOT_ADDR 0x300) waits on
  the spintables at 0xd8/0xe0/0xe8/0xf0.
- Circle's release mechanism MATCHES QEMU's stub exactly
  (multicore.cpp: `ARM_SPIN_TABLE_BASE 0xD8`, writes
  `_start_secondary`, `CleanDataCache()`, `sev` loop, with "did not
  start" error logging) — yet no mcore error appears in the serial
  log and the secondaries never start. And core 0 freezes after boot
  (no TBs for 3+ min), which together with the HMP monitor, GDB stub,
  and SIGTERM all going unresponsive while vCPUs spin points at a
  QEMU-side control-plane/device stall (vCPU-0 wedged in device
  emulation, likely the bcm2835-dma path that also logged
  "Blocked re-entrant IO" in run A), not at guest code.
- **Consequence: the Pi kernel's render loop NEVER executes under
  system QEMU** (AudioTask is pinned to core 2, which never starts;
  MainTask on core 0 wedges after boot). Any "profile" from that rig
  would be boot-only, not render data. The user-mode QEMU + harness
  path above was chosen because it runs the IDENTICAL backend
  (same sources, same -Ofast -mcpu=cortex-a53 build) with exact
  counts and a working exit path. The system-QEMU rig needs a
  separate debugging effort (secondary release + core-0 wedge) before
  it can profile the kernel; it is NOT a prerequisite for the
  optimization strategy, which rests on the backend data.
- **Also established en route (rig findings worth keeping):**
  (a) `qemu_plugin_outs` output is DROPPED unless `-d plugin` is
  passed (run A's report was lost for this reason; run B/C used
  direct file snapshots instead, which are mask-independent).
  (b) QEMU's HMP monitor and GDB stub go unresponsive under heavy
  TCG load on this rig (socat got no prompt in 15 s; all 200 HMP
  reads returned empty; GDB stub sent no stop packet in 10 s) —
  monitor/GDB sampling is not a viable cross-check on this rig.
  (c) The guest's diag flush requires UpdateLCD, which never runs
  with lcd type none + mister off (UITask early-exits) — so missing
  diag render/profile lines in QEMU prove nothing about audio.
  (d) The MIDI path DOES work: USB-serial (QEMU usb-serial chardev)
  delivers M_E1M1 bytes to the guest (sender backpressure stretches
  wall time ~3x, consistent with guest-time consumption); the guest
  just never renders them because AudioTask never starts.
  (e) GDB (toolchain) remains unusable: libncursesw.so.5 loader error
  (worked around via ~/workspace/tools/gdb-libs), then embedded
  Python 3.8 stdlib missing (only Python 3.12 on the VM).

## Caveats (plainly)

- QEMU timing differs enormously from hardware (prime wall cost
  ~60-70x hardware; TCG executes ~120M guest insns/sec vs the Pi's
  ~1M H8 insns/sec + PCM). Absolute times do NOT transfer. What
  transfers is the INSTRUCTION MIX / relative hot spots — same
  backend sources, same optimization level, same A53 codegen target
  — and that is what the strategy rests on.
- The profile is E1M1 (moderately dense, 165M steps, 96 s of music).
  PCM share scales with active voice count; timer/dispatch/step costs
  are per-step constants. On sparser music PCM share falls and the
  timer share rises; on denser music (BELMO) PCM share rises further.
  The ranking (PCM > timers > dispatch) is robust across densities
  unless music is nearly silent (no voices -> PCM collapses to the
  mixing head only; timers then dominate — a degenerate case, not a
  music load).
- The wrapper (RenderFrames/ring/resample) is NOT in the harness
  profile (it needs Circle). It is bounded separately by hardware
  (<=10% non-emulator share) and is not a target.
- gprof numbers are samples (100 Hz, ~1350 samples, +/-1-2 pp on
  small entries); plugin numbers are EXACT counts (85.3B insns in
  the primary interval). Where they differ slightly (e.g. timer
  14.2% x86 vs 19.4% AArch64), codegen/inlining is the cause, and
  both are reported.
- No Pi kernel was built or delivered for Joe in this task
  (measurement only, per instructions). The clone's null-sink patch
  (mt32pi.cpp AudioTask, `QEMU_PROFILE_NULL_SINK`, plus the
  Kernel.mk conditional) and the snapshot plugin are clone/scratch
  only, never production. Production repo and pristine upstream
  Nuked-SC55 tree were not modified (upstream verified clean at
  66d1272 throughout; clone kernel8.img/elf for run B saved at
  /tmp/runB-kernel8.img/.elf, run A at /tmp/runA-kernel8.elf/.img).

## Scratch / artifacts from this investigation

- Plugin: /tmp/plugins/myhotblocks_snap.c, libmyhotblocks_snap.so
  (snapshot variant; stock myhotblocks.c/.so also there).
- User-mode QEMU 8.2.2 build: ~/workspace/tools/qemu-user-build/
  (qemu-aarch64; system build at ~/workspace/tools/qemu-build/
  untouched).
- Harness variants: /tmp/jit-prof/jit-test-prof (x86 gprof build,
  gmon.out in /tmp/jit-prof/), /tmp/jit-arm-g/jit-test-arm (AArch64
  + debug info, same .text addresses as the profiled binary).
- Snapshots: /tmp/plugin-snap-{1,2,3,4,5}.txt (user-mode run),
  /tmp/plugin-exit.txt (full run, 119.4B insns),
  /tmp/golden-e1m1-arm.txt (steps 164997213), /tmp/golden-e1m1-prof.txt.
- System-QEMU evidence: /tmp/plugin-snap-*.txt were OVERWRITTEN by the
  user-mode run (same filenames); the system-QEMU snapshots' content
  is recorded above (core-0 freeze + 0x30c spin) but the files no
  longer exist under those names. Run C serial log: /tmp/profC-serial.log;
  run C SD image: /tmp/sd-profC.img; null-sink kernel: /tmp/runB-kernel8.img.
- Analyzers: /tmp/analyze.py (system-QEMU format), /tmp/analyze_user.py
  (user-mode, function+line), /tmp/diff_snap.py (interval differentials),
  /tmp/mon_hist.py (abandoned monitor cross-check), /tmp/midi_send.py.
