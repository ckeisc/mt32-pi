// Ported from MAME src/devices/cpu/h8/h8.cpp do_* helpers
// (BSD-3-Clause, copyright Olivier Galibert). Member functions of Mu50H8.
// do_*2_* variants omitted (unused by the H8H instruction set).
u8 do_addx8(u8 v1, u8 v2)
{
	u8 c = ccr_ & F_C ? 1 : 0;
	u16 res = v1 + v2 + c;
	ccr_ &= ~(F_N|F_V|F_C);
	if(true) {
		if(((v1 & 0xf) + (v2 & 0xf) + c) & 0x10)
			ccr_ |= F_H;
		else
			ccr_ &= ~F_H;
	}
	if(u8(res))
		ccr_ &= ~F_Z;
	if(s8(res) < 0)
		ccr_ |= F_N;
	if(~(v1^v2) & (v1^res) & 0x80)
		ccr_ |= F_V;
	if(res & 0x100)
		ccr_ |= F_C;
	return res;
}

u8 do_subx8(u8 v1, u8 v2)
{
	u8 c = ccr_ & F_C ? 1 : 0;
	u16 res = v1 - v2 - c;
	ccr_ &= ~(F_N|F_V|F_C);
	if(true) {
		if(((v1 & 0xf) - (v2 & 0xf) - c) & 0x10)
			ccr_ |= F_H;
		else
			ccr_ &= ~F_H;
	}
	if(u8(res))
		ccr_ &= ~F_Z;
	if(s8(res) < 0)
		ccr_ |= F_N;
	if((v1^v2) & (v1^res) & 0x80)
		ccr_ |= F_V;
	if(res & 0x100)
		ccr_ |= F_C;
	return res;
}

u8 do_inc8(u8 v1, u8 v2)
{
	u8 res = v1 + v2;
	ccr_ &= ~(F_N|F_V|F_Z);
	if(!res)
		ccr_ |= F_Z;
	else if(s8(res) < 0)
		ccr_ |= F_N;
	if(~(v1^v2) & (v1^res) & 0x80)
		ccr_ |= F_V;
	return res;
}

u16 do_inc16(u16 v1, u16 v2)
{
	u16 res = v1 + v2;
	ccr_ &= ~(F_N|F_V|F_Z);
	if(!res)
		ccr_ |= F_Z;
	else if(s16(res) < 0)
		ccr_ |= F_N;
	if(~(v1^v2) & (v1^res) & 0x8000)
		ccr_ |= F_V;
	return res;
}

u32 do_inc32(u32 v1, u32 v2)
{
	u32 res = v1 + v2;
	ccr_ &= ~(F_N|F_V|F_Z);
	if(!res)
		ccr_ |= F_Z;
	else if(s32(res) < 0)
		ccr_ |= F_N;
	if(~(v1^v2) & (v1^res) & 0x80000000)
		ccr_ |= F_V;
	return res;
}

u8 do_add8(u8 v1, u8 v2)
{
	u16 res = v1 + v2;
	ccr_ &= ~(F_N|F_V|F_Z|F_C);
	if(true) {
		if(((v1 & 0xf) + (v2 & 0xf)) & 0x10)
			ccr_ |= F_H;
		else
			ccr_ &= ~F_H;
	}
	if(!u8(res))
		ccr_ |= F_Z;
	else if(s8(res) < 0)
		ccr_ |= F_N;
	if(~(v1^v2) & (v1^res) & 0x80)
		ccr_ |= F_V;
	if(res & 0x100)
		ccr_ |= F_C;
	return res;
}

u16 do_add16(u16 v1, u16 v2)
{
	u32 res = v1 + v2;
	ccr_ &= ~(F_N|F_V|F_Z|F_C);
	if(true) {
		if(((v1 & 0xfff) + (v2 & 0xfff)) & 0x1000)
			ccr_ |= F_H;
		else
			ccr_ &= ~F_H;
	}
	if(!u16(res))
		ccr_ |= F_Z;
	else if(s16(res) < 0)
		ccr_ |= F_N;
	if(~(v1^v2) & (v1^res) & 0x8000)
		ccr_ |= F_V;
	if(res & 0x10000)
		ccr_ |= F_C;
	return res;
}

