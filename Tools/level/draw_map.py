"""Draw a top-down floor map (PNG) from a layout JSON. +X = north = up, +Y = east = right.

Usage: python draw_map.py layout.json out.png
"""
import json
import sys

from PIL import Image, ImageDraw, ImageFont

L = json.load(open(sys.argv[1], encoding="utf-8"))
OUT = sys.argv[2]
BOSS = L.get("boss_house")
OUTLINE = L.get("outline", [])

# The island (2026-10-11): a rectangle around the coast at 2 px/m, sea outside. The old square: 280 m at 4 px/m
if OUTLINE:
    S = 2
    X0, X1 = min(p[0] for p in OUTLINE) - 10, max(p[0] for p in OUTLINE) + 10
    Y0, Y1 = min(p[1] for p in OUTLINE) - 10, max(p[1] for p in OUTLINE) + 10
else:
    S = 4
    X0, X1, Y0, Y1 = -140, 140, -140, 140
PAD = 60
W = int((Y1 - Y0) * S) + PAD * 2
H = int((X1 - X0) * S) + PAD * 2
img = Image.new("RGB", (W, H), (160, 196, 220) if OUTLINE else (236, 232, 222))
d = ImageDraw.Draw(img)


def font(sz):
    for name in ("malgun.ttf", "arial.ttf"):
        try:
            return ImageFont.truetype(name, sz)
        except OSError:
            pass
    return ImageFont.load_default()


F_S, F_M, F_L = font(14), font(18), font(24)


def px(x, y):
    """World (x north, y east) in metres -> image pixel."""
    return PAD + (y - Y0) * S, PAD + (X1 - x) * S


# the island ground over the sea, then the grid inside it
if OUTLINE:
    d.polygon([px(p[0], p[1]) for p in OUTLINE], fill=(236, 232, 222))

