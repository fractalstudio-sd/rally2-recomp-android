#!/usr/bin/env python3
"""Lets the runtime replace a few lifted functions (touch menus, runtime/app/menu_guest.c; leaderboard, runtime/app/leaderboard_guest.c).
For each address below the lifted definition becomes fn_XXXXXXXX_orig; call sites keep calling fn_XXXXXXXX,
which the runtime defines (and which normally calls the _orig body). Idempotent; run on the lifter output dir."""
import sys, glob, os, re
OVERRIDES = [0x00046ce4, 0x0002d26c, 0x0000532c, 0x00003f30, 0x00004028, 0x00047754, 0x0003576c, 0x00003c10, 0x000479b4, 0x0004758c, 0x00047efc, 0x0002bd60]
out = sys.argv[1]
names = ['fn_%08x' % a for a in OVERRIDES]
found = set()
for f in sorted(glob.glob(os.path.join(out, 'recomp_funcs_*.c'))):
    s = open(f).read(); n = s
    for nm in names:
        if ('void %s_orig(CPU *c) {' % nm) in n: found.add(nm); continue
        d = 'void %s(CPU *c) {' % nm
        if d in n: n = n.replace(d, 'void %s_orig(CPU *c) {' % nm); found.add(nm)
    if n != s: open(f, 'w').write(n)
missing = [nm for nm in names if nm not in found]
if missing: sys.exit('apply_overrides: not found in lifted code: ' + ' '.join(missing))
h = os.path.join(out, 'recomp.h'); s = open(h).read()
add = ''.join('void %s_orig(CPU *c);\n' % nm for nm in names if ('void %s_orig(CPU *c);' % nm) not in s)
if add: open(h, 'a').write(add)
print('apply_overrides: %d functions routed to the runtime' % len(names))
