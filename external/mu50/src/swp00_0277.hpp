// license:BSD-3-Clause
// Ported from MAME 0.277 src/devices/sound/swp00.cpp (Olivier Galibert).
// This is a faithful standalone translation of swp00_device's voice
// pipeline: flat 32-channel arrays, the same helper functions
// (istep/fpstep/fpadd/fpsub/fpapply/lpffpapply), the same device_start
// tables, and the same per-sample voice loop as sound_stream_update's
// voice section. The MEG tail is NOT here (see swp00_meg.hpp, which ports
// the same update function's tail). MAME device/stream/ROM-interface
// machinery is replaced by direct ROM buffers and a per-sample loop.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace libmu50::detail {

struct VoiceMix {
    std::int32_t dry_l = 0, dry_r = 0, reverb = 0;
    std::int32_t chorus_l = 0, chorus_r = 0, variation_l = 0, variation_r = 0;
};

class Swp0277Core final {
public:
    static constexpr std::size_t kChannels = 32;
    void reset() noexcept;

    // Channel register writes (the Synth shell handles address decoding).
    void set_lpf_info(int chan, std::uint16_t v) noexcept;
    void set_lpf_speed(int chan, std::uint8_t v) noexcept { lpf_speed_[chan] = v; }
    void set_lfo_famod_depth(int chan, std::uint8_t v) noexcept { lfo_famod_depth_[chan] = v; }
    void set_lfo_step(int chan, std::uint8_t v) noexcept { lfo_step_[chan] = v; }
    void set_lfo_pmod_depth(int chan, std::uint8_t v) noexcept { lfo_pmod_depth_[chan] = v; }
    void set_attack_speed(int chan, std::uint8_t v) noexcept { attack_speed_[chan] = v; }
    void set_attack_level(int chan, std::uint8_t v) noexcept { attack_level_[chan] = v; }
    void set_decay_speed(int chan, std::uint8_t v) noexcept {
        decay_speed_[chan] = v;
        if (v & 0x80) decay_[chan] = true; // MAME: decay_speed_w side effect
    }
    void set_decay_level(int chan, std::uint8_t v) noexcept { decay_level_[chan] = v; }
    void set_reverb_level(int chan, std::uint8_t v) noexcept { rev_level_[chan] = v; }
    void set_dry_level(int chan, std::uint8_t v) noexcept { dry_level_[chan] = v; }
    void set_chorus_level(int chan, std::uint8_t v) noexcept { cho_level_[chan] = v; }
    void set_variation_level(int chan, std::uint8_t v) noexcept { var_level_[chan] = v; }
    void set_glo_level(int chan, std::uint8_t v) noexcept { glo_level_[chan] = v; }
    void set_panning(int chan, std::uint8_t v) noexcept { panning_[chan] = v; }
    void set_format(int chan, std::uint8_t v) noexcept { format_[chan] = v; }
    void set_address(int chan, std::uint32_t v) noexcept { address_[chan] = v; }
    void set_pitch(int chan, std::uint16_t v) noexcept { pitch_[chan] = v; }
    void set_start(int chan, std::uint16_t v) noexcept { start_[chan] = v; }
    void set_end(int chan, std::uint16_t v) noexcept { end_[chan] = v; }
    void keyon(int chan) noexcept;

    // Stored register values (readback).
    std::uint16_t lpf_info(int chan) const noexcept { return lpf_info_[chan]; }
    std::uint8_t lpf_speed(int chan) const noexcept { return lpf_speed_[chan]; }
    std::uint8_t lfo_famod_depth(int chan) const noexcept { return lfo_famod_depth_[chan]; }
    std::uint8_t lfo_step(int chan) const noexcept { return lfo_step_[chan]; }
    std::uint8_t lfo_pmod_depth(int chan) const noexcept { return lfo_pmod_depth_[chan]; }
    std::uint8_t attack_speed(int chan) const noexcept { return attack_speed_[chan]; }
    std::uint8_t attack_level(int chan) const noexcept { return attack_level_[chan]; }
    std::uint8_t decay_speed(int chan) const noexcept { return decay_speed_[chan]; }
    std::uint8_t decay_level(int chan) const noexcept { return decay_level_[chan]; }
    std::uint8_t reverb_level(int chan) const noexcept { return rev_level_[chan]; }
    std::uint8_t dry_level(int chan) const noexcept { return dry_level_[chan]; }
    std::uint8_t chorus_level(int chan) const noexcept { return cho_level_[chan]; }
    std::uint8_t variation_level(int chan) const noexcept { return var_level_[chan]; }
    std::uint8_t glo_level(int chan) const noexcept { return glo_level_[chan]; }
    std::uint8_t panning(int chan) const noexcept { return panning_[chan]; }
    std::uint8_t format(int chan) const noexcept { return format_[chan]; }
    std::uint32_t address(int chan) const noexcept { return address_[chan]; }
    std::uint16_t pitch(int chan) const noexcept { return pitch_[chan]; }
    std::uint16_t start(int chan) const noexcept { return start_[chan]; }
    std::uint16_t end(int chan) const noexcept { return end_[chan]; }
    bool active(int chan) const noexcept { return active_[chan]; }
    // Read-only state accessors (tests / diagnostics only).
    std::int32_t sample_pos(int chan) const noexcept { return sample_pos_[chan]; }
    std::uint32_t dpcm_address(int chan) const noexcept { return dpcm_address_[chan]; }
    std::int32_t dpcm_sum(int chan) const noexcept { return dpcm_sum_[chan]; }
    std::int16_t dpcm_current(int chan) const noexcept { return dpcm_current_[chan]; }
    std::int16_t dpcm_next(int chan) const noexcept { return dpcm_next_[chan]; }

