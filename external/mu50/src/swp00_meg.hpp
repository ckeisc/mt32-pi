// license:BSD-3-Clause
// Adapted from MAME src/devices/sound/swp00.cpp (Olivier Galibert).
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include "swp00_0277.hpp"

namespace libmu50::detail {
class MegState final {
public:
    static constexpr std::size_t kReverbWords=0x20000, kChorusWords=0x8000, kVariationWords=0x20000;
    void reset() noexcept;
    void write_offset(std::uint16_t,std::uint8_t) noexcept;
    std::uint8_t read_offset(std::uint16_t) const noexcept;
    void write_constant(std::uint16_t,std::uint8_t) noexcept;
    std::uint8_t read_constant(std::uint16_t) const noexcept;
    void set_control(std::uint8_t v) noexcept { control_=v; }
    std::uint8_t control() const noexcept { return control_; }
    // Ablation toggles (diagnosis only, default off). When set, the
    // named effect's output contribution is zeroed (networks still
    // advance; cross-feeds from a disabled effect are also zeroed).
    void set_ablation(bool nr, bool nc, bool nv) noexcept { no_reverb_=nr; no_chorus_=nc; no_variation_=nv; }
    std::int32_t constant_signed(std::size_t i) const noexcept { return i<constants_.size() ? ((constants_[i]&0x3ff)>0x200 ? (constants_[i]&0x3ff)-0x400 : constants_[i]&0x3ff) : 0; }
    std::int32_t mul7(std::int32_t v,std::int32_t m) const noexcept { return s32((s64(v)*m)>>7); }
    std::int32_t mul9(std::int32_t v,std::int32_t m) const noexcept { return s32((s64(v)*m)>>9); }
    // Process one AWM2 seven-bus frame through variation, chorus and reverb.
    // Returns the full-domain dry stereo samples (MAME 0.277's put_int
    // domain: the host clamps these directly to int16). This differs
    // from the HEAD port, whose MEG returned dry >> 6 (18-bit ADC domain)
    // to match HEAD's put_int_clamp staging.
    std::array<std::int32_t,2> process(const VoiceMix&) noexcept;
private:
    using s32=std::int32_t; using s64=std::int64_t; using u32=std::uint32_t;
    struct Delay {
        MegState& m; std::array<s32,kReverbWords>* r=nullptr; std::array<s32,kChorusWords>* c=nullptr; std::array<s32,kVariationWords>* v=nullptr;
        s32 get(int off) const noexcept; void put(int off,s32 x) noexcept;
        s32 lfo(int off,u32 phase,s32 delta,int level) const noexcept;
        s32 lfo2(int off,s32 offset) const noexcept;
    };
    s32 sx(int) const noexcept; s32 m7(s32,int) const noexcept; s32 m9(s32,int) const noexcept;
    static s32 m7v(s32,s32) noexcept; static s32 m9v(s32,s32) noexcept;
    s32 lfo_step_value(int) const noexcept; void lfo_step(u32&,int) const noexcept;
    static s32 lfo_saturate(s32) noexcept; static u32 lfo_wrap(s32,s32) noexcept;
    void filtered_lfo_step(s32&,s32,int,int,int,int) const noexcept;
    s32 alfo(u32,s32,int,int,bool) const noexcept; s32 lfo_mod(s32,int) const noexcept;
    s32 lfo_scale(s32,int) const noexcept; s32 lfo_wrap_reg(s32,int) const noexcept;
    static s32 saturate(s32) noexcept;
    std::array<s32,kReverbWords> reverb_{}; std::array<s32,kChorusWords> chorus_{}; std::array<s32,kVariationWords> variation_{};
    std::array<std::uint16_t,0x40> offsets_{}; std::array<std::uint16_t,0xc0> constants_{};
    u32 buffer_offset_=0; std::uint8_t control_=0;
    bool no_reverb_=false, no_chorus_=false, no_variation_=false;
    s32 rev_vol_=0,cho_vol_=0,var_vol_=0; u32 var_lfo_phase_=0,cho_lfo_phase_=0;
    s32 var_lfo_h1_=0,var_lfo_h2_=0,var_lfo1a_=0,var_lfo2a_=0,var_lfo3a_=0,var_lfo4a_=0;
    s32 vf1_=0,vf2_=0,vf3_=0,vfl1_=0,vfl2_=0,vfl3_=0,vfr1_=0,vfr2_=0,vfr3_=0;
    s32 vf21_=0,vf22a_=0,vf22b_=0,vf23a_=0,vf23b_=0,vf24_=0,vf31_=0,vf32_=0;
    s32 vflp1_=0,vflp2_=0,vflp3_=0,vflp4_=0,vflp5_=0,vflp6_=0,vfrp1_=0,vfrp2_=0,vfrp3_=0,vfrp4_=0,vfrp5_=0,vfrp6_=0;
    s32 vh1_=0,vh2_=0,vh3_=0,vh4_=0;
    s32 cfl1_=0,cfl2_=0,cfl3_=0,cfr1_=0,cfr2_=0,cfr3_=0;
    s32 rf1_=0,rf2_=0,rf3_=0,rha_=0,rhb_=0,rhc_=0,rhd_=0;
};
} // namespace libmu50::detail
