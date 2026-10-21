"""Promote one floor's level export into the live data the server and client read (ADR-0006 Migration 4).

The graybox commandlet exports a floor to Server/Data/export/floor<N>/ — a staging folder nothing reads.
Server/Data/*.csv hold every floor. This replaces floor N's rows in each live file with the export's rows and keeps
the other floors' rows as they are; a file the live data doesn't have yet is copied whole.

    python Tools/level/promote_export.py --floor 0           # write
    python Tools/level/promote_export.py --floor 0 --check   # exit 1 if the live data is not what promotion would write

Run it after re-exporting a level, then restart the server — WallTable checks level_versions.csv against walls.csv.
"""

import argparse
import csv
import io
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
LIVE = ROOT / "Server" / "Data"

# Every exported file carries floor_id, so rows are replaced per floor
FILES = [
    "walls.csv",
    "level_versions.csv",
    "entry_points.csv",
    "chest_spawns.csv",
    "towers.csv",
    "houses.csv",
    "monster_spots.csv",
    "floor_outline.csv",   # the island coast (2026-10-11, ADR-0013) — a floor without one exports only the header
]


def read_rows(path):
    text = path.read_text(encoding="utf-8-sig")
    rows = list(csv.reader(io.StringIO(text)))
    rows = [r for r in rows if r]
    if not rows:
        raise SystemExit(f"{path}: empty")
    return rows[0], rows[1:]


def merged(name, floor, export_dir):
    """The live file's text after replacing floor's rows; None if the export has no such file."""
    staged = export_dir / name
    if not staged.exists():
        return None
    header, new_rows = read_rows(staged)
    if "floor_id" not in header:
        raise SystemExit(f"{staged}: no floor_id column")
    col = header.index("floor_id")
    for r in new_rows:
        if r[col] != str(floor):
            raise SystemExit(f"{staged}: a row is on floor {r[col]}, export is for floor {floor}")

    live = LIVE / name
    if live.exists():
        live_header, live_rows = read_rows(live)
        if live_header != header:
            raise SystemExit(f"{name}: live header {live_header} != export header {header}")
        before = [r for r in live_rows if int(r[col]) < floor]
        after = [r for r in live_rows if int(r[col]) > floor]
        rows = before + new_rows + after
    else:
        rows = new_rows

    out = io.StringIO()
    writer = csv.writer(out, lineterminator="\n")
    writer.writerow(header)
    writer.writerows(rows)
    return out.getvalue()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--floor", type=int, required=True)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()

    export_dir = LIVE / "export" / f"floor{args.floor}"
    if not export_dir.is_dir():
        raise SystemExit(f"{export_dir}: no export for floor {args.floor}")

    stale = 0
    for name in FILES:
        text = merged(name, args.floor, export_dir)
        if text is None:
            print(f"  {name}: not in the export, skipped")
            continue
        live = LIVE / name
        current = live.read_text(encoding="utf-8-sig") if live.exists() else None
        if current == text:
            print(f"  {name}: up to date")
            continue
        stale += 1
        if args.check:
            print(f"  {name}: differs from the export")
        else:
            live.write_text(text, encoding="utf-8", newline="")
            print(f"  {name}: floor {args.floor} rows replaced ({text.count(chr(10)) - 1} rows in file)")

    if args.check and stale:
        print(f"{stale} file(s) out of date — run without --check")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
