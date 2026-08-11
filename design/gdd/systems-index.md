# Systems Index: For be the King (가제)

> **Status**: Approved
> **Created**: 2026-08-11
> **Last Updated**: 2026-08-11
> **Source Concept**: `design/gdd/game-concept.md` (결정 19건 반영본)
> **Supersedes**: `design/archive/dungeonking-2026-07/systems-index.md` (DungeonKing, 29개 시스템)

---

## Overview

**For be the King**은 UE 5.8 클라이언트 + 자체 C++ IOCP 서버로 만드는 **탑다운 익스트랙션 PvPvE 던전 크롤러**(층당 30명 × 4층)다. 기계적 스코프는 세 축으로 갈린다.

1. **액션 축** — 탑다운 조작 + **장비=스킬** 클래스리스 빌드 + 하드/소프트 CC 체계
2. **넷코드 축** — 자체 IOCP 서버 · **서버 권위 + 클라이언트 예측** · Protobuf/TCP. **UE 리플리케이션 미사용**
3. **손실·정보 축** — 풀루팅 사망 · 30분 체류 타이머 · **미니맵 없는 근거리 레이더**

코어 루프(베이스캠프 → 개활지 → 집 → 귀환 → 탈출)의 각 단계가 이 축에 하나씩 대응한다.

### 이 인덱스가 이전 것과 다른 점

DungeonKing 인덱스(29개)를 그대로 잇지 않는다. **전제가 세 개 바뀌었다.**

| 항목 | DungeonKing | **현재** |
|---|---|---|
| 넷코드 | UE Dedicated Server + Iris/RepGraph | **자체 IOCP 서버** — 이미 구현·동작 중 |
| 동시 인원 | 구역당 12~16 | **층당 30** |
| 캐릭터 성장 | 레벨 18 · 경험치 · 스킬포인트 | **없음.** 장비 레벨만 |

그 결과:

- **`매치메이킹/인스턴스 관리` 제거** — 층 1개 = Room 1개, 30명. "베이스캠프에서 층 선택 = Room 배정"이 자명해 별도 시스템이 필요 없다
- **`성장 시스템`(레벨·경험치) 제거** — P2가 캐릭터 성장을 금지한다
- **`시즌 & 프레스티지`·`바운티/평판` 제거** — 컨셉 v3에서 폐기됨
- **`배신 시스템` 미등재** — P3("시스템은 처벌하지 않는다")가 시스템 개입을 금지하므로, 배신은 **파티 시스템의 규칙 부재 자체**가 설계다. 만들 것이 없다
- **`레이더 시스템` 신규** — 미니맵 폐지 결정의 대체물
- **`시야/릴러번시` → `FoW` + `AOI`로 분리** — 하나는 정보 은닉(P4), 하나는 대역폭 최적화. 30명 확정으로 후자의 성격이 "필수"에서 "최적화"로 바뀌어 더는 한 시스템으로 묶을 이유가 없다

---

## Systems Enumeration