u32 do_add32(u32 v1, u32 v2)
{
	u64 res = u64(v1) + u64(v2);
	ccr_ &= ~(F_N|F_V|F_Z|F_C);
	if(true) {
		if(((v1 & 0xfffffff) + (v2 & 0xfffffff)) & 0x10000000)
			ccr_ |= F_H;
		else
			ccr_ &= ~F_H;
	}
	if(!u32(res))
		ccr_ |= F_Z;
	else if(s32(res) < 0)
		ccr_ |= F_N;
	if(~(v1^v2) & (v1^res) & 0x80000000)
		ccr_ |= F_V;
	if(res & 0x100000000ULL)
		ccr_ |= F_C;
	return res;
}

u8 do_dec8(u8 v1, u8 v2)
{
	u8 res = v1 - v2;
	ccr_ &= ~(F_N|F_V|F_Z);
	if(!res)
		ccr_ |= F_Z;
	else if(s8(res) < 0)
		ccr_ |= F_N;
	if((v1^v2) & (v1^res) & 0x80)
		ccr_ |= F_V;
	return res;
}

u16 do_dec16(u16 v1, u16 v2)
{
	u16 res = v1 - v2;
	ccr_ &= ~(F_N|F_V|F_Z);
	if(!res)
		ccr_ |= F_Z;
	else if(s16(res) < 0)
		ccr_ |= F_N;
	if((v1^v2) & (v1^res) & 0x8000)
		ccr_ |= F_V;
	return res;
}

u32 do_dec32(u32 v1, u32 v2)
{
	u32 res = v1 - v2;
	ccr_ &= ~(F_N|F_V|F_Z);
	if(!res)
		ccr_ |= F_Z;
	else if(s32(res) < 0)
		ccr_ |= F_N;
	if((v1^v2) & (v1^res) & 0x80000000)
		ccr_ |= F_V;
	return res;
}

u8 do_sub8(u8 v1, u8 v2)
{
	u16 res = v1 - v2;
	ccr_ &= ~(F_N|F_V|F_Z|F_C);
	if(true) {
		if(((v1 & 0xf) - (v2 & 0xf)) & 0x10)
			ccr_ |= F_H;
		else
			ccr_ &= ~F_H;
	}
	if(!u8(res))
		ccr_ |= F_Z;
	else if(s8(res) < 0)
		ccr_ |= F_N;
	if((v1^v2) & (v1^res) & 0x80)
		ccr_ |= F_V;
	if(res & 0x100)
		ccr_ |= F_C;
	return res;
}

u16 do_sub16(u16 v1, u16 v2)
{
	u32 res = v1 - v2;
	ccr_ &= ~(F_N|F_V|F_Z|F_C);
	if(true) {
		if(((v1 & 0xfff) - (v2 & 0xfff)) & 0x1000)
			ccr_ |= F_H;
		else
			ccr_ &= ~F_H;
	}
	if(!u16(res))
		ccr_ |= F_Z;
	else if(s16(res) < 0)
		ccr_ |= F_N;
	if((v1^v2) & (v1^res) & 0x8000)
		ccr_ |= F_V;
	if(res & 0x10000)
		ccr_ |= F_C;
	return res;
}

u32 do_sub32(u32 v1, u32 v2)
{
	u64 res = u64(v1) - u64(v2);
	ccr_ &= ~(F_N|F_V|F_Z|F_C);
	if(true) {
		if(((v1 & 0xfffffff) - (v2 & 0xfffffff)) & 0x10000000)
			ccr_ |= F_H;
		else
			ccr_ &= ~F_H;
	}
	if(!u32(res))
		ccr_ |= F_Z;
	else if(s32(res) < 0)
		ccr_ |= F_N;
	if((v1^v2) & (v1^res) & 0x80000000)
		ccr_ |= F_V;
	if(res & 0x100000000ULL)
		ccr_ |= F_C;
	return res;
}

u8 do_shal8(u8 v)
{
	ccr_ &= ~(F_N|F_V|F_Z|F_C);
	if(v & 0x80)
		ccr_ |= F_C;
	if((v & 0xc0) == 0x40 || (v & 0xc0) == 0x80)
		ccr_ |= F_V;
	v <<= 1;
	if(!v)
		ccr_ |= F_Z;
	else if(s8(v) < 0)
		ccr_ |= F_N;
	return v;
}

