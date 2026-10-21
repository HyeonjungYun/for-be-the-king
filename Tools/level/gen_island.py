"""Island floor layout generator - the 2026-10-11 map expansion from src/맵 조감도.png (0.5 m per image pixel).

Writes Tools/level/floor1.json: the coast polygon, the three validated 56 m houses moved into the image's north-west
cluster (interiors unchanged - the grid files are relative), 4 towers, 9 entries, the trail network, lantern routes,
detours, open chests and cover. Chests and cover are placed by search; everything else is authored here.

Usage:
    python gen_island.py            # writes floor1.json next to this file, then run check_layout.py on it
"""
import json
import math
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
import check_layout as C  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))

# Coast - 38 vertices, Douglas-Peucker 4 px on the image, clockwise from north (research-map Appendix A). Metres, +X north, +Y east
OUTLINE = [
    [153.2, -2.7], [151.2, 40.5], [131.3, 80.5], [132.2, 111.0], [127.7, 152.2], [98.1, 192.5], [89.9, 217.0], [87.6, 234.4],
    [81.0, 249.4], [59.6, 280.5], [50.6, 286.9], [25.8, 294.4], [7.6, 291.9], [0.0, 286.4], [-35.5, 252.9], [-51.8, 224.5],
    [-66.2, 216.5], [-94.0, 184.4], [-111.2, 137.4], [-121.7, 67.4], [-153.2, -6.7], [-147.7, -61.2], [-110.0, -169.4],
    [-86.7, -204.4], [-54.2, -234.9], [-46.2, -262.0], [-32.2, -282.9], [-10.3, -294.4], [12.7, -292.0], [32.3, -263.4],
    [42.7, -255.4], [57.2, -221.3], [65.2, -174.5], [92.2, -131.7], [101.7, -98.2], [106.7, -91.2], [120.7, -83.0], [142.7, -63.6],
]

# The square layout's houses (floor1.json before the island) and where each moves - (dX, dY) in metres
OLD_HOUSES = {
    "H1": {"x0": -36, "x1": 20, "y0": -112, "y1": -56, "doors": [("N", -90, None), ("S", -86, None), ("E", -14, "rubble")],
           "inner_chest": (-10, -82), "grid": "houses/H1.txt", "open_air": ["Y"]},
    "H2": {"x0": -36, "x1": 20, "y0": -28, "y1": 28, "doors": [("N", 2, None), ("S", -2, None)],
           "inner_chest": (-10, -2), "grid": "houses/H2.txt"},
    "H3": {"x0": -36, "x1": 20, "y0": 56, "y1": 112, "doors": [("N", 90, None), ("S", 86, None), ("E", -10, "rubble")],
           "inner_chest": (-10, 82), "grid": "houses/H3.txt"},
}
MOVE = {"H1": (52, -16), "H2": (58, -16), "H3": (-32, -216)}

OLD_HOUSE_CHESTS = [
    (201, "H1", "Y 마당 (첫 방)", 10, -78), (202, "H1", "M 안방 (안쪽)", -10, -82), (203, "H1", "W 사랑방", -10, -106),
    (204, "H1", "E 곳간", -6, -62), (205, "H2", "A 남쪽 홀 (첫 방)", -30, -14), (206, "H2", "K 보스방 (안쪽)", -10, -2),
    (207, "H2", "G 서쪽 홀", 6, -22), (208, "H2", "H 동쪽 홀", -26, 22), (209, "H3", "F 북쪽 방 (첫 방)", 10, 86),
    (210, "H3", "I 안쪽 방", -10, 82), (211, "H3", "G 서쪽 방", -10, 62), (212, "H3", "J 동쪽 방", -2, 106),
]
OLD_BOSS = ("boss", "H2", -6, 2)

TOWERS = [("Ta", 110, -60), ("Tb", -40, -190), ("Tc", 0, 180), ("Td", -100, 45)]

