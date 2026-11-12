// Native translator runtime (AArch64 and 32-bit ARM): translates the user's program image at start-up into executable
// memory, provides dispatch() to the CPU glue, translates functions met only at run time (computed calls) on first use.
// GPLv3.
#include <sys/mman.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <string>
#include <fstream>
#include <sstream>
#include <mutex>
#include <chrono>
#include <cstddef>
#include <algorithm>
#if defined(__aarch64__)
#include "backend_a64.h"
typedef xl::A64Code Code;
static void translate(xl::Program &p, Code &c, const std::set<uint32_t> &o) { xl::translateAll(p, c, o); }
#elif defined(__arm__)
#include "backend_a32.h"
#include <sys/auxv.h>
typedef xl::A32Code Code;
static void translate(xl::Program &p, Code &c, const std::set<uint32_t> &o) { xl::translateAll32(p, c, o); }
#else
#error "the native translator targets AArch64 and 32-bit ARM hosts"
#endif
extern "C" {
#include "ppc_rt.h"
}

using namespace xl;

extern "C" {
void ppc_safepoint(CPU *c, uint32_t pc);
void ppc_sc(CPU *c, uint32_t next_pc);
int ppc_rfi(CPU *c);
void ppc_unimpl(CPU *c, uint32_t pc, uint32_t w);
void dispatch(CPU *c, uint32_t addr);
// trampoline: sets the pinned registers (CPU, RAM, helper table, cycle balance) and calls translated code
void xl_enter(CPU *c, const void *code, void *ram, void **helpers);
}
static_assert(offsetof(CPU, cyc) == 4576, "xl_enter hard-codes the offset of CPU.cyc");

#if defined(__aarch64__)
// x19 CPU, x20 RAM, x21 helper table, w22 cycle balance (written back to CPU.cyc on return), x23-x28 guest registers
// (translated functions do not preserve x23-x28 among themselves, so the C caller's values are saved here)
__asm__(".text\n.p2align 2\n.globl xl_enter\n.type xl_enter,%function\nxl_enter:\n"
	"stp x29, x30, [sp, #-96]!\n"
	"mov x29, sp\n"
	"stp x19, x20, [sp, #16]\n"
	"stp x21, x22, [sp, #32]\n"
	"stp x23, x24, [sp, #48]\n"
	"stp x25, x26, [sp, #64]\n"
	"stp x27, x28, [sp, #80]\n"
	"mov x19, x0\nmov x20, x2\nmov x21, x3\n"
	"ldr w22, [x19, #4576]\n"
	"blr x1\n"
	"str w22, [x19, #4576]\n"
	"ldp x19, x20, [sp, #16]\n"
	"ldp x21, x22, [sp, #32]\n"
	"ldp x23, x24, [sp, #48]\n"
	"ldp x25, x26, [sp, #64]\n"
	"ldp x27, x28, [sp, #80]\n"
	"ldp x29, x30, [sp], #96\n"
	"ret\n");
#else
// r4 CPU + 1024, r5 RAM, r6 helper table, r7 cycle balance, r8-r11 guest registers. ARM state; callers may be Thumb.
__asm__(".text\n.p2align 2\n.arm\n.globl xl_enter\n.type xl_enter,%function\nxl_enter:\n"
	"push {r4-r11, lr}\n"
	"sub sp, sp, #4\n"
	"add r4, r0, #1024\n"
	"mov r5, r2\nmov r6, r3\n"
	"ldr r7, [r4, #3552]\n"
	"blx r1\n"
	"str r7, [r4, #3552]\n"
	"add sp, sp, #4\n"
	"pop {r4-r11, pc}\n"
	".size xl_enter, .-xl_enter\n");
// software paths for devices without VFPv4 (fused multiply-add) or the integer divide instructions
static void h_fma(CPU *c, uint32_t k)
{
	unsigned rt = k & 31, fa = k >> 5 & 31, fb = k >> 10 & 31, fc = k >> 15 & 31, kind = k >> 20 & 3; bool single = k >> 22 & 1;
	double a = c->f[fa].d, b = c->f[fb].d, m = c->f[fc].d, r;
	r = fma(a, m, (kind & 1) ? b : -b);
	if (single) r = (double)(float)r;
	FP o; o.d = r;
	if (kind >= 2) o.u ^= 0x8000000000000000ull;   // negate the result as a sign flip: the compiler may not fold it into fnma
	c->f[rt] = o;
}
static uint32_t h_divw(uint32_t a, uint32_t b)
{
	if (b == 0) return (int32_t)a < 0 ? 0xFFFFFFFFu : 0;
	if (b == 0xFFFFFFFFu && a == 0x80000000u) return 0xFFFFFFFFu;
	return (uint32_t)((int32_t)a / (int32_t)b);
}
static uint32_t h_divwu(uint32_t a, uint32_t b) { return b ? a / b : 0; }
#endif

