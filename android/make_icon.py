import os, sys
from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
RES = os.path.join(HERE, "app", "src", "main", "res")
BG_TOP, BG_BOT = (14, 20, 38), (34, 46, 78)
LEGACY = {"mdpi": 48, "hdpi": 72, "xhdpi": 96, "xxhdpi": 144, "xxxhdpi": 192}
ADAPT = {"mdpi": 108, "hdpi": 162, "xhdpi": 216, "xxhdpi": 324, "xxxhdpi": 432}


def placeholder():
    s = 1024
    im = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.rounded_rectangle((80, 330, s - 80, 700), 120, fill=(215, 30, 40, 255))
    try:
        f = ImageFont.load_default(size=300)
    except TypeError:
        f = ImageFont.load_default()
    d.text((s // 2, 515), "SR2", fill=(255, 255, 255, 255), font=f, anchor="mm")
    return im


def fit(art, box):
    w, h = art.size
    k = min(box / w, box / h)
    return art.resize((max(1, round(w * k)), max(1, round(h * k))), Image.LANCZOS)


def gradient(n):
    g = Image.new("RGBA", (n, n))
    px = g.load()
    for y in range(n):
        t = y / (n - 1)
        c = tuple(round(BG_TOP[i] + (BG_BOT[i] - BG_TOP[i]) * t) for i in range(3)) + (255,)
        for x in range(n):
            px[x, y] = c
    return g


def paste_center(dst, art):
    dst.alpha_composite(art, ((dst.width - art.width) // 2, (dst.height - art.height) // 2))


def main():
    src = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "icon.png")
    art = Image.open(src).convert("RGBA") if os.path.exists(src) else placeholder()
    bb = art.getbbox()
    if bb:
        art = art.crop(bb)
    for dens, n in LEGACY.items():
        d = os.path.join(RES, "mipmap-" + dens)
        os.makedirs(d, exist_ok=True)
        base = gradient(n)
        paste_center(base, fit(art, round(n * 0.9)))
        mask = Image.new("L", (n, n), 0)
        ImageDraw.Draw(mask).rounded_rectangle((0, 0, n - 1, n - 1), round(n * 0.2), fill=255)
        out = Image.new("RGBA", (n, n), (0, 0, 0, 0))
        out.paste(base, (0, 0), mask)
        out.save(os.path.join(d, "ic_launcher.png"))
    for dens, n in ADAPT.items():
        d = os.path.join(RES, "mipmap-" + dens)
        fg = Image.new("RGBA", (n, n), (0, 0, 0, 0))
        paste_center(fg, fit(art, round(n * 0.60)))
        fg.save(os.path.join(d, "ic_launcher_foreground.png"))
    v26 = os.path.join(RES, "mipmap-anydpi-v26")
    os.makedirs(v26, exist_ok=True)
    open(os.path.join(v26, "ic_launcher.xml"), "w").write(
        '<?xml version="1.0" encoding="utf-8"?>\n'
        '<adaptive-icon xmlns:android="http://schemas.android.com/apk/res/android">\n'
        '    <background android:drawable="@color/ic_launcher_background"/>\n'
        '    <foreground android:drawable="@mipmap/ic_launcher_foreground"/>\n'
        '</adaptive-icon>\n')
    vals = os.path.join(RES, "values")
    os.makedirs(vals, exist_ok=True)
    open(os.path.join(vals, "ic_launcher_background.xml"), "w").write(
        '<?xml version="1.0" encoding="utf-8"?>\n<resources>\n    <color name="ic_launcher_background">#0E1426</color>\n</resources>\n')
    print("icons written to", RES)


main()
