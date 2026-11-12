// PowerPC 603e -> AArch64 code generator. GPLv3.
#pragma once
#include "frontend.h"
#include "a64asm.h"
#include "helpers.h"
#include <unordered_map>

namespace xl {


struct CallFix { size_t at; uint32_t target; };        // bl to the function translated from guest address `target`

struct A64Code {
	a64::Asm a;
	std::unordered_map<uint32_t, size_t> entry;          // guest function entry -> word index
	std::vector<CallFix> calls;                           // to patch once every function is placed
};

// Translate every function of the program into one code block (word indices relative to its start).
void translateAll(Program &p, A64Code &out, const std::set<uint32_t> &overridden);

}
