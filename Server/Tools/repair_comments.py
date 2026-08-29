"""
인코딩 사고로 깨진 주석(? 또는 U+FFFD)을 참조 원본에서 복구한다.

매칭 방법:
    깨진 줄과 참조 줄에서 "비ASCII·? 를 전부 제거한 뼈대"를 만들어 비교한다.
    뼈대가 유일하게 일치할 때만 복구한다. 애매하면 건너뛰고 보고한다.
    → 코드는 절대 건드리지 않고, 깨진 문자 자리만 되살린다.

사용법:
    python repair_comments.py           # 제안만 출력
    python repair_comments.py --apply
"""
import difflib
import os
import re
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))

DAMAGED_RE = re.compile(r'\?{3,}|\ufffd')

# (복구 대상, 참조 소스)  — 참조는 파일 경로 또는 ('git', 커밋, 경로)
TARGETS = [
    (
        r'Server\ServerCore\Session.cpp',
        r'C:\Users\guswn\Downloads\04.+이동+동기화\MMO\Server\ServerCore\Session.cpp',
    ),
    (
        r'S1\Source\S1\Game\S1MyPlayer.cpp',
        ('git', '15637e5', 'S1/Source/S1/Game/S1MyPlayer.cpp'),
    ),
    (
        r'S1\Source\S1\Game\S1Player.cpp',
        ('git', '15637e5', 'S1/Source/S1/Game/S1Player.cpp'),
    ),
    (
        r'S1\Source\S1\Game\S1Player.h',
        ('git', '15637e5', 'S1/Source/S1/Game/S1Player.h'),
    ),
]


def read_text(src):
    """참조 원본을 읽어 유니코드 텍스트로 반환."""
    if isinstance(src, tuple):
        _, commit, path = src
        raw = subprocess.run(
            ['git', 'show', f'{commit}:{path}'],
            capture_output=True, cwd=ROOT,
        ).stdout
    else:
        with open(src, 'rb') as f:
            raw = f.read()

    try:
        return raw.decode('utf-8-sig')
    except UnicodeDecodeError:
        return raw.decode('cp949')


def skeleton(line):
    """비ASCII 문자와 ? 를 제거한 뼈대. 공백도 접어서 비교 안정성을 높인다."""
    s = ''.join(c for c in line if ord(c) < 0x80 and c != '?')
    return re.sub(r'\s+', ' ', s).strip()


def main():
    apply = '--apply' in sys.argv
    total_fixed = total_skipped = 0

    for rel_target, ref_src in TARGETS:
        target = os.path.join(ROOT, rel_target)
        if not os.path.exists(target):
            print(f'!! 대상 없음: {rel_target}')
            continue

        with open(target, 'rb') as f:
            raw = f.read()
        had_bom = raw.startswith(b'\xef\xbb\xbf')
        text = raw.decode('utf-8-sig')

        ref_lines = read_text(ref_src).splitlines()

        # ① 뼈대가 유일한 경우 — 바로 대응시킨다.
        ref_map = {}
        for rl in ref_lines:
            if not re.search(r'[가-힣]', rl):
                continue
            k = skeleton(rl)
            if not k:
                continue
            ref_map.setdefault(k, set()).add(rl.rstrip())

        lines = text.split('\n')

        # ② 뼈대가 겹치는 짧은 주석을 위해 파일 전체를 정렬해 위치로 대응시킨다.
        #    코드 줄(ASCII)은 대부분 그대로이므로 정렬 기준으로 신뢰할 수 있다.
        tgt_skel = [skeleton(l.rstrip('\r')) for l in lines]
        ref_skel = [skeleton(l) for l in ref_lines]
        align = {}
        for blk in difflib.SequenceMatcher(None, tgt_skel, ref_skel, autojunk=False)\
                          .get_matching_blocks():
            for off in range(blk.size):
                align[blk.a + off] = blk.b + off

        fixed = []
        skipped = []

        for i, line in enumerate(lines):
            stripped = line.rstrip('\r')
            if not DAMAGED_RE.search(stripped):
                continue

            k = skeleton(stripped)
            cands = ref_map.get(k)
            new_line = None
            how = ''

            if cands is not None and len(cands) == 1:
                new_line = next(iter(cands))
                how = '뼈대 유일'
            elif i in align:
                cand = ref_lines[align[i]].rstrip()
                # 정렬로 찾은 줄도 뼈대가 같아야 하고 한글이 있어야 채택한다
                if skeleton(cand) == k and re.search(r'[가-힣]', cand):
                    new_line = cand
                    how = '위치 정렬'

            if new_line is None:
                why = '참조에 대응 줄 없음' if cands is None else f'후보 {len(cands)}개 — 정렬 실패'
                skipped.append((i + 1, stripped, why))
                continue
            # 원본 들여쓰기(탭/공백)를 유지한다
            indent = re.match(r'[ \t]*', stripped).group(0)
            new_line = indent + new_line.lstrip()

            fixed.append((i + 1, stripped, new_line, how))
            lines[i] = new_line + ('\r' if line.endswith('\r') else '')

        print('=' * 74)
        print(f'{rel_target}   복구 {len(fixed)} · 보류 {len(skipped)}')
        print('=' * 74)
        for ln, old, new, how in fixed:
            print(f'  {ln:5} - {old.strip()[:88]}   [{how}]')
            print(f'  {"":5} + {new.strip()[:88]}')
        for ln, old, why in skipped:
            print(f'  {ln:5} ! {old.strip()[:70]}   [{why}]')
        print()

        total_fixed += len(fixed)
        total_skipped += len(skipped)

        if apply and fixed:
            out = '\n'.join(lines).encode('utf-8')
            if had_bom:
                out = b'\xef\xbb\xbf' + out
            with open(target, 'wb') as f:
                f.write(out)

    print(f'합계 — 복구 {total_fixed} · 보류 {total_skipped}')
    if not apply:
        print('실제로 적용하려면:  python repair_comments.py --apply')


if __name__ == '__main__':
    main()
