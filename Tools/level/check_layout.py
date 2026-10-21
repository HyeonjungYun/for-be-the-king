"""Floor layout checker for design/gdd/level-map.md (Core Rules 4-11, F1, F5, F8).

Usage:
    python check_layout.py layout.json                 # check every rule, print margins
    python check_layout.py layout.json --place-chests  # also search open-field chest spots (layout "open_chest_count", default 12)

Units: metres. Origin = floor centre, +X = north, +Y = east.
The play area is the layout's "outline" polygon (the island coast, 2026-10-11 map expansion) or, without one, the old
square |x|, |y| <= 140. Every boundary distance is measured to the coast (negative = outside).
Houses are axis-aligned rectangles {x0, x1, y0, y1}; doors are {side: N|S|E|W, at: coord along the wall}.
"""
import json
import math
import random
import sys

HALF = 140.0
OUTLINE = []        # the coast polygon in metres (set_floor) - empty = the old square
R = {
    "house_short_min": 52.0, "house_long_max": 60.0,
    "inner_chest_wall": 24.0,
    "tower_sep": 100.0, "tower_house": 30.0, "tower_boundary": 12.0,
    "near_tower_min": 40.0, "near_tower_max": 80.0, "far_tower_min": 80.0,
    "entry_band_min": 15.0, "entry_band_max": 30.0, "entry_outer_sep": 80.0,
    "entry_house_min": 20.0, "entry_house_max": 30.0,
    "entry_tower": 40.0, "entry_clear": 9.0,
    "chest_sep": 45.0, "chest_house": 30.0, "chest_tower": 40.0, "chest_entry": 20.0, "chest_boundary": 10.0,
    "monster_entry": 20.0,
    "d_cover_max": 170.0,
    # island (2026-10-11): the three 56 m houses are 8 % of a 117,600 m2 island - the share is measured on the island area
    "house_area_min": 0.07, "house_area_max": 0.14,
    "tower_count_min": 3, "tower_count_max": 4,
}


def set_floor(L):
    """Reads the floor shape from the layout - the coast polygon, else the 280 m square."""
    global OUTLINE
    OUTLINE = [tuple(p) for p in L.get("outline", [])]


def floor_bounds():
    """(x0, x1, y0, y1) of the play area."""
    if OUTLINE:
        xs = [p[0] for p in OUTLINE]
        ys = [p[1] for p in OUTLINE]
        return min(xs), max(xs), min(ys), max(ys)
    return -HALF, HALF, -HALF, HALF


def floor_area():
    if OUTLINE:
        s = 0.0
        for i in range(len(OUTLINE)):
            a, b = OUTLINE[i], OUTLINE[(i + 1) % len(OUTLINE)]
            s += a[0] * b[1] - b[0] * a[1]
        return abs(s) * 0.5
    return (2 * HALF) ** 2


def inside_outline(p):
    """Ray casting - true inside the coast polygon."""
    inside = False
    n = len(OUTLINE)
    for i in range(n):
        a, b = OUTLINE[i], OUTLINE[(i + 1) % n]
        if (a[1] > p[1]) != (b[1] > p[1]):
            x = a[0] + (p[1] - a[1]) * (b[0] - a[0]) / (b[1] - a[1])
            if p[0] < x:
                inside = not inside
    return inside


def dist(a, b):
    return math.hypot(a[0] - b[0], a[1] - b[1])


def rect_dist(p, h):
    """Distance from point p to rectangle h (0 if inside)."""
    dx = max(h["x0"] - p[0], 0.0, p[0] - h["x1"])
    dy = max(h["y0"] - p[1], 0.0, p[1] - h["y1"])
    return math.hypot(dx, dy)


def inside_depth(p, h):
    """Distance from an inside point to the rectangle's outer boundary."""
    return min(p[0] - h["x0"], h["x1"] - p[0], p[1] - h["y0"], h["y1"] - p[1])


def door_point(h, d):
    s, at = d["side"], d["at"]
    if s == "N":
        return (h["x1"], at)
    if s == "S":
        return (h["x0"], at)
    if s == "E":
        return (at, h["y1"])
    if s == "W":
        return (at, h["y0"])
    raise ValueError(s)


