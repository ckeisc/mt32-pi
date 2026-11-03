// Portions derived from MAME src/devices/sound/swp00.cpp.
// Copyright (C) Olivier Galibert. SPDX-License-Identifier: BSD-3-Clause.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <tuple>
#include <utility>

namespace libmu50::detail {
using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using s16 = std::int16_t;
using s32 = std::int32_t;
using s64 = std::int64_t;

// Shared envelope/filter interpolation cadence, ported from MAME SWP00.
u16 interpolation_step(u32 speed, u32 sample_counter) noexcept;

class StreamingBlock {
public:
    void clear() noexcept;
    void keyon() noexcept;
    std::pair<s16, bool> step(const u8* rom, std::size_t rom_size, s32 fmod) noexcept;
    void set_phase(u16 v) noexcept { phase_ = v; }
    void set_start(u16 v) noexcept { start_ = v; }
    void set_loop(u16 v) noexcept { loop_ = v; }
    void set_address(u32 v) noexcept { address_ = v & 0xffffff; }
    void set_pitch(u16 v) noexcept { pitch_ = v; }
    void set_format(u8 v) noexcept { format_ = v; }
    u32 address() const noexcept { return address_; }
private:
    u8 byte_at(const u8* rom, std::size_t size, s64 address) const noexcept;
    u16 word_at(const u8* rom, std::size_t size, s64 address) const noexcept;
    void read_16(const u8*, std::size_t, s16&, s16&) const noexcept;
    void read_12(const u8*, std::size_t, s16&, s16&) const noexcept;
    void read_8(const u8*, std::size_t, s16&, s16&) const noexcept;
    void read_8c(const u8*, std::size_t, s16&, s16&) noexcept;
    void dpcm_step(u8) noexcept;