| # | System Name | Category | Priority | Status | Design Doc | Depends On |
|---|-------------|----------|----------|--------|------------|------------|
| 1 | 입력 시스템 | Core | Tier 1 | Not Started | — | — |
| 2 | 네트워킹 & 서버 아키텍처 | Core | Tier 1 | Not Started | — | — |
| 3 | 이동 & 카메라 시스템 | Core | Tier 1 | **Designed** | `design/gdd/movement-camera.md` | 입력, 네트워킹 |
| 4 | 계정 & 로그인 (inferred) | Core | Tier 1 | Not Started | — | 네트워킹 |
| 5 | 세이브 & 영속성 (inferred) | Persistence | Tier 1 | Not Started | — | 계정 & 로그인 |
| 6 | 아이템 & 장비 시스템 | Economy | Tier 1 | Not Started | — | 세이브 & 영속성 |
| 7 | 전투 시스템 | Gameplay | Tier 1 | Not Started | — | 이동 & 카메라, 네트워킹 |
| 8 | 스킬 시스템 | Gameplay | Tier 1 | Not Started | — | 전투, 입력 |
| 9 | **장비-스킬 결속 시스템** ⭐ | Gameplay | Tier 1 | Not Started | — | 스킬, 아이템 & 장비 |
| 10 | 몬스터 AI 시스템 | Gameplay | Tier 1 | Not Started | — | 전투, 이동 & 카메라 |
| 11 | 인벤토리 & 사망 처리 시스템 | Gameplay | Tier 1 | Not Started | — | 아이템 & 장비, 전투, 세이브 & 영속성 |
| 12 | 레벨 & 맵 시스템 | Gameplay (World) | Tier 1 | Not Started | — | 네트워킹, 이동 & 카메라 |
| 13 | 탈출 & 체류 타이머 시스템 | Gameplay | Tier 1 | Not Started | — | 레벨 & 맵, 인벤토리 & 사망 처리, 네트워킹 |
| 14 | 레이더 시스템 | Gameplay (Info) | Tier 1 | Not Started | — | 이동 & 카메라, 네트워킹 |
| 15 | HUD 시스템 (inferred) | UI | Tier 1 | Not Started | — | 전투, 아이템 & 장비, 탈출 & 체류 타이머, 인벤토리 |
| 16 | 베이스캠프 시스템 (inferred) | Hub | Tier 1 (최소) | Not Started | — | 세이브 & 영속성, 레벨 & 맵 |
| 17 | 파티 시스템 | Gameplay (Social) | Tier 2 | Not Started | — | 네트워킹, 티어 & 게이팅, 인벤토리 |
| 18 | AOI / 거리 컬링 | Core (Netcode) | Tier 2 | Not Started | — | 네트워킹, 레벨 & 맵 |
| 19 | FoW (전장의 안개) | Gameplay (Info) | Tier 2 | Not Started | — | 네트워킹, AOI, 레벨 & 맵 |
| 20 | 티어 & 게이팅 시스템 | Progression | Tier 3 | Not Started | — | 아이템 & 장비, 세이브 & 영속성 |
| 21 | 창고 시스템 | Economy | Tier 3 | Not Started | — | 아이템 & 장비, 세이브 & 영속성, 인벤토리 |
| 22 | 골드 시스템 | Economy | Tier 3 | Not Started | — | 세이브 & 영속성, 인벤토리 & 사망 처리 |
| 23 | 고독한 길 시스템 | Gameplay (Social) | Tier 3 | Not Started | — | 파티, 아이템 & 장비, 티어 & 게이팅 |
| 24 | 안티치트 확장 (inferred) | Core (Security) | Tier 3 | Not Started | — | 네트워킹, 전투 |
| 25 | 튜토리얼 & 온보딩 (inferred) | Meta | Full Vision | Not Started | — | (다수 코어 시스템) |

> **점진 구현 시스템**: `아이템 & 장비`(Tier 1은 등급만 → Tier 3에서 강화 추가)와
> `베이스캠프`(Tier 1은 "층 입장" 버튼만 → Tier 2/3에서 창고·파티 결성 추가)는
> 위 티어에서 **최소 버전**으로 시작한다. 표의 Priority는 "1차 필요 티어"다.

---

## Categories

| Category | Description | 이 게임의 시스템 |
|----------|-------------|-----------------|
| **Core** | 모든 것이 의존하는 기반 | 입력, 네트워킹 & 서버 아키텍처, 이동 & 카메라, 계정 & 로그인, AOI, 안티치트 |
| **Gameplay** | 재미를 만드는 시스템 | 전투, 스킬, 장비-스킬 결속, 몬스터 AI, 인벤토리 & 사망, 레벨 & 맵, 탈출 & 타이머, 레이더, FoW, 파티, 고독한 길 |
| **Progression** | 성장 (캐릭터 아닌 **장비** 축) | 티어 & 게이팅 |
| **Economy** | 자원 생성·소비 | 아이템 & 장비, 창고, 골드 |
| **Persistence** | 저장·연속성 | 세이브 & 영속성 |
| **UI** | 플레이어 대면 정보 | HUD |
| **Hub** | 던전 밖 공간 | 베이스캠프 |
| **Meta** | 코어 루프 밖 | 튜토리얼 & 온보딩 |

> **`Progression` 카테고리가 거의 비어 있는 것이 정상이다.** P2("힘은 손에 든 것에서만")가
> 캐릭터 성장을 금지하므로, 성장에 해당하는 것은 전부 `Economy`(장비 획득·축적)로 간다.

---

## Priority Tiers

`game-concept.md` § Scope Tiers와 **동일한 용어**를 쓴다.

| Tier | 정의 | 기한 | 설계 긴급도 |
|------|------|------|-------------|
| **Tier 1 (MVP)** | 코어 루프가 작동하는 최소 — "들고 나가는 결정이 갈등을 유발하나?"를 검증 | **2026-11-03** (12주) | 설계 FIRST |
| **Tier 2** | 파티·정보 은닉·대역폭 최적화 | **2026-12-31** | 설계 SECOND |
| **Tier 3** | 층 4개 · 티어 게이팅 · 경제 | 2027 | 설계 THIRD |
| **Full Vision** | 폴리시·온보딩 | — | 필요 시 |

