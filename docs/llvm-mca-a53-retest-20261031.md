# llvm-mca Cortex-A53 retest (2026-10-31)

Question: does CURRENT llvm-mca still reproduce the February 2025
Estevez complaints for `-mcpu=cortex-a53`?

Background: Daniel Estevez, "Coding NEON kernels for the Cortex-A53"
(Feb 2025), reported llvm-mca gives "quite wrong results even in many
simple cases" for A53, with two concrete Compiler Explorer examples:
(1) it believes `ldr q0, [x0]` has a throughput of one instruction per
cycle (measured reality: 2 cycles to issue, no dual-issue, because the
A53 load path is 64 bits); (2) it believes a NEON `fadd` and an integer
`add` cannot dual-issue, and that the `add` must stall to retire at the
same time as the `fadd`.

Parent's prior .td read (current LLVM main, 2026-10-31): single FPALU
resource (no two-64-bit-half NEON dual-issue modeling), optimistic
LoadLatency, forwarding assumed — but those are MODEL facts. This
retest checks the TOOL's behavior, which changes independently of
the .td.

## Binary obtained

- `llvm-mca --version`: LLVM version 23.1.4, Optimized build,
  Default target x86_64-unknown-linux-gnu, Host CPU znver5.
- Source: official LLVM release tarball
  `LLVM-23.1.4-Linux-X64.tar.xz` (tag llvmorg-23.1.4, published
  2026-10-20), downloaded via curl through the egress proxy from
  `github.com/llvm/llvm-project/releases/download/llvmorg-23.1.4/`.
  Only `bin/llvm-mca` (53,466,704 bytes) was extracted, by exact-member
  tar extraction. Note: GNU tar did NOT stop the stream after the
  member was found — the pipeline kept downloading (reached ~390 MB)
  until killed; the extracted binary was already complete and was
  verified by `--version` and `ldd` (system libs only, no tarball
  libLLVM dependency). No LLVM source was built; no mt32-pi tree,
  kernel image, or git state was touched.
- Invocation base for every test below:
  `llvm-mca -march=aarch64 -mcpu=cortex-a53`
  Full raw outputs: `~/workspace/llvm-mca-test/results.txt`
  plus the per-test files in the same directory.

## Test inputs

- `a_ldr_q0_repeat.s` — Estevez literal: `ldr q0, [x0]` x10.
- `a_ldr_q_indep.s` — dependency-free 128-bit loads (rotating dests).
- `b_fadd_add.s` — Estevez literal: `fadd v0.4s, v0.4s, v0.4s` +
  `add x0, x0, x0`.
- `b_fadd_add_indep.s` — dependency-free pairs (rotating regs, 10 pairs).
- `b_indep_pair.s` — fully independent single pair
  (`fadd v0.4s, v1.4s, v2.4s` + `add x0, x1, x2`).
- `c_fadd64_ldr64.s` — Estevez 64-bit-half pair:
  `fadd v0.2s, v0.2s, v0.2s` + `ldr d1, [x0]`.
- `c_fadd64_ldr64_indep.s` — dependency-free version of (c).
- `c_indep_pair.s` — fully independent single pair
  (`fadd v0.2s, v1.2s, v2.2s` + `ldr d1, [x0]`).
- `ctrl_add_add.s` — control: independent integer adds (MUST dual-issue).
- `swap_add_fadd.s` — order control: add FIRST, fadd second.
- `cal_dep_adds.s` — calibration: dependent adds (forwarding behavior).

Timeline notation: `D` = dispatched, `e` = executing, `E` = executed
(writeback); cycle numbers are the timeline columns. `RThroughput`
is llvm-mca's reciprocal-throughput column (cycles per instruction
from resource pressure).

## Results

### Control: independent scalar adds — PASS (tool is not globally broken)

- `ctrl_add_add.s`, 10 iterations (100 insns): 53 cycles, IPC 1.89.
- Single-pair timeline: both instructions `D` cycle 0, `E` cycle 3.
  Dual-issue works for scalar. Invocation is sane.

### (a) `ldr q0` throughput — STILL WRONG

- `a_ldr_q0_repeat.s`, 10 iterations (100 loads): 104 cycles,
  IPC 0.96 -> **1.04 cycles per 128-bit load** (4 cycles are
  pipeline fill/drain; steady state is 1.0).
- `a_ldr_q_indep.s` (dependency-free loads): identical 104 cycles,
  1.04/load. The result is resource modeling, not a dependency chain.
- Instruction info: `ldr q0, [x0]` latency 4, **RThroughput 1.00**.
- Measured reality (Estevez): 2 cycles to issue, no dual-issue
  (64-bit load path). llvm-mca 23.1.4 still reports 1.0.
- Verdict: **STILL WRONG**, unchanged since Feb 2025.

### (b) `fadd` + `add` dual-issue and retire-together — STILL WRONG

Single-pair timeline (literal AND fully-independent registers,
identical result):

```
[0,0]  0:D 1:e 2:e 3:e 4:e 5:e 6:E   fadd v0.4s, v0.4s, v0.4s
[0,1]  0:. 1:  2:  3:D 4:e 5:e 6:E   add  x0, x0, x0
```

- Instruction info: fadd latency 6, RThroughput 1.00; add latency 3,
  RThroughput 0.50. Resources used are disjoint (fadd -> FPALU,
  add -> ALU), so no resource contention explains the delay.