u16 do_shal16(u16 v)
{
	ccr_ &= ~(F_N|F_V|F_Z|F_C);
	if(v & 0x8000)
		ccr_ |= F_C;
	if((v & 0xc000) == 0x4000 || (v & 0xc000) == 0x8000)
		ccr_ |= F_V;
	v <<= 1;
	if(!v)
		ccr_ |= F_Z;
	else if(s16(v) < 0)
		ccr_ |= F_N;
	return v;
}

u32 do_shal32(u32 v)
{
	ccr_ &= ~(F_N|F_V|F_Z|F_C);
	if(v & 0x80000000)
		ccr_ |= F_C;
	if((v & 0xc0000000) == 0x40000000 || (v & 0xc0000000) == 0x80000000)
		ccr_ |= F_V;
	v <<= 1;
	if(!v)
		ccr_ |= F_Z;
	else if(s32(v) < 0)
		ccr_ |= F_N;
	return v;
}

u8 do_shar8(u8 v)
{
	ccr_ &= ~(F_N|F_V|F_Z|F_C);
	if(v & 1)
		ccr_ |= F_C;
	v >>= 1;
	if(!v)
		ccr_ |= F_Z;
	else if(v & 0x40) {
		v |= 0x80;
		ccr_ |= F_N;
	}
	return v;
}

u16 do_shar16(u16 v)
{
	ccr_ &= ~(F_N|F_V|F_Z|F_C);
	if(v & 1)
		ccr_ |= F_C;
	v >>= 1;
	if(!v)
		ccr_ |= F_Z;
	else if(v & 0x4000) {
		v |= 0x8000;
		ccr_ |= F_N;
	}
	return v;
}

u32 do_shar32(u32 v)
{
	ccr_ &= ~(F_N|F_V|F_Z|F_C);
	if(v & 1)
		ccr_ |= F_C;
	v >>= 1;
	if(!v)
		ccr_ |= F_Z;
	else if(v & 0x40000000) {
		v |= 0x80000000;
		ccr_ |= F_N;
	}
	return v;
}

u8 do_shll8(u8 v)
{
	ccr_ &= ~(F_N|F_V|F_Z|F_C);
	if(v & 0x80)
		ccr_ |= F_C;
	v <<= 1;
	if(!v)
		ccr_ |= F_Z;
	else if(s8(v) < 0)
		ccr_ |= F_N;
	return v;
}

u16 do_shll16(u16 v)
{
	ccr_ &= ~(F_N|F_V|F_Z|F_C);
	if(v & 0x8000)
		ccr_ |= F_C;
	v <<= 1;
	if(!v)
		ccr_ |= F_Z;
	else if(s16(v) < 0)
		ccr_ |= F_N;
	return v;
}

u32 do_shll32(u32 v)
{
	ccr_ &= ~(F_N|F_V|F_Z|F_C);
	if(v & 0x80000000)
		ccr_ |= F_C;
	v <<= 1;
	if(!v)
		ccr_ |= F_Z;
	else if(s32(v) < 0)
		ccr_ |= F_N;
	return v;
}

u8 do_shlr8(u8 v)
{
	ccr_ &= ~(F_N|F_V|F_Z|F_C);
	if(v & 1)
		ccr_ |= F_C;
	v >>= 1;
	if(!v)
		ccr_ |= F_Z;
	return v;
}

u16 do_shlr16(u16 v)
{
	ccr_ &= ~(F_N|F_V|F_Z|F_C);
	if(v & 1)
		ccr_ |= F_C;
	v >>= 1;
	if(!v)
		ccr_ |= F_Z;
	return v;
}

u32 do_shlr32(u32 v)
{
	ccr_ &= ~(F_N|F_V|F_Z|F_C);
	if(v & 1)
		ccr_ |= F_C;
	v >>= 1;
	if(!v)
		ccr_ |= F_Z;
	return v;
}













u8 do_rotl8(u8 v)
{
	ccr_ &= ~(F_N|F_V|F_Z|F_C);
	if(v & 0x80)
		ccr_ |= F_C;
	v = (v << 1) | (v >> 7);
	if(!v)
		ccr_ |= F_Z;
	else if(s8(v) < 0)
		ccr_ |= F_N;
	return v;
}