def boundary_gap(p):
    """Distance to the edge of the play area - the coast when there is one. Negative = outside (in the sea)."""
    if OUTLINE:
        n = len(OUTLINE)
        d = min(seg_dist(p, OUTLINE[i], OUTLINE[(i + 1) % n]) for i in range(n))
        return d if inside_outline(p) else -d
    return HALF - max(abs(p[0]), abs(p[1]))


class Report:
    def __init__(self):
        self.rows = []
        self.fail = 0

    def check(self, name, value, limit, op, note=""):
        ok = value >= limit if op == ">=" else value <= limit
        margin = value - limit if op == ">=" else limit - value
        if not ok:
            self.fail += 1
        self.rows.append((("PASS" if ok else "FAIL"), name, f"{value:8.2f}", f"{op} {limit:g}", f"{margin:+7.2f}", note))

    def print(self):
        for r in self.rows:
            print(f"{r[0]}  {r[1]:<44} {r[2]} {r[3]:<8} margin {r[4]}  {r[5]}")
        print(f"\n{len(self.rows)} checks, {self.fail} failed")


def check(L, rep):
    set_floor(L)
    houses, towers, entries = L["houses"], L["towers"], L["entries"]
    chests = L.get("open_chests", [])

    # Houses (R4, R5, F1)
    area = 0.0
    for h in houses:
        w, d = h["x1"] - h["x0"], h["y1"] - h["y0"]
        area += w * d
        rep.check(f"house {h['id']} short side", min(w, d), R["house_short_min"], ">=")
        rep.check(f"house {h['id']} long side", max(w, d), R["house_long_max"], "<=")
        rep.check(f"house {h['id']} boundary gap", min(boundary_gap((h["x0"], h["y0"])), boundary_gap((h["x1"], h["y1"])),
                                                       boundary_gap((h["x0"], h["y1"])), boundary_gap((h["x1"], h["y0"]))), 0.0, ">=")
        sides = {dd["side"] for dd in h["doors"]}
        rep.check(f"house {h['id']} door sides", len(sides), 2, ">=")
        if "inner_chest" in h:
            rep.check(f"house {h['id']} inner chest depth (F1)", inside_depth(h["inner_chest"], h), R["inner_chest_wall"], ">=")
    for i in range(len(houses)):
        for j in range(i + 1, len(houses)):
            a, b = houses[i], houses[j]
            corners = [(a["x0"], a["y0"]), (a["x0"], a["y1"]), (a["x1"], a["y0"]), (a["x1"], a["y1"])]
            gap = min(rect_dist(c, b) for c in corners)
            corners_b = [(b["x0"], b["y0"]), (b["x0"], b["y1"]), (b["x1"], b["y0"]), (b["x1"], b["y1"])]
            gap = min(gap, min(rect_dist(c, a) for c in corners_b))
            rep.check(f"house {a['id']}-{b['id']} gap", gap, 0.0, ">=")
    share = area / floor_area()
    rep.check("house area share (R4) min", share, R["house_area_min"], ">=")
    rep.check("house area share (R4) max", share, R["house_area_max"], "<=")

    # Towers (R6) - 3 on the old square, 4 on the island (one near tower per house, the rest cover the far coast)
    rep.check("tower count min", len(towers), R["tower_count_min"], ">=")
    rep.check("tower count max", len(towers), R["tower_count_max"], "<=")
    tp = {t["id"]: (t["x"], t["y"]) for t in towers}
    for i in range(len(towers)):
        for j in range(i + 1, len(towers)):
            rep.check(f"tower {towers[i]['id']}-{towers[j]['id']} separation", dist(tp[towers[i]['id']], tp[towers[j]['id']]), R["tower_sep"], ">=")
    for t in towers:
        p = tp[t["id"]]
        rep.check(f"tower {t['id']} boundary", boundary_gap(p), R["tower_boundary"], ">=")
        for h in houses:
            rep.check(f"tower {t['id']} - house {h['id']} wall", rect_dist(p, h), R["tower_house"], ">=")
    for h in houses:
        doors = [door_point(h, d) for d in h["doors"]]
        per = sorted(((min(dist(dp, tp[t["id"]]) for dp in doors), t["id"]) for t in towers))
        near_d, near_id = per[0]
        rep.check(f"house {h['id']} near tower {near_id} (straight) min", near_d, R["near_tower_min"] / 1.2, ">=", "path ~ x1.1-1.2")
        rep.check(f"house {h['id']} near tower {near_id} (straight) max", near_d, R["near_tower_max"] / 1.1, "<=", "path <= 80")
        for d2, tid in per[1:]:
            rep.check(f"house {h['id']} far tower {tid}", d2, R["far_tower_min"], ">=")
        if h.get("near_tower") and h["near_tower"] != near_id:
            rep.check(f"house {h['id']} declared near tower matches", 0, 1, ">=", f"declared {h['near_tower']} got {near_id}")

    # Entry points (R7)
    outer = [e for e in entries if e["kind"] == "outer"]
    near = [e for e in entries if e["kind"] == "house"]
    rep.check("entry count", len(entries), 9, ">=")
    rep.check("entry count max", len(entries), 9, "<=")
    for e in outer:
        g = boundary_gap((e["x"], e["y"]))
        rep.check(f"entry {e['id']} band min", g, R["entry_band_min"], ">=")
        rep.check(f"entry {e['id']} band max", g, R["entry_band_max"], "<=")
    for i in range(len(outer)):
        for j in range(i + 1, len(outer)):
            a, b = outer[i], outer[j]
            rep.check(f"entry {a['id']}-{b['id']} outer separation", dist((a["x"], a["y"]), (b["x"], b["y"])), R["entry_outer_sep"], ">=")
    # clockwise from north: compass bearing atan2(y, x) increasing (x = north, y = east)
    bearings = [(math.degrees(math.atan2(e["y"], e["x"])) % 360, e["id"]) for e in outer]
    ids_by_bearing = [i for _, i in sorted(bearings)]
    ids_sorted = sorted(e["id"] for e in outer)
    start = ids_by_bearing.index(ids_sorted[0])
    rotated = ids_by_bearing[start:] + ids_by_bearing[:start]
    rep.check("outer entries numbered clockwise from north", 1 if rotated == ids_sorted else 0, 1, ">=", f"order {rotated}")
    hmap = {h["id"]: h for h in houses}
    for e in near:
        h = hmap[e["house"]]
        dd = min(dist((e["x"], e["y"]), door_point(h, d)) for d in h["doors"])
        rep.check(f"entry {e['id']} to house {h['id']} door min", dd, R["entry_house_min"], ">=")
        rep.check(f"entry {e['id']} to house {h['id']} door max", dd, R["entry_house_max"], "<=")
    for e in entries:
        p = (e["x"], e["y"])
        rep.check(f"entry {e['id']} nearest tower", min(dist(p, tp[t['id']]) for t in towers), R["entry_tower"], ">=")
        rep.check(f"entry {e['id']} clear radius (house walls)", min(rect_dist(p, h) for h in houses), R["entry_clear"], ">=")

    # Open-field chests (R10)
    if chests:
        rep.check("open chest count", len(chests), L.get("open_chest_count", 12), ">=")
        for c in chests:
            p = (c["x"], c["y"])
            rep.check(f"chest {c['id']} boundary", boundary_gap(p), R["chest_boundary"], ">=")
            rep.check(f"chest {c['id']} house", min(rect_dist(p, h) for h in houses), R["chest_house"], ">=")
            rep.check(f"chest {c['id']} tower", min(dist(p, tp[t['id']]) for t in towers), R["chest_tower"], ">=")
            rep.check(f"chest {c['id']} entry", min(dist(p, (e["x"], e["y"])) for e in entries), R["chest_entry"], ">=")
        for i in range(len(chests)):
            for j in range(i + 1, len(chests)):
                a, b = chests[i], chests[j]
                rep.check(f"chest {a['id']}-{b['id']}", dist((a["x"], a["y"]), (b["x"], b["y"])), R["chest_sep"], ">=")

    # Monster spots (R11)
    for m in L.get("monsters", []):
        p = (m["x"], m["y"])
        rep.check(f"monster {m['id']} entry", min(dist(p, (e["x"], e["y"])) for e in entries), R["monster_entry"], ">=")

    # F5 coverage - every point of the play area (the island only, not the sea around it)
    worst, wp = 0.0, None
    for xi, yi in floor_grid(2, 0.0):
        d = min(dist((xi, yi), tp[t["id"]]) for t in towers)
        if d > worst:
            worst, wp = d, (xi, yi)
    rep.check(f"d_cover (F5) worst at {wp}", worst, R["d_cover_max"], "<=", f"t_reach {worst*1.2/3.4:.1f}s")


