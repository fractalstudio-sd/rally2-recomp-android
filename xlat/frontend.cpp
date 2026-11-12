// See frontend.h. GPLv3.
#include "frontend.h"
#include <cstdio>
#include <cstdlib>
#include <algorithm>

namespace xl {

static inline int32_t sx16(uint32_t v) { return (int32_t)(int16_t)(v & 0xFFFF); }
static inline int32_t sx26(uint32_t v) { v &= 0x03FFFFFC; return (v & 0x02000000) ? (int32_t)(v | 0xFC000000) : (int32_t)v; }

// Which instructions the translator handles (the same set as the reference C lifter).
static bool isSupported(uint32_t w)
{
	uint32_t op = w >> 26, ra = (w >> 16) & 31, xo = (w >> 1) & 0x3ff, xo5 = (w >> 1) & 0x1f;
	switch (op) {
	case 7: case 8: case 10: case 11: case 12: case 13: case 14: case 15: case 16: case 18: case 20: case 21: case 23:
	case 24: case 25: case 26: case 27: case 28: case 29: case 46: case 47:
	case 32: case 34: case 36: case 38: case 40: case 42: case 44: case 48: case 50: case 52: case 54:
		return true;
	case 33: case 35: case 37: case 39: case 41: case 43: case 45: case 49: case 51: case 53: case 55:
		return ra != 0;                                         // update forms need a base register
	case 17: return (w & 2) != 0;                                // sc
	case 19:
		switch (xo) { case 16: case 528: case 0: case 33: case 129: case 193: case 225: case 257: case 289: case 417: case 449: case 150: case 50: return true; }
		return false;
	case 31:
		switch (xo) {
		case 55: case 119: case 311: case 375: case 183: case 247: case 439: case 567: case 631: case 695: case 759: return ra != 0;
		case 23: case 87: case 279: case 343: case 20: case 534: case 790: case 151: case 215: case 407: case 662: case 918:
		case 535: case 599: case 663: case 727: case 983: case 0: case 32: case 266: case 40: case 10: case 8: case 138: case 136:
		case 202: case 234: case 200: case 232: case 104: case 235: case 75: case 11: case 491: case 459: case 28: case 60: case 444:
		case 412: case 316: case 476: case 124: case 284: case 24: case 536: case 792: case 824: case 26: case 954: case 922: case 19:
		case 144: case 83: case 146: case 595: case 210: case 242: case 659: case 339: case 467: case 371:
		case 598: case 854: case 86: case 54: case 246: case 278: case 982: case 470: case 306: case 566: case 1014: case 274:
			return true;
		}
		return false;
	case 59: case 63: {
		static const uint16_t ex[] = {0, 12, 14, 15, 32, 38, 40, 64, 70, 72, 134, 136, 264, 583, 711};
		bool excl = false; if (op == 63) for (uint16_t e : ex) if (xo == e) excl = true;
		bool arith = (xo5 == 18 || xo5 == 20 || xo5 == 21 || xo5 == 25 || xo5 == 28 || xo5 == 29 || xo5 == 30 || xo5 == 31 || xo5 == 24 || xo5 == 23 || xo5 == 26) && !excl;
		if (arith) {
			if (xo5 == 23 || xo5 == 26) return op == 63;
			if (xo5 == 24) return op == 59;
			return true;
		}
		if (op == 63) switch (xo) { case 0: case 32: case 12: case 15: case 14: case 72: case 40: case 264: case 136: case 583: case 711: return true; }
		return false;
	}
	}
	return false;
}

Insn decode(uint32_t pc, uint32_t w)
{
	Insn i; i.pc = pc; i.w = w; i.op = (uint8_t)(w >> 26); i.xo = (uint16_t)((w >> 1) & 0x3ff);
	i.supported = isSupported(w);
	if (!i.supported) return i;
	uint32_t bo = (w >> 21) & 31;
	if (i.op == 18) { int32_t li = sx26(w); i.target = (w & 2) ? (uint32_t)li : pc + (uint32_t)li; i.ctl = C_JMP; i.link = w & 1; i.uncond = true; }
	else if (i.op == 16) { int32_t bd = sx16(w & 0xFFFC); i.target = (w & 2) ? (uint32_t)bd : pc + (uint32_t)bd; i.ctl = C_COND; i.link = w & 1; }
	else if (i.op == 19 && i.xo == 16) { i.ctl = C_RET; i.link = w & 1; i.uncond = (bo & 4) && (bo & 16); }
	else if (i.op == 19 && i.xo == 528) { i.ctl = C_IND; i.link = w & 1; i.uncond = (bo & 16) != 0; }
	else if (i.op == 19 && i.xo == 50) { i.ctl = C_RFI; }
	return i;
}

bool loadImage(const std::string &path, uint32_t base, Image &img, size_t maxBytes)
{
	FILE *f = fopen(path.c_str(), "rb"); if (!f) return false;
	std::vector<uint8_t> b; uint8_t buf[65536]; size_t n;
	while ((n = fread(buf, 1, sizeof buf, f)) > 0) b.insert(b.end(), buf, buf + n);
	fclose(f);
	if (b.size() > maxBytes) b.resize(maxBytes);
	img.base = base; img.w.resize(b.size() / 4);
	for (size_t k = 0; k < img.w.size(); k++) img.w[k] = (uint32_t)b[4 * k] << 24 | (uint32_t)b[4 * k + 1] << 16 | (uint32_t)b[4 * k + 2] << 8 | b[4 * k + 3];
	return true;
}

bool Program::word(uint32_t a, uint32_t &v) const
{
	for (auto &s : code) if (a >= s.base && (uint64_t)a < (uint64_t)s.base + 4ull * s.w.size()) { v = s.w[(a - s.base) / 4]; return true; }
	return false;
}
bool Program::dataWord(uint32_t a, uint32_t &v) const
{
	if (word(a, v)) return true;
	for (auto &s : data) if (a >= s.base && (uint64_t)a < (uint64_t)s.base + 4ull * s.w.size()) { v = s.w[(a - s.base) / 4]; return true; }
	return false;
}
const Insn *Program::insn(uint32_t pc)
{
	auto it = cache_.find(pc); if (it != cache_.end()) return &it->second;
	uint32_t w; if ((pc & 3) || !word(pc, w)) return nullptr;
	return &(cache_[pc] = decode(pc, w));
}

static bool endsFlow(uint32_t w) { return w == 0x4E800020 || w == 0x4E800420 || w == 0x4C000064 || w == 0 || w == 0x60000000 || ((w >> 26) == 18 && !(w & 1)); }

std::set<uint32_t> Program::seeds() const
{
	std::set<uint32_t> s;
	for (auto &seg : code) for (int i = 1; i <= 20; i++) s.insert(seg.base + 0x100u * i);   // exception vectors
	const Image &p = code[0]; uint32_t hi = 4u * (uint32_t)p.w.size();
	auto isPtr = [&](uint32_t w) { return !(w & 3) && w >= 0x100 && w < hi && p.w[w / 4] != 0; };
	auto prevEnds = [&](uint32_t a) { return a >= 4 && endsFlow(p.w[a / 4 - 1]); };
	for (auto &d : data) {                                                  // (a) pointer words in the data image, beyond the program
		for (size_t i = 0; i < d.w.size(); i++) {
			uint32_t a = d.base + 4u * (uint32_t)i; if (a < hi) continue;
			uint32_t w = d.w[i]; if (!isPtr(w)) continue;
			int run = 1; size_t j = i + 1; while (j < d.w.size() && isPtr(d.w[j]) && run < 3) { run++; j++; }
			if (prevEnds(w) || run >= 3) s.insert(w);
		}
	}
	for (size_t i = 0; i + 4 < p.w.size(); i++) {                           // (b) lis + addi/ori constants in the code
		uint32_t w = p.w[i];
		if ((w >> 26) == 15 && ((w >> 16) & 31) == 0) {
			uint32_t rd = (w >> 21) & 31, h16 = (w & 0xFFFF) << 16;
			for (int j = 1; j < 5; j++) {
				uint32_t v = p.w[i + j];
				if (((v >> 26) == 14 || (v >> 26) == 24) && ((v >> 16) & 31) == rd) {
					uint32_t val = (v >> 26) == 14 ? h16 + (uint32_t)sx16(v) : (h16 | (v & 0xFFFF));
					if (isPtr(val) && prevEnds(val)) s.insert(val);
					break;
				}
			}
		}
	}
	for (size_t i = 0; i < p.w.size(); i++) {                               // (c) runs of pointer words inside the program image
		uint32_t w = p.w[i];
		if (isPtr(w) && prevEnds(w)) {
			int run = 1; size_t j = i + 1; while (j < p.w.size() && isPtr(p.w[j]) && run < 3) { run++; j++; }
			if (run >= 3) s.insert(w);
		}
	}
	for (size_t i = 1; i < p.w.size(); i++)                                 // (d) function prologues right after the end of a previous routine
		if (endsFlow(p.w[i - 1]) && ((p.w[i] >> 16) == 0x9421 || p.w[i] == 0x7C0802A6)) s.insert(4u * (uint32_t)i);
	return s;
}

// Jump tables: base address built just before the bctr (lis/addi/ori), relative or absolute entries.
std::set<uint32_t> Program::tableTargets(uint32_t pc)
{
	std::map<uint32_t, int64_t> known; std::vector<uint32_t> Ks;
	auto get = [&](uint32_t r) -> int64_t { auto it = known.find(r); return it == known.end() ? -1 : it->second; };
	for (uint32_t a = pc - 160; a != pc; a += 4) {
		uint32_t w; if (!word(a, w)) continue;
		uint32_t op = w >> 26, rd = (w >> 21) & 31, ra = (w >> 16) & 31, imm = w & 0xFFFF; int32_t simm = sx16(imm);
		if (op == 15) {
			int64_t b = ra == 0 ? 0 : get(ra);
			known[rd] = (ra == 0 || b >= 0) ? (int64_t)(((uint32_t)b + ((uint32_t)simm << 16)) & 0xFFFFFFFF) : -1;
		} else if (op == 14) {
			int64_t b = ra == 0 ? 0 : get(ra);
			known[rd] = b < 0 ? -1 : (int64_t)(((uint32_t)b + (uint32_t)simm) & 0xFFFFFFFF);
			if (known[rd] >= 0) Ks.push_back((uint32_t)known[rd]);
		} else if (op == 24) {
			int64_t b = get(ra); uint32_t d = (w >> 16) & 31;   // as the reference lifter does (destination field read as ra)
			known[d] = b < 0 ? -1 : (int64_t)((uint32_t)b | imm);
			if (known[d] >= 0) Ks.push_back((uint32_t)known[d]);
		}
	}
	std::set<uint32_t> out; std::set<uint32_t> seen;
	for (uint32_t K : Ks) {
		if (!seen.insert(K).second) continue;
		uint32_t t0; if ((K & 3) || !dataWord(K, t0)) continue;
		for (int kind = 0; kind < 2; kind++) {
			std::vector<uint32_t> tg;
			for (int i = 0; i < 1024; i++) {
				uint32_t wv; if (!dataWord(K + 4u * i, wv)) break;
				uint32_t t = kind == 0 ? K + wv : wv, tw;
				if ((t & 3) || !word(t, tw) || tw == 0) break;
				if ((t > pc ? t - pc : pc - t) > 0x20000) break;
				tg.push_back(t);
			}
			if (tg.size() >= 2) { out.insert(tg.begin(), tg.end()); break; }
		}
	}
	return out;
}

static bool terminal(const Insn &r)
{
	switch (r.ctl) {
	case C_RFI: return true;
	case C_JMP: return !r.link;
	case C_RET: return !r.link && r.uncond;
	default: return false;
	}
}

// Recursive descent to a fixpoint: first the set of function entries (seeds + every call target, transitively), then each
// body with that final set (a branch to another entry is a tail call, not part of the body).
void Program::discover(const std::set<uint32_t> &seedSet)
{
	std::set<uint32_t> entries;
	for (uint32_t s : seedSet) if (insn(s)) entries.insert(s);
	std::map<uint32_t, std::set<uint32_t>> sitesCache;
	auto bodyOf = [&](uint32_t e, std::set<uint32_t> &vis, std::set<uint32_t> &calls, std::map<uint32_t, std::vector<uint32_t>> *sites) {
		std::vector<uint32_t> stack{e};
		while (!stack.empty()) {
			uint32_t pc = stack.back(); stack.pop_back();
			while (!vis.count(pc)) {
				const Insn *r = insn(pc);
				if (!r || !r->supported) break;
				vis.insert(pc);
				auto follow = [&](uint32_t t) {
					if (entries.count(t) && t != e) calls.insert(t);
					else if (insn(t)) stack.push_back(t);
				};
				if (r->ctl == C_JMP || r->ctl == C_COND) { if (r->link) calls.insert(r->target); else follow(r->target); }
				else if ((r->ctl == C_IND || r->ctl == C_RFI) && !r->link) {
					auto it = sitesCache.find(pc);
					if (it == sitesCache.end()) it = sitesCache.emplace(pc, tableTargets(pc)).first;
					if (sites) (*sites)[pc] = std::vector<uint32_t>(it->second.begin(), it->second.end());
					for (uint32_t t : it->second) follow(t);
				}
				if (terminal(*r)) break;
				pc += 4;
			}
		}
	};
	for (;;) {                                             // grow the entry set until no new call target appears
		size_t before = entries.size();
		std::vector<uint32_t> cur(entries.begin(), entries.end());
		for (uint32_t e : cur) {
			std::set<uint32_t> vis, calls; bodyOf(e, vis, calls, nullptr);
			for (uint32_t t : calls) if (insn(t)) entries.insert(t);
		}
		if (entries.size() == before) break;
	}
	funcs.clear();
	for (uint32_t e : entries) {
		Function f; f.entry = e;
		std::set<uint32_t> vis, calls; bodyOf(e, vis, calls, &f.sites);
		if (vis.empty()) continue;
		f.pcs.assign(vis.begin(), vis.end());
		funcs.emplace(e, std::move(f));
	}
}

}
