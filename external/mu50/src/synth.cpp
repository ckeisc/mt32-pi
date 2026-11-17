// license:BSD-3-Clause
// SWP00 register shell for the MAME HEAD-based voice core (synthesis
// overhaul b75872c plus the 84fde62 and 40dd6fc corrections). Address
// decoding mirrors MAME HEAD's rchan/rctrl maps; the voice semantics
// live in Swp0277Core and the MEG tail in MegState.
#include "libmu50/synth.hpp"
#include "libmu50/keyon_status.hpp"
#include "swp00_0277.hpp"
#include "swp00_meg.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <utility>

namespace libmu50 {

// Post-key-on SWP00 voice status (see keyon_status.hpp): runs the
// core's own keyon()/status() on a scratch Swp0277Core seeded with the
// audio-side register shadow, so the overlay can never drift from the
// core. One ~5 KB scratch core per call, on the key-on write path (not
// the per-sample render path); key-ons arrive at MIDI rates.
void KeyOnStatus(const KeyOnRegs& r, std::uint8_t out[5]) noexcept {
    detail::Swp0277Core core;
    core.set_filter_0277(r.filter_0277);
    core.set_attack_speed(0, r.attack_speed);
    core.set_attack_level(0, r.attack_level);
    core.set_decay_speed(0, r.decay_speed);
    core.set_lpf_info(0, r.lpf_info);
    core.set_lpf_speed(0, r.lpf_speed);
    core.set_glo_level(0, r.glo);
    core.set_pan(0, r.pan);
    core.keyon(0);
    out[0] = core.filter_status(0);
    out[1] = core.envelope_status(0);
    out[2] = core.mixer_status_glo(0);
    out[3] = core.mixer_status_panl(0);
    out[4] = core.mixer_status_panr(0);
}

namespace {
constexpr std::size_t kWaveRomSize = 0x400000;
constexpr std::uint32_t kNativeRate = 44100;

std::int16_t clamp_s16(std::int64_t value) noexcept {
    return static_cast<std::int16_t>(std::clamp<std::int64_t>(value, -32768, 32767));
}
}

struct Synth::Impl {
    explicit Impl(Config cfg) : config(cfg) {
        reset();
        meg.set_ablation(cfg.no_reverb, cfg.no_chorus, cfg.no_variation);
    }

    Config config;
    std::vector<std::uint8_t> wave_rom;
    detail::Swp0277Core core;
    std::array<std::array<std::uint8_t, 0x40>, 32> channel_regs{};
    detail::MegState meg;
    std::array<std::uint8_t, 0x10> control_regs{};

    void reset() {
        core.reset();
        for (auto& r : channel_regs) r.fill(0);
        // Seed the register pairs whose core reset values are non-zero
        // (Copilot review on PR #12, comment 2). The core resets the
        // filter info to 0x27ff and the phase to 0x8000, but the shadow
        // above resets to 0. If the firmware then writes only one byte
        // of a pair, write_channel recomputes the 16-bit value from
        // the shadow and the core gets the wrong other half (0 instead
        // of the reset value). Seed them so a single-byte write uses
        // the correct reset value for the other half. All other pairs
        // (start, loop, address, pitch) reset to 0, matching the fill.
        // LPF info 0x27ff: pair(0x21, 0x20), so [0x20]=0x27 (hi),
        // [0x21]=0xff (lo). Phase 0x8000: pair(0x09, 0x08), so
        // [0x08]=0x80 (hi), [0x09]=0x00 (lo, already 0).
        for (auto& r : channel_regs)
        {
            r[0x20] = 0x27;
            r[0x21] = 0xff;
            r[0x08] = 0x80;
        }
        meg.reset();
        control_regs.fill(0);
    }

    // HEAD swp00_device::state_r (register 0x01 readback): returns the
    // computed voice state selected by the state address in
    // control_regs[1] (written by state_adr_w). chan = adr & 0x1f.
    // Cases 6/7 return 0 (HEAD logs "state %d unsupported").
    std::uint8_t state_r() const noexcept {
        const std::uint8_t adr = control_regs[1];
        const int chan = adr & 0x1f;
        switch (adr >> 5) {
        case 0: return core.filter_status(chan);
        case 1: return static_cast<std::uint8_t>(0xc0 | (core.sample_counter() >> 13));
        case 2: return core.envelope_status(chan);
        case 3: return core.mixer_status_glo(chan);
        case 4: return core.mixer_status_panl(chan);
        case 5: return core.mixer_status_panr(chan);
        default: return 0;
        }
    }