static uint32_t h_mfcr(CPU *c) { uint32_t v = 0; for (int i = 0; i < 8; i++) v = (v << 4) | (c->cr[i] & 15); return v; }
static void h_mtcrf(CPU *c, uint32_t crm, uint32_t v) { for (int i = 0; i < 8; i++) if (crm & (0x80 >> i)) c->cr[i] = (v >> (28 - 4 * i)) & 15; }
static void h_mtfsf(CPU *c, uint32_t fm, uint32_t v) { uint32_t m = 0; for (int i = 0; i < 8; i++) if (fm & (0x80 >> i)) m |= 0xFu << (28 - 4 * i); c->fpscr = (c->fpscr & ~m) | (v & m); }
static void h_call(CPU *c, uint32_t addr) { dispatch(c, addr); }

static Program g_prog;
static uint8_t *g_code = nullptr; static size_t g_codeCap = 0, g_codeUsed = 0;
static std::unordered_map<uint32_t, const void *> g_fn;
static uint8_t *g_ram = nullptr;
static void *g_helpers[H_COUNT];
static std::mutex g_jitLock;
// routines replaced by host code (touch menus): calls reach the hook, the hook can run the translated original
typedef struct { uint32_t addr; void (*fn)(CPU *); } XlHook;
extern "C" const XlHook xl_hooks[] __attribute__((weak));
static std::unordered_map<uint32_t, void (*)(CPU *)> g_hook;
static std::unordered_map<uint32_t, const void *> g_orig;

static bool place(const Code &c, std::unordered_map<uint32_t, const void *> &fns)
{
	size_t bytes = c.a.w.size() * 4;
	if (g_codeUsed + bytes > g_codeCap) return false;
	uint8_t *dst = g_code + g_codeUsed;
	if (mprotect(g_code, g_codeCap, PROT_READ | PROT_WRITE) != 0) return false;
	memcpy(dst, c.a.w.data(), bytes);
	mprotect(g_code, g_codeCap, PROT_READ | PROT_EXEC);
	__builtin___clear_cache((char *)dst, (char *)dst + bytes);
	for (auto &e : c.entry) fns[e.first] = dst + 4 * e.second;
	g_codeUsed += bytes;
	return true;
}

static bool loadLayout(const char *path, Program &p)
{
	std::ifstream in(path); if (!in) return false;
	std::string line; p.funcs.clear();
	Function *cur = nullptr;
	while (std::getline(in, line)) {
		std::istringstream ss(line); std::string tag; ss >> tag;
		if (tag == "F") { uint32_t e; ss >> std::hex >> e; cur = &p.funcs[e]; cur->entry = e; uint32_t pc; while (ss >> std::hex >> pc) cur->pcs.push_back(pc); }
		else if (tag == "S" && cur) { uint32_t pc, t; ss >> std::hex >> pc; auto &v = cur->sites[pc]; while (ss >> std::hex >> t) v.push_back(t); }
	}
	for (auto &f : p.funcs) std::sort(f.second.pcs.begin(), f.second.pcs.end());
	return !p.funcs.empty();
}