ENTRIES = [
    (1, "outer", None, 130, 34), (2, "outer", None, 56, 246), (3, "outer", None, -74, 174), (4, "outer", None, -128, -40),
    (5, "outer", None, -29, -242), (6, "outer", None, 54, -151),
    (7, "house", "H1", -6, -95), (8, "house", "H2", 12.5, -39), (9, "house", "H3", -78, -148),
]

# Trail network - pieces meet at shared end points (check_layout: an end point within 0.6 m of another polyline joins them)
TRAILS = [
    # the coast road through the six outer entries
    ("ring-ne", [[130, 34], [56, 246]], False),
    ("ring-e", [[56, 246], [-74, 174]], False),
    ("ring-sw", [[-74, 174], [-128, -40]], False),
    ("ring-w", [[-128, -40], [-110, -120], [-95, -165], [-29, -242]], False),
    ("ring-nw", [[-29, -242], [54, -151], [80, -140], [90, -112], [90, -106]], False),
    ("ring-n", [[90, -14], [110, 10], [130, 34]], False),
    # the north road past H1 and H2 to Ta - lantern
    ("N-road-a", [[90, -106], [90, -60]], True),
    ("N-road-b", [[90, -60], [90, -14]], True),
    ("Ta", [[90, -60], [99, -60]], True),
    ("H1N", [[72, -106], [90, -106]], True),
    ("H2N", [[78, -14], [90, -14]], True),
    # the village road south of H1 / H2, on to Tc
    ("V-road", [[4, -126], [4, 170]], False),
    ("H1S", [[16, -102], [4, -102]], False),
    ("H2S", [[22, -18], [4, -18]], False),
    ("H3N", [[-12, -126], [4, -126]], False),
    ("mid-alley", [[4, -58], [90, -58]], False),
    ("H1E", [[38, -72], [38, -58]], False),
    # west of H3 to Tb - lantern
    ("W-road-n", [[-72, -90], [-72, -130]], False),
    ("W-road-s", [[-72, -130], [-72, -175]], True),
    ("H3S", [[-68, -130], [-72, -130]], True),
    ("Tb", [[-72, -175], [-48, -182]], True),
    ("S-alley", [[-72, -90], [4, -90]], False),
    ("H3E", [[-42, -104], [-42, -90]], False),
    ("W-link", [[-95, -165], [-72, -165]], False),
]

LANTERN_ROUTES = [
    ("H1", "Ta", [[72, -106], [90, -106], [90, -60], [99, -60]]),
    ("H2", "Ta", [[78, -14], [90, -14], [90, -60], [99, -60]]),
    ("H3", "Tb", [[-68, -130], [-72, -130], [-72, -175], [-48, -182]]),
]

DETOURS = [
    ("H1", "Ta", [[16, -102], [4, -102], [4, 25], [40, 30], [100, 25], [112, -48]]),
    ("H2", "Ta", [[22, -18], [4, -18], [4, 25], [40, 30], [100, 25], [112, -48]]),
    ("H3", "Tb", [[-12, -126], [4, -126], [10, -200], [-30, -196]]),
]

OPEN_CHEST_COUNT = 18
COVER_GAP = 20.0


def build_houses():
    out = []
    for hid in ("H1", "H2", "H3"):
        h = OLD_HOUSES[hid]
        dx, dy = MOVE[hid]
        doors = []
        for side, at, kind in h["doors"]:
            d = {"side": side, "at": at + (dy if side in "NS" else dx)}
            if kind:
                d["kind"] = kind
            doors.append(d)
        house = {"id": hid, "x0": h["x0"] + dx, "x1": h["x1"] + dx, "y0": h["y0"] + dy, "y1": h["y1"] + dy, "doors": doors,
                 "inner_chest": [h["inner_chest"][0] + dx, h["inner_chest"][1] + dy], "grid": h["grid"]}
        if "open_air" in h:
            house["open_air"] = h["open_air"]
        out.append(house)
    return out


