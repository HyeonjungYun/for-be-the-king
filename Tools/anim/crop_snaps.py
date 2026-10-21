"""Crop the character out of 1280x720 top-down screenshots and upscale, so stances can be compared by eye.

Usage: python crop_snaps.py <dir> [--size 220] [--scale 3] [--cx 640] [--cy 365]
Writes <name>-crop.png next to each <weapon>-<n>-<pose>.png (skips files already ending in -crop).
"""
import argparse
import glob
import os

from PIL import Image

ap = argparse.ArgumentParser()
ap.add_argument("dir")
ap.add_argument("--size", type=int, default=220)
ap.add_argument("--scale", type=int, default=3)
ap.add_argument("--cx", type=int, default=640)
ap.add_argument("--cy", type=int, default=365)
args = ap.parse_args()

n = 0
for path in sorted(glob.glob(os.path.join(args.dir, "*.png"))):
    if path.endswith("-crop.png") or "-sheet" in path:
        continue
    img = Image.open(path)
    half = args.size // 2
    box = (args.cx - half, args.cy - half, args.cx + half, args.cy + half)
    crop = img.crop(box).resize((args.size * args.scale, args.size * args.scale), Image.LANCZOS)
    out = path[:-4] + "-crop.png"
    crop.save(out)
    n += 1
print(f"cropped {n} file(s)")

# one contact sheet per weapon: idle | run | attack | skill | hit | after
weapons = {}
for path in sorted(glob.glob(os.path.join(args.dir, "*-crop.png"))):
    name = os.path.basename(path)
    weapon = name.split("-")[0]
    weapons.setdefault(weapon, []).append(path)
for weapon, paths in weapons.items():
    tiles = [Image.open(p) for p in paths]
    w, h = tiles[0].size
    sheet = Image.new("RGB", (w * len(tiles), h), (20, 20, 20))
    for i, t in enumerate(tiles):
        sheet.paste(t, (i * w, 0))
    sheet.save(os.path.join(args.dir, f"{weapon}-sheet.png"))
print(f"sheets: {', '.join(sorted(weapons))}")
