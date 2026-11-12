/* Runtime for lifted PPC 603e code (big-endian guest, host-endian state). */
#pragma once
#include <stdint.h>
#include <string.h>
#include <math.h>

typedef union { double d; uint64_t u; } FP;
typedef struct {
    uint32_t r[32], lr, ctr, xer, msr, fpscr;
    uint8_t  cr[8];             /* one nibble per field: LT=8 GT=4 EQ=2 SO=1 */
    FP       f[32];
    uint32_t spr[1024];
    uint32_t sr[16];
    int32_t  cyc;               /* cycle balance: <0 => slice used up, yield at next safepoint */
    uint32_t pc;                /* last safepoint pc (debug / exception SRR0) */
} CPU;

#define XER_SO 0x80000000u
#define XER_OV 0x40000000u
#define XER_CA 0x20000000u

static inline uint8_t cmp_s(int32_t a, int32_t b, uint32_t xer) { return (a < b ? 8 : a > b ? 4 : 2) | ((xer >> 31) & 1); }
static inline uint8_t cmp_u(uint32_t a, uint32_t b, uint32_t xer) { return (a < b ? 8 : a > b ? 4 : 2) | ((xer >> 31) & 1); }
static inline uint32_t rotl32(uint32_t v, unsigned n) { n &= 31; return n ? (v << n) | (v >> (32 - n)) : v; }
static inline uint32_t cntlzw(uint32_t v) { return v ? (uint32_t)__builtin_clz(v) : 32; }
static inline uint32_t bswap32(uint32_t v) { return __builtin_bswap32(v); }
static inline uint16_t bswap16(uint16_t v) { return __builtin_bswap16(v); }
static inline void set_ca(CPU *c, int ca) { c->xer = (c->xer & ~XER_CA) | (ca ? XER_CA : 0); }
static inline void set_ov(CPU *c, int ov) { c->xer &= ~XER_OV; if (ov) c->xer |= XER_OV | XER_SO; }
static inline int crbit(const CPU *c, int bi) { return (c->cr[bi >> 2] >> (3 - (bi & 3))) & 1; }
static inline void setcrbit(CPU *c, int bi, int v) { int s = 3 - (bi & 3); c->cr[bi >> 2] = (c->cr[bi >> 2] & ~(1 << s)) | ((v & 1) << s); }
static inline uint32_t mfcr(const CPU *c) { uint32_t v = 0; for (int i = 0; i < 8; i++) v = (v << 4) | (c->cr[i] & 15); return v; }
static inline void mtcrf(CPU *c, uint32_t crm, uint32_t v) { for (int i = 0; i < 8; i++) if (crm & (0x80 >> i)) c->cr[i] = (v >> (28 - 4 * i)) & 15; }
static inline uint8_t fcmp(double a, double b) { return (isnan(a) || isnan(b)) ? 1 : a < b ? 8 : a > b ? 4 : 2; }
static inline uint64_t fctiwz(double v) {
    double t = trunc(v); int64_t r = isnan(t) ? INT64_MIN : (t > 4294967296.0 ? INT64_MAX : t < -4294967296.0 ? INT64_MIN : (int64_t)t);
    if (r > 0x7FFFFFFFLL) return (uint64_t)(int64_t)0x7FFFFFFF;
    if (r < -0x80000000LL) return (uint64_t)(int64_t)(int32_t)0x80000000;
    return (uint32_t)r;
}
static inline uint64_t fctiw(double v) { double t = nearbyint(v); return fctiwz(t); }
static inline float f32_of_bits(uint32_t b) { float f; memcpy(&f, &b, 4); return f; }
static inline uint32_t bits_of_f32(float f) { uint32_t b; memcpy(&b, &f, 4); return b; }

/* fmadd family: real 603e is fused (single rounding). Supermodel computes (a*c)+b in two steps;
   build with -DPPC_NONFUSED_FMA to match Supermodel bit-for-bit when verifying against it. */
#ifdef PPC_NONFUSED_FMA
#define PPC_FMA(a, c, b) ((a) * (c) + (b))
#else
#define PPC_FMA(a, c, b) fma((a), (c), (b))
#endif

/* Memory + environment: provided by the embedding program (verify harness or real runtime). */
uint8_t  LD8(CPU *c, uint32_t a);  uint16_t LD16(CPU *c, uint32_t a);
uint32_t LD32(CPU *c, uint32_t a); uint64_t LD64(CPU *c, uint32_t a);
void ST8(CPU *c, uint32_t a, uint8_t v);  void ST16(CPU *c, uint32_t a, uint16_t v);
void ST32(CPU *c, uint32_t a, uint32_t v); void ST64(CPU *c, uint32_t a, uint64_t v);
uint32_t TB_READ(CPU *c, int spr, int rt);
void ppc_mtspr(CPU *c, int spr, uint32_t v);        /* DEC / time base writes */
void ppc_safepoint(CPU *c, uint32_t pc);            /* yield + interrupt delivery */
void ppc_sc(CPU *c, uint32_t next_pc);              /* system call exception */
int  ppc_rfi(CPU *c);                               /* rfi: returns 1 if it returned from a nested exception */          /* time base / decrementer reads */
void ppc_unimpl(CPU *c, uint32_t pc, uint32_t w);
void dispatch(CPU *c, uint32_t addr);               /* call lifted function at addr */
