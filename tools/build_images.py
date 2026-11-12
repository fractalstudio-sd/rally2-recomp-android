#!/usr/bin/env python3
"""Build flat region images from a ROM zip using a layout JSON (data/<game>.json). CRC32 of every file is checked.
usage: python3 -I build_images.py data/srally2.json srally2.zip outdir
crom/banked_crom come out big-endian (PPC-native); the other regions are written as loaded."""
import sys, zipfile, zlib, os, json
layout, zpath, out = sys.argv[1:4]
os.makedirs(out, exist_ok=True)
z = zipfile.ZipFile(zpath)
for r in json.load(open(layout))['regions']:
    stride, chunk, swap = r['stride'], r['chunk_size'], r['byte_swap']
    if swap and stride == 1 and chunk == 1:
        stride = chunk = 2  # Supermodel GameLoader special case
    parts, size = [], 0
    for f in r['files']:
        data = z.read(f['name'])
        if zlib.crc32(data) & 0xffffffff != f['crc32']:
            sys.exit(f"CRC mismatch in {f['name']}: this is not the expected ROM set")
        parts.append((f['offset'], data))
        size = max(size, f['offset'] + (len(data) // chunk - 1) * stride + chunk)
    size = -(-size // stride) * stride
    buf = bytearray(size)
    for off, data in parts:
        for i in range(len(data) // chunk):
            c = data[i*chunk:(i+1)*chunk]
            buf[off + i*stride: off + i*stride + chunk] = c[::-1] if swap else c
    open(os.path.join(out, r['name'] + '.bin'), 'wb').write(buf)
    print(f"{r['name']:20s} {len(buf):>10d} bytes  ({len(r['files'])} files, CRC ok)")