---

## Dependency Map

### Foundation Layer (의존 없음)

1. **입력 시스템** — WASD 이동 + 커서 조준 분리, 6키 스킬. 조작 정체성의 출발점
2. **네트워킹 & 서버 아키텍처** — 거의 모든 시스템이 의존. **단, 이미 구현·동작 중이라 위험도가 낮다**

### Core Layer (Foundation 의존)

1. **이동 & 카메라** — depends on: 입력, 네트워킹
2. **계정 & 로그인** — depends on: 네트워킹
3. **세이브 & 영속성** — depends on: 계정 & 로그인

### Feature Layer (Core 의존)

1. **아이템 & 장비** — depends on: 세이브 & 영속성
2. **전투 시스템** — depends on: 이동 & 카메라, 네트워킹
3. **스킬 시스템** — depends on: 전투, 입력
4. **장비-스킬 결속** ⭐ — depends on: 스킬, 아이템 & 장비
5. **몬스터 AI** — depends on: 전투, 이동 & 카메라
6. **인벤토리 & 사망 처리** — depends on: 아이템 & 장비, 전투, 세이브 & 영속성
7. **레벨 & 맵** — depends on: 네트워킹, 이동 & 카메라
8. **탈출 & 체류 타이머** — depends on: 레벨 & 맵, 인벤토리 & 사망 처리, 네트워킹
9. **레이더** — depends on: 이동 & 카메라, 네트워킹
10. **티어 & 게이팅** — depends on: 아이템 & 장비, 세이브 & 영속성
11. **창고** — depends on: 아이템 & 장비, 세이브 & 영속성, 인벤토리
12. **골드** — depends on: 세이브 & 영속성, 인벤토리 & 사망 처리

### Social Layer

1. **파티** — depends on: 네트워킹, 티어 & 게이팅, 인벤토리
2. **고독한 길** — depends on: 파티, 아이템 & 장비, 티어 & 게이팅
3. **베이스캠프** — depends on: 세이브 & 영속성, 레벨 & 맵 (Tier 1 최소) / + 창고, 파티 (Tier 2~3)

### Presentation Layer

1. **HUD** — depends on: 전투, 아이템 & 장비, 탈출 & 체류 타이머, 인벤토리

### Polish Layer

1. **AOI / 거리 컬링** — depends on: 네트워킹, 레벨 & 맵
2. **FoW** — depends on: 네트워킹, AOI, 레벨 & 맵
3. **안티치트 확장** — depends on: 네트워킹, 전투
4. **튜토리얼 & 온보딩** — depends on: 다수 코어 시스템

---

## Recommended Design Order

| Order | System | Priority | Layer | Agent(s) | Est. Effort |
|-------|--------|----------|-------|----------|-------------|
| 1 | 입력 시스템 | Tier 1 | Foundation | game-designer, unreal-specialist | S |
| 2 | 네트워킹 & 서버 아키텍처 | Tier 1 | Foundation | network-programmer, technical-director | M |
| 3 | ~~이동 & 카메라~~ ✅ | Tier 1 | Core | — | **완료 2026-08-11** |
| 4 | 계정 & 로그인 | Tier 1 | Core | network-programmer | S |
| 5 | 세이브 & 영속성 | Tier 1 | Core | systems-designer, technical-director | M |
| 6 | 아이템 & 장비 | Tier 1 | Feature | economy-designer, systems-designer | M |
| 7 | 전투 시스템 | Tier 1 | Feature | systems-designer, game-designer | L |
| 8 | 스킬 시스템 | Tier 1 | Feature | systems-designer | M |
| 9 | **장비-스킬 결속** ⭐ | Tier 1 | Feature | game-designer, systems-designer | L |
| 10 | 몬스터 AI | Tier 1 | Feature | ai-programmer, game-designer | M |
| 11 | 인벤토리 & 사망 처리 | Tier 1 | Feature | systems-designer, game-designer | M |
| 12 | 레벨 & 맵 | Tier 1 | Feature | level-designer | L |
| 13 | 탈출 & 체류 타이머 | Tier 1 | Feature | game-designer, level-designer | M |
| 14 | 레이더 | Tier 1 | Feature | game-designer, ux-designer | S |
| 15 | HUD | Tier 1 | Presentation | ux-designer, ue-umg-specialist | M |
| 16 | 베이스캠프 (최소) | Tier 1 | Social | game-designer, ux-designer | S |
| 17 | 파티 | Tier 2 | Social | game-designer, network-programmer | M |
| 18 | AOI / 거리 컬링 | Tier 2 | Polish | network-programmer | M |
| 19 | FoW | Tier 2 | Polish | network-programmer, technical-director | L |
| 20 | 티어 & 게이팅 | Tier 3 | Feature | economy-designer, systems-designer | M |
| 21 | 창고 | Tier 3 | Feature | economy-designer | S |
| 22 | 골드 | Tier 3 | Feature | economy-designer | M |
| 23 | 고독한 길 | Tier 3 | Social | game-designer, systems-designer | M |
| 24 | 안티치트 확장 | Tier 3 | Polish | security-engineer | M |
| 25 | 튜토리얼 & 온보딩 | Full Vision | Polish | ux-designer | M |

