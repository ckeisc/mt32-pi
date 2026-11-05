// license:BSD-3-Clause
// SWP00 register shell for the MAME 0.277-based voice core. Address
// decoding mirrors MAME 0.277's rchan/rctrl maps; the voice semantics
// live in Swp0277Core and the MEG tail in MegState.
#include "libmu50/synth.hpp"
#include "swp00_0277.hpp"
#include "swp00_meg.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <utility>

namespace libmu50 {
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
        meg.reset();
        control_regs.fill(0);
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
        // 0x08/0x09 are the HEAD phase registers; 0.277 has no phase
        // register, so these writes are shadow-only (ignored by the core).
        case 0x08: case 0x09: break;
        case 0x0a: case 0x0b: c.set_start(channel, pair(0x0b, 0x0a)); break;
        case 0x20: case 0x21: c.set_lpf_info(channel, pair(0x21, 0x20)); break;
        case 0x22: c.set_lpf_speed(channel, value); break;
        case 0x23: c.set_lfo_famod_depth(channel, value); break;
        case 0x24: c.set_lfo_step(channel, value); break;
        case 0x25: c.set_lfo_pmod_depth(channel, value); break;
        case 0x26: c.set_attack_speed(channel, value); break;
        case 0x27: c.set_attack_level(channel, value); break;
        case 0x28: c.set_decay_speed(channel, value); break;
        case 0x29: c.set_decay_level(channel, value); break;
        case 0x2a: c.set_reverb_level(channel, value); break;
        case 0x2b: c.set_dry_level(channel, value); break;
        case 0x2c: c.set_chorus_level(channel, value); break;
        case 0x2d: c.set_variation_level(channel, value); break;
        case 0x2e: c.set_glo_level(channel, value); break;
        case 0x2f: c.set_panning(channel, value); break;
        case 0x30: c.set_format(channel, value); break;
        case 0x31: case 0x32: case 0x33:
            c.set_address(channel, std::uint32_t(channel_regs[channel][0x33]) |
                          (std::uint32_t(channel_regs[channel][0x32]) << 8) |
                          (std::uint32_t(channel_regs[channel][0x31]) << 16));
            break;
        case 0x34: case 0x35: c.set_pitch(channel, pair(0x35, 0x34)); break;
        case 0x36: case 0x37: c.set_end(channel, pair(0x37, 0x36)); break;
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

Status Synth::render(std::int16_t* output, std::size_t frames) {
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
        // MAME 0.277 output staging: put_int(i, dry, 32768) delivers
        // int16 = clamp(dry) directly. No >>2 (that was HEAD's
        // put_int_clamp staging, which does not apply here).
        // Mix headroom (2026-11-04, Silpheed forensics): the s32
        // voice+MEG sum peaked at 49031 on Silpheed's first 120 s
        // (1.496x the int16 rail) and 40828 on E1M1, so the clamp
        // below flattened loud passages at gain 1.0. Scale the sum
        // by H = (32768 / 49031) * 0.95 ~= 0.6349 BEFORE the clamp,
        // same factor both channels, so the loudest measured
        // material peaks at ~95% of full scale unclamped.
        // Downstream [mu50] gain restores full loudness cleanly.
        // routed is s32, exactly representable in double, so the
        // multiply loses no precision on large sums.
        constexpr double kMixHeadroom = (32768.0 / 49031.0) * 0.95;
        output[2 * i] = clamp_s16(static_cast<std::int64_t>(routed[0] * kMixHeadroom));
        output[2 * i + 1] = clamp_s16(static_cast<std::int64_t>(routed[1] * kMixHeadroom));
    }
    return {};
}

void Synth::reset() { impl_->reset(); }
const Config& Synth::config() const noexcept { return impl_->config; }
bool Synth::wave_rom_loaded() const noexcept { return !impl_->wave_rom.empty(); }
unsigned Synth::active_voices() const noexcept {
    unsigned n = 0;
    for (std::size_t ch = 0; ch < impl_->core.kChannels; ++ch)
        if (impl_->core.active(static_cast<int>(ch))) ++n;
    return n;
}
const char* version() noexcept { return "0.277.0"; }

} // namespace libmu50
