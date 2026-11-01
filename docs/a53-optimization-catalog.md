# A53 Optimization Catalog — mt32-pi Nuked-SC55

**Target:** Pi Zero 2 W — Broadcom BCM2710A1 (same silicon as Pi 3), 4× Cortex-A53 @ 1 GHz, 512 MB LPDDR2, bare-metal Circle kernel.
**Workload:** Nuked-SC55 voice loop (`PCM_Update` slot loop, `pcm.cpp`), 48 kHz real-time audio, dual-core split (MCU on core 2, PCM on core 3), JV880-shape sync.
**Hard constraint:** bit-exactness. A candidate that changes audio output is FAILED, never tuned to fit goldens. Goldens: E1M1 164,997,213 steps / 164 checkpoints; BELMO 441,939,679 steps / 441 checkpoints; prefix = truncated E1M1, first 30 s + 3 s tail, 54,999,240 steps / 54 checkpoints. Gate: E1M1 ONLY since 2026-10-31 ~07:57 (BELMO dropped until 100% on one song; full E1M1 still required for engine-semantics claims).
**Standing rule:** QEMU grades correctness only. Hardware (shape-kernel pct + variant-kernel cycle counts) is the verdict for performance. Never present a model estimate as a Pi percentage.

## Baseline (what everything is measured against)

Step-4 harness attribution (QEMU plugin counts, truncated E1M1 prefix): total 40,831,173,616 insns (step-4 total 40,831,173,458; +158 negligible), host wall 55.451 s (TCG-caveated, never a grade). PCM 53.26% of insns (PCM_Update 43.21%, PCM_ReadROM 5.91%, calc_tv 4.04%); MCU 42.42% (TIMER_Clock 18.90%). Engine work drops 5.78% in the split; prototype sync overhead (22.54%) does NOT transfer to the JV880 shape (state lock only). Binding core on Pi is core 2 (98–99% step-loop wall) vs core 3 at 73–75%, with ~39 points unexplained by engine work — lock spins vs MCU-insn premium vs core-2 extras. Shape runs: 83–88% of real time with DMA zero-fill chop (~1.1 ms silence per ~6.4 ms chunk, ~156 Hz); sound is correct pitch/notes/resampler — a pure speed gap.
Sources: `hidden_files/optimization-20261031.md` §0–1, `hidden_files/hotblocks-step4-20261031.md`, MEMORY.md mt32-pi section.

## Status legend

- **TRIED** — built in the harness, gated, measured. Verdict follows.
- **QUEUED** — green but model-negative or otherwise waiting on a hardware run.
- **DESIGN ONLY** — designed, never implemented.
- **IN PROGRESS** — round 3 work, correctness gate pending.
- **AWAITING RUN** — built kernel, the hardware run pending.
- **NOT TRIED** — named in research, never attempted.
- **NOT APPLICABLE / DO NOT TRANSFER** — does not apply to this target or loop.

## Summary verdicts

### Part A — Microarchitecture facts (all RELEVANT context)

| # | Fact | Why it matters here |
|---|---|---|
| A1 | In-order, partial dual-issue | Scheduling, not width, is the core's real 2× lever |
| A2 | NEON latency ~2–3 cycles; chains stall | Voice chains are sequential; naive NEON stalls at full latency |
| A3 | No gather | Fetch head stays scalar; split loop on fetch/mix boundary |
| A4 | 64-bit load path / 128-bit store path | Loading vectors is load-bandwidth-bound before compute |
| A5 | Single AGU (1 load OR store / cycle) | Load-heavy loop is AGU-bound as much as ALU-bound |
| A6 | 128-bit NEON ops occupy both 64-bit halves | 64-bit vectors sometimes schedule better than 128-bit |
| A7 | Memory latency unhidden (L1 4 / L2 10–12 / DRAM ~150; mispredict 7–10) | ROM-fetch locality and prefetch matter more than ALU width |
| A8 | No public A53 timing doc; A55 guide is the prior | A55 tables = strong prior; A53 measurements arbitrate |
| A9 | Icache effects of code growth (unmodeled) | Pipe 1.9× / neon2 code growth could cost on hardware; model has no icache |

### Part B — FFmpeg/dav1d code tricks

| # | Trick | Relevance | Status |
|---|---|---|---|
| B1 | Same-register accumulation (forwarding path, ~40% on A53/A55/A520) | RELEVANT | In our code by construction (serial sum); NEON split form IN PROGRESS (round 3) |
| B2 | Multiple independent accumulators (chain-breaking) | RELEVANT (hypothesis) | NOT TRIED as scalar change; NEON split form IN PROGRESS (round 3) |
| B3 | Tree reduction vs serial chain | RELEVANT, BLOCKED by bit-exactness | NOT TRIED (would break the serial addclip20 sum) |
| B4 | Partial-load fix (LD1R replicate / zeroing; 3× in dav1d) | RELEVANT | TRIED in spirit: v1 INS/UMOV is the trap; v2 moved to full-width loads — still lost |
| B5 | Load-first vs defer-load | RELEVANT | TRIED: both orders measured (baseline vs pipe) — neither wins |
| B6 | Post-increment pointer dependencies | RELEVANT | TRIED (audit): NO-OP — zero post-increment loads in hot path |
| B7 | Hand-interleaved software pipelining | RELEVANT | TRIED (pipe pair split): GREEN but model-negative |
| B8 | Prefetch + layout | RELEVANT | DESIGN ONLY — shared-helper implementation queued |
| B9 | Runtime dispatch / per-CPU micro-kernels | NOT APPLICABLE | Single-core target; no dispatch needed |
| B10 | Don't copy x86 control flow | NOT APPLICABLE | Native AArch64 code; warning only |
| B11 | SVE | NOT APPLICABLE | No SVE on A53; NEON is the only vector path |

### Part C — Our-loop candidates

| # | Candidate | Gate | Grade | Verdict |
|---|---|---|---|---|
| C1 | NEON intrinsics | GREEN but vacuous (0 vector insns) | n/a | NOT A ROUTE — GCC scalarizes |
| C2 | NEON mix-tail v1, 64-bit asm | GREEN (prefix) | +1.68% insns | REJECTED (cost) — hardware verdict via variant 3 pending |
| C3 | NEON mix-tail v1, 128-bit asm | GREEN (prefix) | +1.68% insns | REJECTED (cost) — width insn-neutral |
| C4 | NEON mix-tail v2, full-width loads | GREEN (prefix) | +2.29% insns | DEPRIORITIZED, not disproven — variant 3 (neon2) settles it on hardware |
| C5 | Split-accumulator NEON | PENDING | — | IN PROGRESS (round 3, variant 4/5) — sx20 reordering risk |
| C6 | Restructured-layout NEON | PENDING | — | IN PROGRESS (round 3, variant 4/5) — removes per-tick packing |
| C7 | Lambda-pair scheduling (sched) | GREEN (prefix + full E1M1 + full BELMO) | Model +0.09% | QUEUED for HW A/B — variant 1 |
| C8 | Head/tail pair split (pipe) | GREEN (prefix + full E1M1) | Model +0.19% | QUEUED, low priority — variant 2 (sched outranks) |
| C9 | GCC `#pragma unroll 2` | n/a (no code change) | — | DEAD — unroller refuses the body |
| C10 | TIMER_Clock closed form | FAILED (2 prefix + 2 boot failures) | Caps ~1% even if fixed | DEAD |
| C11 | PCM_ReadROM inlining (item-4) | Measured safe | −0.76% | MEASURED, not yet in split (branch `feature/nuked-sc55-optimization`, `f8a0a2e`) |

### Part D — Methodology (all RELEVANT — they govern every number above)

| # | Method | Status |
|---|---|---|
| D1 | Measure-or-don't-submit (checkasm discipline) | ADOPTED — our verify + measure pairing mirrors it |
| D2 | PMU cycle tooling in the variant kernel (PMCCNTR_EL0, NOP-calibrated, boot-validated) | BEING BUILT — variant kernel in progress |
| D3 | PMU phase-2 event counters (instructions / L1D refill / mispredict → IPC + stall attribution) | PROPOSED, not built |
| D4 | QEMU insn-count limits (counts ≠ wall; no cache model; TCG distortion) | ESTABLISHED — correctness-only for NEON |
| D5 | llvm-mca A53 limits (NEON complaints reproduce; scalar parts pass) | ESTABLISHED — never the scheduling grade |
| D6 | Bit-exact gate discipline | ABSOLUTE — failing candidate is FAILED, goldens never tuned |
| D7 | Diag-kernel gating measurement (lock_us / mcu_us split) | AWAITING RUN — diag kernel built, run pending |

