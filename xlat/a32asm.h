// Minimal ARM (A32, ARMv7-A + VFPv3-D16, VFPv4/IDIV optional) assembler for the PowerPC translator. Encodes into a
// word vector; branch targets are patched by the caller once known. GPLv3.
#pragma once
#include <cstdint>
#include <vector>
#include <cassert>

namespace a32 {

enum Cond { EQ = 0, NE, CS, CC, MI, PL, VS, VC, HI, LS, GE, LT, GT, LE, AL };
static inline Cond invert(Cond c) { return (Cond)(c ^ 1); }
enum { SP = 13, LR = 14, PC = 15, IP = 12 };
enum Shift { LSL = 0, LSR = 1, ASR = 2, ROR = 3 };
enum Op { AND = 0, EOR, SUB, RSB, ADD, ADC, SBC, RSC, TST, TEQ, CMP, CMN, ORR, MOV, BIC, MVN };

// modified immediate (8 bits rotated right by an even amount); returns false if v has no encoding
static inline bool modImm(uint32_t v, uint32_t &enc)
{
	for (unsigned r = 0; r < 16; r++) {
		uint32_t x = r ? (v << (2 * r)) | (v >> (32 - 2 * r)) : v;   // rotate left by 2r undoes "ror 2r"
		if (x <= 0xFF) { enc = (r << 8) | x; return true; }
	}
	return false;
}

struct Asm {
	std::vector<uint32_t> w;
	Cond cc = AL;                                    // condition applied to the next emitted instructions
	size_t pos() const { return w.size(); }
	void emit(uint32_t x) { w.push_back(((uint32_t)cc << 28) | (x & 0x0FFFFFFFu)); }

	// ---- data processing
	void dpi(Op op, bool s, int d, int n, uint32_t enc) { emit(0x02000000u | ((uint32_t)op << 21) | (s ? 1u << 20 : 0) | (n << 16) | (d << 12) | enc); }
	void dpr(Op op, bool s, int d, int n, int m, Shift t = LSL, unsigned sh = 0) {
		emit(((uint32_t)op << 21) | (s ? 1u << 20 : 0) | (n << 16) | (d << 12) | ((sh & 31) << 7) | ((uint32_t)t << 5) | m);
	}
	void dprs(Op op, bool s, int d, int n, int m, Shift t, int rs) {   // shift by register
		emit(((uint32_t)op << 21) | (s ? 1u << 20 : 0) | (n << 16) | (d << 12) | (rs << 8) | ((uint32_t)t << 5) | 0x10 | m);
	}
	bool tryImm(Op op, bool s, int d, int n, uint32_t v) { uint32_t e; if (!modImm(v, e)) return false; dpi(op, s, d, n, e); return true; }
	void mov(int d, int m) { dpr(MOV, false, d, 0, m); }
	void movs(int d, int m) { dpr(MOV, true, d, 0, m); }
	void add(int d, int n, int m) { dpr(ADD, false, d, n, m); }
	void adds(int d, int n, int m) { dpr(ADD, true, d, n, m); }
	void sub(int d, int n, int m) { dpr(SUB, false, d, n, m); }
	void subs(int d, int n, int m) { dpr(SUB, true, d, n, m); }
	void and_(int d, int n, int m) { dpr(AND, false, d, n, m); }
	void orr(int d, int n, int m) { dpr(ORR, false, d, n, m); }
	void eor(int d, int n, int m) { dpr(EOR, false, d, n, m); }
	void bic(int d, int n, int m) { dpr(BIC, false, d, n, m); }
	void mvn(int d, int m) { dpr(MVN, false, d, 0, m); }
	void cmp(int n, int m) { dpr(CMP, true, 0, n, m); }
	void tst(int n, int m) { dpr(TST, true, 0, n, m); }
	void lsli(int d, int m, unsigned s) { dpr(MOV, false, d, 0, m, LSL, s); }
	void lsri(int d, int m, unsigned s) { if (s == 0) mov(d, m); else dpr(MOV, false, d, 0, m, LSR, s & 31); assert(s < 32); }
	void asri(int d, int m, unsigned s) { if (s == 0) mov(d, m); else dpr(MOV, false, d, 0, m, ASR, s & 31); assert(s < 32); }
	void rori(int d, int m, unsigned s) { s &= 31; if (s == 0) mov(d, m); else dpr(MOV, false, d, 0, m, ROR, s); }
	void movw(int d, uint32_t v) { emit(0x03000000u | ((v >> 12 & 15) << 16) | (d << 12) | (v & 0xFFF)); }
	void movt(int d, uint32_t v) { emit(0x03400000u | ((v >> 12 & 15) << 16) | (d << 12) | (v & 0xFFF)); }
	void movi32(int d, uint32_t v) {
		uint32_t e;
		if (modImm(v, e)) { dpi(MOV, false, d, 0, e); return; }
		if (modImm(~v, e)) { dpi(MVN, false, d, 0, e); return; }
		movw(d, v & 0xFFFF); if (v >> 16) movt(d, v >> 16);
	}
	void mul(int d, int n, int m) { emit(0x00000090u | (d << 16) | (m << 8) | n); }
	void smull(int lo, int hi, int n, int m) { emit(0x00C00090u | (hi << 16) | (lo << 12) | (m << 8) | n); }
	void umull(int lo, int hi, int n, int m) { emit(0x00800090u | (hi << 16) | (lo << 12) | (m << 8) | n); }
	void sdiv(int d, int n, int m) { emit(0x0710F010u | (d << 16) | (m << 8) | n); }
	void udiv(int d, int n, int m) { emit(0x0730F010u | (d << 16) | (m << 8) | n); }
	void clz(int d, int m) { emit(0x016F0F10u | (d << 12) | m); }
	void rev(int d, int m) { emit(0x06BF0F30u | (d << 12) | m); }
	void rev16(int d, int m) { emit(0x06BF0FB0u | (d << 12) | m); }
	void sxtb(int d, int m) { emit(0x06AF0070u | (d << 12) | m); }
	void sxth(int d, int m) { emit(0x06BF0070u | (d << 12) | m); }
	void uxtb(int d, int m) { emit(0x06EF0070u | (d << 12) | m); }
	void uxth(int d, int m) { emit(0x06FF0070u | (d << 12) | m); }
	void ubfx(int d, int n, unsigned lsb, unsigned width) { emit(0x07E00050u | ((width - 1) << 16) | (d << 12) | (lsb << 7) | n); }
	void sbfx(int d, int n, unsigned lsb, unsigned width) { emit(0x07A00050u | ((width - 1) << 16) | (d << 12) | (lsb << 7) | n); }
	void bfi(int d, int n, unsigned lsb, unsigned width) { emit(0x07C00010u | ((lsb + width - 1) << 16) | (d << 12) | (lsb << 7) | n); }

