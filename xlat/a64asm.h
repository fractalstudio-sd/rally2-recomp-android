// Minimal AArch64 assembler for the PowerPC translator. Encodes into a word vector; labels are resolved by the caller
// through Fixup records (branch offsets are patched once the target position is known). GPLv3.
#pragma once
#include <cstdint>
#include <vector>
#include <cassert>

namespace a64 {

enum Cond { EQ = 0, NE, CS, CC, MI, PL, VS, VC, HI, LS, GE, LT, GT, LE, AL };
static inline Cond invert(Cond c) { return (Cond)(c ^ 1); }

// Register numbers: 0..30, 31 = zr or sp depending on the instruction.
enum { ZR = 31, SP = 31 };

// Logical immediate encoder (N:immr:imms). Returns false if the value has no encoding.
static inline bool logImm(uint64_t v, bool is64, uint32_t &enc)
{
	if (!is64) { v &= 0xFFFFFFFFull; v |= v << 32; }
	if (v == 0 || v == ~0ull) return false;
	unsigned size = 64;
	while (size > 2) {
		unsigned h = size / 2; uint64_t mask = (h == 64) ? ~0ull : ((1ull << h) - 1);
		if ((v & mask) != ((v >> h) & mask)) break;
		size = h;
	}
	uint64_t mask = size == 64 ? ~0ull : ((1ull << size) - 1);
	uint64_t e = v & mask;
	// rotate so that the run of ones starts at bit 0
	unsigned ones = __builtin_popcountll(e);
	unsigned rot = 0;
	for (rot = 0; rot < size; rot++) {
		uint64_t r = ((e >> rot) | (e << (size - rot))) & mask;
		if (rot == 0) r = e;
		if (r == ((ones == 64) ? ~0ull : ((1ull << ones) - 1))) break;
	}
	if (rot == size) return false;
	unsigned immr = (size - rot) % size;
	unsigned imms = ((~(size - 1) << 1) & 0x3F) | (ones - 1);
	unsigned N = size == 64 ? 1 : 0;
	if (!is64 && N) return false;
	enc = (N << 12) | (immr << 6) | (imms & 0x3F);
	return true;
}

struct Asm {
	std::vector<uint32_t> w;
	size_t pos() const { return w.size(); }
	void emit(uint32_t x) { w.push_back(x); }

