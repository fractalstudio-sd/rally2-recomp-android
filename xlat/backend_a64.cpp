// PowerPC 603e -> AArch64 code generator. Same semantics as the reference C lifter (lifter/lifter.py): one native function per
// guest function, guest calls are native calls, blr returns, cycles are counted per block with a safepoint at function
// entries and loop heads. Guest state lives in the CPU struct (x19); main RAM is accessed directly (x20) with an
// out-of-line slow path through the bus for everything else; runtime services are reached through a table (x21). GPLv3.
#include "backend_a64.h"
extern "C" {
#include "ppc_rt.h"
}
#include <cstddef>
#include <cstdio>
#include <algorithm>

namespace xl {
using namespace a64;

static const int RC = 19, RRAM = 20, RH = 21;
#define OFF_R(n) ((uint32_t)(offsetof(CPU, r) + 4 * (n)))
#define OFF_LR ((uint32_t)offsetof(CPU, lr))
#define OFF_CTR ((uint32_t)offsetof(CPU, ctr))
#define OFF_XER ((uint32_t)offsetof(CPU, xer))
#define OFF_MSR ((uint32_t)offsetof(CPU, msr))
#define OFF_FPSCR ((uint32_t)offsetof(CPU, fpscr))
#define OFF_CR(n) ((uint32_t)(offsetof(CPU, cr) + (n)))
#define OFF_F(n) ((uint32_t)(offsetof(CPU, f) + 8 * (n)))
#define OFF_SPR(n) ((uint32_t)(offsetof(CPU, spr) + 4 * (n)))
#define OFF_SR(n) ((uint32_t)(offsetof(CPU, sr) + 4 * (n)))
#define OFF_CYC ((uint32_t)offsetof(CPU, cyc))

static inline int32_t sx16(uint32_t v) { return (int32_t)(int16_t)(v & 0xFFFF); }
static inline uint32_t mask32(unsigned mb, unsigned me) {
	if (mb <= me) return (0xFFFFFFFFu >> mb) & (0xFFFFFFFFu << (31 - me));
	return ~mask32(me + 1, mb - 1);
}

struct Slow { size_t at; size_t back; int kind; int size; bool sign; int reg; };   // out-of-line memory access

class FnGen {
public:
	FnGen(Program &p, A64Code &o, const Function &f, const std::set<uint32_t> &entries) : P(p), O(o), A(o.a), F(f), E(entries) { for (int &h : host) h = -1; }
	void run();
	// register allocation: a first (recording) pass counts guest register uses, the real pass keeps the busiest ones
	// in x23..x28 for the whole function (written back before calls / at returns, reloaded after calls)
	const std::set<uint32_t> *H = nullptr;
	bool rec = false; double wt = 1; std::map<int, double> useW; std::set<int> written; double syncW = 0;
	int host[32]; std::vector<int> assigned; std::set<uint32_t> loopPcs;
	// compare + branch fusion: NZCV still holds the last compare of this CR field (nothing in between)
	int flagsField = -1; bool flagsSigned = true;
	// guest registers holding a known constant inside the current block (lis/addi/ori address building)
	std::map<int, uint32_t> kconst; bool eaConst = false; uint32_t eaVal = 0;
private:
	Program &P; A64Code &O; Asm &A; const Function &F; const std::set<uint32_t> &E;
	std::set<uint32_t> vis, labels, backs, blocks;
	std::map<uint32_t, size_t> labelPos;
	std::vector<std::pair<size_t, uint32_t>> jumps;     // branch at -> guest label
	std::vector<size_t> toEpilogue;
	std::vector<Slow> slows;
	std::map<uint32_t, int> cost;

