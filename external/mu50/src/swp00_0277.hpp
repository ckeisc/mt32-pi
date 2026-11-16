// license:BSD-3-Clause
// Ported from MAME HEAD src/devices/sound/swp00.cpp (Olivier Galibert),
// synthesis overhaul b75872c (2025-11-14) plus the 84fde62 (delay-attack /
// interpolation-step) and 40dd6fc (envelope status/active) corrections.
// This is a faithful standalone translation of swp00_device's synthesis
// blocks (streaming / envelope / filter / LFO / mixer) and the per-sample
// voice loop of sound_stream_update's voice section. The MEG tail is NOT
// here (see swp00_meg.hpp, which ports the same update function's tail).
// MAME device/stream/ROM-interface machinery is replaced by direct ROM
// buffers and a per-sample loop.
//
// Differences from MAME HEAD, by design:
// - Register writes apply at render-call boundaries, not at sample
//   boundaries: Synth::render calls precompute_voices() once (now a
//   no-op kept for the shell's call site) and render_one() once per
//   frame, with no register writes in between. MAME's require_sync() /
//   STREAM_SYNCHRONOUS semantics (apply a write at the exact sample it
//   lands on) are NOT implemented.
// - The voice buses are returned unshifted (HEAD scale): HEAD's mixer
//   (volume_apply over the filter's s16<<6 domain) produces buses 64x
//   the scale of 0.277's fpapply buses, and MAME HEAD compensates with
//   a >>6 at its final output (put_int_clamp dry>>6). The MEG runs on
//   these full-scale buses, exactly as in HEAD, so its distortion
//   stage's saturate() (variation variant 0x80) engages at the right
//   level; shifting >>6 before the MEG (as the 0.277 port did) would
//   run it 64x cold and never saturate. synth.cpp applies HEAD's >>6
//   (put_int_clamp, 18-bit ADC) to the MEG dry output, then >>2 to
//   16-bit PCM.
// - The old 0.277 helpers (istep/fpstep/fpadd/fpsub/fpapply/lpffpapply)
//   and tables (attack_linear_step/decay_linear_step) are gone: HEAD
//   removed them, and the new blocks do not use them. interpolation_step
//   is the only shared helper.
// - No state-save (device_t/save_item) and no describe() debug output.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <tuple>
#include <utility>

namespace libmu50::detail {

struct VoiceMix {
    std::int32_t dry_l = 0, dry_r = 0, reverb = 0;
    std::int32_t chorus_l = 0, chorus_r = 0, variation_l = 0, variation_r = 0;
};

class Swp0277Core final {
public:
    static constexpr std::size_t kChannels = 32;
    void reset() noexcept;

    // Channel register writes (HEAD hardware names; the Synth shell
    // handles address decoding and assembles 16-bit values). The
    // setters mirror the corresponding block *_w methods in HEAD.
    void set_phase(int chan, std::uint16_t v) noexcept;
    void set_start(int chan, std::uint16_t v) noexcept;
    void set_loop(int chan, std::uint16_t v) noexcept;
    void set_address(int chan, std::uint32_t v) noexcept;
    void set_pitch(int chan, std::uint16_t v) noexcept;
    void set_format(int chan, std::uint8_t v) noexcept;
    void set_lpf_info(int chan, std::uint16_t v) noexcept;
    void set_lpf_speed(int chan, std::uint8_t v) noexcept { filter_[chan].speed_w(v); }
    void set_lfo_lamod(int chan, std::uint8_t v) noexcept { lfo_[chan].lamod_w(v); }
    void set_lfo_speed(int chan, std::uint8_t v) noexcept { lfo_[chan].speed_w(v); }
    void set_lfo_fmod(int chan, std::uint8_t v) noexcept { lfo_[chan].fmod_w(v); }
    void set_attack_speed(int chan, std::uint8_t v) noexcept { envelope_[chan].attack_speed_w(v); }
    void set_attack_level(int chan, std::uint8_t v) noexcept { envelope_[chan].attack_level_w(v); }
    void set_decay_speed(int chan, std::uint8_t v) noexcept { envelope_[chan].decay_speed_w(v); }
    void set_decay_level(int chan, std::uint8_t v) noexcept { envelope_[chan].decay_level_w(v); }
    void set_reverb_level(int chan, std::uint8_t v) noexcept { mixer_[chan].rev_w(v); }
    void set_dry_level(int chan, std::uint8_t v) noexcept { mixer_[chan].dry_w(v); }
    void set_chorus_level(int chan, std::uint8_t v) noexcept { mixer_[chan].cho_w(v); }
    void set_variation_level(int chan, std::uint8_t v) noexcept { mixer_[chan].var_w(v); }
    void set_glo_level(int chan, std::uint8_t v) noexcept { mixer_[chan].glo_w(v); }
    void set_pan(int chan, std::uint8_t v) noexcept { mixer_[chan].pan_w(v); }
    void keyon(int chan) noexcept;

