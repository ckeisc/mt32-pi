// Voice-core tests for the vendored libmu50 (MAME HEAD port, b75872c
// + 84fde62/40dd6fc corrections).
//
// This is the HEAD adaptation of the old 0.277 swp00_0277 test -- it
// CANNOT be ported verbatim, because the vendored core is the HEAD
// synthesis-block port, not the 0.277 flat-array port:
//   - The 0.277 helpers (istep/fpstep/fpadd/fpsub/fpapply/lpffpapply)
//     and tables are gone in HEAD; there is no equivalent to test.
//   - The DPCM wrap assertions of the 0.277 test encode 0.277's
//     sequential-decode-with-sum-reset semantics. HEAD does NOT
//     reset the predictor sum on wrap (it resets dpcm_pos to 1 and
//     keeps the stale sum -- the old test's own comment calls this
//     "HEAD-style skipping"). Those assertions are therefore NOT
//     ported; instead this test pins HEAD's actual wrap behavior
//     (stream_pos wraps to 0, dpcm_pos resets to 1, the predictor
//     sum is kept, so dpcm_s1 stays nonzero).
//   - Setter names follow HEAD hardware names (set_loop, set_pan,
//     set_lfo_lamod/speed/fmod); accessors are the HEAD streaming
//     state (stream_pos, dpcm_s0/s1/pos/delta).
// What IS ported, with values re-derived from the HEAD source
// (StreamingBlock::keyon/step, EnvelopeBlock/FilterBlock/MixerBlock
// in external/mu50/src/swp00_0277.cpp):
//   - key-on state, the three other sample encodings producing
//     nonzero dry output, and the ROM mirror.
#include "swp00_0277.hpp"
#include <array>
#include <cassert>
#include <cstdint>

using namespace libmu50::detail;

namespace {
void setup_voice(Swp0277Core& core, int chan, std::uint8_t format,
                 std::uint32_t address, std::uint16_t start, std::uint16_t loop,
                 std::uint16_t pitch) {
    core.set_format(chan, format);
    core.set_address(chan, address);
    core.set_start(chan, start);
    core.set_loop(chan, loop);
    core.set_pitch(chan, pitch);
    core.set_lpf_info(chan, 0x27ff);
    core.set_lpf_speed(chan, 0);
    core.set_lfo_lamod(chan, 0);
    core.set_lfo_speed(chan, 0);
    core.set_lfo_fmod(chan, 0);
    core.set_attack_speed(chan, 0);
    core.set_attack_level(chan, 0);
    core.set_decay_speed(chan, 0);
    core.set_decay_level(chan, 0);
    core.set_glo_level(chan, 0);
    core.set_pan(chan, 0x80); // center: pan_l = 0x200, pan_r = 0
    core.set_dry_level(chan, 0);
    core.set_reverb_level(chan, 0);
    core.set_chorus_level(chan, 0);
    core.set_variation_level(chan, 0);
    core.keyon(chan);
}
} // namespace

int main() {
    // Key-on state (StreamingBlock::keyon, HEAD): m_pos = -start,
    // m_pos_dec = (phase<<1)&0x7ffe (phase resets to 0x8000, so 0),
    // DPCM predictor cleared, dpcm_pos = (u32)(m_pos+1), voice active
    // (envelope keyon with speed/level 0 -> level 0, ATTACK).
    Swp0277Core core;
    core.reset();
    core.set_address(0, 100);
    core.set_start(0, 5);
    core.keyon(0);
    assert(core.stream_pos(0) == -5);
    assert(core.stream_pos_dec(0) == 0);
    assert(core.dpcm_s0(0) == 0 && core.dpcm_s1(0) == 0);
    assert(core.dpcm_pos(0) == 0xfffffffcU); // (u32)(-5 + 1)
    assert(core.dpcm_delta(0) == 0);
    assert(core.active(0));

    // DPCM loop wrap (HEAD): format 0xc0, loop 8, pitch 0x0800
    // advances exactly 1 position/frame and decodes one byte/frame.
    // After 8 frames m_pos wraps 8 -> 0, dpcm_pos resets to 1, and
    // the predictor sum is NOT reset (dpcm_s1 stays nonzero -- the
    // HEAD behavior the 0.277 test distinguished itself from).
    // ROM bytes are all 0x20 (dpcm_expand 32, positive every step).
    std::array<std::uint8_t, 64> rom{};
    for (int i = 0; i < 16; ++i) rom[static_cast<std::size_t>(i)] = 0x20;
    Swp0277Core dpcm;
    dpcm.reset();
    setup_voice(dpcm, 0, 0xc0, 0, 0, 8, 0x0800);
    assert(dpcm.stream_pos(0) == 0);
    assert(dpcm.dpcm_pos(0) == 1);
    for (int frame = 0; frame < 8; ++frame)
        (void)dpcm.render_one(rom.data(), rom.size());
    assert(dpcm.stream_pos(0) == 0); // wrapped
    assert(dpcm.dpcm_pos(0) == 1);   // HEAD wrap reset
    assert(dpcm.dpcm_s1(0) != 0);    // predictor sum kept, not reset
    assert(dpcm.active(0));

    // The other three sample encodings produce nonzero dry output
    // through the same pipeline (filter info 0x27ff, center pan,
    // dry send path).
    std::array<std::uint8_t, 64> encoded_rom{};
    encoded_rom.fill(0x7f);
    auto run_format = [&](std::uint8_t format) {
        Swp0277Core c;
        c.reset();
        setup_voice(c, 0, format, 0, 0, 32, 0x0800);
        VoiceMix mix{};
        for (int i = 0; i < 8; ++i) mix = c.render_one(encoded_rom.data(), encoded_rom.size());
        return mix.dry_l + mix.dry_r;
    };
    assert(run_format(0x00) != 0); // 16-bit PCM
    assert(run_format(0x40) != 0); // packed 12-bit PCM
    assert(run_format(0x80) != 0); // 8-bit PCM

    // MAME's device_rom_interface mirrors a power-of-two ROM over
    // the 24-bit bus. The test region is 64 bytes, so address
    // 0x500000 aliases 0 (read_byte masks with rom_size - 1).
    Swp0277Core open_rom;
    open_rom.reset();
    setup_voice(open_rom, 0, 0x00, 0x500000, 0, 32, 0x0800);
    VoiceMix mix{};
    for (int i = 0; i < 4; ++i) mix = open_rom.render_one(encoded_rom.data(), encoded_rom.size());
    assert(mix.dry_l != 0 || mix.dry_r != 0);
    return 0;
}