> Effort: **S** = 1 세션 · **M** = 2~3 세션 · **L** = 4+ 세션 (1 세션 = GDD 하나를 완성하는 집중 설계 대화).
> 같은 레이어의 독립 시스템은 병렬 설계 가능 (예: 7·12, 10·14).

### ⚠️ 설계 순서 ≠ 구현 순서

12주 구현 일정(`game-concept.md` § MVP Definition)은 **넷코드 1주 → 전투 3주 → 아이템 2주 → AI 1주 → 레벨 2주 → 탈출 1주 → 통합 2주**다.
설계 순서는 **의존성**을 따르고, 구현 순서는 **리스크**를 따른다. 둘을 혼동하지 말 것.

---

## Circular Dependencies

**없음.** 모든 의존이 단방향이다.

확인한 잠재 순환 두 건과 해소 방식:

| 잠재 순환 | 해소 |
|---|---|
| `전투` ↔ `아이템` (장비가 공격력을 주고, 전투가 아이템을 드랍한다) | **드랍은 `인벤토리 & 사망 처리`의 책임**으로 분리. 전투는 "사망 이벤트"만 발생시키고 무엇이 떨어지는지는 모른다 |
| `파티` ↔ `티어 & 게이팅` (파티 인원 제한이 티어에 의존하고, 티어 판정이 파티 진입 시 일어난다) | **티어 판정은 개인 단위**, 파티는 그 결과를 읽기만 한다. 단방향 유지 |

---

## High-Risk Systems

| System | Risk Type | Risk Description | Mitigation |
|--------|-----------|-----------------|------------|
| 🔴 **세이브 & 영속성** | Technical / Scope | **코드 0줄.** 서버에 DB 계층이 전혀 없는데 6개 시스템이 의존한다. 12주 일정에 독립 항목이 없다 | **범위를 최소로 묶는다** — 계정 식별자 + 기본 캐릭터 상태만. 창고·골드가 Tier 3이라 저장할 게 원래 적다. 넷코드/아이템 주차에 흡수 |
| 🔴 **이동 & 카메라** | Design (blocking) | `SpringArm TargetArmLength`·FOV 미측정. **레이더 반경·동시 가시 밀도·LOD 전환 거리·집 배치가 전부 이 값에 종속** | 설계 착수 전 에디터에서 실측. 5분이면 되는데 안 하면 4개 시스템이 가정값 위에 세워진다 |
| ⭐ **장비-스킬 결속** | Design | USP. P2의 기계적 구현체이며 재미없으면 게임 정체성이 붕괴한다 | 전투 프로토타입에서 손맛 우선 검증. **"실력 표현 축" Open Question을 먼저 확정** |
| **전투 시스템** | Design | "30초 교전 손맛"과 "실력 표현 축"이 둘 다 미결인데 구현은 2~4주차다 | 손맛보다 **실력 표현 축을 먼저** 정한다. CC 규칙(무기 부위 전용·최대값 중첩)은 이미 확정 |
| **네트워킹 & 서버 아키텍처** | Technical | 구현은 되어 있으나 **동시성 스트레스 테스트가 0개**. 2026-08-10에 `Session::_sendQueue` 데이터 레이스를 실제로 겪었고, 단위 테스트로는 안 잡혔다 | GoogleTest 도입 시 `LockQueue`·`Session::_sendQueue`·`Room::_objects` 다중 스레드 테스트 최우선 |
| **티어 & 게이팅** | Design | **"고티어의 1층 소모품 독점"이 구조적 문제로 남아 있다.** 자기교정 장치 둘(사망→강등, 자경단)이 정확히 이 시나리오에서 가장 약하다 | 드랍률 조정으로 안 풀린다. 층 하한선·저층 스탯 정규화·세션당 입장 제한 중 하나를 **설계 착수 전에** 정한다 |
| **레벨 & 맵** | Scope | 집이 1개 → **3개**로 늘었는데 구현은 2주다 | **모듈 재조합**으로 만든다. 안 되면 절삭 순서대로 집 하나를 Tier 2로 민다 |

