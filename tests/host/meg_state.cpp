// MEG state test for the 0.277-based libmu50. Mirrors the HEAD port's
// meg_state test except the dry-path expectation: the 0.277 MEG returns
// the full-domain dry (MAME 0.277's put_int(i, dry, 32768) delivers
// int16 = clamp(dry) directly), so dry 16384 with unity gains comes back
// as 16384 — not 256 (the HEAD port's >>6 ADC staging, which does not
// apply to the 0.277 output path).
#include "swp00_meg.hpp"
#include <cassert>
#include <cstdint>

int main() {
    libmu50::detail::MegState meg;
    meg.write_offset(0x180, 0x12);
    meg.write_offset(0x181, 0x34);
    assert(meg.read_offset(0x180) == 0x12);
    assert(meg.read_offset(0x181) == 0x34);
    meg.write_constant(0x200, 0x02);
    meg.write_constant(0x201, 0x00);
    assert(meg.constant_signed(0) == 0x200);
    meg.write_constant(0x202, 0x03);
    meg.write_constant(0x203, 0xff);
    assert(meg.constant_signed(1) == -1);
    assert(meg.mul7(256, 64) == 128);
    assert(meg.mul9(1024, 256) == 512);
    // Dry path: MEG applies signed 7-bit gains, then returns the
    // full-domain dry (0.277 put_int domain, no >>6).
    meg.write_constant(0x200 + 0xbe * 2, 0x00);
    meg.write_constant(0x200 + 0xbe * 2 + 1, 0x80);
    meg.write_constant(0x200 + 0x01 * 2, 0x00);
    meg.write_constant(0x200 + 0x01 * 2 + 1, 0x80);
    libmu50::detail::VoiceMix input;
    input.dry_l = 16384;
    input.dry_r = -16384;
    const auto routed = meg.process(input);
    assert(routed[0] == 16384);
    assert(routed[1] == -16384);

    meg.set_control(0xc0);
    assert(meg.control() == 0xc0);
    meg.reset();
    assert(meg.read_offset(0x180) == 0);
    assert(meg.read_constant(0x200) == 0);
    assert(meg.control() == 0);

    // Exercise each fixed variation program selector through its complete
    // processing branch. These are stability/coverage checks, not MAME goldens.
    input.variation_l = 1200;
    input.variation_r = -900;
    input.chorus_l = 300;
    input.chorus_r = -250;
    input.reverb = 180;
    for (std::uint8_t selector : {std::uint8_t(0x00), std::uint8_t(0x40),
                                  std::uint8_t(0x80), std::uint8_t(0xc0)}) {
        meg.reset();
        meg.set_control(selector);
        for (int frame = 0; frame < 8; ++frame)
            (void)meg.process(input);
    }
}