    // One sample: advance all active voices, accumulate the seven buses
    // (already >>8, exactly as MAME feeds them to the MEG tail).
    VoiceMix render_one(const std::uint8_t* rom, std::size_t rom_size) noexcept;

    // Per-voice DERIVED constants, recomputed once per Synth::render
    // call (see precompute_voices). All fields derive ONLY from
    // voice registers, which are frozen across a whole render call:
    // Synth::render advances the H8 first (all W writes land), then
    // renders frames with no register writes in between, so these
    // values are valid for every frame in the call. Stateful values
    // (sample_pos, envelope level, LFO phase, glo/pan currents, LPF
    // state, DPCM state, active/decay flags) are NOT here; they stay
    // per-sample in render_one. No invalidation logic is needed
    // within a call; correctness = correct derivation, proven by
    // the byte-identical PCM gate.
    struct VoicePrecomp {
        std::uint8_t format_case = 0;   // format_ >> 6
        std::uint8_t dpcm_offset = 0;   // kDpcmOffset[format_ & 3]
        std::uint8_t dpcm_scale = 0;    // (format_ >> 2) & 7
        const std::uint32_t* lfo_centered = nullptr; // Tri if lfo_step & 0x40 else Saw
        const std::uint32_t* lfo_offset = nullptr;   // Tri if lfo_step & 0x40 else Saw
        std::int32_t lfo_fa_shift = 1;  // lfo_step & 0x40 ? 2 : 1
        std::int32_t tremolo_shift = 2; // lfo_step & 0x40 ? 3 : 2
        std::int32_t pmod_shift = 19;   // lfo_step & 0x40 ? 18 : 19
        std::uint32_t lfo_step_value = 0; // gstep[0x20 + (lfo_step_ & 0x3f)]
        std::uint32_t sample_increment = 0; // from pitch_ (see render_one)
        std::int32_t dry_const = 0;     // dry_level_ << 20
        std::int32_t rev_const = 0;     // rev_level_ << 20
        std::int32_t cho_const = 0;     // cho_level_ << 20
        std::int32_t var_const = 0;     // var_level_ << 20
        std::int32_t famod_hi = 0;      // lfo_famod_depth_ >> 5
        std::int32_t famod_lo = 0;      // lfo_famod_depth_ & 0x1f
        std::int32_t lfo_pmod_depth = 0; // lfo_pmod_depth_ (copy)
        std::int32_t lpf_step_hi = 0;   // gstep[lpf_speed_ & 0x7f] >> 1
        std::int32_t lpf_step_lo = 0;   // gstep[lpf_speed_] >> 1 (else branch: bit 7 = 0)
        bool lpf_hi = false;            // lpf_speed_ & 0x80
        std::int32_t lpf_target = 0;    // lpf_target_value_ (write-time derived)
        std::int32_t lpf_feedback = 0;  // lpf_feedback_ (write-time derived)
        bool decay_exp = false;         // (decay_speed_ & 0x60) == 0x60
        std::int32_t decay_step_exp = 0; // dstep[decay_speed_ & 0x1f]
        std::int32_t decay_step_lin = 0; // gstep[decay_speed_ & 0x7f] << 1 (MASKED: decay fix)
        std::int32_t decay_target = 0;  // decay_level_ << 20
        std::int32_t attack_step = 0;   // astep[attack_speed_ & 0x7f]
        std::int32_t attack_step_lin = 0; // gstep[attack_speed_] << 1 (else branch: bit 7 = 0)
        bool attack_hi = false;         // attack_speed_ & 0x80
        std::int32_t glo_target = 0;    // glo_level_ << 4
        std::int32_t pan_l_target = 0;  // kPanmap[(panning_ >> 4) & 0xf]
        std::int32_t pan_r_target = 0;  // kPanmap[panning_ & 0xf]
        std::int32_t end = 0;           // end_ (copy)
        std::uint32_t address = 0;      // address_ (copy)
    };