### Part E — Does not transfer

| # | Item | Why not |
|---|---|---|
| E1 | FFmpeg video block / streaming shapes | Our loop is short, branchy, per-voice stateful with a 128-tick batch bound — not a streaming block loop |
| E2 | dav1d/FIR headline numbers (3×, 3.37×, 40%) as predictions | They are priors and candidate shapes, not expected gains for our loop |

**Counts: 40 items total — 35 relevant (A9 + B8 + C11 + D7), 5 not relevant / do not transfer (B9–B11 + E1–E2).**

---

## Part A — Microarchitecture facts (Cortex-A53 @ 1 GHz, BCM2710A1)

### A1 — In-order, partial dual-issue — RELEVANT

**What it is.** A53 executes strictly in order and issues two instructions per cycle ONLY when they are independent and use different pipes. It cannot skip over a stalled instruction.
**Relevance.** This is the core's real 2× lever, and it is a scheduling lever, not a width lever. Badly ordered code stalls at full latency; well-ordered code wins big. Every scheduling candidate in Part C exists because of this fact.
**Tried.** Indirectly, via C7 (lambda-pair) and C8 (pipe pair split) — both attempts to create dual-issue pairs across slot boundaries.
**Outcome.** Both model-negative (C7 +0.09%, C8 +0.19% cycles). The fact itself stands: the lever is real, but our slot body is fragmented by switches and branches into small basic blocks, so pair structures do not create larger scheduling regions.
Source: `hidden_files/a53-execution-research-20261031.md` (Traps §1), `hidden_files/optimization-20261031.md` §3, `hidden_files/optimization-round2-20261031.md` §2.

### A2 — NEON latency ~2–3 cycles; dependency chains stall — RELEVANT

**What it is.** Only VEOR was observed at 1-cycle latency; typical NEON ops are ≥2 cycles, and fmul→fadd chains stall ~3 cycles.
**Relevance.** Per-voice envelope/phase/accumulator chains are sequential, so naive NEON vectorization of the voice loop stalls at full latency. Fix candidates: multiple independent accumulators, interleaved voices — helps scalar and NEON alike.
**Tried.** NEON mix-tail candidates C2–C4 keep the serial accumulation chain scalar and in order (deliberately); the chain-breaking alternative is round-3 item C5.
**Outcome.** The latency cost is part of why NEON mix-tail regressions would be worse on A53 wall time than the insn deltas suggest (NEON ops cost 2–3 cycles, and the volatile-asm blocks are barriers).
Source: research file (Traps §2), round-1 report §2.

### A3 — No gather — RELEVANT

**What it is.** NEON has no gather instruction. Gathering per-voice data from scattered addresses requires scalar loads + lane packing (INS/TBL).
**Relevance.** Our voice loop fetches wave ROM per voice by phase — random access, per-voice pointers. Vectorizing the fetch head requires scalar loads + lane packing, and that pack overhead typically eats the vector gain. Consequence, applied in every NEON candidate: the fetch/envelope head stays scalar; only the mix/accumulate tail (streaming, sequential, independent lanes) is NEON-shaped. Split the loop on that boundary; do not vectorize it whole.
**Tried.** All NEON candidates C2–C4 implement exactly this split (mix tail only, head scalar).
**Outcome.** Even the tail-only shape loses, because the tail's inputs are still scattered scalars that must be packed per slot-tick (see C4 lesson).
Source: research file (Traps §3, Consequences), round-1 report §2.

### A4 — 64-bit load path / 128-bit store path asymmetry — RELEVANT

**What it is.** `ldr q0` takes 2 cycles to issue and does not dual-issue; multi-register loads take 2 cycles per register. Stores are 1 cycle per register and can dual-issue with non-NEON work. (TRM-documented.)
**Relevance.** Loading voice data into vectors is load-bandwidth-bound even before compute. This is why the full-width-load shape (C4) was attempted — and why its failure is informative: even with full-width loads, the cost of building the vector to load dominates.
**Tried.** C4 (full-width `ldr q1`/`ldr q2`) vs C2–C3 (lane-insert construction).
**Outcome.** Full-width loads did NOT win: pack construction ≈ 20 insns vs 8 insns for lane inserts. The asymmetry matters, but it is not the binding constraint — pack construction is.
Source: research file (Traps §4), round-2 report §1.

### A5 — Single AGU: one load OR store per cycle — RELEVANT

**What it is.** No dual-issue of load+store, or of two 64-bit stores. One load/store unit total.
**Relevance.** The voice loop is load-heavy (ROM fetch + voice state + envelope tables), so it is AGU-bound as much as ALU-bound. Any candidate that adds memory traffic (PipeCtx stack round-trips in C8, spill/reload traffic around volatile-asm blocks in C2–C4) pays directly against this single unit.
**Tried.** Modeled in the A53 cycle model used for C7/C8 (1 LS/cycle); the C8 mechanism analysis cites PipeCtx stack traffic as pure added cost against this constraint.
**Outcome.** C8 stall cycles actually fell (−12,472) but total cycles rose — added instructions cost more dual-issue slots than the reorder saves, consistent with AGU pressure from the added stack traffic.
Source: research file (Traps §5), round-2 report §2.

### A6 — 128-bit NEON ops occupy both 64-bit halves — RELEVANT

**What it is.** A full-width NEON op blocks all other NEON work that cycle; two 64-bit-half ops on independent registers CAN dual-issue. Counterintuitive consequence: 64-bit vectors (4×16-bit samples) sometimes schedule better than 128-bit vectors (8×16-bit).
**Relevance.** Both widths must be tested, not assumed — which is what round 1 did.
**Tried.** C2 (64-bit, 4× `mul v0.2s`) vs C3 (128-bit, 3× `mul v0.4s`).
**Outcome.** Width is insn-neutral here: totals differ by 11 insns (41,518,178,621 vs 41,518,178,610, both +1.68%). The bottleneck is packing + barriers, not vector width.
Source: research file (Traps §6), round-1 report §2.

### A7 — Memory latency cannot be hidden (no out-of-order) — RELEVANT

**What it is.** L1 hit 4 cycles, L2 10–12, DRAM ~150 cycles; branch mispredict ~7–10 cycles. An in-order core has nothing else to run during a miss.
**Relevance.** Wave ROM tables are megabytes, so a voice fetch missing the caches stalls the core for ~150 cycles. ROM-fetch locality matters more than ALU width. Software prefetch of the next tick's ROM data, issued early, is a real lever precisely because it does not depend on dual-issue at all.
**Tried.** Not directly — prefetch is B8 (design only). The counter-consideration (per-voice ROM reads advance slowly, so the current region is overwhelmingly L1-resident at ~4-cycle exposed latency, not DRAM ~150) is why B8's expected hardware gain is small.
**Outcome.** Unmeasured. The latency numbers themselves come from AsiaBSDcon 2026 notes and A53 microbenchmark folklore, not from Pi measurements — Pi-specific cache config is unknown (see Unmapped below).
Source: research file (Traps §7), round-2 report §3.

### A8 — No public A53 timing doc; the A55 guide is the prior — RELEVANT (context)

**What it is.** ARM never published an A53 Software Optimization Guide (NDA only); the A53-specific spreadsheet is still not public. The public transferable source is ARM's Cortex-A55 Software Optimization Guide (ARM-EPM-128372, Non-Confidential since issue 2.0, Jan 2018; issue 4.0, Aug 2022, r2p0, 48 pp.).
**Relevance.** The A55 retains most of the A53 backend (same 2-wide in-order formula, 8-stage integer pipeline, inherited backend, robust dual-issue), so A55 tables are the closest thing to an official A53 timing doc — but transfer caveats are measured, not assumed: SMLAL.int8 + 64-bit load = 0.97 IPC on A53 vs 1.72 on A55; A55 has a predecoded I-cache and perceptron predictor vs A53's GShare, so branch/frontend behavior does not transfer; the A55 guide even contradicts itself on SMLAL dual-issue slots. Rule: A55 tables = strong prior, A53 measurements arbitrate conflicts.
**Tried.** Used as background for the hand-built A53 cycle model (`a53sim.py`: in-order dual-issue, 2/cycle, 1 LS/cycle, ALU 1, mul 3, NEON 3, load→use 4 L1, mem RAW exact-addr +1, branches consume a slot, no mispredict penalty). The model itself was built from microbenchmark timings, NOT directly from A55 tables — the tables remain an unused prior (see Unmapped).
**Outcome.** Ranking-grade estimates only (C7 +0.09%, C8 +0.19%); hardware A/B is ground truth.
Source: research file (App. 2, "has understanding improved?"), round-1 NOTES.md (model description).

