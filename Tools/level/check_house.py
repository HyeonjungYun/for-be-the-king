"""House interior checker for design/levels/floor-1.md § 4 (4 m grid, 14 x 14 cells).

Grid file: 29 x 29 characters. Cell (u, v) is at row 27 - 2u, column 2v + 1 (u = 0 south, v = 0 west).
Edge characters: ' ' open, '-' or '|' wall, 'D' door frame (3 m opening), '~' rubble gap (passable).
Cell characters: room letter, '.' corridor, 'c' chest, 'B' boss spot. Grid points: '+' pillar, 'o' free pillar.

Usage: python check_house.py H1.txt x0 y0 [H2.txt x0 y0 ...]
"""
import math
import sys
from collections import deque

N = 14
CELL = 4.0
PASS = {" ", "D", "~"}


def load(path):
    g = [line.rstrip("\r\n") for line in open(path, encoding="utf-8")]
    assert len(g) == 29 and all(len(r) == 29 for r in g), path
    return g


def cell(g, u, v):
    return g[27 - 2 * u][2 * v + 1]


def edge(g, u, v, du, dv):
    """Character on the edge between cell (u, v) and its neighbour (u+du, v+dv)."""
    r, c = 27 - 2 * u, 2 * v + 1
    return g[r - du][c + dv]


