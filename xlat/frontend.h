// PowerPC 603e front end for the on-device translator: decodes the program image of the user's own ROM set and rebuilds
// its functions (no ROM-derived data ships with the app; everything here is computed from the dump at install time).
// GPLv3.
#pragma once
#include <cstdint>
#include <vector>
#include <map>
#include <set>
#include <string>

namespace xl {

enum Ctl : uint8_t { C_NONE, C_JMP, C_COND, C_RET, C_IND, C_RFI };

struct Insn {
	uint32_t pc = 0, w = 0, target = 0;
	uint8_t op = 0; uint16_t xo = 0;
	Ctl ctl = C_NONE;
	bool link = false;          // bl / bcl / bclrl / bcctrl
	bool uncond = false;        // branch condition always true (for C_RET: plain blr)
	bool supported = false;
};

struct Image { uint32_t base; std::vector<uint32_t> w; };   // big-endian words already converted to host values

struct Function {
	uint32_t entry;
	std::vector<uint32_t> pcs;                       // sorted instruction addresses of the body
	std::map<uint32_t, std::vector<uint32_t>> sites; // indirect branch pc -> resolved targets (jump tables)
};

class Program {
public:
	std::vector<Image> code;    // code segments (program at 0, boot block at 0xFFF00000)
	std::vector<Image> data;    // extra images read only for jump tables / code pointers (the whole CROM, copied to RAM at boot)
	bool word(uint32_t a, uint32_t &v) const;
	bool dataWord(uint32_t a, uint32_t &v) const;
	const Insn *insn(uint32_t pc);
	std::set<uint32_t> seeds() const;               // exception vectors + code pointers + function prologues
	void discover(const std::set<uint32_t> &seeds);
	std::map<uint32_t, Function> funcs;
private:
	std::map<uint32_t, Insn> cache_;
	std::set<uint32_t> tableTargets(uint32_t pc);
};

Insn decode(uint32_t pc, uint32_t w);
bool loadImage(const std::string &path, uint32_t base, Image &img, size_t maxBytes = SIZE_MAX);

}
