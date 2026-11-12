/* Lockstep verifier. For each trace record i: run the lifted instruction on state[i], compare with state[i+1],
   and check every memory access against the bus events Supermodel logged (<trace>.mem). */
#include "ppc_rt.h"
#include <stdio.h>
#include <stdlib.h>
#define R 412
uint32_t exec_at(CPU *c, uint32_t pc);
typedef struct { uint32_t pc, r[32], lr, ctr, xer, msr; uint8_t cr[8]; uint64_t f[32]; } __attribute__((packed)) Rec;
typedef struct { uint8_t k, sz; uint32_t a; uint64_t v; } __attribute__((packed)) Ev;
typedef struct { Ev e[256]; int n; } Evs;
static Rec *nx; static Evs cur; static int iev, memerr; static char memmsg[220];
static void merr(const char *m, int k, int sz, uint32_t a, uint64_t v, uint64_t ev) {
    if (!memerr) snprintf(memmsg, sizeof memmsg, "%s: kind %d size %d addr %08X val %llX | evento: val %llX (idx %d/%d)", m, k, sz, a, (unsigned long long)v, (unsigned long long)ev, iev, cur.n);
    memerr++;
}
static uint64_t ld(int sz, uint32_t a) {
    if (iev >= cur.n) { merr("load senza evento", 0, sz, a, 0, 0); return 0; }
    Ev *e = &cur.e[iev++];
    if (e->k != 0 || e->sz != sz || e->a != a) merr("load diverso", 0, sz, a, 0, e->v);
    return e->v;
}
static void st(int sz, uint32_t a, uint64_t v) {
    if (iev >= cur.n) { merr("store senza evento", 1, sz, a, v, 0); return; }
    Ev *e = &cur.e[iev++];
    if (e->k != 1 || e->sz != sz || e->a != a || e->v != v) merr("store diverso", 1, sz, a, v, e->v);
}
uint8_t LD8(CPU *c, uint32_t a) { return (uint8_t)ld(1, a); }
uint16_t LD16(CPU *c, uint32_t a) { return (uint16_t)ld(2, a); }
uint32_t LD32(CPU *c, uint32_t a) { return (uint32_t)ld(4, a); }
uint64_t LD64(CPU *c, uint32_t a) { return ld(8, a); }
void ST8(CPU *c, uint32_t a, uint8_t v) { st(1, a, v); }
void ST16(CPU *c, uint32_t a, uint16_t v) { st(2, a, v); }
void ST32(CPU *c, uint32_t a, uint32_t v) { st(4, a, v); }
void ST64(CPU *c, uint32_t a, uint64_t v) { st(8, a, v); }
uint32_t TB_READ(CPU *c, int spr, int rt) { return nx->r[rt]; }
static uint32_t g_unimpl;
void ppc_unimpl(CPU *c, uint32_t pc, uint32_t w) { g_unimpl++; }
void dispatch(CPU *c, uint32_t a) {}

