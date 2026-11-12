#!/usr/bin/env python3
"""Compare two SM_TRACE files record by record (412 B: pc,r0-31,lr,ctr,xer,msr,cr[8],fpr0-31, host-endian).
usage: python3 -I trace_diff.py ref.bin test.bin"""
import sys, struct
R = 412
a, b = open(sys.argv[1], 'rb'), open(sys.argv[2], 'rb')
names = ['pc'] + [f'r{i}' for i in range(32)] + ['lr', 'ctr', 'xer', 'msr']
i = 0
while True:
    x, y = a.read(R), b.read(R)
    if len(x) < R or len(y) < R:
        print(f'ok: {i} record identici'); break
    if x != y:
        wx = struct.unpack('<36I', x[:144]); wy = struct.unpack('<36I', y[:144])
        print(f'divergenza all\'istruzione {i}, pc ref={wx[0]:08X} test={wy[0]:08X}')
        for n, p, q in zip(names, wx, wy):
            if p != q: print(f'  {n}: ref={p:08X} test={q:08X}')
        if x[144:152] != y[144:152]: print('  cr differisce:', x[144:152].hex(), y[144:152].hex())
        for k in range(32):
            if x[152+8*k:160+8*k] != y[152+8*k:160+8*k]: print(f'  f{k}: ref={x[152+8*k:160+8*k].hex()} test={y[152+8*k:160+8*k].hex()}')
        sys.exit(1)
    i += 1