---

## 아카이브 계승 판단

`design/archive/dungeonking-2026-07/` 의 기존 GDD를 재활용한다.

| 아카이브 문서 | 계승 대상 시스템 | 계승도 | 필요한 수정 |
|---|---|---|---|
| `input-system.md` | #1 입력 시스템 | **높음** | 🔴 **커서 서비스 2개로 분리 필수** + 인벤토리/줍기 IA 추가 + Trigger 타입 명세. 상세는 `active.md` |
| `movement-camera.md` | #3 이동 & 카메라 | **높음** | §Core Rules 9(서버 권위 전제) — **이제 서버 권위가 맞으므로 오히려 부합한다.** 이동속도 340 반영 |
| `combat-system.md` | #7 전투 시스템 | **중간** | 공식 5/6·규칙 6/8·엣지 14/16 생존. 🔴 **파손 요인 3개** — GAS 전역 참조 7곳 · D4→아이템화 전환(서브시스템 신설) · **"성장" 소실로 상수 재검증**. 상세는 `active.md` |
| `entities.yaml` | #6 아이템 & 장비 | **높음** | 살아남는 공식만 이전 |

---

## Progress Tracker

| Metric | Count |
|--------|-------|
| Total systems identified | 25 |
| Design docs started | 1 |
| Design docs reviewed | 0 |
| Design docs approved | 0 |
| **Tier 1 (MVP) systems designed** | **1 / 16** |
| Tier 2 systems designed | 0 / 3 |
| Tier 3 systems designed | 0 / 5 |

---

## Next Steps

- [x] 시스템 열거·의존성·우선순위 검토 및 승인 — **완료 2026-08-11**
- [x] ~~카메라 세팅 실측~~ → ✅ **역산으로 확정** (`TargetArmLength` 2,600 · FOV 60° · pitch −60°)
- [x] ~~`/design-system 이동-카메라`~~ → ✅ **완료 2026-08-11** — 757행, 공식 7개, AC 30여 개
- [ ] 🔴 **`production/roadmap.md` §8 결정 3건** — 이게 나와야 Phase 배분이 확정된다
- [ ] `/design-system 입력-시스템` (또는 `/map-systems next`)
- [ ] 각 GDD 완성 후 `/design-review design/gdd/[system].md`
- [ ] **정식 GDD 6개 + quick-spec 10개** 완성 시 `/gate-check pre-production`

> **2026-08-11 결정 3**: Tier 1 16개를 전부 정식 8섹션으로 쓰면 **+3주**가 든다 (14주 → 17주).
> **공식과 수치가 있는 시스템만 정식으로 쓴다.**
>
> | 정식 8섹션 (6개) | quick-spec 1페이지 (10개) |
> |---|---|
> | 입력 · **이동&카메라 ✅** · 전투 · 아이템&장비 · 인벤토리&사망 · 탈출&타이머 | 네트워킹 · 계정&로그인 · 세이브&영속성 · 스킬 · 장비-스킬결속 · 몬스터AI · 레벨&맵 · 레이더 · HUD · 베이스캠프 |
>
> 각 Phase 착수 전 **0.5일**을 그 Phase 시스템 문서에 쓴다 — Phase 시간 안에 포함되므로 별도 시간이 필요 없다.
> **전부 quick-spec으로 가지 않은 이유**: 전투 공식을 코드에만 두면 3개월 뒤 밸런싱할 때
> `K_mitigation = 100`이 왜 100인지 아무도 모른다.

### 병행 가능한 코드 작업

설계와 무관하게 지금 할 수 있는 것들 (`production/session-state/active.md` 참조):

- 🔴 서버 이동 검증 구현 (`ServerPacketHandler.cpp:83` TODO) — 이동속도 340 확정으로 착수 가능
- 🔴 `GameSession::OnDisconnected` Room 이탈 누락 → 유령 플레이어
- ⚠️ `AS1Player` 원격 프록시 애니메이션 — 해법 제시됨, 적용 미확인
