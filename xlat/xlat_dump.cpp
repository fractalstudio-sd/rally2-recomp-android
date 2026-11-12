// Dev tool: run the front end on images and print the function list (compared with the reference lifter).
#include "frontend.h"
#include <cstdio>
int main(int argc, char **argv)
{
	if (argc < 4) { fprintf(stderr, "usage: xlat_dump program_at_0.bin boot_at_FFF00000.bin crom.bin\n"); return 2; }
	xl::Program p; xl::Image a, b, d;
	if (!xl::loadImage(argv[1], 0, a) || !xl::loadImage(argv[2], 0xFFF00000u, b) || !xl::loadImage(argv[3], 0, d)) { fprintf(stderr, "cannot read images\n"); return 1; }
	p.code = {a, b}; p.data = {d};
	auto s = p.seeds(); fprintf(stderr, "seeds %zu\n", s.size());
	p.discover(s);
	size_t n = 0; for (auto &f : p.funcs) n += f.second.pcs.size();
	fprintf(stderr, "functions %zu instructions %zu\n", p.funcs.size(), n);
	for (auto &f : p.funcs) { printf("F %08x", f.first); for (uint32_t pc : f.second.pcs) printf(" %x", pc); printf("\n"); }
}
// (seed listing for debugging)