    // Stored register values (readback).
    std::uint16_t phase(int chan) const noexcept { return streaming_[chan].m_phase; }
    std::uint16_t start(int chan) const noexcept { return streaming_[chan].m_start; }
    std::uint16_t loop(int chan) const noexcept { return streaming_[chan].m_loop; }
    std::uint32_t address(int chan) const noexcept { return streaming_[chan].m_address; }
    std::uint16_t pitch(int chan) const noexcept { return streaming_[chan].m_pitch; }
    std::uint8_t format(int chan) const noexcept { return streaming_[chan].m_format; }
    std::uint16_t lpf_info(int chan) const noexcept { return filter_[chan].m_info; }
    std::uint8_t lpf_speed(int chan) const noexcept { return filter_[chan].m_speed; }
    std::uint8_t lfo_lamod(int chan) const noexcept { return lfo_[chan].m_lamod; }
    std::uint8_t lfo_speed(int chan) const noexcept { return lfo_[chan].m_speed; }
    std::uint8_t lfo_fmod(int chan) const noexcept { return lfo_[chan].m_fmod; }
    std::uint8_t attack_speed(int chan) const noexcept { return envelope_[chan].m_attack_speed; }
    std::uint8_t attack_level(int chan) const noexcept { return envelope_[chan].m_attack_level; }
    std::uint8_t decay_speed(int chan) const noexcept { return envelope_[chan].m_decay_speed; }
    std::uint8_t decay_level(int chan) const noexcept { return envelope_[chan].m_decay_level; }
    std::uint8_t reverb_level(int chan) const noexcept { return mixer_[chan].m_rev; }
    std::uint8_t dry_level(int chan) const noexcept { return mixer_[chan].m_dry; }
    std::uint8_t chorus_level(int chan) const noexcept { return mixer_[chan].m_cho; }
    std::uint8_t variation_level(int chan) const noexcept { return mixer_[chan].m_var; }
    std::uint8_t glo_level(int chan) const noexcept { return mixer_[chan].m_glo; }
    std::uint8_t pan(int chan) const noexcept { return mixer_[chan].m_pan; }
    // Voice-active predicate (HEAD envelope_block::active, 40dd6fc):
    // a voice counts as active while its envelope has not fully decayed.
    bool active(int chan) const noexcept { return envelope_[chan].active(); }
    // Envelope status byte (HEAD envelope_block::status), diagnostics.
    std::uint8_t envelope_status(int chan) const noexcept { return envelope_[chan].status(); }
    // Voice-state accessors for HEAD swp00_device::state_r (register
    // 0x01 readback): filter / mixer status bytes and the device
    // sample counter.
    std::uint8_t filter_status(int chan) const noexcept { return filter_[chan].status(); }
    std::uint8_t mixer_status_glo(int chan) const noexcept { return mixer_[chan].status_glo(); }
    std::uint8_t mixer_status_panl(int chan) const noexcept { return mixer_[chan].status_panl(); }
    std::uint8_t mixer_status_panr(int chan) const noexcept { return mixer_[chan].status_panr(); }
    std::uint32_t sample_counter() const noexcept { return sample_counter_; }
    // Read-only streaming state accessors (tests / diagnostics only).
    std::int32_t stream_pos(int chan) const noexcept { return streaming_[chan].m_pos; }
    std::int32_t stream_pos_dec(int chan) const noexcept { return streaming_[chan].m_pos_dec; }
    std::int16_t dpcm_s0(int chan) const noexcept { return streaming_[chan].m_dpcm_s0; }
    std::int16_t dpcm_s1(int chan) const noexcept { return streaming_[chan].m_dpcm_s1; }
    std::uint32_t dpcm_pos(int chan) const noexcept { return streaming_[chan].m_dpcm_pos; }
    std::int32_t dpcm_delta(int chan) const noexcept { return streaming_[chan].m_dpcm_delta; }

