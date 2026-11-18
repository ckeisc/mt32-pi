#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace libmu50 {

// Voice pipeline ported from MAME 0.277's swp00_device (pre-overhaul
// synthesis). The HEAD-based toggles (bypass_filter, nearest_interp,
// wrap_decode) do NOT exist here: 0.277's filter and interpolation are
// integral to its voice loop, and its sequential-by-address DPCM decode
// has no loop-byte skip to work around.
struct Config {
    std::uint32_t sample_rate = 44100;
    std::uint32_t chip_clock_hz = 33868800;
    // Ablation toggles for differential diagnosis ONLY (default off =
    // normal behavior). Each silences one MEG subsystem's output
    // contribution so a broken subsystem can be identified by
    // elimination: if disabling X moves the output closer to the
    // golden, X's rendering (not its absence) is the error source.
    bool no_reverb = false;
    bool no_chorus = false;
    bool no_variation = false;
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
    // register-level path renders 0.277-style AWM2 voices through the four
    // MAME-derived MEG variation programs, chorus and reverb networks.
    Status render(std::int16_t* interleaved_stereo, std::size_t frames);

    // Render stereo interleaved signed 18-bit DAC samples (full scale
    // 0x20000) as int32 at 44.1 kHz. Same samples as render(int16_t*),
    // without the >>2 truncation to 16-bit: the DAC is 18-bit, and the
    // caller (the mt32-pi wrapper) applies its own scale/trim to the
    // full 18-bit value instead of losing the two low DAC bits here.
    Status render(std::int32_t* interleaved_stereo, std::size_t frames);

    void reset();
    const Config& config() const noexcept;
    bool wave_rom_loaded() const noexcept;
    // Number of voices currently active (read-only split-diag probe;
    // sums the core's per-voice active flags, no behavior change).
    unsigned active_voices() const noexcept;

    // Route A hybrid (test): select the MAME 0.277 filter behaviours
    // (sweep + cutoff cap) in the SWP00 core. Default false (HEAD).
    // When true, a key-on with the filter-speed bit 7 set puts the
    // cutoff straight to its target (no muffled onset) and the sweep
    // is a hidden timer, and info_w caps the cutoff at 0x7c0 when the
    // resonance bits are below 4.
    void set_filter_0277(bool v) noexcept;

    // Per-block SWP00 state snapshot for the split-mode readback path.
    // The SWP thread owns the live Synth; the audio thread must never
    // read it. The SWP thread captures this after each rendered block
    // and publishes it; the audio thread computes state_r (register
    // 0x01) from the snapshot using the shadowed state address. One
    // block of lag, no live-state race. Plain value, no behavior change.
    struct StateSnapshot {
        // Per-voice status, indexed [chan][slot]:
        //   [0] filter_status (state_r case 0)
        //   [1] envelope_status (case 2)
        //   [2] mixer_status_glo (case 3)
        //   [3] mixer_status_panl (case 4)
        //   [4] mixer_status_panr (case 5)
        // state_r case 1 is the sample counter (0xc0 | counter>>13).
        std::uint8_t voice[32][5];
        std::uint32_t sample_counter;
    };
    // Capture the current voice/mixer state. Call only from the thread
    // that owns the Synth (the SWP thread in split mode).
    void snapshot_state(StateSnapshot& out) const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

const char* version() noexcept;

} // namespace libmu50
