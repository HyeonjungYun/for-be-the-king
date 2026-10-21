"""Are the generated packet files up to date with the .proto files? (verify-server.ps1, 2026-10-21)

  python -I Tools/verify/check_proto.py <repo> <out.json>

Regenerates everything GenPackets.bat makes into a temporary folder (protoc + GenPackets.exe + Tools/codegen templates)
and compares it byte for byte with the copies the three projects compile: GameServer · DummyClient · S1.
A difference means someone edited a .proto without running GenPackets.bat (or edited a generated file by hand).
Nothing in the repository is written.
"""
import filecmp, json, os, shutil, subprocess, sys, tempfile

repo, out = sys.argv[1], sys.argv[2]
binDir = os.path.join(repo, 'Server', 'Common', 'protoc-21.12-win64', 'bin')
templates = os.path.join(repo, 'Tools', 'codegen', 'PacketGenerator', 'Templates')
errors = []

work = tempfile.mkdtemp(prefix='fbtk-proto-')
try:
    for f in ('Enum.proto', 'Struct.proto', 'Protocol.proto', 'protoc.exe', 'GenPackets.exe'):
        shutil.copy(os.path.join(binDir, f), work)
    shutil.copytree(templates, os.path.join(work, 'Templates'))
    steps = [['protoc.exe', '-I=./', '--cpp_out=./', './Enum.proto'],
             ['protoc.exe', '-I=./', '--cpp_out=./', './Struct.proto'],
             ['protoc.exe', '-I=./', '--cpp_out=./', './Protocol.proto'],
             ['GenPackets.exe', '--path=./Protocol.proto', '--output=ClientPacketHandler', '--recv=S_', '--send=C_'],
             ['GenPackets.exe', '--path=./Protocol.proto', '--output=ServerPacketHandler', '--recv=C_', '--send=S_']]
    for s in steps:
        r = subprocess.run([os.path.join(work, s[0])] + s[1:], cwd=work, capture_output=True, text=True)
        if r.returncode != 0:
            errors.append(f'{" ".join(s)} failed: {r.stderr.strip()[:300]}')

    pb = [f'{n}.pb.{e}' for n in ('Enum', 'Struct', 'Protocol') for e in ('h', 'cc')]
    targets = {
        'Server/GameServer': pb + ['ServerPacketHandler.h'],
        'Server/DummyClient': pb + ['ClientPacketHandler.h'],
        'S1/Source/S1/Network': pb,
        'S1/Source/S1': ['ClientPacketHandler.h'],
    }
    compared = 0
    if not errors:
        for folder, names in targets.items():
            for n in names:
                gen = os.path.join(work, n)
                cur = os.path.join(repo, folder, n)
                compared += 1
                if not os.path.exists(cur):
                    errors.append(f'{folder}/{n}: missing')
                elif not filecmp.cmp(gen, cur, shallow=False):
                    errors.append(f'{folder}/{n}: differs from what GenPackets.bat makes from the current .proto - run GenPackets.bat (or revert the hand edit)')
finally:
    shutil.rmtree(work, ignore_errors=True)

json.dump({'compared': compared if 'compared' in dir() else 0, 'errors': errors}, open(out, 'w', encoding='utf-8'), ensure_ascii=False, indent=1)
print(f'[PROTO] compared {compared if "compared" in dir() else 0} generated files - {len(errors)} error(s)')
for e in errors:
    print('  ' + e)
sys.exit(1 if errors else 0)
