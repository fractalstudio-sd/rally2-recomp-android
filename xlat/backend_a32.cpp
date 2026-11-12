// PowerPC 603e -> ARM (A32) code generator. Same structure and semantics as the AArch64 generator (backend_a64.cpp):
// one native function per guest function, guest calls are native calls, cycles counted per block with a safepoint at
// function entries and loop heads. r4 = CPU (+A32_BIAS), r5 = RAM, r6 = helper table, r7 = cycle balance,
// r8-r11 = the busiest guest registers of the function; r0-r3, r12 and lr are scratch. GPLv3.
#include "backend_a32.h"
extern "C" {
#include "ppc_rt.h"
}
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <algorithm>

namespace xl {
using namespace a32;

bool a32HasVfp4 = true, a32HasIdiv = true;

static const int RC = 4, RRAM = 5, RH = 6, RCYC = 7;
#define BO(x) ((int32_t)(x) - A32_BIAS)
#define OFF_R(n) BO(offsetof(CPU, r) + 4 * (n))
#define OFF_LR BO(offsetof(CPU, lr))
#define OFF_CTR BO(offsetof(CPU, ctr))
#define OFF_XER BO(offsetof(CPU, xer))
#define OFF_MSR BO(offsetof(CPU, msr))
#define OFF_FPSCR BO(offsetof(CPU, fpscr))
#define OFF_CR(n) BO(offsetof(CPU, cr) + (n))
#define OFF_F(n) BO(offsetof(CPU, f) + 8 * (n))
#define OFF_SPR(n) BO(offsetof(CPU, spr) + 4 * (n))
#define OFF_SR(n) BO(offsetof(CPU, sr) + 4 * (n))
#define OFF_CYC BO(offsetof(CPU, cyc))

static inline int32_t sx16(uint32_t v) { return (int32_t)(int16_t)(v & 0xFFFF); }
static inline uint32_t mask32(unsigned mb, unsigned me) {
	if (mb <= me) return (0xFFFFFFFFu >> mb) & (0xFFFFFFFFu << (31 - me));
	return ~mask32(me + 1, mb - 1);
}

struct Slow { size_t at; size_t back; int kind; int size; bool sign; };

class FnGen32 {
public:
	FnGen32(Program &p, A32Code &o, const Function &f, const std::set<uint32_t> &entries) : P(p), O(o), A(o.a), F(f), E(entries) { for (int &h : host) h = -1; }
	void run();
	const std::set<uint32_t> *H = nullptr;
	bool rec = false; double wt = 1; std::map<int, double> useW; std::set<int> written; double syncW = 0;
	int host[32]; std::vector<int> assigned; std::set<uint32_t> loopPcs;
	int flagsField = -1; bool flagsSigned = true;
	std::map<int, uint32_t> kconst; bool eaConst = false; uint32_t eaVal = 0;
private:
	Program &P; A32Code &O; Asm &A; const Function &F; const std::set<uint32_t> &E;
	std::set<uint32_t> vis, labels, backs, blocks;
	std::map<uint32_t, size_t> labelPos;
	std::vector<std::pair<size_t, uint32_t>> jumps;
	std::vector<size_t> toEpilogue, toEpilogueNoWB;
	std::vector<Slow> slows;
	std::map<uint32_t, int> cost;

