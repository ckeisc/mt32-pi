// Smoke test for the 0.277-based libmu50. Mirrors the HEAD port's smoke
// test except where 0.277 semantics differ:
// - 0.277 has no phase register (0x08/0x09 are ignored), so no phase writes.
// - 0.277 deactivates a voice with end == 0 after its first sample, so
//   the test voice gets an explicit loop end (0x36/0x37) to survive the
//   32-frame render.
#include "libmu50/synth.hpp"

#include <cassert>
#include <cstdint>
#include <vector>

int main() {
    libmu50::Synth synth;
    assert(!synth.wave_rom_loaded());

    std::vector<std::uint8_t> wrong_size(16, 0);
    const auto bad_rom = synth.load_wave_rom(wrong_size);
    assert(bad_rom.error == libmu50::Error::rom_size_invalid);

    std::int16_t sample[2]{};
    const auto no_rom = synth.render(sample, 1);
    assert(no_rom.error == libmu50::Error::rom_not_loaded);

    // Exercise the physical SWP00 channel-window address decoder and the
    // register-level readback path (channel 7, global-level register 0x2e).
    auto channel_address = [](std::uint8_t reg, std::uint8_t channel) {
        return static_cast<std::uint16_t>(((reg & 0x3e) << 5) | (reg & 1) | (channel << 1));
    };
    const auto global_address = channel_address(0x2e, 7);
    assert(synth.write_register(global_address, 0x35));
    std::uint8_t readback = 0;
    assert(synth.read_register(global_address, readback));
    assert(readback == 0x35);
    assert(synth.write_register(0x0f, 0).error == libmu50::Error::invalid_argument);
    // 0x0c-0x1b are not channel-window registers; the physical 0x180+ window
    // is global offset/constant storage and must not alias voice registers.
    assert(synth.write_register(0x180, 0x12));
    assert(synth.read_register(0x180, readback));
    assert(readback == 0x12);
    assert(synth.write_register(0x200, 0x34));
    assert(synth.read_register(0x200, readback));
    assert(readback == 0x34);

    // Loading a synthetic 4 MiB ROM enables the native-rate register-level
    // render path. This is deliberately not an end-to-end MU50/MIDI test.
    std::vector<std::uint8_t> rom(0x400000, 0);
    for (std::size_t i = 0; i < rom.size(); i += 2) {
        rom[i] = 0x00;
        rom[i + 1] = 0x20; // little-endian +8192 PCM samples
    }
    assert(synth.load_wave_rom(rom));

    // Unity 7-bit dry-bus gains in the MAME MEG constant register format.
    assert(synth.write_register(0x37c, 0x00));
    assert(synth.write_register(0x37d, 0x80));
    assert(synth.write_register(0x202, 0x00));
    assert(synth.write_register(0x203, 0x80));

    auto write_voice = [&](std::uint8_t reg, std::uint8_t channel, std::uint8_t value) {
        return synth.write_register(channel_address(reg, channel), value);
    };
    assert(write_voice(0x20, 0, 0xff)); // filter info high
    assert(write_voice(0x21, 0, 0xff)); // filter info low
    // 0.277 mixer gate: fpapply returns 0 for summed levels >= 0x10000000,
    // so the attack level must stay modest (0xff would silence the voice
    // for ~132 frames: envelope 0xFF00000 + pan 0x2000000 >= 0x10000000).
    assert(write_voice(0x26, 0, 0x00)); // attack speed (slow decay)
    assert(write_voice(0x27, 0, 0x40)); // attack level (envelope 0x4000000)
    assert(write_voice(0x28, 0, 0x00)); // decay speed
    assert(write_voice(0x29, 0, 0x00)); // decay level
    assert(write_voice(0x2e, 0, 0x00)); // global attenuation
    assert(write_voice(0x2f, 0, 0x80)); // center pan
    assert(write_voice(0x2b, 0, 0x00)); // dry send
    assert(write_voice(0x30, 0, 0x00)); // 16-bit PCM
    assert(write_voice(0x31, 0, 0x00));
    assert(write_voice(0x32, 0, 0x00));
    assert(write_voice(0x33, 0, 0x00)); // sample address 0
    assert(write_voice(0x34, 0, 0x00));
    assert(write_voice(0x35, 0, 0x01)); // pitch
    assert(write_voice(0x36, 0, 0x00));
    assert(write_voice(0x37, 0, 0x20)); // loop end 32 (0.277: end 0 dies)
    assert(synth.write_register(0x04, 0xc0)); // select MEG variation program 3
    assert(synth.read_register(0x04, readback));
    assert(readback == 0xc0);
    assert(synth.write_register(0x0b, 0x01)); // key-on voice 0

    std::vector<std::int16_t> audio(32 * 2, 0);
    assert(synth.render(audio.data(), 32));
    bool audible = false;
    for (auto value : audio) audible = audible || value != 0;
    assert(audible);
    return 0;
}
