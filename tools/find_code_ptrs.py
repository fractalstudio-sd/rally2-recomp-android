#!/usr/bin/env python3
"""Static discovery of function entry points that are only reachable through pointers.
Code addresses are collected (a) from aligned 32-bit words anywhere in the data image (function pointer
tables, vtables, task tables: the game copies them from CROM to RAM at boot) and (b) from lis+addi/ori
constants in the code. A candidate is kept when it sits right after an unconditional control transfer
(blr/b/bctr/rfi) or padding, or belongs to a run of >= 3 consecutive code pointers.
usage: find_code_ptrs.py program.bin out_seeds.txt [data_image.bin]   (program is based at 0)"""
import struct, sys
prog_f, out = sys.argv[1], sys.argv[2]
data_f = sys.argv[3] if len(sys.argv) > 3 else prog_f
pd = open(prog_f, 'rb').read(); n = len(pd) // 4
pw = struct.unpack('>%dI' % n, pd[:n * 4]); hi = n * 4
dd = open(data_f, 'rb').read(); dw = struct.unpack('>%dI' % (len(dd) // 4), dd[:(len(dd) // 4) * 4])
def ends_flow(w):
    return w in (0x4E800020, 0x4E800420, 0x4C000064, 0, 0x60000000) or ((w >> 26) == 18 and not (w & 1))
def is_ptr(w): return w & 3 == 0 and 0x100 <= w < hi and pw[w // 4] != 0
keep = set()
for i, w in enumerate(dw):                                                    # (a) data words
    if i * 4 < hi and data_f == prog_f: continue                              # code area: handled by (b)
    if i * 4 < hi: continue
    if not is_ptr(w): continue
    run = 1; j = i + 1
    while j < len(dw) and is_ptr(dw[j]) and run < 3: run += 1; j += 1
    if ends_flow(pw[w // 4 - 1]) or run >= 3: keep.add(w)
for i in range(n - 4):                                                        # (b) lis + addi/ori in code
    w = pw[i]
    if (w >> 26) == 15 and ((w >> 16) & 31) == 0:
        rd = (w >> 21) & 31; h16 = (w & 0xFFFF) << 16
        for j in range(1, 5):
            v = pw[i + j]
            if (v >> 26) in (14, 24) and ((v >> 16) & 31) == rd:
                imm = v & 0xFFFF
                val = (h16 + (imm - 0x10000 if imm & 0x8000 else imm)) if (v >> 26) == 14 else (h16 | imm)
                val &= 0xFFFFFFFF
                if is_ptr(val) and ends_flow(pw[val // 4 - 1]): keep.add(val)
                break
for i, w in enumerate(pw):                                                    # (c) pointer words inside the program image
    if is_ptr(w) and ends_flow(pw[w // 4 - 1]):
        run = 1; j = i + 1
        while j < n and is_ptr(pw[j]) and run < 3: run += 1; j += 1
        if run >= 3: keep.add(w)
open(out, 'w').write(''.join('%x\n' % a for a in sorted(keep)))
print('code-pointer seeds:', len(keep))
