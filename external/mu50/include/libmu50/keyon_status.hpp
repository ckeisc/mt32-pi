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
// KeyOnStatus is computed by the core's OWN keyon() + status()
// functions (EnvelopeBlock, FilterBlock, MixerBlock in
// external/mu50/src/swp00_0277.cpp), run on a scratch Swp0277Core in
// synth.cpp -- one source of truth, so later core fixes cannot silently
// drift from a hand transcription here. (PR #15 originally transcribed
// those functions by hand; the transcription was verified 0-mismatch
// over 4000 random cases against the core, and the scratch-core
// implementation reproduces it exactly over the same 4000 cases.)
//
// Fresh-state assumption: the scratch core is fresh, so a filter sweep
// still in flight from a previous key-on on the same voice reads as
// "done", exactly as the old transcription assumed -- keyon() with the
// filter-speed bit 7 clear leaves m_sweep untouched, so only the filter
// byte is affected, only when a voice is re-keyed mid-sweep with bit 7
// clear, and only for the <=2 blocks the overlay lives. The envelope
// byte (the one the firmware's note-end decision reads) is exact. The
// overlay value is static for up to two blocks, so a very fast attack
// completing inside that window is reported as "still attacking"
// slightly longer than on hardware -- errs on the safe side.
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
// Defined in synth.cpp: runs the core's own keyon()/status() on a
// scratch Swp0277Core seeded with these registers. No render state is
// consulted: keyon() sets every status-relevant field from the
// registers alone, except the filter sweep flag (see above).
void KeyOnStatus(const KeyOnRegs& r, std::uint8_t out[5]) noexcept;

// Envelope status with the mode forced to DECAY, level bits kept:
// what a decay-speed write with bit 7 set (EnvelopeBlock::
// decay_speed_w) does to the mode, and what a release after a key-on
// looks like before the snapshot catches up.
inline std::uint8_t DecayEnvelopeStatus(std::uint8_t env) noexcept {
    return static_cast<std::uint8_t>(0x80 | (env & 0x3f));
}

} // namespace libmu50
