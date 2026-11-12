// Runs statically-recompiled guest code (lifted PPC 603e) and exposes the CPU API the
// Model 3 hardware code expects (ppc.h). Guest runs on its own stack (fiber) so that
// ppc_execute(n) can suspend it at a "safepoint" once n cycles are used. GPLv3.
// Fibers: a tiny hand-written context switch (x86_64 SysV / AArch64 / ARM32 AAPCS) is used by default - Android's libc has no
// ucontext, and it is far cheaper than swapcontext (no signal-mask syscall). -DM3_FIBER_UCONTEXT forces ucontext.
#if !defined(M3_FIBER_UCONTEXT) && (defined(__x86_64__) || defined(__aarch64__) || defined(__arm__)) && !defined(_WIN32)
#define M3_FIBER_ASM 1
#endif
#if !M3_FIBER_ASM && defined(__APPLE__) && !defined(_XOPEN_SOURCE)
#define _XOPEN_SOURCE 600   // macOS gates <ucontext.h> behind this
#endif
#include <cstdio>
#include <cstdlib>
#include <cstring>
#if !M3_FIBER_ASM
#include <ucontext.h>
#endif
#include "CPU/PowerPC/ppc.h"
#include "CPU/Bus.h"
extern "C" {
#include "ppc_rt.h"
void dispatch(CPU *c, uint32_t addr);
}

#define MSR_EE 0x8000u
#define MSR_IP 0x40u
#define MSR_CLR (0x40000u|0x8000u|0x4000u|0x2000u|0x800u|0x400u|0x200u|0x100u|0x20u|0x10u|0x2u|0x10001u)


#if M3_FIBER_ASM
#ifdef __APPLE__
#define M3SYM "_m3_fib_switch"
#else
#define M3SYM "m3_fib_switch"
#endif
extern "C" void m3_fib_switch(void **save_sp, void *load_sp);
#if defined(__x86_64__)
__asm__(".text\n.globl " M3SYM "\n" M3SYM ":\n"
	"pushq %rbp\npushq %rbx\npushq %r12\npushq %r13\npushq %r14\npushq %r15\n"
	"movq %rsp,(%rdi)\nmovq %rsi,%rsp\n"
	"popq %r15\npopq %r14\npopq %r13\npopq %r12\npopq %rbx\npopq %rbp\nret\n");
#elif defined(__arm__)
// AAPCS: r4-r11, lr, d8-d15 are callee-saved. Frame (104 bytes): d8-d15 | pad | r4-r11 | lr
__asm__(".text\n.arm\n.p2align 2\n.globl " M3SYM "\n.type " M3SYM ",%function\n" M3SYM ":\n"
	"push {r4-r11,lr}\nsub sp,sp,#4\nvpush {d8-d15}\n"
	"str sp,[r0]\nmov sp,r1\n"
	"vpop {d8-d15}\nadd sp,sp,#4\npop {r4-r11,pc}\n");
#else
__asm__(".text\n.p2align 2\n.globl " M3SYM "\n" M3SYM ":\n"
	"sub sp,sp,#176\n"
	"stp x19,x20,[sp,#0]\nstp x21,x22,[sp,#16]\nstp x23,x24,[sp,#32]\nstp x25,x26,[sp,#48]\n"
	"stp x27,x28,[sp,#64]\nstp x29,x30,[sp,#80]\n"
	"stp d8,d9,[sp,#96]\nstp d10,d11,[sp,#112]\nstp d12,d13,[sp,#128]\nstp d14,d15,[sp,#144]\n"
	"mov x2,sp\nstr x2,[x0]\nmov sp,x1\n"
	"ldp x19,x20,[sp,#0]\nldp x21,x22,[sp,#16]\nldp x23,x24,[sp,#32]\nldp x25,x26,[sp,#48]\n"
	"ldp x27,x28,[sp,#64]\nldp x29,x30,[sp,#80]\n"
	"ldp d8,d9,[sp,#96]\nldp d10,d11,[sp,#112]\nldp d12,d13,[sp,#128]\nldp d14,d15,[sp,#144]\n"
	"add sp,sp,#176\nret\n");