# grid every 20 m, labels every 40 m
gx0, gx1 = int(X0 // 20 * 20), int(X1 // 20 * 20 + 20)
gy0, gy1 = int(Y0 // 20 * 20), int(Y1 // 20 * 20 + 20)
for v in range(gy0, gy1 + 1, 20):
    c = (210, 205, 195) if v % 40 else (190, 184, 172)
    d.line([px(X1, v), px(X0, v)], fill=c, width=1)
    if v % 40 == 0:
        gx, gy = px(X0, v)
        d.text((gx - 10, gy + 6), f"y{v:+d}", fill=(120, 110, 100), font=F_S)
for v in range(gx0, gx1 + 1, 20):
    c = (210, 205, 195) if v % 40 else (190, 184, 172)
    d.line([px(v, Y0), px(v, Y1)], fill=c, width=1)
    if v % 40 == 0:
        gx, gy = px(v, Y0)
        d.text((gx - 50, gy - 8), f"x{v:+d}", fill=(120, 110, 100), font=F_S)

# boundary - the coast, or the square band
if OUTLINE:
    pts = [px(p[0], p[1]) for p in OUTLINE]
    d.line(pts + [pts[0]], fill=(70, 60, 50), width=4)
else:
    d.rectangle([px(140, -140), px(-140, 140)], outline=(70, 60, 50), width=4)

# trails (optional)
for t in L.get("trails", []):
    pts = [px(p[0], p[1]) for p in t["points"]]
    d.line(pts, fill=(196, 170, 120), width=int(3 * S))
    if t.get("lantern"):
        d.line(pts, fill=(250, 214, 120), width=3)
for t in L.get("detours", []):
    pts = [px(p[0], p[1]) for p in t["points"]]
    for i in range(len(pts) - 1):
        d.line([pts[i], pts[i + 1]], fill=(120, 150, 110), width=3)

# cover (optional)
for c in L.get("cover", []):
    x, y = px(c["x"], c["y"])
    d.rectangle([x - 5, y - 5, x + 5, y + 5], fill=(110, 110, 110))

# tower rings + towers
for t in L["towers"]:
    x, y = px(t["x"], t["y"])
    r = 12 * S
    d.ellipse([x - r, y - r, x + r, y + r], fill=(222, 214, 196), outline=(150, 120, 60), width=2)
    d.rectangle([x - 10, y - 10, x + 10, y + 10], fill=(60, 90, 160))
    d.text((x + 14, y - 30), f"{t['id']} ({t['x']},{t['y']})", fill=(40, 60, 120), font=F_M)

# houses
import os
BASE = os.path.dirname(os.path.abspath(sys.argv[1]))
for h in L["houses"]:
    a, b = px(h["x1"], h["y0"]), px(h["x0"], h["y1"])
    boss = h["id"] == BOSS
    d.rectangle([a, b], fill=(200, 160, 140) if boss else (205, 190, 165), outline=(90, 60, 40), width=4)
    if "grid" in h:   # interior walls from the 29 x 29 grid (design/levels/floor-1.md § 4)
        g = [ln.rstrip("\r\n") for ln in open(os.path.join(BASE, h["grid"]), encoding="utf-8")]
        for r in range(1, 28):
            for c in range(1, 28):
                ch = g[r][c]
                if (r % 2 == 0) == (c % 2 == 0) or ch not in "-|D~":
                    continue
                if r % 2 == 0:   # horizontal wall line between rows of cells: runs along y (east-west)
                    lx, ly0 = h["x0"] + (28 - r) * 2, h["y0"] + (c - 1) * 2
                    p0, p1 = px(lx, ly0), px(lx, ly0 + 4)
                else:            # vertical wall line: runs along x
                    ly, lx0 = h["y0"] + c * 2, h["x0"] + (27 - r) * 2
                    p0, p1 = px(lx0, ly), px(lx0 + 4, ly)
                col = {"-": (90, 60, 40), "|": (90, 60, 40), "D": (205, 190, 165), "~": (160, 140, 120)}[ch]
                d.line([p0, p1], fill=col, width=3 if ch in "-|" else 2)
        for r in range(1, 28, 2):
            for c in range(1, 28, 2):
                if g[r][c] in "cB":
                    u, v = (27 - r) // 2, (c - 1) // 2
                    x, y = px(h["x0"] + 4 * u + 2, h["y0"] + 4 * v + 2)
                    fill = (240, 200, 40) if g[r][c] == "c" else (150, 30, 30)
                    d.rectangle([x - 6, y - 6, x + 6, y + 6], fill=fill, outline=(80, 50, 0))
    for dd in h["doors"]:
        s, at = dd["side"], dd["at"]
        if s in "NS":
            x = h["x1"] if s == "N" else h["x0"]
            p0, p1 = px(x, at - 1.5), px(x, at + 1.5)
        else:
            y = h["y1"] if s == "E" else h["y0"]
            p0, p1 = px(at - 1.5, y), px(at + 1.5, y)
        d.line([p0, p1], fill=(236, 232, 222), width=8)
    cx, cy = px((h["x0"] + h["x1"]) / 2, (h["y0"] + h["y1"]) / 2)
    label = h["id"] + (" (보스)" if boss else "")
    d.text((cx - 40, cy - 12), label, fill=(60, 30, 20), font=F_L)
    d.text((cx - 70, cy + 18), f"x {h['x0']}..{h['x1']}  y {h['y0']}..{h['y1']}", fill=(60, 30, 20), font=F_S)

# open-field chests
for c in L.get("open_chests", []):
    x, y = px(c["x"], c["y"])
    d.rectangle([x - 7, y - 7, x + 7, y + 7], fill=(240, 200, 40), outline=(120, 90, 0), width=2)
    d.text((x + 9, y - 8), str(c["id"]), fill=(120, 90, 0), font=F_S)

# entries
for e in L["entries"]:
    x, y = px(e["x"], e["y"])
    r = 9 * S
    d.ellipse([x - r, y - r, x + r, y + r], outline=(170, 60, 60), width=2)
    d.ellipse([x - 13, y - 13, x + 13, y + 13], fill=(200, 70, 70))
    d.text((x - 5, y - 10), str(e["id"]), fill=(255, 255, 255), font=F_M)

# compass + legend
d.text((PAD, 14), "북 (+X) ↑   동 (+Y) →   격자 20 m", fill=(60, 50, 40), font=F_M)
d.text((W - 560, 14), "■ 타워(12 m 광장)  ● 진입 지점(9 m 원)  ■ 상자", fill=(60, 50, 40), font=F_M)
img.save(OUT)
print("saved", OUT, img.size)