    // Recompute precomp_ for all 32 channels from current registers.
    // Called once at the start of every Synth::render call, before
    // any render_one call in that call.
    void precompute_voices() noexcept;

    // Helpers exposed for tests (same names/semantics as MAME's statics).
    static bool istep(std::int32_t& value, std::int32_t limit, std::int32_t step) noexcept;
    static bool fpstep(std::int32_t& value, std::int32_t limit, std::int32_t step) noexcept;
    static std::int32_t fpadd(std::int32_t v1, std::int32_t v2) noexcept;
    static std::int32_t fpsub(std::int32_t v1, std::int32_t v2) noexcept;
    static std::int32_t fpapply(std::int32_t value, std::int32_t sample) noexcept;
    static std::int32_t lpffpapply(std::int32_t value, std::int32_t sample) noexcept;

private:
    using s32 = std::int32_t;
    using s64 = std::int64_t;
    using u32 = std::uint32_t;
    using u8 = std::uint8_t;
    using u16 = std::uint16_t;
    using s16 = std::int16_t;

    u8 read_byte(u32 address, const u8* rom, std::size_t rom_size) const noexcept;
    u16 read_word(u32 address, const u8* rom, std::size_t rom_size) const noexcept;

    // device_start tables (computed once, shared).
    static const std::array<u32, 128>& global_step() noexcept;
    static const std::array<s16, 256>& dpcm_table() noexcept;
    static const std::array<s32, 0x80>& attack_linear_step() noexcept;
    static const std::array<s32, 0x20>& decay_linear_step() noexcept;

    // Channel registers.
    std::array<u16, kChannels> lpf_info_{};
    std::array<u8, kChannels> lpf_speed_{};
    std::array<u8, kChannels> lfo_famod_depth_{};
    std::array<u8, kChannels> rev_level_{};
    std::array<u8, kChannels> dry_level_{};
    std::array<u8, kChannels> cho_level_{};
    std::array<u8, kChannels> var_level_{};
    std::array<u8, kChannels> glo_level_{};
    std::array<u8, kChannels> panning_{};
    std::array<u8, kChannels> attack_speed_{};
    std::array<u8, kChannels> attack_level_{};
    std::array<u8, kChannels> decay_speed_{};
    std::array<u8, kChannels> decay_level_{};
    std::array<u16, kChannels> pitch_{};
    std::array<u16, kChannels> start_{};
    std::array<u16, kChannels> end_{};
    std::array<u8, kChannels> format_{};
    std::array<u32, kChannels> address_{};
    std::array<u8, kChannels> lfo_step_{};
    std::array<u8, kChannels> lfo_pmod_depth_{};
    // Channel state.
    std::array<u32, kChannels> lfo_phase_{};
    std::array<s32, kChannels> sample_pos_{};
    std::array<s32, kChannels> envelope_level_{};
    std::array<s32, kChannels> glo_level_cur_{};
    std::array<s32, kChannels> pan_l_{};
    std::array<s32, kChannels> pan_r_{};
    std::array<s32, kChannels> lpf_feedback_{};
    std::array<s32, kChannels> lpf_target_value_{};
    std::array<s32, kChannels> lpf_value_{};
    std::array<s32, kChannels> lpf_timer_{};
    std::array<s32, kChannels> lpf_ha_{};
    std::array<s32, kChannels> lpf_hb_{};
    std::array<bool, kChannels> active_{};
    std::array<bool, kChannels> decay_{};
    std::array<bool, kChannels> decay_done_{};
    std::array<bool, kChannels> lpf_done_{};
    std::array<s16, kChannels> dpcm_current_{};
    std::array<s16, kChannels> dpcm_next_{};
    std::array<u32, kChannels> dpcm_address_{};
    std::array<s32, kChannels> dpcm_sum_{};
    // Per-render-call derived constants (see precompute_voices).
    std::array<VoicePrecomp, kChannels> precomp_{};
};

} // namespace libmu50::detail