    // One sample: advance all voices (LFO first, for every channel),
    // accumulate the seven buses, unshifted (HEAD scale). The MEG
    // runs on these full-scale buses; synth.cpp applies HEAD's >>6
    // (put_int_clamp) to the MEG dry output before the >>2 to PCM.
    VoiceMix render_one(const std::uint8_t* rom, std::size_t rom_size) noexcept;

    // No per-render-call derived constants in the HEAD model: every
    // block reads its registers live. Kept as a no-op so the Synth
    // shell's call site (and its "registers frozen for this call"
    // invariant) is unchanged.
    void precompute_voices() noexcept {}

    // The dithered/stepped interpolation subblock shared by the
    // envelope, filter and streaming blocks (HEAD
    // swp00_device::interpolation_step, including the 84fde62 fix:
    // speed >= 0x78 returns 0x1f, speed >= 0x58 halves the steps).
    static std::uint16_t interpolation_step(std::uint32_t speed, std::uint32_t sample_counter) noexcept;

private:
    using s32 = std::int32_t;
    using s64 = std::int64_t;
    using u32 = std::uint32_t;
    using u8 = std::uint8_t;
    using u16 = std::uint16_t;
    using s16 = std::int16_t;

    static u8 read_byte(u32 address, const u8* rom, std::size_t rom_size) noexcept;
    static u16 read_word(u32 address, const u8* rom, std::size_t rom_size) noexcept;

    // DPCM expansion table (HEAD streaming_block::dpcm_expand) and the
    // per-scale clamp table (HEAD streaming_block::max_value).
    static const std::array<s16, 256>& dpcm_expand() noexcept;
    static const std::array<s32, 8>& dpcm_max_value() noexcept;

    // --- Streaming block (HEAD streaming_block) ---
    // Sample playback: phase/start/loop/address/pitch/format registers
    // plus position, sub-sample phase, DPCM state and done/last flags.
    struct StreamingBlock {
        u16 m_phase = 0x8000;
        u16 m_start = 0;
        u16 m_loop = 0;
        u32 m_address = 0;
        u16 m_pitch = 0;
        u8 m_format = 0;
        s32 m_pos = 0;
        s32 m_pos_dec = 0;
        s16 m_dpcm_s0 = 0, m_dpcm_s1 = 0;
        u32 m_dpcm_pos = 0;
        s32 m_dpcm_delta = 0;
        bool m_first = false;
        bool m_done = false;
        s16 m_last = 0;

        void clear() noexcept;
        void keyon() noexcept;
        // Returns (sample, trigger_release). trigger_release is true
        // once, when a non-looping sample reaches its end; the caller
        // then triggers the envelope release.
        std::pair<s16, bool> step(const u8* rom, std::size_t rom_size, s32 fmod) noexcept;
        void read_16(const u8* rom, std::size_t rom_size, s16& val0, s16& val1) noexcept;
        void read_12(const u8* rom, std::size_t rom_size, s16& val0, s16& val1) noexcept;
        void read_8(const u8* rom, std::size_t rom_size, s16& val0, s16& val1) noexcept;
        void read_8c(const u8* rom, std::size_t rom_size, s16& val0, s16& val1) noexcept;
        void dpcm_step(u8 input) noexcept;
    };

    // --- Envelope block (HEAD envelope_block) ---
    // Attack/decay with modes ATTACK/DECAY/DECAY_DONE and a 12-bit
    // attenuation level (0 = loudest, 0xfff = silent).
    struct EnvelopeBlock {
        enum : u8 { ATTACK = 0, DECAY = 2, DECAY_DONE = 3 };
        u8 m_attack_speed = 0;
        u8 m_attack_level = 0;
        u8 m_decay_speed = 0;
        u8 m_decay_level = 0;
        s32 m_envelope_level = 0xfff;
        u8 m_envelope_mode = DECAY_DONE;