extern "C" int xl_init(const char *imagesDir, uint8_t *ram, const char *layout)
{
	auto t0 = std::chrono::steady_clock::now();
	g_ram = ram;
#if defined(__arm__)
	{
		unsigned long hw = getauxval(AT_HWCAP);
		a32HasVfp4 = (hw & (1ul << 16)) != 0;   // HWCAP_VFPv4
		a32HasIdiv = (hw & (1ul << 17)) != 0;   // HWCAP_IDIVA
		if (getenv("XL_NO_VFP4")) a32HasVfp4 = false;
		if (getenv("XL_NO_IDIV")) a32HasIdiv = false;
		fprintf(stderr, "[xl] ARM: VFPv4 %s, integer divide %s\n", a32HasVfp4 ? "yes" : "no", a32HasIdiv ? "yes" : "no");
	}
#endif
	std::string dir = imagesDir;
	Image crom;
	if (!loadImage(dir + "/crom.bin", 0, crom)) { fprintf(stderr, "[xl] cannot read %s/crom.bin\n", imagesDir); return 0; }
	Image prog; prog.base = 0; prog.w.assign(crom.w.begin(), crom.w.begin() + std::min<size_t>(crom.w.size(), 0x8E000 / 4));
	Image boot; boot.base = 0xFFF00000u; size_t n1 = 0x100000 / 4; boot.w.assign(crom.w.end() - std::min(n1, crom.w.size()), crom.w.end());
	g_prog.code = {prog, boot}; g_prog.data = {crom};
	if (layout && *layout) { if (!loadLayout(layout, g_prog)) { fprintf(stderr, "[xl] bad layout %s\n", layout); return 0; } }
	else g_prog.discover(g_prog.seeds());
	auto t1 = std::chrono::steady_clock::now();
	std::set<uint32_t> hooked;
	if (xl_hooks) for (const XlHook *h = xl_hooks; h->fn; h++) if (g_prog.funcs.count(h->addr)) { hooked.insert(h->addr); g_hook[h->addr] = h->fn; }
	Code code;
	translate(g_prog, code, hooked);
	g_codeCap = 64u << 20;
	g_code = (uint8_t *)mmap(nullptr, g_codeCap, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (g_code == MAP_FAILED) { g_code = nullptr; fprintf(stderr, "[xl] cannot map code memory\n"); return 0; }
	if (!place(code, g_fn)) { fprintf(stderr, "[xl] code memory full\n"); return 0; }
	for (uint32_t a : hooked) { g_orig[a] = g_fn[a]; g_fn.erase(a); }
	void *h[H_COUNT] = {
		(void *)LD8, (void *)LD16, (void *)LD32, (void *)LD64, (void *)ST8, (void *)ST16, (void *)ST32, (void *)ST64,
		(void *)ppc_safepoint, (void *)ppc_sc, (void *)ppc_rfi, (void *)TB_READ, (void *)ppc_mtspr, (void *)h_call,
		(void *)h_mfcr, (void *)h_mtcrf, (void *)ppc_unimpl, (void *)h_mtfsf,
#if defined(__arm__)
		(void *)h_fma, (void *)h_divw, (void *)h_divwu,
#endif
	};
	memcpy(g_helpers, h, sizeof h);
	auto t2 = std::chrono::steady_clock::now();
	fprintf(stderr, "[xl] %zu functions (%d hooked), %zu KB of native code; analysis %d ms, translation %d ms\n", g_fn.size() + g_orig.size(),
		(int)hooked.size(), g_codeUsed / 1024, (int)std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count(),
		(int)std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t1).count());
	return 1;
}

// A call to a function no static analysis found (computed address): translate it now and keep it.
static const void *jit(uint32_t addr)
{
	std::lock_guard<std::mutex> lk(g_jitLock);
	auto it = g_fn.find(addr); if (it != g_fn.end()) return it->second;
	Program &p = g_prog;
	if (!p.insn(addr)) return nullptr;
	Program one = p; one.funcs.clear();
	one.discover(std::set<uint32_t>{addr});
	auto f = one.funcs.find(addr); if (f == one.funcs.end()) return nullptr;
	Code code;
	{   // generated alone: its calls go through the dispatcher
		Program tmp = p; tmp.funcs.clear(); tmp.funcs[addr] = f->second;
		std::set<uint32_t> none; translate(tmp, code, none);
	}
	std::unordered_map<uint32_t, const void *> fns;
	if (!place(code, fns)) return nullptr;
	g_fn[addr] = fns[addr];
	g_prog.funcs[addr] = f->second;
	fprintf(stderr, "[xl] translated at run time: %08x\n", addr);
	return g_fn[addr];
}

extern "C" void dispatch(CPU *c, uint32_t addr)
{
	if (!g_hook.empty()) { auto h = g_hook.find(addr); if (h != g_hook.end()) { h->second(c); return; } }
	auto it = g_fn.find(addr);
	const void *code = it != g_fn.end() ? it->second : jit(addr);
	if (!code) { ppc_unimpl(c, addr, 0); return; }
	xl_enter(c, code, g_ram, g_helpers);
}

extern "C" void xl_call(CPU *c, uint32_t addr) { dispatch(c, addr); }
extern "C" void xl_call_orig(CPU *c, uint32_t addr)
{
	auto it = g_orig.find(addr);
	if (it == g_orig.end()) { ppc_unimpl(c, addr, 0); return; }
	xl_enter(c, it->second, g_ram, g_helpers);
}
