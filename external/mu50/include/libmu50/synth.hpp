#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace libmu50 {

struct Config {
    std::uint32_t sample_rate = 44100;
    std::uint32_t chip_clock_hz = 33868800;
};

enum class Error {
    none,
    invalid_argument,
    rom_not_loaded,
    rom_size_invalid,
    unsupported_sample_rate,
    unsupported_operation
};

struct Status {
    Error error = Error::none;
    std::string message;
    explicit operator bool() const noexcept { return error == Error::none; }
};

// Host-facing API for the standalone SWP00 register core. The type hides all
// MAME device types; MIDI-to-MU50-part translation is a separate TODO.
class Synth final {
public:
    explicit Synth(Config config = {});
    ~Synth();

    Synth(Synth&&) noexcept;
    Synth& operator=(Synth&&) noexcept;
    Synth(const Synth&) = delete;
    Synth& operator=(const Synth&) = delete;

    // Loads the 4 MiB MU50 SWP00 waveform ROM region. Caller retains ownership.
    Status load_wave_rom(const std::uint8_t* data, std::size_t size);
    Status load_wave_rom(const std::vector<std::uint8_t>& data);

    // Raw SWP00 register access. Channel registers use the SWP00 channel-window
    // address encoding; control/global registers use their documented addresses.
    Status write_register(std::uint16_t address, std::uint8_t value);
    Status read_register(std::uint16_t address, std::uint8_t& value) const;

    // Raw MIDI channel/system messages (one complete MIDI message per call).
    Status midi_message(std::uint8_t status, std::uint8_t data1, std::uint8_t data2);
    Status sysex(const std::uint8_t* data, std::size_t size);

    // Render stereo interleaved signed 16-bit PCM at 44.1 kHz. The current
    // register-level path renders AWM2 voices through the four MAME-derived MEG variation programs, chorus and reverb networks.
    Status render(std::int16_t* interleaved_stereo, std::size_t frames);

    void reset();
    const Config& config() const noexcept;
    bool wave_rom_loaded() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

const char* version() noexcept;

} // namespace libmu50