        void clear() noexcept;
        void keyon() noexcept;
        u8 status() const noexcept;
        bool active() const noexcept;
        u16 step(u32 sample_counter) noexcept;
        void trigger_release() noexcept { m_envelope_mode = DECAY; }
        void attack_speed_w(u8 data) noexcept { m_attack_speed = data; }
        void attack_level_w(u8 data) noexcept { m_attack_level = data; }
        void decay_speed_w(u8 data) noexcept {
            m_decay_speed = data;
            if (data & 0x80) m_envelope_mode = DECAY;
        }
        void decay_level_w(u8 data) noexcept { m_decay_level = data; }
    };

    // --- Filter block (HEAD filter_block) ---
    // Chamberlin-configuration LPF. m_k/m_k_target are the 12-bit
    // cutoff (top bit is an added 1); m_q is the resonance.
    struct FilterBlock {
        enum : u8 { SWEEP_NONE = 0, SWEEP_UP = 1, SWEEP_DONE = 2 };
        s32 m_q = 0x80, m_b = 0, m_l = 0;
        u16 m_k = 0xfff, m_k_target = 0x7ff;
        u16 m_info = 0x27ff;
        u8 m_speed = 0, m_sweep = SWEEP_NONE;

        void clear() noexcept;
        void keyon() noexcept;
        s32 step(s16 input, s32 lmod, u32 sample_counter) noexcept;
        u8 status() const noexcept;
        void info_w(u16 data) noexcept;
        void speed_w(u8 data) noexcept { m_speed = data; }
        u8 speed_r() const noexcept { return m_speed; }
    };

    // --- LFO block (HEAD lfo_block) ---
    // 22-bit counter, optionally triangle-shaped. Returns
    // (amod, fmod, lmod): tremolo amplitude, pitch modulation and
    // filter-cutoff modulation. keyon() is intentionally a no-op in
    // HEAD: the LFO is free-running and is NOT reset by keyon.
    struct LfoBlock {
        u32 m_counter = 0;
        u8 m_speed = 0x80, m_lamod = 0, m_fmod = 0;

        void clear() noexcept;
        void keyon() noexcept {}
        std::tuple<u32, s32, s32> step() noexcept;
        void lamod_w(u8 data) noexcept { m_lamod = data; }
        void speed_w(u8 data) noexcept { m_speed = data; }
        void fmod_w(u8 data) noexcept { m_fmod = data; }
    };

    // --- Mixer block (HEAD mixer_block) ---
    // Per-voice volumes. The 8-bit levels apply immediately; glo and
    // pan left/right interpolate by 1 per sample toward their targets.
    struct MixerBlock {
        u16 m_cglo = 0xfff, m_cpanl = 0xfff, m_cpanr = 0xfff;
        u16 m_tglo = 0xfff, m_tpanl = 0xfff, m_tpanr = 0xfff;
        u8 m_glo = 0xff, m_pan = 0xff, m_dry = 0xff, m_rev = 0xff, m_cho = 0xff, m_var = 0xff;

        void clear() noexcept;
        void keyon() noexcept;
        u8 status_glo() const noexcept;
        u8 status_panl() const noexcept;
        u8 status_panr() const noexcept;
        void step(s32 sample, u16 envelope, u16 tremolo,
                  s32& dry_l, s32& dry_r, s32& rev,
                  s32& cho_l, s32& cho_r, s32& var_l, s32& var_r) noexcept;
        static s32 volume_apply(s32 level, s32 sample) noexcept;
        void glo_w(u8 data) noexcept { m_glo = data; m_tglo = static_cast<u16>(data << 4); }
        void pan_w(u8 data) noexcept;
        void dry_w(u8 data) noexcept { m_dry = data; }
        void rev_w(u8 data) noexcept { m_rev = data; }
        void cho_w(u8 data) noexcept { m_cho = data; }
        void var_w(u8 data) noexcept { m_var = data; }
    };

    std::array<StreamingBlock, kChannels> streaming_{};
    std::array<EnvelopeBlock, kChannels> envelope_{};
    std::array<FilterBlock, kChannels> filter_{};
    std::array<LfoBlock, kChannels> lfo_{};
    std::array<MixerBlock, kChannels> mixer_{};
    // Device-level sample counter, incremented once per render_one()
    // call (after the voice loop), exactly as MAME increments
    // m_sample_counter at the end of each sound_stream_update sample.
    u32 sample_counter_ = 0;
};

} // namespace libmu50::detail