	// ---- loads / stores. word/byte: imm12 (+/-), extra (half, signed, dual): imm8 (+/-)
	void ldst(bool load, bool byte, int t, int n, int32_t off) {
		bool u = off >= 0; uint32_t a = (uint32_t)(u ? off : -off); assert(a <= 0xFFF);
		emit(0x05000000u | (u ? 1u << 23 : 0) | (byte ? 1u << 22 : 0) | (load ? 1u << 20 : 0) | (n << 16) | (t << 12) | a);
	}
	void ldstr(bool load, bool byte, int t, int n, int m) { emit(0x07800000u | (byte ? 1u << 22 : 0) | (load ? 1u << 20 : 0) | (n << 16) | (t << 12) | m); }
	void ldr(int t, int n, int32_t off) { ldst(true, false, t, n, off); }
	void str(int t, int n, int32_t off) { ldst(false, false, t, n, off); }
	void ldrb(int t, int n, int32_t off) { ldst(true, true, t, n, off); }
	void strb(int t, int n, int32_t off) { ldst(false, true, t, n, off); }
	// extra: kind 0 strh 1 ldrh 2 ldrsb 3 ldrsh 4 ldrd 5 strd
	void xreg(int kind, int t, int n, int m) {
		static const uint32_t k[] = {0x000000B0u, 0x001000B0u, 0x001000D0u, 0x001000F0u, 0x000000D0u, 0x000000F0u};
		emit(0x01800000u | k[kind] | (n << 16) | (t << 12) | m);
	}
	void ximm(int kind, int t, int n, int32_t off) {
		static const uint32_t k[] = {0x000000B0u, 0x001000B0u, 0x001000D0u, 0x001000F0u, 0x000000D0u, 0x000000F0u};
		bool u = off >= 0; uint32_t a = (uint32_t)(u ? off : -off); assert(a <= 0xFF);
		emit(0x01400000u | (u ? 1u << 23 : 0) | k[kind] | (n << 16) | (t << 12) | ((a >> 4) << 8) | (a & 15));
	}
	void push(uint32_t list) { emit(0x092D0000u | list); }
	void pop(uint32_t list) { emit(0x08BD0000u | list); }

