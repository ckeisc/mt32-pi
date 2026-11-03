#include "libmu50/synth.hpp"
#include "swp00_voice.hpp"
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
    explicit Impl(Config cfg) : config(cfg) { reset(); }

    Config config;
    std::vector<std::uint8_t> wave_rom;
    detail::Awm2Core awm2;
    std::array<std::array<std::uint8_t, 0x40>, 32> channel_regs{};
    detail::MegState meg;
    std::array<std::uint8_t, 0x10> control_regs{};
    std::uint32_t sample_counter = 0;

    void reset() {
        awm2.clear();
        for (auto& r : channel_regs) r.fill(0);
        meg.reset();
        control_regs.fill(0);
        sample_counter = 0;
    }

    void write_channel(std::uint8_t reg, std::uint8_t channel, std::uint8_t value) {
        auto& shadow = channel_regs[channel][reg];
        shadow = value;
        auto& v = awm2.voice(channel);
        auto pair = [&](std::uint8_t lo_reg, std::uint8_t hi_reg) {
            return std::uint16_t(channel_regs[channel][lo_reg]) |
                   (std::uint16_t(channel_regs[channel][hi_reg]) << 8);
        };
        switch (reg) {
        case 0x08: case 0x09: v.set_phase(pair(0x09, 0x08)); break;
        case 0x0a: case 0x0b: v.set_start(pair(0x0b, 0x0a)); break;
        case 0x20: v.set_filter_info_high(value); break;
        case 0x21: v.set_filter_info_low(value); break;
        case 0x22: v.set_filter_speed(value); break;
        case 0x23: v.set_lfo_lamod(value); break;
        case 0x24: v.set_lfo_speed(value); break;
        case 0x25: v.set_lfo_fmod(value); break;
        case 0x26: v.set_attack_speed(value); break;
        case 0x27: v.set_attack_level(value); break;
        case 0x28: v.set_decay_speed(value); break;
        case 0x29: v.set_decay_level(value); break;
        case 0x2a: v.set_reverb(value); break;
        case 0x2b: v.set_dry(value); break;
        case 0x2c: v.set_chorus(value); break;
        case 0x2d: v.set_variation(value); break;
        case 0x2e: v.set_global(value); break;
        case 0x2f: v.set_pan(value); break;
        case 0x30: v.set_format(value); break;
        case 0x31: case 0x32: case 0x33:
            v.set_address(std::uint32_t(channel_regs[channel][0x33]) |
                          (std::uint32_t(channel_regs[channel][0x32]) << 8) |
                          (std::uint32_t(channel_regs[channel][0x31]) << 16));
            break;
        case 0x34: case 0x35: v.set_pitch(pair(0x35, 0x34)); break;
        case 0x36: case 0x37: v.set_loop(pair(0x37, 0x36)); break;
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
                    impl_->awm2.voice(group * 8 + bit).keyon();
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

    for (std::size_t i = 0; i < frames; ++i) {
        const auto mix = impl_->awm2.render_one(impl_->wave_rom.data(), impl_->wave_rom.size(), impl_->sample_counter++);
        const auto routed = impl_->meg.process(mix);
        // MAME presents the MEG result as signed 18-bit ADC samples. Convert
        // that normalized range to host signed 16-bit PCM.
        output[2 * i] = clamp_s16(std::int64_t(routed[0]) >> 2);
        output[2 * i + 1] = clamp_s16(std::int64_t(routed[1]) >> 2);
    }
    return {};
}

void Synth::reset() { impl_->reset(); }
const Config& Synth::config() const noexcept { return impl_->config; }
bool Synth::wave_rom_loaded() const noexcept { return !impl_->wave_rom.empty(); }
const char* version() noexcept { return "0.2.0-dev"; }

} // namespace libmu50
