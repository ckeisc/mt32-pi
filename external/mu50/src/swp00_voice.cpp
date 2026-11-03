// Standalone AWM2 voice blocks extracted/adapted from MAME's SWP00 implementation.
// Copyright (C) Olivier Galibert. MAME source license: BSD-3-Clause.
#include "swp00_voice.hpp"

#include <algorithm>
#include <array>
#include <limits>

namespace libmu50::detail {
u16 interpolation_step(u32 speed, u32 sample_counter) noexcept {
    if (speed >= 0x78) return 0x1f;
    u32 k0 = speed >> 3;
    const u32 k1 = speed & 7;
    if (speed >= 0x58) {
        k0 -= 10;
        const u32 a = (2u << k0) - 1;
        const u32 b = (1u << k0) - 1;
        constexpr u8 pattern[8] = {0x00,0x80,0x88,0xa8,0xaa,0xea,0xee,0xfe};
        return ((u32(pattern[k1]) << (sample_counter & 7)) & 0x80) ? u16(a) : u16(b);
    }
    k0 = 10 - k0;
    if (sample_counter & ((u32(1) << k0) - 1)) return 0;
    constexpr u16 pattern[8] = {0xaaaa,0xeaaa,0xeaea,0xeeea,0xeeee,0xfeee,0xfefe,0xfffe};
    return ((u32(pattern[k1]) << ((sample_counter >> k0) & 0xf)) & 0x8000) ? 1 : 0;
}

void StreamingBlock::clear() noexcept {
    phase_=0x8000; start_=loop_=0; address_=0; pitch_=0; format_=0;
    pos_=pos_dec_=0; dpcm_s0_=dpcm_s1_=0; dpcm_pos_=dpcm_delta_=0;
    first_=done_=false; last_=0;
}
void StreamingBlock::keyon() noexcept {
    pos_=-s32(start_); pos_dec_=(s32(phase_)<<1)&0x7ffe;
    dpcm_s0_=dpcm_s1_=0; dpcm_pos_=pos_+1; dpcm_delta_=0;
    first_=true; done_=false;
}
u8 StreamingBlock::byte_at(const u8* rom, std::size_t size, s64 a) const noexcept {
    if (!rom || size == 0) return 0xff;
    // Match device_rom_interface::set_rom: the ROM region is mirrored
    // over the high address bits. For a non-power-of-two region, the
    // rounded-up hole remains unmapped and reads as 0xff.
    const u32 bus_address = u32(a) & 0x00ffffff;
    const u32 rend = u32(size - 1);
    u32 rmask = rend;
    rmask |= rmask >> 1; rmask |= rmask >> 2; rmask |= rmask >> 4;
    rmask |= rmask >> 8; rmask |= rmask >> 16;
    const u32 offset = bus_address & rmask;
    if (offset > rend) return 0xff;
    return rom[offset];
}
u16 StreamingBlock::word_at(const u8* rom, std::size_t size, s64 a) const noexcept {
    return u16(byte_at(rom,size,a)) | (u16(byte_at(rom,size,a+1)) << 8);
}
void StreamingBlock::read_16(const u8* r,std::size_t n,s16& a,s16& b) const noexcept {
    const s64 p=s64(address_)+(s64(pos_)<<1); a=s16(word_at(r,n,p)); b=s16(word_at(r,n,p+2));
}
void StreamingBlock::read_12(const u8* r,std::size_t n,s16& a,s16& b) const noexcept {
    const s64 p=s64(address_)+(s64(pos_)>>2)*6;
    switch(pos_&3) {
    case 0: { u16 w0=word_at(r,n,p),w1=word_at(r,n,p+2); a=s16(w0<<4); b=s16(((w0>>8)|(w1<<8))&0xfff0); break; }
    case 1: { u16 w0=word_at(r,n,p),w1=word_at(r,n,p+2),w2=word_at(r,n,p+4); a=s16(((w0>>8)|(w1<<8))&0xfff0); b=s16(((w1>>4)&0x0ff0)|(w2<<12)); break; }
    case 2: { u16 w1=word_at(r,n,p+2),w2=word_at(r,n,p+4); a=s16(((w1>>4)&0x0ff0)|(w2<<12)); b=s16(w2&0xfff0); break; }
    default:{ u16 w2=word_at(r,n,p+4),w3=word_at(r,n,p+6); a=s16(w2&0xfff0); b=s16(w3<<4); break; }
    }
}
void StreamingBlock::read_8(const u8* r,std::size_t n,s16& a,s16& b) const noexcept {
    const s64 p=s64(address_)+pos_; a=s16(u16(byte_at(r,n,p))<<8); b=s16(u16(byte_at(r,n,p+1))<<8);
}
void StreamingBlock::dpcm_step(u8 input) noexcept {
    static const std::array<s16,256> expand=[] { constexpr s16 offset[4]={0,0x20,0x60,0xe0}; std::array<s16,256> d{}; for(u32 i=0;i<128;i++){u32 e=i>>5; s16 v=s16(((i&0x1f)<<e)+offset[e]); d[i]=v; d[i+128]=s16(-v);} d[0x80]=0x88; return d; }();
    static constexpr s32 maxv[8]={0x7fff,0x7ffe,0x7ffc,0x7ff8,0x7ff0,0x7fe0,0x7fc0,0x7f80};
    const u32 mode=format_&3, scale=(format_>>2)&7; const s32 limit=maxv[scale];
    dpcm_s0_=dpcm_s1_;
    s32 delta=dpcm_delta_+expand[input];
    s32 sample=s32(dpcm_s1_)+delta*s32(1u<<scale);
    if(sample < -0x8000){sample=-0x8000;delta=0;} else if(sample>limit){sample=limit;delta=0;}
    dpcm_s1_=s16(sample);
    switch(mode){case 0:delta=delta*7/8;break;case 1:delta=delta*3/4;break;case 2:delta/=2;break;default:delta=0;break;}
    dpcm_delta_=delta;
}
void StreamingBlock::read_8c(const u8* r,std::size_t n,s16& a,s16& b) noexcept {
    while(dpcm_pos_ != pos_+2){dpcm_step(byte_at(r,n,s64(address_)+dpcm_pos_));++dpcm_pos_;}
    a=dpcm_s0_; b=dpcm_s1_;
}
std::pair<s16,bool> StreamingBlock::step(const u8* rom,std::size_t size,s32 fmod) noexcept {
    if(done_) return {last_,false};
    s16 v0=0,v1=0;
    switch(format_>>6){case 0:read_16(rom,size,v0,v1);break;case 1:read_12(rom,size,v0,v1);break;case 2:read_8(rom,size,v0,v1);break;default:read_8c(rom,size,v0,v1);break;}
    const u32 step=((u32(pitch_&0xfff) << (8 + (s16(pitch_) >> 12))) >> 4);
    const s32 interp=(pos_dec_>>6)&0x1ff;
    const s32 result=s32(v0)+((((s32(v1)-v0)*interp)>>10)<<1);
    pos_dec_+=s32(step)+(fmod*16);
    if(pos_dec_>=0x8000){first_=false;pos_+=pos_dec_>>15;pos_dec_&=0x7fff;
        if(pos_>=loop_){if(loop_){pos_-=loop_;if((format_&0xc0)!=0xc0){pos_dec_+=(s32(format_)<<9)&0x7e00;if(pos_dec_>=0x8000)++pos_;pos_dec_&=0x7fff;}dpcm_pos_=1;}
            else{done_=true;last_=s16(result);return {last_,true};}}
    }
    return {s16(result),false};
}

void EnvelopeBlock::clear() noexcept {attack_speed_=attack_level_=decay_speed_=decay_level_=0;level_=0xfff;mode_=DECAY_DONE;}
void EnvelopeBlock::keyon() noexcept {if(decay_speed_&0x80){level_=0;mode_=DECAY;}else{level_=(attack_speed_&0x80)?s32(attack_level_)<<4:0;mode_=ATTACK;}}
bool EnvelopeBlock::active() const noexcept {return mode_!=DECAY_DONE||level_<0x800;}
u16 EnvelopeBlock::step(u32 counter) noexcept {
    u16 result=u16(level_);
    switch(mode_){case ATTACK:{if(attack_speed_&0x80){s32 v=level_-interpolation_step((attack_speed_&0x7f)+((level_>>7)<<2),counter);if(v<=0){v=0;mode_=DECAY;}level_=v;}
        else{s32 v=level_+interpolation_step(attack_speed_,counter);if(attack_level_){s32 lim=s32(attack_level_)<<4;if(v>=lim){mode_=DECAY;if(v>=0xfff)v=0xfff;}}
            else{if(v>=0x800){v=0;mode_=DECAY;}result=0;}level_=v;}break;}
    case DECAY:{u32 key=decay_speed_>=0xe0?decay_speed_-0x90+((level_>>7)<<2):(decay_speed_&0x7f);s32 lim=s32(decay_level_)<<4; s32 v=level_;
        if (v < lim) {
            v += interpolation_step(key, counter);
            if (v > lim) { mode_ = DECAY_DONE; if (v > 0xfff) v = 0xfff; }
        } else if (v > lim) {
            v -= interpolation_step(key, counter);
            if (v < lim) { mode_ = DECAY_DONE; if (v < 0) v = 0; }
        } else {
            mode_ = DECAY_DONE;
        }
        level_ = v;
        break;
    }
    default:break;}
    return result;
}
void EnvelopeBlock::trigger_release() noexcept {mode_=DECAY;}
void EnvelopeBlock::set_decay_speed(u8 v) noexcept {decay_speed_=v;if(v&0x80)mode_=DECAY;}

void FilterBlock::clear() noexcept {info_=0x27ff;speed_=0;sweep_=SWEEP_NONE;k_target_=0x7ff;k_=0xfff;q_=0x80;b_=l_=0;}
void FilterBlock::keyon() noexcept {b_=l_=0;if(speed_&0x80){sweep_=SWEEP_UP;k_=0x800;}else k_=k_target_;}
s32 FilterBlock::step(s16 input,s32 lmod,u32 counter) noexcept {
    s32 km=std::max<s32>(s32(k_)-lmod,0); s64 k=s64(0x101+(km&0xff))<<(km>>8);
    s32 h=(s32(input)*64)-l_-s32((s64(q_)*b_)>>7); b_+=s32((k*h)>>24);l_+=s32((k*b_)>>24);
    if(sweep_==SWEEP_NONE){if(k_<k_target_){k_+=interpolation_step(speed_&0x7f,counter);if(k_>k_target_)k_=k_target_;}else if(k_>k_target_){u16 d=interpolation_step(speed_&0x7f,counter);k_=k_>d?u16(k_-d):0;if(k_<k_target_)k_=k_target_;}}
    else if(sweep_==SWEEP_UP){k_+=interpolation_step(speed_&0x7f,counter);if(k_>=0xfff){k_=k_target_;sweep_=SWEEP_DONE;}}
    return l_;
}
void FilterBlock::set_info_low(u8 v) noexcept {info_=(info_&0xff00)|v;k_target_=info_&0x7ff;if(k_target_)k_target_|=0x800;}
void FilterBlock::set_info_high(u8 v) noexcept {info_=(info_&0x00ff)|(u16(v)<<8);s32 q=(info_>>11)+4;q_=(0x10-(q&7))<<(4-(q>>3));k_target_=info_&0x7ff;if(k_target_)k_target_|=0x800;}

void LfoBlock::clear() noexcept {counter_=0;lamod_=fmod_=0;speed_=0x80;}
void LfoBlock::keyon() noexcept {}
std::tuple<u32,s32,s32> LfoBlock::step() noexcept {
    u32 e=(speed_>>3)&7;u32 step=e<7?((8|(speed_&7))<<e):((8+2*(speed_&7))<<7);
    counter_=(counter_+step)&0x3fffff;u32 amod=0;s32 fmod=0,lmod=0;
    if(!(speed_&0x80)){s32 shaped=(speed_&0x40)?s32(((counter_<<1)&0x3fffff)^(counter_&0x200000?0x3fffff:0)):s32(counter_);
        amod=(u32(shaped)*(lamod_&0x1f))>>16;fmod=((shaped-0x200000)*fmod_)>>21;lmod=(u32(shaped)*(lamod_&0xe0))>>20;}
    return {amod,fmod,lmod};
}

void MixerBlock::clear() noexcept {current_global_=current_pan_l_=current_pan_r_=target_global_=target_pan_l_=target_pan_r_=0xfff;global_=pan_=dry_=rev_=cho_=var_=0xff;}
void MixerBlock::keyon() noexcept {current_global_=target_global_;current_pan_l_=target_pan_l_;current_pan_r_=target_pan_r_;}
s32 MixerBlock::volume_apply(s32 level,s32 sample) noexcept {if(level>=0xfff)return 0;s32 e=level>>8,m=level&0xff;s64 mul=(0x1000000-(m<<15))>>e;return s32((s64(sample)*mul)>>24);}
void MixerBlock::step(s32 sample,u16 envelope,u16 tremolo,s32& dl,s32& dr,s32& rev,s32& cl,s32& cr,s32& vl,s32& vr) noexcept {
    u16 base=u16(envelope+tremolo+current_global_);
    dl+=volume_apply(base+(dry_<<4)+current_pan_l_,sample);dr+=volume_apply(base+(dry_<<4)+current_pan_r_,sample);
    rev+=volume_apply(base+(rev_<<4),sample);cl+=volume_apply(base+(cho_<<4)+current_pan_l_,sample);cr+=volume_apply(base+(cho_<<4)+current_pan_r_,sample);
    vl+=volume_apply(base+(var_<<4)+current_pan_l_,sample);vr+=volume_apply(base+(var_<<4)+current_pan_r_,sample);
    if(current_global_<target_global_)++current_global_;else if(current_global_>target_global_)--current_global_;
    if(current_pan_l_<target_pan_l_)++current_pan_l_;else if(current_pan_l_>target_pan_l_)--current_pan_l_;
    if(current_pan_r_<target_pan_r_)++current_pan_r_;else if(current_pan_r_>target_pan_r_)--current_pan_r_;
}
void MixerBlock::set_global(u8 v) noexcept {global_=v;target_global_=u16(v)<<4;}
void MixerBlock::set_pan(u8 v) noexcept {pan_=v;target_pan_l_=(v<<2)&0x3c0;target_pan_r_=(v<<6)&0x3c0;if(target_pan_l_==0x3c0)target_pan_l_=0xfff;if(target_pan_r_==0x3c0)target_pan_r_=0xfff;}

void VoiceChannel::clear() noexcept {
    stream_.clear(); envelope_.clear(); filter_.clear(); lfo_.clear(); mixer_.clear();
}
void VoiceChannel::keyon() noexcept {
    stream_.keyon(); envelope_.keyon(); filter_.keyon(); lfo_.keyon(); mixer_.keyon();
}
void VoiceChannel::render_one(const u8* rom, std::size_t rom_size, u32 sample_counter, VoiceMix& mix) noexcept {
    const auto [amod, fmod, lmod] = lfo_.step();
    if (!envelope_.active()) return;
    const auto [sample1, trigger_release] = stream_.step(rom, rom_size, fmod);
    if (trigger_release) envelope_.trigger_release();
    const s32 sample2 = filter_.step(sample1, lmod, sample_counter);
    const u32 envelope_level = envelope_.step(sample_counter);
    mixer_.step(sample2, u16(envelope_level), u16(amod), mix.dry_l, mix.dry_r,
                mix.reverb, mix.chorus_l, mix.chorus_r, mix.variation_l, mix.variation_r);
}

void Awm2Core::clear() noexcept {
    for (auto& v : voices_) v.clear();
}
VoiceMix Awm2Core::render_one(const u8* rom, std::size_t rom_size, u32 sample_counter) noexcept {
    VoiceMix mix;
    for (auto& v : voices_) v.render_one(rom, rom_size, sample_counter, mix);
    return mix;
}
} // namespace libmu50::detail