#endif
struct Fib { void *sp = nullptr; };
static void fib_make(Fib &f, char *stack, size_t size, void (*entry)()) {
	uintptr_t top = ((uintptr_t)stack + size) & ~(uintptr_t)15;
#if defined(__x86_64__)
	void **p = (void **)(top - 64);
	memset(p, 0, 64);
	p[6] = (void *)entry;                       // return address popped by the final ret; p[7] stays 0 (fake caller)
	f.sp = p;
#elif defined(__arm__)
	uint32_t *p = (uint32_t *)(top - 104);
	memset(p, 0, 104);
	p[25] = (uint32_t)(uintptr_t)entry;         // lr slot, popped into pc
	f.sp = p;
#else
	void **p = (void **)(top - 176);
	memset(p, 0, 176);
	p[11] = (void *)entry;                      // x30
	f.sp = p;
#endif
}
static inline void fib_switch(Fib &from, Fib &to) { m3_fib_switch(&from.sp, to.sp); }
#else
struct Fib { ucontext_t uc; };
static void fib_make(Fib &f, char *stack, size_t size, void (*entry)()) {
	getcontext(&f.uc); f.uc.uc_stack.ss_sp = stack; f.uc.uc_stack.ss_size = size; f.uc.uc_link = nullptr;
	makecontext(&f.uc, entry, 0);
}
static inline void fib_switch(Fib &from, Fib &to) { swapcontext(&from.uc, &to.uc); }
#endif

static CPU g_cpu;
static IBus *g_bus;
// Guest execution contexts. Each one owns a C stack that mirrors the guest call chain of one
// "task". An exception suspends the interrupted context (keyed by its resume pc and r1) and runs the
// vector on a fresh context; `rfi` to a suspended (pc,r1) switches back to that context, which is how
// the game's RTOS context switches work without ever emulating instructions.
struct Ctx { Fib uc; char *stack; uint32_t key_pc, key_r1, entry; int state; uint64_t seq; };  // 0 free 1 running 2 suspended 3 dead
enum { MAXCTX = 64 };
static Ctx g_ctxs[MAXCTX]; static Ctx *g_cur;
static Fib g_host_ctx;
static const size_t GUEST_STACK = 4u << 20;
static bool g_started, g_halted;
static int g_irq_pending;
static uint64_t g_total;            // cycles consumed before the current slice
static int32_t g_cyc_start;         // cyc balance at slice start
static uint32_t g_pvr; static int g_mult2, g_ratio = 10, g_cps;
// time base / decrementer, in cycles
static uint64_t g_tb_set_cycle, g_tb_val; static uint64_t g_dec_set_cycle; static uint32_t g_dec_val = 0xFFFFFFFFu;
static uint64_t g_dec_trigger = ~0ull;
static uint64_t g_nswitch, g_nexc, g_seq; static int g_dbg = getenv("M3_DEBUG") ? atoi(getenv("M3_DEBUG")) : 0;

static inline uint64_t now_cycles() { return g_total + (uint64_t)(int64_t)(g_cyc_start - g_cpu.cyc); }
static uint64_t tb_now() { return g_tb_val + (now_cycles() - g_tb_set_cycle) / (uint64_t)g_ratio; }
static uint32_t dec_now() { return g_dec_val - (uint32_t)((now_cycles() - g_dec_set_cycle) / (uint64_t)g_ratio); }
static void dec_arm() { g_dec_trigger = g_dec_set_cycle + ((uint64_t)g_dec_val + 1) * (uint64_t)g_ratio; }

extern "C" {
uint8_t LD8(CPU *, uint32_t a) { return g_bus->Read8(a); }
uint16_t LD16(CPU *, uint32_t a) { return g_bus->Read16(a); }
uint32_t LD32(CPU *, uint32_t a) { return g_bus->Read32(a); }
uint64_t LD64(CPU *, uint32_t a) { return g_bus->Read64(a); }
void ST8(CPU *, uint32_t a, uint8_t v) { g_bus->Write8(a, v); }
void ST16(CPU *, uint32_t a, uint16_t v) { g_bus->Write16(a, v); }
void ST32(CPU *, uint32_t a, uint32_t v) { g_bus->Write32(a, v); }
void ST64(CPU *, uint32_t a, uint64_t v) { g_bus->Write64(a, v); }

uint32_t TB_READ(CPU *, int spr, int) {
	switch (spr) {
	case 22: return dec_now();
	case 268: case 284: return (uint32_t)tb_now();
	default: return (uint32_t)(tb_now() >> 32);
	}
}
void ppc_mtspr(CPU *, int spr, uint32_t v) {
	uint64_t n = now_cycles();
	switch (spr) {
	case 22: g_dec_val = v; g_dec_set_cycle = n; dec_arm(); break;
	case 268: case 284: g_tb_val = (tb_now() & 0xFFFFFFFF00000000ull) | v; g_tb_set_cycle = n; break;
	default: g_tb_val = ((uint64_t)v << 32) | (uint32_t)tb_now(); g_tb_set_cycle = n; break;
	}
}
} // extern "C"