def analyse(path, x0, y0):
    g = load(path)
    name = path.split("/")[-1].split("\\")[-1].split(".")[0]
    fails = []

    def world(lu, lv):  # local metres -> world
        return (x0 + lu, y0 + lv)

    # rooms = components over open (' ') edges
    room = {}
    rid = 0
    for u in range(N):
        for v in range(N):
            if (u, v) in room:
                continue
            q = deque([(u, v)])
            room[(u, v)] = rid
            while q:
                a, b = q.popleft()
                for du, dv in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                    na, nb = a + du, b + dv
                    if 0 <= na < N and 0 <= nb < N and (na, nb) not in room and edge(g, a, b, du, dv) == " ":
                        room[(na, nb)] = rid
                        q.append((na, nb))
            rid += 1
    rooms = {}
    for k, r in room.items():
        rooms.setdefault(r, []).append(k)
    label = {}
    for r, cells in rooms.items():
        letters = [cell(g, u, v) for u, v in cells if cell(g, u, v) not in "cB"]
        label[r] = max(set(letters), key=letters.count) if letters else "?"

    # openings: internal (between rooms) and outer (on the boundary)
    openings, outer = [], []
    for u in range(N):
        for v in range(N):
            for du, dv in ((1, 0), (0, 1)):
                nu, nv = u + du, v + dv
                if nu < N and nv < N:
                    ch = edge(g, u, v, du, dv)
                    if ch in ("D", "~"):
                        p = (4 * u + 2 + 2 * du, 4 * v + 2 + 2 * dv)
                        openings.append((p, ch, (u, v), (nu, nv)))
    for v in range(N):
        for (r, c, side, u) in ((0, 2 * v + 1, "N", N - 1), (28, 2 * v + 1, "S", 0)):
            ch = g[r][c]
            if ch in ("D", "~"):
                p = (56.0 if side == "N" else 0.0, 4 * v + 2)
                outer.append((p, ch, side, (u, v)))
    for u in range(N):
        for (c, side, v) in ((0, "W", 0), (28, "E", N - 1)):
            ch = g[27 - 2 * u][c]
            if ch in ("D", "~"):
                p = (4 * u + 2, 0.0 if side == "W" else 56.0)
                outer.append((p, ch, side, (u, v)))
    all_open_pts = [o[0] for o in openings] + [o[0] for o in outer]

    def reach(starts):
        seen = set(starts)
        q = deque(starts)
        while q:
            a, b = q.popleft()
            for du, dv in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                na, nb = a + du, b + dv
                if 0 <= na < N and 0 <= nb < N and (na, nb) not in seen and edge(g, a, b, du, dv) in PASS:
                    seen.add((na, nb))
                    q.append((na, nb))
        return seen

    def bfs_dist(start):
        dist = {start: 0}
        q = deque([start])
        while q:
            a, b = q.popleft()
            for du, dv in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                na, nb = a + du, b + dv
                if 0 <= na < N and 0 <= nb < N and (na, nb) not in dist and edge(g, a, b, du, dv) in PASS:
                    dist[(na, nb)] = dist[(a, b)] + 1
                    q.append((na, nb))
        return dist

    print(f"\n=== {name}  (world x {x0}..{x0 + 56}, y {y0}..{y0 + 56})")
    print("rooms:", ", ".join(f"{label[r]}={len(c)}칸/{len(c) * 16:.0f}m²" for r, c in sorted(rooms.items(), key=lambda t: label[t[1] and t[0]])))
    for p, ch, side, uv in outer:
        w = world(*p)
        print(f"outer {'door' if ch == 'D' else 'rubble'} {side} cell{uv} -> world ({w[0]:.0f}, {w[1]:.0f})")

    # every cell reachable from the outer openings, and with any single outer opening blocked
    allc = {(u, v) for u in range(N) for v in range(N)}
    if reach([o[3] for o in outer]) != allc:
        fails.append("some cells unreachable from outside")
    for i, o in enumerate(outer):
        rest = [x[3] for j, x in enumerate(outer) if j != i]
        if reach(rest) != allc:
            fails.append(f"blocking outer {o[2]} {o[3]} isolates cells")

    # inner room covers the centre 4 cells
    centre = {(6, 6), (6, 7), (7, 6), (7, 7)}
    inner_r = room[(6, 6)]
    if any(room[c] != inner_r for c in centre):
        fails.append("centre cells are not one room")
    print(f"inner room = {label[inner_r]} ({len(rooms[inner_r])} cells)")

    # chests
    chests = [(u, v) for u in range(N) for v in range(N) if cell(g, u, v) == "c"]
    first_rooms = {room[o[3]] for o in outer if o[1] == "D"}
    if len(chests) != 4:
        fails.append(f"{len(chests)} chests (need 4)")
    has_first, has_inner = False, False
    for u, v in chests:
        lp = (4 * u + 2, 4 * v + 2)
        depth = min(lp[0], 56 - lp[0], lp[1], 56 - lp[1])
        dmin = min(math.dist(lp, q) for q in all_open_pts)
        r = room[(u, v)]
        tag = []
        if r == inner_r:
            has_inner = True
            tag.append("inner")
            if depth < 24:
                fails.append(f"inner chest ({u},{v}) depth {depth} < 24")
            if max(abs(lp[0] - 28), abs(lp[1] - 28)) > 4:
                fails.append(f"inner chest ({u},{v}) more than 4 m from centre")
        if r in first_rooms:
            has_first = True
            tag.append("first room")
        if dmin < 3:
            fails.append(f"chest ({u},{v}) {dmin:.1f} m from an opening")
        w = world(*lp)
        print(f"chest cell ({u},{v}) room {label[r]:<2} world ({w[0]:.0f}, {w[1]:.0f})  depth {depth:.0f} m  "
              f"nearest opening {dmin:.1f} m  {' '.join(tag)}")
    if not has_inner:
        fails.append("no chest in the inner room")
    if not has_first:
        fails.append("no chest in a first room")

    # shortest walk from each outer door to the inner chest
    inner_chest = next(((u, v) for u, v in chests if room[(u, v)] == inner_r), None)
    if inner_chest:
        for p, ch, side, uv in outer:
            d = bfs_dist(uv).get(inner_chest)
            print(f"walk {side} {'door' if ch == 'D' else 'rubble'} -> inner chest: {d} cells ~ {d * CELL:.0f} m")

    # module counts
    walls = doors = rubble = 0
    for r in range(29):
        for c in range(29):
            ch = g[r][c]
            if (r % 2 == 0) != (c % 2 == 0):      # edge position
                walls += ch in "-|"
                doors += ch == "D"
                rubble += ch == "~"
    pillars = sum(row.count("+") for row in g)
    free = sum(row.count("o") for row in g)
    rooms_n = sum(1 for r in rooms if label[r] != ".")
    corridor = sum(len(c) for r, c in rooms.items() if label[r] == ".")
    print(f"modules: wall {walls}  door frame {doors}  rubble {rubble}  pillar {pillars} (+{free} free)  floor {N * N}")
    print(f"rooms (non-corridor) {rooms_n}  corridor cells {corridor}")
    print("RESULT:", "PASS" if not fails else "FAIL")
    for f in fails:
        print("  -", f)
    return not fails


if __name__ == "__main__":
    a = sys.argv[1:]
    ok = all(analyse(a[i], float(a[i + 1]), float(a[i + 2])) for i in range(0, len(a), 3))
    sys.exit(0 if ok else 1)