	// ---- data processing (immediate)
	void addi(bool x, int d, int n, uint32_t imm, bool setf = false) { emitAddSubImm(x, false, setf, d, n, imm); }
	void subi(bool x, int d, int n, uint32_t imm, bool setf = false) { emitAddSubImm(x, true, setf, d, n, imm); }
	void cmpi(bool x, int n, uint32_t imm) { subi(x, ZR, n, imm, true); }
	void cmni(bool x, int n, uint32_t imm) { addi(x, ZR, n, imm, true); }
	void emitAddSubImm(bool x, bool sub, bool s, int d, int n, uint32_t imm) {
		uint32_t sh = 0; if (imm > 0xFFF) { assert((imm & 0xFFF) == 0 && imm <= 0xFFF000); imm >>= 12; sh = 1; }
		emit((x ? 1u << 31 : 0) | (sub ? 1u << 30 : 0) | (s ? 1u << 29 : 0) | 0x11000000u | (sh << 22) | (imm << 10) | (n << 5) | d);
	}
	bool logi(bool x, int opc, int d, int n, uint64_t imm) {   // opc 0 and 1 orr 2 eor 3 ands
		uint32_t e; if (!logImm(imm, x, e)) return false;
		emit((x ? 1u << 31 : 0) | ((uint32_t)opc << 29) | 0x12000000u | (e << 10) | (n << 5) | d); return true;
	}
	void movz(bool x, int d, uint32_t imm16, int sh) { emit((x ? 1u << 31 : 0) | 0x52800000u | ((uint32_t)(sh / 16) << 21) | (imm16 << 5) | d); }
	void movk(bool x, int d, uint32_t imm16, int sh) { emit((x ? 1u << 31 : 0) | 0x72800000u | ((uint32_t)(sh / 16) << 21) | (imm16 << 5) | d); }
	void movn(bool x, int d, uint32_t imm16, int sh) { emit((x ? 1u << 31 : 0) | 0x12800000u | ((uint32_t)(sh / 16) << 21) | (imm16 << 5) | d); }
	void movi32(int d, uint32_t v) {
		if ((v & 0xFFFF0000u) == 0) { movz(false, d, v, 0); return; }
		if ((v & 0xFFFF) == 0) { movz(false, d, v >> 16, 16); return; }
		if ((v | 0xFFFF) == 0xFFFFFFFFu) { movn(false, d, ~v & 0xFFFF, 0); return; }
		if ((v | 0xFFFF0000u) == 0xFFFFFFFFu) { movn(false, d, (~v >> 16) & 0xFFFF, 16); return; }
		if (logi(false, 1, d, ZR, v)) return;
		movz(false, d, v & 0xFFFF, 0); movk(false, d, v >> 16, 16);
	}
	void movi64(int d, uint64_t v) {
		bool first = true;
		for (int s = 0; s < 64; s += 16) {
			uint32_t h = (v >> s) & 0xFFFF;
			if (!h) continue;
			if (first) { movz(true, d, h, s); first = false; } else movk(true, d, h, s);
		}
		if (first) movz(true, d, 0, 0);
	}
	// ---- data processing (register)
	enum Shift { LSL = 0, LSR = 1, ASR = 2, ROR = 3 };
	void addsub(bool x, bool sub, bool s, int d, int n, int m, Shift sh = LSL, int amt = 0) {
		emit((x ? 1u << 31 : 0) | (sub ? 1u << 30 : 0) | (s ? 1u << 29 : 0) | 0x0B000000u | ((uint32_t)sh << 22) | (m << 16) | ((uint32_t)amt << 10) | (n << 5) | d);
	}
	void add(bool x, int d, int n, int m) { addsub(x, false, false, d, n, m); }
	void sub(bool x, int d, int n, int m) { addsub(x, true, false, d, n, m); }
	void adds(bool x, int d, int n, int m) { addsub(x, false, true, d, n, m); }
	void subs(bool x, int d, int n, int m) { addsub(x, true, true, d, n, m); }
	void cmp(bool x, int n, int m) { subs(x, ZR, n, m); }
	void cmn(bool x, int n, int m) { adds(x, ZR, n, m); }
	void neg(bool x, int d, int m) { sub(x, d, ZR, m); }
	void adcsbc(bool x, bool sub, bool s, int d, int n, int m) { emit((x ? 1u << 31 : 0) | (sub ? 1u << 30 : 0) | (s ? 1u << 29 : 0) | 0x1A000000u | (m << 16) | (n << 5) | d); }
	// logical (shifted register): opc 0 and 1 orr 2 eor 3 ands, N inverts m (bic/orn/eon/bics)
	void logr(bool x, int opc, bool N, int d, int n, int m, Shift sh = LSL, int amt = 0) {
		emit((x ? 1u << 31 : 0) | ((uint32_t)opc << 29) | 0x0A000000u | ((uint32_t)sh << 22) | (N ? 1u << 21 : 0) | (m << 16) | ((uint32_t)amt << 10) | (n << 5) | d);
	}
	void and_(bool x, int d, int n, int m) { logr(x, 0, false, d, n, m); }
	void orr(bool x, int d, int n, int m) { logr(x, 1, false, d, n, m); }
	void eor(bool x, int d, int n, int m) { logr(x, 2, false, d, n, m); }
	void ands(bool x, int d, int n, int m) { logr(x, 3, false, d, n, m); }
	void bic(bool x, int d, int n, int m) { logr(x, 0, true, d, n, m); }
	void orn(bool x, int d, int n, int m) { logr(x, 1, true, d, n, m); }
	void eon(bool x, int d, int n, int m) { logr(x, 2, true, d, n, m); }
	void mov(bool x, int d, int m) { orr(x, d, ZR, m); }
	void mvn(bool x, int d, int m) { orn(x, d, ZR, m); }
	void tst(bool x, int n, int m) { ands(x, ZR, n, m); }
	void tsti(bool x, int n, uint64_t imm) { bool ok = logi(x, 3, ZR, n, imm); assert(ok); (void)ok; }
	// variable shifts
	void shiftv(bool x, int op2, int d, int n, int m) { emit((x ? 1u << 31 : 0) | 0x1AC02000u | (m << 16) | ((uint32_t)op2 << 10) | (n << 5) | d); }
	void lslv(bool x, int d, int n, int m) { shiftv(x, 0, d, n, m); }
	void lsrv(bool x, int d, int n, int m) { shiftv(x, 1, d, n, m); }
	void asrv(bool x, int d, int n, int m) { shiftv(x, 2, d, n, m); }
	void rorv(bool x, int d, int n, int m) { shiftv(x, 3, d, n, m); }
	// bitfield
	void bfm(bool x, int opc, int d, int n, int immr, int imms) {   // opc 0 sbfm 1 bfm 2 ubfm
		emit((x ? 1u << 31 : 0) | ((uint32_t)opc << 29) | 0x13000000u | (x ? 1u << 22 : 0) | ((uint32_t)immr << 16) | ((uint32_t)imms << 10) | (n << 5) | d);
	}
	void lsli(bool x, int d, int n, int s) { int sz = x ? 64 : 32; bfm(x, 2, d, n, (sz - s) % sz, sz - 1 - s); }
	void lsri(bool x, int d, int n, int s) { bfm(x, 2, d, n, s, x ? 63 : 31); }
	void asri(bool x, int d, int n, int s) { bfm(x, 0, d, n, s, x ? 63 : 31); }
	void ubfx(bool x, int d, int n, int lsb, int width) { bfm(x, 2, d, n, lsb, lsb + width - 1); }
	void sbfx(bool x, int d, int n, int lsb, int width) { bfm(x, 0, d, n, lsb, lsb + width - 1); }
	void bfi(bool x, int d, int n, int lsb, int width) { int sz = x ? 64 : 32; bfm(x, 1, d, n, (sz - lsb) % sz, width - 1); }
	void bfxil(bool x, int d, int n, int lsb, int width) { bfm(x, 1, d, n, lsb, lsb + width - 1); }
	void sxtb(bool x, int d, int n) { bfm(x, 0, d, n, 0, 7); }
	void sxth(bool x, int d, int n) { bfm(x, 0, d, n, 0, 15); }
	void sxtw(int d, int n) { bfm(true, 0, d, n, 0, 31); }
	void uxtb(int d, int n) { bfm(false, 2, d, n, 0, 7); }
	void uxth(int d, int n) { bfm(false, 2, d, n, 0, 15); }
	void extr(bool x, int d, int n, int m, int lsb) { emit((x ? 0x93C00000u : 0x13800000u) | (m << 16) | ((uint32_t)lsb << 10) | (n << 5) | d); }
	void rori(bool x, int d, int n, int s) { extr(x, d, n, n, s); }
	// multiply / divide / misc
	void madd(bool x, int d, int n, int m, int a) { emit((x ? 1u << 31 : 0) | 0x1B000000u | (m << 16) | (a << 10) | (n << 5) | d); }
	void msub(bool x, int d, int n, int m, int a) { emit((x ? 1u << 31 : 0) | 0x1B008000u | (m << 16) | (a << 10) | (n << 5) | d); }
	void mul(bool x, int d, int n, int m) { madd(x, d, n, m, ZR); }
	void smull(int d, int n, int m) { emit(0x9B207C00u | (m << 16) | (n << 5) | d); }
	void umull(int d, int n, int m) { emit(0x9BA07C00u | (m << 16) | (n << 5) | d); }
	void sdiv(bool x, int d, int n, int m) { emit((x ? 1u << 31 : 0) | 0x1AC00C00u | (m << 16) | (n << 5) | d); }
	void udiv(bool x, int d, int n, int m) { emit((x ? 1u << 31 : 0) | 0x1AC00800u | (m << 16) | (n << 5) | d); }
	void clz(bool x, int d, int n) { emit((x ? 1u << 31 : 0) | 0x5AC01000u | (n << 5) | d); }
	void rev(bool x, int d, int n) { emit(x ? 0xDAC00C00u | (n << 5) | d : 0x5AC00800u | (n << 5) | d); }
	void rev16(bool x, int d, int n) { emit((x ? 1u << 31 : 0) | 0x5AC00400u | (n << 5) | d); }
	void csel(bool x, int d, int n, int m, Cond c) { emit((x ? 1u << 31 : 0) | 0x1A800000u | (m << 16) | ((uint32_t)c << 12) | (n << 5) | d); }
	void csinc(bool x, int d, int n, int m, Cond c) { emit((x ? 1u << 31 : 0) | 0x1A800400u | (m << 16) | ((uint32_t)c << 12) | (n << 5) | d); }
	void csinv(bool x, int d, int n, int m, Cond c) { emit((x ? 1u << 31 : 0) | 0x5A800000u | (m << 16) | ((uint32_t)c << 12) | (n << 5) | d); }
	void cset(bool x, int d, Cond c) { csinc(x, d, ZR, ZR, invert(c)); }
	void csetm(bool x, int d, Cond c) { csinv(x, d, ZR, ZR, invert(c)); }
	// ---- loads / stores (unsigned scaled immediate offset)
	// size: 0 byte 1 half 2 word 3 dword; opc: 0 store 1 load 2 load signed (64-bit dest) 3 load signed (32-bit dest)
	void ldst(int size, int opc, int t, int n, uint32_t off) {
		assert((off & ((1u << size) - 1)) == 0 && (off >> size) < 4096);
		emit(((uint32_t)size << 30) | 0x39000000u | ((uint32_t)opc << 22) | ((off >> size) << 10) | (n << 5) | t);
	}
	// register offset: Rm extended (UXTW = 2, LSL/UXTX = 3), S = scale by size
	void ldstr(int size, int opc, int t, int n, int m, int option = 3, bool S = false) {
		emit(((uint32_t)size << 30) | 0x38200800u | ((uint32_t)opc << 22) | (m << 16) | ((uint32_t)option << 13) | (S ? 1u << 12 : 0) | (n << 5) | t);
	}
	void ldrw(int t, int n, uint32_t off) { ldst(2, 1, t, n, off); }
	void strw(int t, int n, uint32_t off) { ldst(2, 0, t, n, off); }
	void ldrx(int t, int n, uint32_t off) { ldst(3, 1, t, n, off); }
	void strx(int t, int n, uint32_t off) { ldst(3, 0, t, n, off); }
	void ldrb(int t, int n, uint32_t off) { ldst(0, 1, t, n, off); }
	void strb(int t, int n, uint32_t off) { ldst(0, 0, t, n, off); }
	// pair, pre/post index (64-bit regs)
	void stp_pre(int t1, int t2, int n, int imm) { emit(0xA9800000u | ((uint32_t)((imm / 8) & 0x7F) << 15) | (t2 << 10) | (n << 5) | t1); }
	void ldp_post(int t1, int t2, int n, int imm) { emit(0xA8C00000u | ((uint32_t)((imm / 8) & 0x7F) << 15) | (t2 << 10) | (n << 5) | t1); }
	void stp(int t1, int t2, int n, int imm) { emit(0xA9000000u | ((uint32_t)((imm / 8) & 0x7F) << 15) | (t2 << 10) | (n << 5) | t1); }
	void ldp(int t1, int t2, int n, int imm) { emit(0xA9400000u | ((uint32_t)((imm / 8) & 0x7F) << 15) | (t2 << 10) | (n << 5) | t1); }
	// ---- FP / SIMD scalar loads and stores: size 2 = S, 3 = D
	void fldst(int size, bool load, int t, int n, uint32_t off) {
		assert((off & ((1u << size) - 1)) == 0 && (off >> size) < 4096);
		emit(((uint32_t)size << 30) | 0x3D000000u | (load ? 1u << 22 : 0) | ((off >> size) << 10) | (n << 5) | t);
	}
	void ldrd(int t, int n, uint32_t off) { fldst(3, true, t, n, off); }
	void strd(int t, int n, uint32_t off) { fldst(3, false, t, n, off); }
	// ---- FP arithmetic, type 1 = double, 0 = single
	void fp2(int type, int opcode, int d, int n, int m) { emit(0x1E200800u | ((uint32_t)type << 22) | (m << 16) | ((uint32_t)opcode << 12) | (n << 5) | d); }
	void fmul(int t, int d, int n, int m) { fp2(t, 0, d, n, m); }
	void fdiv(int t, int d, int n, int m) { fp2(t, 1, d, n, m); }
	void fadd(int t, int d, int n, int m) { fp2(t, 2, d, n, m); }
	void fsub(int t, int d, int n, int m) { fp2(t, 3, d, n, m); }
	void fp1(int type, int opcode, int d, int n) { emit(0x1E204000u | ((uint32_t)type << 22) | ((uint32_t)opcode << 15) | (n << 5) | d); }
	void fmovr(int t, int d, int n) { fp1(t, 0, d, n); }
	void fabs_(int t, int d, int n) { fp1(t, 1, d, n); }
	void fneg(int t, int d, int n) { fp1(t, 2, d, n); }
	void fsqrt(int t, int d, int n) { fp1(t, 3, d, n); }
	void fcvt_sd(int d, int n) { fp1(0, 5, d, n); }   // single -> double
	void fcvt_ds(int d, int n) { fp1(1, 4, d, n); }   // double -> single
	void fp3(int type, bool o1, bool o0, int d, int n, int m, int a) { emit(0x1F000000u | ((uint32_t)type << 22) | (o1 ? 1u << 21 : 0) | (m << 16) | (o0 ? 1u << 15 : 0) | (a << 10) | (n << 5) | d); }
	void fmadd(int t, int d, int n, int m, int a) { fp3(t, false, false, d, n, m, a); }    // a + n*m
	void fmsub(int t, int d, int n, int m, int a) { fp3(t, false, true, d, n, m, a); }     // a - n*m
	void fnmadd(int t, int d, int n, int m, int a) { fp3(t, true, false, d, n, m, a); }    // -a - n*m
	void fnmsub(int t, int d, int n, int m, int a) { fp3(t, true, true, d, n, m, a); }     // -a + n*m
	void fcmp(int t, int n, int m) { emit(0x1E202000u | ((uint32_t)t << 22) | (m << 16) | (n << 5)); }
	void fcmpz(int t, int n) { emit(0x1E202008u | ((uint32_t)t << 22) | (n << 5)); }
	void fcsel(int t, int d, int n, int m, Cond c) { emit(0x1E200C00u | ((uint32_t)t << 22) | (m << 16) | ((uint32_t)c << 12) | (n << 5) | d); }
	// moves between general and FP registers
	void fmov_w2s(int d, int n) { emit(0x1E270000u | (n << 5) | d); }
	void fmov_s2w(int d, int n) { emit(0x1E260000u | (n << 5) | d); }
	void fmov_x2d(int d, int n) { emit(0x9E670000u | (n << 5) | d); }
	void fmov_d2x(int d, int n) { emit(0x9E660000u | (n << 5) | d); }
	// conversions double -> int64: rmode 3 = toward zero (fcvtzs), 0 = nearest even (fcvtns)
	void fcvtzs_xd(int d, int n) { emit(0x9E780000u | (n << 5) | d); }
	void fcvtns_xd(int d, int n) { emit(0x9E600000u | (n << 5) | d); }
	// ---- branches (offsets patched later)
	size_t b() { emit(0x14000000u); return pos() - 1; }
	size_t bl() { emit(0x94000000u); return pos() - 1; }
	size_t bcond(Cond c) { emit(0x54000000u | (uint32_t)c); return pos() - 1; }
	size_t cbz(bool x, int t) { emit((x ? 1u << 31 : 0) | 0x34000000u | t); return pos() - 1; }
	size_t cbnz(bool x, int t) { emit((x ? 1u << 31 : 0) | 0x35000000u | t); return pos() - 1; }
	size_t tbz(int t, int bit) { emit((bit >= 32 ? 1u << 31 : 0) | 0x36000000u | ((uint32_t)(bit & 31) << 19) | t); return pos() - 1; }
	size_t tbnz(int t, int bit) { emit((bit >= 32 ? 1u << 31 : 0) | 0x37000000u | ((uint32_t)(bit & 31) << 19) | t); return pos() - 1; }
	void br(int n) { emit(0xD61F0000u | (n << 5)); }
	void blr(int n) { emit(0xD63F0000u | (n << 5)); }
	void ret() { emit(0xD65F03C0u); }
	void nop() { emit(0xD503201Fu); }
	// patch the branch at index `at` to jump to word index `to`
	void patch(size_t at, size_t to) {
		int64_t off = (int64_t)to - (int64_t)at; uint32_t &x = w[at];
		if ((x & 0x7C000000u) == 0x14000000u) { assert(off >= -(1 << 25) && off < (1 << 25)); x = (x & 0xFC000000u) | ((uint32_t)off & 0x03FFFFFFu); }
		else if ((x & 0xFF000010u) == 0x54000000u || (x & 0x7E000000u) == 0x34000000u) { assert(off >= -(1 << 18) && off < (1 << 18)); x = (x & 0xFF00001Fu) | (((uint32_t)off & 0x7FFFFu) << 5); }
		else if ((x & 0x7E000000u) == 0x36000000u) { assert(off >= -(1 << 13) && off < (1 << 13)); x = (x & 0xFFF8001Fu) | (((uint32_t)off & 0x3FFFu) << 5); }
		else assert(!"not a branch");
	}
};

}
