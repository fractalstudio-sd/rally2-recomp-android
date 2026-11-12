// PowerPC 603e -> ARM (A32) code generator for 32-bit Android devices. GPLv3.
#pragma once
#include "frontend.h"
#include "a32asm.h"
#include "helpers.h"
#include <unordered_map>

namespace xl {

struct CallFix32 { size_t at; uint32_t target; };

struct A32Code {
	a32::Asm a;
	std::unordered_map<uint32_t, size_t> entry;          // guest function entry -> word index
	std::vector<CallFix32> calls;
};

// host features (set by the runtime from the kernel's hwcaps before translating)
extern bool a32HasVfp4, a32HasIdiv;

// the CPU pointer register holds CPU + A32_BIAS, so that every field (cycle counter at 4576) is in reach of an
// immediate offset and the FP registers in reach of VLDR/VSTR (+-1020)
enum { A32_BIAS = 1024 };

void translateAll32(Program &p, A32Code &out, const std::set<uint32_t> &overridden);

}
