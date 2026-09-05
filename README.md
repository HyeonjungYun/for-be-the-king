# For be the King

**자체 C++ IOCP 게임 서버 + Unreal Engine 5.8 클라이언트.**
엔진 리플리케이션을 쓰지 않고 네트워크 계층을 바닥부터 직접 구현한 멀티플레이 프로젝트입니다.

탑다운 PvPvE 익스트랙션 게임 — 30명이 한 던전 층에 들어가 파밍하고, 제한시간 안에 탈출해야
장비를 들고 나옵니다. 죽으면 들고 있던 장비를 전부 떨어뜨립니다.

| | |
|---|---|
| **역할** | 1인 개발 (서버 · 클라이언트 · 설계 전부) |
| **기간** | 2026-06 ~ (진행 중) |
| **서버** | C++17 · Windows IOCP · Protobuf over TCP · 워커 5스레드 |
| **인증** | C# ASP.NET Core (별도 프로세스) · PBKDF2-SHA256 |
| **클라** | Unreal Engine 5.8 (C++) — UE 리플리케이션 **미사용** |
| **DB** | MySQL 8.0 (C API) — 전용 스레드 풀에서 비동기 실행 |

## 실측 요약

**120인 동접**(층당 30명 × 4층) 기준입니다.

| 항목 | 값 |
|---|---|
| 서버 프레임 (`Room` JobQueue 1회 Flush) | **P95 ≤ 100 μs** |
| 서버 송신 | **14.4 Mbps** |
| 메모리 | **158 MB** |
| 지급 정확성 | 8,000개 동시 요청 → **정확히 1,000번만 지급**, 잔액 오차 0 |
| 테스트 | GoogleTest **53개** · `ServerCore` 라인 커버리지 **77%** |
| CI | GitHub Actions — push 마다 빌드 + 테스트 + **스트레스 100회 반복** |

---

## 📄 먼저 읽을 문서

### → **[`docs/portfolio/server-portfolio.md`](docs/portfolio/server-portfolio.md)**

측정 → 실패 → 원인 규명 → 수정 → 재측정의 전 과정을 기록한 문서입니다.
**틀린 가설과 계측기 오류까지 지우지 않고 남겼습니다.**

| 절 | 내용 |
|---|---|
| §1 | **강의에서 온 것과 직접 얹은 것** — 어디까지가 배운 것인지 먼저 밝힙니다 |
| §7 | 동접 성능 — 30인 O(N²) 병목을 패킷 병합으로, 120인에서 다시 만나 층 분리로 |
| §8 | 자체 계측 하네스 — 그리고 계측기 자체에서 찾은 오차 2건 |
| §9 | 부하 생성기 — 봇이 사람보다 가혹해서 잡아낸 버그 |
| §11 | 동시성 버그와 스트레스 테스트 — 커버리지가 찾아낸 use-after-free |
| §13 | 데이터 영속화 · 인증 분리 (게임 서버는 비밀번호를 모릅니다) |
| §14 | 지급 트랜잭션 + 멱등성 — 그리고 제가 오독한 측정치 |
| §15 | 아이템 · 인벤토리 — 레이드 경계 커밋 |
| **§16** | **알려진 한계** — 재지 못한 것과 풀지 못한 것의 목록 |

## 저장소 구조

```
Server/              게임 서버 (C++)
├── ServerCore/          네트워크 라이브러리 — IOCP · 세션 · JobQueue · DB · 계측
├── GameServer/          게임 로직 — 방 · 이동 검증 · 전투 · 아이템
├── AuthServer/          인증 서버 (C# ASP.NET Core)
├── DummyClient/         부하 생성 봇
├── ServerCoreSTests/    GoogleTest
└── Tools/               스키마 · 시딩 · PacketGenerator

S1/                  Unreal Engine 5.8 클라이언트
docs/architecture/   ADR — 기술 결정 기록
docs/portfolio/      포트폴리오
production/          로드맵 · 게이트 정의 · 전체 실측 기록
design/              게임 디자인 문서
```

## 빌드 · 실행

```
필요       Visual Studio 2022 · MySQL 8.0 · .NET 10 SDK · Unreal Engine 5.8

1  DB      powershell -ExecutionPolicy Bypass -File Server\Tools\setup_db.ps1 -Password <비번>
2  서버    Server\Server.sln 빌드 → GameServer 실행   (환경변수 FBTK_DB_PASSWORD 필요)
3  인증    cd Server\AuthServer && dotnet run          (http://127.0.0.1:8080)
4  클라    S1\S1.uproject
```

## AI 도구 사용 경계

저장소에 `.claude/`(에이전트 설정 217개 파일)가 그대로 보입니다. 숨기지 않고 규칙을 밝힙니다.

| 영역 | 작성 |
|---|---|
| **게임 서버 · 넷코드 · 프로토콜** (`Server/ServerCore` · `Server/GameServer` · `*.proto` · 클라 `Network/`) | **직접 작성.** AI 에이전트에 수정 권한을 주지 않았습니다 |
| 설계 문서 · 리뷰 · 수정 제안 | AI. 파일과 줄 번호까지 짚은 제안을 받아 읽고 판단한 뒤 적용 |
| 클라이언트 연출 코드 · 문서 정리 | AI 초안을 검토해 반영 |

서버가 학습 목표였기에 **제가 설명하지 못하는 줄이 게임 서버에 한 줄도 없게** 하려는 경계입니다.
규칙 원문은 [`.claude/docs/technical-preferences.md`](.claude/docs/technical-preferences.md)
§ 작업 소유권 경계에 있습니다.

---

문의 · 코드 관련 질문은 이슈나 메일로 받습니다.
