#!/usr/bin/env python3
"""Turn a coverage run (SM_COV edges) into Ghidra seed lists.
usage: python3 -I make_seeds.py program_image.bin run.edges.txt outdir
Writes seeds_calls.txt (targets of bl/bcl/bctrl) and seeds_bctr_targets.txt (targets of bctr)."""
import sys, struct, os
img, edges_f, out = sys.argv[1:4]
rom = open(img, 'rb').read()
calls, bctr = set(), set()
for l in open(edges_f):
    f, t = (int(x, 16) for x in l.split())
    if f + 4 > len(rom): continue
    w = struct.unpack('>I', rom[f:f + 4])[0]; op = w >> 26; lk = w & 1
    if op in (16, 18) and lk: calls.add(t)
    elif op == 19:
        xo = (w >> 1) & 0x3ff
        if xo == 528:
            bctr.add(t)
            if lk: calls.add(t)
os.makedirs(out, exist_ok=True)
open(os.path.join(out, 'seeds_calls.txt'), 'w').write('\n'.join('%08X' % a for a in sorted(calls)))
open(os.path.join(out, 'seeds_bctr_targets.txt'), 'w').write('\n'.join('%08X' % a for a in sorted(bctr)))
print('seeds: %d call targets, %d bctr targets' % (len(calls), len(bctr)))
