// license:BSD-3-Clause
// Ported from MAME HEAD src/devices/sound/swp00.cpp (Olivier Galibert),
// synthesis overhaul b75872c plus the 84fde62 and 40dd6fc corrections.
// See swp00_0277.hpp for the model and the deliberate differences.
#include "swp00_0277.hpp"

#include <algorithm>

namespace libmu50::detail {

// ---------------------------------------------------------------------------
// interpolation_step
// ---------------------------------------------------------------------------
// A generic interpolation subblock used by envelopes, filter and LFO.
// Provides a step to add/subtract to a counter, depending on a 00-7f
// speed value and the current sample number. 84fde62: for speed >= 0x78
// return 0x1f (was 0x7f); for speed >= 0x58 the step sizes halve.
std::uint16_t Swp0277Core::interpolation_step(u32 speed, u32 sample_counter) noexcept {
    // Phase is incorrect, and very weird
    if (speed >= 0x78)
        return 0x1f;

    u32 k0 = speed >> 3;
    u32 k1 = speed & 7;

    if (speed >= 0x58) {
        k0 -= 10;
        u32 a = (2 << k0) - 1;
        u32 b = (1 << k0) - 1;
        static const u8 mx[8] = { 0x00, 0x80, 0x88, 0xa8, 0xaa, 0xea, 0xee, 0xfe };
        return ((mx[k1] << (sample_counter & 7)) & 0x80) ? static_cast<u16>(a) : static_cast<u16>(b);
    } else {
        k0 = 10 - k0;
        // util::make_bitmask<u32>(k0): k0 low bits set (0 when k0 == 0).
        if (sample_counter & ((k0 == 0) ? 0u : ((1u << k0) - 1)))
            return 0;
        static const u16 mx[8] = { 0xaaaa, 0xeaaa, 0xeaea, 0xeeea, 0xeeee, 0xfeee, 0xfefe, 0xfffe };
        return (mx[k1] << ((sample_counter >> k0) & 0xf)) & 0x8000 ? 1 : 0;
    }
}

// ---------------------------------------------------------------------------
// 0.277 global step table (Route A hybrid only)
// ---------------------------------------------------------------------------
// MAME 0.277's global_step(): the per-sample step for the filter sweep
// timer (and the LFO/envelope linear steps). Used only by the 0.277
// sweep timer in FilterBlock::step when m_filter_0277 is set. Ported
// verbatim from the pre-overhaul 0.277 port.
static std::uint32_t filter_0277_global_step(std::uint32_t i) noexcept {
    std::uint32_t v = 0;
    switch (i >> 3) {
    default: v = static_cast<std::uint32_t>(((i & 7) + 8) << (1 + (i >> 3))); break;
    case 0xb: v = static_cast<std::uint32_t>(((i & 7) + 4) << 13); break;
    case 0xc: v = static_cast<std::uint32_t>(((i & 6) + 6) << 14); break;
    case 0xd: v = static_cast<std::uint32_t>(((i & 4) + 7) << 15); break;
    case 0xe: v = 15 << 15; break;
    case 0xf: v = 31 << 15; break;
    }
    return v;
}

// ---------------------------------------------------------------------------
// ROM access (direct buffer; the region mirrors over the 24-bit bus, and
// our 4 MiB ROM mirrors within itself via the size mask, as before).
// ---------------------------------------------------------------------------
Swp0277Core::u8 Swp0277Core::read_byte(u32 address, const u8* rom, std::size_t rom_size) noexcept {
    if (rom_size == 0)
        return 0;
    return rom[address & static_cast<u32>(rom_size - 1)];
}

Swp0277Core::u16 Swp0277Core::read_word(u32 address, const u8* rom, std::size_t rom_size) noexcept {
    // ENDIANNESS_LITTLE: low byte first.
    return static_cast<u16>(read_byte(address, rom, rom_size) |
                            (static_cast<u16>(read_byte(address + 1, rom, rom_size)) << 8));
}

// ---------------------------------------------------------------------------
// DPCM tables (HEAD streaming_block::dpcm_expand / max_value).
// ---------------------------------------------------------------------------
const std::array<Swp0277Core::s16, 256>& Swp0277Core::dpcm_expand() noexcept {
    static const std::array<s16, 256> table = [] {
        std::array<s16, 256> t{};
        const s16 offset[4] = { 0, 0x20, 0x60, 0xe0 };
        for (u32 i = 0; i != 128; i++) {
            u32 e = i >> 5;
            s16 base = static_cast<s16>(((i & 0x1f) << e) + offset[e]);
            t[i] = base;
            t[i + 128] = static_cast<s16>(-base);
        }
        t[0x80] = 0x88; // Not actually used by samples, but tested on swp30 hardware
        return t;
    }();
    return table;
}

const std::array<Swp0277Core::s32, 8>& Swp0277Core::dpcm_max_value() noexcept {
    static const std::array<s32, 8> table = {
        0x7fff, 0x7ffe, 0x7ffc, 0x7ff8, 0x7ff0, 0x7fe0, 0x7fc0, 0x7f80};
    return table;
}

// ---------------------------------------------------------------------------
// Streaming block
// ---------------------------------------------------------------------------
void Swp0277Core::StreamingBlock::clear() noexcept {
    m_phase = 0x8000;
    m_start = 0;
    m_loop = 0;
    m_address = 0;
    m_pitch = 0;
    m_format = 0;
    m_pos = 0;
    m_pos_dec = 0;
    m_dpcm_s0 = m_dpcm_s1 = 0;
    m_dpcm_pos = 0;
    m_dpcm_delta = 0;
    m_first = false;
    m_done = false;
    m_last = 0;
}

void Swp0277Core::StreamingBlock::keyon() noexcept {
    m_pos = -m_start;
    m_pos_dec = (m_phase << 1) & 0x7ffe;
    m_dpcm_s0 = m_dpcm_s1 = 0;
    m_dpcm_pos = static_cast<u32>(m_pos + 1);
    m_dpcm_delta = 0;
    m_first = true;
    m_done = false;
}

void Swp0277Core::StreamingBlock::read_16(const u8* rom, std::size_t rom_size, s16& val0, s16& val1) noexcept {
    // m_pos can be negative just after keyon; cast to u32 before the
    // shift so the address wraps modularly (same value as MAME's
    // signed arithmetic, without UB).
    u32 adr = m_address + (static_cast<u32>(m_pos) << 1);
    val0 = static_cast<s16>(Swp0277Core::read_word(adr, rom, rom_size));
    val1 = static_cast<s16>(Swp0277Core::read_word(adr + 2, rom, rom_size));
}

void Swp0277Core::StreamingBlock::read_12(const u8* rom, std::size_t rom_size, s16& val0, s16& val1) noexcept {
    u32 adr = m_address + static_cast<u32>((m_pos >> 2) * 6);
    switch (m_pos & 3) {
    case 0: { // bcCa AB.. -> Cabc ..AB
        u16 w0 = Swp0277Core::read_word(adr, rom, rom_size);
        u16 w1 = Swp0277Core::read_word(adr + 2, rom, rom_size);
        val0 = static_cast<s16>(w0 << 4);
        val1 = static_cast<s16>(((w0 >> 8) | (w1 << 8)) & 0xfff0);
        break;
    }
    case 1: { // ..c. abBC .A.. -> c... BCab ...A
        u16 w0 = Swp0277Core::read_word(adr, rom, rom_size);
        u16 w1 = Swp0277Core::read_word(adr + 2, rom, rom_size);
        u16 w2 = Swp0277Core::read_word(adr + 4, rom, rom_size);
        val0 = static_cast<s16>(((w0 >> 8) | (w1 << 8)) & 0xfff0);
        val1 = static_cast<s16>(((w1 >> 4) & 0x0ff0) | (w2 << 12));
        break;
    }
    case 2: { // ..bc CaAB -> bc.. ABCa
        u16 w1 = Swp0277Core::read_word(adr + 2, rom, rom_size);
        u16 w2 = Swp0277Core::read_word(adr + 4, rom, rom_size);
        val0 = static_cast<s16>(((w1 >> 4) & 0x0ff0) | (w2 << 12));
        val1 = static_cast<s16>(w2 & 0xfff0);
        break;
    }
    case 3: { // c.ab BC.A -> abc. .ABC
        u16 w2 = Swp0277Core::read_word(adr + 4, rom, rom_size);
        u16 w3 = Swp0277Core::read_word(adr + 6, rom, rom_size);
        val0 = static_cast<s16>(w2 & 0xfff0);
        val1 = static_cast<s16>(w3 << 4);
        break;
    }
    }
}

void Swp0277Core::StreamingBlock::read_8(const u8* rom, std::size_t rom_size, s16& val0, s16& val1) noexcept {
    u32 adr = m_address + static_cast<u32>(m_pos);
    val0 = static_cast<s16>(Swp0277Core::read_byte(adr, rom, rom_size) << 8);
    val1 = static_cast<s16>(Swp0277Core::read_byte(adr + 1, rom, rom_size) << 8);
}

void Swp0277Core::StreamingBlock::dpcm_step(u8 input) noexcept {
    u32 mode = m_format & 3;
    u32 scale = (m_format >> 2) & 7;
    s32 limit = Swp0277Core::dpcm_max_value()[scale];

    m_dpcm_s0 = m_dpcm_s1;

    s32 delta = m_dpcm_delta + Swp0277Core::dpcm_expand()[input];
    s32 sample = m_dpcm_s1 + (delta << scale);

    if (sample < -0x8000) {
        sample = -0x8000;
        delta = 0;
    } else if (sample > limit) {
        sample = limit;
        delta = 0;
    }
    m_dpcm_s1 = static_cast<s16>(sample);

    switch (mode) {
    case 0: delta = delta * 7 / 8; break;
    case 1: delta = delta * 3 / 4; break;
    case 2: delta = delta / 2; break;
    case 3: delta = 0; break;
    }
    m_dpcm_delta = delta;
}

void Swp0277Core::StreamingBlock::read_8c(const u8* rom, std::size_t rom_size, s16& val0, s16& val1) noexcept {
    while (m_dpcm_pos != static_cast<u32>(m_pos + 2)) {
        dpcm_step(Swp0277Core::read_byte(m_address + m_dpcm_pos, rom, rom_size));
        m_dpcm_pos++;
    }
    val0 = m_dpcm_s0;
    val1 = m_dpcm_s1;
}

std::pair<Swp0277Core::s16, bool> Swp0277Core::StreamingBlock::step(const u8* rom, std::size_t rom_size, s32 fmod) noexcept {
    if (m_done)
        return std::make_pair(m_last, false);

    s16 val0 = 0, val1 = 0;
    switch (m_format >> 6) {
    case 0: read_16(rom, rom_size, val0, val1); break;
    case 1: read_12(rom, rom_size, val0, val1); break;
    case 2: read_8(rom, rom_size, val0, val1); break;
    case 3: read_8c(rom, rom_size, val0, val1); break;
    }

    // 2-point linear interpolation (9-bit multiplier, 15-bit counter).
    u32 step = static_cast<u32>(((m_pitch & 0xfff) << (8 + (static_cast<s16>(m_pitch) >> 12))) >> 4);
    s32 interp = (m_pos_dec >> 6) & 0x1ff;
    // The multiplier has a precision limited to 15 bits.
    s32 result = val0 + ((((val1 - val0) * interp) >> 10) << 1);

    m_pos_dec += static_cast<s32>(step) + (fmod << 4);
    if (m_pos_dec >= 0x8000) {
        m_first = false;
        m_pos += m_pos_dec >> 15;
        m_pos_dec &= 0x7fff;
        if (m_pos >= m_loop) {
            if (m_loop) {
                m_pos -= m_loop;
                if ((m_format & 0xc0) != 0xc0) {
                    m_pos_dec += (m_format << 9) & 0x7e00;
                    if (m_pos_dec >= 0x8000)
                        m_pos++;
                    m_pos_dec &= 0x7fff;
                }
                m_dpcm_pos = 1;
            } else {
                m_done = true;
                m_last = static_cast<s16>(result);
                return std::make_pair(m_last, true);
            }
        }
    }
    return std::make_pair(static_cast<s16>(result), false);
}

// ---------------------------------------------------------------------------
// Envelope block
// ---------------------------------------------------------------------------
// The attenuation is on 12 bits (4 exponent, 8 mantissa) and targets
// are left-shifted and zero-padded by 4 bits. Attack can be normal
// (speed bit 7 set: attenuation falls to 0), timed (speed bit 7 clear
// and starting point 0: attenuation stays at 0 for a time depending
// on the speed) or decay-like (speed bit 7 clear and target != 0).
// Decay is normal (constant speed toward the target) or, when bit 7
// is set and speed >= 0x60, variable speed linear in amplitude space.
void Swp0277Core::EnvelopeBlock::clear() noexcept {
    m_attack_speed = 0;
    m_attack_level = 0;
    m_decay_speed = 0;
    m_decay_level = 0;
    m_envelope_level = 0xfff;
    m_envelope_mode = DECAY_DONE;
}

void Swp0277Core::EnvelopeBlock::keyon() noexcept {
    if (m_decay_speed & 0x80) {
        // Immediate release
        m_envelope_level = 0;
        m_envelope_mode = DECAY;
    } else {
        if (m_attack_speed & 0x80)
            // normal mode
            m_envelope_level = m_attack_level << 4;
        else
            // decay-like or timed mode
            m_envelope_level = 0;
        m_envelope_mode = ATTACK;
    }
}

Swp0277Core::u8 Swp0277Core::EnvelopeBlock::status() const noexcept {
    // 40dd6fc: never returns 0xff for DECAY_DONE; always
    // (mode<<6)|(level>>6).
    return static_cast<u8>((m_envelope_mode << 6) | (m_envelope_level >> 6));
}

bool Swp0277Core::EnvelopeBlock::active() const noexcept {
    // 40dd6fc: a voice is still active if its level has not fully
    // decayed, even when the envelope mode is DECAY_DONE.
    return m_envelope_mode != DECAY_DONE || m_envelope_level < 0x800;
}

Swp0277Core::u16 Swp0277Core::EnvelopeBlock::step(u32 sample_counter) noexcept {
    u16 result = static_cast<u16>(m_envelope_level);
    switch (m_envelope_mode) {
    case ATTACK: {
        if (m_attack_speed & 0x80) {
            // normal mode
            s32 level = m_envelope_level - static_cast<s32>(Swp0277Core::interpolation_step(
                (m_attack_speed & 0x7f) + ((m_envelope_level >> 7) << 2), sample_counter));
            if (level <= 0) {
                level = 0;
                m_envelope_mode = DECAY;
            }
            m_envelope_level = level;
        } else {
            s32 level = m_envelope_level + static_cast<s32>(Swp0277Core::interpolation_step(m_attack_speed, sample_counter));
            if (m_attack_level) {
                // decay-like mode
                s32 limit = m_attack_level << 4;
                if (level >= limit) {
                    m_envelope_mode = DECAY;
                    if (level >= 0xfff)
                        level = 0xfff;
                }
            } else {
                // timed mode
                if (level >= 0x800) {
                    level = 0;
                    m_envelope_mode = DECAY;
                }
                result = 0;
            }
            // 84fde62: the delay-attack path computes the level but
            // must also store it, or the attack ramp stalls.
            m_envelope_level = level;
        }
        break;
    }

    case DECAY: {
        u32 key = m_decay_speed >= 0xe0
            ? static_cast<u32>(m_decay_speed - 0x90 + ((m_envelope_level >> 7) << 2))
            : static_cast<u32>(m_decay_speed & 0x7f);
        s32 limit = m_decay_level << 4;
        s32 level = m_envelope_level;
        if (level < limit) {
            level += static_cast<s32>(Swp0277Core::interpolation_step(key, sample_counter));
            if (level > limit) {
                m_envelope_mode = DECAY_DONE;
                if (level > 0xfff)
                    level = 0xfff;
            }
        } else if (level > limit) {
            level -= static_cast<s32>(Swp0277Core::interpolation_step(key, sample_counter));
            if (level < limit) {
                m_envelope_mode = DECAY_DONE;
                if (level < 0)
                    level = 0;
            }
        } else {
            m_envelope_mode = DECAY_DONE;
        }
        m_envelope_level = level;
        break;
    }

    case DECAY_DONE:
        break;
    }

    return result;
}

// ---------------------------------------------------------------------------
// Filter block
// ---------------------------------------------------------------------------
// Chamberlin-configuration LPF. Encoded q is fp 2.3 (with a 4 offset),
// k is fp 4.8 but the top bit is an added 1 except when it is all zero.
// The chip interpolates between consecutive values of k; when updating
// the coefficient registers Q is updated immediately and K uses the
// interpolation block with the configured speed to reach the new value.
// When w=1 at keyon, the filter sweeps from 800 to fff at the configured
// speed then jumps to the programmed level.
void Swp0277Core::FilterBlock::clear() noexcept {
    m_info = 0x27ff; // Actual reset value unknown
    m_speed = 0x00;
    m_sweep = SWEEP_NONE;
    m_sweep_timer = 0;

    m_k_target = 0x7ff;
    m_k = 0xfff;
    m_q = 0x80;

    m_b = 0;
    m_l = 0;
}

void Swp0277Core::FilterBlock::keyon() noexcept {
    m_b = 0;
    m_l = 0;

    if (m_speed & 0x80) {
        if (m_filter_0277) {
            // 0.277 sweep (Route A hybrid): the cutoff goes straight
            // to its target and the sweep is only a hidden timer that
            // reports "done" when it expires. HEAD instead starts the
            // cutoff almost closed (m_k = 0x800, ~27 Hz) and sweeps it
            // up, which gives every such note a muffled onset when the
            // firmware uses bit 7 with slow speeds -- the "radio"
            // sound. The timer starts at 0x4000000 like 0.277 and
            // counts down in step(); m_k stays at target throughout.
            m_k = m_k_target;
            m_sweep = SWEEP_UP;
            m_sweep_timer = 0x4000000;
        } else {
            m_sweep = SWEEP_UP;
            m_k = 0x800;
        }
    } else {
        // Deviation from MAME HEAD (filter_block::keyon in
        // swp00.cpp): HEAD also leaves m_sweep untouched here, so
        // a previous sweep note's state leaks into the next
        // non-sweep note on the same voice -- a stale SWEEP_UP
        // keeps stepping m_k in step() (HEAD) or leaves the sweep
        // timer running and the status bit clear (0277),
        // corrupting the filter for the following notes. A key-on
        // starts a new note, so reset the sweep state: with
        // SWEEP_NONE and m_k == m_k_target, step() is a no-op and
        // status() still reports done (0xc0).
        m_k = m_k_target;
        m_sweep = SWEEP_NONE;
        m_sweep_timer = 0;
    }
}

Swp0277Core::s32 Swp0277Core::FilterBlock::step(s16 input, s32 lmod, u32 sample_counter) noexcept {
    s32 km = std::max(static_cast<s32>(m_k) - lmod, 0);
    s64 k = (0x101 + (km & 0xff)) << (km >> 8);
    s32 h = (input << 6) - m_l - static_cast<s32>((static_cast<s64>(m_q) * m_b) >> 7);
    m_b = m_b + static_cast<s32>((k * h) >> 24);
    m_l = m_l + static_cast<s32>((k * m_b) >> 24);

    switch (m_sweep) {
    case SWEEP_NONE:
        if (m_k < m_k_target) {
            m_k += Swp0277Core::interpolation_step(m_speed & 0x7f, sample_counter);
            if (m_k > m_k_target)
                m_k = m_k_target;
        } else if (m_k > m_k_target) {
            m_k -= Swp0277Core::interpolation_step(m_speed & 0x7f, sample_counter);
            if (m_k < m_k_target)
                m_k = m_k_target;
        }
        break;
    case SWEEP_UP:
        if (m_filter_0277) {
            // 0.277 sweep: the cutoff is already at target (set in
            // keyon); only the hidden timer counts down. The step is
            // the 0.277 global-step value for this speed, halved, one
            // decrement per sample. When it reaches zero the sweep is
            // done and status() reports 0xc0, exactly as 0.277's
            // istep(lpf_timer, 0, step_hi).
            const u32 step = filter_0277_global_step(m_speed & 0x7f) >> 1;
            if (step >= m_sweep_timer) {
                m_sweep_timer = 0;
                m_sweep = SWEEP_DONE;
            } else {
                m_sweep_timer -= step;
            }
            break;
        }
        m_k += Swp0277Core::interpolation_step(m_speed & 0x7f, sample_counter);
        if (m_k >= 0xfff) {
            m_k = m_k_target;
            m_sweep = SWEEP_DONE;
        }
        break;
    case SWEEP_DONE:
        break;
    }

    return m_l;
}

Swp0277Core::u8 Swp0277Core::FilterBlock::status() const noexcept {
    return static_cast<u8>((m_sweep == SWEEP_DONE || (m_sweep == SWEEP_NONE && m_k == m_k_target) ? 0xc0 : 0x00) |
                           ((m_k >> 6) ^ 0x3f));
}

void Swp0277Core::FilterBlock::info_w(u16 data) noexcept {
    // HEAD info_w<1> (high byte) recomputes Q; both byte writes
    // recompute k_target. The shell writes the full 16-bit value at
    // once, so both derivations run here.
    m_info = data;
    s32 q = (m_info >> 11) + 4;
    m_q = (0x10 - (q & 7)) << (4 - (q >> 3));
    u16 level = static_cast<u16>(m_info & 0x7ff);
    if (m_filter_0277) {
        // 0.277 cutoff cap (Route A hybrid): when the resonance bits
        // (m_info >> 11) are below 4, cap the cutoff level at 0x7c0.
        // HEAD has no cap. Ported from 0.277 set_lpf_info.
        if ((m_info >> 11) < 4 && level > 0x7c0)
            level = 0x7c0;
    }
    m_k_target = level;
    if (m_k_target)
        m_k_target |= 0x800;
}

// ---------------------------------------------------------------------------
// LFO block
// ---------------------------------------------------------------------------
// 22-bit counter, optionally triangle-shaped, multiplied by the
// different levels. The increment is encoded in a variant of fp 3.3:
// for e<7 the value is (8+m)<<e, for e=7 (8+2*m)<<e, giving a cycle
// time of 1489 to 523288 cycles (0.08 to 30 Hz).
void Swp0277Core::LfoBlock::clear() noexcept {
    m_counter = 0;
    m_lamod = 0;
    m_fmod = 0;
    m_speed = 0x80;
}

std::tuple<Swp0277Core::u32, Swp0277Core::s32, Swp0277Core::s32> Swp0277Core::LfoBlock::step() noexcept {
    u32 e = (m_speed >> 3) & 7;
    u32 step;
    if (e < 7)
        step = (8 | (m_speed & 7)) << e;
    else
        step = (8 + 2 * (m_speed & 7)) << 7;

    m_counter = (m_counter + step) & 0x3fffff;

    u32 amod = 0;
    s32 fmod = 0;
    s32 lmod = 0;

    if (!(m_speed & 0x80)) {
        s32 shaped;
        if (m_speed & 0x40)
            shaped = static_cast<s32>(((m_counter << 1) & 0x3fffff) ^ (m_counter & 0x200000 ? 0x3fffff : 0));
        else
            shaped = static_cast<s32>(m_counter);

        amod = (static_cast<u32>(shaped) * (m_lamod & 0x1f)) >> 16;
        fmod = ((shaped - 0x200000) * m_fmod) >> 21;
        lmod = (shaped * (m_lamod & 0xe0)) >> 20;
    }

    return std::make_tuple(amod, fmod, lmod);
}

// ---------------------------------------------------------------------------
// Mixer block
// ---------------------------------------------------------------------------
// An attenuation is a 12-bit floating-point 4.8 value which combines
// additively. The 8-bit levels are left-shifted by 4, the 4-bit (pan)
// by 6. A pan of 0xf has a special mapped value of 0xfff (e.g. off).
// Envelope and tremolo are given externally; this block manages all
// other volumes. The four levels dry/reverb/chorus/variation apply
// immediately when changed. Global and pan left/right interpolate
// by 1 per sample toward their targets.
void Swp0277Core::MixerBlock::clear() noexcept {
    m_cglo = m_cpanl = m_cpanr = 0xfff;
    m_tglo = m_tpanl = m_tpanr = 0xfff;
    m_glo = m_pan = m_dry = m_rev = m_cho = m_var = 0xff;
}

void Swp0277Core::MixerBlock::keyon() noexcept {
    m_cglo = m_tglo;
    m_cpanl = m_tpanl;
    m_cpanr = m_tpanr;
}

Swp0277Core::u8 Swp0277Core::MixerBlock::status_glo() const noexcept {
    return static_cast<u8>((m_cglo == m_tglo ? 0xc0 : 0x00) | (m_cglo >> 6));
}

Swp0277Core::u8 Swp0277Core::MixerBlock::status_panl() const noexcept {
    return static_cast<u8>((m_cpanl == m_tpanl ? 0xc0 : 0x00) | (m_cpanl >> 6));
}

Swp0277Core::u8 Swp0277Core::MixerBlock::status_panr() const noexcept {
    return static_cast<u8>((m_cpanr == m_tpanr ? 0xc0 : 0x00) | (m_cpanr >> 6));
}

void Swp0277Core::MixerBlock::step(s32 sample, u16 envelope, u16 tremolo,
                                    s32& dry_l, s32& dry_r, s32& rev,
                                    s32& cho_l, s32& cho_r, s32& var_l, s32& var_r) noexcept {
    u16 base = static_cast<u16>(envelope + tremolo + m_cglo);
    dry_l += volume_apply(base + (m_dry << 4) + m_cpanl, sample);
    dry_r += volume_apply(base + (m_dry << 4) + m_cpanr, sample);
    rev   += volume_apply(base + (m_rev << 4), sample);
    cho_l += volume_apply(base + (m_cho << 4) + m_cpanl, sample);
    cho_r += volume_apply(base + (m_cho << 4) + m_cpanr, sample);
    var_l += volume_apply(base + (m_var << 4) + m_cpanl, sample);
    var_r += volume_apply(base + (m_var << 4) + m_cpanr, sample);

    if (m_cglo < m_tglo)
        m_cglo++;
    else if (m_cglo > m_tglo)
        m_cglo--;
    if (m_cpanl < m_tpanl)
        m_cpanl++;
    else if (m_cpanl > m_tpanl)
        m_cpanl--;
    if (m_cpanr < m_tpanr)
        m_cpanr++;
    else if (m_cpanr > m_tpanr)
        m_cpanr--;
}

Swp0277Core::s32 Swp0277Core::MixerBlock::volume_apply(s32 level, s32 sample) noexcept {
    // Level is 4.8 floating point positive, and represents an attenuation.
    // Sample is 16.6 signed and the result is in the same format.
    // Passed-in value may have overflowed.
    if (level >= 0xfff)
        return 0;

    s32 e = level >> 8;
    s32 m = level & 0xff;
    s64 mul = (0x1000000 - (m << 15)) >> e;
    return static_cast<s32>((sample * mul) >> 24);
}

void Swp0277Core::MixerBlock::pan_w(u8 data) noexcept {
    m_pan = data;
    m_tpanl = static_cast<u16>((data << 2) & 0x3c0);
    m_tpanr = static_cast<u16>((data << 6) & 0x3c0);
    if (m_tpanl == 0x3c0)
        m_tpanl = 0xfff;
    if (m_tpanr == 0x3c0)
        m_tpanr = 0xfff;
}

// ---------------------------------------------------------------------------
// Core: reset / register writes / keyon
// ---------------------------------------------------------------------------
void Swp0277Core::reset() noexcept {
    for (auto& s : streaming_) s.clear();
    for (auto& e : envelope_) e.clear();
    for (auto& f : filter_) f.clear();
    for (auto& l : lfo_) l.clear();
    for (auto& m : mixer_) m.clear();
    sample_counter_ = 0;
}

void Swp0277Core::set_phase(int chan, u16 v) noexcept {
    streaming_[chan].m_phase = v;
}

void Swp0277Core::set_start(int chan, u16 v) noexcept {
    streaming_[chan].m_start = v;
}

void Swp0277Core::set_loop(int chan, u16 v) noexcept {
    streaming_[chan].m_loop = v;
}

void Swp0277Core::set_address(int chan, u32 v) noexcept {
    streaming_[chan].m_address = v & 0xffffff;
}

void Swp0277Core::set_pitch(int chan, u16 v) noexcept {
    streaming_[chan].m_pitch = v;
}

void Swp0277Core::set_format(int chan, u8 v) noexcept {
    streaming_[chan].m_format = v;
}

void Swp0277Core::set_lpf_info(int chan, u16 v) noexcept {
    filter_[chan].info_w(v);
}

void Swp0277Core::keyon(int chan) noexcept {
    streaming_[chan].keyon();
    envelope_[chan].keyon();
    filter_[chan].keyon();
    lfo_[chan].keyon();
    mixer_[chan].keyon();
}

// ---------------------------------------------------------------------------
// Core: one sample (HEAD sound_stream_update voice section)
// ---------------------------------------------------------------------------
VoiceMix Swp0277Core::render_one(const u8* rom, std::size_t rom_size) noexcept {
    s32 dry_l = 0, dry_r = 0;
    s32 rev = 0;
    s32 cho_l = 0, cho_r = 0;
    s32 var_l = 0, var_r = 0;

    for (int chan = 0; chan != 32; chan++) {
        // The LFO advances for every channel on every sample, even
        // inactive ones (HEAD steps it before the active() check).
        auto [amod, fmod, lmod] = lfo_[chan].step();

        if (!envelope_[chan].active())
            continue;

        auto [sample1, trigger_release] = streaming_[chan].step(rom, rom_size, fmod);
        if (trigger_release)
            envelope_[chan].trigger_release();

        s32 sample2 = filter_[chan].step(sample1, lmod, sample_counter_);
        u16 envelope_level = envelope_[chan].step(sample_counter_);
        mixer_[chan].step(sample2, envelope_level, static_cast<u16>(amod),
                          dry_l, dry_r, rev, cho_l, cho_r, var_l, var_r);
    }

    sample_counter_++;

    VoiceMix mix;
    // HEAD scale (unshifted): the MEG runs on the full 16.6 buses,
    // exactly as in MAME HEAD. Shifting >>6 here (as the 0.277 port
    // did) would run the MEG 64x cold and disable the distortion
    // stage's saturate() in variation variant 0x80. synth.cpp applies
    // HEAD's >>6 (put_int_clamp) to the MEG dry output instead.
    mix.dry_l = dry_l;
    mix.dry_r = dry_r;
    mix.reverb = rev;
    mix.chorus_l = cho_l;
    mix.chorus_r = cho_r;
    mix.variation_l = var_l;
    mix.variation_r = var_r;
    return mix;
}

} // namespace libmu50::detail
