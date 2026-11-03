// license:BSD-3-Clause
// Adapted from MAME src/devices/sound/swp00.cpp (Olivier Galibert).
#include "swp00_meg.hpp"
#include <algorithm>

namespace libmu50::detail {
void MegState::reset() noexcept {
 reverb_.fill(0);chorus_.fill(0);variation_.fill(0);offsets_.fill(0);constants_.fill(0);buffer_offset_=0;control_=0;
 rev_vol_=cho_vol_=var_vol_=0;var_lfo_phase_=cho_lfo_phase_=0;
 var_lfo_h1_=var_lfo_h2_=var_lfo1a_=var_lfo2a_=var_lfo3a_=var_lfo4a_=0;
 vf1_=vf2_=vf3_=vfl1_=vfl2_=vfl3_=vfr1_=vfr2_=vfr3_=0;
 vf21_=vf22a_=vf22b_=vf23a_=vf23b_=vf24_=vf31_=vf32_=0;
 vflp1_=vflp2_=vflp3_=vflp4_=vflp5_=vflp6_=vfrp1_=vfrp2_=vfrp3_=vfrp4_=vfrp5_=vfrp6_=0;
 vh1_=vh2_=vh3_=vh4_=cfl1_=cfl2_=cfl3_=cfr1_=cfr2_=cfr3_=rf1_=rf2_=rf3_=rha_=rhb_=rhc_=rhd_=0;
}
void MegState::write_offset(std::uint16_t a,std::uint8_t v) noexcept {if(a<0x180||a>0x1ff)return;auto i=std::size_t(a-0x180);auto&w=offsets_[i>>1];unsigned s=(i&1)?0:8;w=std::uint16_t((w&~(0xffu<<s))|(std::uint16_t(v)<<s));}
std::uint8_t MegState::read_offset(std::uint16_t a)const noexcept {if(a<0x180||a>0x1ff)return 0xff;auto i=std::size_t(a-0x180);return std::uint8_t(offsets_[i>>1]>>((i&1)?0:8));}
void MegState::write_constant(std::uint16_t a,std::uint8_t v) noexcept {if(a<0x200||a>0x37f)return;auto i=std::size_t(a-0x200);auto&w=constants_[i>>1];unsigned s=(i&1)?0:8;w=std::uint16_t((w&~(0xffu<<s))|(std::uint16_t(v)<<s));}
std::uint8_t MegState::read_constant(std::uint16_t a)const noexcept {if(a<0x200||a>0x37f)return 0xff;auto i=std::size_t(a-0x200);return std::uint8_t(constants_[i>>1]>>((i&1)?0:8));}
MegState::s32 MegState::sx(int r)const noexcept {s32 v=constants_[std::size_t(r)]&0x3ff;if(v>0x200)v|=s32(0xfffffc00u);return v;}
MegState::s32 MegState::m7v(s32 v,s32 m)noexcept{return s32((s64(v)*m)>>7);} MegState::s32 MegState::m9v(s32 v,s32 m)noexcept{return s32((s64(v)*m)>>9);}
MegState::s32 MegState::m7(s32 v,int r)const noexcept{return m7v(v,sx(r));} MegState::s32 MegState::m9(s32 v,int r)const noexcept{return m9v(v,sx(r));}
MegState::s32 MegState::lfo_step_value(int r)const noexcept {u32 e=(constants_[r]>>7)&7;return s32((constants_[r]&0x7f)<<(e==7?15:e));}
void MegState::lfo_step(u32&p,int r)const noexcept{p=(p+u32(lfo_step_value(r)))&0x7fffff;}
MegState::s32 MegState::lfo_saturate(s32 p)noexcept{return p< -0x400000?-0x400000:p>=0x400000?0x3fffff:p;}
MegState::u32 MegState::lfo_wrap(s32 p,s32 d)noexcept{s32 q=(p-(d*8192))&0x7fffff;if(q&0x400000)q^=0x7fffff;return u32(q);}
void MegState::filtered_lfo_step(s32&pos,s32 phase,int delta,int post,int scale,int feedback)const noexcept{s32 p=lfo_saturate((delta==-1?phase:s32(lfo_wrap(phase,sx(delta))))-(sx(post)*8192));s64 q=s64(lfo_step_value(scale))*p+s64(0x400000-lfo_step_value(feedback))*pos;pos=s32(q>>22);}
MegState::s32 MegState::alfo(u32 phase,s32 delta,int level,int offset,bool sub)const noexcept{u32 p=lfo_wrap(s32(phase),delta);s32 o=sx(offset);if(sub)o=-o;s32 b=s32((s64(p)*sx(level))>>19)+(o*8);s32 a=((b&0x1ff)|0x200)<<((b>>9)&15);a>>=8;return a<=-0x200?-0x1ff:a>=0x200?0x200:a;}
MegState::s32 MegState::lfo_mod(s32 p,int r)const noexcept{return (m9(p,r)>>13)+0x200;} MegState::s32 MegState::lfo_scale(s32 p,int r)const noexcept{return lfo_saturate((p-(sx(r)<<13))*4);} MegState::s32 MegState::lfo_wrap_reg(s32 p,int r)const noexcept{return s32(lfo_wrap(p,sx(r)));}
MegState::s32 MegState::saturate(s32 v)noexcept{return v<=-0x20000?-0x20000:v>0x1ffff?0x1ffff:v;}
MegState::s32 MegState::Delay::get(int off)const noexcept{const auto idx=(m.buffer_offset_+m.offsets_[off/3])&((r?MegState::kReverbWords:c?MegState::kChorusWords:MegState::kVariationWords)-1);return r?(*r)[idx]:c?(*c)[idx]:(*v)[idx];}
void MegState::Delay::put(int off,s32 x)noexcept{const auto idx=(m.buffer_offset_+m.offsets_[off/3])&((r?MegState::kReverbWords:c?MegState::kChorusWords:MegState::kVariationWords)-1);if(r)(*r)[idx]=x;else if(c)(*c)[idx]=x;else(*v)[idx]=x;}
MegState::s32 MegState::Delay::lfo(int off,u32 phase,s32 delta,int level)const noexcept {
 u32 p=MegState::lfo_wrap(s32(phase),delta);u32 o32=p*u32(m.sx(level));std::uint64_t o=o32;u32 i=u32(o>>22);s32 f=s32(o&0x3fffff);u32 pos=m.buffer_offset_+m.offsets_[off/3];
 const s32* b;std::size_t n;if(r){b=r->data();n=r->size();}else if(c){b=c->data();n=c->size();}else{b=v->data();n=v->size();}pos+=i;
 return s32((s64(b[pos&(n-1)])*(0x400000-f)+s64(b[(pos+1)&(n-1)])*f)>>22);
}
MegState::s32 MegState::Delay::lfo2(int off,s32 o)const noexcept {
 u32 i=u32(o>>11);s32 f=o&0x7ff;u32 p=m.buffer_offset_+m.offsets_[off/3]+i;const s32* b;std::size_t n;
 if(r){b=r->data();n=r->size();}else if(c){b=c->data();n=c->size();}else{b=v->data();n=v->size();}
 return s32((s64(b[p&(n-1)])*(0x800-f)+s64(b[(p+1)&(n-1)])*f)>>11);
}

std::array<std::int32_t,2> MegState::process(const VoiceMix& in) noexcept {
 Delay br{*this,&reverb_,nullptr,nullptr},bc{*this,nullptr,&chorus_,nullptr},bv{*this,nullptr,nullptr,&variation_};
 s32 dl=in.dry_l,dr=in.dry_r,rev=in.reverb,cl=in.chorus_l,cr=in.chorus_r,vl=in.variation_l,vr=in.variation_r;
 var_vol_=m9(var_vol_,0xbd)+constants_[0xbc];vl=m7(vl,0x04);vr=m7(vr,0x07);s32 vo_l=0,vo_r=0;
 switch(control_&0xc0){
 case 0x00:{
  s32 a=m7(vfl1_,0x7e)+m7(vl,0x7f)+m9(vfl2_,0x80),b=m7(vfl2_,0xa7)+m7(a,0xa8)+m9(vfl3_,0xa9);vfl1_=vl;vfl2_=a;vfl3_=b;
  s32 c=m7(vfr1_,0x98)+m7(vr,0x99)+m9(vfr2_,0x9a),d=m7(vfr2_,0x9b)+m7(c,0x9c)+m9(vfr3_,0x9d);vfr1_=vr;vfr2_=c;vfr3_=d;
  lfo_step(var_lfo_phase_,0x77);s32 ph=m7(m7(s32(var_lfo_phase_),0x6d),0x70)&0x7fffff;
  filtered_lfo_step(var_lfo1a_,s32(var_lfo_phase_),0x6e,0x6f,0x72,0x71);filtered_lfo_step(var_lfo2a_,s32(var_lfo_phase_),0x79,0x7a,0x7c,0x7b);filtered_lfo_step(var_lfo3a_,s32(var_lfo_phase_),0x88,0x89,0x8b,0x8a);
  s32 l1=lfo_scale(var_lfo1a_,0x73),l2=lfo_scale(var_lfo1a_,0x7d),l3=lfo_scale(var_lfo1a_,0x8c),p1=lfo_wrap_reg(ph,0x74),p2=lfo_wrap_reg(ph,0x84),p3=lfo_wrap_reg(ph,0x8d);
  filtered_lfo_step(var_lfo4a_,p3,-1,0x8e,0x90,0x8f);s32 l4=lfo_scale(var_lfo4a_,0x91);
  s32 t1=bv.lfo2(0x78,m9(l1,0x75)+m9(p1,0x76)),t2=bv.lfo2(0x87,m9(l2,0x85)+m9(p2,0x86)),t3=bv.lfo2(0x99,m9(l3,0x95)+m9(p3,0x96)),t4=bv.lfo2(0xa8,m9(l4,0xa5));
  s32 m1=lfo_mod(l1,0x83),m2=lfo_mod(l2,0x94),m3=lfo_mod(l3,0xa4);var_lfo_h1_=m9(var_lfo_h1_,0x9e)+m9(t1,0x9f);var_lfo_h2_=m9(var_lfo_h2_,0xa0)+m9(t1,0xa1);
  bv.put(0xae,b+m9(d,0xaa)+m9(var_lfo_h1_,0xab)+m9(var_lfo_h2_,0xac));bv.put(0xb1,m9(d,0xad)+m9(var_lfo_h1_,0xae)+m9(var_lfo_h2_,0xaf));
  vo_l=m9(b,0xb2)+m9(d,0xb3)+m9(m9v(t2,m1),0xb4)+m9(m9v(t3,m3),0xb5)+m9(t4,0xb6);vo_r=m9(b,0xb7)+m9(d,0xb8)+m9(m9v(t2,m2),0xb9)+m9(m9v(t3,m3),0xba)+m9(t4,0xbb);break;}
 case 0x40:{
  s32 a=m7(vfl1_,0x6d)+m7(vl,0x6e)+m9(vfl2_,0x6f),b=m7(vfl2_,0x70)+m7(a,0x71)+m9(vfl3_,0x72);vfl1_=vl;vfl2_=a;vfl3_=b;
  s32 c=m7(vfr1_,0x73)+m7(vr,0x74)+m9(vfr2_,0x75),d=m7(vfr2_,0x76)+m7(c,0x77)+m9(vfr3_,0x78);vfr1_=vr;vfr2_=c;vfr3_=d;
  s32 rawl=m9(vflp4_,0x7b)+m9(vflp5_,0x7c)+m9(vflp6_,0x7d),rawr=m9(vfrp4_,0x7e)+m9(vfrp5_,0x7f)+m9(vfrp6_,0x80);s32 ol=m9(rawl,0xa3)+m9(vfrp3_,0xa4)+m9(vfrp5_,0xa5),orr=m9(rawr,0xa7);
  lfo_step(var_lfo_phase_,0x79);s32 al=0x200-alfo(var_lfo_phase_,0,0x83,0x82,false),ar=0x200-alfo(var_lfo_phase_,sx(0x9c),0x9e,0x9d,false);
  s32 x=m9(b,0x84)+m9(d,0x85)+m9(rawl,0x86)+m9(rawr,0x87),y=vflp1_+m9v(vflp2_-x,al);vflp1_=x;s32 z=vflp2_+m9v(vflp3_-y,al);vflp2_=y;s32 q=vflp3_+m9v(vflp4_-z,al);vflp3_=z;s32 t=vflp4_+m9v(vflp5_-q,al);vflp4_=q;vflp6_=vflp5_+m9v(vflp6_-t,al);vflp5_=t;
  x=m9(d,0x9f)+m9(rawl,0xa0)+m9(rawr,0xa1);y=vfrp1_+m9v(vfrp2_-x,ar);vfrp1_=x;z=vfrp2_+m9v(vfrp3_-y,ar);vfrp2_=y;q=vfrp3_+m9v(vfrp4_-z,ar);vfrp3_=z;t=vfrp4_+m9v(vfrp5_-q,ar);vfrp4_=q;vfrp6_=vfrp5_+m9v(vfrp6_-t,ar);vfrp5_=t;
  vo_l=ol+m9(b,0xa2);vo_r=orr+m9(d,0xa6);break;}
 case 0x80:{
  s32 mid=m9(vl,0x6d)+m9(vr,0x6e),a=m7(vf1_,0x6f)+m7(mid,0x70)+m9(vf2_,0x71),b=m7(vf2_,0x72)+m7(a,0x73)+m9(vf3_,0x74);vf1_=mid;vf2_=a;vf3_=b;
  bv.put(0x7e,m9(bv.get(0x6c),0x7b)+m9(mid,0x7c));s32 tap0=m7(bv.get(0x6c),0x7e)+m7(mid,0x7f);bv.put(0x84,m9(bv.get(0x78),0x81)+m9(tap0,0x82));s32 f31=bv.get(0x6f),f32=m7(vf21_,0x77)+m7(f31,0x78)+m9(vf32_,0x79);bv.put(0x87,m7(bv.get(0x78),0x84)+m7(tap0,0x85)+m9(f32,0x86));vf31_=f31;vf32_=f32;
  s32 t1=m9(bv.get(0x6f),0x99)+m9(bv.get(0x72),0x9a)+m9(bv.get(0x75),0x9b)+m9(bv.get(0x8d),0x9c)+m9(bv.get(0x90),0x9d)+m9(bv.get(0x93),0x9e)+m9(bv.get(0x96),0x9f),t2=m9(bv.get(0x9f),0xb4)+m9(bv.get(0xa2),0xb5)+m9(bv.get(0xa5),0xb6)+m9(bv.get(0xa8),0xb7)+m9(bv.get(0xab),0xb8)+m9(bv.get(0xae),0xb9)+m9(bv.get(0xb1),0xba);bv.put(0xb7,t1);bv.put(0xba,t2);
  s32 t2b=t2+m9(br.get(0xb4),0xbb);bv.put(0x8a,m9(bv.get(0x7b),0x88)+m9(t2b,0x89));s32 gl=m7(bv.get(0x7b),0x8b)+m7(t2b,0x8c);s32 t1b=t1+m9(br.get(0x99),0xa0);bv.put(0x9c,m9(bv.get(0x81),0x8e)+m9(t1b,0x8f));s32 gr=m7(bv.get(0x7b),0x8b)+m7(t1b,0x8c);
  s32 d1=saturate(m7(b,0x76)),d2=saturate(m7(d1,0x83)),d3=saturate(m7(d2,0x87)),d4=saturate(m7(d3,0x8a)),d5=saturate(m7(d4,0x8d)),d6=saturate(m7(d5,0x90)),dist=m9(m9(d1,0x91)+m9(d2,0x92)+m9(d3,0x93)+m9(d4,0x94)+m9(d5,0x95)+m9(d6,0x96),0xa1);
  s32 f22=m7(vf21_,0xa2)+m7(dist,0xa3)+m9(vf22a_,0xa4),f23=m7(vf23b_,0xa5)+m7(vf23a_,0xa6)+m7(vf22b_,0xa7)+m7(vf22a_,0xa8)+m7(f22,0xa9),f24=m7(vf23a_,0xaa)+m7(f23,0xab)+m9(vf24_,0xac);vf21_=dist;vf22b_=vf22a_;vf22a_=f22;vf23b_=vf23a_;vf23a_=f23;vf24_=f24;
  vo_l=m9(vl,0xad)+m9(gl,0xaf)+m9(f24,0xb0);vo_r=m9(vr,0xb1)+m9(gr,0xb2)+m9(f24,0xb3);break;}
 case 0xc0:{
  s32 mid=m9(vl,0x6d)+m9(vr,0x6e),a=m7(vf1_,0x6f)+m7(mid,0x70)+m9(vf2_,0x71),b=m7(vf2_,0x72)+m7(a,0x73)+m9(vf3_,0x74);vf1_=mid;vf2_=a;vf3_=b;
  s32 a1=bv.get(0x6c),b1=bv.get(0x6f),c1=bv.get(0x72);bv.put(0x75,b+m9(a1,0x75));bv.put(0x78,m9(b1,0x76)+m9(a1,0x77));s32 t2a=m7(b1,0x78)+m7(a1,0x79);bv.put(0x7b,m9(b1,0x7a)+m9(t2a,0x7b));s32 t2b=m7(c1,0x7c)+m7(t2a,0x7d);s32 t1d=bv.get(0x9c),t1e=bv.get(0x9f);bv.put(0xa8,m9(vh1_,0xa5)+m9(t1d,0xa6)+m9(t2b,0xa7));vh1_=t1d;bv.put(0xae,m9(vh2_,0xa8)+m9(t1e,0xa9)+m9(t2b,0xaa));vh2_=t1e;s32 t1f=bv.get(0xab),t1g=bv.get(0xb1);bv.put(0xb7,m9(vh3_,0xb3)+m9(t1f,0xb4)+m9(t2b,0xb5));vh3_=t1f;bv.put(0xba,m9(vh4_,0xb6)+m9(t1g,0xb7)+m9(t2b,0xb8));vh4_=t1g;
  s32 t1h=bv.get(0x7e),t3a=m9(bv.get(0x81)+bv.get(0x84)+bv.get(0x87)+bv.get(0x8a),0x8f)+m9(t1h,0x93),t3b=bv.get(0xa5);bv.put(0xb4,m9(t3b,0xaf)+m9(t3a,0xb0));s32 ol=m7(t3b,0xb1)+m7(t3a,0xb2),t4a=m9(bv.get(0x8d)+bv.get(0x90)+bv.get(0x93)+bv.get(0x96),0x9c)+m9(t1h,0xa0),t4b=bv.get(0x99);bv.put(0xa2,m9(t4b,0xa1)+m9(t4a,0xa2));s32 oor=m7(t4b,0xa3)+m7(t4a,0xa4);
  s32 x=m7(vfl1_,0x80)+m7(vl,0x81)+m9(vfl2_,0x82),y=m7(vfl2_,0x83)+m7(x,0x84)+m9(vfl3_,0x85);vfl1_=vl;vfl2_=x;vfl3_=y;s32 z=m7(vfr1_,0x6f)+m7(vr,0x70)+m9(vfr2_,0x71),q=m7(vfr2_,0x72)+m7(z,0x73)+m9(vfr3_,0x74);vfr1_=vr;vfr2_=z;vfr3_=q;s32 wl=m7(y,0x94)+m7(q,0x95),wr=m7(q,0x88);lfo_step(var_lfo_phase_,0x7e);s32 amp=alfo(var_lfo_phase_,0,0x86,0x87,true);
  vflp1_=m9v(m9(vflp1_,0x89)+m9(vflp2_,0x8a)+wl,amp)+m9(vflp1_,0x8b);vflp2_=m9v(vflp1_,amp)+m9(vflp2_,0x8d);vfrp1_=m9v(m9(vfrp1_,0x96)+m9(vfrp2_,0x97)+wr,amp)+m9(vfrp1_,0x98);vfrp2_=m9v(vfrp1_,amp)+m9(vfrp2_,0x9a);
  vo_l=m9(y,0xb9)+m9(vflp1_,0xba)+m9(ol,0xbb);vo_r=m9(q,0xab)+m9(vr,0xac)+m9(vfrp1_,0xad)+m9(oor,0xae);break;}
 }
 cho_vol_=m9(cho_vol_,0x58)+constants_[0x57];cl=m7(cl,0x02);cr=m7(cr,0x05);cl+=m9v(m7(vo_l,0x03),var_vol_);cr+=m9v(m7(vo_r,0x06),var_vol_);lfo_step(cho_lfo_phase_,0x09);
 s32 c1=bc.lfo(0x1b,cho_lfo_phase_,0,0x1a),c2=bc.lfo(0x2a,cho_lfo_phase_,constants_[0x25],0x28),c3=bc.lfo(0x39,cho_lfo_phase_,constants_[0x34],0x37);
 s32 cr2=m7(cfr1_,0x3c)+m7(cr,0x3d)+m9(cfr2_,0x3e),crf=m7(cfr2_,0x3f)+m7(cr2,0x40)+m9(cfr3_,0x41);cfr1_=cr;cfr2_=cr2;cfr3_=crf;
 s32 cl2=m7(cfl1_,0x49)+m7(cl,0x4a)+m9(cfl2_,0x4b),clf=m7(cfl2_,0x4c)+m7(cl2,0x4d)+m9(cfl3_,0x4e);cfl1_=cl;cfl2_=cl2;cfl3_=clf;bc.put(0x42,m9(c2,0x42)+crf);bc.put(0x51,m9(c1,0x4f)+clf+m9(crf,0x50));s32 co_l=m9(c1,0x60)+m9(c3,0x61),co_r=m9(c2,0x69)+m9(c3,0x6a);
 rev_vol_=m9(rev_vol_,0x0c)+constants_[0x0b];rev=m7(rev,0x11);rev+=m9v(m7(co_l,0x12)+m7(co_r,0x13),cho_vol_);rev+=m9v(m7(vo_l,0x14)+m7(vo_r,0x15),var_vol_);
 s32 rf2=m7(rf1_,0x2d)+m7(rev,0x2e)+m9(rf2_,0x2f),rf=m7(rf2_,0x30)+m7(rf2,0x31)+m9(rf3_,0x32);rf1_=rev;rf2_=rf2;rf3_=rf;
 br.put(0x30,m9(br.get(0x21),0x29)+m9(br.get(0x18),0x2a));br.put(0x33,m9(br.get(0x1b),0x33)+rf);s32 r1=m7(br.get(0x33),0x2b)+m7(br.get(0x18),0x2c),r2=m7(br.get(0x27),0x3a)+m7(r1,0x3b);br.put(0x3f,m9(br.get(0x39),0x38)+m9(r1,0x39));
 br.put(0x5d,m9(rha_,0x59)+m9(br.get(0x24),0x5a)+m9(r2,0x5b));rha_=br.get(0x24);br.put(0x63,m9(rhb_,0x5c)+m9(br.get(0x54),0x5d)+m9(r2,0x5e));rhb_=br.get(0x54);br.put(0x69,m9(rhc_,0x62)+m9(br.get(0x5a),0x63)+m9(r2,0x64));rhc_=br.get(0x63);br.put(0x6c,m9(rhd_,0x65)+m9(br.get(0x60),0x66)+m9(r2,0x67));rhd_=br.get(0x66);
 s32 bl=m9(br.get(0x00)+br.get(0x03)+br.get(0x06)+br.get(0x09),0x1c)+m9(br.get(0xbd),0x1b);br.put(0x48,m9(br.get(0x36),0x45)+m9(bl,0x46));s32 rol=m7(br.get(0x36),0x47)+m7(bl,0x48);s32 rr=m9(br.get(0x0c)+br.get(0x0f)+br.get(0x12)+br.get(0x15),0x21)+m9(br.get(0xbd),0x20);br.put(0x48,m9(br.get(0x36),0x51)+m9(rr,0x52));s32 ror=m7(br.get(0x36),0x53)+m7(rr,0x54);
 dl=m7(dl,0xbe);dr=m7(dr,0x01);dl+=m9v(rol,rev_vol_)+m9v(m9(co_l,0x17),cho_vol_)+m9v(m9(vo_l,0x18),var_vol_);dr+=m9v(ror,rev_vol_)+m9v(m9(co_r,0x0e),cho_vol_)+m9v(m9(vo_r,0x0f),var_vol_);buffer_offset_--;
 return {std::clamp<s32>(dl>>6,-0x20000,0x1ffff),std::clamp<s32>(dr>>6,-0x20000,0x1ffff)};
}
} // namespace libmu50::detail
