// mu50h8.h -- standalone H8/300H interpreter for the MU50 engine.
//
// Runs the real Yamaha MU50 firmware (H8/3003 @ 10 MHz) with a bus shim:
// 512 KB firmware ROM (word-swapped view, as MAME loads it), 64 KB work
// RAM, SWP00 window mapped to libmu50's register API, SCI1 as the MIDI
// UART, panel stubbed (mt32-pi has its own LCD). No MAME dependencies,
// no threads/mutex/exceptions; C++17. Instruction semantics are
// transpiled from MAME's h8.lst (BSD-3-Clause, Olivier Galibert) -- see
// src/synth/mu50h8_semantics.inc and mu50h8_opcodes.h (generated).
//
// Cycle model mirrors MAME's H8H accounting: each bus access costs 2
// cycles, internal(n) costs n+1 cycles, and a discarded prefetch word
// (+2 cycles) is charged on every control transfer.
#pragma once

#include <cstdint>
#include <cstddef>

namespace mu50h8 {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s8 = std::int8_t;
using s16 = std::int16_t;
using s32 = std::int32_t;

// CCR flag bits (H8)
enum {
  F_I = 0x80, F_UI = 0x40, F_H = 0x20, F_U = 0x10,
  F_N = 0x08, F_Z = 0x04, F_V = 0x02, F_C = 0x01
};
// EXR bits (vestigial on H8H: never pushed/popped, kept for completeness)
enum { EXR_T = 0x80, EXR_NC = 0x78, EXR_I = 0x07 };

struct Hooks {
  void *ctx = nullptr;
  // SWP00 register write, stamped with the machine cycle it happened at.
  void (*swp_write)(void *ctx, u64 cycle, u16 addr, u8 data) = nullptr;
  // SWP00 register read (libmu50 read_register).
  u8 (*swp_read)(void *ctx, u16 addr) = nullptr;
  // SCI1 TX byte (firmware MIDI out); may be null (dropped).
  void (*midi_tx)(void *ctx, u8 byte) = nullptr;
  // Diagnostic log (unhandled accesses, enabled DMA/watchdog, ...).
  // Null in the engine; the Linux harness prints it.
  void (*log)(void *ctx, const char *msg) = nullptr;
  // SCI1 RDR read by the firmware (a received MIDI byte delivered
  // to the firmware). Fires only for SCI1, never SCI0. May be null.
  void (*sci1_rdr_read)(void *ctx) = nullptr;
};

class Mu50H8 {
public:
  // rom: 512 KB raw firmware file bytes (the bus applies MAME's
  // word-swap view: region[A] = rom[A^1]).
  Mu50H8(const u8 *rom, const Hooks &hooks);
  ~Mu50H8();
  void reset();
  // Host MIDI byte in (SCI1 RX). Queued; the line driver sends it at
  // 31250 baud as soon as the line is idle. Returns false if the
  // 256-byte queue overruns and the byte is dropped.
  bool queue_midi_byte(u8 b);
  // Test hook: write one byte through the bus (peripheral dispatch).
  void dbg_write8(u32 a, u8 v) { bus_write8(a, v); }
  // Advance until cycles() >= target (or halted). Returns false if the
  // CPU halted on an illegal opcode (see illegal_pc()).
  bool run_until(u64 target);
  u64 cycles() const { return cycles_; }
  u32 pc() const { return pc_; }
  u32 sp() const { return r32_r(7); }
  u32 instr_count() const { return instr_count_; }
  bool halted() const { return halted_; }
  u32 illegal_pc() const { return illegal_pc_; }
  bool sleeping() const { return sleeping_; }

private:
  // ---- CPU state (names match the transpiled semantics) ----
  u16 r_[16]; // Rn (0-7), En (8-15): ERn = (r_[n+8] << 16) | r_[n]
  u32 pc_;
  u8 ccr_;
  u8 exr_;
  u8 syscr_;
  u64 cycles_;
  u16 ir_[8];
  u32 tmp1_, tmp2_, tmp3_;
  u32 taken_vector_;
  bool sleeping_;
  bool halted_;
  u32 illegal_pc_;
  u32 instr_count_;
  u32 instr_start_pc_;

  // ---- memory ----
  const u8 *rom_;
  u8 ram_[65536];   // work RAM 0x200000-0x20FFFF
  u8 iram_[512];    // internal RAM 0xFFFD10-0xFFFF0F (peripherals override)
  Hooks hooks_;

