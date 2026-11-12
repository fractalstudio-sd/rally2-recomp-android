/* Glue between the touch-menu code and translated guest code. GPLv3. */
#include "recomp.h"
#define XL_GUEST(a) void fn_##a(CPU *c) { xl_call(c, 0x##a##u); }
#define XL_HOOK(a) void fn_##a##_orig(CPU *c) { xl_call_orig(c, 0x##a##u); }
#include "hooks.def"
#undef XL_GUEST
#undef XL_HOOK
/* table of replaced routines for the translator */
typedef struct { uint32_t addr; void (*fn)(CPU *); } XlHook;
#define XL_GUEST(a)
#define XL_HOOK(a) { 0x##a##u, fn_##a },
const XlHook xl_hooks[] = {
#include "hooks.def"
    { 0, 0 } };
