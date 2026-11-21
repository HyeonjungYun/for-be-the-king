"""Summarize one run-scale.ps1 report folder -> summary.json + summary.md (2026-11-21, docs/server-scaling-10k.md).

Steady state = rows after every bot process reported in_game >= 95 % of its target (or the last half of the run if never).
Server numbers come from metrics/stats-*.csv (one row per 10 s), bot numbers from bots-*.log [SCALE] lines,
process CPU/memory from samples.csv.
usage: python summarize.py <report dir>
"""
import csv
import glob
import json
import os
import re
import sys

SCALE_RE = re.compile(
    r'\[SCALE\] t=(\d+)s bots=(\d+)/(\d+) login=(\d+) in_game=(\d+) pending=(\d+) \| rtt n=(\d+) p50=([\d.]+)ms p95=([\d.]+)ms '
    r'p99=([\d.]+)ms max=([\d.]+)ms \| enter=([\d.]+)/s reject=([\d.]+)/s death=([\d.]+)/s login_fail=(\d+) '
    r'kick\(full=(\d+) cheat=(\d+) ban=(\d+) dup=(\d+) shut=(\d+)\) \| move=([\d.]+)/s atk=([\d.]+)/s skill=([\d.]+)/s \| tick_max=([\d.]+)ms late=([\d.]+)ms')


def num(v):
    try:
        return float(v)
    except (TypeError, ValueError):
        return None


def pct(values, p):
    vals = sorted(v for v in values if v is not None)
    if not vals:
        return None
    k = min(len(vals) - 1, max(0, int(round(p * (len(vals) - 1)))))
    return vals[k]