  // ---- register accessors (MAME h8.h/h8h.h semantics) ----
  inline void r8_w(int reg, u8 val) {
    if (reg & 8) r_[reg & 7] = (r_[reg & 7] & 0xff00) | val;
    else r_[reg & 7] = (r_[reg & 7] & 0xff) | (val << 8);
  }
  inline u8 r8_r(int reg) const {
    if (reg & 8) return (u8)r_[reg & 7];
    return (u8)(r_[reg & 7] >> 8);
  }
  inline void r16_w(int reg, u16 val) { r_[reg & 0xf] = val; }
  inline u16 r16_r(int reg) const { return r_[reg & 0xf]; }
  inline void r32_w(int reg, u32 val) {
    r_[reg & 7] = (u16)val;
    r_[(reg & 7) | 8] = (u16)(val >> 16);
  }
  inline u32 r32_r(int reg) const {
    return r_[reg & 7] | ((u32)r_[(reg & 7) | 8] << 16);
  }

  // ---- flag helpers (MAME h8.cpp) ----
  inline void set_nzv8(u8 v) {
    ccr_ &= ~(F_N | F_V | F_Z);
    if (!v) ccr_ |= F_Z;
    else if ((s8)v < 0) ccr_ |= F_N;
  }
  inline void set_nzv16(u16 v) {
    ccr_ &= ~(F_N | F_V | F_Z);
    if (!v) ccr_ |= F_Z;
    else if ((s16)v < 0) ccr_ |= F_N;
  }
  inline void set_nzv32(u32 v) {
    ccr_ &= ~(F_N | F_V | F_Z);
    if (!v) ccr_ |= F_Z;
    else if ((s32)v < 0) ccr_ |= F_N;
  }
  inline void set_nz16(u16 v) {
    ccr_ &= ~(F_N | F_Z);
    if (!v) ccr_ |= F_Z;
    else if ((s16)v < 0) ccr_ |= F_N;
  }
  inline void set_nz32(u32 v) {
    ccr_ &= ~(F_N | F_Z);
    if (!v) ccr_ |= F_Z;
    else if ((s32)v < 0) ccr_ |= F_N;
  }

  // ---- cycle accounting (MAME H8H: bus 2 cycles, internal n+1) ----
  inline void internal(int n) { cycles_ += (u64)n + 1; }

  // ---- bus ----
  u8 bus_read8(u32 a);
  u16 bus_read16(u32 a);
  void bus_write8(u32 a, u8 v);
  void bus_write16(u32 a, u16 v);
  // Peripheral-space dispatch (a >= 0xff0000, 8/16-bit).
  u8 bus_read8_per(u32 a);
  u16 bus_read16_per(u32 a);
  void bus_write8_per(u32 a, u8 v);
  void bus_write16_per(u32 a, u16 v);
  // Names used by the transpiled semantics:
  inline u8 read8(u32 a) { return bus_read8(a); }
  inline u16 read16(u32 a) { return bus_read16(a); }
  inline u16 read16i(u32 a) { return bus_read16(a); }
  inline void write8(u32 a, u8 v) { bus_write8(a, v); }
  inline void write16(u32 a, u16 v) { bus_write16(a, v); }

  // ---- exception helpers used by the transpiled semantics ----
  inline bool exr_in_stack() const { return false; } // H8H: no EXR frame
  inline int trapa_setup() { // h83003: TRAPA vectors 8-11, masks like IRQ
    if (syscr_ & 0x08) ccr_ |= F_I;
    else ccr_ |= F_I | F_UI;
    return 8;
  }
  inline void irq_setup() {
    if (syscr_ & 0x08) ccr_ |= F_I;
    else ccr_ |= F_I | F_UI;
  }
  void update_irq_filter();
  void interrupt_taken();
  inline void illegal() {
    halted_ = true;
    illegal_pc_ = instr_start_pc_;
  }

  // ---- execution ----
  void execute_one();
  void take_irq();
  bool pending_irq() const { return irq_vector_ != 0; }

