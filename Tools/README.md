# Tools — 저장소의 모든 도구

2026-10-21 에 흩어져 있던 도구(루트 `tools/` · `Server/Tools/` · `.github/workflows/`)를 한 곳에 모았다.
원격 저장소에도 올라간다 (`repo/push-remote.ps1` 의 허용 경로).

| 폴더 | 무엇 | 대표 명령 |
|---|---|---|
| `verify/` | **서버 검증 툴** — 서버를 열기 전에 정적 검사 · 빌드 + 테스트 · DB 스키마 · 격리 부팅 스모크 | `.\Tools\verify\verify-server.ps1` |
| `ServerMonitor/` | **디버깅 툴** (C# WPF) — 접속자 · 플레이어별 로그 · 태그 필터 · 맵 · 지표 · 관리 명령 · 지난 로그 | `dotnet run --project Tools\ServerMonitor` |
| `ci/` | GitHub Actions — 원본은 `ci/workflows/*.yml`, `.github/workflows/` 는 사본 | `.\Tools\ci\sync-workflows.ps1` (`-Check`) |
| `db/` | 스키마 · 이전 · 시드 SQL · `setup_db.ps1` · 로그인/기본 장비 시험 스크립트 | `.\Tools\db\setup_db.ps1 -Password …` |
| `codegen/` | PacketGenerator (`.proto` → 패킷 핸들러). `Server/Common/protoc-21.12-win64/bin/GenPackets.bat` 이 템플릿을 여기서 가져간다 | `GenPackets.bat` |
| `package/` | 클라 패키징 · 한 PC 실행 묶음 | `.\Tools\package\package-client.ps1` → `package-bundle.ps1 -Zip` |
| `dev/` | 개발 실행 (계정별 클라 · LAN 플레이테스트 호스트 / 테스터) | `.\Tools\dev\dev-launch.ps1 dev_1` |
| `repo/` | 원격 푸시 · 인코딩 점검 | `.\Tools\repo\push-remote.ps1 -DryRun` · `python Tools/repo/fix_encoding.py` |
| `level/` · `anim/` · `fx/` | 레벨 생성 · 검사 · 애니메이션 클립 표 · 재질 생성 | |
| `telemetry/` | 런 텔레메트리 요약 | `python Tools/telemetry/summarize_runs.py` |
| `demo/` | 포트폴리오 시연 녹화 스크립트 (로컬 전용 - 커밋하지 않는다) | |

**규칙**

- 워크플로는 `ci/workflows/` 를 고치고 `sync-workflows.ps1` 로 복사한다. `.github/workflows/` 를 직접 고치면 검증 툴의 정적 검사가 실패한다
- 새 PowerShell 스크립트는 ASCII 로 쓰거나 UTF-8 **BOM** 으로 저장한다 (PowerShell 5.1 은 BOM 없는 파일을 cp949 로 읽는다)
- 스크립트 안에서 저장소 루트는 `Split-Path (Split-Path $PSScriptRoot -Parent) -Parent` (Tools 아래 두 단계)
