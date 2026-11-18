// keyon_test -- KeyOnStatus (libmu50::KeyOnStatus, PR #15 Finding 3)
// regression gate: 4000 seeded random cases + hand-derived goldens.
//
// What this pins: KeyOnStatus() drives a scratch Swp0277Core through
// its public setters + keyon(0) + status accessors (one source of
// truth, PR #15 review item 8). The 4000-case random comparison
// drives a SECOND scratch core directly in this test with the same
// register values and compares all 5 status bytes. That guards the
// KeyOnStatus REGISTER MAPPING (a swapped setter argument, a wrong
// lpf_info byte order, a missed filter_0277 flag would all fail
// here); it does NOT independently verify the core's keyon/status
// formulas, which are pinned by the hand-derived goldens below
// (computed from the core source: EnvelopeBlock::keyon/status,
// FilterBlock::keyon/status/info_w, MixerBlock::keyon/status_glo/
// status_panl/status_panr in external/mu50/src/swp00_0277.cpp) and
// by swp00_0277.cpp's MAME-verbatim assertions.
//
// History: PR #15's round-1 response cited a 4000-case gate against
// a hand transcription of the same formulas, but that test's source
// was never put in the repo (it lived in /tmp) and is lost. This
// test replaces it: same case count, same seed discipline (fixed
// seed, reproducible), mapping check + goldens instead of the lost
// transcription comparison. The claim it supports is "KeyOnStatus
// matches the core's own keyon()/status() over 4000 random register
// combinations, 0 mismatches", plus the 7 golden cases below.
#include "libmu50/keyon_status.hpp"
#include "swp00_0277.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>

using libmu50::KeyOnRegs;
using libmu50::detail::Swp0277Core;