    void write_channel(std::uint8_t reg, std::uint8_t channel, std::uint8_t value) {
        auto& shadow = channel_regs[channel][reg];
        shadow = value;
        auto& c = core;
        auto pair = [&](std::uint8_t lo_reg, std::uint8_t hi_reg) {
            return std::uint16_t(channel_regs[channel][lo_reg]) |
                   (std::uint16_t(channel_regs[channel][hi_reg]) << 8);
        };
        switch (reg) {
        // 0x08/0x09 are the HEAD phase registers (high/low byte).
        case 0x08: case 0x09: c.set_phase(channel, pair(0x09, 0x08)); break;
        case 0x0a: case 0x0b: c.set_start(channel, pair(0x0b, 0x0a)); break;
        case 0x20: case 0x21: c.set_lpf_info(channel, pair(0x21, 0x20)); break;
        case 0x22: c.set_lpf_speed(channel, value); break;
        case 0x23: c.set_lfo_lamod(channel, value); break;
        case 0x24: c.set_lfo_speed(channel, value); break;
        case 0x25: c.set_lfo_fmod(channel, value); break;
        case 0x26: c.set_attack_speed(channel, value); break;
        case 0x27: c.set_attack_level(channel, value); break;
        case 0x28: c.set_decay_speed(channel, value); break;
        case 0x29: c.set_decay_level(channel, value); break;
        case 0x2a: c.set_reverb_level(channel, value); break;
        case 0x2b: c.set_dry_level(channel, value); break;
        case 0x2c: c.set_chorus_level(channel, value); break;
        case 0x2d: c.set_variation_level(channel, value); break;
        case 0x2e: c.set_glo_level(channel, value); break;
        case 0x2f: c.set_pan(channel, value); break;
        case 0x30: c.set_format(channel, value); break;
        case 0x31: case 0x32: case 0x33:
            c.set_address(channel, std::uint32_t(channel_regs[channel][0x33]) |
                          (std::uint32_t(channel_regs[channel][0x32]) << 8) |
                          (std::uint32_t(channel_regs[channel][0x31]) << 16));
            break;
        case 0x34: case 0x35: c.set_pitch(channel, pair(0x35, 0x34)); break;
        case 0x36: case 0x37: c.set_loop(channel, pair(0x37, 0x36)); break;
        default: break;
        }
    }
};

Synth::Synth(Config config) : impl_(std::make_unique<Impl>(config)) {}
Synth::~Synth() = default;
Synth::Synth(Synth&&) noexcept = default;
Synth& Synth::operator=(Synth&&) noexcept = default;

Status Synth::load_wave_rom(const std::uint8_t* data, std::size_t size) {
    if (!data || size == 0)
        return {Error::invalid_argument, "ROM data pointer is null or size is zero"};
    if (size != kWaveRomSize)
        return {Error::rom_size_invalid, "MU50 SWP00 waveform ROM region must be exactly 4 MiB"};
    impl_->wave_rom.assign(data, data + size);
    return {};
}

Status Synth::load_wave_rom(const std::vector<std::uint8_t>& data) {
    return load_wave_rom(data.data(), data.size());
}

Status Synth::write_register(std::uint16_t address, std::uint8_t value) {
    if (address <= 0x0e) {
        impl_->control_regs[address] = value;
        if (address == 0x04) impl_->meg.set_control(value);
        if (address >= 0x08 && address <= 0x0b) {
            const unsigned group = 0x0b - address;
            for (unsigned bit = 0; bit < 8; ++bit)
                if (value & (1u << bit))
                    impl_->core.keyon(static_cast<int>(group * 8 + bit));
        }
        return {};
    }

    // These global windows occupy addresses that would otherwise decode as
    // the unused per-channel MEG register slots 0x0c-0x1b.
    if (address >= 0x180 && address <= 0x1ff) {
        impl_->meg.write_offset(address, value);
        return {};
    }
    if (address >= 0x200 && address <= 0x37f) {
        impl_->meg.write_constant(address, value);
        return {};
    }

    // MAME rchan() expands each logical register into a 32-voice window:
    // address = ((reg & 0x3e) << 5) | (reg & 1) | (voice << 1).
    if (address < 0x800) {
        const std::uint8_t reg = static_cast<std::uint8_t>(((address >> 5) & 0x3e) | (address & 1));
        const std::uint8_t channel = static_cast<std::uint8_t>((address >> 1) & 0x1f);
        if ((reg >= 0x08 && reg <= 0x0b) || (reg >= 0x20 && reg <= 0x37)) {
            impl_->write_channel(reg, channel, value);
            return {};
        }
        return {Error::invalid_argument, "Address is not a mapped SWP00 voice register"};
    }
    return {Error::invalid_argument, "Address is outside the implemented SWP00 register map"};
}

Status Synth::read_register(std::uint16_t address, std::uint8_t& value) const {
    if (address <= 0x0e) {
        // MAME 0.277: waverom_access_r always returns 0x00 (busy flag),
        // and waverom_val_r returns the byte at voice-31's address and
        // auto-increments that address (24-bit wrap).
        if (address == 0x01) {
            // HEAD state_r: computed voice state, not the stored
            // address byte (the firmware's only SWP00 read).
            value = impl_->state_r();
            return {};
        }
        if (address == 0x02) {
            value = 0x00;
            return {};
        }
        if (address == 0x03) {
            const auto& rom = impl_->wave_rom;
            const std::uint32_t adr = impl_->core.address(31);
            value = rom.empty() ? std::uint8_t(0)
                                : rom[adr & static_cast<std::uint32_t>(rom.size() - 1)];
            const std::uint32_t next = (adr + 1) & 0xffffff;
            impl_->core.set_address(31, next);
            impl_->channel_regs[31][0x31] = static_cast<std::uint8_t>((next >> 16) & 0xff);
            impl_->channel_regs[31][0x32] = static_cast<std::uint8_t>((next >> 8) & 0xff);
            impl_->channel_regs[31][0x33] = static_cast<std::uint8_t>(next & 0xff);
            return {};
        }
        value = impl_->control_regs[address];
        return {};
    }
    if (address >= 0x180 && address <= 0x1ff) {
        value = impl_->meg.read_offset(address);
        return {};
    }
    if (address >= 0x200 && address <= 0x37f) {
        value = impl_->meg.read_constant(address);
        return {};
    }
    if (address < 0x800) {
        const std::uint8_t reg = static_cast<std::uint8_t>(((address >> 5) & 0x3e) | (address & 1));
        const std::uint8_t channel = static_cast<std::uint8_t>((address >> 1) & 0x1f);
        if ((reg >= 0x08 && reg <= 0x0b) || (reg >= 0x20 && reg <= 0x37)) {
            value = impl_->channel_regs[channel][reg];
            return {};
        }
    }
    return {Error::invalid_argument, "Address is outside the implemented SWP00 register map"};
}

Status Synth::midi_message(std::uint8_t, std::uint8_t, std::uint8_t) {
    return {Error::unsupported_operation, "MIDI-to-tone-generator translation is not part of the SWP00 register core"};
}

Status Synth::sysex(const std::uint8_t*, std::size_t) {
    return {Error::unsupported_operation, "MU50 MIDI SysEx and tone-map handling are not implemented"};
}

Status Synth::render(std::int32_t* output, std::size_t frames) {
    if (frames && !output)
        return {Error::invalid_argument, "Output buffer is null"};
    if (impl_->config.sample_rate != kNativeRate)
        return {Error::unsupported_sample_rate, "The SWP00 core currently renders at its native 44100 Hz rate"};
    if (impl_->wave_rom.empty())
        return {Error::rom_not_loaded, "Load the MU50 waveform ROM before rendering"};

    // Voice registers are frozen for this whole call (all H8 writes
    // landed before render); derive per-voice constants once here
    // instead of once per frame in render_one.
    impl_->core.precompute_voices();
    for (std::size_t i = 0; i < frames; ++i) {
        const auto mix = impl_->core.render_one(impl_->wave_rom.data(), impl_->wave_rom.size());
        const auto routed = impl_->meg.process(mix);
        // MAME HEAD output staging: the MEG runs on the full 16.6
        // voice buses, and its dry output is converted to a signed
        // 18-bit ADC sample by >>6 with clamp to ±0x20000
        // (put_int_clamp). Running the MEG on unshifted buses
        // (instead of pre-shifting >>6 in render_one) lets the
        // distortion stage's saturate() in variation variant 0x80
        // engage exactly as in HEAD. This 32-bit render returns
        // that 18-bit DAC sample as-is (full scale 0x20000); the
        // 16-bit render below applies the >>2 truncation.
        auto adc = [](std::int64_t v) -> std::int32_t {
            v >>= 6;
            if (v > 0x20000) v = 0x20000;
            if (v < -0x20000) v = -0x20000;
            return static_cast<std::int32_t>(v);
        };
        output[2 * i] = adc(routed[0]);
        output[2 * i + 1] = adc(routed[1]);
    }
    return {};
}

Status Synth::render(std::int16_t* output, std::size_t frames) {
    // Same guards as render(int32_t*) and as the pre-PR#15 direct
    // path (c4d640c): null output, sample rate and ROM are validated
    // even for a 0-frame call, so render(nullptr, N) cannot write
    // through the null pointer and a 0-frame call still reports a
    // bad rate / missing ROM instead of silently succeeding.
    if (frames && !output)
        return {Error::invalid_argument, "Output buffer is null"};
    if (impl_->config.sample_rate != kNativeRate)
        return {Error::unsupported_sample_rate, "The SWP00 core currently renders at its native 44100 Hz rate"};
    if (impl_->wave_rom.empty())
        return {Error::rom_not_loaded, "Load the MU50 waveform ROM before rendering"};
    // 16-bit PCM via the 18-bit render, in chunks: the >>2 discards
    // the two low DAC bits, exactly as the old direct path did
    // (clamp_s16(adc >> 2)), so this is bit-identical to it. The
    // final clamp_s16 handles the 0x20000>>2 = 0x8000 edge (one
    // above int16 max).
    constexpr std::size_t kChunkFrames = 512;
    std::int32_t buf[2 * kChunkFrames];
    std::size_t done = 0;
    while (done < frames) {
        const std::size_t chunk = std::min(kChunkFrames, frames - done);
        Status st = render(buf, chunk);
        if (!st)
            return st;
        for (std::size_t i = 0; i < chunk; ++i) {
            output[2 * (done + i)] = clamp_s16(buf[2 * i] >> 2);
            output[2 * (done + i) + 1] = clamp_s16(buf[2 * i + 1] >> 2);
        }
        done += chunk;
    }
    return {};
}

void Synth::reset() { impl_->reset(); }

void Synth::set_filter_0277(bool v) noexcept {
    if (impl_)
        impl_->core.set_filter_0277(v);
}

const Config& Synth::config() const noexcept { return impl_->config; }
bool Synth::wave_rom_loaded() const noexcept { return !impl_->wave_rom.empty(); }
unsigned Synth::active_voices() const noexcept {
    unsigned n = 0;
    for (std::size_t ch = 0; ch < impl_->core.kChannels; ++ch)
        if (impl_->core.active(static_cast<int>(ch))) ++n;
    return n;
}

void Synth::snapshot_state(StateSnapshot& out) const noexcept {
    for (int chan = 0; chan < 32; ++chan) {
        out.voice[chan][0] = impl_->core.filter_status(chan);
        out.voice[chan][1] = impl_->core.envelope_status(chan);
        out.voice[chan][2] = impl_->core.mixer_status_glo(chan);
        out.voice[chan][3] = impl_->core.mixer_status_panl(chan);
        out.voice[chan][4] = impl_->core.mixer_status_panr(chan);
    }
    out.sample_counter = impl_->core.sample_counter();
}
const char* version() noexcept { return "HEAD-b75872c"; }

} // namespace libmu50
