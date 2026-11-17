// render_guards -- Synth::render guard cases (PR #15 review item 5).
//
// Round 1 found the null-buffer / sample-rate / ROM guards had been
// removed from external/mu50/src/synth.cpp: render(nullptr, 0) did
// not write through null, but render(nullptr, 100) segfaulted. The
// guards were restored; these 7 cases pin them (both render
// overloads share the same guard order: null output, sample rate,
// ROM -- validated even for a 0-frame call, so render(nullptr, N)
// cannot write through the null pointer and a 0-frame call still
// reports a bad rate / missing ROM instead of silently succeeding).
//
// Case list:
//   1. s16 render(nullptr, 0), no ROM -> rom_not_loaded (no crash,
//      no write through null; the null check is skipped for 0
//      frames, the ROM check still fires).
//   2. s16 render(nullptr, 100) -> invalid_argument (was segfault).
//   3. s32 render(nullptr, 100) -> invalid_argument.
//   4. s16 render(buf, 1), no ROM -> rom_not_loaded.
//   5. s32 render(buf, 1), no ROM -> rom_not_loaded.
//   6. render(buf, 1) at 48000 Hz -> unsupported_sample_rate
//      (the core renders at its native 44100 Hz only).
//   7. load_wave_rom with a wrong-size buffer -> rom_size_invalid
//      (the ROM region must be exactly 4 MiB).
// A valid-ROM render sanity check follows (not a guard case).
#include "libmu50/synth.hpp"

#include <cstdint>
#include <cstdio>
#include <vector>

using libmu50::Error;
using libmu50::Synth;

namespace {
int g_failures = 0;

void Check(const char* name, Error got, Error want) {
    if (got != want) {
        std::fprintf(stderr, "GUARD FAIL %s: got %d, want %d\n", name,
                     static_cast<int>(got), static_cast<int>(want));
        ++g_failures;
    } else {
        std::fprintf(stderr, "guard ok: %s\n", name);
    }
}
} // namespace

int main() {
    Synth synth; // native 44100 Hz, no ROM loaded
    std::int16_t s16buf[2] = {};
    std::int32_t s32buf[2] = {};

    Check("s16 render(nullptr,0) no-ROM", synth.render(static_cast<std::int16_t*>(nullptr), 0).error,
          Error::rom_not_loaded);
    Check("s16 render(nullptr,100)", synth.render(static_cast<std::int16_t*>(nullptr), 100).error,
          Error::invalid_argument);
    Check("s32 render(nullptr,100)", synth.render(static_cast<std::int32_t*>(nullptr), 100).error,
          Error::invalid_argument);
    Check("s16 render(buf,1) no-ROM", synth.render(s16buf, 1).error, Error::rom_not_loaded);
    Check("s32 render(buf,1) no-ROM", synth.render(s32buf, 1).error, Error::rom_not_loaded);

    libmu50::Config bad_rate;
    bad_rate.sample_rate = 48000;
    Synth synth48k(bad_rate);
    Check("render at 48000 Hz", synth48k.render(s16buf, 1).error, Error::unsupported_sample_rate);

    std::vector<std::uint8_t> wrong_size(16, 0);
    Check("load_wave_rom wrong size", synth.load_wave_rom(wrong_size).error, Error::rom_size_invalid);

    // Sanity: a valid 4 MiB ROM loads and renders (not a guard case).
    std::vector<std::uint8_t> rom(0x400000, 0);
    for (std::size_t i = 0; i < rom.size(); i += 2) {
        rom[i] = 0x00;
        rom[i + 1] = 0x20;
    }
    if (!synth.load_wave_rom(rom)) {
        std::fprintf(stderr, "SANITY FAIL: valid ROM did not load\n");
        ++g_failures;
    } else if (!synth.render(s16buf, 1) || !synth.render(s32buf, 1)) {
        std::fprintf(stderr, "SANITY FAIL: render with valid ROM failed\n");
        ++g_failures;
    } else {
        std::fprintf(stderr, "sanity ok: valid ROM renders\n");
    }

    if (g_failures != 0) {
        std::fprintf(stderr, "render_guards: FAIL (%d failures)\n", g_failures);
        return 1;
    }
    std::fprintf(stderr, "render_guards: PASS (7/7 guard cases)\n");
    return 0;
}
