// license:BSD-3-Clause
// keyon_status.hpp -- post-key-on SWP00 voice status, Circle-free.
//
// Finding 3 (mu50-audio-findings-20261117): in split mode the firmware
// reads voice state (register 0x01) from a per-block snapshot that lags
// the H8 by up to ~two blocks (~10.6 ms). A read shortly after a key-on
// would see the voice's PREVIOUS state -- typically DECAY_DONE / full
// attenuation 0xFF, exactly what a finished voice looks like -- and the
// firmware could conclude the new note already ended. The wrapper keeps
// the snapshot and overlays the known post-key-on state (computed here,
// from the audio-side register shadow) until a snapshot that includes
// the key-on arrives.
//
// The formulas below are a literal transcription of the core's keyon()
// + status() functions in external/mu50/src/swp00_0277.cpp (HEAD port):
//   EnvelopeBlock::keyon/status, FilterBlock::keyon/status/info_w,
//   MixerBlock::keyon/status_glo/status_panl/status_panr/pan_w.
// No render state is consulted: keyon() sets every status-relevant
// field from the registers alone, except the filter sweep flag (see
// below), so the five bytes are a pure function of the registers.
//
// Filter sweep assumption: keyon() with the filter-speed bit 7 clear
// leaves m_sweep untouched, so a sweep still in flight from a previous
// key-on on the same voice would report "not done" while this code
// reports "done" (0xc0 bit set). That only affects the filter byte,
// only when a voice is re-keyed mid-sweep with bit 7 clear, and only
// for the <=2 blocks the overlay lives -- the envelope byte (the one
// the firmware's note-end decision reads) is exact. The overlay value
// is static for up to two blocks, so a very fast attack completing
// inside that window is reported as "still attacking" slightly longer
// than on hardware -- errs on the safe side.
#pragma once

#include <cstdint>

namespace libmu50 {

// Per-channel register values the post-key-on status depends on.
// lpf_info is the 16-bit pair (reg 0x20 hi, 0x21 lo); filter_0277 is
// the Route A hybrid flag (SetFilter0277).
struct KeyOnRegs {
    std::uint8_t attack_speed = 0;
    std::uint8_t attack_level = 0;
    std::uint8_t decay_speed = 0;
    std::uint16_t lpf_info = 0x27ff;
    std::uint8_t lpf_speed = 0;
    std::uint8_t glo = 0xff;
    std::uint8_t pan = 0xff;
    bool filter_0277 = false;
};

// Post-key-on status bytes, in snapshot order:
//   out[0] filter, out[1] envelope, out[2] mixer glo,
//   out[3] mixer panl, out[4] mixer panr.
inline void KeyOnStatus(const KeyOnRegs& r, std::uint8_t out[5]) noexcept {
    // Envelope (EnvelopeBlock::keyon + status):
    //   decay_speed bit 7 set -> immediate release: level 0, DECAY.
    //   else attack_speed bit 7 set -> normal attack: level =
    //   attack_level << 4, ATTACK. Else level 0, ATTACK.
    //   status = (mode << 6) | (level >> 6).
    std::uint8_t env;
    if (r.decay_speed & 0x80) {
        env = static_cast<std::uint8_t>((2 << 6) | 0);
    } else if (r.attack_speed & 0x80) {
        env = static_cast<std::uint8_t>((0 << 6) | (r.attack_level >> 2));
    } else {
        env = 0;
    }
    out[1] = env;

    // Filter (FilterBlock::info_w + keyon + status):
    //   k_target = level | 0x800 (level = info & 0x7ff), with the
    //   0277 cap at 0x7c0 when the resonance bits are below 4.
    //   keyon with speed bit 7 set: HEAD starts the cutoff at 0x800
    //   and sweeps up (not done); 0277 puts it straight to target
    //   (still sweeping, hidden timer). Without bit 7 the cutoff
    //   goes straight to target and the sweep is done (see the
    //   sweep assumption in the header comment).
    //   status = (done ? 0xc0 : 0x00) | ((k >> 6) ^ 0x3f).
    std::uint16_t level = static_cast<std::uint16_t>(r.lpf_info & 0x7ff);
    if (r.filter_0277 && (r.lpf_info >> 11) < 4 && level > 0x7c0)
        level = 0x7c0;
    const std::uint16_t k_target =
        level ? static_cast<std::uint16_t>(level | 0x800) : 0;
    std::uint16_t k;
    bool sweep_done;
    if (r.lpf_speed & 0x80) {
        k = r.filter_0277 ? k_target : 0x800;
        sweep_done = false;
    } else {
        k = k_target;
        sweep_done = true;
    }
    out[0] = static_cast<std::uint8_t>(
        (sweep_done ? 0xc0 : 0x00) | ((k >> 6) ^ 0x3f));

    // Mixer (MixerBlock::keyon + status_*): keyon snaps the current
    // glo/panl/panr to their targets, so all three report done.
    //   glo: tglo = glo << 4, status = 0xc0 | (tglo >> 6).
    out[2] = static_cast<std::uint8_t>(0xc0 | (r.glo >> 2));
    //   pan: tpanl = (pan << 2) & 0x3c0, tpanr = (pan << 6) & 0x3c0,
    //   either 0x3c0 maps to 0xfff; status = 0xc0 | (t >> 6).
    std::uint16_t tpanl =
        static_cast<std::uint16_t>((r.pan << 2) & 0x3c0);
    if (tpanl == 0x3c0)
        tpanl = 0xfff;
    std::uint16_t tpanr =
        static_cast<std::uint16_t>((r.pan << 6) & 0x3c0);
    if (tpanr == 0x3c0)
        tpanr = 0xfff;
    out[3] = static_cast<std::uint8_t>(0xc0 | (tpanl >> 6));
    out[4] = static_cast<std::uint8_t>(0xc0 | (tpanr >> 6));
}

// Envelope status with the mode forced to DECAY, level bits kept:
// what a decay-speed write with bit 7 set (EnvelopeBlock::
// decay_speed_w) does to the mode, and what a release after a key-on
// looks like before the snapshot catches up.
inline std::uint8_t DecayEnvelopeStatus(std::uint8_t env) noexcept {
    return static_cast<std::uint8_t>(0x80 | (env & 0x3f));
}

} // namespace libmu50