- The add does NOT dispatch until cycle 3 — it does not dual-issue
  with the fadd — and it completes (E) at cycle 6, the exact cycle
  the fadd completes. That is Estevez's complaint reproduced
  verbatim: cannot dual-issue, stalls to complete with the fadd.
- Steady state confirms it: `b_fadd_add_indep.s`, 10 iterations
  (200 insns): 304 cycles, IPC 0.66 -> **1.52 cycles/instruction,
  3.04 cycles/pair**, vs 1.0 cycle/pair if dual-issued and even
  2.0 cycles/pair if merely serial. The coupling compounds across
  iterations (fadd chains + retire coupling), landing ~3x worse
  than resource bounds (10 fadds x 1.0 FPALU = 10 cycles/iteration
  would be the bound; observed 30.4).
- Observed dispatch-delay pattern (two points, inference not proof):
  follower dispatch cycle = fadd completion cycle minus follower
  latency (add: 6-3=3; ldr below: 6-4=2). I.e. the tool appears to
  back-schedule the follower so it completes with the fadd.
- Order asymmetry (further evidence this is a tool artifact, not
  hardware): with the order swapped (`swap_add_fadd.s`, add FIRST),
  BOTH dispatch at cycle 0 (add E@3, fadd E@6) — dual-issue works.
  The failure is specific to instructions FOLLOWING a NEON op.
- Verdict: **STILL WRONG**, unchanged since Feb 2025.

### (c) 64-bit NEON `fadd` + 64-bit `ldr` dual-issue — STILL WRONG

Single-pair timeline (literal AND fully-independent registers,
identical result):

```
[0,0]  0:D 1:e 2:e 3:e 4:e 5:e 6:E   fadd v0.2s, v0.2s, v0.2s
[0,1]  0:. 1:  2:D 3:e 4:e 5:e 6:E   ldr  d1, [x0]
```

- The load does NOT dispatch until cycle 2 and completes at cycle 6
  with the fadd, although microbenchmarks say a 64-bit NEON op and
  a 64-bit load CAN dual-issue (each uses one 64-bit NEON half /
  the load pipe). Steady state: `c_fadd64_ldr64_indep.s`,
  10 iterations (200 insns): 205 cycles, IPC 0.98 ->
  **1.025 cycles/instruction, 2.05 cycles/pair** — serial, not
  dual-issue (~1.0/pair expected).
- This is consistent with the parent's .td finding (single FPALU
  resource, no two-64-bit-half modeling) — now confirmed at the
  TOOL level, not just the model level.
- Verdict: **STILL WRONG**.

### Calibration note (not a verdict item)

Dependent adds (`add x0, x0, x0` twice): second instruction D@1,
E@4 — i.e. the tool models result forwarding (dependent op begins
executing 1 cycle before the producer's writeback). Forwarding
itself works in the tool; the NEON-follower delay above is a
separate artifact.

### Side observation (not a verdict item)

llvm-mca's fadd latency is 6 cycles, vs ~2-3 cycles from
microbenchmarks. Latency and throughput are different quantities
(fadd RThroughput is 1.00, which is plausible); the latency figure
may reflect the model's no-forwarding pessimism (the .td comment
itself admits latencies "will be incorrect when the result cannot
be forwarded"). The verdicts above rest on throughput and
dispatch behavior, not on this latency number.

## Per-example verdicts

| Example | llvm-mca 23.1.4 | Microbenchmark reality | Verdict |
|---|---|---|---|
| (a) `ldr q0` throughput | 1.00-1.04 cycles/load | 2.0 cycles, no dual-issue | STILL WRONG |
| (b) `fadd`+`add` dual-issue / retire-together | add D@3, both E@6; 3.04 cycles/pair steady-state | dual-issue, independent completion | STILL WRONG |
| (c) 64-bit `fadd`+`ldr` dual-issue | ldr D@2, both E@6; 2.05 cycles/pair | dual-issue (~1.0/pair) | STILL WRONG |
| Control: independent scalar adds | both D@0, IPC 1.89 | dual-issue | (tool sane) |

No example was FIXED; none was UNCLEAR. All three Feb 2025
complaints reproduce on LLVM 23.1.4 (Oct 2026), ~20 months later.

## What this does and does not change

- It CONFIRMS, rather than softens, the Feb 2025 critique for the
  NEON cases. The parent's revised guidance stands: llvm-mca's A53
  numbers are usable as a rough pipeline sanity check for
  straight-line scalar code (the control passes), but NOT
  trustworthy for NEON dual-issue, vector-load throughput, or
  anything involving memory or branches — and never as the grading
  tool for scheduling (own cycle model from microbenchmarks, or
  hardware A/B, remains the way).
- It answers the "wrong is not binary" challenge precisely:
  the failure is LOCALIZED, not global. Dispatch width (2),
  scalar dual-issue, and per-instruction resource assignment are
  right; the failure is specifically the scheduling of
  instructions that FOLLOW a NEON op (dispatch delayed, completion
  coupled to the NEON op). So "has it got better?" gets a split
  answer: the scalar parts are fine (and the tool has clearly been
  maintained — LLVM 23 is 20 months newer than the critique), but
  the specific NEON complaints are unchanged.
- It does NOT change the overnight optimization plan: scheduling
  candidates are still graded by trace-plus-cycle-model or
  hardware A/B, never by llvm-mca. If anything it strengthens
  that rule, since the tool's NEON-follower serialization would
  actively mis-rank a NEON/scalar interleaving.