def floor_grid(step, inset):
    """Integer grid points of the play area at least `inset` metres inside its edge (the square: -140+inset .. 140-inset)."""
    x0, x1, y0, y1 = floor_bounds()
    if not OUTLINE:
        lo, hi = int(math.ceil(-HALF + inset)), int(math.floor(HALF - inset))
        # the square keeps its old even grid (-140 .. 140 step 2, cover -139 .. 139) - the golden numbers depend on it
        for xi in range(lo, hi + 1, step):
            for yi in range(lo, hi + 1, step):
                yield xi, yi
        return
    for xi in range(int(math.ceil(x0)), int(math.floor(x1)) + 1, step):
        for yi in range(int(math.ceil(y0)), int(math.floor(y1)) + 1, step):
            if boundary_gap((xi, yi)) >= inset:
                yield xi, yi


# ─── trails · lanterns · detours · cover (§ 5–6 of design/levels/floor-1.md) ───────────────

# Screen footprint relative to a viewer (measured, camera-footprint-2026-10-05): ahead 13.1 m, behind 8.0 m,
# half width w(x) = 15.5 + 0.2887 x. MARGIN widens it by m_d (F4) on every side.
VIEW_AHEAD, VIEW_BEHIND, HALF_W0, HALF_WK, MARGIN = 13.1, 8.0, 15.5, 0.2887, 1.0