	void ldg(int d, int r) {
		if (rec) useW[r] += wt;
		if (host[r] >= 0) { if (d != host[r]) A.mov(d, host[r]); } else A.ldr(d, RC, OFF_R(r));
	}
	void stg(int s, int r) {
		if (rec) { useW[r] += wt; written.insert(r); }
		kconst.erase(r);
		if (host[r] >= 0) { if (s != host[r]) A.mov(host[r], s); } else A.str(s, RC, OFF_R(r));
	}
	void flushRegs() { for (int r : assigned) if (written.count(r)) A.str(host[r], RC, OFF_R(r)); }
	void reloadRegs() { for (int r : assigned) A.ldr(host[r], RC, OFF_R(r)); }
	void cpuArg() { A.tryImm(SUB, false, 0, RC, A32_BIAS); }   // r0 = CPU *
	void helper(int slot) {
		bool guest = slot == H_SAFEPOINT || slot == H_SC || slot == H_RFI || slot == H_CALL || slot == H_UNIMPL;
		bool cyc = !(slot == H_MFCR || slot == H_MTCRF || slot == H_MTFSF || slot == H_DIVW || slot == H_DIVWU || slot == H_FMA);
		if (rec && guest) syncW += wt;
		if (guest) { flushRegs(); kconst.clear(); }
		if (cyc) A.str(RCYC, RC, OFF_CYC);
		A.ldr(IP, RH, 4 * slot); A.blx(IP);
		if (cyc) A.ldr(RCYC, RC, OFF_CYC);
		if (guest) reloadRegs();
	}
	void movw(int d, uint32_t v) { A.movi32(d, v); }
	void addimm(int d, int n, int32_t imm) {
		if (imm == 0) { if (d != n) A.mov(d, n); return; }
		if (A.tryImm(ADD, false, d, n, (uint32_t)imm)) return;
		if (A.tryImm(SUB, false, d, n, (uint32_t)-(int64_t)imm)) return;
		movw(IP, (uint32_t)imm); A.add(d, n, IP);
	}
	void andimm(int d, int n, uint32_t m) {
		if (m == 0xFFFFFFFFu) { if (d != n) A.mov(d, n); return; }
		if (m == 0) { movw(d, 0); return; }
		if (A.tryImm(AND, false, d, n, m)) return;
		if (A.tryImm(BIC, false, d, n, ~m)) return;
		if ((m & (m + 1)) == 0) { A.ubfx(d, n, 0, 32 - __builtin_clz(m)); return; }   // low bits
		movw(IP, m); A.and_(d, n, IP);
	}
	void orrimm(int d, int n, uint32_t m) { if (!m) { if (d != n) A.mov(d, n); return; } if (!A.tryImm(ORR, false, d, n, m)) { movw(IP, m); A.orr(d, n, IP); } }
	void eorimm(int d, int n, uint32_t m) { if (!m) { if (d != n) A.mov(d, n); return; } if (!A.tryImm(EOR, false, d, n, m)) { movw(IP, m); A.eor(d, n, IP); } }
	void cmpimm(int n, uint32_t v) { if (!A.tryImm(CMP, true, 0, n, v) && !A.tryImm(CMN, true, 0, n, (uint32_t)-(int64_t)(int32_t)v)) { movw(IP, v); A.cmp(n, IP); } }
	void setcr0(int r) { cmpNibble(0, [&] { A.tryImm(CMP, true, 0, r, 0); }, true); }
	template <class Fn> void cmpNibble(int field, Fn cmp, bool signedCmp) {
		cmp();
		A.tryImm(MOV, false, 1, 0, 2);
		A.cc = signedCmp ? LT : CC; A.tryImm(MOV, false, 1, 0, 8);
		A.cc = signedCmp ? GT : HI; A.tryImm(MOV, false, 1, 0, 4); A.cc = AL;
		A.ldr(IP, RC, OFF_XER); A.dpr(ORR, false, 1, 1, IP, LSR, 31);
		A.strb(1, RC, OFF_CR(field));
		flagsField = field; flagsSigned = signedCmp;
	}
	void setCA(int rca) { A.ldr(IP, RC, OFF_XER); A.bfi(IP, rca, 29, 1); A.str(IP, RC, OFF_XER); }   // rca = 0/1
	void crbit(int d, int bi) { A.ldrb(d, RC, OFF_CR(bi >> 2)); A.ubfx(d, d, 3 - (bi & 3), 1); }
	void setcrbit(int bi, int r) { A.ldrb(IP, RC, OFF_CR(bi >> 2)); A.bfi(IP, r, 3 - (bi & 3), 1); A.strb(IP, RC, OFF_CR(bi >> 2)); }
	void eaD(uint32_t ra, int32_t d) {
		eaConst = false;
		if (ra == 0) { eaConst = true; eaVal = (uint32_t)d; movw(0, eaVal); return; }
		auto k = kconst.find((int)ra);
		if (k != kconst.end()) { eaConst = true; eaVal = k->second + (uint32_t)d; movw(0, eaVal); return; }
		ldg(0, ra); addimm(0, 0, d);
	}
	void eaX(uint32_t ra, uint32_t rb) {
		eaConst = false;
		if (ra == 0) ldg(0, rb);
		else {
			int a = host[ra] >= 0 ? host[ra] : (ldg(0, ra), 0), b = host[rb] >= 0 ? host[rb] : (ldg(1, rb), 1);
			if (rec) { if (host[ra] >= 0) useW[ra] += wt; if (host[rb] >= 0) useW[rb] += wt; }
			A.add(0, a, b);
		}
	}
	// RAM check: address < 8 MB and aligned to the access size, else the slow path through the bus
	size_t check(int size) {
		if (size == 0) { A.dpr(MOV, true, IP, 0, 0, LSR, 23); }
		else { A.lsri(IP, 0, 23); A.dpr(ORR, true, IP, IP, 0, LSL, size == 1 ? 31 : 30); }
		return A.b(NE);
	}
	// loads: ea in r0, value to r2 (64-bit: r2 low word, r3 high word). stores take the value there.
	void load(int size, bool sign) {
		uint32_t m = size == 0 ? 0xFF800000u : size == 1 ? 0xFF800001u : 0xFF800003u;
		bool known = eaConst && !(eaVal & m) && (size < 3 || eaVal < 0x7FFFF8u); eaConst = false;
		size_t br = known ? 0 : check(size);
		if (size == 0) {
			if (known) movw(1, eaVal ^ 3); else A.tryImm(EOR, false, 1, 0, 3);
			if (sign) A.xreg(2, 2, RRAM, 1); else A.ldstr(true, true, 2, RRAM, 1);
		} else if (size == 1) {
			if (known) movw(1, eaVal ^ 2); else A.tryImm(EOR, false, 1, 0, 2);
			A.xreg(sign ? 3 : 1, 2, RRAM, 1);
		} else if (size == 2) A.ldstr(true, false, 2, RRAM, 0);
		else { A.add(IP, RRAM, 0); A.ldr(3, IP, 0); A.ldr(2, IP, 4); }
		if (!known) slows.push_back({br, A.pos(), 0, size, sign});
	}
	void store(int size) {
		uint32_t m = size == 0 ? 0xFF800000u : size == 1 ? 0xFF800001u : 0xFF800003u;
		bool known = eaConst && !(eaVal & m) && (size < 3 || eaVal < 0x7FFFF8u); eaConst = false;
		size_t br = known ? 0 : check(size);
		if (size == 0) { if (known) movw(1, eaVal ^ 3); else A.tryImm(EOR, false, 1, 0, 3); A.ldstr(false, true, 2, RRAM, 1); }
		else if (size == 1) { if (known) movw(1, eaVal ^ 2); else A.tryImm(EOR, false, 1, 0, 2); A.xreg(0, 2, RRAM, 1); }
		else if (size == 2) A.ldstr(false, false, 2, RRAM, 0);
		else { A.add(IP, RRAM, 0); A.str(3, IP, 0); A.str(2, IP, 4); }
		if (!known) slows.push_back({br, A.pos(), 1, size, false});
	}
	void emitSlows() {
		for (auto &s : slows) {
			A.patch(s.at, A.pos());
			if (s.kind == 0) {
				A.mov(1, 0); cpuArg(); helper(H_LD8 + s.size);
				if (s.size == 3) { A.mov(2, 0); A.mov(3, 1); }   // returned in r0 (low) / r1 (high)
				else if (s.sign && s.size == 1) A.sxth(2, 0);
				else if (s.sign && s.size == 0) A.sxtb(2, 0);
				else A.mov(2, 0);
			} else {
				A.mov(1, 0);
				if (s.size == 0) A.uxtb(2, 2); else if (s.size == 1) A.uxth(2, 2);
				cpuArg(); helper(H_ST8 + s.size);   // value already in r2 (r2:r3 for 64-bit)
			}
			size_t b = A.b(); A.patch(b, s.back);
		}
	}
	void gotoLabel(uint32_t t, Cond c = AL) { jumps.push_back({A.b(c), t}); }
	void callGuest(uint32_t t) {
		if (E.count(t)) { if (rec) syncW += wt; flushRegs(); kconst.clear(); O.calls.push_back({A.bl(), t}); reloadRegs(); }
		else { cpuArg(); movw(1, t); helper(H_CALL); }
	}
	void callDynamic(int rt) { A.mov(1, rt); cpuArg(); helper(H_CALL); }
	void ret() { toEpilogue.push_back(A.b()); }
	void retAfterCall() { toEpilogueNoWB.push_back(A.b()); }
	bool isEntry(uint32_t t) const { return E.count(t) || (H && H->count(t)); }   // hooked routines are entries too
	bool intra(uint32_t t) { return vis.count(t) && !(isEntry(t) && t != F.entry); }
	// sets the flags for a conditional branch and returns the condition under which it is taken (AL: always)
	Cond branchCond(uint32_t bo, uint32_t bi, bool decCtr) {
		bool ctr = decCtr && !(bo & 4), crb = !(bo & 16);
		uint32_t bit = 8u >> (bi & 3);
		if (ctr) { A.ldr(IP, RC, OFF_CTR); A.tryImm(SUB, true, IP, IP, 1); A.str(IP, RC, OFF_CTR); }
		if (ctr && !crb) return (bo & 2) ? EQ : NE;
		if (crb && !ctr) { A.ldrb(IP, RC, OFF_CR(bi >> 2)); A.tryImm(TST, true, 0, IP, bit); return (bo & 8) ? NE : EQ; }
		if (!ctr && !crb) return AL;
		// both: r3 = ctr condition, then cleared when the CR bit does not match
		Cond cc = (bo & 2) ? EQ : NE;
		A.tryImm(MOV, false, 3, 0, 0); A.cc = cc; A.tryImm(MOV, false, 3, 0, 1); A.cc = AL;
		A.ldrb(IP, RC, OFF_CR(bi >> 2)); A.tryImm(TST, true, 0, IP, bit);
		A.cc = (bo & 8) ? EQ : NE; A.tryImm(MOV, false, 3, 0, 0); A.cc = AL;
		A.tryImm(CMP, true, 0, 3, 0); return NE;
	}
	void insn(const Insn &r);
	void fpArith(uint32_t w, bool single);
	void ldf(int d, uint32_t f) { A.vldrd(d, RC, OFF_F(f)); }
	void stf(int d, uint32_t f) { A.vstrd(d, RC, OFF_F(f)); }
	void fcr1(uint32_t rc) { if (rc) { A.ldr(0, RC, OFF_FPSCR); A.lsri(0, 0, 28); A.strb(0, RC, OFF_CR(1)); } }
};

void FnGen32::insn(const Insn &I)
{
	const uint32_t w = I.w, pc = I.pc, op = I.op, xo = I.xo;
	const uint32_t rt = (w >> 21) & 31, rs = rt, ra = (w >> 16) & 31, rb = (w >> 11) & 31, rc = w & 1;
	const int32_t simm = sx16(w); const uint32_t uimm = w & 0xFFFF;
	auto cr0 = [&](int r) { if (rc) setcr0(r); };
	switch (op) {
	case 14: {
		auto k = kconst.find((int)ra); bool kc = ra == 0 || k != kconst.end(); uint32_t v = (ra == 0 ? 0 : kc ? k->second : 0) + (uint32_t)simm;
		if (kc) movw(0, v); else { ldg(0, ra); addimm(0, 0, simm); }
		stg(0, rt); if (kc) kconst[(int)rt] = v; return;
	}
	case 15: {
		auto k = kconst.find((int)ra); bool kc = ra == 0 || k != kconst.end(); uint32_t v = (ra == 0 ? 0 : kc ? k->second : 0) + ((uint32_t)simm << 16);
		if (kc) movw(0, v); else { ldg(0, ra); addimm(0, 0, (int32_t)((uint32_t)simm << 16)); }
		stg(0, rt); if (kc) kconst[(int)rt] = v; return;
	}
	case 12: case 13:   // addic(.)
		ldg(0, ra); movw(1, (uint32_t)simm); A.adds(0, 0, 1); A.tryImm(MOV, false, 2, 0, 0); A.cc = CS; A.tryImm(MOV, false, 2, 0, 1); A.cc = AL;
		stg(0, rt); setCA(2); if (op == 13) setcr0(0); return;
	case 8:             // subfic: imm - a, CA = imm >= a (unsigned)
		ldg(0, ra); movw(1, (uint32_t)simm); A.subs(2, 1, 0); A.tryImm(MOV, false, 3, 0, 0); A.cc = CS; A.tryImm(MOV, false, 3, 0, 1); A.cc = AL;
		stg(2, rt); setCA(3); return;
	case 7: ldg(0, ra); movw(1, (uint32_t)simm); A.mul(0, 0, 1); stg(0, rt); return;
	case 11: { int f = rt >> 2; ldg(0, ra); cmpNibble(f, [&] { cmpimm(0, (uint32_t)simm); }, true); return; }
	case 10: { int f = rt >> 2; ldg(0, ra); cmpNibble(f, [&] { cmpimm(0, uimm); }, false); return; }
	case 24: case 25: case 26: case 27: case 28: case 29: {
		uint32_t imm = (op == 25 || op == 27 || op == 29) ? uimm << 16 : uimm;
		auto k = kconst.find((int)rs);
		if (op <= 25 && k != kconst.end()) { uint32_t v = k->second | imm; movw(0, v); stg(0, ra); kconst[(int)ra] = v; return; }
		ldg(0, rs);
		if (op <= 25) orrimm(0, 0, imm); else if (op <= 27) eorimm(0, 0, imm); else andimm(0, 0, imm);
		stg(0, ra); if (op >= 28) setcr0(0); return;
	}
	case 21: {   // rlwinm
		uint32_t sh = rb, mb = (w >> 6) & 31, me = (w >> 1) & 31, m = mask32(mb, me);
		int s = host[rs] >= 0 ? host[rs] : 0; if (s == 0) ldg(0, rs); else if (rec) useW[rs] += wt;
		if (mb <= me && me == 31 - sh) { A.lsli(0, s, sh); if (mb) andimm(0, 0, m); }                     // slwi (+ clear high)
		else if (sh && me == 31 && 32 - mb <= sh) A.ubfx(0, s, 32 - sh, 32 - mb);                        // extract (srwi)
		else if (mb <= me && sh == 0) andimm(0, s, m);
		else { A.rori(0, s, 32 - sh); andimm(0, 0, m); }
		stg(0, ra); cr0(0); return;
	}
	case 20: {   // rlwimi
		uint32_t sh = rb, mb = (w >> 6) & 31, me = (w >> 1) & 31, m = mask32(mb, me);
		ldg(0, rs); A.rori(0, 0, 32 - sh); andimm(0, 0, m); ldg(1, ra); andimm(1, 1, ~m); A.orr(0, 0, 1); stg(0, ra); cr0(0); return;
	}
	case 23: {   // rlwnm: rotate left by rb & 31
		uint32_t mb = (w >> 6) & 31, me = (w >> 1) & 31;
		ldg(0, rs); ldg(1, rb); A.tryImm(AND, false, 1, 1, 31); A.tryImm(RSB, false, 1, 1, 32);
		A.dprs(MOV, false, 0, 0, 0, ROR, 1); andimm(0, 0, mask32(mb, me)); stg(0, ra); cr0(0); return;
	}
	case 32: case 33: eaD(ra, simm); if (op == 33) stg(0, ra); load(2, false); stg(2, rt); return;
	case 34: case 35: eaD(ra, simm); if (op == 35) stg(0, ra); load(0, false); stg(2, rt); return;
	case 40: case 41: eaD(ra, simm); if (op == 41) stg(0, ra); load(1, false); stg(2, rt); return;
	case 42: case 43: eaD(ra, simm); if (op == 43) stg(0, ra); load(1, true); stg(2, rt); return;
	case 36: case 37: eaD(ra, simm); ldg(2, rs); if (op == 37) stg(0, ra); store(2); return;
	case 38: case 39: eaD(ra, simm); ldg(2, rs); if (op == 39) stg(0, ra); store(0); return;
	case 44: case 45: eaD(ra, simm); ldg(2, rs); if (op == 45) stg(0, ra); store(1); return;
	case 46: eaD(ra, simm); A.str(0, SP, 0); for (uint32_t i = rt; i < 32; i++) { A.ldr(0, SP, 0); addimm(0, 0, 4 * (int32_t)(i - rt)); load(2, false); stg(2, i); } return;
	case 47: eaD(ra, simm); A.str(0, SP, 0); for (uint32_t i = rs; i < 32; i++) { A.ldr(0, SP, 0); addimm(0, 0, 4 * (int32_t)(i - rs)); ldg(2, i); store(2); } return;
	case 48: case 49: eaD(ra, simm); if (op == 49) stg(0, ra); load(2, false); A.vmov_sr(2, 2); A.vcvt_ds(0, 2); stf(0, rt); return;
	case 50: case 51: eaD(ra, simm); if (op == 51) stg(0, ra); load(3, false); A.str(2, RC, OFF_F(rt)); A.str(3, RC, OFF_F(rt) + 4); return;
	case 52: case 53: eaD(ra, simm); ldf(0, rs); A.vcvt_sd(2, 0); A.vmov_rs(2, 2); if (op == 53) stg(0, ra); store(2); return;
	case 54: case 55: eaD(ra, simm); A.ldr(2, RC, OFF_F(rs)); A.ldr(3, RC, OFF_F(rs) + 4); if (op == 55) stg(0, ra); store(3); return;
	case 17: cpuArg(); movw(1, pc + 4); helper(H_SC); return;
	case 19:
		switch (xo) {
		case 0: A.ldrb(0, RC, OFF_CR(ra >> 2)); A.strb(0, RC, OFF_CR(rt >> 2)); return;
		case 33: case 129: case 193: case 225: case 257: case 289: case 417: case 449:
			crbit(2, ra); crbit(3, rb);
			switch (xo) {
			case 33: A.orr(0, 2, 3); A.tryImm(EOR, false, 0, 0, 1); break;
			case 129: A.bic(0, 2, 3); break;
			case 193: A.eor(0, 2, 3); break;
			case 225: A.and_(0, 2, 3); A.tryImm(EOR, false, 0, 0, 1); break;
			case 257: A.and_(0, 2, 3); break;
			case 289: A.eor(0, 2, 3); A.tryImm(EOR, false, 0, 0, 1); break;
			case 417: A.tryImm(EOR, false, 3, 3, 1); A.orr(0, 2, 3); break;
			case 449: A.orr(0, 2, 3); break;
			}
			setcrbit(rt, 0); return;
		case 150: return;
		}
		break;
	case 31:
		switch (xo) {
		case 23: case 55: eaX(ra, rb); if (xo == 55) stg(0, ra); load(2, false); stg(2, rt); return;
		case 87: case 119: eaX(ra, rb); if (xo == 119) stg(0, ra); load(0, false); stg(2, rt); return;
		case 279: case 311: eaX(ra, rb); if (xo == 311) stg(0, ra); load(1, false); stg(2, rt); return;
		case 343: case 375: eaX(ra, rb); if (xo == 375) stg(0, ra); load(1, true); stg(2, rt); return;
		case 20: eaX(ra, rb); load(2, false); stg(2, rt); return;
		case 534: eaX(ra, rb); load(2, false); A.rev(2, 2); stg(2, rt); return;
		case 790: eaX(ra, rb); load(1, false); A.rev16(2, 2); A.uxth(2, 2); stg(2, rt); return;
		case 151: case 183: eaX(ra, rb); ldg(2, rs); if (xo == 183) stg(0, ra); store(2); return;
		case 215: case 247: eaX(ra, rb); ldg(2, rs); if (xo == 247) stg(0, ra); store(0); return;
		case 407: case 439: eaX(ra, rb); ldg(2, rs); if (xo == 439) stg(0, ra); store(1); return;
		case 662: eaX(ra, rb); ldg(2, rs); A.rev(2, 2); store(2); return;
		case 918: eaX(ra, rb); ldg(2, rs); A.rev16(2, 2); store(1); return;
		case 535: case 567: eaX(ra, rb); if (xo == 567) stg(0, ra); load(2, false); A.vmov_sr(2, 2); A.vcvt_ds(0, 2); stf(0, rt); return;
		case 599: case 631: eaX(ra, rb); if (xo == 631) stg(0, ra); load(3, false); A.str(2, RC, OFF_F(rt)); A.str(3, RC, OFF_F(rt) + 4); return;
		case 663: case 695: eaX(ra, rb); ldf(0, rs); A.vcvt_sd(2, 0); A.vmov_rs(2, 2); if (xo == 695) stg(0, ra); store(2); return;
		case 727: case 759: eaX(ra, rb); A.ldr(2, RC, OFF_F(rs)); A.ldr(3, RC, OFF_F(rs) + 4); if (xo == 759) stg(0, ra); store(3); return;
		case 983: eaX(ra, rb); A.ldr(2, RC, OFF_F(rs)); store(2); return;
		case 0: { int f = rt >> 2; ldg(0, ra); ldg(1, rb); cmpNibble(f, [&] { A.cmp(0, 1); }, true); return; }
		case 32: { int f = rt >> 2; ldg(0, ra); ldg(1, rb); cmpNibble(f, [&] { A.cmp(0, 1); }, false); return; }
		case 266: ldg(0, ra); ldg(1, rb); A.add(0, 0, 1); stg(0, rt); cr0(0); return;
		case 40: ldg(0, ra); ldg(1, rb); A.sub(0, 1, 0); stg(0, rt); cr0(0); return;
		case 10: ldg(0, ra); ldg(1, rb); A.adds(0, 0, 1); A.tryImm(MOV, false, 2, 0, 0); A.cc = CS; A.tryImm(MOV, false, 2, 0, 1); A.cc = AL; stg(0, rt); setCA(2); cr0(0); return;
		case 8: ldg(0, ra); ldg(1, rb); A.subs(0, 1, 0); A.tryImm(MOV, false, 2, 0, 0); A.cc = CS; A.tryImm(MOV, false, 2, 0, 1); A.cc = AL; stg(0, rt); setCA(2); cr0(0); return;
		case 138: case 136: case 202: case 234: case 200: case 232: {
			// a' = a or ~a; b' = rb, 0 or 0xFFFFFFFF; s = a' + b' + CA, carry out to CA
			ldg(0, ra); if (xo == 136 || xo == 200 || xo == 232) A.mvn(0, 0);
			if (xo == 138 || xo == 136) ldg(1, rb); else if (xo == 202 || xo == 200) movw(1, 0); else movw(1, 0xFFFFFFFFu);
			A.ldr(IP, RC, OFF_XER); A.dpr(MOV, true, IP, 0, IP, LSL, 3);   // carry flag = XER.CA (bit 29 shifted out)
			A.dpr(ADC, true, 3, 0, 1);
			A.tryImm(MOV, false, 2, 0, 0); A.cc = CS; A.tryImm(MOV, false, 2, 0, 1); A.cc = AL;
			stg(3, rt); setCA(2); cr0(3); return;
		}
		case 104: ldg(0, ra); A.tryImm(RSB, false, 0, 0, 0); stg(0, rt); cr0(0); return;
		case 235: ldg(0, ra); ldg(1, rb); A.mul(0, 0, 1); stg(0, rt); cr0(0); return;
		case 75: ldg(0, ra); ldg(1, rb); A.smull(2, 3, 0, 1); stg(3, rt); cr0(3); return;
		case 11: ldg(0, ra); ldg(1, rb); A.umull(2, 3, 0, 1); stg(3, rt); cr0(3); return;
		case 491: {   // divw with the reference's handling of the undefined cases
			ldg(0, ra); ldg(1, rb);
			if (!a32HasIdiv) { helper(H_DIVW); A.mov(2, 0); stg(2, rt); cr0(2); return; }
			A.tryImm(CMP, true, 0, 1, 0); size_t z = A.b(EQ);
			A.tryImm(CMN, true, 0, 1, 1); size_t n1 = A.b(NE);
			A.tryImm(CMP, true, 0, 0, 0x80000000u); size_t ov = A.b(EQ);
			A.patch(n1, A.pos());
			A.sdiv(2, 0, 1); size_t done1 = A.b();
			A.patch(z, A.pos()); A.asri(2, 0, 31); size_t done2 = A.b();
			A.patch(ov, A.pos()); A.tryImm(MVN, false, 2, 0, 0);
			A.patch(done1, A.pos()); A.patch(done2, A.pos());
			stg(2, rt); cr0(2); return;
		}
		case 459:
			ldg(0, ra); ldg(1, rb);
			if (!a32HasIdiv) { helper(H_DIVWU); A.mov(2, 0); }
			else A.udiv(2, 0, 1);   // /0 = 0, as the reference
			stg(2, rt); cr0(2); return;
		case 28: ldg(0, rs); ldg(1, rb); A.and_(0, 0, 1); stg(0, ra); cr0(0); return;
		case 60: ldg(0, rs); ldg(1, rb); A.bic(0, 0, 1); stg(0, ra); cr0(0); return;
		case 444: ldg(0, rs); if (rs != rb) { ldg(1, rb); A.orr(0, 0, 1); } stg(0, ra); cr0(0); return;
		case 412: ldg(0, rs); ldg(1, rb); A.mvn(1, 1); A.orr(0, 0, 1); stg(0, ra); cr0(0); return;
		case 316: ldg(0, rs); ldg(1, rb); A.eor(0, 0, 1); stg(0, ra); cr0(0); return;
		case 476: ldg(0, rs); ldg(1, rb); A.and_(0, 0, 1); A.mvn(0, 0); stg(0, ra); cr0(0); return;
		case 124: ldg(0, rs); ldg(1, rb); A.orr(0, 0, 1); A.mvn(0, 0); stg(0, ra); cr0(0); return;
		case 284: ldg(0, rs); ldg(1, rb); A.eor(0, 0, 1); A.mvn(0, 0); stg(0, ra); cr0(0); return;
		case 24: case 536:   // slw / srw: counts 32..63 give 0 (ARM shifts by the low byte: keep 6 bits)
			ldg(0, rs); ldg(1, rb); A.tryImm(AND, false, 1, 1, 63);
			A.dprs(MOV, false, 2, 0, 0, xo == 24 ? LSL : LSR, 1); stg(2, ra); cr0(2); return;
		case 792: {   // sraw
			ldg(0, rs); ldg(1, rb); A.tryImm(AND, false, 1, 1, 63);
			A.dprs(MOV, false, 2, 0, 0, ASR, 1);
			A.tryImm(MVN, false, IP, 0, 0); A.dprs(MOV, false, IP, 0, IP, LSL, 1);   // bits kept: ~0 << n (0 for n >= 32)
			A.dpr(BIC, true, IP, 0, IP);                                             // bits shifted out
			A.tryImm(MOV, false, 3, 0, 0); A.cc = NE; A.tryImm(MOV, false, 3, 0, 1); A.cc = AL;
			A.dpr(AND, false, 3, 3, 0, LSR, 31);
			stg(2, ra); setCA(3); cr0(2); return;
		}
		case 824: {   // srawi
			uint32_t sh = rb; ldg(0, rs);
			A.asri(2, 0, sh);
			if (sh) { andimm(3, 0, (1u << sh) - 1); A.tryImm(CMP, true, 0, 3, 0); A.tryImm(MOV, false, 3, 0, 0); A.cc = NE; A.tryImm(MOV, false, 3, 0, 1); A.cc = AL; A.dpr(AND, false, 3, 3, 0, LSR, 31); }
			else movw(3, 0);
			stg(2, ra); setCA(3); cr0(2); return;
		}
		case 26: ldg(0, rs); A.clz(0, 0); stg(0, ra); cr0(0); return;
		case 954: ldg(0, rs); A.sxtb(0, 0); stg(0, ra); cr0(0); return;
		case 922: ldg(0, rs); A.sxth(0, 0); stg(0, ra); cr0(0); return;
		case 19: cpuArg(); helper(H_MFCR); stg(0, rt); return;
		case 144: ldg(2, rs); cpuArg(); movw(1, (w >> 12) & 0xFF); helper(H_MTCRF); return;
		case 83: A.ldr(0, RC, OFF_MSR); stg(0, rt); return;
		case 146: ldg(0, rs); A.str(0, RC, OFF_MSR); return;
		case 595: A.ldr(0, RC, OFF_SR(ra & 15)); stg(0, rt); return;
		case 210: ldg(0, rs); A.str(0, RC, OFF_SR(ra & 15)); return;
		case 242: ldg(0, rb); A.lsri(0, 0, 28); ldg(1, rs); A.dpr(ADD, false, IP, RC, 0, LSL, 2); A.str(1, IP, OFF_SR(0)); return;
		case 659: ldg(0, rb); A.lsri(0, 0, 28); A.dpr(ADD, false, IP, RC, 0, LSL, 2); A.ldr(1, IP, OFF_SR(0)); stg(1, rt); return;
		case 339: case 467: case 371: {
			uint32_t spr = ((w >> 16) & 31) | (((w >> 11) & 31) << 5); bool rd = xo != 467;
			if (xo == 371) spr = ra == 12 ? 268 : 269;
			if (rd) {
				if (spr == 22 || spr == 268 || spr == 269 || spr == 284 || spr == 285) { cpuArg(); movw(1, spr); movw(2, rt); helper(H_TBREAD); stg(0, rt); }
				else if (spr == 1) { A.ldr(0, RC, OFF_XER); stg(0, rt); }
				else if (spr == 8) { A.ldr(0, RC, OFF_LR); stg(0, rt); }
				else if (spr == 9) { A.ldr(0, RC, OFF_CTR); stg(0, rt); }
				else { A.ldr(0, RC, OFF_SPR(spr)); stg(0, rt); }
			} else {
				ldg(0, rs);
				if (spr == 1) A.str(0, RC, OFF_XER);
				else if (spr == 8) A.str(0, RC, OFF_LR);
				else if (spr == 9) A.str(0, RC, OFF_CTR);
				else if (spr == 22 || spr == 268 || spr == 269 || spr == 284 || spr == 285) { A.mov(2, 0); cpuArg(); movw(1, spr); helper(H_MTSPR); }
				else A.str(0, RC, OFF_SPR(spr));
			}
			return;
		}
		case 598: case 854: case 86: case 54: case 246: case 278: case 982: case 470: case 306: case 566: case 1014: case 274: return;
		}
		break;
	case 59: case 63: fpArith(w, op == 59); return;
	}
	cpuArg(); movw(1, pc); movw(2, w); helper(H_UNIMPL);
}

void FnGen32::fpArith(uint32_t w, bool single)
{
	const uint32_t op = w >> 26, rt = (w >> 21) & 31, fa = (w >> 16) & 31, fb = (w >> 11) & 31, fc = (w >> 6) & 31;
	const uint32_t xo = (w >> 1) & 0x3ff, xo5 = (w >> 1) & 0x1f, rc = w & 1;
	auto round1 = [&](int d) { if (single) { A.vcvt_sd(2 * d, d); A.vcvt_ds(d, 2 * d); } };
	auto one = [&](int d) { movw(0, 0); movw(1, 0x3FF00000u); A.vmov_drr(d, 0, 1); };
	static const uint16_t ex[] = {0, 12, 14, 15, 32, 38, 40, 64, 70, 72, 134, 136, 264, 583, 711};
	bool excl = false; if (op == 63) for (uint16_t e : ex) if (xo == e) excl = true;
	bool arith = (xo5 == 18 || xo5 == 20 || xo5 == 21 || xo5 == 25 || xo5 == 28 || xo5 == 29 || xo5 == 30 || xo5 == 31 || xo5 == 24 || xo5 == 23 || xo5 == 26) && !excl;
	if (arith) {
		if ((xo5 >= 28 && xo5 <= 31) && !a32HasVfp4) {   // fused multiply-add in software
			cpuArg(); movw(1, rt | fa << 5 | fb << 10 | fc << 15 | (xo5 - 28) << 20 | (single ? 1u << 22 : 0)); helper(H_FMA); fcr1(rc); return;
		}
		switch (xo5) {
		case 21: ldf(0, fa); ldf(1, fb); A.vadd(0, 0, 1); break;
		case 20: ldf(0, fa); ldf(1, fb); A.vsub(0, 0, 1); break;
		case 18: ldf(0, fa); ldf(1, fb); A.vdiv(0, 0, 1); break;
		case 25: ldf(0, fa); ldf(1, fc); A.vmul(0, 0, 1); break;
		case 29: ldf(0, fb); ldf(1, fa); ldf(2, fc); A.vfma(0, 1, 2); break;               // a*c + b
		case 28: ldf(0, fb); ldf(1, fa); ldf(2, fc); A.vfnms(0, 1, 2); break;              // a*c - b
		case 31: ldf(0, fb); ldf(1, fa); ldf(2, fc); A.vfma(0, 1, 2); A.vneg(0, 0); break;  // -(a*c + b)
		case 30: ldf(0, fb); ldf(1, fa); ldf(2, fc); A.vfnms(0, 1, 2); A.vneg(0, 0); break; // -(a*c - b)
		case 23: ldf(0, fa); ldf(1, fb); ldf(2, fc); A.vcmpz(0); A.vmrs(); A.vmovd(3, 1); A.cc = GE; A.vmovd(3, 2); A.cc = AL; stf(3, rt); fcr1(rc); return;
		case 24: ldf(1, fb); one(0); A.vdiv(0, 0, 1); break;
		case 26: ldf(1, fb); A.vsqrt(1, 1); one(0); A.vdiv(0, 0, 1); stf(0, rt); fcr1(rc); return;
		}
		round1(0); stf(0, rt); fcr1(rc); return;
	}
	switch (xo) {
	case 0: case 32: {
		int f = rt >> 2; ldf(0, fa); ldf(1, fb); A.vcmp(0, 1); A.vmrs();
		A.tryImm(MOV, false, 1, 0, 2); A.cc = MI; A.tryImm(MOV, false, 1, 0, 8); A.cc = GT; A.tryImm(MOV, false, 1, 0, 4);
		A.cc = VS; A.tryImm(MOV, false, 1, 0, 1); A.cc = AL;
		A.strb(1, RC, OFF_CR(f)); return;
	}
	case 12: ldf(0, fb); round1(0); if (!single) { A.vcvt_sd(0, 0); A.vcvt_ds(0, 0); } stf(0, rt); fcr1(rc); return;
	case 15: case 14: {   // fctiwz / fctiw: low word = saturated int; high word 0, or all ones below -2^31 and for NaN
		bool rz = xo == 15;
		ldf(0, fb); A.vcvt_s32d(2, 0, rz); A.vmov_rs(0, 2); movw(1, 0);
		A.vcmp(0, 0); A.vmrs(); A.cc = VS; A.tryImm(MOV, false, 0, 0, 0x80000000u); A.tryImm(MVN, false, 1, 0, 0); A.cc = AL;
		movw(2, rz ? 0x00200000u : 0x00100000u); movw(3, 0xC1E00000u); A.vmov_drr(1, 2, 3);   // -2^31-1 / -2^31-0.5
		A.vcmp(0, 1); A.vmrs(); A.cc = rz ? LS : MI; A.tryImm(MVN, false, 1, 0, 0); A.cc = AL;
		A.str(0, RC, OFF_F(rt)); A.str(1, RC, OFF_F(rt) + 4); fcr1(rc); return;
	}
	case 72: ldf(0, fb); stf(0, rt); fcr1(rc); return;
	case 40: case 264: case 136:
		A.ldr(0, RC, OFF_F(fb)); A.ldr(1, RC, OFF_F(fb) + 4);
		if (xo == 40) A.tryImm(EOR, false, 1, 1, 0x80000000u); else if (xo == 264) A.tryImm(BIC, false, 1, 1, 0x80000000u); else A.tryImm(ORR, false, 1, 1, 0x80000000u);
		A.str(0, RC, OFF_F(rt)); A.str(1, RC, OFF_F(rt) + 4); fcr1(rc); return;
	case 583: A.ldr(0, RC, OFF_FPSCR); movw(1, 0); A.str(0, RC, OFF_F(rt)); A.str(1, RC, OFF_F(rt) + 4); fcr1(rc); return;
	case 711: A.ldr(2, RC, OFF_F(fb)); cpuArg(); movw(1, (w >> 17) & 0xFF); helper(H_MTFSF); fcr1(rc); return;
	}
	cpuArg(); movw(1, 0); movw(2, w); helper(H_UNIMPL);
}

void FnGen32::run()
{
	vis.insert(F.pcs.begin(), F.pcs.end());
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
	if (rec) {
		for (uint32_t pc : F.pcs) {
			const Insn *r = P.insn(pc);
			if ((r->ctl == C_JMP || r->ctl == C_COND) && !r->link && r->target <= pc && vis.count(r->target))
				for (uint32_t q = r->target; q <= pc; q += 4) if (vis.count(q)) loopPcs.insert(q);
		}
	}
	O.entry[F.entry] = A.pos();
	A.push((1u << 0) | (1u << LR));   // [sp]: scratch slot (lmw/stmw base); keeps the stack 8-byte aligned
	reloadRegs();
	if (F.pcs.front() != F.entry) gotoLabel(F.entry);   // the body is laid out by address: code below the entry comes first
	for (uint32_t pc : F.pcs) {
		const Insn &I = *P.insn(pc);
		wt = loopPcs.count(pc) ? 8 : 1;
		labelPos[pc] = A.pos();
		int ff = flagsField; flagsField = -1;
		if (labels.count(pc) || blocks.count(pc) || pc == F.entry) ff = -1;
		if (blocks.count(pc)) {
			kconst.clear();
			if (!A.tryImm(SUB, true, RCYC, RCYC, (uint32_t)cost[pc])) { movw(IP, (uint32_t)cost[pc]); A.subs(RCYC, RCYC, IP); }
			if (backs.count(pc) || pc == F.entry) {
				size_t skip = A.b(PL);
				cpuArg(); movw(1, pc); helper(H_SAFEPOINT);
				A.patch(skip, A.pos());
			}
		}
		uint32_t bo = (I.w >> 21) & 31, bi = (I.w >> 16) & 31;
		if (I.ctl == C_NONE) { insn(I); continue; }
		if (I.ctl == C_RFI) { A.ldr(0, RC, OFF_SPR(27)); A.str(0, RC, OFF_MSR); cpuArg(); helper(H_RFI); retAfterCall(); continue; }
		if (I.ctl == C_JMP) {
			if (I.link) { movw(0, pc + 4); A.str(0, RC, OFF_LR); callGuest(I.target); }
			else if (intra(I.target)) gotoLabel(I.target);
			else { callGuest(I.target); retAfterCall(); }
			continue;
		}
		Cond c = AL;
		if (I.ctl == C_COND && !I.link && (bo & 4) && !(bo & 16) && (int)(bi >> 2) == ff && (bi & 3) != 3) {
			c = (bi & 3) == 0 ? (flagsSigned ? LT : CC) : (bi & 3) == 1 ? (flagsSigned ? GT : HI) : EQ;   // flags of the compare just made
			if (!((bo >> 3) & 1)) c = invert(c);
		} else if (I.ctl == C_COND) c = branchCond(bo, bi, true);
		if (I.ctl == C_COND) {
			if (I.link) { movw(0, pc + 4); A.str(0, RC, OFF_LR); }
			if (!I.link && intra(I.target)) { gotoLabel(I.target, c); continue; }
			size_t skip = c != AL ? A.b(invert(c)) : (size_t)-1;
			if (I.link) callGuest(I.target);
			else { callGuest(I.target); retAfterCall(); }
			if (c != AL) A.patch(skip, A.pos());
			continue;
		}
		if (I.ctl == C_RET) {   // target = lr & ~3, read before the link register is replaced
			A.ldr(2, RC, OFF_LR); A.tryImm(BIC, false, 2, 2, 3);
			c = branchCond(bo, bi, true);
			if (I.link) { movw(0, pc + 4); A.str(0, RC, OFF_LR); }
			size_t skip = c != AL ? A.b(invert(c)) : (size_t)-1;
			if (I.link) callDynamic(2); else ret();
			if (c != AL) A.patch(skip, A.pos());
			continue;
		}
		if (I.ctl == C_IND) {
			A.ldr(2, RC, OFF_CTR); A.tryImm(BIC, false, 2, 2, 3);
			c = branchCond(bo, bi, false);
			if (I.link) { movw(0, pc + 4); A.str(0, RC, OFF_LR); }
			size_t skip = c != AL ? A.b(invert(c)) : (size_t)-1;
			if (I.link) callDynamic(2);
			else {
				auto it = F.sites.find(pc);
				if (it != F.sites.end()) for (uint32_t t : it->second) {
					cmpimm(2, t);
					if (intra(t)) gotoLabel(t, EQ);
					else { size_t ne = A.b(NE); callGuest(t); retAfterCall(); A.patch(ne, A.pos()); }
				}
				callDynamic(2); retAfterCall();
			}
			if (c != AL) A.patch(skip, A.pos());
			continue;
		}
	}
	size_t ep = A.pos();
	flushRegs();
	size_t ep2 = A.pos();
	A.pop((1u << 0) | (1u << PC));
	for (size_t at : toEpilogue) A.patch(at, ep);
	for (size_t at : toEpilogueNoWB) A.patch(at, ep2);
	for (auto &j : jumps) A.patch(j.first, labelPos.at(j.second));
	emitSlows();
}

void translateAll32(Program &p, A32Code &out, const std::set<uint32_t> &overridden)
{
	std::set<uint32_t> entries;
	for (auto &f : p.funcs) if (!overridden.count(f.first)) entries.insert(f.first);
	for (auto &f : p.funcs) {
		A32Code scratch;
		FnGen32 g0(p, scratch, f.second, entries); g0.H = &overridden; g0.rec = true; g0.run();
		std::vector<std::pair<double, int>> cand;
		for (auto &u : g0.useW) {
			double cost = 1 + (g0.written.count(u.first) ? 1 : 0) * (1 + g0.syncW) + g0.syncW;
			if (u.second > cost + 1) cand.push_back({u.second - cost, u.first});
		}
		std::sort(cand.rbegin(), cand.rend());
		FnGen32 g(p, out, f.second, entries); g.H = &overridden;
		g.written = g0.written;
		for (size_t i = 0; i < cand.size() && i < 4; i++) { g.host[cand[i].second] = 8 + (int)i; g.assigned.push_back(cand[i].second); }
		g.run();
	}
	for (auto &c : out.calls) out.a.patch(c.at, out.entry.at(c.target));
}

}