### A9 — Instruction-cache effects of code growth — RELEVANT, UNMEASURED

**What it is.** Larger code footprints can cost instruction-cache misses. The A53 cycle model has no icache; QEMU/TCG has no cache model either.
**Relevance.** Pipe grows PCM_Update 0x2584 → 0x4ab0 (~1.9×); neon2 grows it to 0x2640; sched grows it to 0x37b4 (+0x1230). None of these growth costs appear in any harness number. A hardware run could differ from the model adversely for this reason alone.
**Tried.** Nothing — no icache measurement exists in the harness or in any kernel.
**Outcome.** NOT MEASURED. Flagged as an icache consideration in both round reports; only the variant-kernel hardware run can show it.
Source: round-1 report §3, round-2 report §2 and "What the harness cannot measure".

---

## Part B — FFmpeg/dav1d code tricks (VDD26, Martin Storsjö + commit history)

Source for all of Part B: research file Appendix 3 (VideoLAN Developer Days 2026, Rome, Oct 18; talk report: Tokyo Video Tech, Medium, Part 3) and Appendix 2 (FFmpeg finding, Oct 2026).

### B1 — Same-register accumulation (A53/A55/A520 forwarding path) — RELEVANT

**What it is.** These cores have a dedicated forwarding path passing the previous accumulation result straight to the next instruction. Accumulating into the SAME register is ~40% faster on A53/A55/A520 than alternating registers (at ~1% cost on out-of-order cores). A510 LACKS the path — irrelevant here, since our target is exactly one core.
**Relevance.** This reverses the naive chain-breaking instinct. Our code ALREADY accumulates serially into `ram1[31][1]/[3]` and `rcsum` (single-accumulator shape by construction). The open question was whether a revived NEON mix-tail should keep one accumulator or split it — round 2 kept one (per the brief); the split form is round-3 item C5. Caveat from the research: the 40% is an FIR/scalar-accumulation finding; whether it extends to NEON fadd mix accumulators is UNMEASURED — test both styles on hardware.
**Tried.** Single-accumulator shape used in C4 (round-2 NEON v2, per brief). The split-accumulator NEON variant was NOT attempted in round 2 (serial sum preserved by design).
**Outcome.** C4 lost for unrelated reasons (pack cost, +2.29% insns) — the accumulator-shape question itself is still open and is exactly what C5 tests.
Source: research file (App. 3, trick 1; App. 2, consequence), round-2 report §1.

### B2 — Multiple independent accumulators (chain-breaking) — RELEVANT (hypothesis)

**What it is.** Breaking a serial accumulation chain across two or more alternating registers so independent accumulations can dual-issue / hide latency. Standard advice on most cores.
**Relevance.** Plausible in general, but B1's forwarding path means it may LOSE on A53 for accumulation chains — do NOT assume multi-accumulator wins (contra the naive instinct). For our loop the serial chain is also bit-exactness-constrained (see B3), so a scalar multi-accumulator rewrite was never attempted; the NEON split-accumulator form is round-3 item C5.
**Tried.** Not tried as a scalar change. NEON split-accumulator form: IN PROGRESS (round 3).
**Outcome.** NOT MEASURED in any form yet. The two styles must be measured on the Pi, not assumed.
Source: research file (Traps §2, App. 3 trick 1 + mapping §3).

### B3 — Tree reduction vs serial chain — RELEVANT, BLOCKED by bit-exactness

**What it is.** Adding x0..x7 serially is a 7-deep chain; a tree reduction is 3 steps — on out-of-order cores. On A53/A55 the forwarding path (B1) reverses this preference.
**Relevance.** Our mix sum MUST stay serial: the serial addclip20 sum into `ram1[31]`/`rcsum` is order-sensitive because intermediate sums wrap when re-read through `sx20` — reordering partial sums is NOT semantics-preserving in general. A tree reduction would change audio output and fail the bit-exact gate. The safe shape (used in C8) is interleaved FETCH + per-voice work with the serial sum kept in order.
**Tried.** Not tried — blocked by construction, not by measurement.
**Outcome.** NOT TRIED, and not recommended: any tree form would have to pass the bit-exact gate first, and the gate is the only thing that could save it.
Source: research file (App. 3, tree-reduction note + mapping §2), round-1 report §2, round-2 report §2.

### B4 — Partial-load fix: LD1R replicate / zeroing (3× in dav1d) — RELEVANT

**What it is.** Where four bytes suffice, a load into PART of a 16-byte register leaves the rest unchanged, creating an implicit dependency on whatever the previous iteration wrote there. Replacing it with a load that REPLICATES the four bytes across the register (LD1R) or one that ZEROES the remainder lets the next iteration start immediately. One-instruction change: 3× faster in dav1d (combined with trick B5: 3.37×).
**Relevance.** Our rejected NEON mix-tail used INS (pack) + UMOV (unpack) per bundle — exactly the dependency-creating lane operations this trick avoids. Any future NEON attempt must prefer full-width loads over lane inserts and keep the accumulate in full registers. Scalar GPR note: AArch64 W-register writes zero-extend, so scalar code has no equivalent partial-register trap — this trap is NEON-specific. Core attribution caveat: the 3× is NOT pinned to A53 specifically in the talk report — do not quote it as an A53 number (see E2).
**Tried.** C2–C3 (v1, INS/UMOV construction) and C4 (v2, full-width `ldr q1`/`ldr q2` construction — the trick applied).
**Outcome.** The trick did NOT save the shape: v2 was WORSE than v1 (+2.29% vs +1.68% insns). Lesson (round-2 report): the full-width-load trick pays only when data is ALREADY in memory — for computed scalars, building a memory vector to load it back is strictly more work than inserting lanes. The trap diagnosis was right; the remedy's precondition (data already in memory) is what our loop violates — which is why C6 (restructured layout) exists.
Source: research file (App. 3, trick 3 + mapping §1), round-2 report §1.

### B5 — Load-first vs defer-load — RELEVANT

**What it is.** dav1d 8-pixel function: the old code started from a value already at hand and deferred the slow load; Arm's suggestion was the opposite (start the slow load FIRST, insert another instruction, then use the loaded value). Result: a few % SLOWER on efficiency cores, 9–13% faster on large cores.
**Relevance.** For an A53-only target the direction inverts: deferring the load (the old shape) is the better bet — but the real lesson is that the optimal schedule differs by core class, so both orders must be measured on the Pi, not assumed. FFmpeg doctrine generally: "issue independent loads first, then line up the calculations that use them"; "in simple functions, unrolling the loop is needed to give the CPU more independent data."
**Tried.** Both orders measured in our loop: the original order (tail(s) before head(s+1)) IS the baseline (4,286,700 model cycles); the pair order (head(s+1) before tail(s)) is C8 (4,294,669).
**Outcome.** Neither wins (+0.19% for the pair order). There is no third order to test: an all-heads-before-all-tails shape would break IRQ order (heads must stay in slot order, which the pair shape preserves) and would still not lengthen any load→use distance.
Source: research file (App. 3, trick 2 + mapping §2), round-2 report §2.

### B6 — Post-increment pointer dependencies — RELEVANT

**What it is.** A load with post-increment makes the next instruction using the advanced pointer wait, and so must calculations using the loaded data. Remedies: separate base registers for even/odd rows with doubled stride, or alternate x2 vs x2+x3 and advance x2 at the end (more instructions, faster on large cores).
**Relevance.** The voice loop advances per-voice phase pointers; a post-increment load in the hot loop would make the next pointer use wait. Advance at loop end or use even/odd base registers.
**Tried.** Round-2 audit (D): baseline `PCM_Update` disassembly examined.
**Outcome.** NO-OP — exactly one post-index instruction exists (function-epilogue `ldp x29, x30, [sp], #352`) and one pre-index (prologue `stp x29, x30, [sp, #-352]!`); ZERO post-increment loads in the hot path. GCC already avoids them here. No variant built, nothing to grade. DEAD as a direction (not a failure — the audit found the code already in the desired shape).
Source: round-2 report §4, round-2 NOTES.md (D section).

### B7 — Hand-interleaved software pipelining — RELEVANT