def sees(dx, dy, margin=MARGIN):
    """True if a point at (dx, dy) relative to the viewer is on the viewer's screen (+ margin)."""
    if dx < -(VIEW_BEHIND + margin) or dx > VIEW_AHEAD + margin:
        return False
    return abs(dy) <= HALF_W0 + HALF_WK * dx + margin


def seg_dist(p, a, b):
    ax, ay = a
    bx, by = b
    vx, vy = bx - ax, by - ay
    L2 = vx * vx + vy * vy
    t = 0.0 if L2 == 0 else max(0.0, min(1.0, ((p[0] - ax) * vx + (p[1] - ay) * vy) / L2))
    return math.hypot(p[0] - (ax + t * vx), p[1] - (ay + t * vy))


def poly_dist(p, pts):
    return min(seg_dist(p, pts[i], pts[i + 1]) for i in range(len(pts) - 1))


def poly_len(pts):
    return sum(dist(pts[i], pts[i + 1]) for i in range(len(pts) - 1))


def sample(pts, step=1.0, t0=0.0, t1=1.0):
    """Points every `step` metres along the polyline, between fractions t0..t1 of its length."""
    total = poly_len(pts)
    out, acc = [], 0.0
    for i in range(len(pts) - 1):
        a, b = pts[i], pts[i + 1]
        seg = dist(a, b)
        s = 0.0
        while s <= seg:
            f = (acc + s) / total if total else 0
            if t0 - 1e-9 <= f <= t1 + 1e-9:
                k = s / seg if seg else 0
                out.append((a[0] + (b[0] - a[0]) * k, a[1] + (b[1] - a[1]) * k))
            s += step
        acc += seg
    return out


def seg_hits_rect(a, b, h, shrink=0.0):
    """Does segment a-b pass through the rectangle (shrunk by `shrink` on every side)?"""
    for p in sample([a, b], 0.5):
        if h["x0"] + shrink < p[0] < h["x1"] - shrink and h["y0"] + shrink < p[1] < h["y1"] - shrink:
            return True
    return False


