// mu50h8.cpp -- standalone H8/300H interpreter for the MU50 engine.
// See mu50h8.h for the model. MAME-derived parts (instruction semantics,
// arithmetic helpers) carry their BSD-3-Clause notice in the generated
// files; the bus/peripheral code below is original.
#include "synth/mu50h8.h"
#include "synth/mu50h8_opcodes.h"
#include <cstdio>
#include <cstring>

namespace mu50h8 {

namespace {
// Internal peripheral map base (H8/3003, advanced mode, a20 off).
constexpr u32 kPerBase = 0xff0000;
// H8/3003 internal map (from MAME h83003.cpp map()).
constexpr u32 kIramLo = 0xfffd10, kIramHi = 0xffff0f;
constexpr u32 kRomLo = 0x000000, kRomHi = 0x07ffff;
constexpr u32 kRamLo = 0x200000, kRamHi = 0x20ffff;
constexpr u32 kSwpLo = 0x400000, kSwpHi = 0x4007ff;
// SCI register offsets within each channel block (0xFFFFB0 / 0xFFFFB8).
constexpr u32 kSciSmr = 0, kSciBrr = 1, kSciScr = 2, kSciTdr = 3,
              kSciSsr = 4, kSciRdr = 5, kSciScmr = 6;
// SCI SMR bits
constexpr u8 kSmrCa = 0x80, kSmrChr = 0x40, kSmrPe = 0x20, kSmrOe = 0x10,
             kSmrStop = 0x08, kSmrCks = 0x03;
// SCI SCR bits
constexpr u8 kScrTie = 0x80, kScrRie = 0x40, kScrTe = 0x20, kScrRe = 0x10,
             kScrTeie = 0x04, kScrCke1 = 0x02, kScrCke0 = 0x01;
// SCI SSR bits
constexpr u8 kSsrTdre = 0x80, kSsrRdrf = 0x40, kSsrOrer = 0x20,
             kSsrFer = 0x10, kSsrPer = 0x08, kSsrTend = 0x04,
             kSsrMpb = 0x02, kSsrMpbt = 0x01;
// SCI states
enum { kStIdle = 0, kStStart, kStBit, kStParity, kStStop, kStLast };
// Timer16 register layout per channel (see mu50h8.h Timer).
constexpr u32 kTmrTstr = 0xffff60, kTmrTsyr = 0xffff61, kTmrTmdr = 0xffff62,
              kTmrTfcr = 0xffff63;
// ADC interrupt vector: H8/3003 ADI. (MAME's driver passes 60 to the
// ADC device; the datasheet vector table puts ADI at 23. The firmware's
// ADIE use -- if any -- decides this; see the access log.)
constexpr int kAdcVector = 23;
} // namespace

Mu50H8::Mu50H8(const u8 *rom, const Hooks &hooks)
    : pc_(0), ccr_(0), exr_(0), syscr_(0x09), cycles_(0),
      tmp1_(0), tmp2_(0), tmp3_(0), taken_vector_(0),
      sleeping_(false), halted_(false), illegal_pc_(0), instr_count_(0),
      instr_start_pc_(0), rom_(rom), hooks_(hooks),
      intc_pending_(0), intc_ier_(0), intc_isr_(0), intc_icr_(0),
      intc_iscr_(0), irq_vector_(0), irq_filter_(2),
      midi_q_head_(0), midi_q_tail_(0), midi_bit_(-2), midi_byte_(0),
      midi_edge_(0), tstr_(0), tsyr_(0), tmdr_(0), tfcr_(0), toer_(0),
      tocr_(0), adcsr_(0), adcr_(0), adc_done_(0), lcd_rs_(0), p6_cur_(0xff) {
  std::memset(r_, 0, sizeof r_);
  std::memset(ir_, 0, sizeof ir_);
  std::memset(ram_, 0, sizeof ram_);
  std::memset(iram_, 0, sizeof iram_);
  std::memset(port_ddr_, 0, sizeof port_ddr_);
  std::memset(port_dr_, 0, sizeof port_dr_);
  std::memset(addr_, 0, sizeof addr_);
  std::memset(wdt_, 0, sizeof wdt_);
  std::memset(refresh_, 0, sizeof refresh_);
  std::memset(dma_, 0, sizeof dma_);
  std::memset(midi_queue_, 0, sizeof midi_queue_);
  // Reset values from MAME h83003 device_add_mconfig (DDR, DR).
  const u8 ddr[16] = {0, 0, 0, 0, 0x00, 0x0f, 0x80, 0x00,
                      0xf0, 0x00, 0x00, 0x00, 0x00, 0, 0, 0};
  const u8 dr[16] = {0, 0, 0, 0, 0x00, 0x00, 0x80, 0x00,
                     0xe0, 0xc0, 0x00, 0x00, 0x00, 0, 0, 0};
  // Ports are indexed 4,5,6,7,8,9,A,B,C (we keep slots 0-15, use 4-12).
  for (int p = 4; p <= 12; p++) {
    port_ddr_[p] = ddr[p];
    port_dr_[p] = dr[p];
  }
  sci_reset(sci_[0], 52, 53, 54, 55);
  sci_reset(sci_[1], 56, 57, 58, 59);
  const int bases[5] = {24, 28, 32, 36, 40};
  for (int i = 0; i < 5; i++) {
    Timer &t = timer_[i];
    t.tcnt = t.tgra = t.tgrb = t.tbra = t.tbrb = 0;
    t.tcr = t.tior = t.tier = 0;
    t.tsr = 0xf8; // H8H TSR reads 0xf8 with no flags
    t.running = 0;
    t.base = bases[i];
    t.next_inc = 0;
    t.divider = 1;
  }
}

void Mu50H8::logf(const char *fmt, u32 a, u32 v) {
  if (!hooks_.log) return;
  char buf[160];
  std::snprintf(buf, sizeof buf, fmt, a, v);
  hooks_.log(hooks_.ctx, buf);
}

void Mu50H8::reset() {
  std::memset(r_, 0, sizeof r_);
  std::memset(ram_, 0, sizeof ram_);
  std::memset(iram_, 0, sizeof iram_);
  std::memset(ir_, 0, sizeof ir_);
  // MAME reset state: CCR |= F_I (has_hc), EXR = EXR_I | EXR_NC,
  // PC = 32-bit vector at address 0 (advanced mode).
  ccr_ |= F_I;
  exr_ = EXR_I | EXR_NC;
  syscr_ = 0x09;
  cycles_ = 0;
  sleeping_ = false;
  halted_ = false;
  illegal_pc_ = 0;
  instr_count_ = 0;
  taken_vector_ = 0;
  intc_pending_ = 0;
  intc_ier_ = 0;
  intc_isr_ = 0;
  intc_icr_ = 0;
  intc_iscr_ = 0;
  midi_q_head_ = midi_q_tail_ = 0;
  midi_bit_ = -2;
  midi_edge_ = 0;
  tstr_ = tsyr_ = tmdr_ = tfcr_ = toer_ = tocr_ = 0;
  p6_cur_ = 0xff; // MAME machine_start: cur_p6 = 0xff
  adcsr_ = adcr_ = 0;
  adc_done_ = 0;
  sci_reset(sci_[0], 52, 53, 54, 55);
  sci_reset(sci_[1], 56, 57, 58, 59);
  for (int i = 0; i < 5; i++) {
    Timer &t = timer_[i];
    t.tcnt = t.tgra = t.tgrb = t.tbra = t.tbrb = 0;
    t.tcr = t.tior = t.tier = 0;
    t.tsr = 0xf8;
    t.running = 0;
    t.next_inc = 0;
    t.divider = 1;
  }
  // Ports reset to their configured DDR/DR values.
  const u8 ddr[16] = {0, 0, 0, 0, 0x00, 0x0f, 0x80, 0x00,
                      0xf0, 0x00, 0x00, 0x00, 0x00, 0, 0, 0};
  const u8 dr[16] = {0, 0, 0, 0, 0x00, 0x00, 0x80, 0x00,
                     0xe0, 0xc0, 0x00, 0x00, 0x00, 0, 0, 0};
  for (int q = 4; q <= 12; q++) {
    port_ddr_[q] = ddr[q];
    port_dr_[q] = dr[q];
  }
  pcr4_ = 0;
  std::memset(addr_, 0, sizeof addr_);
  u32 pc = ((u32)bus_read16(0) << 16) | bus_read16(2);
  pc_ = pc & 0xffffff;
  update_irq_filter();
}

void Mu50H8::sci_reset(Sci &s, int eri, int rxi, int txi, int tei) {
  s.smr = 0;
  s.brr = 0xff;
  s.scr = 0;
  s.tdr = 0xff;
  s.ssr = 0x84; // TDRE + TEND
  s.ssr_read = 0;
  s.rdr = 0;
  s.rsr = 0;
  s.tsr = 0xff;
  s.rx_bit = s.tx_bit = 0;
  s.rx_parity = s.tx_parity = 0;
  s.rx_state = s.tx_state = kStIdle;
  s.rx_counter = s.tx_counter = 0;
  s.divider = 2 << (2 * (s.smr & kSmrCks));
  s.divider *= (u32)s.brr + 1;
  s.next_tick = 0;
  s.rx_clock_on = s.tx_clock_on = false;
  s.rx_value = 1;
  s.eri = eri;
  s.rxi = rxi;
  s.txi = txi;
  s.tei = tei;
}

// ---------------- INTC ----------------

int Mu50H8::intc_slot(int vector) {
  // MAME h8_intc.cpp vector_to_slot table (H8H).
  static const int8_t table[64] = {
    -1, -1, -1, -1, -1, -1, -1, -1, // 0-7 (7 = NMI, handled separately)
    -1, -1, -1, -1, 0, 1, 2, 2,     // 8-15 (IRQ0-3)
    3, 3, 3, 3, 4, 4, 4, 4,         // 16-23
    5, 5, 5, 5, 6, 6, 6, 6,         // 24-31
    7, 7, 7, 7, 8, 8, 8, 8,         // 32-39
    9, 9, 9, 9, 10, 10, 10, 10,     // 40-47
    11, 11, 11, 11, 12, 12, 12, 12, // 48-55
    13, 13, 13, 13, 14, 14, 14, 14  // 56-63
  };
  return table[vector & 63];
}

void Mu50H8::intc_update() {
  // External IRQ inputs are inactive, so ISR bits only reflect what the
  // firmware wrote (edge-latched) -- but with no input edges, external
  // pending bits come from ISR & IER only if firmware set them; MAME
  // latches ISR on input edges, which never happen here. Keep the
  // general form: pending |= (ISR & IER) mapped to vectors 12-19.
  u64 pending = intc_pending_;
  u8 ext = intc_isr_ & intc_ier_;
  for (int i = 0; i < 8; i++)
    if (ext & (1 << i)) pending |= (u64)1 << (12 + i);
    else pending &= ~((u64)1 << (12 + i));
  // Select: highest icr_pri >= filter; ties -> lowest vector.
  int best = 0, best_pri = -1;
  for (int v = 1; v < 64; v++) {
    if (!(pending & ((u64)1 << v))) continue;
    int pri;
    if (v == 7) pri = 2; // NMI
    else {
      int slot = intc_slot(v);
      if (slot < 0) pri = 0;
      else pri = (intc_icr_ >> (slot ^ 7)) & 1;
    }
    if (pri >= irq_filter_ && pri > best_pri) {
      best = v;
      best_pri = pri;
    }
  }
  irq_vector_ = best;
}

void Mu50H8::intc_raise(int vector) {
  intc_pending_ |= (u64)1 << vector;
  intc_update();
}

void Mu50H8::update_irq_filter() {
  // h83003 update_irq_filter (SYSCR INTM = bit 3).
  if (syscr_ & 0x08) {
    irq_filter_ = (ccr_ & F_I) ? 2 : 0;
  } else {
    if ((ccr_ & (F_I | F_UI)) == (F_I | F_UI)) irq_filter_ = 2;
    else if (ccr_ & F_I) irq_filter_ = 1;
    else irq_filter_ = 0;
  }
  intc_update();
}

void Mu50H8::interrupt_taken() {
  int v = taken_vector_;
  intc_pending_ &= ~((u64)1 << v);
  if (v >= 12 && v < 20) {
    // External IRQ: clear ISR unless the (inactive) input still holds it.
    intc_isr_ &= ~(1 << (v - 12));
  }
  intc_update();
}

void Mu50H8::take_irq() {
  // TEMP-DEBUG: IRQ vectors taken (remove after MIDI diagnosis).
  if (irq_vector_ == 56 || irq_vector_ == 57)
    logf("IRQ take vec %02x", (u32)irq_vector_, 0);
  int v = irq_vector_;
  taken_vector_ = v;
  // H8H IRQ frame (exr_in_stack() == false): NPC low word, then
  // CCR + NPC high word. NPC = next instruction (pc_ is post-fetch).
  cycles_ += 2; // internal(1)
  u32 sp = r32_r(7) - 2;
  r32_w(7, sp);
  bus_write16(sp, (u16)pc_);
  sp = r32_r(7) - 2;
  r32_w(7, sp);
  bus_write16(sp, (u16)((ccr_ << 8) | ((pc_ >> 16) & 0xff)));
  u32 vec;
  if (v == 7) {
    // NMI vector: H8/3003 NMI is vector 7 -> address 28.
    vec = ((u32)bus_read16(4 * 7) << 16) | bus_read16(4 * 7 + 2);
  } else {
    vec = ((u32)bus_read16(4 * v) << 16) | bus_read16(4 * v + 2);
  }
  cycles_ += 2; // internal(1)
  irq_setup();
  update_irq_filter();
  interrupt_taken();
  pc_ = vec & 0xffffff;
  // No discarded-prefetch charge: MAME state_irq prefetches at the vector
  // target (m_PIR = read16i), which the next instruction's fresh w[0] fetch
  // already charges. (Verified against generated h8h.hxx state_irq_full.)
  sleeping_ = false;
}

// ---------------- bus ----------------

u8 Mu50H8::bus_read8(u32 a) {
  a &= 0xffffff;
  cycles_ += 2;
  if (a >= kRomLo && a <= kRomHi) return rom_[a ^ 1];
  if (a >= kRamLo && a <= kRamHi) return ram_[a - kRamLo];
  if (a >= kSwpLo && a <= kSwpHi) {
    u16 off = (u16)(a - kSwpLo);
    return hooks_.swp_read ? hooks_.swp_read(hooks_.ctx, off) : 0;
  }
  if (a >= kPerBase) return bus_read8_per(a);
  logf("unhandled read8 %06x", a, 0);
  return 0xff;
}

u16 Mu50H8::bus_read16(u32 a) {
  a &= 0xffffff;
  a &= ~1u;
  cycles_ += 2;
  if (a >= kRomLo && a <= kRomHi) {
    // Word read from the word-swapped ROM view.
    return (u16)((rom_[a ^ 1] << 8) | rom_[(a + 1) ^ 1]);
  }
  if (a >= kRamLo && a <= kRamHi) {
    return (u16)((ram_[a - kRamLo] << 8) | ram_[a - kRamLo + 1]);
  }
  if (a >= kSwpLo && a <= kSwpHi) {
    u16 off = (u16)(a - kSwpLo);
    u8 hi = hooks_.swp_read ? hooks_.swp_read(hooks_.ctx, off) : 0;
    u8 lo = hooks_.swp_read ? hooks_.swp_read(hooks_.ctx, off + 1) : 0;
    return (u16)((hi << 8) | lo);
  }
  if (a >= kPerBase) return bus_read16_per(a);
  logf("unhandled read16 %06x", a, 0);
  return 0xffff;
}

void Mu50H8::bus_write8(u32 a, u8 v) {
  a &= 0xffffff;
  cycles_ += 2;
  if (a >= kRamLo && a <= kRamHi) {
    ram_[a - kRamLo] = v;
    return;
  }
  if (a >= kSwpLo && a <= kSwpHi) {
    if (hooks_.swp_write) hooks_.swp_write(hooks_.ctx, cycles_, (u16)(a - kSwpLo), v);
    return;
  }
  if (a >= kRomLo && a <= kRomHi) {
    logf("write8 to ROM %06x=%02x", a, v);
    return;
  }
  if (a >= kPerBase) {
    bus_write8_per(a, v);
    return;
  }
  logf("unhandled write8 %06x=%02x", a, v);
}

void Mu50H8::bus_write16(u32 a, u16 v) {
  a &= 0xffffff;
  a &= ~1u;
  cycles_ += 2;
  if (a >= kRamLo && a <= kRamHi) {
    ram_[a - kRamLo] = (u8)(v >> 8);
    ram_[a - kRamLo + 1] = (u8)v;
    return;
  }
  if (a >= kSwpLo && a <= kSwpHi) {
    if (hooks_.swp_write) {
      hooks_.swp_write(hooks_.ctx, cycles_, (u16)(a - kSwpLo), (u8)(v >> 8));
      hooks_.swp_write(hooks_.ctx, cycles_, (u16)(a - kSwpLo + 1), (u8)v);
    }
    return;
  }
  if (a >= kRomLo && a <= kRomHi) {
    logf("write16 to ROM %06x=%04x", a, v);
    return;
  }
  if (a >= kPerBase) {
    bus_write16_per(a, v);
    return;
  }
  logf("unhandled write16 %06x=%04x", a, v);
}

// ---------------- peripheral dispatch ----------------
// Channel bases (TCR offset 0) for the 5 timer16 channels.
static const u32 kTimerBase[5] = {0xffff64, 0xffff6e, 0xffff78, 0xffff82,
                                  0xffff92};

u8 Mu50H8::bus_read8_per(u32 a) {
  // DMA channel registers: storage + log (DMA not implemented; the
  // firmware is not expected to use it -- the log proves it).
  if (a >= 0xfffd20 && a <= 0xfffd5f) {
    return (u8)(dma_[a - 0xfffd20] & 0xff);
  }
  // Timer block.
  if (a == 0xffff60) return tstr_;
  if (a == 0xffff61) return tsyr_;
  if (a == 0xffff62) return tmdr_;
  if (a == 0xffff63) return tfcr_;
  if (a == 0xffff90) return toer_;
  if (a == 0xffff91) return tocr_;
  for (int ch = 0; ch < 5; ch++) {
    u32 b = kTimerBase[ch];
    if (a >= b && a <= b + 3) return timer_read(ch, a - b);
    if (a == b + 4) return (u8)(timer_[ch].tcnt >> 8);
    if (a == b + 5) return (u8)timer_[ch].tcnt;
    if (a == b + 6) return (u8)(timer_[ch].tgra >> 8);
    if (a == b + 7) return (u8)timer_[ch].tgra;
    if (a == b + 8) return (u8)(timer_[ch].tgrb >> 8);
    if (a == b + 9) return (u8)timer_[ch].tgrb;
  }
  // ch3/ch4 TBR (0x8c-0x8f, 0x9c-0x9f).
  if (a == 0xffff8c) return (u8)(timer_[3].tbra >> 8);
  if (a == 0xffff8d) return (u8)timer_[3].tbra;
  if (a == 0xffff8e) return (u8)(timer_[3].tbrb >> 8);
  if (a == 0xffff8f) return (u8)timer_[3].tbrb;
  if (a == 0xffff9c) return (u8)(timer_[4].tbra >> 8);
  if (a == 0xffff9d) return (u8)timer_[4].tbra;
  if (a == 0xffff9e) return (u8)(timer_[4].tbrb >> 8);
  if (a == 0xffff9f) return (u8)timer_[4].tbrb;
  // Watchdog / refresh: storage.
  if (a >= 0xffffa8 && a <= 0xffffab) return wdt_[a - 0xffffa8];
  if (a >= 0xffffad && a <= 0xffffaf) return refresh_[a - 0xffffad];
  // SCI.
  for (int id = 0; id < 2; id++) {
    u32 b = id == 0 ? 0xffffb0 : 0xffffb8;
    if (a >= b && a <= b + 5) return sci_read(sci_[id], a - b);
  }
  // Ports.
  if (a == 0xffffc5 || a == 0xffffc8 || a == 0xffffc9 || a == 0xffffcd ||
      a == 0xffffd0 || a == 0xffffd1 || a == 0xffffd4 || a == 0xffffd5)
    return 0xff; // DDR reads (MAME ff_r)
  if (a == 0xffffc7) return port_read(4);
  if (a == 0xffffca) return port_read(5);
  if (a == 0xffffcb) return port_read(6);
  if (a == 0xffffce) return port_read(7);
  if (a == 0xffffcf) return port_read(8);
  if (a == 0xffffd2) return port_read(9);
  if (a == 0xffffd3) return port_read(10);
  if (a == 0xffffd6) return port_read(11);
  if (a == 0xffffd7) return port_read(12);
  if (a == 0xffffda) return pcr4_;
  // ADC.
  if (a >= 0xffffe0 && a <= 0xffffe7) return adc_read(a - 0xffffe0);
  if (a == 0xffffe8) return adcsr_;
  if (a == 0xffffe9) return adcr_;
  // SYSCR / INTC.
  if (a == 0xfffff2) return syscr_;
  if (a == 0xfffff4) return (u8)intc_iscr_;
  if (a == 0xfffff5) return intc_ier_;
  if (a == 0xfffff6) return intc_isr_;
  if (a == 0xfffff8) return (u8)(intc_icr_ >> 8);
  if (a == 0xfffff9) return (u8)intc_icr_;
  // Internal RAM (peripherals above override it).
  if (a >= kIramLo && a <= kIramHi) return iram_[a - kIramLo];
  logf("unhandled per read8 %06x", a, 0);
  return 0xff;
}

u16 Mu50H8::bus_read16_per(u32 a) {
  // 16-bit peripheral reads: ICR, ISCR (treated as bytes), timer 16-bit
  // regs, ADC (via two byte reads, as MAME's 8-bit map gives), else two
  // byte reads combined big-endian.
  if (a == 0xfffff8) return intc_icr_;
  if (a == 0xfffff4) return intc_iscr_;
  for (int ch = 0; ch < 5; ch++) {
    u32 b = kTimerBase[ch];
    if (a == b + 4) return timer_[ch].tcnt;
    if (a == b + 6) return timer_[ch].tgra;
    if (a == b + 8) return timer_[ch].tgrb;
  }
  if (a == 0xffff8c) return timer_[3].tbra;
  if (a == 0xffff8e) return timer_[3].tbrb;
  if (a == 0xffff9c) return timer_[4].tbra;
  if (a == 0xffff9e) return timer_[4].tbrb;
  if (a == 0xffffa8) return (u16)((wdt_[0] << 8) | wdt_[1]);
  if (a == 0xffffaa) return (u16)((wdt_[2] << 8) | wdt_[3]);
  return (u16)((bus_read8_per(a) << 8) | bus_read8_per(a + 1));
}

void Mu50H8::bus_write8_per(u32 a, u8 v) {
  if (a >= 0xfffd20 && a <= 0xfffd5f) {
    if (v) logf("DMA write %06x=%02x (DMA not implemented)", a, v);
    dma_[a - 0xfffd20] = v;
    return;
  }
  if (a == 0xffff60) {
    u8 changed = tstr_ ^ v;
    tstr_ = v;
    logf("TSTR=%02x", 0, (u32)v);  // TEMP-DEBUG
    for (int ch = 0; ch < 5; ch++) {
      timer_[ch].running = (v >> ch) & 1;
      if ((changed >> ch) & 1) {
        // divider == 0 means external clock: no autonomous increments,
        // so never schedule a (zero-progress) event at the current cycle.
        if (timer_[ch].running && timer_[ch].divider)
          timer_[ch].next_inc = cycles_ + timer_[ch].divider;
        else timer_[ch].next_inc = 0;
      }
    }
    return;
  }
  if (a == 0xffff61) { tsyr_ = v; return; }
  if (a == 0xffff62) {
    tmdr_ = v;
    if (v) logf("TMDR=%02x (non-normal timer mode, ignored)", a, v);
    return;
  }
  if (a == 0xffff63) { tfcr_ = v; return; }
  if (a == 0xffff90) { toer_ = v; return; }
  if (a == 0xffff91) { tocr_ = v; return; }
  for (int ch = 0; ch < 5; ch++) {
    u32 b = kTimerBase[ch];
    if (a >= b && a <= b + 3) {
      timer_write(ch, a - b, v);
      return;
    }
    if (a == b + 4) { timer_[ch].tcnt = (u16)((v << 8) | (timer_[ch].tcnt & 0xff)); return; }
    if (a == b + 5) { timer_[ch].tcnt = (u16)((timer_[ch].tcnt & 0xff00) | v); return; }
    if (a == b + 6) { timer_[ch].tgra = (u16)((v << 8) | (timer_[ch].tgra & 0xff)); return; }
    if (a == b + 7) { timer_[ch].tgra = (u16)((timer_[ch].tgra & 0xff00) | v); return; }
    if (a == b + 8) { timer_[ch].tgrb = (u16)((v << 8) | (timer_[ch].tgrb & 0xff)); return; }
    if (a == b + 9) { timer_[ch].tgrb = (u16)((timer_[ch].tgrb & 0xff00) | v); return; }
  }
  if (a == 0xffff8c) { timer_[3].tbra = (u16)((v << 8) | (timer_[3].tbra & 0xff)); return; }
  if (a == 0xffff8d) { timer_[3].tbra = (u16)((timer_[3].tbra & 0xff00) | v); return; }
  if (a == 0xffff8e) { timer_[3].tbrb = (u16)((v << 8) | (timer_[3].tbrb & 0xff)); return; }
  if (a == 0xffff8f) { timer_[3].tbrb = (u16)((timer_[3].tbrb & 0xff00) | v); return; }
  if (a == 0xffff9c) { timer_[4].tbra = (u16)((v << 8) | (timer_[4].tbra & 0xff)); return; }
  if (a == 0xffff9d) { timer_[4].tbra = (u16)((timer_[4].tbra & 0xff00) | v); return; }
  if (a == 0xffff9e) { timer_[4].tbrb = (u16)((v << 8) | (timer_[4].tbrb & 0xff)); return; }
  if (a == 0xffff9f) { timer_[4].tbrb = (u16)((timer_[4].tbrb & 0xff00) | v); return; }
  if (a >= 0xffffa8 && a <= 0xffffab) {
    wdt_[a - 0xffffa8] = v;
    if (a == 0xffffa8 && v) logf("watchdog write %06x=%02x", a, v);
    return;
  }
  if (a >= 0xffffad && a <= 0xffffaf) {
    refresh_[a - 0xffffad] = v;
    return;
  }
  for (int id = 0; id < 2; id++) {
    u32 b = id == 0 ? 0xffffb0 : 0xffffb8;
    if (a >= b && a <= b + 5) {
      sci_write(sci_[id], a - b, v);
      return;
    }
  }
  if (a == 0xffffc5) { port_ddr_[4] = v; return; }
  if (a == 0xffffc7) { port_write(4, v); return; }
  if (a == 0xffffc8) { port_ddr_[5] = v; return; }
  if (a == 0xffffc9) { port_ddr_[6] = v; return; }
  if (a == 0xffffca) { port_write(5, v); return; }
  if (a == 0xffffcb) { port_write(6, v); return; }
  if (a == 0xffffcd) { port_ddr_[8] = v; return; }
  if (a == 0xffffcf) { port_write(8, v); return; }
  if (a == 0xffffd0) { port_ddr_[9] = v; return; }
  if (a == 0xffffd1) { port_ddr_[10] = v; return; }
  if (a == 0xffffd2) { port_write(9, v); return; }
  if (a == 0xffffd3) { port_write(10, v); return; }
  if (a == 0xffffd4) { port_ddr_[11] = v; return; }
  if (a == 0xffffd5) { port_ddr_[12] = v; return; }
  if (a == 0xffffd6) { port_write(11, v); return; }
  if (a == 0xffffd7) { port_write(12, v); return; }
  if (a == 0xffffda) { pcr4_ = v; return; }
  if (a >= 0xffffe0 && a <= 0xffffe7) {
    logf("write to ADC ADDR %06x=%02x", a, v);
    return;
  }
  if (a == 0xffffe8) { adc_write(0, v); return; }
  if (a == 0xffffe9) { adcr_ = v; return; }
  if (a == 0xfffff2) {
    syscr_ = v;
    update_irq_filter();
    return;
  }
  if (a == 0xfffff4) { intc_iscr_ = v; intc_update(); return; }
  if (a == 0xfffff5) { intc_ier_ = v; intc_update(); return; }
  if (a == 0xfffff6) { intc_isr_ &= v; intc_update(); return; }
  if (a == 0xfffff8) { intc_icr_ = (u16)((v << 8) | (intc_icr_ & 0xff)); intc_update(); return; }
  if (a == 0xfffff9) { intc_icr_ = (u16)((intc_icr_ & 0xff00) | v); intc_update(); return; }
  if (a >= kIramLo && a <= kIramHi) {
    iram_[a - kIramLo] = v;
    return;
  }
  logf("unhandled per write8 %06x=%02x", a, v);
}

void Mu50H8::bus_write16_per(u32 a, u16 v) {
  if (a == 0xfffff8) {
    intc_icr_ = v;
    intc_update();
    return;
  }
  if (a == 0xfffff4) {
    intc_iscr_ = (u8)(v >> 8);
    intc_update();
    return;
  }
  for (int ch = 0; ch < 5; ch++) {
    u32 b = kTimerBase[ch];
    if (a == b + 4) { timer_[ch].tcnt = v; return; }
    if (a == b + 6) { timer_[ch].tgra = v; return; }
    if (a == b + 8) { timer_[ch].tgrb = v; return; }
  }
  if (a == 0xffff8c) { timer_[3].tbra = v; return; }
  if (a == 0xffff8e) { timer_[3].tbrb = v; return; }
  if (a == 0xffff9c) { timer_[4].tbra = v; return; }
  if (a == 0xffff9e) { timer_[4].tbrb = v; return; }
  if (a == 0xffffa8) { wdt_[0] = (u8)(v >> 8); wdt_[1] = (u8)v; return; }
  if (a == 0xffffaa) { wdt_[2] = (u8)(v >> 8); wdt_[3] = (u8)v; return; }
  bus_write8_per(a, (u8)(v >> 8));
  bus_write8_per(a + 1, (u8)v);
}

// ---------------- ports / panel stub ----------------

u8 Mu50H8::port_read(int p) {
  switch (p) {
    case 4: case 5: case 8:
      return (u8)((port_dr_[p] & port_ddr_[p]) | (0xff & ~port_ddr_[p]));
    case 6:
      // MAME port_r: output bits from m_dr, input bits from the
      // driver (cur_p6). DDR reads return 0xff here, so a DDR of 0
      // means "input" for this combination.
      return (u8)((port_dr_[6] & port_ddr_[6]) | (p6_cur_ & ~port_ddr_[6]));
    case 7:
      return 0x00; // driver constant
    case 9:
      return (u8)(port_dr_[9] & port_ddr_[9]); // driver input = 0
    case 10: { // PA: driver pa_r, combined with DDR like port_r.
      // Driver: stored ENABLE set -> LCD read (stub: never busy, so
      // 0x00, bit 7 clear); stored ENABLE clear -> cur_pa, which the
      // driver tracks as the last PA output data (m_dr | ~ddr).
      u8 drv = (p6_cur_ & 0x01)
                   ? 0x00
                   : (u8)(port_dr_[10] | ~port_ddr_[10]);
      return (u8)((port_dr_[10] & port_ddr_[10]) | (drv & ~port_ddr_[10]));
    }
    case 11:
      return port_dr_[11]; // driver returns stored PB (LEDs)
    case 12:
      // Driver: cur_pc | 0x7c, ANDed with button ioports (0xFF unpressed).
      return (u8)(port_dr_[12] | 0x7c);
    default:
      return 0xff;
  }
}

void Mu50H8::port_write(int p, u8 v) {
  if (p == 6) {
    // MAME two-layer model: the port device stores m_dr, then
    // update_output() delivers (m_dr | ~ddr) to the driver, whose
    // p6_w XORs ENABLE (the hardware inverts it) and stores cur_p6.
    // On a falling edge of the STORED ENABLE bit with stored RW=0,
    // PA is latched into the LCD (stubbed: ignored; stub never busy).
    u8 old = p6_cur_;
    port_dr_[6] = v;
    u8 out = (u8)(v | ~port_ddr_[6]);
    u8 ns = (u8)(out ^ 0x01);
    p6_cur_ = ns;
    if ((old & 0x01) && !(ns & 0x01) && !(old & 0x02))
      lcd_rs_ = (ns >> 2) & 1; // LCD latch (stubbed)
    return;
  }
  port_dr_[p] = v;
}

// ---------------- timer16 ----------------

u8 Mu50H8::timer_read(int ch, u32 off) {
  Timer &t = timer_[ch];
  switch (off) {
    case 0: return t.tcr;
    case 1: return t.tior;
    case 2: return (u8)(t.tier | 0xf8); // H8H TIER reads high bits set
    case 3: return (u8)(0xf8 | (t.tsr & 0x07));
    default: return 0xff;
  }
}

void Mu50H8::timer_update_dividers(int ch) {
  Timer &t = timer_[ch];
  int ct = t.tcr & 7;
  if (ct < 4) {
    // H8H: divider = 2^ct (ct 0..3 -> /1, /2, /4, /8; phase bits 4-3
    // shift the first edge by half a period for ct 2/3 -- ignored,
    // sub-microsecond, invisible to firmware-visible timing).
    t.divider = 1u << ct;
  } else {
    t.divider = 0; // external clock input: no autonomous increments
  }
  if (t.running && t.divider) t.next_inc = cycles_ + t.divider;
  else if (!t.divider) t.next_inc = 0;
}

void Mu50H8::timer_write(int ch, u32 off, u8 v) {
  // TEMP-DEBUG: timer writes.
  logf("TIMW %04x=%02x", (u32)((ch << 8) | off), (u32)v);
  Timer &t = timer_[ch];
  switch (off) {
    case 0: // TCR
      t.tcr = v;
      timer_update_dividers(ch);
      return;
    case 1: // TIOR (capture/output config; pins unmodeled)
      t.tior = v;
      if (v) logf("TIOR ch%d=%02x (capture/output, pins unmodeled)", ch, v);
      return;
    case 2: // TIER (H8H: bits 0,1,4 = TGIEA/TGIEB/TGIEV)
      t.tier = (u8)(v & 0x07);
      return;
    case 3: // TSR write: clear flags where the written bit is 0
      if (!(v & 1)) t.tsr &= ~0x01;
      if (!(v & 2)) t.tsr &= ~0x02;
      if (!(v & 4)) t.tsr &= ~0x04;
      return;
    default: return;
  }
}

void Mu50H8::timer_inc(int ch) {
  Timer &t = timer_[ch];
  if (!t.running || !t.divider) { t.next_inc = 0; return; }
  u16 old = t.tcnt;
  t.tcnt = (u16)(t.tcnt + 1);
  bool wrapped = (old == 0xffff);
  int cclr = (t.tcr >> 5) & 3; // 0 none, 1 TGRA, 2 TGRB, 3 external
  bool cleared = false;
  if (t.tcnt == t.tgra) {
    t.tsr |= 0x01;
    if (t.tier & 0x01) intc_raise(t.base);
    if (cclr == 1) { t.tcnt = 0; cleared = true; }
  }
  if (!cleared && t.tcnt == t.tgrb) {
    t.tsr |= 0x02;
    if (t.tier & 0x02) intc_raise(t.base + 1);
    if (cclr == 2) { t.tcnt = 0; cleared = true; }
  }
  if (wrapped && !cleared) {
    t.tsr |= 0x04;
    if (t.tier & 0x04) intc_raise(t.base + 2);
  }
  t.next_inc = cycles_ + t.divider;
}

// ---------------- ADC ----------------

void Mu50H8::adc_write(u32 off, u8 v) {
  (void)off;
  // ADCSR write: ADF is write-0-to-clear; other bits from data.
  u8 prev = adcsr_;
  adcsr_ = (u8)((v & 0x7f) | (adcsr_ & v & 0x80));
  if ((v & 0x20) && !(prev & 0x20)) {
    // Conversion start (single-shot; scan fills all channels -- the
    // values are constants, so the distinction is invisible).
    adc_done_ = cycles_ + 100;
  }
  if (v & 0x40) logf("ADC ADIE set (vector %d)", 0, (u32)kAdcVector);
}

u8 Mu50H8::adc_read(u32 off) {
  // addr8_r: even offset -> addr[ch] >> 2, odd -> addr[ch] << 6.
  int ch = (int)(off >> 1);
  if (off & 1) return (u8)(addr_[ch] << 6);
  return (u8)(addr_[ch] >> 2);
}

// ---------------- SCI ----------------

void Mu50H8::sci_write(Sci &s, u32 off, u8 v) {
  // TEMP-DEBUG: SCI config writes (remove after MIDI diagnosis).
  if (off <= 2) logf("SCI cfg %04x=%02x", (u32)(((&s == &sci_[1] ? 1 : 0) << 8) | off), (u32)v);
  // TEMP-DEBUG: SCI TX writes (remove after MIDI diagnosis).
  if (off == 3) logf("SCI TDR write %02x", (u32)v, (u32)(&s == &sci_[1] ? 1 : 0));
  switch (off) {
    case kSciSmr:
      s.smr = v;
      s.divider = (u32)(2 << (2 * (v & kSmrCks))) * ((u32)s.brr + 1);
      break;
    case kSciBrr:
      s.brr = v;
      s.divider = (u32)(2 << (2 * (s.smr & kSmrCks))) * ((u32)v + 1);
      break;
    case kSciScr: {
      u8 delta = s.scr ^ v;
      s.scr = v;
      if ((delta & kScrRe) && !(v & kScrRe)) {
        s.rx_state = kStIdle;
        s.rx_clock_on = false;
      }
      if ((delta & kScrTie) && (v & kScrTie) && (s.ssr & kSsrTdre))
        intc_raise(s.txi);
      if ((delta & kScrTeie) && (v & kScrTeie) && (s.ssr & kSsrTend))
        intc_raise(s.tei);
      if ((delta & kScrRie) && (v & kScrRie) && (s.ssr & kSsrRdrf))
        intc_raise(s.rxi);
      if ((delta & kScrRie) && (v & kScrRie) &&
          (s.ssr & (kSsrOrer | kSsrFer | kSsrPer)))
        intc_raise(s.eri);
      break;
    }
    case kSciTdr:
      s.tdr = v; // CPU write never starts TX (only DMA or SSR-clear does)
      break;
    case kSciSsr: {
      // MAME ssr_w: TDRE/TEND clear protocol + flag-clear masking.
      if ((s.scr & kScrTe) && (s.ssr & s.ssr_read & kSsrTdre) &&
          !(v & kSsrTdre))
        s.ssr &= ~(kSsrTdre | kSsrTend);
      s.ssr = (u8)((s.ssr & (~s.ssr_read | v | kSsrTdre | kSsrTend | kSsrMpb) &
                    ~kSsrMpbt) |
                   (v & kSsrMpbt));
      s.ssr_read &= s.ssr;
      if (s.tx_state == kStIdle && !(s.ssr & kSsrTdre)) sci_tx_start(s);
      break;
    }
    default:
      break;
  }
}

u8 Mu50H8::sci_read(Sci &s, u32 off) {
  // TEMP-DEBUG: SCI1 RDR reads (remove after MIDI diagnosis).
  if (off == 5) logf("SCI%d RDR read %02x", (u32)(&s == &sci_[1] ? 1 : 0), (u32)s.rdr);
  switch (off) {
    case kSciSmr: return s.smr;
    case kSciBrr: return s.brr;
    case kSciScr: return s.scr;
    case kSciTdr: return s.tdr;
    case kSciSsr:
      s.ssr_read = s.ssr;
      return s.ssr;
    case kSciRdr: return s.rdr; // CPU read does NOT clear RDRF (SSR-write does)
    default: return 0xff;
  }
}

void Mu50H8::sci_rx_start(Sci &s) {
  s.rx_parity = (s.smr & kSmrOe) ? 0 : 1;
  s.rsr = 0;
  if (s.smr & kSmrCa) {
    s.rx_state = kStBit;
    s.rx_bit = 8;
  } else {
    s.rx_state = kStStart;
    s.rx_bit = 1;
  }
  s.rx_counter = 15;
  s.rx_clock_on = true;
  if (s.next_tick == 0 || s.next_tick < cycles_)
    s.next_tick = cycles_ + s.divider;
}

void Mu50H8::sci_rx_done(Sci &s) {
  if (!(s.ssr & kSsrFer)) {
    if ((s.smr & kSmrPe) && s.rx_parity) {
      s.ssr |= kSsrPer;
    } else if (s.ssr & kSsrRdrf) {
      s.ssr |= kSsrOrer;
    } else {
      s.ssr |= kSsrRdrf;
      s.rdr = s.rsr;
    }
  }
  if (s.scr & kScrRie) {
    if (s.ssr & (kSsrOrer | kSsrFer | kSsrPer)) intc_raise(s.eri);
    else intc_raise(s.rxi);
  }
  s.rx_state = kStIdle;
  s.rx_clock_on = false;
}

void Mu50H8::sci_rx_step(Sci &s) {
  switch (s.rx_state) {
    case kStStart:
      if (s.rx_value) {
        s.rx_state = kStIdle;
        s.rx_clock_on = false;
        break;
      }
      s.rx_state = kStBit;
      s.rx_bit = (s.smr & kSmrChr) ? 7 : 8;
      break;
    case kStBit:
      s.rsr >>= 1;
      if (s.rx_value) {
        s.rx_parity = !s.rx_parity;
        s.rsr |= ((s.smr & (kSmrCa | kSmrChr)) == kSmrChr) ? 0x40 : 0x80;
      }
      if (--s.rx_bit == 0) {
        if (s.smr & kSmrPe) {
          s.rx_state = kStParity;
          s.rx_bit = 1;
        } else {
          s.rx_state = kStStop;
          s.rx_bit = 1;
        }
      }
      break;
    case kStParity:
      s.rx_parity ^= s.rx_value;
      s.rx_state = kStStop;
      s.rx_bit = 1;
      break;
    case kStStop:
      if (!s.rx_value) s.ssr |= kSsrFer;
      else if ((s.smr & kSmrPe) && s.rx_parity) s.ssr |= kSsrPer;
      sci_rx_done(s);
      break;
    default:
      break;
  }
}

void Mu50H8::sci_tx_start(Sci &s) {
  s.ssr |= kSsrTdre;
  s.tsr = s.tdr;
  s.tx_parity = (s.smr & kSmrOe) ? 0 : 1;
  if (s.scr & kScrTie) intc_raise(s.txi);
  if (s.smr & kSmrCa) {
    s.tx_state = kStBit;
    s.tx_bit = 8;
  } else {
    s.tx_state = kStStart;
    s.tx_bit = 1;
  }
  s.tx_counter = 15;
  s.tx_clock_on = true;
  if (s.next_tick == 0 || s.next_tick < cycles_)
    s.next_tick = cycles_ + s.divider;
}

void Mu50H8::sci_tx_step(Sci &s) {
  auto tx_out = [&](int level) {
    // Only SCI1 TX is routed anywhere (firmware MIDI out, via the hook).
    if (&s == &sci_[1] && hooks_.midi_tx && s.tx_state != kStIdle)
      hooks_.midi_tx(hooks_.ctx, (u8)level);
  };
  (void)tx_out; // TX line level is not consumed by the engine; bytes are
  // observed via TDR/TDRE/TEND flags and TXI/TEI interrupts only.
  switch (s.tx_state) {
    case kStStart:
      s.tx_state = kStBit;
      s.tx_bit = (s.smr & kSmrChr) ? 7 : 8;
      break;
    case kStBit:
      s.tx_parity ^= (s.tsr & 1);
      s.tsr >>= 1;
      if (--s.tx_bit == 0) {
        if (s.smr & kSmrCa) {
          if (!(s.ssr & kSsrTdre)) sci_tx_start(s);
          else { s.tx_state = kStLast; s.tx_bit = 0; }
        } else if (s.smr & kSmrPe) {
          s.tx_state = kStParity;
          s.tx_bit = 1;
        } else {
          s.tx_state = kStStop;
          s.tx_bit = (s.smr & kSmrStop) ? 2 : 1;
        }
      }
      break;
    case kStParity:
      s.tx_state = kStStop;
      s.tx_bit = (s.smr & kSmrStop) ? 2 : 1;
      break;
    case kStStop:
      if (--s.tx_bit == 0) {
        if (!(s.ssr & kSsrTdre)) sci_tx_start(s);
        else { s.tx_state = kStLast; s.tx_bit = 0; }
      }
      break;
    case kStLast:
      s.tx_state = kStIdle;
      s.tx_bit = 0;
      s.tx_clock_on = false;
      if (s.scr & kScrTeie) intc_raise(s.tei);
      if ((s.scr & kScrTe) && !(s.ssr & kSsrTdre)) sci_tx_start(s);
      else s.ssr |= kSsrTend;
      break;
    default:
      break;
  }
}

void Mu50H8::sci_tick(Sci &s) {
  // One 16x tick: RX and TX counters advance; a step fires at counter 0.
  if (s.rx_clock_on) {
    s.rx_counter = (s.rx_counter + 1) & 15;
    if (s.rx_counter == 0) sci_rx_step(s);
  }
  if (s.tx_clock_on) {
    s.tx_counter = (s.tx_counter + 1) & 15;
    if (s.tx_counter == 0) sci_tx_step(s);
  }
  // Next tick: divider cycles later if either clock still runs.
  if (s.rx_clock_on || s.tx_clock_on) s.next_tick = cycles_ + s.divider;
  else s.next_tick = 0;
}

void Mu50H8::sci_rx_line(int id, int level) {
  Sci &s = sci_[id];
  if (level != s.rx_value) {
    // MAME do_rx_w resync: an edge while receiving re-syncs the
    // oversample counter (harmless with exact 320-cycle bit times).
    if ((s.rx_clock_on) && (s.rx_counter == 1 || s.rx_counter == 15))
      s.rx_counter = 0;
    if (s.rx_state == kStIdle && !(s.smr & kSmrCa) && (s.scr & kScrRe) &&
        s.rx_value && !level)
      sci_rx_start(s);
  }
  s.rx_value = level;
}

// ---------------- MIDI line driver ----------------

void Mu50H8::queue_midi_byte(u8 b) {
  int next = (midi_q_tail_ + 1) & 255;
  if (next == midi_q_head_) {
    logf("MIDI queue overrun, dropping %02x", 0, b);
    return;
  }
  midi_queue_[midi_q_tail_] = b;
  midi_q_tail_ = next;
  if (midi_bit_ == -2) {
    // Line idle: start this byte now (start bit = 0).
    midi_byte_ = midi_queue_[midi_q_head_];
    midi_q_head_ = (midi_q_head_ + 1) & 255;
    midi_bit_ = -1;
    midi_edge_ = cycles_ + 320;
    sci_rx_line(1, 0);
  }
}

void Mu50H8::midi_line_tick(u64 now) {
  // Advance the MIDI line through bit edges due at or before `now`.
  // Bit time is 320 cycles (31250 baud at 10 MHz). States: -1 =
  // start bit on the line, 0..7 = that data bit on the line, -3 =
  // stop bit on the line, -2 = idle (line high). Each edge advances
  // exactly one bit period: the stop bit gets its full period before
  // the next start bit (a zero-length stop bit would frame-error
  // every back-to-back byte in the SCI receiver).
  while (midi_bit_ != -2 && midi_edge_ <= now) {
    if (midi_bit_ == -1) {
      midi_bit_ = 0; // start bit done -> data bit 0
      sci_rx_line(1, (midi_byte_ >> 0) & 1);
    } else if (midi_bit_ == -3) {
      // Stop bit done -> next byte's start bit, or idle. NOTE: this
      // check must precede the data-bit branch: -3 < 7 is true, and
      // the old order drove a phantom start bit here (the MIDI bug
      // that corrupted every byte after the first).
      if (midi_q_head_ != midi_q_tail_) {
        midi_byte_ = midi_queue_[midi_q_head_];
        midi_q_head_ = (midi_q_head_ + 1) & 255;
        midi_bit_ = -1;
        sci_rx_line(1, 0);
      } else {
        midi_bit_ = -2;
        break;
      }
    } else if (midi_bit_ < 7) {
      midi_bit_++;
      sci_rx_line(1, (midi_byte_ >> midi_bit_) & 1);
    } else { // == 7: data done -> stop bit (1) for a full period
      midi_bit_ = -3;
      sci_rx_line(1, 1);
    }
    midi_edge_ += 320;
  }
}

// ---------------- peripheral event loop ----------------

u64 Mu50H8::next_event_cycle(u64 limit) const {
  u64 best = limit;
  for (int i = 0; i < 2; i++) {
    if (sci_[i].next_tick && sci_[i].next_tick < best) best = sci_[i].next_tick;
  }
  if (midi_bit_ != -2 && midi_edge_ < best) best = midi_edge_;
  for (int ch = 0; ch < 5; ch++) {
    if (timer_[ch].next_inc && timer_[ch].next_inc < best)
      best = timer_[ch].next_inc;
  }
  if (adc_done_ && adc_done_ < best) best = adc_done_;
  return best;
}

void Mu50H8::tick_peripherals() {
  // Process every peripheral event due at or before cycles_, in order.
  // Note: with no events pending, next_event_cycle returns cycles_
  // itself, so "no event fired" is the termination condition, not
  // e == cycles_. A same-cycle re-fire is always a bug (every event
  // either clears itself or reschedules strictly in the future);
  // guard against it rather than spinning forever. Legitimate
  // catch-up is bounded by (max instruction cycles / min divider) x
  // channels, a few hundred at most, so 10000 is a safe backstop.
  int guard = 0;
  for (;;) {
    u64 e = next_event_cycle(cycles_);
    if (e > cycles_) break;
    if (++guard > 10000) {
      logf("tick_peripherals: zero-progress event %06x pc %06x", (u32)e, pc_);
      if (midi_bit_ != -2 && midi_edge_ <= cycles_) midi_bit_ = -2;
      for (int i = 0; i < 2; i++)
        if (sci_[i].next_tick <= cycles_) sci_[i].next_tick = 0;
      for (int ch = 0; ch < 5; ch++)
        if (timer_[ch].next_inc <= cycles_) timer_[ch].next_inc = 0;
      if (adc_done_ && adc_done_ <= cycles_) adc_done_ = 0;
      break;
    }
    // Advance the event clock to e (events observe cycles_ == e).
    u64 saved = cycles_;
    cycles_ = e;
    bool fired = false;
    if (midi_bit_ != -2 && midi_edge_ == e) { midi_line_tick(e); fired = true; }
    for (int i = 0; i < 2; i++)
      if (sci_[i].next_tick == e) { sci_tick(sci_[i]); fired = true; }
    for (int ch = 0; ch < 5; ch++)
      if (timer_[ch].next_inc == e) { timer_inc(ch); fired = true; }
    if (adc_done_ && adc_done_ == e) {
      fired = true;
      adc_done_ = 0;
      // Fixed panel values (MAME ymmu50 driver).
      addr_[0] = 0x3ff;
      addr_[1] = 0x000;
      addr_[2] = 0x3ff;
      addr_[3] = 0x000;
      addr_[4] = 0x000; // MIDI selector: pure MIDI
      addr_[5] = 0x000;
      addr_[6] = 0x200; // battery
      addr_[7] = 0x000;
      adcsr_ |= 0x80; // ADF
      adcsr_ &= ~0x20; // ADST clears on completion
      if (adcsr_ & 0x40) intc_raise(kAdcVector);
    }
    cycles_ = saved;
    // Loop: one event may schedule another at the same cycle. If
    // nothing fired, no event was actually due: stop.
    if (!fired) break;
  }
}

// ---------------- execution ----------------

const Mu50H8::ExecFn Mu50H8::kExecFns[] = {
#include "synth/mu50h8_dispatch.inc"
};

void Mu50H8::execute_one() {
  instr_start_pc_ = pc_;
  // TEMP-DEBUG: trace handler-B callees (remove after MIDI diagnosis).
  if (pc_ == 0x11bf6) {
    u32 sp = r32_r(7);
    u32 ret = ((u32)bus_read16(sp) << 16) | bus_read16(sp + 2);
    logf("DEQ ret=%06x", ret, 0);
  }
  if (pc_ == 0x11720 || pc_ == 0x116b2 || pc_ == 0x116e0 || pc_ == 0x112e4 || pc_ == 0x11560 || pc_ == 0x117cc || pc_ == 0x11838)
    logf("CALL %06x R0=%04x", pc_, r16_r(0));
  // Fetch opcode words on demand (2 cycles each, via the bus). MAME's
  // dispatch guarantees the enabled set is collision-free, so filtering
  // by successive words leaves exactly one entry at each length.
  u16 w[3] = {0, 0, 0};
  w[0] = bus_read16(pc_);
  const Mu50H8Opcode *match = nullptr;
  bool any_long = false;
  for (const auto &op : kMu50H8Opcodes) {
    if ((w[0] & op.mask[0]) != op.val[0]) continue;
    if (op.nwords == 1) { match = &op; break; }
    any_long = true;
  }
  if (match == nullptr && any_long) {
    w[1] = bus_read16(pc_ + 2);
    bool any_longer = false;
    for (const auto &op : kMu50H8Opcodes) {
      if (op.nwords < 2) continue;
      if ((w[0] & op.mask[0]) != op.val[0]) continue;
      if ((w[1] & op.mask[1]) != op.val[1]) continue;
      if (op.nwords == 2) { match = &op; break; }
      any_longer = true;
    }
    if (match == nullptr && any_longer) {
      w[2] = bus_read16(pc_ + 4);
      for (const auto &op : kMu50H8Opcodes) {
        if (op.nwords < 3) continue;
        if ((w[0] & op.mask[0]) != op.val[0]) continue;
        if ((w[1] & op.mask[1]) != op.val[1]) continue;
        if ((w[2] & op.mask[2]) != op.val[2]) continue;
        match = &op;
        break;
      }
    }
  }
  if (match == nullptr) {
    illegal();
    logf("illegal opcode %04x at %06x", w[0], instr_start_pc_);
    return;
  }
  int total = match->nwords + match->extra;
  for (int i = match->nwords; i < total; i++) {
    ir_[i] = bus_read16(pc_ + 2 * i);
  }
  ir_[0] = w[0];
  if (match->nwords >= 2) ir_[1] = w[1];
  if (match->nwords >= 3) ir_[2] = w[2];
  // Upfront fetch complete: pc points past the instruction (== MAME NPC).
  pc_ = (pc_ + 2 * total) & 0xffffff;
  (this->*kExecFns[match->fn])();
  instr_count_++;
  // No discarded-prefetch charge: MAME's fall-through/target fetches are
  // already accounted for — bxx semantics read the target word (read16i)
  // and the next instruction refetches its first word fresh, which together
  // charge exactly MAME's F+T words; jmp/jsr/rts/rte fetch no fall-through
  // word in MAME at all. (Verified against generated h8h.hxx handlers.)
  tick_peripherals();
}

bool Mu50H8::run_until(u64 target) {
  while (cycles_ < target && !halted_) {
    if (sleeping_) {
      if (pending_irq()) {
        sleeping_ = false;
        take_irq();
        continue;
      }
      u64 e = next_event_cycle(target);
      if (e >= target) {
        cycles_ = target;
        tick_peripherals();
        break;
      }
      cycles_ = e;
      tick_peripherals();
      continue;
    }
    if (pending_irq()) {
      take_irq();
      continue;
    }
    execute_one();
  }
  return !halted_;
}

} // namespace mu50h8
