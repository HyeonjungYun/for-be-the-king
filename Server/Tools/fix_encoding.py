"""
소스 파일 인코딩을 UTF-8 + BOM 으로 통일한다.

왜 BOM 인가:
    MSVC 는 BOM 이 없는 소스를 시스템 코드페이지(한글 Windows = CP949)로 읽는다.
    BOM 이 있으면 항상 UTF-8 로 인식한다. /utf-8 컴파일 옵션 없이 안전한 유일한 방법.

사용법:
    python fix_encoding.py           # 검사만 (dry run)
    python fix_encoding.py --apply   # 실제 변환
"""
import os
import sys
import re

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))

EXCLUDE_DIRS = {
    'x64', 'Binaries', 'packages', 'Libraries', '.git', '.vs',
    'Intermediate', 'Saved', 'DerivedDataCache', 'obj', 'bin', 'Build',
    # 서드파티 · 백업 — 우리 코드가 아니므로 손대지 않는다.
    # (protobuf 테스트 픽스처는 바이트 단위로 인코딩을 검사한다. BOM 을 붙이면 깨진다)
    'ProtobufCore', 'Backup',
}
# 자동 생성 파일은 건드리지 않는다 (protoc / PacketGenerator 가 다시 만든다)
EXCLUDE_NAME_RE = re.compile(r'\.pb\.(cc|h)$', re.IGNORECASE)

# 이 스크립트 자신은 제외
SELF_PATH = os.path.abspath(__file__)

TARGET_EXT = {
    '.cpp', '.h', '.hpp', '.cc', '.cs', '.py',
    '.proto', '.jinja2', '.template',
}

BOM = b'\xef\xbb\xbf'


def classify(data: bytes):
    """(종류, 유니코드 텍스트) 반환. 변환 불필요면 텍스트는 None."""
    has_bom = data.startswith(BOM)
    body = data[len(BOM):] if has_bom else data

    try:
        text = body.decode('utf-8')
        if has_bom:
            return 'ok', None                 # 이미 UTF-8 + BOM
        return 'utf8-nobom', text             # BOM 만 붙이면 됨
    except UnicodeDecodeError:
        pass

    try:
        text = data.decode('cp949')
        return 'cp949', text                  # 디코딩 후 재인코딩 필요
    except UnicodeDecodeError:
        return 'unknown', None                # 손대지 않는다


def main():
    apply = '--apply' in sys.argv
    counts = {'ok': 0, 'utf8-nobom': 0, 'cp949': 0, 'unknown': 0, 'ascii': 0}
    changed = []
    failed = []

    for root, dirs, files in os.walk(ROOT):
        dirs[:] = [d for d in dirs if d not in EXCLUDE_DIRS]
        for name in files:
            if os.path.splitext(name)[1].lower() not in TARGET_EXT:
                continue
            if EXCLUDE_NAME_RE.search(name):
                continue

            path = os.path.join(root, name)
            if os.path.abspath(path) == SELF_PATH:
                continue
            with open(path, 'rb') as f:
                data = f.read()

            # 순수 ASCII 는 BOM 도 필요 없다 — 건드리지 않는다
            if not data.startswith(BOM) and not any(b > 0x7F for b in data):
                counts['ascii'] += 1
                continue

            kind, text = classify(data)
            counts[kind] += 1

            if kind in ('ok', 'unknown'):
                if kind == 'unknown':
                    failed.append(path)
                continue

            new_data = BOM + text.encode('utf-8')

            # 라운드트립 검증: 다시 읽었을 때 원본 텍스트와 같아야 한다
            if new_data[len(BOM):].decode('utf-8') != text:
                failed.append(path + '  (round-trip mismatch)')
                continue

            rel = os.path.relpath(path, ROOT)
            korean = len(re.findall(r'[가-힣]', text))
            changed.append((kind, rel, korean))

            if apply:
                with open(path, 'wb') as f:
                    f.write(new_data)

    print('=' * 70)
    print('변환 대상' if apply else '변환 예정 (dry run)')
    print('=' * 70)
    for kind, rel, korean in sorted(changed):
        print(f'  {kind:11} {rel:55} 한글 {korean}자')

    print()
    print(f'  이미 UTF-8+BOM : {counts["ok"]}')
    print(f'  순수 ASCII     : {counts["ascii"]} (변경 없음)')
    print(f'  CP949          : {counts["cp949"]}')
    print(f'  UTF-8 no BOM   : {counts["utf8-nobom"]}')
    print(f'  판별 불가      : {counts["unknown"]}')
    print(f'  총 변경        : {len(changed)}')

    if failed:
        print()
        print('!! 처리하지 못한 파일:')
        for p in failed:
            print('   ', p)

    if not apply:
        print()
        print('실제로 바꾸려면:  python fix_encoding.py --apply')


if __name__ == '__main__':
    main()