  // ---- decoded-instruction cache (H1) ----
  // Direct-mapped cache, one slot per ROM word address: index =
  // (pc >> 1) & 0x3FFFF. An entry is used only if the full PC tag and
  // every opcode word the match depends on still match the freshly
  // fetched words, so a hit yields exactly the table scan's result
  // even if code in RAM ever executed (RAM words are re-validated on
  // every hit; ROM words can never change). The match is a pure
  // function of the opcode words, so a validated hit is exact. On a
  // hit only the scan is skipped, never a bus fetch: w[0] always,
  // w[1] iff nwords >= 2, w[2] iff nwords >= 3, exactly as the scan
  // path fetches -- so cycle counts are unchanged. Null when the
  // allocation failed, in which case execute_one falls back to the
  // scan path. All 28.2M traced instructions (boot + steady state +
  // MAME reference) execute from ROM, so ROM slots never collide.
  struct DecodeCacheEntry {
    u32 pc = 0;      // instruction-start PC (tag)
    u16 w0 = 0, w1 = 0, w2 = 0;  // opcode words the match depends on
    u16 fn = 0;      // index into kExecFns
    u8 nwords = 0;   // opcode words (matched)
    u8 extra = 0;    // extra words (immediates/displacements/addresses)
    u8 skip = 0;     // words skipped between opcode and extra words
    u8 valid = 0;    // entry populated
  };
  static constexpr int kDecodeCacheSize = 262144;  // one slot per ROM word
  DecodeCacheEntry *decode_cache_ = nullptr;

  // ---- lazy event time cache (H2 Part A) ----
  // Cached minimum over all pending peripheral event times (see
  // next_event_cycle). Recomputed only when dirty: any firmware
  // peripheral write (funneled through bus_write8_per /
  // bus_write16_per), host MIDI queueing, event firing, the guard
  // fallback, or reset marks it dirty. ~u64(0) when no event is
  // pending. In steady state the hot tick path does not consult this
  // cache at all (see tick_peripherals); it serves the sleeping path
  // in run_until, where event sparsity makes the cache pay.
  u64 next_event_cache_ = 0;
  bool next_event_dirty_ = true;
  // Cycles at the most recent tick_peripherals entry (= end of the
  // most recently completed instruction, or the last sleep jump).
  // Timer catch-up/touch targets this, never current cycles_: the
  // eager model advances channels only at instruction boundaries,
  // so eager state at any mid-instruction read/write is exactly the
  // state as of the last tick boundary.
  u64 last_tick_cycles_ = 0;

  // ---- INTC (H8H) ----
  // pending IRQ bitmap, vectors 0-63. External IRQ inputs: IRQ0 is
  // permanently asserted on the MU50 (PCB ties it to ground; MAME
  // ymmu50.cpp machine_reset: set_input_line(0, ASSERT_LINE));
  // IRQ1-7 inputs are inactive. Internal sources set bits directly.
  u64 intc_pending_;
  u8 intc_ier_; // external IRQ enables (vectors 12-19)
  u8 intc_isr_; // external IRQ status (vectors 12-19)
  u16 intc_icr_; // priority bits: slot s -> bit (s ^ 7)
  u16 intc_iscr_; // per-IRQ level-low (0) vs falling-edge (1) select
  u8 intc_irq_input_; // asserted external input levels (bit i = IRQ i)
  int irq_vector_; // selected vector (0 = none), recomputed on any change
  int irq_filter_; // 0/1/2 from CCR + SYSCR (see update_irq_filter)
  void intc_update();
  void intc_raise(int vector);
  static int intc_slot(int vector);

  // ---- SCI (x2: 0 = unused by MU50, 1 = MIDI) ----
  struct Sci {
    u8 smr, brr, scr, tdr, ssr, ssr_read, rdr, rsr, tsr;
    u8 rx_bit, tx_bit, rx_parity, tx_parity;
    int rx_state, tx_state; // 0 idle, 1 start, 2 bit, 3 parity, 4 stop, 5 last
    int rx_counter, tx_counter; // 16x oversample counters
    u32 divider; // cycles per 16x tick
    u64 next_tick; // next 16x tick cycle (0 = clock stopped)
    bool rx_clock_on, tx_clock_on;
    int rx_value; // current line level (1 idle)
    int eri, rxi, txi, tei; // interrupt vectors
  };
  Sci sci_[2];
  void sci_reset(Sci &s, int eri, int rxi, int txi, int tei);
  void sci_write(Sci &s, u32 off, u8 v);
  u8 sci_read(Sci &s, u32 off);
  void sci_rx_line(int id, int level); // host line edge (SCI1 MIDI in)
  void sci_tick(Sci &s); // one 16x tick (RX + TX state machines)
  void sci_rx_start(Sci &s);
  void sci_rx_step(Sci &s);
  void sci_tx_start(Sci &s);
  void sci_tx_step(Sci &s);
  void sci_rx_done(Sci &s);

  // MIDI line driver (host side, SCI1): bytes queued by queue_midi_byte,
  // sent at 31250 baud (320 cycles/bit) regardless of RE (the SCI only
  // samples them when RE is set, as on hardware).
  u8 midi_queue_[256];
  int midi_q_head_, midi_q_tail_;
  int midi_bit_; // -1 start, 0-7 data, 8 stop, -2 idle
  u8 midi_byte_;
  u64 midi_edge_; // cycle of the next line transition
  void midi_line_tick(u64 now);

