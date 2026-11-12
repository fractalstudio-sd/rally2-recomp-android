import sys, json, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from lifter import *
# usage: gen_funcs.py outdir edges.txt data_image.bin image:base:functions.json [image:base:functions.json ...]
outdir, edges_f, data_img = sys.argv[1:4]
mem, entries = [], []
for spec in sys.argv[4:]:
    img, base, fj = spec.rsplit(':', 2)
    base = int(base, 0)
    mem.append((base, decode_image(img)))
    entries += [int(f['entry'], 16) for f in json.load(open(fj))['functions']]
edges = {tuple(int(x, 16) for x in l.split()) for l in open(edges_f)}
data_mem = [(0, decode_image(data_img))]          # CROM start is copied 1:1 to RAM at boot: jump tables live here
print(gen_functions(mem, entries, edges, outdir + '/recomp_funcs.c', outdir + '/recomp.h', outdir + '/recomp_dispatch.c', data_mem))