static void ctx_main();
static Ctx *ctx_alloc(uint32_t entry) {
	Ctx *x = nullptr;
	for (int i = 0; i < MAXCTX; i++) {
		Ctx *y = &g_ctxs[i];
		if (y->state == 3 && y != g_cur) y->state = 0;          // reclaim dead contexts we are not standing on
		if (y->state == 0 && !x) x = y;
	}
	if (!x) { fprintf(stderr, "[ppc] FATAL: out of guest contexts\n"); g_halted = true; return nullptr; }
	if (!x->stack) x->stack = (char *)malloc(GUEST_STACK);
	fib_make(x->uc, x->stack, GUEST_STACK, ctx_main);
	x->entry = entry; x->state = 1;
	return x;
}
static void halt_forever() { g_halted = true; for (;;) fib_switch(g_cur->uc, g_host_ctx); }
extern "C" void ppc_unimpl(CPU *c, uint32_t pc, uint32_t w) {
	fprintf(stderr, "[ppc] FATAL: unimplemented/unknown code at %08x (insn %08x) pc_last_safepoint=%08x lr=%08x r1=%08x\n", pc, w, c->pc, c->lr, c->r[1]);
	halt_forever();
}
static void ctx_main() {
	dispatch(&g_cpu, g_cur->entry);
	fprintf(stderr, "[ppc] guest context entered at %08x returned (lr=%08x pc=%08x)\n", g_cur->entry, g_cpu.lr, g_cpu.pc);
	halt_forever();
}

// Take an exception: suspend the current context at ret_pc and run the vector on a new one.
static void deliver(uint32_t vec, uint32_t ret_pc) {
	CPU *c = &g_cpu; Ctx *me = g_cur;
	uint32_t msr = c->msr;
	c->spr[26] = ret_pc; c->spr[27] = msr & 0xff73;
	c->msr = msr & ~MSR_CLR;
	me->key_pc = ret_pc; me->key_r1 = c->r[1]; me->state = 2; me->seq = ++g_seq;
	Ctx *h = ctx_alloc(((msr & MSR_IP) ? 0xFFF00000u : 0u) | vec);
	if (!h) halt_forever();
	g_cur = h; g_nexc++;
	if (g_dbg) fprintf(stderr, "[exc] vec=%03x ret_pc=%08x r1=%08x: ctx%d suspended -> handler ctx%d\n", vec, ret_pc, c->r[1], (int)(me - g_ctxs), (int)(h - g_ctxs));
	fib_switch(me->uc, h->uc);
	// resumed by an rfi that matched us
}
extern "C" {
void ppc_sc(CPU *, uint32_t next_pc) { deliver(0xC00, next_pc); }
int ppc_rfi(CPU *c) {
	uint32_t t = c->spr[26] & ~3u, r1 = c->r[1];
	Ctx *y = nullptr; int npc = 0; Ctx *pcmatch = nullptr;
	for (int i = 0; i < MAXCTX; i++) {
		Ctx *z = &g_ctxs[i];
		if (z->state != 2 || z->key_pc != t) continue;
		npc++; if (!pcmatch || z->seq > pcmatch->seq) pcmatch = z;
		if (z->key_r1 == r1 && (!y || z->seq > y->seq)) y = z;      // identical keys nest: most recent first
	}
	if (!y && npc == 1) y = pcmatch;                    // r1 changed (e.g. handler adjusted it): unambiguous by pc
	if (y) {
		Ctx *r = g_cur;
		if (r != y) { if (r->state == 1) r->state = 3; }
		if (g_dbg) fprintf(stderr, "[rfi] to %08x r1=%08x: ctx%d -> ctx%d (npc=%d)\n", t, r1, (int)(r - g_ctxs), (int)(y - g_ctxs), npc);
		y->state = 1; g_cur = y; g_nswitch++;
		{ Fib dead; fib_switch(dead, y->uc); }              // does not return
	}
	if (g_dbg) {
		fprintf(stderr, "[ppc] rfi to %08x r1=%08x with no suspended context; table:\n", t, r1);
		for (int i = 0; i < MAXCTX; i++) if (g_ctxs[i].state) fprintf(stderr, "   ctx%d state=%d key_pc=%08x key_r1=%08x entry=%08x\n", i, g_ctxs[i].state, g_ctxs[i].key_pc, g_ctxs[i].key_r1, g_ctxs[i].entry);
	}
	dispatch(c, t);                                     // rfi used as a plain jump (boot code, new tasks)
	return 0;
}

void ppc_safepoint(CPU *c, uint32_t pc) {
	c->pc = pc;
	for (;;) {
		Ctx *me = g_cur;
		fib_switch(me->uc, g_host_ctx);              // slice finished: back to the hardware loop
		c->pc = pc;
		uint64_t n = now_cycles();
		if (n >= g_dec_trigger) { g_irq_pending |= 2; g_dec_trigger += (uint64_t)g_ratio << 32; }
		if (c->msr & MSR_EE) {
			if (g_irq_pending & 1) { g_irq_pending &= ~1; deliver(0x500, pc); }
			else if (g_irq_pending & 2) { g_irq_pending &= ~2; deliver(0x900, pc); }
		}
		if (c->cyc >= 0) return;
	}
}
}