  // ---- Timer16 x5 ----
  struct Timer {
    u16 tcnt, tgra, tgrb, tbra, tbrb;
    u8 tcr, tior, tier, tsr;
    u8 running; // TSTR bit
    int base; // irq base: TGIA=base, TGIB=base+1, OVI=base+2
    u64 next_inc; // next TCNT increment cycle (0 = stopped)
    u32 divider; // cycles per count (1/2/4/8 or 0 = external input)
    // Timer-guard (per-channel lazy advance) state, packed into ONE
    // u64 so the hot tick check is a single load + compare.
    // edge_packed is either a real next-edge cycle time, kEdgeNone
    // (~u64(0): no future edge -- stopped), or kEdgeDirty (~u64(0)-1:
    // stale -- a write changed counter/compare/divider/running since
    // the last advance, and the channel must be advanced, never
    // skipped). Collision proof: real edge times are cycle counts;
    // reaching 2^64-1 at 10 MHz takes ~58,000 years, and the distance
    // construction adds at most 65535*8 cycles to a real next_inc,
    // so no computed edge can land on either sentinel in any
    // feasible run. timer_advance is the only writer of edge_packed
    // (plus timer_touch, which forces kEdgeDirty, and reset).
    static constexpr u64 kEdgeNone = ~u64(0);
    static constexpr u64 kEdgeDirty = ~u64(0) - 1;
    u64 edge_packed = kEdgeDirty;
  };
  Timer timer_[5];
  u8 tstr_, tsyr_, tmdr_, tfcr_, toer_, tocr_;
  void timer_write(int ch, u32 off, u8 v);
  u8 timer_read(int ch, u32 off);
  void timer_inc(int ch);
  void timer_update_dividers(int ch);
  // Timer-guard helpers. timer_next_edge computes the next observable
  // edge time from current state (same distance construction as
  // timer_advance). timer_catch_up advances a channel to the last tick
  // boundary (for TCNT reads: eager state at read time). timer_touch
  // does the same advance and then marks the edge cache dirty (for
  // writes: the write that follows invalidates the cache).
  // Due-list: due_order_ is a permutation of {0..4} sorted ascending
  // by edge deadline (dirty = 0 = due now, INF = last). The tick loop
  // advances only the head while it is due; due_reinsert restores
  // sorted order after any deadline change. Order-independence proof:
  // per-channel timer state is disjoint, shared outputs are
  // OR-accumulated IRQ pending bits and per-channel TSR flags, and
  // IRQ takes happen only at instruction boundaries after the full
  // tick -- so advancing due channels in deadline order is
  // observationally identical to fixed 0..4 order.
  u8 due_order_[5] = {0, 1, 2, 3, 4};
  u64 edge_deadline(int ch) const;
  void due_reinsert(int ch);
  u64 timer_next_edge(int ch) const;
  void timer_catch_up(int ch);
  void timer_touch(int ch);

  // ---- ADC ----
  u8 adcsr_, adcr_;
  u16 addr_[8];
  u64 adc_done_; // completion cycle (0 = idle)
  void adc_write(u32 off, u8 v);
  u8 adc_read(u32 off);

  // ---- ports / panel stub ----
  u8 port_ddr_[16], port_dr_[16];
  u8 pcr4_;
  u8 lcd_rs_; // current LCD RS bit (from P6) for pa_r
  u8 p6_cur_; // driver cur_p6: P6 output XOR ENABLE (MAME ymmu50 p6_w);
              // init 0xff (machine_start), NOT the port-device DR reset
  u8 port_read(int p);
  void port_write(int p, u8 v);

  // ---- misc peripherals (registers as storage + logging) ----
  u8 wdt_[4], refresh_[4];
  u32 dma_[64];
  void logf(const char *fmt, u32 a, u32 v);

  // ---- peripheral event loop ----
  void tick_peripherals(); // process all events with cycle <= cycles_
  u64 next_event_cycle(u64 limit); // cached full minimum (sleep path)
  u64 next_nontimer_event_cycle(u64 limit) const; // SCI/MIDI/ADC only
  void timer_advance(int ch, u64 now); // bulk/lazy timer advance (H2B)

  // ---- transpiled instruction semantics + arithmetic helpers ----
#include "mu50h8_helpers.inc"
#include "mu50h8_semantics.inc"

  typedef void (Mu50H8::*ExecFn)();
  static const ExecFn kExecFns[];
};

} // namespace mu50h8