def check_routes(L, rep):
    trails = L.get("trails", [])
    if not trails:
        return
    houses, towers, entries = L["houses"], L["towers"], L["entries"]
    tp = {t["id"]: (t["x"], t["y"]) for t in towers}

    # Trails never run through a house (only door spurs touch the wall).
    for t in trails:
        pts = [tuple(p) for p in t["points"]]
        for h in houses:
            hit = any(seg_hits_rect(pts[i], pts[i + 1], h, shrink=0.6) for i in range(len(pts) - 1))
            rep.check(f"trail {t['id']} stays out of house {h['id']}", 0 if hit else 1, 1, ">=")

    # One connected network: polylines touch (endpoint within 0.6 m of another polyline).
    polys = [[tuple(p) for p in t["points"]] for t in trails]
    n = len(polys)
    adj = [set() for _ in range(n)]
    for i in range(n):
        for j in range(n):
            if i != j and (poly_dist(polys[i][0], polys[j]) < 0.6 or poly_dist(polys[i][-1], polys[j]) < 0.6):
                adj[i].add(j)
                adj[j].add(i)
    seen, stack = {0}, [0]
    while stack:
        for k in adj[stack.pop()]:
            if k not in seen:
                seen.add(k)
                stack.append(k)
    rep.check("trail network connected (polylines reached)", len(seen), n, ">=", f"{len(seen)}/{n}")

    def near_trail(p):
        return min(poly_dist(p, q) for q in polys)

    for h in houses:
        for d in h["doors"]:
            rep.check(f"house {h['id']} door {d['side']} on a trail", near_trail(door_point(h, d)), 1.0, "<=")
    for t in towers:
        rep.check(f"tower {t['id']} reached by a trail (ring edge)", near_trail(tp[t["id"]]), 12.5, "<=")
    for e in entries:
        rep.check(f"entry {e['id']} beside a trail", near_trail((e["x"], e["y"])), 12.0, "<=")

    # Lantern routes: each house has one unbroken lantern polyline from a door to a tower.
    lantern = [[tuple(p) for p in t["points"]] for t in trails if t.get("lantern")]
    for r in L.get("lantern_routes", []):
        pts = [tuple(p) for p in r["points"]]
        h = next(hh for hh in houses if hh["id"] == r["house"])
        start_ok = min(dist(pts[0], door_point(h, d)) for d in h["doors"])
        rep.check(f"lantern {r['house']}->{r['tower']} starts at a door", start_ok, 1.0, "<=")
        rep.check(f"lantern {r['house']}->{r['tower']} ends at the tower ring", dist(pts[-1], tp[r["tower"]]), 12.5, "<=")
        on_ground = [p for p in sample(pts, 1.0) if dist(p, tp[r["tower"]]) > 12.0]   # the plaza itself is paved
        off = max(min(poly_dist(p, q) for q in lantern) for p in on_ground)
        rep.check(f"lantern {r['house']}->{r['tower']} lies on lantern trails", off, 0.6, "<=")
        rep.check(f"lantern {r['house']}->{r['tower']} path length", poly_len(pts), R["near_tower_max"], "<=", "40-80 m")
        rep.check(f"lantern {r['house']}->{r['tower']} path length min", poly_len(pts), R["near_tower_min"], ">=")

    # Detours (F4, AC-LM-16): nobody on the lantern route ever has a detour walker (middle third) on screen,
    # and vice versa — brute force every 1 m on both.
    for dt in L.get("detours", []):
        pts = [tuple(p) for p in dt["points"]]
        route = next(r for r in L["lantern_routes"] if r["house"] == dt["house"])
        A = sample([tuple(p) for p in route["points"]], 1.0)
        B = sample(pts, 1.0, 1 / 3, 2 / 3)
        seen_pairs = sum(1 for a in A for b in B if sees(b[0] - a[0], b[1] - a[1]) or sees(a[0] - b[0], a[1] - b[1]))
        rep.check(f"detour {dt['house']} middle third never on lantern walkers' screen", -seen_pairs, 0, ">=",
                  f"{seen_pairs} visible pairs")
        clearance = min(poly_dist(b, [tuple(p) for p in route["points"]]) for b in B)
        extra = poly_len(pts) - poly_len([tuple(p) for p in route["points"]])
        rep.check(f"detour {dt['house']} min distance to lantern route", clearance, 0.0, ">=",
                  f"detour {poly_len(pts):.0f} m, +{extra:.0f} m = +{extra / 3.4:.1f}s")
        h = next(hh for hh in houses if hh["id"] == dt["house"])
        rep.check(f"detour {dt['house']} starts at a door", min(dist(pts[0], door_point(h, d)) for d in h["doors"]), 1.0, "<=")
        rep.check(f"detour {dt['house']} ends at tower ring", dist(pts[-1], tp[dt["tower"]]), 12.5, "<=")
        for hh in houses:
            hit = any(seg_hits_rect(pts[i], pts[i + 1], hh, shrink=0.6) for i in range(len(pts) - 1))
            rep.check(f"detour {dt['house']} stays out of house {hh['id']}", 0 if hit else 1, 1, ">=")

    # Cover (R9, F7): every open-field point within 20 m of cover, except tower rings and entry circles.
    # House outer walls and the boundary band count as cover too — both block skills (2026-10-05 decision).
    cover = [(c["x"], c["y"]) for c in L.get("cover", [])]
    if cover:
        worst, wp = 0.0, None
        for xi, yi in floor_grid(2, 1.0):
            p = (xi, yi)
            if any(h["x0"] - 1 <= xi <= h["x1"] + 1 and h["y0"] - 1 <= yi <= h["y1"] + 1 for h in houses):
                continue
            if min(dist(p, tp[t["id"]]) for t in towers) <= 12.0:
                continue
            if min(dist(p, (e["x"], e["y"])) for e in entries) <= 9.0:
                continue
            d = min(min(dist(p, c) for c in cover), min(rect_dist(p, h) for h in houses), boundary_gap(p))
            if d > worst:
                worst, wp = d, p
        rep.check(f"cover gap (R9) worst at {wp}", worst, 20.0, "<=", f"{len(cover)} cover pieces")
        for c in cover:
            rep.check(f"cover ({c[0]},{c[1]}) outside tower rings", min(dist(c, tp[t['id']]) for t in towers), 12.0 + 1.0, ">=")
            rep.check(f"cover ({c[0]},{c[1]}) outside entry circles", min(dist(c, (e['x'], e['y'])) for e in entries), 9.0 + 1.0, ">=")
            rep.check(f"cover ({c[0]},{c[1]}) off trails", min(poly_dist(c, q) for q in polys), 3.0, ">=")


