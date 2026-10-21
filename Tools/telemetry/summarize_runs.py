"""Telemetry summary for runs-*.csv (design/gdd/escape-timer.md § G · F8).

    python Tools/telemetry/summarize_runs.py Server/Logs/telemetry/runs-20261007-190000.csv [more.csv ...]
    python Tools/telemetry/summarize_runs.py --selftest

Prints:
  H_all   hesitation rate — runs of at least min_run with a voluntary zone exit / runs of at least min_run  (gate >= 0.30)
  H_reach the same over runs that reached a zone (diagnostic)
  R       risk-taking — deaths carrying more than that character's other runs' mean / deaths whose character
          has at least 3 other runs  (gate >= 0.40)
  C       camping — total alert-radius dwell / total run time  (alarm > 0.25 → shrink R_alert)

Only the standard library — the server writes plain CSV without quoting (Telemetry::FormatRun).
"""

import argparse
import csv
import io
import sys

MIN_RUN_MS = 60_000      # escape_config telemetry_min_run_s
DEATHS = {"DIED", "DISCONNECT_DEATH"}
COLUMNS = 22


def load(paths):
    rows = []
    for path in paths:
        with open(path, newline="", encoding="utf-8") as f:
            rows.extend(read(f))
    return rows


def read(stream):
    reader = csv.DictReader(stream)
    if reader.fieldnames is None or len(reader.fieldnames) != COLUMNS:
        raise SystemExit(f"expected {COLUMNS} columns, got {reader.fieldnames}")
    out = []
    for r in reader:
        for key in ("run_ms", "zone_visits", "zone_leaves_voluntary", "end_carried_sum", "alert_dwell_ms"):
            r[key] = int(r[key])
        out.append(r)
    return out


def summarize(rows, min_run_ms=MIN_RUN_MS):
    long_runs = [r for r in rows if r["run_ms"] >= min_run_ms]
    hesitant = [r for r in long_runs if r["zone_leaves_voluntary"] >= 1]
    reached = [r for r in long_runs if r["zone_visits"] >= 1]

    h_all = len(hesitant) / len(long_runs) if long_runs else None
    h_reach = len(hesitant) / len(reached) if reached else None

    by_char = {}
    for r in rows:
        by_char.setdefault(r["char_id"], []).append(r)

    flags = 0
    eligible = 0
    for r in rows:
        if r["reason"] not in DEATHS:
            continue
        others = [o for o in by_char[r["char_id"]] if o is not r]
        if len(others) < 3:
            continue
        eligible += 1
        mean = sum(o["end_carried_sum"] for o in others) / len(others)
        if r["end_carried_sum"] > mean:
            flags += 1
    risk = flags / eligible if eligible else None

    total_run = sum(r["run_ms"] for r in rows)
    camping = sum(r["alert_dwell_ms"] for r in rows) / total_run if total_run else None

    return {
        "runs": len(rows), "runs_min": len(long_runs), "reached": len(reached), "hesitant": len(hesitant),
        "H_all": h_all, "H_reach": h_reach, "risk_eligible": eligible, "risk_flags": flags, "R": risk, "C": camping,
    }


def fmt(v):
    return "n/a" if v is None else f"{v * 100:.1f} %"


def report(s):
    print(f"runs {s['runs']} (>= min_run {s['runs_min']}) · reached a zone {s['reached']} · voluntary exit {s['hesitant']}")
    print(f"H_all   {fmt(s['H_all'])}   gate >= 30 %  {'PASS' if s['H_all'] is not None and s['H_all'] >= 0.30 else 'FAIL / n/a'}")
    print(f"H_reach {fmt(s['H_reach'])}   (diagnostic)")
    print(f"R       {fmt(s['R'])}   ({s['risk_flags']}/{s['risk_eligible']} deaths)  gate >= 40 %  {'PASS' if s['R'] is not None and s['R'] >= 0.40 else 'FAIL / n/a'}")
    print(f"C       {fmt(s['C'])}   alarm > 25 %  {'ALARM' if s['C'] is not None and s['C'] > 0.25 else 'ok'}")


HEADER = ("ts_unix_ms,boot_id,run_seq,char_id,floor_id,entry_point,reason,run_ms,brought_in_sum,end_carried_sum,end_carried_count,"
          "end_carried_max_grade,run_gold,gold_kept,zone_visits,zone_leaves_voluntary,zone_leaves_forced,zone_dwell_ms,"
          "gauge_reversals_present,alert_dwell_ms,last_tower_id,killer_id")


def row(seq, char, reason, run_ms, carried, visits, leaves, alert_ms=0):
    return f"0,1,{seq},{char},0,1,{reason},{run_ms},0,{carried},0,0,0,0,{visits},{leaves},0,0,0,{alert_ms},Ta,0"


def selftest():
    """AC-ET-17 golden fixture: 40 runs ≥ min_run, 22 reached a zone, 14 voluntary exits → H_all 35.0 % · H_reach 63.6 %.
    Character 9 ends [2, 2, 3, 8, 1] with the 8 a death → flag 1. Runs under min_run are left out of the denominator."""
    lines = [HEADER]
    seq = 0
    for i in range(40):
        seq += 1
        visits = 1 if i < 22 else 0
        leaves = 1 if i < 14 else 0
        lines.append(row(seq, 100 + i, "ESCAPED", 120_000, 1, visits, leaves))
    for i in range(5):                         # short test runs - excluded
        seq += 1
        lines.append(row(seq, 500 + i, "ESCAPED", 10_000, 1, 1, 1))
    for carried, reason in ((2, "ESCAPED"), (2, "ESCAPED"), (3, "EXPIRED"), (8, "DIED"), (1, "ESCAPED")):
        seq += 1
        lines.append(row(seq, 9, reason, 30_000, carried, 0, 0))

    s = summarize(read(io.StringIO("\n".join(lines) + "\n")))
    ok = True

    def check(name, got, want):
        nonlocal ok
        good = abs(got - want) < 1e-3
        ok = ok and good
        print(f"  {'ok ' if good else 'BAD'} {name} = {got:.4f} (want {want:.4f})")

    print("selftest (AC-ET-17)")
    check("H_all", s["H_all"], 14 / 40)
    check("H_reach", s["H_reach"], 14 / 22)
    check("R", s["R"], 1.0)
    check("risk eligible", s["risk_eligible"], 1)
    return 0 if ok else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("files", nargs="*")
    parser.add_argument("--min-run-s", type=int, default=MIN_RUN_MS // 1000)
    parser.add_argument("--selftest", action="store_true")
    args = parser.parse_args()

    if args.selftest:
        return selftest()
    if not args.files:
        parser.error("give runs-*.csv files or --selftest")

    report(summarize(load(args.files), args.min_run_s * 1000))
    return 0


if __name__ == "__main__":
    sys.exit(main())
