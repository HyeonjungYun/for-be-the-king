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

## 개발 과정에서 풀어 온 문제

측정 → 실패 → 원인 규명 → 수정 → 재측정을 날짜순으로 기록하며 진행했습니다.
**틀린 가설과 계측기 오류도 지우지 않고 남겼습니다.**

| 주제 | 내용 |
|---|---|
| 30인 O(N²) 병목 | 인원 사다리 실측 · 패킷 병합 · 계측기 오차 2건 |
| 전투 패킷 대역폭 | 그리고 그 과정에서 드러난 이동 검증 구멍 |
| 120인 — 층 분리 | **1차 가설이 틀렸고 그 기록을 남겼다** |
| DB 통합 후 재측정 | 영속화가 측정 재현성을 깬 사례 |
| 커버리지 77% | 숫자보다 먼저 찾아낸 use-after-free |
| 지급 트랜잭션 + 멱등성 | 8,000 동시 요청 · **제가 오독한 −38%** |
| CI | 깨끗한 러너가 드러낸 저장소 무결성 문제 3건 |
| 사망 · 가방 · 루팅 | DB 우선 사망 트랜잭션 · 크래시 후 루팅 아이템 자기치유 · 부팅 복구 |

측정 기록과 설계 문서(ADR · GDD)는 이 저장소에 포함하지 않습니다.

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
.github/workflows/   CI — push 마다 서버 빌드 · 테스트 · 스트레스 100회 반복
```

## 빌드 · 실행

```
필요       Visual Studio 2022 · MySQL 8.0 · .NET 10 SDK · Unreal Engine 5.8

1  DB      powershell -ExecutionPolicy Bypass -File Server\Tools\setup_db.ps1 -Password <비번>
2  서버    Server\Server.sln 빌드 → GameServer 실행   (환경변수 FBTK_DB_PASSWORD 필요)
3  인증    cd Server\AuthServer && dotnet run          (http://127.0.0.1:8080)
4  클라    S1\S1.uproject
```

## 패키징 빌드 (클라이언트)

Win64 · **Development** 로 만듭니다. 보스 예고 · 투사체 · 장판 표시가 아직 디버그 그리기라 Shipping 에서는 화면에서 사라집니다.

```
전제     에디터를 닫는다 (열려 있으면 빌드 · 쿡이 파일 잠금에 걸린다)
         애니메이션 팩 7 개(Archer · BCD_Anims · Dagger · Fighter_Animations · Spear · EssentialAnimation ·
         GhostSamurai_Bundle)는 마켓플레이스 에셋이라 저장소에 없다 — S1\Content\ 에 따로 설치해야 동작이 나온다

1  빌드 · 쿡 · pak
   C:\Unreal5.8\UE_5.8\Engine\Build\BatchFiles\RunUAT.bat BuildCookRun -project="<저장소>\S1\S1.uproject" ^
       -noP4 -utf8output -platform=Win64 -clientconfig=Development ^
       -build -cook -stage -pak -archive -archivedirectory="<출력 폴더>" -prereqs

2  서버 데이터 동봉 — 클라가 실행 중에 S1 폴더 옆 Server\Data 의 CSV 를 읽는다
   robocopy Server\Data "<출력 폴더>\Windows\Server\Data" *.csv

3  실행 — 서버 주소는 명령줄로 덮어쓴다 (기본값 127.0.0.1)
   "<출력 폴더>\Windows\S1.exe" -windowed -ResX=1280 -ResY=720 ^
       "-ini:Game:[/Script/S1.S1LoginSubsystem]:AuthServerUrl=\"http://<호스트 IP>:8080\"" ^
       "-ini:Game:[/Script/S1.S1LoginSubsystem]:GameServerHost=<호스트 IP>"
```

- 결과는 약 1.1 GB 입니다. 테스터 PC 에는 `Windows` 폴더만 복사하면 되고 UE 는 필요 없습니다. VC++ 런타임이 없다는 오류가 나면 `Engine\Extras\Redist\en-us\vc_redist.x64.exe` 를 설치합니다.
- 호스트는 위 **빌드 · 실행**의 1~3(DB · 게임 서버 · 인증 서버)을 띄웁니다. 다른 PC 에서 붙으려면 `FBTK_BIND_ADDRESS=0.0.0.0` 으로 게임 서버를 띄우고, 인증 서버는 `Kestrel__Endpoints__Http__Url=http://0.0.0.0:8080` 으로 띄운 뒤 방화벽에서 TCP 7777 · 8080 을 엽니다.
- 쿡 대상은 `S1/Config/DefaultGame.ini` 에 정해 두었습니다. 맵은 `Floor1` 하나만 쿡하고, CSV 경로로만 불리는 애니메이션 151 개 폴더는 항상 쿡합니다(쿠커가 스스로 찾지 못한다). 개발용 로그인 토큰은 `IniKeyDenylist` 로 패키지에서 뺍니다.
- 🔴 인증이 http 라 비밀번호가 평문으로 오갑니다. 같은 공간의 LAN 플레이테스트까지만 씁니다.

## AI 도구 사용 경계

개발에 AI 코딩 에이전트(Claude Code)를 썼습니다. 어디까지 맡겼는지 밝힙니다.

| 영역 | 작성 |
|---|---|
| **게임 서버 · 넷코드 · 프로토콜** (`Server/ServerCore` · `Server/GameServer` · `*.proto` · 클라 `Network/`) | **직접 작성.** AI 에이전트에 수정 권한을 주지 않았습니다 |
| 설계 문서 · 리뷰 · 수정 제안 | AI. 파일과 줄 번호까지 짚은 제안을 받아 읽고 판단한 뒤 적용 |
| 클라이언트 연출 코드 · 문서 정리 | AI 초안을 검토해 반영 |

서버가 학습 목표였기에 **제가 설명하지 못하는 줄이 게임 서버에 한 줄도 없게** 하려는 경계입니다.

---

문의 · 코드 관련 질문은 이슈나 메일로 받습니다.