def chest_ok(p, L):
    houses, towers, entries = L["houses"], L["towers"], L["entries"]
    if boundary_gap(p) < R["chest_boundary"]:
        return False
    if min(rect_dist(p, h) for h in houses) < R["chest_house"]:
        return False
    if min(dist(p, (t["x"], t["y"])) for t in towers) < R["chest_tower"]:
        return False
    if min(dist(p, (e["x"], e["y"])) for e in entries) < R["chest_entry"]:
        return False
    return True


def place_chests(L, n=None, tries=4000, seed=7):
    set_floor(L)
    n = n or L.get("open_chest_count", 12)
    rng = random.Random(seed)
    cand = [p for p in floor_grid(2, 10.0) if chest_ok(p, L)]
    print(f"\nfree candidate points (2 m grid): {len(cand)}")
    best, best_score = None, -1
    for _ in range(tries):
        rng.shuffle(cand)
        pick = []
        for p in cand:
            if all(dist(p, q) >= R["chest_sep"] for q in pick):
                pick.append(p)
                if len(pick) == n:
                    break
        if len(pick) < n:
            continue
        min_sep = min(dist(pick[i], pick[j]) for i in range(n) for j in range(i + 1, n))
        quads = [0, 0, 0, 0]
        for x, y in pick:
            quads[(x >= 0) * 2 + (y >= 0)] += 1
        balance = min(quads)
        score = balance * 100 + min_sep
        if score > best_score:
            best, best_score = list(pick), score
    if best is None:
        print("could not place", n, "chests")
        return None
    quads = [0, 0, 0, 0]
    for x, y in best:
        quads[(x >= 0) * 2 + (y >= 0)] += 1
    print(f"placed {n} chests  min sep {min(dist(best[i], best[j]) for i in range(n) for j in range(i + 1, n)):.1f} m  "
          f"quadrants SW/SE/NW/NE = {quads}")
    for k, (x, y) in enumerate(sorted(best, key=lambda p: (-p[0], p[1])), 1):
        print(f'  {{"id": {100 + k}, "x": {x}, "y": {y}}},')
    return best


if __name__ == "__main__":
    L = json.load(open(sys.argv[1], encoding="utf-8"))
    rep = Report()
    check(L, rep)
    check_routes(L, rep)
    rep.print()
    if "--place-chests" in sys.argv:
        place_chests(L)