**What it is.** Hand-written asm that interleaves two independent streams: fetch voice N+1's ROM data while mixing voice N. FFmpeg's doctrine applied: insert independent work between a load and its first use; "code with broken dependency chains runs faster than code that looks tidy"; macros reduce scheduling freedom, so interleave separate calculations with comments instead.
**Relevance.** Ranked in the research as the STRONGEST untested candidate class for the voice loop — because the round-1 sched candidate failed only in that GCC fragments basic blocks and the slot body is switch-heavy, while hand interleaving changes order across voices (legal here: voices are independent until the final mix sum, and the sum order must be preserved). This is NOT the same as the lambda-pair experiment (which kept execution order identical).
**Tried.** C8 (round-2 pipe pair split): each pair of slots runs head(s), head(s+1), tail(s), tail(s+1); head = per-slot setup → address gen → 4 ROM fetches → DPCM → interpolation → filter → IRQ check; tail = calc_tv ×3 → mix tail → sums → key update → !active cleanup. Serial addclip20 sum kept in order; 7-field PipeCtx (v3, usenew, newnibble, old_nibble, key, kon, active); surgery script `cand-pipe/split-loop.py` (mechanical, assertion-checked); loop at `pcm.cpp:1105`, head lines 1107..1458, tail 1459..1577.
**Outcome.** GREEN (prefix + full E1M1, 164,997,213 steps, 164 checkpoints, 0 failures) but model +0.19% cycles (4,294,669 vs 4,286,700; CPI 0.8589 vs 0.8573; stall −12,472 but total up) — NO MODELED GAIN, slightly negative, and mechanism-negative: load→use distances WITHIN each head are unchanged by the split (each head's fetches are still immediately followed by its own DPCM/interp/filter uses); calc_tv calls and IRQ/filter branches block GCC's scheduler from moving work across the new boundaries; on an in-order core, reordering alone creates no overlap — only longer load→use distances do, and the split does not lengthen any. What it adds is cost: PipeCtx zero-init + stack round-trips + pair-loop control, plus ~1.9× code growth (icache note, A9). QUEUED for hardware A/B ONLY as a low-priority check (variant 2); round-1 sched (+0.09%, smaller code growth) outranks it. A deeper 3–4 voice interleave was NOT attempted; the same mechanism analysis applies a fortiori, but that is unmeasured (see Unmapped).
Source: research file (App. 3, doctrine + mapping §2), round-2 report §2, round-2 NOTES.md (B section).

### B8 — Prefetch + layout — RELEVANT

**What it is.** On an in-order core a cache miss (~150 cycles DRAM) has nothing to hide behind, so prefetching next tick's ROM data early, issued ahead of use, is the one scheduling lever that does not depend on dual-issue at all. (OpenBLAS lesson, already in our plan; wave ROM tables are MBs.)
**Relevance.** Real lever in principle; small expected gain in practice for our loop (see below).
**Tried.** DESIGN ONLY — never implemented, for three stated reasons: (1) the only non-no-op prefetch points (cross-slot samp0, cross-tick samp0) require computing a HOST pointer from a ROM address, and PCM_ReadROM (`pcm.cpp:33`) maps ROM→host through a ~30-line bank/mode switch (config_reg_3d bit 0x20 selects the bank shift; MK1/JV880 variants per bank; waverom1/2/3/card/exp) — a prefetch would have to replicate that switch, a duplicated mapping that must track future ReadROM changes, for a hint; (2) the harness CANNOT measure any cache effect (TCG has no cache model), so an implementation would be gradeable only as "still bit-exact + added insn cost" — which proves nothing about the gain; (3) expected hardware gain is small anyway: per-voice ROM reads advance slowly through wave data, so the current region is overwhelmingly L1-resident (~4-cycle exposed latency, not DRAM ~150). The in-loop alternative (prefetch the current slot's fetches right before their calls) is a no-op: prefetch→load distance ~0.
**Outcome.** DESIGN ONLY, queued as a hardware-design note. Clean implementation if a future HW A/B wants it: cross-tick samp0 prefetch at tick end — next tick's samp0 address is computable then ((hiaddr<<20)|ram1[slot][4]; hiaddr = ram2[7] bits [11:8], untouched by the key update which only touches bits 15:12 and 5). Implement via a SHARED address-translation helper (not a duplicated switch); estimated cost ~5–8 insns/slot-tick; ~30-line change + prefix verify. Bit-exactness unaffected by construction (prfm never faults, never changes results). NOT in the variant kernel (prefetch excluded from variants by design).
Source: research file (App. 3, mapping §5), round-2 report §3, round-2 NOTES.md (C section).

### B9 — Runtime dispatch / per-CPU micro-kernels — NOT APPLICABLE

**What it is.** dav1d detects NEON/DotProd/I8MM/SVE via getauxval(AT_HWCAP) and dispatches to hand-written GAS assembly with size-specific kernels; FFmpeg does the same via av_get_cpu_flags; OpenBLAS uses per-CPU micro-kernels (with optimized CGEMM/ZGEMM kernels specifically for Cortex-A53/A55).
**Relevance.** NONE for our target. We tune for exactly one core (A53 @ 1 GHz in the Pi Zero 2 W) — there is nothing to dispatch between, and A510/out-of-order costs are irrelevant to us. This is the meta-clue in reverse: FFmpeg balances across cores and accepts per-core regressions; we get to be more single-minded than FFmpeg.
**Tried.** Nothing — correctly nothing.
**Outcome.** NOT APPLICABLE. Recorded here so the catalog is complete and the idea is not re-proposed.
Source: research file (App. 3, FFmpeg working method + mapping §6).

### B10 — Don't copy x86 control flow — NOT APPLICABLE

**What it is.** x86 addressing idioms (e.g. indexing from the end of a range) differ from ARM64's. FFmpeg advice: "to learn idiomatic NEON, read small NEON functions in FFmpeg."
**Relevance.** NONE as a change: our code is native AArch64 C plus hand-written AArch64 asm, not ported x86 flow. Recorded as a warning only — if a future candidate ports an x86-shaped loop, re-check its addressing idioms.
**Tried.** Nothing.
**Outcome.** NOT APPLICABLE.
Source: research file (App. 3, doctrines).

### B11 — SVE — NOT APPLICABLE

**What it is.** ARM's Scalable Vector Extension (variable-length vectors). Until very recently SVE2 was 128-bit-only; variable length gives up unrolling.
**Relevance.** NONE: A53 has no SVE. NEON is the only vector path. Moot for this core, confirmed by the research.
**Tried.** Nothing.
**Outcome.** NOT APPLICABLE.
Source: research file (App. 3, doctrines).

---

## Part C — Our-loop candidates (built and measured in the QEMU harness)

Harness: `~/workspace/tools/qemu-user-build/qemu-aarch64` (8.2.2); plugin output to QEMU log (`-d plugin -D <file>`). All insn deltas vs round-1 `cand-base` (pristine), prefix 40,831,173,616 insns / 55.451 s host wall. Host wall is TCG-distorted and is never a grade for scheduling; for count-changing candidates it is a second signal only. QEMU insn deltas OVERSTATE NEON wall-time gains (a 128-bit NEON op counts as 1 insn regardless of A53 latency/AGU/dual-issue) — and here, understate the regression, since NEON ops cost 2–3 cycles and volatile-asm barriers block dual-issue scheduling.

### C1 — NEON intrinsics — TRIED, NOT A ROUTE

**What it is.** Volume pairs + pan/rc quad written with NEON intrinsics, both widths.
**Relevance.** Was the natural first NEON attempt; would have been the maintainable route if the toolchain cooperated.
**Tried.** Round 1: intrinsics implementation, both widths (`opt-20261031/NOTES.md` § "NEON: intrinsics scalarized").
**Outcome.** Bit-exact GREEN (0 failures both) BUT VACUOUS — GCC 11.3.1 at `-Ofast -mcpu=cortex-a53` scalarizes the intrinsics outright (verified in isolation: 0 vector insns; standalone helper also 0). Intrinsics binaries kept (neon64 `64ac1f67…`, neon128 `29553d85…`) but superseded by the asm builds. Future NEON work needs asm (or different flags — the flags alternative was named but never pursued; see Unmapped).
Source: round-1 report §2, round-1 NOTES.md.

### C2 — NEON mix-tail v1, 64-bit asm — TRIED, REJECTED (cost)

**What it is.** Vectorize the per-slot `multi()` bundles of the mix tail ONLY — volume pairs (multiv1/2 from `sample`, multiv3/4 from `sample2`) and the pan/reverb quad (sampl/sampr/rc0/rc1 from `sample3`) — with hand-written volatile asm, 64-bit vectors (4× `mul v0.2s`: vol ×2 + pan ×2). Fetch/envelope head stays scalar (A3). Serial accumulation untouched and in order. `multi(a,b) = sx20(a) * b` with int32 wrap maps exactly to SHL/SSHR #12 + MUL low 32 bits.
**Relevance.** The direct test of "is the mix tail NEON-shaped?" at the width A6 says sometimes schedules better.
**Tried.** Round 1, `opt-20261031/cand-neon64/tree/src/backend/pcm.cpp` (width selected by `-DNEON_MIX_TAIL_WIDTH`). Vector code confirmed present in the binary (not just the source — the C1 lesson).
**Outcome.** Bit-exact GREEN on prefix (0 failures) but a regression: 41,518,178,621 insns (+1.68%), PCM_Update 18,328,871,411 (+3.89%), host wall 57.623 s (+3.9%). Why: +27 insns/slot-tick — pack/unpack overhead (dup + ins×2/4 + umov×2/4 + shl + sshr ≈ 8–12 asm insns per bundle) exceeds the 2–4 scalar multis replaced, AND volatile asm is a scheduling barrier (forces multiv values through stack slots instead of registers, blocks dual-issue scheduling across blocks). REJECTED as a performance direction. Full-song runs not done (round-1 precedent: a candidate rejected on prefix does not need a full run). Hardware verdict for the NEON direction as a whole now rides on C4 as variant 3.
Source: round-1 report §2, round-1 NOTES.md.

### C3 — NEON mix-tail v1, 128-bit asm — TRIED, REJECTED (cost, width-neutral)

**What it is.** Same call sites and bundles as C2, 128-bit vectors (3× `mul v0.4s`: vol ×2 + pan ×1).
**Relevance.** The A6 width question, tested directly against C2.
**Tried.** Round 1, `opt-20261031/cand-neon128/tree/src/backend/pcm.cpp`.
**Outcome.** Bit-exact GREEN on prefix (0 failures), 41,518,178,610 insns (+1.68%) — differing from C2 by 11 insns total; PCM_Update identical 18,328,871,411. Host wall 54.390 s (−1.9%) is TCG distortion, NOT evidence (it proves the distortion: insns up, wall down). Width is insn-neutral here — the bottleneck is packing + barriers, not vector width. REJECTED with C2.
Source: round-1 report §2.

### C4 — NEON mix-tail v2, full-width loads, zero INS — TRIED, DEPRIORITIZED (not disproven)

**What it is.** Single asm block per slot-tick for the whole mix-tail multi chain, zero INS. Coefficient vectors built by scalar C code into a 16-byte-aligned stack array (`neon_pack[4]`, pairs packed as `cA | cB<<32` etc.) and loaded with full-width `ldr q1`/`ldr q2` (vol pair in q1 low/high halves, pan/rc pair in q2); multiplicands DUP'd full-width (`.4s`); serial addclip20 chain including the cin rounding bit runs as scalar instructions inside the block (sx20(x>>k) via SBFX: `sbfx #6,#20` for sx20(m>>6), `sbfx #13,#19` for sx20(m>>13) — the latter disassembles as the `asr` alias, which is correct, not a bug); only sampl/sampr/rc0raw/rc1raw leave via UMOV (unavoidable scalar interface to the serial sum chain, which stays in order). Single-accumulator (B1), per the round-2 brief.
**Relevance.** The B4 remedy applied: full-width loads instead of lane inserts, plus the v1 barrier lesson kept in view. The question it answers is whether the pack cost was an artifact of the INS construction or structural to the loop.
**Tried.** Round 2, `opt-round2-20261031/cand-neon2/tree/src/backend/pcm.cpp` (mix region, `#if defined(NEON_MIX_TAIL_V2)`). Binary md5 `4a5fa16ca8fe3c13e778b4d384e420d6`; PCM_Update 0x2584 → 0x2640; new code confirmed in disassembly (`ldr q1`/`ldr q2`, `dup v0.4s`, w17/w18 sample2/sample3 adds).
**Outcome.** Bit-exact GREEN on prefix (54,999,240 steps, 54 checkpoints, 0 failures); full E1M1 not run (rejected-on-prefix precedent). Numbers: 41,764,790,169 insns vs baseline 40,831,173,616 = +933,616,553 (+2.29%); PCM_Update +933,616,539 (+5.29%); ≈ +36.8 insns/slot-tick; host wall 57.619 s (+3.9%). WORSE than v1 (+1.68%). Why (from disassembly): pack construction ≈ 20 insns (extracts + ubfiz/orr + stp/str + address computation + 2× ldr q); the block itself is 38 insns vs ≈ 37 for the scalar region it replaces (roughly a wash); plus `memory`-clobber spill/reload traffic around the block. Lesson: INS was the cheaper construction (8 insns for all coefficient vectors vs ~20 for pack+load). The FFmpeg full-width-load trick pays only when data is ALREADY in memory — for computed scalars, building a memory vector to load it back is strictly more work than inserting lanes. Status correction (2026-10-31, the challenge): the round-2 report said "CLOSED"/"DEAD" and "do NOT build a NEON hardware kernel from either" — that overstated the evidence, since the rejection rested on net insn increase + packing mechanism, NOT correctness (both shapes bit-exact green), and hardware remains the verdict. Corrected status: DEPRIORITIZED, not disproven. Hardware verdict: variant 3 ("neon2") in the auto-sequence variant kernel — if its cycle totals and real-time rate beat baseline, the rejection is overturned by measurement.
Source: round-2 report §1, MEMORY.md (correction 2026-10-31).

### C5 — Split-accumulator NEON — IN PROGRESS (round 3, variant 4/5)

**What it is.** The NEON mix tail with the serial accumulation split across two (or more) independent accumulators, combined at the end — the B2 shape applied to the NEON tail, as opposed to C4's single accumulator (B1).
**Relevance.** Directly tests B1 vs B2 on hardware: does the A53 forwarding path (same-register wins) or the chain-breaking instinct (split wins) govern OUR mix accumulators? The research is explicit that whether the FIR finding extends to NEON fadd mix accumulators is UNMEASURED.
**Tried.** NOT attempted in rounds 1–2 (round 2 preserved the serial sum by design). Authorized 2026-10-31 ~09:39 as a round-3 variant.
**Outcome.** PENDING harness bit-exact gate — full E1M1 green required for inclusion in the variant kernel (variants 4/5). RISK, stated plainly: reordering intermediate sums through sx20 is not bit-exact in general (an intermediate sum exceeding 20 bits wraps when re-read through sx20) — the gate is the only thing that can save it, and a split form must pass the gate first. If it fails the gate, it is FAILED, not tuned.
Source: round-2 report §1 (split-accumulator note), MEMORY.md (round-3 authorization 2026-10-31).

### C6 — Restructured-layout NEON — IN PROGRESS (round 3, variant 4/5)

**What it is.** Restructure the mix so vectors do not need per-tick packing at all — keeping voice coefficients in vector form across ticks, or processing voices in vector lanes, instead of packing scattered scalars into a vector on every slot-tick and unpacking results after.
**Relevance.** This is the only direction that removes the packing cost rather than relocating it: C2 packed in registers (INS, 8 insns), C4 packed in memory (pack+load, ~20 insns) — both lost because the loop's inputs are scattered scalars. A layout change attacks the cause (data layout), not the construction method. It is algorithmic surgery on the mix, not an asm tweak, and it is bigger than any round-1/2 change.
**Tried.** NOT attempted in rounds 1–2. Authorized 2026-10-31 ~09:39 as a round-3 variant.
**Outcome.** PENDING harness bit-exact gate — full E1M1 green required for inclusion in the variant kernel (variants 4/5). No numbers exist yet; no gain is claimed.
Source: MEMORY.md (round-3 authorization 2026-10-31); parent analysis 2026-10-31 ~09:35 (packing-cost diagnosis).

### C7 — Lambda-pair scheduling (sched) — TRIED, QUEUED for HW A/B (variant 1)

**What it is.** The slot loop body (~465 lines, `pcm.cpp` slot loop) executed as an `always_inline` lambda called twice per pair iteration (`process_slot(s)`, `process_slot(s+1)`, odd remainder handled), so the compiler can schedule the second slot's independent head (ROM fetch, envelope) across the first slot's serial mix tail. Execution ORDER is identical (s, s+1, …), so all shared-state updates (ram1[31] mix bus, rcsum, slot2 cross-writes) keep their original sequence. No `return`/`goto` in the body; all `break`s are inside switches.
**Relevance.** The scalar-scheduling candidate the A1 fact predicts: create dual-issue pairs without changing order or semantics.
**Tried.** Round 1, `opt-20261031/cand-sched/tree/src/backend/pcm.cpp`. Dead end first: `#pragma GCC unroll 2` has NO effect (PCM_Update byte-identical, 0x2584) even with `--param max-unrolled-insns=3000` and `-funroll-loops` — the lambda-pair restructure was the replacement that actually changes codegen (PCM_Update 0x2584 → 0x37b4 (+0x1230), no `process_slot` symbol, fully inlined).
**Outcome.** Bit-exact GREEN on prefix AND full E1M1 (164,997,213 steps, 164 checkpoints, 0 failures) AND full BELMO (441,939,679 steps, 441 checkpoints, 0 failures) — full engine-semantics success. Grade (ordered-trace + A53 cycle model ONLY, never insn delta or host wall for scheduling): baseline window 5,000,000 insns → 4,286,700 cycles, CPI 0.8573; sched window → 4,290,718 cycles, CPI 0.8581; delta +4,018 cycles (+0.09%) — no gain, within model noise. Why no gain (reasoned argument): GCC's scheduler reorders WITHIN basic blocks, and the slot body is fragmented by switches (DPCM, filter, MK1/JV880 paths) and branches into many small basic blocks — the pair structure does not create larger scheduling regions; the accumulator chain serializes slot tails regardless; the stalls the model does see (~38% of cycles) are load-use chains with limited independent work in the same basic block to fill the latency. QUEUED for hardware A/B (variant 1 in the auto-sequence kernel): shape-kernel pct is the ground truth — the model assumes L1 hits and no icache effects, and a real hardware run could differ (including adversely, if the larger code hurts icache). Do not present +0.09% as a Pi percentage; it is a ranking-grade estimate that says "no modeled gain." Still the best scheduling candidate (outranks C8 on model and code growth).
Source: round-1 report §3, round-1 NOTES.md (sched + model tooling sections).

### C8 — Head/tail pair split, hand-interleaved software pipelining (pipe) — TRIED, QUEUED low-priority (variant 2)

**What it is.** See B7 for the full description: head(s), head(s+1), tail(s), tail(s+1) per pair; serial sum kept in order; 7-field PipeCtx.
**Relevance.** The strongest untested candidate class per the research (B7), now tested.
**Tried.** Round 2, `opt-round2-20261031/cand-pipe/` (fresh copy of round-1 cand-base, NOT built on neon2 — measured against the pristine baseline). Binary md5 `42f50ee4e9f89f9d0abb0753875eea9c`; PCM_Update 0x4ecff0 size 0x4ab0 (baseline 0x2584 — ~1.9× code growth, icache note A9).
**Outcome.** Bit-exact GREEN on prefix + full E1M1 (164,997,213 steps, 164 checkpoints, 0 failures) — the reorder is bit-exact, as the safety case predicted. Model: 4,294,669 cycles vs baseline 4,286,700 = +7,969 (+0.19%), CPI 0.8589 vs 0.8573 — NO MODELED GAIN, slightly negative, and mechanism-negative (B7). QUEUED for hardware A/B ONLY as a low-priority check (variant 2); the model is ranking-grade and hardware is ground truth, so a hardware run could in principle overturn it — but the mechanism analysis independently predicts no gain, the code grew ~1.9×, and there is nothing for hardware to find that the model + mechanism haven't already excluded. If a future HW A/B sweep wants a scheduling candidate, C7 outranks this one.
Source: round-2 report §2, round-2 NOTES.md (B section).

### C9 — GCC `#pragma GCC unroll 2` on the slot loop — TRIED, DEAD

**What it is.** Asking the compiler to unroll the slot loop by 2 (the cheap route to the C7 pair structure).
**Relevance.** Would have given C7's shape for free if the compiler cooperated.
**Tried.** Round 1, on the ~465-line slot loop body.
**Outcome.** NO effect: PCM_Update object byte-identical to pristine (0x2584), even with `--param max-unrolled-insns=3000` and `-funroll-loops` (with `-funroll-loops` only a small inner loop grew, 0x2584 → 0x268c, not the slot loop). DEAD — the lambda-pair restructure (C7) was the replacement.
Source: round-1 report §3, round-1 NOTES.md.

### C10 — TIMER_Clock closed form — TRIED, FAILED (correctness) + caps ~1%

**What it is.** Event-driven skip inside `TIMER_Clock` (`mcu_timer.cpp`, function only). A tick that is neither a compare-match nor a counter wrap only increments the counter, so a run of event-free ticks is skipped arithmetically and only the event tick runs the original `ClockFrt`/`ClockTmr` body (flag, CCLRA-clear, overflow, interrupt, deadline semantics identical by construction; wrap excluded since rem ≤ d_wrap keeps the counter ≤ 0xffff/0xff). Original bodies untouched; ONLY `mcu_timer.cpp` differs from the backup tree.
**Relevance.** TIMER_Clock is 18.90% of baseline insns (step-4 attribution) — the largest single MCU-side block, and the step-4 assumption was a −50% cut worth −9.5% total. If core 2's gap turns out to be MCU work (D7 decision rule), the MCU side is where the fix must live.
**Tried.** Round 1, `opt-20261031/cand-timer/tree/src/backend/mcu_timer.cpp` (TIMER_Clock + TicksToTarget helper).
**Outcome.** FAILED bit-exactness: prefix 2 failures (checkpoint 0 @ step 1,000,000 + final state); boot 2 failures (checkpoint 22 @ step 575,000 + final state, of 400 checkpoints). Control (pristine tree) verifies 0 failures on boot, so the divergence IS the timer change, not tree drift. HashState covers only r/pc/sr/sram/ram/dev_register (timer internals NOT hashed) and audio matched, so the divergence is a TRANSIENT firmware-execution phase difference — an interrupt taken at a slightly different step (state differs at the checkpoint instant, reconverges by the next checkpoint). The closed form shifts SOME timer event by ≥1 step somewhere before step 575,000; the tick-count/deadline/flag logic was re-proven correct on paper, so the shift mechanism is NOT yet identified. Diagnostic insn result only (a FAILED candidate is not ranked): total −1.04%, TIMER_Clock −4.64% (7,358,746,020 vs 7,717,003,576). Why so small (vs the assumed −50%): rem is almost always 1–3 ticks (target advances 6/call, strides ≥ 4), so the per-call distance math (~120 insns ×3 FRTs + TMR, every one of 55M calls) costs MORE than the 1–3 tick bodies it skips in the common case; the net −4.6% comes only from rarer large-rem windows. Even if the bit-exactness bug were fixed, this shape caps at ~1% total — TIMER_Clock is NOT the easy 9.5% win. A future timer attempt would need near-zero per-call overhead (e.g. a cheaper event test) or a different structure, and must first pass the gate via the checkpoint-dump debug path (dump all timer fields at every checkpoint on boot for baseline + timer, diff, localize the first differing checkpoint, then step-window dump around it). DEAD in current shape; do not revive without fixing bit-exactness AND redesigning for near-zero per-call overhead.
Source: round-1 report §4, round-1 NOTES.md (timer sections).

### C11 — PCM_ReadROM inlining (item-4) — MEASURED SAFE WIN, not yet in split

**What it is.** Inlining the PCM_ReadROM call (`pcm.cpp:33`), a real call in the harness tree at 5.91% of baseline insns as its own symbol.
**Relevance.** Small, safe total-work reduction — the kind of win that survives the bit-exact gate by construction (no semantics change, just call overhead removed).
**Tried.** Measured in an earlier round (not round 1 or 2 of 2026-10-31); pushed to branch `feature/nuked-sc55-optimization` as `f8a0a2e`.
**Outcome.** −0.76% measured. NOT in the harness tree or in main; NOT re-measured in rounds 1–2. It remains the only previously measured safe win not yet in the split — pending integration, small, and deliberately not bundled with other work (commit discipline: only what was approved, separately).
Source: round-1 report §7 (footnote), round-1 NOTES.md (code findings).

---

## Part D — Methodology (how every number above was earned, and what it cannot show)

### D1 — Measure-or-don't-submit (checkasm discipline) — ADOPTED

**What it is.** FFmpeg's rule, from Martin Storsjö's VDD26 talk and commit history: "Before submitting a patch, measure it. If it is not faster than the existing C implementation, adding assembly has no value." Every ARM asm patch carries before/after checkasm numbers across cores, A53 included (2017 vp9 loop filters 2–6× on A53; 2019 vp8 put_epel16_h6v6: A53 2183.0 → 1729.4, ~21% — in cntvct TICKS, see D2 units correction).
**Relevance.** This is our discipline too, in our own shape: the harness's verify + measure pairing (bit-exact gate + insn/model grade) and the variant-kernel hardware A/B (pct + cycle counts) are the same rule. "Code with broken dependency chains runs faster than code that looks tidy" and "in simple functions, unrolling the loop is needed to give the CPU more independent data" are the scheduling doctrines behind C7/C8.
**Tried.** Adopted as the grading rule for rounds 1–2 (never present insn deltas or host wall as a scheduling measurement; never present a model estimate as a Pi percentage).
**Outcome.** No candidate has yet passed it on hardware — that is the point of the variant kernel.
Source: research file (App. 3, doctrines + FFmpeg working method).

### D2 — PMU cycle tooling in the variant kernel — BEING BUILT

**What it is.** FFmpeg's new ext perf layer (dav1d-derived rewrite, `tests/checkasm/ext/`, 2025, Niklas Haas) reads TRUE CPU cycles via direct `pmccntr_el0` (isb + mrs), enabled via checkasm_pmccntr_enable (PMCR.E=1, PMCNTENSET bit 31, PMCCFILTR bits 30/28 cleared) run on ALL cores, validated (must increment within 20 µs or it errors, never silently reports zeros). Bench methodology worth copying: adaptive iteration count toward a time budget but ALWAYS ≥30 samples even past budget; ASM loop shape with 32-call batches and inline outlier rejection (a batch is kept only if t×count ≤ sum×4, i.e. batches >4× the running trimmed mean are dropped — kills interrupt/SIMD-warmup outliers); NOP calibration (measures an empty call and SUBTRACTS it from every result); log-mean + log-variance statistics (mode ± stddev, C-vs-asm ratio, perf_scale to nanoseconds); CPU affinity flag pins to one core; correctness + bench unified (without --bench each function is called once with bit-exact validation; with --bench the same inputs are timed).
**Relevance.** Ported to our bare-metal kernel: NO permission problem (privileged kernel; the Linux-userspace PMUSERENR problem does not exist). PMU registers are PER CORE, so core 2 enables/reads its own counter and core 3 its own. Boot-time validation: if a counter does not increment, log an error, never report zeros. NOP calibration: measure the empty timed region once at boot, subtract from per-region numbers (matters for short regions like a single tick).
**Tried.** Surveyed 2026-10-31 (FFmpeg master cloned to `/tmp/ffmpeg` — ephemeral, not a durable source). Port NOT built at survey time; authorized by 2026-10-31 ~09:21 ("Build the PMU cycle tooling into the variant kernel").
**Outcome.** BEING BUILT into the variant kernel (tree `~/workspace/mt32-pi-shaped-var/`, base = shaped-diag tree): win fields `pcm_cyc` (core-3 PCM task work) + `mix_cyc` (voice-mix portion, accumulated per batch, not per tick, to keep read overhead negligible) + `step_cyc` (core-2 stepping only) + `lock_cyc` (core-2 lock spins only), alongside the existing µs fields; boot line carries `pmc2=ok|fail pmc3=ok|fail`; NOP-calibrated; auto-sequence variants 0 baseline / 1 sched / 2 pipe / 3 neon2 → stop (no hardware keys; audio silence + `sequence_done` log line are the done signals; LED heartbeat unchanged). NOT the diag kernel (do not churn the pending gating run); NOT the harness (QEMU cannot read PMU meaningfully). Units correction carried forward: the CLASSIC checkasm timer reads `cntvct_el0` (typically 62.5 MHz) — the old A53 commit-message numbers are in TICKS, not CPU cycles; ratios/speedups are unaffected, absolute "cycles" claims from those numbers would be wrong.
Source: research file (App. 4), MEMORY.md (variant-kernel build 2026-10-31).

### D3 — PMU phase-2 event counters — PROPOSED, not built

**What it is.** PMU event counters (PMXEVCNTR) — instructions retired + L1D refill + branch mispredict — giving IPC and stall attribution alongside cycle counts.
**Relevance.** Cycles + instructions distinguishes "more work" from "more stalls" — which is exactly the core-2 39-point question (lock spins vs MCU-insn premium vs core-2 extras), and the same distinction for every engine candidate.
**Tried.** Nothing.
**Outcome.** PROPOSED (small, phase 2 of the PMU port), NOT BUILT. No numbers exist.
Source: research file (App. 4, port proposal).

### D4 — QEMU insn-count limits — ESTABLISHED

**What it is.** QEMU counts TCG instructions. A 128-bit NEON op counts as 1 insn regardless of its A53 latency, AGU occupancy, or dual-issue behavior. TCG has no cache model. Host wall time is distorted by x86 out-of-order execution, which hides exactly the stalls that matter on the in-order A53.
**Relevance.** QEMU ranks scalar-scheduling variants ONLY via ordered trace + cycle model (D5's tooling); it CANNOT adjudicate NEON-vs-scalar (NEON insn deltas OVERSTATE wall-time gains); it CANNOT measure cache effects (all of B8's gain hypothesis, any L1/L2 behavior, icache effects A9); it CANNOT answer the core-2 binding question. QEMU is correctness-only for NEON (the verify gate) — and checkasm's own practice agrees (QEMU is correctness-only; checkasm without --bench).
**Tried.** Established across step 4 and rounds 1–2 (neon128's insns-up/wall-down case is the standing proof of TCG distortion).
**Outcome.** ESTABLISHED as the grading rule: count-changing candidates get insn delta + host wall as approximate signals with caveats; scheduling candidates get trace + model only; hardware A/B is ground truth for all.
Source: round-1 report §1, round-2 report ("What the harness cannot measure"), research file (App. 4, dev setup).

### D5 — llvm-mca A53 limits — ESTABLISHED (localized, not global)

**What it is.** LLVM's AArch64SchedA53.td (origin Feb 2014, D2829, no NEON at first; last substantive timing fix Oct 2023, David Green postinc operand order; 2024–2026 touches mechanical only — FEAT_FPv2, C1-Nano, Ampere1B bookkeeping, typo fixes; NO systematic re-measurement). What is RIGHT in current main: in-order (MicroOpBufferSize=0), IssueWidth=2, single LdSt resource (matches the single AGU), 2 ALU units, MispredictPenalty=9 (matches measured ~7–10), latencies sourced from ARM's own A53 timing spreadsheet (cited in the .td), multi-register vector load/store issue cycles modeled explicitly (VLD1–5: latency 4–8, release 2–5). What is WRONG or MISSING: single FPALU resource (does not model the two 64-bit NEON halves dual-issuing); plain 128-bit load occupancy not modeled as 2-cycle (Estevez's `ldr q0` complaint); forwarding assumed (the .td comment itself admits "These latencies will be incorrect when the result cannot be forwarded"); LoadLatency=3 optimistic (assumes L1 hit + bypass). Tool-scope limits (not model bugs): llvm-mca analyzes a straight-line block in isolation — per its own man page it does not model branch prediction or fetch/decode, and it never models caches or memory addresses.
**Relevance.** A challenge to the blanket "LLVM's A53 model is wrong" (LLVM is not static; "wrong" is not binary). The revised guidance supersedes the blanket ban: llvm-mca's A53 numbers are usable as a rough pipeline sanity check for straight-line scalar code, but NOT trustworthy for NEON dual-issue, vector-load throughput, or anything involving memory or branches — and never as the grading tool for scheduling (own cycle model from microbenchmarks, or hardware A/B, remains the way).
**Tried.** Empirically retested 2026-10-31 (`llvm-mca-a53-retest-20261031.md`, LLVM 23.1.4, Oct 2026): all three Feb 2025 complaints STILL reproduce — `ldr q0` 1.0–1.04 cycles/load (reality 2.0); fadd+add retire-together (add D@3, both E@6; 3.04 cycles/pair steady-state); 64-bit fadd+ldr no dual-issue (2.05 cycles/pair). Scalar control PASSES (independent adds dual-issue, IPC 1.89; order-swapped add-first dual-issues; dependent-add forwarding works). So the wrongness is LOCALIZED to NEON-follower scheduling, not global — scalar parts fine, NEON complaints unchanged ~20 months later.
**Outcome.** ESTABLISHED: our scheduling grades use the hand-built `a53sim.py` (ordered trace + microbenchmark timings), never llvm-mca.
Source: research file (App. 1, LLVM appendix), `hidden_files/llvm-mca-a53-retest-20261031.md`.

### D6 — Bit-exact gate discipline — ABSOLUTE

**What it is.** Every candidate must reproduce the decoder2-oracle goldens exactly (state hash excludes `pcm_t`; `grep 'verify:'` for the verdict, never tail output — verify exit status is masked when piped through `tail`). A failing candidate is FAILED with its checkpoint, never tuned to fit goldens. Prefix is for iteration only; full E1M1 (+ BELMO where in scope) is required before claiming engine-semantics success.
**Relevance.** This is why C10 is dead despite a favorable diagnostic number, why C5/C6 must pass the gate before reaching the variant kernel, and why B3 (tree reduction) is blocked rather than merely unmeasured.
**Tried.** Applied to every candidate in rounds 1–2 (C7/C8 full-song green; C2–C4 prefix green; C10 failed with checkpoints named).
**Outcome.** ESTABLISHED, non-negotiable. MIDI/golden paths kept short (ReadGolden truncates at 63 chars) — a harness trap, not a gate rule, but recorded here because it has bitten runs before.
Source: round-1 report §1, round-2 report (Scope and gates).

### D7 — Diag-kernel gating measurement — AWAITING RUN

**What it is.** The shaped-diag kernel (`~/workspace/kernel8-debug-shaped-diag-20261031.img`, 582,225 B, md5 `1f12844ef17e35d25434840deedcf46c`, `_end` 0x2ef56c, tree `~/workspace/mt32-pi-shaped-diag/`, log `SD:/shaped-diag-stats.log`, marker `kernel=mt32-shaped-diag`) splits core-2 `StepUntilFill` time into MCU stepping vs state-lock spin waits: `ThreadSplitStateCircle` gains `lock_wait_us` + `lock_waits` (core-2-only, written in `SkelPCMWrite` committing branch + `SkelPCMRead` status branch around `Acquire`, timed with `CTimer::GetClockTicks()`; uncontended acquires read 0 µs, so `lock_us` counts contended waits and `lock_waits` counts every lock op; core-3 batch holds NOT counted — they are what core 2 waits ON). Win line gains `lock_us` + `lock_waits` + `mcu_us` (step-loop wall MINUS `lock_us`).
**Relevance.** THE gating measurement for all optimization claims: the ~39-point core-2 gap decides whether engine speed work matters at all.
**Tried.** Built and parent-verified (all guards pass); diagnostic-only (two extra timer reads per lock op on the hot path — never ship it).
**Outcome.** AWAITING HARDWARE RUN (E1M1 only; auto-starts, no key presses needed). Decision rule: if `lock_us` is a large fraction of the step-loop wall → the gap is LOCK SPINS (fix sync design: finer lock granularity, shorter batches, lock-free publication — NOT engine speed; do not pursue NEON voices as the fix). If `lock_us` ≈ 0 (`mcu_us` ≈ step wall) → the gap is MCU WORK (H8 decoder2 dispatch costs more per instruction on A53 than QEMU insn counts suggest, and/or core-2 extras) — NEON voices becomes credible again as one lever, and C7's hardware A/B is worth running. Either way, report sustained pct + devq_low + sound_drops as before (the chop metric); the new fields explain the pct, they don't replace it.
Source: round-1 report §5, MEMORY.md (diag kernel entry).

---

## Part E — Does not transfer

### E1 — FFmpeg video block / streaming shapes — DO NOT TRANSFER

**What it is.** FFmpeg's kernels are mostly pure-NEON streaming throughput loops over large blocks; dav1d's are block-oriented pixel functions.
**Relevance.** NONE to our loop shape: our voice loop is short, branchy, per-voice stateful (envelope, phase), with a 128-tick batch bound and a state lock. FFmpeg's unroll-and-interleave doctrine transfers; its block sizes and streaming shapes do not. Ours also mixes a scalar fetch/envelope head (no gather — stays scalar) with a vector mix tail, where FFmpeg's kernels are mostly pure NEON streaming — the FFmpeg playbook agrees with the split (do not vectorize the gather part).
**Tried.** Nothing — correctly nothing.
**Outcome.** DO NOT TRANSFER. Recorded so a future reader does not import block-loop unroll factors or streaming shapes into this loop.
Source: research file (App. 3, "What does NOT transfer").

### E2 — dav1d/FIR headline numbers (3×, 3.37×, 40%) as predictions — DO NOT TRANSFER

**What it is.** The 3× partial-load fix, the 3.37× combined figure, and the ~40% same-register accumulation gain are dav1d/FIR numbers on their own loops and cores.
**Relevance.** They set priors and candidate shapes, NOT expected gains for our loop. The 3× core attribution is not even pinned to A53 specifically in the talk report. Quoting any of them as a predicted gain for the Nuked voice loop would be inventing a roadmap — the standing rule (a feature/number either exists with a source, does not exist, or is Not Verified) forbids it.
**Tried.** The shapes behind the numbers were tried (B4, B1/C5); the numbers themselves were never claimed for our loop.
**Outcome.** DO NOT TRANSFER as predictions. Our loop's numbers are only the ones measured in Parts C–D.
Source: research file (App. 3, "What does NOT transfer" + trick caveats).

---

## Research items with NO tried/queued outcome (unmapped — stated plainly)

These surfaced in the research but have no candidate, measurement, or queued implementation. They are NOT hidden inside a verdict above.

1. **Pi-specific cache configuration (BCM2710A1 L1/L2 sizes, replacement policy).** SoC cache sizes are implementation choices — phone-SoC numbers (ARMageddon 2016: Snapdragon 410, L1 32 KB 4-way, L2 512 KB 16-way, instr-inclusive / data-non-inclusive) do NOT transfer exactly to BCM2710A1. No Pi source was found. NOT MEASURED. Consequence: A7's latency numbers are folklore-grade for our SoC, and B8's "L1-resident" expectation is an inference, not a measurement.
2. **A55-guide per-instruction tables as the basis for a cycle model.** The tables exist and are public (ARM-EPM-128372 §4), but our `a53sim.py` was hand-built from microbenchmark timings, NOT from A55 tables. The tables remain an unused prior — a future model revision could start from them and validate against A53 microbenchmarks. NOT TRIED.
3. **llvm-exegesis on a Linux-booted Pi.** llvm-exegesis exists for AArch64 but must run ON A53 hardware under Linux with libpfm; no published A53 exegesis tables were found, and it cannot run on the bare-metal Circle kernel. A Linux-booted Pi 3 / Zero 2 W could run it — a separate hardware exercise. NOT DONE.
4. **Deeper (3–4 voice) interleaving.** Round 2 tested the pair split (B7/C8). A 3–4-deep interleave was NOT attempted. The pair-split mechanism analysis (load→use distances within each head unchanged; ctx traffic grows with depth) predicts the same outcome a fortiori, but that prediction is UNMEASURED — it is analysis, not a result.
5. **Different compiler flags for NEON intrinsics.** Round-1 notes name "asm (or different flags)" as the alternative after scalarization (C1). The flags route was never pursued once asm worked. NOT TRIED.
6. **Branch-predictor behavior on our kernel.** A55's perceptron predictor vs A53's GShare is a known transfer caveat (A8), and branch mispredict is ~7–10 cycles (A7) — but no branch-behavior measurement exists for our kernel's switch-heavy slot loop. NOT MEASURED. (The cycle model gives branches no mispredict penalty because the path is identical in both schedules, so branch costs cancel in the delta — that is a modeling choice, not a measurement.)
7. **PMU phase-2 event counters.** See D3 — proposed, not built. No IPC or stall-attribution numbers exist yet.
8. **The core-2 39-point gap itself.** Not an optimization — the gating unknown (D7). Until the diag run lands, every engine-speed claim stays provisional, and no item in Parts B–C can be claimed as the real-time fix.
9. **Prefetch shared-helper implementation.** See B8 — design only. The ~5–8 insns/slot-tick cost is an estimate, not a measurement.
10. **Split-accumulator + restructured-layout NEON outcomes.** See C5–C6 — round 3 in progress, gate pending. No numbers exist yet; they are listed as IN PROGRESS, not as results.

## Sources (all under `~/workspace/goals/mt32-pi-nuked-sc55-engine/hidden_files/` unless noted)

- `a53-execution-research-20261031.md` — microarchitecture traps, LLVM A53 appendix, A55-guide transfer analysis, FFmpeg/dav1d Appendix 3, checkasm/PMU Appendix 4.
- `optimization-20261031.md` (round 1) — C1–C3, C7, C9–C11, D7 build, grading rules.
- `optimization-round2-20261031.md` (round 2) — C4, C8, B8, B6, "what the harness cannot measure."
- `opt-20261031/NOTES.md` — round-1 working notes (scalarization, sched pragma dead end, model tooling, timer debug path).
- `opt-round2-20261031/NOTES.md` — round-2 working notes (pipe surgery, prefetch design, post-increment audit).
- `llvm-mca-a53-retest-20261031.md` — LLVM 23.1.4 retest (D5 numbers).
- `hotblocks-step4-20261031.md` — step-4 attribution (baseline numbers).
- `~/MEMORY.md` (mt32-pi Nuked-SC55 section) — standing context: shape runs, diag kernel, variant kernel (0–3 + round-3 4/5), PMU build, corrections (NEON "CLOSED" → DEPRIORITIZED).
- Candidate trees: `opt-20261031/cand-{base,sched,timer,neon64,neon128}/`, `opt-round2-20261031/cand-{neon2,pipe}/`.