def main(folder):
    bots = {}
    for path in sorted(glob.glob(os.path.join(folder, 'bots-*.log'))):
        rows = []
        for line in open(path, encoding='utf-8', errors='replace'):
            m = SCALE_RE.search(line)
            if not m:
                continue
            g = m.groups()
            rows.append({'t': int(g[0]), 'connected': int(g[1]), 'target': int(g[2]), 'login': int(g[3]), 'in_game': int(g[4]),
                         'rtt_n': int(g[6]), 'rtt_p50': float(g[7]), 'rtt_p95': float(g[8]), 'rtt_p99': float(g[9]), 'rtt_max': float(g[10]),
                         'enter_s': float(g[11]), 'reject_s': float(g[12]), 'death_s': float(g[13]), 'login_fail': int(g[14]),
                         'kick_full': int(g[15]), 'kick_cheat': int(g[16]), 'kick_ban': int(g[17]), 'kick_dup': int(g[18]),
                         'move_s': float(g[20]), 'atk_s': float(g[21]), 'skill_s': float(g[22]), 'tick_max': float(g[23]), 'late': float(g[24])})
        bots[os.path.basename(path)] = rows

    # merge bot processes by report index (each process reports every 10 s)
    merged = []
    n = min((len(r) for r in bots.values()), default=0)
    for i in range(n):
        rs = [r[i] for r in bots.values()]
        merged.append({
            't': max(x['t'] for x in rs),
            'connected': sum(x['connected'] for x in rs), 'target': sum(x['target'] for x in rs),
            'in_game': sum(x['in_game'] for x in rs), 'login': sum(x['login'] for x in rs),
            'rtt_p50': max(x['rtt_p50'] for x in rs), 'rtt_p95': max(x['rtt_p95'] for x in rs),
            'rtt_p99': max(x['rtt_p99'] for x in rs), 'rtt_max': max(x['rtt_max'] for x in rs),
            'enter_s': sum(x['enter_s'] for x in rs), 'reject_s': sum(x['reject_s'] for x in rs), 'death_s': sum(x['death_s'] for x in rs),
            'kick_cheat': sum(x['kick_cheat'] for x in rs), 'kick_full': sum(x['kick_full'] for x in rs), 'kick_dup': sum(x['kick_dup'] for x in rs),
            'kick_ban': sum(x['kick_ban'] for x in rs),
            'login_fail': sum(x['login_fail'] for x in rs),
            'move_s': sum(x['move_s'] for x in rs), 'atk_s': sum(x['atk_s'] for x in rs), 'skill_s': sum(x['skill_s'] for x in rs),
            'bot_tick_max': max(x['tick_max'] for x in rs), 'bot_late': max(x['late'] for x in rs),
        })

    target = merged[0]['target'] if merged else 0
    steady = [r for r in merged if target and r['in_game'] >= 0.95 * target]
    if not steady:
        steady = merged[len(merged) // 2:]

    # one stats CSV per game server (step 2 runs several: <folder>/server-N/metrics) - steady rows per server, then pooled
    server = []
    server_steady = []
    peak_sessions = 0
    for path in glob.glob(os.path.join(folder, 'metrics', 'stats-*.csv')) + glob.glob(os.path.join(folder, 'server-*', 'metrics', 'stats-*.csv')):
        with open(path, encoding='utf-8', errors='replace') as f:
            rows = list(csv.DictReader(f))
        server += rows
        peak = max((int(r['sessions']) for r in rows), default=0)
        peak_sessions += peak
        server_steady += [r for r in rows if peak and int(r['sessions']) >= 0.95 * peak] or rows[len(rows) // 2:]

    samples = []
    sp = os.path.join(folder, 'samples.csv')
    if os.path.exists(sp):
        with open(sp, encoding='utf-8-sig') as f:
            samples = list(csv.DictReader(f))

    def col(rows, key):
        return [num(r.get(key)) for r in rows]

    out = {
        'folder': folder,
        'bots_target': target,
        'bots_in_game_peak': max((r['in_game'] for r in merged), default=0),
        'bots_connected_peak': max((r['connected'] for r in merged), default=0),
        'steady_windows': len(steady),
        'bot_rtt_p50_ms_median': pct([r['rtt_p50'] for r in steady], 0.5),
        'bot_rtt_p95_ms_worst': max((r['rtt_p95'] for r in steady), default=None),
        'bot_rtt_p99_ms_worst': max((r['rtt_p99'] for r in steady), default=None),
        'bot_rtt_p99_ms_median': pct([r['rtt_p99'] for r in steady], 0.5),
        'bot_rtt_max_ms': max((r['rtt_max'] for r in steady), default=None),
        'enter_per_s': pct([r['enter_s'] for r in steady], 0.5),
        'death_per_s': pct([r['death_s'] for r in steady], 0.5),
        'moves_per_s': pct([r['move_s'] for r in steady], 0.5),
        'attacks_per_s': pct([r['atk_s'] for r in steady], 0.5),
        'skills_per_s': pct([r['skill_s'] for r in steady], 0.5),
        'kicks_cheat': merged[-1]['kick_cheat'] if merged else None,
        'kicks_full': merged[-1]['kick_full'] if merged else None,
        'kicks_ban': merged[-1]['kick_ban'] if merged else None,
        'login_fail': merged[-1]['login_fail'] if merged else None,
        'bot_tick_max_ms': max((r['bot_tick_max'] for r in steady), default=None),
        'server_sessions_peak': peak_sessions,
        'server_flush_p95_us_worst': max((v for v in col(server_steady, 'flush_p95_us') if v is not None), default=None),
        'server_flush_p99_us_worst': max((v for v in col(server_steady, 'flush_p99_us') if v is not None), default=None),
        'server_flush_max_us_worst': max((v for v in col(server_steady, 'flush_max_us') if v is not None), default=None),
        'server_jobwait_p99_us_worst': max((v for v in col(server_steady, 'jobwait_p99_us') if v is not None), default=None),
        'server_jobwait_max_us_worst': max((v for v in col(server_steady, 'jobwait_max_us') if v is not None), default=None),
        'server_rtt_p99_us_worst': max((v for v in col(server_steady, 'rtt_p99_us') if v is not None), default=None),
        'server_sent_mbps_median': pct(col(server_steady, 'sent_mbps'), 0.5),
        'server_recv_mbps_median': pct(col(server_steady, 'recv_mbps'), 0.5),
        'server_commit_mb_peak': max((v for v in col(server, 'commit_mb') if v is not None), default=None),
        'server_db_pending_peak': max((v for v in col(server, 'db_pending') if v is not None), default=None),
        'server_cpu_pct_median': pct([sum(num(r.get(k)) or 0 for k in r if k.startswith('server') and k.endswith('_cpu_pct')) for r in samples[len(samples) // 2:]], 0.5),
        'server_mem_mb_peak': max((sum(num(r.get(k)) or 0 for k in r if k.startswith('server') and k.endswith('_mem_mb')) for r in samples), default=None),
        'servers': len(glob.glob(os.path.join(folder, 'server-*'))) or 1,
        'bots_cpu_pct_median_total': pct([sum(num(r.get(k)) or 0 for k in r if k.startswith('bots') and k.endswith('_cpu_pct')) for r in samples[len(samples) // 2:]], 0.5),
        'bots_mem_mb_peak_total': max((sum(num(r.get(k)) or 0 for k in r if k.startswith('bots') and k.endswith('_mem_mb')) for r in samples), default=None),
        'free_mem_mb_min': min((v for v in col(samples, 'free_mem_mb') if v is not None), default=None),
    }

    # server log signals
    logs = [p for p in [os.path.join(folder, 'server.log')] + glob.glob(os.path.join(folder, 'server-*', 'server.log')) if os.path.exists(p)]
    if logs:
        text = ''.join(open(p, encoding='utf-8', errors='replace').read() for p in logs)
        out['server_slow_flush_lines'] = text.count('[SLOW FLUSH]')
        out['server_log_dropped'] = len(re.findall(r'\[LOG\] dropped', text))
        out['server_cheat_kicks'] = len(re.findall(r'\[CHEAT\] kick', text))
        out['server_full_kicks'] = text.count('server full')
        out['server_log_mb'] = round(len(text.encode('utf-8')) / 1e6, 1)
    out['server_crash_dumps'] = len(glob.glob(os.path.join(folder, 'crash', '*.dmp')) + glob.glob(os.path.join(folder, 'server-*', 'crash', '*.dmp')))

    json.dump(out, open(os.path.join(folder, 'summary.json'), 'w', encoding='utf-8'), indent=1)
    lines = ['# load test summary', '', f'`{folder}`', '', '| key | value |', '|---|---|']
    lines += [f'| {k} | {v} |' for k, v in out.items() if k != 'folder']
    lines += ['', '## merged bot windows', '', '| t | connected | in_game | rtt p50 | p95 | p99 | max | enter/s | death/s | move/s |', '|---|---|---|---|---|---|---|---|---|---|']
    lines += [f"| {r['t']} | {r['connected']} | {r['in_game']} | {r['rtt_p50']} | {r['rtt_p95']} | {r['rtt_p99']} | {r['rtt_max']} | {r['enter_s']:.1f} | {r['death_s']:.1f} | {r['move_s']:.0f} |" for r in merged]
    open(os.path.join(folder, 'summary.md'), 'w', encoding='utf-8').write('\n'.join(lines) + '\n')
    for k, v in out.items():
        print(f'{k:32} {v}')


if __name__ == '__main__':
    main(sys.argv[1])