    u16 phase_ = 0x8000, start_ = 0, loop_ = 0, pitch_ = 0;
    u32 address_ = 0;
    u8 format_ = 0;
    s32 pos_ = 0, pos_dec_ = 0;
    s16 dpcm_s0_ = 0, dpcm_s1_ = 0;
    s32 dpcm_pos_ = 0, dpcm_delta_ = 0;
    bool first_ = false, done_ = false;
    s16 last_ = 0;
};

class EnvelopeBlock {
public:
    void clear() noexcept;
    void keyon() noexcept;
    bool active() const noexcept;
    u16 step(u32 sample_counter) noexcept;
    void trigger_release() noexcept;
    void set_attack_speed(u8 v) noexcept { attack_speed_ = v; }
    void set_attack_level(u8 v) noexcept { attack_level_ = v; }
    void set_decay_speed(u8 v) noexcept;
    void set_decay_level(u8 v) noexcept { decay_level_ = v; }
private:
    enum : u8 { ATTACK = 0, DECAY = 2, DECAY_DONE = 3 };
    u8 attack_speed_ = 0, attack_level_ = 0, decay_speed_ = 0, decay_level_ = 0;
    s32 level_ = 0xfff;
    u8 mode_ = DECAY_DONE;
};

class FilterBlock {
public:
    void clear() noexcept;
    void keyon() noexcept;
    s32 step(s16 input, s32 lmod, u32 sample_counter) noexcept;
    void set_info_low(u8 v) noexcept;
    void set_info_high(u8 v) noexcept;
    void set_speed(u8 v) noexcept { speed_ = v; }
private:
    enum : u8 { SWEEP_NONE, SWEEP_UP, SWEEP_DONE };
    s32 q_ = 0, b_ = 0, l_ = 0;
    u16 k_ = 0xfff, k_target_ = 0x7ff, info_ = 0x27ff;
    u8 speed_ = 0, sweep_ = SWEEP_NONE;
};

class LfoBlock {
public:
    void clear() noexcept;
    void keyon() noexcept;
    std::tuple<u32, s32, s32> step() noexcept;
    void set_lamod(u8 v) noexcept { lamod_ = v; }
    void set_speed(u8 v) noexcept { speed_ = v; }
    void set_fmod(u8 v) noexcept { fmod_ = v; }
private:
    u32 counter_ = 0;
    u8 speed_ = 0x80, lamod_ = 0, fmod_ = 0;
};

class MixerBlock {
public:
    void clear() noexcept;
    void keyon() noexcept;
    void step(s32 sample, u16 envelope, u16 tremolo,
              s32& dry_l, s32& dry_r, s32& rev,
              s32& cho_l, s32& cho_r, s32& var_l, s32& var_r) noexcept;
    void set_global(u8 v) noexcept;
    void set_pan(u8 v) noexcept;
    void set_dry(u8 v) noexcept { dry_ = v; }
    void set_reverb(u8 v) noexcept { rev_ = v; }
    void set_chorus(u8 v) noexcept { cho_ = v; }
    void set_variation(u8 v) noexcept { var_ = v; }
private:
    static s32 volume_apply(s32 level, s32 sample) noexcept;
    u16 current_global_ = 0xfff, current_pan_l_ = 0xfff, current_pan_r_ = 0xfff;
    u16 target_global_ = 0xfff, target_pan_l_ = 0xfff, target_pan_r_ = 0xfff;
    u8 global_ = 0xff, pan_ = 0xff, dry_ = 0xff, rev_ = 0xff, cho_ = 0xff, var_ = 0xff;
};

struct VoiceMix {
    s32 dry_l = 0, dry_r = 0, reverb = 0;
    s32 chorus_l = 0, chorus_r = 0, variation_l = 0, variation_r = 0;
};

// One AWM2 voice pipeline in the same order as SWP00 sound_stream_update.
class VoiceChannel {
public:
    void clear() noexcept;
    void keyon() noexcept;
    void set_phase(u16 v) noexcept { stream_.set_phase(v); }
    void set_start(u16 v) noexcept { stream_.set_start(v); }
    void set_loop(u16 v) noexcept { stream_.set_loop(v); }
    void set_address(u32 v) noexcept { stream_.set_address(v); }
    void set_pitch(u16 v) noexcept { stream_.set_pitch(v); }
    void set_format(u8 v) noexcept { stream_.set_format(v); }
    void set_attack_speed(u8 v) noexcept { envelope_.set_attack_speed(v); }
    void set_attack_level(u8 v) noexcept { envelope_.set_attack_level(v); }
    void set_decay_speed(u8 v) noexcept { envelope_.set_decay_speed(v); }
    void set_decay_level(u8 v) noexcept { envelope_.set_decay_level(v); }
    void set_filter_info_low(u8 v) noexcept { filter_.set_info_low(v); }
    void set_filter_info_high(u8 v) noexcept { filter_.set_info_high(v); }
    void set_filter_speed(u8 v) noexcept { filter_.set_speed(v); }
    void set_lfo_lamod(u8 v) noexcept { lfo_.set_lamod(v); }
    void set_lfo_speed(u8 v) noexcept { lfo_.set_speed(v); }
    void set_lfo_fmod(u8 v) noexcept { lfo_.set_fmod(v); }
    void set_global(u8 v) noexcept { mixer_.set_global(v); }
    void set_pan(u8 v) noexcept { mixer_.set_pan(v); }
    void set_dry(u8 v) noexcept { mixer_.set_dry(v); }
    void set_reverb(u8 v) noexcept { mixer_.set_reverb(v); }
    void set_chorus(u8 v) noexcept { mixer_.set_chorus(v); }
    void set_variation(u8 v) noexcept { mixer_.set_variation(v); }
    void render_one(const u8* rom, std::size_t rom_size, u32 sample_counter, VoiceMix& mix) noexcept;
private:
    StreamingBlock stream_;
    EnvelopeBlock envelope_;
    FilterBlock filter_;
    LfoBlock lfo_;
    MixerBlock mixer_;
};


class Awm2Core {
public:
    static constexpr std::size_t voice_count = 32;
    void clear() noexcept;
    VoiceChannel& voice(std::size_t index) noexcept { return voices_[index % voice_count]; }
    const VoiceChannel& voice(std::size_t index) const noexcept { return voices_[index % voice_count]; }
    VoiceMix render_one(const u8* rom, std::size_t rom_size, u32 sample_counter) noexcept;
private:
    std::array<VoiceChannel, voice_count> voices_{};
};

} // namespace libmu50::detail