def search_cover(L):
    """Greedy set cover: cover pieces until every open-field point is within COVER_GAP of cover, a house wall or the coast."""
    C.set_floor(L)
    houses, towers, entries = L["houses"], L["towers"], L["entries"]
    tp = [(t["x"], t["y"]) for t in towers]
    ep = [(e["x"], e["y"]) for e in entries]
    polys = [[tuple(p) for p in t["points"]] for t in L["trails"]]

    points = []
    for xi, yi in C.floor_grid(2, 1.0):
        p = (xi, yi)
        if any(h["x0"] - 1 <= xi <= h["x1"] + 1 and h["y0"] - 1 <= yi <= h["y1"] + 1 for h in houses):
            continue
        if min(C.dist(p, t) for t in tp) <= 12.0 or min(C.dist(p, e) for e in ep) <= 9.0:
            continue
        base = min(min(C.rect_dist(p, h) for h in houses), C.boundary_gap(p))
        if base > COVER_GAP - 0.5:
            points.append(p)

    cands = []
    for xi, yi in C.floor_grid(4, 3.0):
        c = (xi, yi)
        if any(h["x0"] - 2 <= xi <= h["x1"] + 2 and h["y0"] - 2 <= yi <= h["y1"] + 2 for h in houses):
            continue
        if min(C.dist(c, t) for t in tp) < 14.0 or min(C.dist(c, e) for e in ep) < 11.0:
            continue
        if min(C.poly_dist(c, q) for q in polys) < 4.0:
            continue
        cands.append(c)

    reach = (COVER_GAP - 1.0) ** 2
    uncovered = set(points)
    chosen = []
    while uncovered:
        best, best_n = None, 0
        for c in cands:
            n = sum(1 for p in uncovered if (p[0] - c[0]) ** 2 + (p[1] - c[1]) ** 2 <= reach)
            if n > best_n:
                best, best_n = c, n
        if best is None:
            print(f"cover: {len(uncovered)} points cannot be covered", file=sys.stderr)
            break
        chosen.append(best)
        uncovered = {p for p in uncovered if (p[0] - best[0]) ** 2 + (p[1] - best[1]) ** 2 > reach}
        cands.remove(best)
    return [{"x": c[0], "y": c[1]} for c in chosen]


def main():
    houses = build_houses()
    hmap = {h["id"]: h for h in houses}
    L = {
        "outline": OUTLINE,
        "open_chest_count": OPEN_CHEST_COUNT,
        "houses": houses,
        "towers": [{"id": t, "x": x, "y": y} for t, x, y in TOWERS],
        "entries": [dict({"id": i, "kind": k, "x": x, "y": y}, **({"house": h} if h else {})) for i, k, h, x, y in ENTRIES],
        "boss_house": "H2",
        "trails": [{"id": i, "points": p, "lantern": lan} for i, p, lan in TRAILS],
        "lantern_routes": [{"house": h, "tower": t, "points": p} for h, t, p in LANTERN_ROUTES],
        "detours": [{"house": h, "tower": t, "points": p} for h, t, p in DETOURS],
    }

    chests = []
    for cid, hid, room, x, y in OLD_HOUSE_CHESTS:
        dx, dy = MOVE[hid]
        chests.append({"id": cid, "house": hid, "room": room, "x": x + dx, "y": y + dy})
    L["house_chests"] = chests
    mid, mh, mx, my = OLD_BOSS
    L["monsters"] = [{"id": mid, "house": mh, "x": mx + MOVE[mh][0], "y": my + MOVE[mh][1]}]

    picked = C.place_chests(L, OPEN_CHEST_COUNT, tries=3000)
    if picked is None:
        sys.exit("open chests could not be placed")
    L["open_chests"] = [{"id": 100 + k, "x": x, "y": y} for k, (x, y) in enumerate(sorted(picked, key=lambda p: (-p[0], p[1])), 1)]

    L["cover"] = search_cover(L)
    print(f"cover pieces: {len(L['cover'])}")

    with open(os.path.join(HERE, "floor1.json"), "w", encoding="utf-8") as f:
        json.dump(L, f, ensure_ascii=False, indent=1)
    print("wrote floor1.json")


if __name__ == "__main__":
    main()
