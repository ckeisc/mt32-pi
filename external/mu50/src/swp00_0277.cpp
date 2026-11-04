// license:BSD-3-Clause
// Ported from MAME 0.277 src/devices/sound/swp00.cpp (Olivier Galibert).
#include "swp00_0277.hpp"

namespace libmu50::detail {

namespace {
constexpr std::array<std::uint32_t, 4> kLfoCenteredSaw = {0x00000000, 0x00000000, 0xfff00000, 0xfff00000};
constexpr std::array<std::uint32_t, 4> kLfoCenteredTri = {0x00000000, 0x0007ffff, 0xfff7ffff, 0xfff00000};
constexpr std::array<std::uint32_t, 4> kLfoOffsetSaw = {0x00000000, 0x00000000, 0x00000000, 0x00000000};
constexpr std::array<std::uint32_t, 4> kLfoOffsetTri = {0x00000000, 0x00000000, 0x000fffff, 0x000fffff};
constexpr std::array<std::int32_t, 16> kPanmap = {
    0x000, 0x040, 0x080, 0x0c0, 0x100, 0x140, 0x180, 0x1c0,
    0x200, 0x240, 0x280, 0x2c0, 0x300, 0x340, 0x380, 0xfff};
constexpr std::array<std::uint8_t, 4> kDpcmOffset = {7, 6, 4, 0};
} // namespace

bool Swp0277Core::istep(std::int32_t& value, std::int32_t limit, std::int32_t step) noexcept {
    if (value < limit) {
        value += step;
        if (value >= limit) {
            value = limit;
            return true;
        }
        return false;
    }
    if (value > limit) {
        value -= step;
        if (value <= limit) {
            value = limit;
            return true;
        }
        return false;
    }
    return true;
}

std::int32_t Swp0277Core::fpadd(std::int32_t value, std::int32_t step) noexcept {
    s32 e = value >> 24;
    s32 m = value & 0xffffff;
    m += step << e;
    if (m & 0xfe000000)
        return 0xfffffff;
    while (m & 0x01000000) {
        m <<= 1;
        e++;
    }
    if (e >= 16)
        return 0xfffffff;
    return (e << 24) | (m & 0xffffff);
}

std::int32_t Swp0277Core::fpsub(std::int32_t value, std::int32_t step) noexcept {
    s32 e = value >> 24;
    s32 m = (value & 0xffffff) | 0xfe000000;
    m = e < 0xc ? m - (step << e) : (m >> (e - 0xb)) - (step << 0xb);
    if (m >= 0)
        return 0;
    if (e >= 0xc)
        e = 0xb;
    // The casts below only silence -Wsign-compare: m < 0 is established
    // above, and in the negative range the signed comparison against
    // these constants gives the same result as MAME's unsigned literals.
    while (m < static_cast<s32>(0xfe000000)) {
        if (!e)
            return 0;
        e--;
        m >>= 1;
    }
    while (e != 0xf && (m >= static_cast<s32>(0xff000000))) {
        e++;
        m <<= 1;
    }
    return (e << 24) | (m & 0xffffff);
}

bool Swp0277Core::fpstep(std::int32_t& value, std::int32_t limit, std::int32_t step) noexcept {
    // value, limit and step are 4.24 but step has its exponent and
    // top four bits zero
    if (value == limit)
        return true;
    if (value < limit) {
        value = fpadd(value, step);
        if (value >= limit) {
            value = limit;
            return true;
        }
        return false;
    }
    value = fpsub(value, step);
    if (value <= limit) {
        value = limit;
        return true;
    }
    return false;
}

// sample is signed 24.8
std::int32_t Swp0277Core::fpapply(std::int32_t value, std::int32_t sample) noexcept {
    if (value >= 0x10000000)
        return 0;
    return (s64(sample) - ((s64(sample) * ((value >> 9) & 0x7fff)) >> 16)) >> (value >> 24);
}

// sample is signed 24.8
// NOTE: ported verbatim from MAME, including the shift count
// (31 - (value >> 22)), which goes negative for large cutoff values.
// The same compiler compiles the same expression the same way on both
// sides; do NOT "fix" it, or the port diverges from MAME.
std::int32_t Swp0277Core::lpffpapply(std::int32_t value, std::int32_t sample) noexcept {
    return ((((value >> 7) & 0x7fff) | 0x8000) * s64(sample)) >> (31 - (value >> 22));
}

const std::array<Swp0277Core::u32, 128>& Swp0277Core::global_step() noexcept {
    static const std::array<u32, 128> table = [] {
        std::array<u32, 128> t{};
        for (int i = 0; i != 128; i++) {
            u32 v = 0;
            switch (i >> 3) {
            default: v = static_cast<u32>(((i & 7) + 8) << (1 + (i >> 3))); break;
            case 0xb: v = static_cast<u32>(((i & 7) + 4) << 13); break;
            case 0xc: v = static_cast<u32>(((i & 6) + 6) << 14); break;
            case 0xd: v = static_cast<u32>(((i & 4) + 7) << 15); break;
            case 0xe: v = 15 << 15; break;
            case 0xf: v = 31 << 15; break;
            }
            t[static_cast<std::size_t>(i)] = v;
        }
        return t;
    }();
    return table;
}

const std::array<Swp0277Core::s16, 256>& Swp0277Core::dpcm_table() noexcept {
    static const std::array<s16, 256> table = [] {
        std::array<s16, 256> t{};
        for (int i = 0; i < 128; i++) {
            s16 base = static_cast<s16>(((i & 0x1f) << (3 + (i >> 5))) + (((1 << (i >> 5)) - 1) << 8));
            t[static_cast<std::size_t>(i | 0x80)] = static_cast<s16>(-base);
            t[static_cast<std::size_t>(i)] = base;
        }
        return t;
    }();
    return table;
}

const std::array<Swp0277Core::s32, 0x80>& Swp0277Core::attack_linear_step() noexcept {
    static const std::array<s32, 0x80> table = {
        0x00027, 0x0002b, 0x0002f, 0x00033, 0x00037, 0x0003d, 0x00042, 0x00048,
        0x0004d, 0x00056, 0x0005e, 0x00066, 0x0006f, 0x0007a, 0x00085, 0x00090,
        0x0009b, 0x000ac, 0x000bd, 0x000cc, 0x000de, 0x000f4, 0x00109, 0x00120,
        0x00135, 0x00158, 0x00179, 0x00199, 0x001bc, 0x001e7, 0x00214, 0x00240,
        0x0026b, 0x002af, 0x002f2, 0x00332, 0x00377, 0x003d0, 0x0042c, 0x00480,
        0x004dc, 0x0055e, 0x005e9, 0x0066e, 0x006f4, 0x007a4, 0x00857, 0x0090b,
        0x009c3, 0x00acb, 0x00bd6, 0x00ce6, 0x00e00, 0x00f5e, 0x010d2, 0x01234,
        0x0139e, 0x015d0, 0x017f3, 0x01a20, 0x01c4a, 0x01f52, 0x02232, 0x0250f,
        0x027ff, 0x02c72, 0x03109, 0x0338b, 0x039c4, 0x04038, 0x04648, 0x04c84,
        0x05262, 0x05c1c, 0x065af, 0x06f5c, 0x07895, 0x0866f, 0x09470, 0x0a19e,
        0x0ae4c, 0x0c566, 0x0db8d, 0x0f00f, 0x10625, 0x12937, 0x14954, 0x16c17,
        0x1886e, 0x1c71c, 0x20000, 0x239e1, 0x2647c, 0x2aaab, 0x2ecfc, 0x3241f,
        0x35e51, 0x3a83b, 0x40000, 0x4325c, 0x47dc1, 0x4c8f9, 0x50505, 0x55555,
        0x58160, 0x5d174, 0x60606, 0x62b2e, 0x67b24, 0x6a63c, 0x6d3a0, 0x6eb3e,
        0x71c72, 0x73616, 0x75075, 0x76b98, 0x78788, 0x78788, 0x7a44c, 0x7a44c,
        0x7a44c, 0x7a44c, 0x7a44c, 0x7a44c, 0x7a44c, 0x7a44c, 0x7a44c, 0x7a44c};
    return table;
}

const std::array<Swp0277Core::s32, 0x20>& Swp0277Core::decay_linear_step() noexcept {
    static const std::array<s32, 0x20> table = {
        0x15083, 0x17ad2, 0x1a41a, 0x1cbe7, 0x1f16d, 0x22ef1, 0x26a44, 0x2a1e4,
        0x2da35, 0x34034, 0x3a197, 0x40000, 0x45b82, 0x4b809, 0x51833, 0x57262,
        0x5d9f7, 0x6483f, 0x6b15c, 0x71c72, 0x77976, 0x7d119, 0x83127, 0x88889,
        0x8d3dd, 0x939a8, 0x991f2, 0x9d89e, 0xa0a0a, 0xa57eb, 0xa72f0, 0xac769};
    return table;
}

void Swp0277Core::reset() noexcept {
    // Mirrors device_reset: all registers and state zeroed. (MAME's
    // device_reset also zeroes the MEG state; the Synth shell resets
    // MegState separately.)
    lpf_info_.fill(0);
    lpf_speed_.fill(0);
    lfo_famod_depth_.fill(0);
    rev_level_.fill(0);
    dry_level_.fill(0);
    cho_level_.fill(0);
    var_level_.fill(0);
    glo_level_.fill(0);
    panning_.fill(0);
    attack_speed_.fill(0);
    attack_level_.fill(0);
    decay_speed_.fill(0);
    decay_level_.fill(0);
    pitch_.fill(0);
    start_.fill(0);
    end_.fill(0);
    format_.fill(0);
    address_.fill(0);
    lfo_step_.fill(0);
    lfo_pmod_depth_.fill(0);
    lfo_phase_.fill(0);
    sample_pos_.fill(0);
    envelope_level_.fill(0);
    glo_level_cur_.fill(0);
    pan_l_.fill(0);
    pan_r_.fill(0);
    lpf_feedback_.fill(0);
    lpf_target_value_.fill(0);
    lpf_value_.fill(0);
    lpf_timer_.fill(0);
    lpf_ha_.fill(0);
    lpf_hb_.fill(0);
    active_.fill(false);
    decay_.fill(false);
    decay_done_.fill(false);
    lpf_done_.fill(false);
    dpcm_current_.fill(0);
    dpcm_next_.fill(0);
    dpcm_address_.fill(0);
    dpcm_sum_.fill(0);
    // Derived constants are recomputed by precompute_voices() at the
    // start of every render call; clearing here only avoids stale
    // values surviving a reset.
    precomp_.fill(VoicePrecomp{});
}

void Swp0277Core::set_lpf_info(int chan, u16 v) noexcept {
    if (lpf_info_[chan] == v)
        return;
    lpf_info_[chan] = v;
    // lpf_info_w recompute (MAME 0.277).
    s32 fb = v >> 11;
    s32 level = v & 0x7ff;
    if (fb < 4 && level > 0x7c0)
        level = 0x7c0;
    if (level)
        level |= 0x800;
    lpf_feedback_[chan] = (fb + 4) << 21;
    lpf_target_value_[chan] = level << 14;
}

void Swp0277Core::keyon(int chan) noexcept {
    // Mirrors swp00_device::keyon (MAME 0.277) exactly.
    sample_pos_[chan] = -static_cast<s32>(start_[chan]) << 15;
    dpcm_address_[chan] = address_[chan] - start_[chan];
    dpcm_sum_[chan] = 0;
    dpcm_current_[chan] = 0;
    dpcm_next_[chan] = 0;
    lpf_value_[chan] = lpf_target_value_[chan];
    lpf_timer_[chan] = 0x4000000;
    lpf_ha_[chan] = 0;
    lpf_hb_[chan] = 0;
    glo_level_cur_[chan] = static_cast<s32>(glo_level_[chan]) << 4;
    pan_l_[chan] = kPanmap[(panning_[chan] >> 4) & 0xf];
    pan_r_[chan] = kPanmap[panning_[chan] & 0xf];
    active_[chan] = true;
    decay_[chan] = false;
    decay_done_[chan] = false;
    lfo_phase_[chan] = 0;
    if (decay_speed_[chan] & 0x80) {
        envelope_level_[chan] = 0;
        decay_[chan] = true;
    } else if ((attack_speed_[chan] & 0x80) || attack_level_[chan]) {
        envelope_level_[chan] = static_cast<s32>(attack_level_[chan]) << 20;
    } else {
        envelope_level_[chan] = 0x8000000;
    }
}

Swp0277Core::u8 Swp0277Core::read_byte(u32 address, const u8* rom, std::size_t rom_size) const noexcept {
    // MAME device_rom_interface: the region mirrors over the 24-bit bus.
    if (rom_size == 0)
        return 0;
    return rom[address & static_cast<u32>(rom_size - 1)];
}

Swp0277Core::u16 Swp0277Core::read_word(u32 address, const u8* rom, std::size_t rom_size) const noexcept {
    // ENDIANNESS_LITTLE: low byte first.
    return static_cast<u16>(read_byte(address, rom, rom_size) |
                            (static_cast<u16>(read_byte(address + 1, rom, rom_size)) << 8));
}

void Swp0277Core::precompute_voices() noexcept {
    const auto& gstep = global_step();
    const auto& astep = attack_linear_step();
    const auto& dstep = decay_linear_step();
    for (int chan = 0; chan != 32; chan++) {
        VoicePrecomp& p = precomp_[chan];
        p.format_case = static_cast<u8>(format_[chan] >> 6);
        p.dpcm_offset = kDpcmOffset[format_[chan] & 3];
        p.dpcm_scale = static_cast<u8>((format_[chan] >> 2) & 7);
        const bool tri = (lfo_step_[chan] & 0x40) != 0;
        p.lfo_centered = tri ? kLfoCenteredTri.data() : kLfoCenteredSaw.data();
        p.lfo_offset = tri ? kLfoOffsetTri.data() : kLfoOffsetSaw.data();
        p.lfo_fa_shift = tri ? 2 : 1;
        p.tremolo_shift = tri ? 3 : 2;
        p.pmod_shift = tri ? 18 : 19;
        p.lfo_step_value = gstep[0x20 + (lfo_step_[chan] & 0x3f)];
        p.sample_increment = ((pitch_[chan] & 0xfff) << (8 + (static_cast<s16>(pitch_[chan]) >> 12))) >> 4;
        p.dry_const = static_cast<s32>(dry_level_[chan]) << 20;
        p.rev_const = static_cast<s32>(rev_level_[chan]) << 20;
        p.cho_const = static_cast<s32>(cho_level_[chan]) << 20;
        p.var_const = static_cast<s32>(var_level_[chan]) << 20;
        p.famod_hi = lfo_famod_depth_[chan] >> 5;
        p.famod_lo = lfo_famod_depth_[chan] & 0x1f;
        p.lfo_pmod_depth = lfo_pmod_depth_[chan];
        p.lpf_step_hi = static_cast<s32>(gstep[lpf_speed_[chan] & 0x7f] >> 1);
        // Masked, not raw: the _lo values are consumed ONLY in the
        // bit-7-clear branch, where masked == raw, so the mask is
        // exact for every use and avoids an OOB read when bit 7 set.
        p.lpf_step_lo = static_cast<s32>(gstep[lpf_speed_[chan] & 0x7f] >> 1);
        p.lpf_hi = (lpf_speed_[chan] & 0x80) != 0;
        p.lpf_target = lpf_target_value_[chan];
        p.lpf_feedback = lpf_feedback_[chan];
        p.decay_exp = (decay_speed_[chan] & 0x60) == 0x60;
        p.decay_step_exp = dstep[decay_speed_[chan] & 0x1f];
        p.decay_step_lin = static_cast<s32>(gstep[decay_speed_[chan] & 0x7f] << 1);
        p.decay_target = static_cast<s32>(decay_level_[chan]) << 20;
        p.attack_step = astep[attack_speed_[chan] & 0x7f];
        p.attack_step_lin = static_cast<s32>(gstep[attack_speed_[chan] & 0x7f] << 1);
        p.attack_hi = (attack_speed_[chan] & 0x80) != 0;
        p.glo_target = static_cast<s32>(glo_level_[chan]) << 4;
        p.pan_l_target = kPanmap[(panning_[chan] >> 4) & 0xf];
        p.pan_r_target = kPanmap[panning_[chan] & 0xf];
        p.end = end_[chan];
        p.address = address_[chan];
    }
}

VoiceMix Swp0277Core::render_one(const u8* rom, std::size_t rom_size) noexcept {
    // Mirrors the voice section of sound_stream_update (MAME 0.277),
    // up to (but not including) the Variation block. Buses are >>8
    // here, exactly as MAME feeds them to the MEG tail.
    // Per-sample derived constants come from precomp_ (recomputed
    // once per Synth::render call by precompute_voices); only
    // stateful values are read from the arrays here.
    const auto& dtab = dpcm_table();
    s32 dry_l = 0, dry_r = 0;
    s32 rev = 0;
    s32 cho_l = 0, cho_r = 0;
    s32 var_l = 0, var_r = 0;

    for (int chan = 0; chan != 32; chan++) {
        if (!active_[chan])
            continue;
        const VoicePrecomp& pre = precomp_[chan];

        u32 lfo_phase = lfo_phase_[chan] >> 7;
        s32 lfo_p_phase = static_cast<s32>(lfo_phase ^
            pre.lfo_centered[lfo_phase >> 18]);
        s32 lfo_fa_phase = static_cast<s32>(lfo_phase ^
            pre.lfo_offset[lfo_phase >> 18]);

        s16 val0 = 0, val1 = 0;
        u32 base_address = pre.address;
        s32 spos = sample_pos_[chan] >> 15;
        switch (pre.format_case) {
        case 0: { // 16-bits linear
            u32 adr = base_address + (static_cast<u32>(spos) << 1);
            val0 = static_cast<s16>(read_word(adr, rom, rom_size));
            val1 = static_cast<s16>(read_word(adr + 2, rom, rom_size));
            break;
        }
        case 1: { // 12-bits linear
            u32 adr = base_address + (static_cast<u32>(spos >> 2) * 6);
            switch (spos & 3) {
            case 0: { // Cabc ..AB .... ....
                u16 w0 = read_word(adr, rom, rom_size);
                u16 w1 = read_word(adr + 2, rom, rom_size);
                val0 = static_cast<s16>((w0 & 0x0fff) << 4);
                val1 = static_cast<s16>(((w0 & 0xf000) >> 8) | ((w1 & 0x00ff) << 8));
                break;
            }
            case 1: { // c... BCab ...A ....
                u16 w0 = read_word(adr, rom, rom_size);
                u16 w1 = read_word(adr + 2, rom, rom_size);
                u16 w2 = read_word(adr + 4, rom, rom_size);
                val0 = static_cast<s16>(((w0 & 0xf000) >> 8) | ((w1 & 0x00ff) << 8));
                val1 = static_cast<s16>(((w1 & 0xff00) >> 4) | ((w2 & 0x000f) << 12));
                break;
            }
            case 2: { // .... bc.. ABCa ....
                u16 w1 = read_word(adr + 2, rom, rom_size);
                u16 w2 = read_word(adr + 4, rom, rom_size);
                val0 = static_cast<s16>(((w1 & 0xff00) >> 4) | ((w2 & 0x000f) << 12));
                val1 = static_cast<s16>(w2 & 0xfff0);
                break;
            }
            case 3: { // .... .... abc. .ABC
                u16 w2 = read_word(adr + 4, rom, rom_size);
                u16 w3 = read_word(adr + 6, rom, rom_size);
                val0 = static_cast<s16>(w2 & 0xfff0);
                val1 = static_cast<s16>((w3 & 0x0fff) << 4);
                break;
            }
            }
            break;
        }
        case 2: // 8-bits linear
            val0 = static_cast<s16>(read_byte(base_address + static_cast<u32>(spos), rom, rom_size) << 8);
            val1 = static_cast<s16>(read_byte(base_address + static_cast<u32>(spos) + 1, rom, rom_size) << 8);
            break;
        case 3: { // 8-bits delta-pcm: sequential by address, every byte
            // decoded exactly once per pass; the loop sum resets on wrap.
            u8 offset = pre.dpcm_offset;
            u8 scale = pre.dpcm_scale;
            u32 target_address = base_address + static_cast<u32>(spos) + 1;
            while (dpcm_address_[chan] <= target_address) {
                dpcm_current_[chan] = dpcm_next_[chan];
                dpcm_sum_[chan] += dtab[read_byte(dpcm_address_[chan], rom, rom_size)] - offset;
                s32 sample = (dpcm_sum_[chan] << scale) >> 3;
                dpcm_address_[chan]++;
                if (sample < -0x8000)
                    sample = -0x8000;
                else if (sample > 0x7fff)
                    sample = 0x7fff;
                dpcm_next_[chan] = static_cast<s16>(sample);
            }
            val0 = dpcm_current_[chan];
            val1 = dpcm_next_[chan];
            break;
        }
        }

        s32 mul = sample_pos_[chan] & 0x7fff;
        s32 sample = (val1 * mul + val0 * (0x8000 - mul)) >> 7;

        s32 lpf_value = lpf_value_[chan] +
            ((lfo_fa_phase * pre.famod_hi) << pre.lfo_fa_shift);

        lpf_ha_[chan] += lpffpapply(lpf_value, sample - 2 * fpapply(pre.lpf_feedback, lpf_ha_[chan]) - lpf_hb_[chan]);
        lpf_hb_[chan] += lpffpapply(lpf_value, lpf_ha_[chan]);

        sample = lpf_hb_[chan];

        s32 envelope_level;
        if (decay_[chan] || attack_level_[chan] || (attack_speed_[chan] & 0x80))
            envelope_level = envelope_level_[chan];
        else
            envelope_level = 0;

        s32 tremolo_level = (lfo_fa_phase * pre.famod_lo) << pre.tremolo_shift;

        // Common bus prefix, computed once (identical add order to
        // the original seven lines: env + glo + trem, then the
        // per-bus constant, then pan where present).
        s32 base = envelope_level + (glo_level_cur_[chan] << 16) + tremolo_level;
        dry_l += fpapply(base + pre.dry_const + (pan_l_[chan] << 16), sample);
        dry_r += fpapply(base + pre.dry_const + (pan_r_[chan] << 16), sample);
        rev += fpapply(base + pre.rev_const, sample);
        cho_l += fpapply(base + pre.cho_const + (pan_l_[chan] << 16), sample);
        cho_r += fpapply(base + pre.cho_const + (pan_r_[chan] << 16), sample);
        var_l += fpapply(base + pre.var_const + (pan_l_[chan] << 16), sample);
        var_r += fpapply(base + pre.var_const + (pan_r_[chan] << 16), sample);

        lfo_phase_[chan] = (lfo_phase_[chan] + pre.lfo_step_value) & 0x7ffffff;

        sample_pos_[chan] += (pre.sample_increment * (0x800 + ((lfo_p_phase * pre.lfo_pmod_depth) >> pre.pmod_shift))) >> 11;

        if ((sample_pos_[chan] >> 15) >= pre.end) {
            if (!pre.end)
                active_[chan] = false;
            else {
                s32 prev = sample_pos_[chan];
                do
                    sample_pos_[chan] -= pre.end << 15;
                while ((sample_pos_[chan] >> 15) >= pre.end);
                dpcm_address_[chan] += static_cast<u32>((sample_pos_[chan] >> 15) - (prev >> 15));
                dpcm_sum_[chan] = 0;
            }
        }

        if (pre.lpf_hi)
            lpf_done_[chan] = istep(lpf_timer_[chan], 0, pre.lpf_step_hi);
        else
            lpf_done_[chan] = istep(lpf_value_[chan], pre.lpf_target, pre.lpf_step_lo);

        istep(glo_level_cur_[chan], pre.glo_target, 1);
        istep(pan_l_[chan], pre.pan_l_target, 1);
        istep(pan_r_[chan], pre.pan_r_target, 1);

        if (decay_[chan]) {
            if (pre.decay_exp)
                decay_done_[chan] = fpstep(envelope_level_[chan], pre.decay_target, pre.decay_step_exp);
            else
                decay_done_[chan] = istep(envelope_level_[chan], pre.decay_target, pre.decay_step_lin);
            if (envelope_level_[chan] & 0x8000000)
                active_[chan] = false;
        } else if (pre.attack_hi)
            decay_[chan] = fpstep(envelope_level_[chan], 0, pre.attack_step);
        else
            decay_[chan] = istep(envelope_level_[chan], 0, pre.attack_step_lin);
    }

    VoiceMix mix;
    mix.dry_l = dry_l >> 8;
    mix.dry_r = dry_r >> 8;
    mix.reverb = rev >> 8;
    mix.chorus_l = cho_l >> 8;
    mix.chorus_r = cho_r >> 8;
    mix.variation_l = var_l >> 8;
    mix.variation_r = var_r >> 8;
    return mix;
}

} // namespace libmu50::detail
