#!/usr/bin/env python3
"""Upscale a dumped asset folder into a replacement pack (hash-named files are kept; only pixels change).

  srally2 build/images -dump-assets ./assets          # 1. dump (play through menus/races to collect more)
  python3 tools/upscale_assets.py ./assets ./pack -s 4 # 2. upscale
  srally2 build/images -pack ./pack                   # 3. play with the pack

Engines (-e): lanczos (PIL, always available), realesrgan (realesrgan-ncnn-vulkan / Upscayl binary on PATH or --bin),
or cmd (--cmd 'mytool {in} {out}' -- any external/generative upscaler; it must produce an image SCALE times larger).
Alpha is always upscaled separately (bicubic) and re-attached, so engines that drop alpha are fine.
2D tiles must keep an exact 8S x 8S size (same S for all tiles). 2D layers may be any size; a layer wider than 1.5:1
is drawn across the whole window (use --wide-layers W H to pad/extend layers to e.g. 16:9, see README).
Never commit or publish the dumped or upscaled files: they are derived from the game ROMs.
"""
import argparse, os, shutil, subprocess, sys, tempfile, json
from PIL import Image

def engine_up(img_rgb, a):
    if a.engine == 'lanczos':
        return img_rgb.resize((img_rgb.width*a.scale, img_rgb.height*a.scale), Image.LANCZOS)
    with tempfile.TemporaryDirectory() as d:
        i = os.path.join(d, 'in.png'); o = os.path.join(d, 'out.png'); img_rgb.save(i)
        if a.engine == 'realesrgan':
            cmd = [a.bin or 'realesrgan-ncnn-vulkan', '-i', i, '-o', o, '-s', str(a.scale), '-n', a.model]
        else:
            cmd = a.cmd.format(**{'in': i, 'out': o, 'scale': a.scale}).split()
        subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        r = Image.open(o).convert('RGB')
        want = (img_rgb.width*a.scale, img_rgb.height*a.scale)
        return r if r.size == want else r.resize(want, Image.LANCZOS)

def process(src, dst, a, exact):
    im = Image.open(src).convert('RGBA')
    rgb, al = im.convert('RGB'), im.getchannel('A')
    # pad tiny images so neural engines have context
    up = engine_up(rgb, a)
    alu = al.resize(up.size, Image.BICUBIC) if al.getextrema() != (255, 255) else Image.new('L', up.size, 255)
    up.putalpha(alu)
    if exact:
        assert up.size == (im.width*a.scale, im.height*a.scale)
    up.save(dst)

def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('src'); p.add_argument('dst')
    p.add_argument('-s', '--scale', type=int, default=4)
    p.add_argument('-e', '--engine', default='lanczos', choices=['lanczos', 'realesrgan', 'cmd'])
    p.add_argument('--bin'); p.add_argument('--cmd'); p.add_argument('--model', default='realesrgan-x4plus')
    p.add_argument('--only', choices=['3d', '2d_tiles', '2d_layers'], action='append')
    a = p.parse_args()
    if a.engine == 'cmd' and not a.cmd: sys.exit('--cmd required')
    kinds = a.only or ['3d', '2d_layers', '2d_tiles']
    for k in kinds:
        sd = os.path.join(a.src, k)
        if not os.path.isdir(sd): continue
        dd = os.path.join(a.dst, k); os.makedirs(dd, exist_ok=True)
        fs = sorted(f for f in os.listdir(sd) if f.endswith('.png'))
        for n, f in enumerate(fs):
            out = os.path.join(dd, f)
            if os.path.exists(out): continue
            process(os.path.join(sd, f), out, a, k == '2d_tiles')
            if n % 50 == 0: print(f'{k}: {n}/{len(fs)}', flush=True)
    json.dump({'scale': a.scale}, open(os.path.join(a.dst, 'pack.json'), 'w'))
    print('done ->', a.dst)

main()
