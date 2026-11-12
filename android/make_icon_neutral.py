"""Original launcher icon (no third-party artwork): a winding gravel road at dusk with a chequered flag. GPLv3.
usage: python3 make_icon_neutral.py OUTDIR   -> OUTDIR/icon_<size>.png for the usual launcher sizes"""
import os, sys, math
from PIL import Image, ImageDraw, ImageFilter

S = 1024

def road_points(t):
    # centre line of the road from the bottom (t=0) to the horizon (t=1), with an S bend
    y = S * (0.98 - 0.56 * t)
    x = S * (0.5 + 0.22 * math.sin(t * 5.2 + 0.4) * (1 - t) ** 0.8)
    w = S * (0.62 * (1 - t) ** 1.35 + 0.012)
    return x, y, w

def draw():
    im = Image.new("RGBA", (S, S))
    d = ImageDraw.Draw(im)
    for y in range(S):   # dusk sky to dark ground
        k = y / S
        if k < 0.42: c = (int(255 - 120 * k / 0.42), int(150 - 70 * k / 0.42), int(60 + 40 * k / 0.42))
        else: c = (int(40 - 20 * (k - 0.42)), int(52 - 22 * (k - 0.42)), int(36 - 10 * (k - 0.42)))
        d.line([(0, y), (S, y)], fill=c + (255,))
    d.ellipse((S * 0.60, S * 0.20, S * 0.80, S * 0.40), fill=(255, 214, 120, 255))   # low sun
    d.polygon([(0, S * 0.45), (S * 0.18, S * 0.36), (S * 0.35, S * 0.43), (S * 0.55, S * 0.34), (S * 0.78, S * 0.44), (S, S * 0.38), (S, S * 0.48), (0, S * 0.48)], fill=(44, 40, 70, 255))
    d.rectangle((0, S * 0.46, S, S), fill=(52, 66, 40, 255))
    N = 160
    left, right = [], []
    for i in range(N + 1):
        x, y, w = road_points(i / N)
        left.append((x - w / 2, y)); right.append((x + w / 2, y))
    d.polygon(left + right[::-1], fill=(176, 140, 96, 255))           # gravel
    for i in range(0, N, 12):                                          # centre dashes
        pts = []
        for j in range(i, min(i + 6, N) + 1):
            x, y, w = road_points(j / N); pts.append((x, y))
        x0, y0, w0 = road_points(i / N)
        d.line(pts, fill=(250, 245, 230, 255), width=max(2, int(w0 * 0.05)), joint="curve")
    # chequered flag on a pole, top left
    px, py = S * 0.16, S * 0.10
    d.rectangle((px - 10, py, px + 10, py + S * 0.50), fill=(235, 235, 235, 255))
    cols, rows, cw, ch = 6, 4, S * 0.075, S * 0.07
    for r in range(rows):
        for c in range(cols):
            wave = math.sin(c * 0.9) * S * 0.015
            x0 = px + 10 + c * cw; y0 = py + r * ch + wave
            d.polygon([(x0, y0), (x0 + cw, y0 + math.sin((c + 1) * 0.9) * S * 0.015 - wave), (x0 + cw, y0 + ch + math.sin((c + 1) * 0.9) * S * 0.015 - wave), (x0, y0 + ch)],
                      fill=(20, 20, 20, 255) if (r + c) % 2 == 0 else (245, 245, 245, 255))
    mask = Image.new("L", (S, S), 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, S - 1, S - 1), radius=int(S * 0.2), fill=255)
    im.putalpha(mask)
    return im

if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else "."
    os.makedirs(out, exist_ok=True)
    im = draw()
    for n in (48, 72, 96, 108, 144, 162, 192, 216, 324, 432, 512):
        im.resize((n, n), Image.LANCZOS).save(os.path.join(out, "icon_%d.png" % n))
    # adaptive-icon foreground (108dp canvas, launcher masks show about the central 66%): the scene in a disc
    sq = draw(); sq.putalpha(255)
    disc = Image.new("L", (S, S), 0); ImageDraw.Draw(disc).ellipse((0, 0, S - 1, S - 1), fill=255)
    sq.putalpha(disc)
    for n in (108, 162, 216, 324, 432):
        fg = Image.new("RGBA", (n, n), (0, 0, 0, 0)); k = int(n * 0.68)
        fg.paste(sq.resize((k, k), Image.LANCZOS), ((n - k) // 2, (n - k) // 2), sq.resize((k, k), Image.LANCZOS))
        fg.save(os.path.join(out, "fg_%d.png" % n))