namespace {

// Fixed-seed splitmix64: the 4000 cases are reproducible run to run.
struct Rng {
    std::uint64_t s;
    std::uint64_t next() {
        std::uint64_t z = (s += 0x9e3779b97f4a7c15ULL);
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
        return z ^ (z >> 31);
    }
    std::uint8_t u8() { return static_cast<std::uint8_t>(next() & 0xff); }
    std::uint16_t u16() { return static_cast<std::uint16_t>(next() & 0xffff); }
    bool bit() { return (next() & 1) != 0; }
};

// Reference: drive a scratch core directly (the same calls
// KeyOnStatus makes internally, written out here so a wiring
// mistake in KeyOnStatus shows up as a mismatch).
void ReferenceStatus(const KeyOnRegs& r, std::uint8_t out[5]) {
    Swp0277Core core;
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

int g_failures = 0;

void CheckGolden(const char* name, const KeyOnRegs& r, const std::uint8_t expect[5]) {
    std::uint8_t got[5] = {};
    libmu50::KeyOnStatus(r, got);
    if (std::memcmp(got, expect, 5) != 0) {
        std::fprintf(stderr, "GOLDEN FAIL %s: got [%02x %02x %02x %02x %02x], want [%02x %02x %02x %02x %02x]\n",
                     name, got[0], got[1], got[2], got[3], got[4],
                     expect[0], expect[1], expect[2], expect[3], expect[4]);
        ++g_failures;
    }
}

} // namespace

int main() {
    // Hand-derived goldens (see the header comment for the formulas).
    // out = [filter, envelope, mixer glo, mixer panl, mixer panr].
    const KeyOnRegs kReset = {}; // attack 0/0, decay 0, info 0x27ff,
                                 // speed 0, glo/pan 0xff, 0277 off
    {
        // Reset-like: filter k=0xfff at target, sweep none -> 0xc0;
        // envelope level 0 ATTACK -> 0x00; glo 0xff -> 0xc0|0x3f=0xff;
        // pan 0xff -> both 0xfff -> 0xff, 0xff.
        const std::uint8_t want[5] = {0xc0, 0x00, 0xff, 0xff, 0xff};
        CheckGolden("reset-like", kReset, want);
    }
    {
        // Normal attack: attack_speed 0x80, attack_level 0x40 ->
        // level 0x400 ATTACK -> (0<<6)|(0x400>>6)=0x10; glo 0x40 ->
        // 0xc0|0x10=0xd0; pan 0x80 (center) -> panl 0xc8, panr 0xc0.
        KeyOnRegs r = kReset;
        r.attack_speed = 0x80; r.attack_level = 0x40;
        r.glo = 0x40; r.pan = 0x80;
        const std::uint8_t want[5] = {0xc0, 0x10, 0xd0, 0xc8, 0xc0};
        CheckGolden("normal-attack", r, want);
    }
    {
        // Immediate release: decay_speed bit 7 -> level 0 DECAY(2)
        // -> (2<<6)|0 = 0x80.
        KeyOnRegs r = kReset;
        r.decay_speed = 0x80;
        const std::uint8_t want[5] = {0xc0, 0x80, 0xff, 0xff, 0xff};
        CheckGolden("immediate-release", r, want);
    }
    {
        // 0277 sweep: speed bit 7 + filter_0277 -> k straight to
        // target 0xfff, sweep UP (not done) -> 0x00|((0xfff>>6)^0x3f)=0.
        KeyOnRegs r = kReset;
        r.lpf_speed = 0x80; r.filter_0277 = true;
        const std::uint8_t want[5] = {0x00, 0x00, 0xff, 0xff, 0xff};
        CheckGolden("sweep-0277", r, want);
    }
    {
        // HEAD sweep: speed bit 7, filter_0277 off -> k starts at
        // 0x800, sweep UP -> 0x00|((0x800>>6)^0x3f)=0x1f.
        KeyOnRegs r = kReset;
        r.lpf_speed = 0x80; r.filter_0277 = false;
        const std::uint8_t want[5] = {0x1f, 0x00, 0xff, 0xff, 0xff};
        CheckGolden("sweep-head", r, want);
    }
    {
        // Zero filter: info 0 -> k_target 0, k 0, sweep none at
        // target -> 0xc0|(0^0x3f)=0xff.
        KeyOnRegs r = kReset;
        r.lpf_info = 0x0000;
        const std::uint8_t want[5] = {0xff, 0x00, 0xff, 0xff, 0xff};
        CheckGolden("zero-filter", r, want);
    }
    {
        // Silent mixer: glo 0 -> 0xc0, pan 0 -> both 0xc0.
        KeyOnRegs r = kReset;
        r.glo = 0x00; r.pan = 0x00;
        const std::uint8_t want[5] = {0xc0, 0x00, 0xc0, 0xc0, 0xc0};
        CheckGolden("silent-mixer", r, want);
    }
    std::fprintf(stderr, "goldens: %d failures\n", g_failures);

    // 4000 seeded random cases vs the directly-driven scratch core.
    Rng rng{0x4b45594f4e535441ULL}; // "KEYONSTA"
    int mismatches = 0;
    int first_case = -1;
    KeyOnRegs first_regs{};
    std::uint8_t first_got[5] = {}, first_want[5] = {};
    for (int i = 0; i < 4000; ++i) {
        KeyOnRegs r;
        r.attack_speed = rng.u8();
        r.attack_level = rng.u8();
        r.decay_speed = rng.u8();
        r.lpf_info = rng.u16();
        r.lpf_speed = rng.u8();
        r.glo = rng.u8();
        r.pan = rng.u8();
        r.filter_0277 = rng.bit();
        std::uint8_t got[5] = {}, want[5] = {};
        libmu50::KeyOnStatus(r, got);
        ReferenceStatus(r, want);
        if (std::memcmp(got, want, 5) != 0) {
            if (mismatches == 0) {
                first_case = i; first_regs = r;
                std::memcpy(first_got, got, 5);
                std::memcpy(first_want, want, 5);
            }
            ++mismatches;
        }
    }
    std::fprintf(stderr, "random: 4000 cases, %d mismatches\n", mismatches);
    if (mismatches > 0) {
        std::fprintf(stderr, "first mismatch case %d: regs atk_spd=%02x atk_lvl=%02x dec_spd=%02x info=%04x spd=%02x glo=%02x pan=%02x f0277=%d\n",
                     first_case, first_regs.attack_speed, first_regs.attack_level,
                     first_regs.decay_speed, first_regs.lpf_info, first_regs.lpf_speed,
                     first_regs.glo, first_regs.pan, first_regs.filter_0277 ? 1 : 0);
        std::fprintf(stderr, "  got  [%02x %02x %02x %02x %02x] want [%02x %02x %02x %02x %02x]\n",
                     first_got[0], first_got[1], first_got[2], first_got[3], first_got[4],
                     first_want[0], first_want[1], first_want[2], first_want[3], first_want[4]);
        ++g_failures;
    }
    if (g_failures != 0) {
        std::fprintf(stderr, "keyon_test: FAIL (%d failures)\n", g_failures);
        return 1;
    }
    std::fprintf(stderr, "keyon_test: PASS (7 goldens, 4000 random, 0 mismatches)\n");
    return 0;
}
