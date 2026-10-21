"""Draw generated patrol routes over the floor-0 island (PNG). +X = north = up, +Y = east = right.

Usage: python draw_routes.py <routes.csv> <out.png> [--data C:\\Server\\MMO\\Server\\Data]

routes.csv is what the server writes at boot (Server/Logs/routes/routes-floor0-set<k>.csv):
route_id,floor_id,seq,x,y,kind,crossing. Walls, houses, towers, entry points and the coast come from Server/Data.
"""
import argparse
import csv
import math
import os
import sys

from PIL import Image, ImageDraw, ImageFont

ap = argparse.ArgumentParser()
ap.add_argument("routes")
ap.add_argument("out")
ap.add_argument("--data", default=r"C:\Server\MMO\Server\Data")
ap.add_argument("--floor", type=int, default=0)
args = ap.parse_args()


def rows(name):
    path = os.path.join(args.data, name)
    if not os.path.exists(path):
        return []
    with open(path, encoding="utf-8-sig", newline="") as f:
        return [r for r in csv.DictReader(f) if int(r.get("floor_id", args.floor)) == args.floor]


outline = sorted(rows("floor_outline.csv"), key=lambda r: int(r["seq"]))
O = [(float(r["x"]), float(r["y"])) for r in outline]
walls = rows("walls.csv")
houses = rows("houses.csv")
towers = rows("towers.csv")
entries = rows("entry_points.csv")

with open(args.routes, encoding="utf-8-sig", newline="") as f:
    routes = {}
    for r in csv.DictReader(f):
        if int(r["floor_id"]) != args.floor:
            continue
        routes.setdefault(int(r["route_id"]), {"kind": r.get("kind", "?"), "crossing": r.get("crossing", "0"), "pts": []})
        routes[int(r["route_id"])]["pts"].append((int(r["seq"]), float(r["x"]), float(r["y"])))

if O:
    X0, X1 = min(p[0] for p in O) - 1000, max(p[0] for p in O) + 1000
    Y0, Y1 = min(p[1] for p in O) - 1000, max(p[1] for p in O) + 1000
else:
    X0, X1, Y0, Y1 = -31000, 31000, -31000, 31000
S = 0.02  # px per cm = 2 px per m
PAD = 40
W = int((Y1 - Y0) * S) + PAD * 2
H = int((X1 - X0) * S) + PAD * 2
img = Image.new("RGB", (W, H), (160, 196, 220) if O else (236, 232, 222))
d = ImageDraw.Draw(img)


def px(x, y):
    return (PAD + (y - Y0) * S, PAD + (X1 - x) * S)


def font(sz):
    for name in ("malgun.ttf", "arial.ttf"):
        try:
            return ImageFont.truetype(name, sz)
        except OSError:
            pass
    return ImageFont.load_default()


F = font(13)
if O:
    d.polygon([px(x, y) for x, y in O], fill=(214, 206, 186), outline=(90, 80, 60))
for h in houses:
    x0, x1, y0, y1 = (float(h[k]) for k in ("x0", "x1", "y0", "y1"))
    d.rectangle([px(x1, y0), px(x0, y1)], fill=(120, 110, 100), outline=(60, 50, 40))
    d.text(px(x1, y0), h["house_id"], fill=(255, 255, 255), font=F)
for w in walls:
    cx, cy, hl, ht, yaw = (float(w[k]) for k in ("cx", "cy", "half_length", "half_thickness", "yaw"))
    if w["kind"] == "boundary":
        continue
    c, s = math.cos(math.radians(yaw)), math.sin(math.radians(yaw))
    corners = []
    for sx, sy in ((hl, ht), (hl, -ht), (-hl, -ht), (-hl, ht)):
        corners.append(px(cx + sx * c - sy * s, cy + sx * s + sy * c))
    d.polygon(corners, fill=(70, 60, 50) if w["kind"] != "cover" else (140, 130, 110))
for t in towers:
    x, y = float(t["x"]), float(t["y"])
    d.ellipse([px(x + 2000, y - 2000), px(x - 2000, y + 2000)], outline=(200, 60, 60), width=1)
    d.ellipse([px(x + 300, y - 300), px(x - 300, y + 300)], fill=(200, 60, 60))
    d.text(px(x + 2200, y), t["tower_id"], fill=(160, 30, 30), font=F)
for e in entries:
    x, y = float(e["x"]), float(e["y"])
    d.ellipse([px(x + 2000, y - 2000), px(x - 2000, y + 2000)], outline=(60, 120, 200), width=1)
    d.ellipse([px(x + 250, y - 250), px(x - 250, y + 250)], fill=(60, 120, 200))

COLORS = [(220, 40, 40), (40, 120, 220), (30, 160, 60), (220, 140, 20), (150, 50, 200), (20, 170, 170), (200, 60, 140), (110, 80, 30), (60, 60, 60), (240, 200, 0),
          (0, 90, 140), (170, 0, 70)]
for i, (rid, r) in enumerate(sorted(routes.items())):
    pts = [px(x, y) for _, x, y in sorted(r["pts"])]
    col = COLORS[i % len(COLORS)]
    d.line(pts + [pts[0]], fill=col, width=2)
    d.ellipse([pts[0][0] - 4, pts[0][1] - 4, pts[0][0] + 4, pts[0][1] + 4], fill=col)
    d.text((pts[0][0] + 6, pts[0][1] - 6), f"{rid} {r['kind'][:1]}{'x' if r['crossing'] == '1' else 'u'}", fill=col, font=F)

d.text((PAD, 6), f"{os.path.basename(args.routes)}  routes={len(routes)}  d=detect r=reactive  x=crossing u=same side  red ring=tower 20 m  blue ring=entry 20 m", fill=(20, 20, 20), font=F)
img.save(args.out)
print("wrote", args.out, img.size)