	// ---- small helpers
	void ldg(int w, int r) {
		if (rec) useW[r] += wt;
		if (host[r] >= 0) A.mov(false, w, host[r]); else A.ldrw(w, RC, OFF_R(r));
	}
	void stg(int w, int r) {
		if (rec) { useW[r] += wt; written.insert(r); }
		kconst.erase(r);
		if (host[r] >= 0) A.mov(false, host[r], w); else A.strw(w, RC, OFF_R(r));
	}
	void flushRegs() { for (int r : assigned) if (written.count(r)) A.strw(host[r], RC, OFF_R(r)); }
	void reloadRegs() { for (int r : assigned) A.ldrw(host[r], RC, OFF_R(r)); }
	// runtime services: those that can run other guest code (interrupts, task switches) see the guest registers in
	// memory; all but the CR/FPSCR helpers may read the cycle counter, kept in w22 while translated code runs
	void helper(int slot) {
		bool guest = slot == H_SAFEPOINT || slot == H_SC || slot == H_RFI || slot == H_CALL || slot == H_UNIMPL;
		bool cyc = !(slot == H_MFCR || slot == H_MTCRF || slot == H_MTFSF);
		if (rec && guest) syncW += wt;
		if (guest) { flushRegs(); kconst.clear(); }
		if (cyc) A.strw(22, RC, OFF_CYC);
		A.ldrx(16, RH, 8 * slot); A.blr(16);
		if (cyc) A.ldrw(22, RC, OFF_CYC);
		if (guest) reloadRegs();
	}
	void movw(int w, uint32_t v) { A.movi32(w, v); }
	void addimm(int d, int n, int32_t imm) {             // w-register add of a signed 32-bit constant
		if (imm == 0) { if (d != n) A.mov(false, d, n); return; }
		uint32_t u = (uint32_t)(imm < 0 ? -(int64_t)imm : imm);
		if (u <= 0xFFF) { if (imm > 0) A.addi(false, d, n, u); else A.subi(false, d, n, u); return; }
		if ((u & 0xFFF) == 0 && u <= 0xFFF000) { if (imm > 0) A.addi(false, d, n, u); else A.subi(false, d, n, u); return; }
		movw(17, (uint32_t)imm); A.add(false, d, n, 17);
	}
	void andimm(int d, int n, uint32_t m) {
		if (m == 0xFFFFFFFFu) { if (d != n) A.mov(false, d, n); return; }
		if (m == 0) { movw(d, 0); return; }
		if (!A.logi(false, 0, d, n, m)) { movw(17, m); A.and_(false, d, n, 17); }
	}
	void orrimm(int d, int n, uint32_t m) { if (!m) { if (d != n) A.mov(false, d, n); return; } if (!A.logi(false, 1, d, n, m)) { movw(17, m); A.orr(false, d, n, 17); } }
	void eorimm(int d, int n, uint32_t m) { if (!m) { if (d != n) A.mov(false, d, n); return; } if (!A.logi(false, 2, d, n, m)) { movw(17, m); A.eor(false, d, n, 17); } }
	// cr0 = cmp_s((int32)wv, 0) | SO
	void setcr0(int wv) { cmpNibble(0, [&] { A.cmpi(false, wv, 0); }, true); }
	// emit compare, then write the 4-bit field (LT 8 GT 4 EQ 2 | SO from XER bit 31)
	template <class F> void cmpNibble(int field, F cmp, bool signedCmp) {
		cmp();
		A.movz(false, 1, 2, 0);
		A.movz(false, 2, 8, 0); A.csel(false, 1, 2, 1, signedCmp ? LT : CC);
		A.movz(false, 2, 4, 0); A.csel(false, 1, 2, 1, signedCmp ? GT : HI);
		A.ldrw(2, RC, OFF_XER); A.lsri(false, 2, 2, 31); A.orr(false, 1, 1, 2);
		A.strb(1, RC, OFF_CR(field));
		flagsField = field; flagsSigned = signedCmp;
	}
	void setCA(int wca) {                                 // XER.CA = wca (0/1)
		A.ldrw(3, RC, OFF_XER); A.bfi(false, 3, wca, 29, 1); A.strw(3, RC, OFF_XER);
	}
	void getCA(int w) { A.ldrw(w, RC, OFF_XER); A.ubfx(false, w, w, 29, 1); }
	void crbit(int w, int bi) { A.ldrb(w, RC, OFF_CR(bi >> 2)); A.ubfx(false, w, w, 3 - (bi & 3), 1); }
	void setcrbit(int bi, int wv) { A.ldrb(3, RC, OFF_CR(bi >> 2)); A.bfi(false, 3, wv, 3 - (bi & 3), 1); A.strb(3, RC, OFF_CR(bi >> 2)); }
	// effective address d(ra) / (ra|0)+rb into w0
	void eaD(uint32_t ra, int32_t d) {
		eaConst = false;
		if (ra == 0) { eaConst = true; eaVal = (uint32_t)d; movw(0, eaVal); return; }
		auto k = kconst.find((int)ra);
		if (k != kconst.end()) { eaConst = true; eaVal = k->second + (uint32_t)d; movw(0, eaVal); return; }
		ldg(0, ra); addimm(0, 0, d);
	}
	void eaX(uint32_t ra, uint32_t rb) { eaConst = false; if (ra == 0) ldg(0, rb); else { ldg(0, ra); ldg(1, rb); A.add(false, 0, 0, 1); } }
	// memory: ea in w0. Loads leave the value in w2 (x2 for 64-bit); stores take it in w2/x2.
	void load(int size, bool sign) {
		uint32_t m = size == 0 ? 0xFF800000u : size == 1 ? 0xFF800001u : 0xFF800003u;
		bool known = eaConst && !(eaVal & m) && (size < 3 || eaVal < 0x7FFFF8u); eaConst = false;
		size_t br = 0;
		if (!known) { A.tsti(false, 0, m); br = A.bcond(NE); }
		if (size == 0) { A.logi(false, 2, 1, 0, 3); A.ldstr(0, sign ? 3 : 1, 2, RRAM, 1, 2); }
		else if (size == 1) { A.logi(false, 2, 1, 0, 2); A.ldstr(1, sign ? 3 : 1, 2, RRAM, 1, 2); }
		else if (size == 2) A.ldstr(2, 1, 2, RRAM, 0, 2);
		else { A.ldstr(3, 1, 2, RRAM, 0, 2); A.rori(true, 2, 2, 32); }
		if (!known) slows.push_back({br, A.pos(), 0, size, sign, 2});
	}
	void store(int size) {
		uint32_t m = size == 0 ? 0xFF800000u : size == 1 ? 0xFF800001u : 0xFF800003u;
		bool known = eaConst && !(eaVal & m) && (size < 3 || eaVal < 0x7FFFF8u); eaConst = false;
		size_t br = 0;
		if (!known) { A.tsti(false, 0, m); br = A.bcond(NE); }
		if (size == 0) { A.logi(false, 2, 1, 0, 3); A.ldstr(0, 0, 2, RRAM, 1, 2); }
		else if (size == 1) { A.logi(false, 2, 1, 0, 2); A.ldstr(1, 0, 2, RRAM, 1, 2); }
		else if (size == 2) A.ldstr(2, 0, 2, RRAM, 0, 2);
		else { A.rori(true, 3, 2, 32); A.ldstr(3, 0, 3, RRAM, 0, 2); }
		if (!known) slows.push_back({br, A.pos(), 1, size, false, 2});
	}
	void emitSlows() {
		for (auto &s : slows) {
			A.patch(s.at, A.pos());
			if (s.kind == 0) {
				A.mov(true, 1, 0); A.mov(true, 0, RC); helper(H_LD8 + s.size);
				if (s.size == 3) A.mov(true, 2, 0);
				else if (s.sign && s.size == 1) A.sxth(false, 2, 0);
				else if (s.sign && s.size == 0) A.sxtb(false, 2, 0);
				else A.mov(false, 2, 0);
			} else {
				A.mov(true, 1, 0); A.mov(s.size == 3, 2, 2); A.mov(true, 0, RC); helper(H_ST8 + s.size);
			}
			size_t b = A.b(); A.patch(b, s.back);
		}
	}
	// control flow
	void gotoLabel(uint32_t t) { jumps.push_back({A.b(), t}); }
	void callGuest(uint32_t t) {
		if (E.count(t)) { if (rec) syncW += wt; flushRegs(); kconst.clear(); O.calls.push_back({A.bl(), t}); reloadRegs(); }
		else { A.mov(true, 0, RC); movw(1, t); helper(H_CALL); }
	}
	void callDynamic(int wTarget) { A.mov(false, 1, wTarget); A.mov(true, 0, RC); helper(H_CALL); }
	void ret() { toEpilogue.push_back(A.b()); }
	std::vector<size_t> toEpilogueNoWB;
	void retAfterCall() { toEpilogueNoWB.push_back(A.b()); }   // registers already in memory (a call was just made)
	bool isEntry(uint32_t t) const { return E.count(t) || (H && H->count(t)); }   // hooked routines are entries too
	bool intra(uint32_t t) { return vis.count(t) && !(isEntry(t) && t != F.entry); }
	// branch condition into w4 (1 = taken); returns false when the branch is unconditional
	bool branchCond(uint32_t bo, uint32_t bi, bool decCtr) {
		bool any = false;
		if (decCtr && !(bo & 4)) {
			A.ldrw(4, RC, OFF_CTR); A.subi(false, 4, 4, 1); A.strw(4, RC, OFF_CTR);
			A.cmpi(false, 4, 0); A.cset(false, 4, (bo & 2) ? EQ : NE); any = true;
		}
		if (!(bo & 16)) {
			crbit(5, bi); if (!((bo >> 3) & 1)) A.logi(false, 2, 5, 5, 1);
			if (any) A.and_(false, 4, 4, 5); else A.mov(false, 4, 5);
			any = true;
		}
		return any;
	}
	void insn(const Insn &r);
	void fpArith(uint32_t w, bool single);
};

void FnGen::insn(const Insn &I)
{
	const uint32_t w = I.w, pc = I.pc, op = I.op, xo = I.xo;
	const uint32_t rt = (w >> 21) & 31, rs = rt, ra = (w >> 16) & 31, rb = (w >> 11) & 31, rc = w & 1;
	const int32_t simm = sx16(w); const uint32_t uimm = w & 0xFFFF;
	auto cr0 = [&](int wv) { if (rc) setcr0(wv); };
	switch (op) {
	case 14: {
		auto k = kconst.find((int)ra); bool kc = ra == 0 || k != kconst.end(); uint32_t v = (ra == 0 ? 0 : kc ? k->second : 0) + (uint32_t)simm;
		if (kc) movw(0, v); else { ldg(0, ra); addimm(0, 0, simm); }
		stg(0, rt); if (kc) kconst[(int)rt] = v; return;
	}
	case 15: {
		auto k = kconst.find((int)ra); bool kc = ra == 0 || k != kconst.end(); uint32_t v = (ra == 0 ? 0 : kc ? k->second : 0) + ((uint32_t)simm << 16);
		if (kc) movw(0, v); else { ldg(0, ra); movw(1, (uint32_t)simm << 16); A.add(false, 0, 0, 1); }
		stg(0, rt); if (kc) kconst[(int)rt] = v; return;
	}
	case 12: case 13:   // addic(.)
		ldg(0, ra); movw(1, (uint32_t)simm); A.adds(false, 0, 0, 1); A.cset(false, 2, CS); stg(0, rt); setCA(2); if (op == 13) setcr0(0); return;
	case 8:             // subfic: b - a, CA = b >= a
		ldg(0, ra); movw(1, (uint32_t)simm); A.subs(false, 2, 1, 0); A.cset(false, 3, CS); stg(2, rt);
		A.ldrw(4, RC, OFF_XER); A.bfi(false, 4, 3, 29, 1); A.strw(4, RC, OFF_XER); return;
	case 7: ldg(0, ra); movw(1, (uint32_t)simm); A.mul(false, 0, 0, 1); stg(0, rt); return;
	case 11: { int f = rt >> 2; ldg(0, ra); movw(5, (uint32_t)simm); cmpNibble(f, [&] { A.cmp(false, 0, 5); }, true); return; }
	case 10: { int f = rt >> 2; ldg(0, ra); movw(5, uimm); cmpNibble(f, [&] { A.cmp(false, 0, 5); }, false); return; }
	case 24: case 25: case 26: case 27: case 28: case 29: {
		uint32_t imm = (op == 25 || op == 27 || op == 29) ? uimm << 16 : uimm;
		auto k = kconst.find((int)rs);
		if (op <= 25 && k != kconst.end()) { uint32_t v = k->second | imm; movw(0, v); stg(0, ra); kconst[(int)ra] = v; return; }
		ldg(0, rs);
		if (op <= 25) orrimm(0, 0, imm); else if (op <= 27) eorimm(0, 0, imm); else andimm(0, 0, imm);
		stg(0, ra); if (op >= 28) setcr0(0); return;
	}
	case 21: { uint32_t sh = rb, mb = (w >> 6) & 31, me = (w >> 1) & 31; ldg(0, rs); if (sh) A.rori(false, 0, 0, 32 - sh); andimm(0, 0, mask32(mb, me)); stg(0, ra); cr0(0); return; }
	case 20: {
		uint32_t sh = rb, mb = (w >> 6) & 31, me = (w >> 1) & 31, m = mask32(mb, me);
		ldg(0, rs); if (sh) A.rori(false, 0, 0, 32 - sh); andimm(0, 0, m); ldg(1, ra); andimm(1, 1, ~m); A.orr(false, 0, 0, 1); stg(0, ra); cr0(0); return;
	}
	case 23: {
		uint32_t mb = (w >> 6) & 31, me = (w >> 1) & 31;
		ldg(0, rs); ldg(1, rb); A.neg(false, 1, 1); A.rorv(false, 0, 0, 1); andimm(0, 0, mask32(mb, me)); stg(0, ra); cr0(0); return;
	}
	case 32: case 33: eaD(ra, simm); if (op == 33) stg(0, ra); load(2, false); stg(2, rt); return;   // note: update writes ea before the load result (rt == ra is invalid)
	case 34: case 35: eaD(ra, simm); if (op == 35) stg(0, ra); load(0, false); stg(2, rt); return;
	case 40: case 41: eaD(ra, simm); if (op == 41) stg(0, ra); load(1, false); stg(2, rt); return;
	case 42: case 43: eaD(ra, simm); if (op == 43) stg(0, ra); load(1, true); stg(2, rt); return;
	case 36: case 37: eaD(ra, simm); ldg(2, rs); if (op == 37) { stg(0, ra); } store(2); return;
	case 38: case 39: eaD(ra, simm); ldg(2, rs); if (op == 39) { stg(0, ra); } store(0); return;
	case 44: case 45: eaD(ra, simm); ldg(2, rs); if (op == 45) { stg(0, ra); } store(1); return;
	case 46: eaD(ra, simm); A.strx(0, SP, 16); for (uint32_t i = rt; i < 32; i++) { A.ldrw(0, SP, 16); addimm(0, 0, 4 * (int32_t)(i - rt)); load(2, false); stg(2, i); } return;
	case 47: eaD(ra, simm); A.strx(0, SP, 16); for (uint32_t i = rs; i < 32; i++) { A.ldrw(0, SP, 16); addimm(0, 0, 4 * (int32_t)(i - rs)); ldg(2, i); store(2); } return;
	case 48: case 49: eaD(ra, simm); if (op == 49) stg(0, ra); load(2, false); A.fmov_w2s(0, 2); A.fcvt_sd(0, 0); A.strd(0, RC, OFF_F(rt)); return;
	case 50: case 51: eaD(ra, simm); if (op == 51) stg(0, ra); load(3, false); A.strx(2, RC, OFF_F(rt)); return;
	case 52: case 53: eaD(ra, simm); A.ldrd(0, RC, OFF_F(rs)); A.fcvt_ds(0, 0); A.fmov_s2w(2, 0); if (op == 53) stg(0, ra); store(2); return;
	case 54: case 55: eaD(ra, simm); A.ldrx(2, RC, OFF_F(rs)); if (op == 55) stg(0, ra); store(3); return;
	case 17: A.mov(true, 0, RC); movw(1, pc + 4); helper(H_SC); return;
	case 19:
		switch (xo) {
		case 0: A.ldrb(0, RC, OFF_CR(ra >> 2)); A.strb(0, RC, OFF_CR(rt >> 2)); return;
		case 33: case 129: case 193: case 225: case 257: case 289: case 417: case 449:
			crbit(6, ra); crbit(7, rb);
			switch (xo) {
			case 33: A.orr(false, 8, 6, 7); A.logi(false, 2, 8, 8, 1); break;   // nor
			case 129: A.bic(false, 8, 6, 7); break;                              // andc
			case 193: A.eor(false, 8, 6, 7); break;                              // xor
			case 225: A.and_(false, 8, 6, 7); A.logi(false, 2, 8, 8, 1); break;  // nand
			case 257: A.and_(false, 8, 6, 7); break;                             // and
			case 289: A.eor(false, 8, 6, 7); A.logi(false, 2, 8, 8, 1); break;  // eqv
			case 417: A.logi(false, 2, 9, 7, 1); A.orr(false, 8, 6, 9); break;  // orc
			case 449: A.orr(false, 8, 6, 7); break;                              // or
			}
			setcrbit(rt, 8); return;
		case 150: return;   // isync
		}
		break;
	case 31:
		switch (xo) {
		case 23: case 55: eaX(ra, rb); if (xo == 55) stg(0, ra); load(2, false); stg(2, rt); return;
		case 87: case 119: eaX(ra, rb); if (xo == 119) stg(0, ra); load(0, false); stg(2, rt); return;
		case 279: case 311: eaX(ra, rb); if (xo == 311) stg(0, ra); load(1, false); stg(2, rt); return;
		case 343: case 375: eaX(ra, rb); if (xo == 375) stg(0, ra); load(1, true); stg(2, rt); return;
		case 20: eaX(ra, rb); load(2, false); stg(2, rt); return;
		case 534: eaX(ra, rb); load(2, false); A.rev(false, 2, 2); stg(2, rt); return;
		case 790: eaX(ra, rb); load(1, false); A.rev16(false, 2, 2); A.uxth(2, 2); stg(2, rt); return;
		case 151: case 183: eaX(ra, rb); ldg(2, rs); if (xo == 183) stg(0, ra); store(2); return;
		case 215: case 247: eaX(ra, rb); ldg(2, rs); if (xo == 247) stg(0, ra); store(0); return;
		case 407: case 439: eaX(ra, rb); ldg(2, rs); if (xo == 439) stg(0, ra); store(1); return;
		case 662: eaX(ra, rb); ldg(2, rs); A.rev(false, 2, 2); store(2); return;
		case 918: eaX(ra, rb); ldg(2, rs); A.rev16(false, 2, 2); store(1); return;
		case 535: case 567: eaX(ra, rb); if (xo == 567) stg(0, ra); load(2, false); A.fmov_w2s(0, 2); A.fcvt_sd(0, 0); A.strd(0, RC, OFF_F(rt)); return;
		case 599: case 631: eaX(ra, rb); if (xo == 631) stg(0, ra); load(3, false); A.strx(2, RC, OFF_F(rt)); return;
		case 663: case 695: eaX(ra, rb); A.ldrd(0, RC, OFF_F(rs)); A.fcvt_ds(0, 0); A.fmov_s2w(2, 0); if (xo == 695) stg(0, ra); store(2); return;
		case 727: case 759: eaX(ra, rb); A.ldrx(2, RC, OFF_F(rs)); if (xo == 759) stg(0, ra); store(3); return;
		case 983: eaX(ra, rb); A.ldrw(2, RC, OFF_F(rs)); store(2); return;   // stfiwx: low word of the double's bits (little-endian host)
		case 0: { int f = rt >> 2; ldg(0, ra); ldg(5, rb); cmpNibble(f, [&] { A.cmp(false, 0, 5); }, true); return; }
		case 32: { int f = rt >> 2; ldg(0, ra); ldg(5, rb); cmpNibble(f, [&] { A.cmp(false, 0, 5); }, false); return; }
		case 266: ldg(0, ra); ldg(1, rb); A.add(false, 0, 0, 1); stg(0, rt); cr0(0); return;
		case 40: ldg(0, ra); ldg(1, rb); A.sub(false, 0, 1, 0); stg(0, rt); cr0(0); return;
		case 10: ldg(0, ra); ldg(1, rb); A.adds(false, 0, 0, 1); A.cset(false, 2, CS); stg(0, rt); setCA(2); cr0(0); return;
		case 8: ldg(0, ra); ldg(1, rb); A.subs(false, 0, 1, 0); A.cset(false, 2, CS); stg(0, rt); setCA(2); cr0(0); return;
		case 138: case 136: case 202: case 234: case 200: case 232: {
			// a' = a or ~a; b' = rb, 0 or 0xFFFFFFFF; s = a' + b' + CA (64-bit sum, carry out = bit 32)
			ldg(0, ra); if (xo == 136 || xo == 200 || xo == 232) A.mvn(false, 0, 0);
			if (xo == 138 || xo == 136) ldg(1, rb); else if (xo == 202 || xo == 200) movw(1, 0); else movw(1, 0xFFFFFFFFu);
			getCA(2);
			A.add(true, 3, 0, 1); A.add(true, 3, 3, 2);           // x regs: w-writes zero-extend, so this is the exact 33-bit sum
			A.lsri(true, 4, 3, 32); stg(3, rt); setCA(4); cr0(3); return;
		}
		case 104: ldg(0, ra); A.neg(false, 0, 0); stg(0, rt); cr0(0); return;
		case 235: ldg(0, ra); ldg(1, rb); A.mul(false, 0, 0, 1); stg(0, rt); cr0(0); return;
		case 75: ldg(0, ra); ldg(1, rb); A.smull(0, 0, 1); A.lsri(true, 0, 0, 32); stg(0, rt); cr0(0); return;
		case 11: ldg(0, ra); ldg(1, rb); A.umull(0, 0, 1); A.lsri(true, 0, 0, 32); stg(0, rt); cr0(0); return;
		case 491: {   // divw with the reference's handling of the undefined cases
			ldg(0, ra); ldg(1, rb);
			size_t z = A.cbz(false, 1);
			A.cmni(false, 1, 1); size_t n1 = A.bcond(NE);
			movw(2, 0x80000000u); A.cmp(false, 0, 2); size_t ov = A.bcond(EQ);
			A.patch(n1, A.pos());
			A.sdiv(false, 2, 0, 1); size_t done1 = A.b();
			A.patch(z, A.pos()); A.asri(false, 2, 0, 31); size_t done2 = A.b();   // a / 0: 0 if a >= 0 else -1
			A.patch(ov, A.pos()); movw(2, 0xFFFFFFFFu);
			A.patch(done1, A.pos()); A.patch(done2, A.pos());
			stg(2, rt); cr0(2); return;
		}
		case 459: ldg(0, ra); ldg(1, rb); A.udiv(false, 2, 0, 1); stg(2, rt); cr0(2); return;   // /0 = 0 on AArch64, as the reference
		case 28: ldg(0, rs); ldg(1, rb); A.and_(false, 0, 0, 1); stg(0, ra); cr0(0); return;
		case 60: ldg(0, rs); ldg(1, rb); A.bic(false, 0, 0, 1); stg(0, ra); cr0(0); return;
		case 444: ldg(0, rs); if (rs != rb) { ldg(1, rb); A.orr(false, 0, 0, 1); } stg(0, ra); cr0(0); return;
		case 412: ldg(0, rs); ldg(1, rb); A.orn(false, 0, 0, 1); stg(0, ra); cr0(0); return;
		case 316: ldg(0, rs); ldg(1, rb); A.eor(false, 0, 0, 1); stg(0, ra); cr0(0); return;
		case 476: ldg(0, rs); ldg(1, rb); A.and_(false, 0, 0, 1); A.mvn(false, 0, 0); stg(0, ra); cr0(0); return;
		case 124: ldg(0, rs); ldg(1, rb); A.orr(false, 0, 0, 1); A.mvn(false, 0, 0); stg(0, ra); cr0(0); return;
		case 284: ldg(0, rs); ldg(1, rb); A.eon(false, 0, 0, 1); stg(0, ra); cr0(0); return;
		case 24: case 536:   // slw / srw: shift counts 32..63 give 0
			ldg(0, rs); ldg(1, rb);
			if (xo == 24) A.lslv(false, 2, 0, 1); else A.lsrv(false, 2, 0, 1);
			A.tsti(false, 1, 0x20); A.csel(false, 2, ZR, 2, NE); stg(2, ra); cr0(2); return;
		case 792: {   // sraw
			ldg(0, rs); ldg(1, rb); andimm(1, 1, 63);
			A.cmpi(false, 1, 31); size_t big = A.bcond(HI);
			A.asrv(false, 2, 0, 1);                                   // n <= 31
			A.movz(false, 4, 1, 0); A.lslv(false, 4, 4, 1); A.subi(false, 4, 4, 1);   // (1 << n) - 1 (n=0 -> 0)
			A.tst(false, 0, 4); A.cset(false, 5, NE); size_t j = A.b();
			A.patch(big, A.pos()); A.asri(false, 2, 0, 31); A.movz(false, 5, 1, 0);    // n > 31: CA = (s < 0) && 1
			A.patch(j, A.pos());
			A.lsri(false, 6, 0, 31); A.and_(false, 5, 5, 6);              // s < 0
			A.cmpi(false, 1, 0); A.csel(false, 5, ZR, 5, EQ);            // n != 0
			stg(2, ra); setCA(5); cr0(2); return;
		}
		case 824: {   // srawi
			uint32_t sh = rb; ldg(0, rs);
			A.asri(false, 2, 0, sh);
			if (sh) { andimm(4, 0, (1u << sh) - 1); A.cmpi(false, 4, 0); A.cset(false, 5, NE); A.lsri(false, 6, 0, 31); A.and_(false, 5, 5, 6); }
			else movw(5, 0);
			stg(2, ra); setCA(5); cr0(2); return;
		}
		case 26: ldg(0, rs); A.clz(false, 0, 0); stg(0, ra); cr0(0); return;
		case 954: ldg(0, rs); A.sxtb(false, 0, 0); stg(0, ra); cr0(0); return;
		case 922: ldg(0, rs); A.sxth(false, 0, 0); stg(0, ra); cr0(0); return;
		case 19: A.mov(true, 0, RC); helper(H_MFCR); stg(0, rt); return;
		case 144: A.mov(true, 0, RC); movw(1, (w >> 12) & 0xFF); ldg(2, rs); helper(H_MTCRF); return;
		case 83: A.ldrw(0, RC, OFF_MSR); stg(0, rt); return;
		case 146: ldg(0, rs); A.strw(0, RC, OFF_MSR); return;
		case 595: A.ldrw(0, RC, OFF_SR(ra & 15)); stg(0, rt); return;
		case 210: ldg(0, rs); A.strw(0, RC, OFF_SR(ra & 15)); return;
		case 242: ldg(0, rb); A.lsri(false, 0, 0, 28); ldg(1, rs); A.addi(true, 2, RC, (uint32_t)offsetof(CPU, sr)); A.ldstr(2, 0, 1, 2, 0, 2, true); return;
		case 659: ldg(0, rb); A.lsri(false, 0, 0, 28); A.addi(true, 2, RC, (uint32_t)offsetof(CPU, sr)); A.ldstr(2, 1, 1, 2, 0, 2, true); stg(1, rt); return;
		case 339: case 467: case 371: {
			uint32_t spr = ((w >> 16) & 31) | (((w >> 11) & 31) << 5); bool rd = xo != 467;
			if (xo == 371) spr = ra == 12 ? 268 : 269;
			if (rd) {
				if (spr == 22 || spr == 268 || spr == 269 || spr == 284 || spr == 285) { A.mov(true, 0, RC); movw(1, spr); movw(2, rt); helper(H_TBREAD); stg(0, rt); }
				else if (spr == 1) { A.ldrw(0, RC, OFF_XER); stg(0, rt); }
				else if (spr == 8) { A.ldrw(0, RC, OFF_LR); stg(0, rt); }
				else if (spr == 9) { A.ldrw(0, RC, OFF_CTR); stg(0, rt); }
				else { A.ldrw(0, RC, OFF_SPR(spr)); stg(0, rt); }
			} else {
				ldg(0, rs);
				if (spr == 1) A.strw(0, RC, OFF_XER);
				else if (spr == 8) A.strw(0, RC, OFF_LR);
				else if (spr == 9) A.strw(0, RC, OFF_CTR);
				else if (spr == 22 || spr == 268 || spr == 269 || spr == 284 || spr == 285) { A.mov(false, 2, 0); A.mov(true, 0, RC); movw(1, spr); helper(H_MTSPR); }
				else A.strw(0, RC, OFF_SPR(spr));
			}
			return;
		}
		case 598: case 854: case 86: case 54: case 246: case 278: case 982: case 470: case 306: case 566: case 1014: case 274: return;   // cache / sync: nothing
		}
		break;
	case 59: case 63: fpArith(w, op == 59); return;
	}
	// unreachable for supported instructions
	A.mov(true, 0, RC); movw(1, pc); movw(2, w); helper(H_UNIMPL);
}

void FnGen::fpArith(uint32_t w, bool single)
{
	const uint32_t op = w >> 26, rt = (w >> 21) & 31, fa = (w >> 16) & 31, fb = (w >> 11) & 31, fc = (w >> 6) & 31;
	const uint32_t xo = (w >> 1) & 0x3ff, xo5 = (w >> 1) & 0x1f, rc = w & 1;
	auto ld = [&](int d, uint32_t f) { A.ldrd(d, RC, OFF_F(f)); };
	auto st = [&](int d) { A.strd(d, RC, OFF_F(rt)); };
	auto round1 = [&](int d) { if (single) { A.fcvt_ds(d, d); A.fcvt_sd(d, d); } };
	auto fcr1 = [&] { if (rc) { A.ldrw(0, RC, OFF_FPSCR); A.lsri(false, 0, 0, 28); A.strb(0, RC, OFF_CR(1)); } };
	static const uint16_t ex[] = {0, 12, 14, 15, 32, 38, 40, 64, 70, 72, 134, 136, 264, 583, 711};
	bool excl = false; if (op == 63) for (uint16_t e : ex) if (xo == e) excl = true;
	bool arith = (xo5 == 18 || xo5 == 20 || xo5 == 21 || xo5 == 25 || xo5 == 28 || xo5 == 29 || xo5 == 30 || xo5 == 31 || xo5 == 24 || xo5 == 23 || xo5 == 26) && !excl;
	if (arith) {
		switch (xo5) {
		case 21: ld(0, fa); ld(1, fb); A.fadd(1, 0, 0, 1); break;
		case 20: ld(0, fa); ld(1, fb); A.fsub(1, 0, 0, 1); break;
		case 18: ld(0, fa); ld(1, fb); A.fdiv(1, 0, 0, 1); break;
		case 25: ld(0, fa); ld(1, fc); A.fmul(1, 0, 0, 1); break;
		case 29: ld(0, fa); ld(1, fc); ld(2, fb); A.fmadd(1, 0, 0, 1, 2); break;            // a*c + b
		case 28: ld(0, fa); ld(1, fc); ld(2, fb); A.fnmsub(1, 0, 0, 1, 2); break;           // a*c - b
		case 31: ld(0, fa); ld(1, fc); ld(2, fb); A.fmadd(1, 0, 0, 1, 2); A.fneg(1, 0, 0); break;   // -(a*c + b)
		case 30: ld(0, fa); ld(1, fc); ld(2, fb); A.fnmsub(1, 0, 0, 1, 2); A.fneg(1, 0, 0); break;  // -(a*c - b)
		case 23: ld(0, fa); ld(1, fb); ld(2, fc); A.fcmpz(1, 0); A.fcsel(1, 0, 2, 1, GE); st(0); fcr1(); return;   // fsel (no rounding)
		case 24: ld(1, fb); A.movz(true, 0, 0x3FF0, 48); A.fmov_x2d(0, 0); A.fdiv(1, 0, 0, 1); break;   // fres: 1/b (single rounded below)
		case 26: ld(1, fb); A.fsqrt(1, 1, 1); A.movz(true, 0, 0x3FF0, 48); A.fmov_x2d(0, 0); A.fdiv(1, 0, 0, 1); st(0); fcr1(); return;   // frsqrte = 1/sqrt(b) in double
		}
		round1(0); st(0); fcr1(); return;
	}
	switch (xo) {
	case 0: case 32: {
		int f = rt >> 2; ld(0, fa); ld(1, fb); A.fcmp(1, 0, 1);
		A.movz(false, 1, 2, 0); A.movz(false, 2, 8, 0); A.csel(false, 1, 2, 1, MI);
		A.movz(false, 2, 4, 0); A.csel(false, 1, 2, 1, GT); A.movz(false, 2, 1, 0); A.csel(false, 1, 2, 1, VS);
		A.strb(1, RC, OFF_CR(f)); return;
	}
	case 12: ld(0, fb); A.fcvt_ds(0, 0); A.fcvt_sd(0, 0); st(0); fcr1(); return;
	case 15: case 14: {   // fctiwz / fctiw with the reference's saturation and upper-word behaviour
		ld(0, fb);
		if (xo == 15) A.fcvtzs_xd(0, 0); else A.fcvtns_xd(0, 0);
		A.fcmp(1, 0, 0); size_t nan = A.bcond(VS);
		movw(1, 0x7FFFFFFFu); A.cmp(true, 0, 1); size_t pos = A.bcond(GT);
		movw(1, 0x80000000u); A.sxtw(1, 1); A.cmp(true, 0, 1); size_t neg = A.bcond(LT);
		A.mov(false, 0, 0); size_t done = A.b();                          // in range: the low word, zero-extended
		A.patch(pos, A.pos()); movw(0, 0x7FFFFFFFu); size_t d2 = A.b();
		A.patch(nan, A.pos()); A.patch(neg, A.pos()); movw(0, 0x80000000u); A.sxtw(0, 0);
		A.patch(done, A.pos()); A.patch(d2, A.pos());
		A.strx(0, RC, OFF_F(rt)); fcr1(); return;
	}
	case 72: ld(0, fb); st(0); fcr1(); return;
	case 40: A.ldrx(0, RC, OFF_F(fb)); A.movz(true, 1, 0x8000, 48); A.eor(true, 0, 0, 1); A.strx(0, RC, OFF_F(rt)); fcr1(); return;
	case 264: A.ldrx(0, RC, OFF_F(fb)); A.lsli(true, 0, 0, 1); A.lsri(true, 0, 0, 1); A.strx(0, RC, OFF_F(rt)); fcr1(); return;
	case 136: A.ldrx(0, RC, OFF_F(fb)); A.movz(true, 1, 0x8000, 48); A.orr(true, 0, 0, 1); A.strx(0, RC, OFF_F(rt)); fcr1(); return;
	case 583: A.ldrw(0, RC, OFF_FPSCR); A.strx(0, RC, OFF_F(rt)); fcr1(); return;   // zero-extended fpscr
	case 711: A.mov(true, 0, RC); movw(1, (w >> 17) & 0xFF); A.ldrw(2, RC, OFF_F(fb)); helper(H_MTFSF); fcr1(); return;
	}
	A.mov(true, 0, RC); movw(1, 0); movw(2, w); helper(H_UNIMPL);
}

void FnGen::run()
{
	vis.insert(F.pcs.begin(), F.pcs.end());
	// blocks and labels exactly like the reference generator
	blocks.insert(F.entry);
	for (uint32_t pc : F.pcs) {
		const Insn *r = P.insn(pc);
		if (r->ctl != C_NONE) {
			std::vector<uint32_t> tg;
			if ((r->ctl == C_JMP || r->ctl == C_COND) && !r->link) tg.push_back(r->target);
			else if (r->ctl == C_IND && !r->link) { auto it = F.sites.find(pc); if (it != F.sites.end()) tg = it->second; }
			for (uint32_t t : tg) if (vis.count(t) && !(isEntry(t) && t != F.entry)) { labels.insert(t); if (t <= pc) backs.insert(t); }
			blocks.insert(pc + 4);
		}
		if (!vis.count(pc - 4)) blocks.insert(pc);
	}
	for (uint32_t l : labels) blocks.insert(l);
	{ std::set<uint32_t> b2; for (uint32_t b : blocks) if (vis.count(b)) b2.insert(b); blocks.swap(b2); }
	for (uint32_t b0 : blocks) { int n = 0; uint32_t q = b0; while (vis.count(q) && (q == b0 || !blocks.count(q))) { n++; q += 4; } cost[b0] = n; }

	// loop bodies (between a backward branch and its target) weigh more in the register allocation
	if (rec) {
		for (uint32_t pc : F.pcs) {
			const Insn *r = P.insn(pc);
			if ((r->ctl == C_JMP || r->ctl == C_COND) && !r->link && r->target <= pc && vis.count(r->target))
				for (uint32_t q = r->target; q <= pc; q += 4) if (vis.count(q)) loopPcs.insert(q);
		}
	}
	O.entry[F.entry] = A.pos();
	A.stp_pre(29, 30, SP, -32); A.addi(true, 29, SP, 0);   // [sp+16]: scratch slot (lmw/stmw base)
	reloadRegs();
	if (F.pcs.front() != F.entry) gotoLabel(F.entry);   // the body is laid out by address: code below the entry comes first
	for (uint32_t pc : F.pcs) {
		const Insn &I = *P.insn(pc);
		wt = loopPcs.count(pc) ? 8 : 1;
		labelPos[pc] = A.pos();
		int ff = flagsField; flagsField = -1;
		if (labels.count(pc) || blocks.count(pc) || pc == F.entry) ff = -1;
		if (blocks.count(pc)) kconst.clear();
		if (blocks.count(pc)) {
			A.subi(false, 22, 22, (uint32_t)cost[pc]);                      // cycle balance lives in w22
			if (backs.count(pc) || pc == F.entry) {
				size_t skip = A.tbz(22, 31);
				A.mov(true, 0, RC); movw(1, pc); helper(H_SAFEPOINT);
				A.patch(skip, A.pos());
			}
		}
		// instruction body
		uint32_t bo = (I.w >> 21) & 31, bi = (I.w >> 16) & 31;
		if (I.ctl == C_NONE) { insn(I); continue; }
		if (I.ctl == C_RFI) { A.ldrw(0, RC, OFF_SPR(27)); A.strw(0, RC, OFF_MSR); A.mov(true, 0, RC); helper(H_RFI); retAfterCall(); continue; }
		if (I.ctl == C_JMP) {
			if (I.link) { A.movi32(0, pc + 4); A.strw(0, RC, OFF_LR); callGuest(I.target); }
			else if (intra(I.target)) gotoLabel(I.target);
			else { callGuest(I.target); retAfterCall(); }
			continue;
		}
		if (I.ctl == C_COND && !I.link && (bo & 4) && !(bo & 16) && (int)(bi >> 2) == ff && (bi & 3) != 3) {
			// compare + branch: branch on the host flags of the compare that was just emitted
			Cond c = (bi & 3) == 0 ? (flagsSigned ? LT : CC) : (bi & 3) == 1 ? (flagsSigned ? GT : HI) : EQ;
			if (!((bo >> 3) & 1)) c = invert(c);
			if (intra(I.target)) jumps.push_back({A.bcond(c), I.target});
			else { size_t skip = A.bcond(invert(c)); callGuest(I.target); retAfterCall(); A.patch(skip, A.pos()); }
			continue;
		}
		if (I.ctl == C_COND) {
			bool cond = branchCond(bo, bi, true);
			if (I.link) { A.movi32(0, pc + 4); A.strw(0, RC, OFF_LR); }
			size_t skip = cond ? A.cbz(false, 4) : (size_t)-1;
			if (I.link) callGuest(I.target);
			else if (intra(I.target)) gotoLabel(I.target);
			else { callGuest(I.target); retAfterCall(); }
			if (cond) A.patch(skip, A.pos());
			continue;
		}
		if (I.ctl == C_RET) {   // tgt = lr & ~3 is read before the link register is replaced
			A.ldrw(6, RC, OFF_LR); andimm(6, 6, ~3u);
			bool cond = branchCond(bo, bi, true);
			if (I.link) { A.movi32(0, pc + 4); A.strw(0, RC, OFF_LR); }
			size_t skip = cond ? A.cbz(false, 4) : (size_t)-1;
			if (I.link) callDynamic(6); else ret();
			if (cond) A.patch(skip, A.pos());
			continue;
		}
		if (I.ctl == C_IND) {
			A.ldrw(6, RC, OFF_CTR); andimm(6, 6, ~3u);
			bool cond = branchCond(bo, bi, false);
			if (I.link) { A.movi32(0, pc + 4); A.strw(0, RC, OFF_LR); }
			size_t skip = cond ? A.cbz(false, 4) : (size_t)-1;
			if (I.link) callDynamic(6);
			else {
				auto it = F.sites.find(pc);
				if (it != F.sites.end() && !it->second.empty()) {
					for (uint32_t t : it->second) {
						movw(7, t); A.cmp(false, 6, 7); size_t ne = A.bcond(NE);
						if (intra(t)) gotoLabel(t); else { callGuest(t); retAfterCall(); }
						A.patch(ne, A.pos());
					}
				}
				callDynamic(6); retAfterCall();
			}
			if (cond) A.patch(skip, A.pos());
			continue;
		}
	}
	// falling off the end returns
	size_t ep = A.pos();
	flushRegs();
	size_t ep2 = A.pos();
	A.ldp_post(29, 30, SP, 32); A.ret();
	for (size_t at : toEpilogue) A.patch(at, ep);
	for (size_t at : toEpilogueNoWB) A.patch(at, ep2);
	for (auto &j : jumps) A.patch(j.first, labelPos.at(j.second));
	emitSlows();
}

void translateAll(Program &p, A64Code &out, const std::set<uint32_t> &overridden)
{
	std::set<uint32_t> entries;
	for (auto &f : p.funcs) if (!overridden.count(f.first)) entries.insert(f.first);
	for (auto &f : p.funcs) {                                     // replaced routines are translated too (their original
		A64Code scratch;                                          // body), but every call to them goes through the dispatcher                                          // pass 1: count register uses
		FnGen g0(p, scratch, f.second, entries); g0.H = &overridden; g0.rec = true; g0.run();
		std::vector<std::pair<double, int>> cand;
		for (auto &u : g0.useW) {
			double cost = 1 + (g0.written.count(u.first) ? 1 : 0) * (1 + g0.syncW) + g0.syncW;   // entry load, write-backs, reloads
			if (u.second > cost + 1) cand.push_back({u.second - cost, u.first});
		}
		std::sort(cand.rbegin(), cand.rend());
		FnGen g(p, out, f.second, entries); g.H = &overridden;                        // pass 2: real code
		g.written = g0.written;
		for (size_t i = 0; i < cand.size() && i < 6; i++) { g.host[cand[i].second] = 23 + (int)i; g.assigned.push_back(cand[i].second); }
		g.run();
	}
	for (auto &c : out.calls) out.a.patch(c.at, out.entry.at(c.target));
}

}