u16 do_rotl16(u16 v)
{
	ccr_ &= ~(F_N|F_V|F_Z|F_C);
	if(v & 0x8000)
		ccr_ |= F_C;
	v = (v << 1) | (v >> 15);
	if(!v)
		ccr_ |= F_Z;
	else if(s16(v) < 0)
		ccr_ |= F_N;
	return v;
}

u32 do_rotl32(u32 v)
{
	ccr_ &= ~(F_N|F_V|F_Z|F_C);
	if(v & 0x80000000)
		ccr_ |= F_C;
	v = (v << 1) | (v >> 31);
	if(!v)
		ccr_ |= F_Z;
	else if(s32(v) < 0)
		ccr_ |= F_N;
	return v;
}

u8 do_rotr8(u8 v)
{
	ccr_ &= ~(F_N|F_V|F_Z|F_C);
	if(v & 0x01)
		ccr_ |= F_C;
	v = (v << 7) | (v >> 1);
	if(!v)
		ccr_ |= F_Z;
	else if(s8(v) < 0)
		ccr_ |= F_N;
	return v;
}

u16 do_rotr16(u16 v)
{
	ccr_ &= ~(F_N|F_V|F_Z|F_C);
	if(v & 0x0001)
		ccr_ |= F_C;
	v = (v << 15) | (v >> 1);
	if(!v)
		ccr_ |= F_Z;
	else if(s16(v) < 0)
		ccr_ |= F_N;
	return v;
}

u32 do_rotr32(u32 v)
{
	ccr_ &= ~(F_N|F_V|F_Z|F_C);
	if(v & 0x00000001)
		ccr_ |= F_C;
	v = (v << 31) | (v >> 1);
	if(!v)
		ccr_ |= F_Z;
	else if(s32(v) < 0)
		ccr_ |= F_N;
	return v;
}

u8 do_rotxl8(u8 v)
{
	u8 c = ccr_ & F_C ? 1 : 0;
	ccr_ &= ~(F_N|F_V|F_Z|F_C);
	if(v & 0x80)
		ccr_ |= F_C;
	v = (v << 1) | c;
	if(!v)
		ccr_ |= F_Z;
	else if(s8(v) < 0)
		ccr_ |= F_N;
	return v;
}

u16 do_rotxl16(u16 v)
{
	u16 c = ccr_ & F_C ? 1 : 0;
	ccr_ &= ~(F_N|F_V|F_Z|F_C);
	if(v & 0x8000)
		ccr_ |= F_C;
	v = (v << 1) | c;
	if(!v)
		ccr_ |= F_Z;
	else if(s16(v) < 0)
		ccr_ |= F_N;
	return v;
}

u32 do_rotxl32(u32 v)
{
	u32 c = ccr_ & F_C ? 1 : 0;
	ccr_ &= ~(F_N|F_V|F_Z|F_C);
	if(v & 0x80000000)
		ccr_ |= F_C;
	v = (v << 1) | c;
	if(!v)
		ccr_ |= F_Z;
	else if(s32(v) < 0)
		ccr_ |= F_N;
	return v;
}

u8 do_rotxr8(u8 v)
{
	u8 c = ccr_ & F_C ? 1 : 0;
	ccr_ &= ~(F_N|F_V|F_Z|F_C);
	if(v & 0x01)
		ccr_ |= F_C;
	v = (v >> 1) | (c << 7);
	if(!v)
		ccr_ |= F_Z;
	else if(s8(v) < 0)
		ccr_ |= F_N;
	return v;
}

u16 do_rotxr16(u16 v)
{
	u8 c = ccr_ & F_C ? 1 : 0;
	ccr_ &= ~(F_N|F_V|F_Z|F_C);
	if(v & 0x0001)
		ccr_ |= F_C;
	v = (v >> 1) | (c << 15);
	if(!v)
		ccr_ |= F_Z;
	else if(s16(v) < 0)
		ccr_ |= F_N;
	return v;
}

u32 do_rotxr32(u32 v)
{
	u8 c = ccr_ & F_C ? 1 : 0;
	ccr_ &= ~(F_N|F_V|F_Z|F_C);
	if(v & 0x00000001)
		ccr_ |= F_C;
	v = (v >> 1) | (c << 31);
	if(!v)
		ccr_ |= F_Z;
	else if(s32(v) < 0)
		ccr_ |= F_N;
	return v;
}

