"""Admin port probe (verify-server.ps1 smoke stage, 2026-10-21) - talks to GameServer exactly like ServerMonitor does.

  python -I Tools/verify/admin_probe.py <admin.json> <out.json> [--kick] [--notice]

Checks: wrong token is refused · hello · events (backlog + live) · players · rooms · player detail · map (5 Hz) · metrics ·
optional notice (to everyone) and kick (one bot). Exit 0 when every check passed.
"""
import json, socket, sys, time

info_path, out_path = sys.argv[1], sys.argv[2]
do_kick = '--kick' in sys.argv
do_notice = '--notice' in sys.argv
info = json.load(open(info_path, encoding='utf-8'))
port, token = info['port'], info['token']
checks, errors = {}, []


class Conn:
    def __init__(self):
        self.s = socket.create_connection(('127.0.0.1', port), timeout=5)
        self.buf = b''

    def send(self, obj):
        self.s.sendall((json.dumps(obj, ensure_ascii=False) + '\n').encode('utf-8'))

    def lines(self, seconds):
        end = time.time() + seconds
        self.s.settimeout(0.2)
        while time.time() < end:
            try:
                chunk = self.s.recv(1 << 16)
                if not chunk:
                    return
                self.buf += chunk
            except socket.timeout:
                pass
            while b'\n' in self.buf:
                line, self.buf = self.buf.split(b'\n', 1)
                if line.strip():
                    yield json.loads(line.decode('utf-8'))

    def wait(self, t, seconds=5):
        for m in self.lines(seconds):
            if m.get('t') == t:
                return m
        return None


def check(name, ok, detail=''):
    checks[name] = {'ok': bool(ok), 'detail': detail}
    if not ok:
        errors.append(f'{name}: {detail}')

# 1. wrong token
try:
    bad = Conn()
    bad.send({'op': 'hello', 'token': 'nope', 'client': 'probe'})
    m = bad.wait('error', 3)
    check('wrong_token_refused', m is not None and m.get('msg') == 'unauthorized', str(m))
except Exception as e:
    check('wrong_token_refused', False, repr(e))

c = Conn()
c.send({'op': 'hello', 'token': token, 'client': 'admin_probe'})
hello = c.wait('hello')
check('hello', hello is not None and hello.get('ok'), str(hello)[:200])

c.send({'op': 'events', 'backlog': 500})
c.send({'op': 'players'})
c.send({'op': 'rooms'})
c.send({'op': 'metrics'})
events, players, rooms, metrics = [], None, None, None
for m in c.lines(4):
    t = m.get('t')
    if t == 'ev':
        events.append(m)
    elif t == 'players':
        players = m['list']
    elif t == 'rooms':
        rooms = m['list']
    elif t == 'metrics':
        metrics = m['m']
tags = sorted({e['tag'] for e in events})
check('events', len(events) > 0, f'{len(events)} events, tags={tags}')
check('players', players is not None and len(players) > 0, f'{0 if players is None else len(players)} players')
check('rooms', rooms is not None and len(rooms) > 0, str(rooms)[:300])
check('metrics', metrics is not None and 'flush_p95_us' in metrics, str(metrics)[:200])

in_room = [p for p in (players or []) if p.get('room')]
target = in_room[0] if in_room else None
if target:
    c.send({'op': 'player', 'id': target['id']})
    p = c.wait('player')
    check('player_detail', p is not None and p.get('p') and p['p'].get('id') == target['id'], str(p)[:300])
    room = target['room']
    c.send({'op': 'map', 'room': room})
    maps = []
    for m in c.lines(2.2):
        if m.get('t') == 'map':
            maps.append(m)
    kinds = sorted({o['k'] for mp in maps for o in mp.get('objs', [])})
    routes = sum(1 for mp in maps if 'routes' in mp)
    check('map', len(maps) >= 6 and 'p' in kinds, f'{len(maps)} snapshots in 2.2 s, kinds={kinds}, with routes={routes}')
    c.send({'op': 'map', 'room': ''})
    actor_events = [e for e in events if target['id'] in e.get('a', []) or target['id'] in e.get('tg', [])]
    check('player_events', len(actor_events) > 0, f'{len(actor_events)} events mention char {target["id"]}')
else:
    check('player_detail', False, 'no player in a room')

if do_notice:
    c.send({'op': 'notice', 'text': '[검증] 관리 포트 공지 시험'})
    r = c.wait('result')
    check('notice', r is not None and r.get('ok'), str(r))
if do_kick and target:
    c.send({'op': 'kick', 'id': target['id'], 'reason': 'admin_probe'})
    r = c.wait('result')
    check('kick', r is not None and r.get('ok'), str(r))
    seen = False
    for m in c.lines(3):
        if m.get('t') == 'ev' and 'kick' in m.get('msg', '') and m.get('tag') == 'system':
            seen = True
            break
    check('kick_event', seen, 'an [ADMIN] kick event came back on the stream')

json.dump({'checks': checks, 'errors': errors, 'events': len(events)}, open(out_path, 'w', encoding='utf-8'), ensure_ascii=False, indent=1)
print(f'[ADMIN PROBE] {sum(1 for v in checks.values() if v["ok"])}/{len(checks)} checks passed')
for e in errors:
    print('  ' + e)
sys.exit(1 if errors else 0)