	// ---- branches (offsets patched later)
	size_t b(Cond c = AL) { Cond o = cc; cc = c; emit(0x0A000000u); cc = o; return pos() - 1; }
	size_t bl() { emit(0x0B000000u); return pos() - 1; }
	void blx(int m) { emit(0x012FFF30u | m); }
	void bx(int m) { emit(0x012FFF10u | m); }
	void patch(size_t at, size_t to) {
		int32_t off = (int32_t)to - (int32_t)at - 2;
		w[at] = (w[at] & 0xFF000000u) | ((uint32_t)off & 0x00FFFFFFu);
	}

	// ---- VFP (double registers d0..d15, single s0..s31)
	void vldst(bool load, bool dbl, int v, int n, int32_t off) {
		bool u = off >= 0; uint32_t a = (uint32_t)(u ? off : -off); assert((a & 3) == 0 && a <= 1020);
		uint32_t vd = dbl ? (uint32_t)v : (uint32_t)v >> 1, D = dbl ? 0 : (v & 1);
		emit(0x0D000A00u | (u ? 1u << 23 : 0) | (D << 22) | (load ? 1u << 20 : 0) | (n << 16) | (vd << 12) | (dbl ? 0x100 : 0) | (a >> 2));
	}
	void vldrd(int d, int n, int32_t off) { vldst(true, true, d, n, off); }
	void vstrd(int d, int n, int32_t off) { vldst(false, true, d, n, off); }
	void vldrs(int s, int n, int32_t off) { vldst(true, false, s, n, off); }
	void vstrs(int s, int n, int32_t off) { vldst(false, false, s, n, off); }
	void vdp(uint32_t base, int d, int n, int m) { emit(base | (n << 16) | (d << 12) | m); }
	void vadd(int d, int n, int m) { vdp(0x0E300B00u, d, n, m); }
	void vsub(int d, int n, int m) { vdp(0x0E300B40u, d, n, m); }
	void vmul(int d, int n, int m) { vdp(0x0E200B00u, d, n, m); }
	void vdiv(int d, int n, int m) { vdp(0x0E800B00u, d, n, m); }
	void vfma(int d, int n, int m) { vdp(0x0EA00B00u, d, n, m); }    // d += n*m (fused)
	void vfms(int d, int n, int m) { vdp(0x0EA00B40u, d, n, m); }    // d -= n*m (fused)
	void vfnms(int d, int n, int m) { vdp(0x0E900B00u, d, n, m); }   // d = n*m - d (fused)
	void vfnma(int d, int n, int m) { vdp(0x0E900B40u, d, n, m); }   // d = -d - n*m (fused)
	void vmovd(int d, int m) { emit(0x0EB00B40u | (d << 12) | m); }
	void vneg(int d, int m) { emit(0x0EB10B40u | (d << 12) | m); }
	void vabs(int d, int m) { emit(0x0EB00BC0u | (d << 12) | m); }
	void vsqrt(int d, int m) { emit(0x0EB10BC0u | (d << 12) | m); }
	void vcmp(int d, int m) { emit(0x0EB40B40u | (d << 12) | m); }
	void vcmpz(int d) { emit(0x0EB50B40u | (d << 12)); }
	void vmrs() { emit(0x0EF1FA10u); }                                  // APSR_nzcv = FPSCR flags
	void vcvt_ds(int d, int s) { emit(0x0EB70AC0u | (d << 12) | ((s & 1) << 5) | (s >> 1)); }   // d = (double) s
	void vcvt_sd(int s, int d) { emit(0x0EB70BC0u | ((s & 1) << 22) | ((s >> 1) << 12) | d); }  // s = (float) d
	void vcvt_s32d(int s, int d, bool rz) { emit(0x0EBD0B40u | (rz ? 0x80 : 0) | ((s & 1) << 22) | ((s >> 1) << 12) | d); }
	void vmov_rs(int t, int s) { emit(0x0E100A10u | ((s >> 1) << 16) | (t << 12) | ((s & 1) << 7)); }   // core <- single
	void vmov_sr(int s, int t) { emit(0x0E000A10u | ((s >> 1) << 16) | (t << 12) | ((s & 1) << 7)); }   // single <- core
	void vmov_rrd(int lo, int hi, int d) { emit(0x0C500B10u | (hi << 16) | (lo << 12) | d); }   // core pair <- double
	void vmov_drr(int d, int lo, int hi) { emit(0x0C400B10u | (hi << 16) | (lo << 12) | d); }   // double <- core pair
};

}