UINT32 ppc_get_pc(void) { return g_cpu.pc; }
extern "C" CPU *ppc_cpu_state(void) { return &g_cpu; }   // dev tools (lockstep dumps)
UINT32 ppc_get_lr(void) { return g_cpu.lr; }
void ppc_set_irq_line(int line) { if (line) g_irq_pending |= 1; else g_irq_pending &= ~1; }
void ppc_attach_bus(IBus *b) { g_bus = b; }
void ppc_set_fetch(PPC_FETCH_REGION *) {}
void ppc_shutdown(void) {}
UINT64 ppc_total_cycles(void) { return now_cycles(); }
int ppc_get_cycles_per_sec(void) { return g_cps; }
int ppc_get_bus_freq_multipler(void) { return g_mult2; }
int ppc_get_timer_ratio(void) { return g_ratio; }
void ppc_set_timer_ratio(int r) { uint64_t n = now_cycles(); g_tb_val = tb_now(); g_tb_set_cycle = n; g_dec_val = dec_now(); g_dec_set_cycle = n; g_ratio = r > 0 ? r : 1; dec_arm(); }
void ppc_save_state(CBlockFile *) {}
void ppc_load_state(CBlockFile *) {}

void ppc_init(const PPC_CONFIG *cfg) {
	float m = (float)((cfg->bus_frequency_multiplier >> 4) & 0xf) + (float)(cfg->bus_frequency_multiplier & 0xf) / 10.0f;
	g_mult2 = (int)(m * 2); g_ratio = 2 * g_mult2; g_pvr = (uint32_t)cfg->pvr;
	static const int mhz[] = {16, 20, 25, 33, 40, 50, 60, 66, 75};
	g_cps = (int)(m * mhz[cfg->bus_frequency] * 1000000);
}

void ppc_reset(void) {
	memset(&g_cpu, 0, sizeof g_cpu);
	g_cpu.msr = 0x40; g_cpu.spr[287] = g_pvr; g_cpu.spr[1008] = 1;
	g_cpu.pc = 0xFFF00100u;
	g_total = 0; g_cyc_start = 0; g_irq_pending = 0; g_halted = false; g_started = false; g_cur = nullptr;
	for (int i = 0; i < MAXCTX; i++) g_ctxs[i].state = 0;
	g_tb_val = 0; g_tb_set_cycle = 0; g_dec_val = 0xFFFFFFFFu; g_dec_set_cycle = 0; dec_arm();
}

int ppc_execute(int cycles) {
	if (g_halted) return cycles;
	if (!g_started) { g_cur = nullptr; g_cur = ctx_alloc(0xFFF00100u); g_started = true; if (!g_cur) return cycles; }
	g_cpu.cyc += cycles;
	g_cyc_start = g_cpu.cyc;
	fib_switch(g_host_ctx, g_cur->uc);
	g_total += (uint64_t)(int64_t)(g_cyc_start - g_cpu.cyc);
	g_cyc_start = g_cpu.cyc;
	return cycles;
}
