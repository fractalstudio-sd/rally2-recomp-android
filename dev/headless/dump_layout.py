#!/usr/bin/env python3
"""Maintainers: function layout (entry, body pcs, resolved jump tables) of a lifted C directory, so the native translator
can be run on exactly the same functions as the reference build (lockstep tests). usage: dump_layout.py <out_dir> <layout.txt>"""
import sys, glob, re
out = open(sys.argv[2], 'w'); n = 0
for f in sorted(glob.glob(sys.argv[1] + '/recomp_funcs_*.c')):
    cur = None; pcs = []; sites = {}; pc = None; insw = False
    def flush():
        global n
        if cur is None: return
        out.write('F %x %s\n' % (cur, ' '.join('%x' % p for p in pcs)))
        for s, ts in sites.items(): out.write('S %x %s\n' % (s, ' '.join('%x' % t for t in ts)))
        n += 1
    for line in open(f):
        m = re.match(r'void fn_([0-9a-f]{8})(?:_orig)?\(CPU \*c\) \{', line)
        if m: flush(); cur = int(m.group(1), 16); pcs = []; sites = {}; continue
        m = re.match(r'\s*\{ /\* ([0-9a-f]{8}) \*/', line)
        if m: pc = int(m.group(1), 16); pcs.append(pc); continue
        if 'switch (tgt)' in line: sites.setdefault(pc, []); insw = True; continue
        if insw:
            m = re.match(r'\s*case 0x([0-9a-f]{8})u:', line)
            if m: sites[pc].append(int(m.group(1), 16)); continue
            if 'default:' in line: insw = False
    flush()
print('functions', n)