static int read_ev(FILE *mf, Evs *o) {
    int n = fgetc(mf); if (n == EOF) { o->n = 0; return 0; }
    o->n = n;
    for (int i = 0; i < n; i++) if (fread(&o->e[i], 14, 1, mf) != 1) return 0;
    return 1;
}
int main(int argc, char **argv) {
    FILE *f = fopen(argv[1], "rb"); char mn[600]; snprintf(mn, sizeof mn, "%s.mem", argv[1]); FILE *mf = fopen(mn, "rb");
    unsigned long lim = argc > 3 ? strtoul(argv[3], 0, 10) : ~0ul, W = argc > 4 ? strtoul(argv[4], 0, 10) : 0;
    FILE *im = fopen(argv[2], "rb"); static unsigned char img[0x8E000]; if (fread(img, 1, sizeof img, im) < 1) return 1;
    Rec a, b; if (!f || !mf || fread(&a, R, 1, f) != 1) return 1;
    static Evs ea, eb; read_ev(mf, &ea);
    unsigned long n = 0, ok = 0, skipped = 0, pcskip = 0, bad = 0, unimpl = 0, membad = 0, memops = 0; int shown = 0;
    static unsigned long badcnt[64][1024], membadcnt[64][1024];
    while (n < lim && fread(&b, R, 1, f) == 1) {
        n++; read_ev(mf, &eb);
        if ((W && n % W == 0) || a.pc >= 0x8E000 || (a.pc & 3)) { if (a.pc >= 0x8E000) skipped++; goto next; }
        {
            CPU c; memset(&c, 0, sizeof c);
            memcpy(c.r, a.r, 128); c.lr = a.lr; c.ctr = a.ctr; c.xer = a.xer; c.msr = a.msr; memcpy(c.cr, a.cr, 8);
            for (int i = 0; i < 32; i++) c.f[i].u = a.f[i];
            nx = &b; cur = ea; iev = 0; memerr = 0; g_unimpl = 0;
            uint32_t w = (img[a.pc] << 24) | (img[a.pc + 1] << 16) | (img[a.pc + 2] << 8) | img[a.pc + 3];
            int op = w >> 26, xo = (op == 19 || op == 31 || op == 63 || op == 59) ? (w >> 1) & 0x3ff : 0;
            uint32_t npc = exec_at(&c, a.pc);
            if (op == 31 && xo == 339) c.r[(w >> 21) & 31] = b.r[(w >> 21) & 31];   /* SPR state is not in the trace */
            if (g_unimpl) { unimpl++; badcnt[op][xo]++; goto next; }
            memops += ea.n;
            if (!memerr && iev != cur.n) merr("eventi non consumati", 0, 0, 0, 0, 0);
            if (memerr) { membad++; membadcnt[op][xo]++; if (shown < 15) { shown++; printf("MEM @%08X word %08X (op %d xo %d): %s\n", a.pc, w, op, xo, memmsg); } }
            if (npc != b.pc) { pcskip++; if (b.pc != 0xFFF00500 && (b.pc & 0xFFFFF0FF) != 0xFFF00000 && a.pc != 0x5A0 && shown < 15) { shown++; printf("pc @%08X (op %d xo %d): pred %08X real %08X\n", a.pc, op, xo, npc, b.pc); } goto next; }
            int diff = memcmp(c.r, b.r, 128) || c.lr != b.lr || c.ctr != b.ctr || c.xer != b.xer || memcmp(c.cr, b.cr, 8);
            for (int i = 0; i < 32; i++) if (c.f[i].u != b.f[i]) diff = 1;
            if (diff) { bad++; badcnt[op][xo]++; if (shown < 15) { shown++; printf("MISMATCH @%08X word %08X (op %d xo %d)\n", a.pc, w, op, xo);
                for (int i = 0; i < 32; i++) if (c.r[i] != b.r[i]) printf("   r%d: lifted %08X ref %08X\n", i, c.r[i], b.r[i]);
                if (c.xer != b.xer) printf("   xer %08X vs %08X\n", c.xer, b.xer);
                for (int i = 0; i < 8; i++) if (c.cr[i] != b.cr[i]) printf("   cr%d %X vs %X\n", i, c.cr[i], b.cr[i]);
                for (int i = 0; i < 32; i++) if (c.f[i].u != b.f[i]) printf("   f%d %016llX vs %016llX\n", i, (unsigned long long)c.f[i].u, (unsigned long long)b.f[i]); } }
            else if (!memerr) ok++;
        }
    next:
        a = b; ea = eb;
    }
    printf("record %lu | ok %lu | fuori-immagine %lu | pc diverso %lu | non supportate %lu | MISMATCH regs %lu | accessi memoria verificati %lu, errori %lu\n", n, ok, skipped, pcskip, unimpl, bad, memops, membad);
    for (int o = 0; o < 64; o++) for (int x = 0; x < 1024; x++) { if (badcnt[o][x]) printf("  regs op %d xo %d: %lu\n", o, x, badcnt[o][x]); if (membadcnt[o][x]) printf("  mem op %d xo %d: %lu\n", o, x, membadcnt[o][x]); }
    return bad || membad;
}
