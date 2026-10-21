"""Static text checks for verify-server.ps1 (2026-10-21).

  python -I Tools/verify/check_text.py <repo> <out.json>

Rules (each one came from a real accident - see memory / tech-debt notes):
  1. Server/Data/*.csv               one line-ending style per file, valid UTF-8 (a half-converted file broke 16 CSVs on 2026-10-12;
                                     both styles parse - CsvTable strips the trailing CR)
  2. Server C++ / C# / Tools scripts  no mixed line endings in one file, valid UTF-8 (cp949 .bat files are skipped)
  3. Tools/**/*.ps1 with non-ASCII    must start with a UTF-8 BOM (PowerShell 5.1 reads BOM-less files as cp949 and breaks Korean)
Exit code 0 = no errors. Warnings do not fail.
"""
import json, os, sys

repo, out = sys.argv[1], sys.argv[2]
errors, warnings, scanned = [], [], 0

SKIP_DIRS = {'.git', 'Binaries', 'Libraries', 'x64', 'Intermediate', 'obj', 'bin', 'packages', 'Logs', '__pycache__', 'coverage', 'reports', 'demo'}

def walk(root, exts):
    for d, dirs, files in os.walk(root):
        dirs[:] = [x for x in dirs if x not in SKIP_DIRS]
        for f in files:
            if f.lower().endswith(exts):
                yield os.path.join(d, f)

def rel(p):
    return os.path.relpath(p, repo).replace('\\', '/')

def endings(b):
    crlf = b.count(b'\r\n')
    lf = b.count(b'\n') - crlf
    return crlf, lf

def utf8_ok(b):
    try:
        b.decode('utf-8')
        return True
    except UnicodeDecodeError as e:
        return e

# 1. data CSV
for p in walk(os.path.join(repo, 'Server', 'Data'), ('.csv',)):
    scanned += 1
    b = open(p, 'rb').read()
    crlf, lf = endings(b)
    if crlf and lf:
        errors.append(f'{rel(p)}: mixed line endings (CRLF {crlf} / LF {lf}) - a half-converted data file')
    ok = utf8_ok(b)
    if ok is not True:
        errors.append(f'{rel(p)}: not UTF-8 ({ok})')

# 2. sources and scripts
SRC = [(os.path.join(repo, 'Server'), ('.cpp', '.h', '.cs', '.ps1', '.sql')),
       (os.path.join(repo, 'Tools'), ('.ps1', '.py', '.yml', '.sql', '.cs', '.xaml', '.csproj', '.md'))]
for root, exts in SRC:
    for p in walk(root, exts):
        name = os.path.basename(p)
        if '.pb.' in name:
            continue
        scanned += 1
        b = open(p, 'rb').read()
        crlf, lf = endings(b)
        if crlf and lf:
            errors.append(f'{rel(p)}: mixed line endings (CRLF {crlf} / LF {lf})')
        ok = utf8_ok(b)
        if ok is not True:
            # SQL 의 cp949 한글 주석은 mysql 이 그대로 지나친다 - 경고만 (seed_dev.sql)
            (warnings if p.lower().endswith('.sql') else errors).append(f'{rel(p)}: not UTF-8 ({ok})')
            continue
        if p.lower().endswith('.ps1') and os.sep + 'Tools' + os.sep in p:
            if any(c > 0x7F for c in b) and not b.startswith(b'\xef\xbb\xbf'):
                errors.append(f'{rel(p)}: non-ASCII PowerShell script without a UTF-8 BOM - PowerShell 5.1 reads it as cp949')

json.dump({'scanned': scanned, 'errors': errors, 'warnings': warnings}, open(out, 'w', encoding='utf-8'), ensure_ascii=False, indent=1)
print(f'[TEXT] scanned {scanned} files - {len(errors)} error(s)')
for e in errors[:40]:
    print('  ' + e)
for w in warnings[:20]:
    print('  (warn) ' + w)
sys.exit(1 if errors else 0)
