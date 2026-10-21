"""Tables and INSERT columns the server code refers to (verify-server.ps1 DB stage, 2026-10-21).

  python -I Tools/verify/check_sql_refs.py <repo> <out.json> [<schema.json>]

Scans string literals in Server/GameServer · Server/ServerCore (C++) and Server/AuthServer (C#) for SQL:
  FROM / INTO / UPDATE / JOIN <table>, "<table> SET" (a table that follows a concatenated "UPDATE"), and INSERT ... INTO t (c1, c2 ...).
Keywords must be upper case - log messages ("item from bag") are not SQL.
With <schema.json> ({"table": ["col", ...]} from information_schema) every table and INSERT column must exist - exit 1 otherwise.
"""
import json, os, re, sys

repo, out = sys.argv[1], sys.argv[2]
schema = json.load(open(sys.argv[3], encoding='utf-8')) if len(sys.argv) > 3 else None

LIT_CPP = re.compile(r'"((?:[^"\\\n]|\\.)*)"')
LIT_CS = re.compile(r'@?\$?"((?:[^"\\\n]|\\.|"")*)"')
ADJ = re.compile(r'"\s*\n?\s*"')       # "a" "b" -> one literal (C++ adjacent literal concatenation)
TABLE = re.compile(r'\b(?:FROM|INTO|UPDATE(?:\s+IGNORE)?|JOIN|TABLE(?:\s+IF\s+NOT\s+EXISTS)?)\s+`?([a-z_][a-z0-9_]*)`?')
SET_LEAD = re.compile(r'^\s*([a-z_][a-z0-9_]*)\s+SET\b')
INSERT = re.compile(r'\bINSERT(?:\s+IGNORE)?\s+INTO\s+`?([a-z_][a-z0-9_]*)`?\s*\(([^)]*)\)')
NOT_TABLES = {'information_schema', 'dual', 'select', 'values'}

refs, inserts = {}, {}

def scan(path, rx):
    text = open(path, encoding='utf-8', errors='replace').read()
    if rx is LIT_CPP:
        text = ADJ.sub('', text)
    for m in rx.finditer(text):
        lit = m.group(1)
        line = text.count('\n', 0, m.start()) + 1
        where = f'{os.path.relpath(path, repo).replace(os.sep, "/")}:{line}'
        names = [t for t in TABLE.findall(lit)]
        lead = SET_LEAD.match(lit)
        if lead:
            names.append(lead.group(1))
        for t in names:
            if t in NOT_TABLES:
                continue
            refs.setdefault(t, []).append(where)
        for t, cols in INSERT.findall(lit):
            for c in cols.split(','):
                c = c.strip().strip('`')
                if re.fullmatch(r'[a-z_][a-z0-9_]*', c):
                    inserts.setdefault(t, {}).setdefault(c, []).append(where)

for sub, exts, rx in (('Server/GameServer', ('.cpp', '.h'), LIT_CPP), ('Server/ServerCore', ('.cpp', '.h'), LIT_CPP), ('Server/AuthServer', ('.cs',), LIT_CS)):
    root = os.path.join(repo, sub)
    for d, dirs, files in os.walk(root):
        dirs[:] = [x for x in dirs if x not in ('bin', 'obj', 'x64')]
        for f in files:
            if f.endswith(exts) and '.pb.' not in f:
                scan(os.path.join(d, f), rx)

errors = []
if schema is not None:
    for t, where in sorted(refs.items()):
        if t not in schema:
            errors.append(f'table `{t}` is used by the code but not in the fresh schema - {where[0]}' + (f' (+{len(where) - 1})' if len(where) > 1 else ''))
    for t, cols in sorted(inserts.items()):
        for c, where in sorted(cols.items()):
            if t in schema and c not in schema[t]:
                errors.append(f'column `{t}.{c}` is inserted by the code but not in the fresh schema - {where[0]}')

json.dump({'tables': {t: len(w) for t, w in refs.items()}, 'insert_columns': {t: sorted(c) for t, c in inserts.items()}, 'errors': errors},
          open(out, 'w', encoding='utf-8'), ensure_ascii=False, indent=1)
print(f'[SQL] {len(refs)} tables, {sum(len(c) for c in inserts.values())} INSERT columns referenced - {len(errors)} error(s)')
for e in errors:
    print('  ' + e)
sys.exit(1 if errors else 0)
