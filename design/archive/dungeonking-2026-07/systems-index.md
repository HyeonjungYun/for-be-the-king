# Systems Index: DungeonKing

> **Status**: Draft
> **Created**: 2026-07-18
> **Last Updated**: 2026-07-18
> **Source Concept**: design/gdd/game-concept.md

---

## Overview

DungeonKing은 UE 5.8 기반의 **PvPvE 익스트랙션 심리스 던전 크롤러**(PC, 구역당 12~16인)로, 그 기계적 스코프는 세 개의 큰 축에 걸쳐 있다: (1) **LoL식 탑다운 전투 + 장비=스킬 클래스리스 빌드**라는 코어 액션 축, (2) **서버권위 심리스 멀티플레이 + 전장의 안개 + 풀루팅**이라는 넷코드/보안 축, (3) **협동·배신·솔로("고독한 길") 선택 + 익스트랙션 수성전 + 시즌 순환**이라는 소셜/메타 축. 코어 루프(베이스캠프 → 파밍 → 조우 → 탈출 → 결과)의 각 단계가 이 축들에 하나씩 대응한다. 가장 무거운 리스크는 넷코드(서버권위 FoW)와 USP(장비=스킬의 재미)이며, 설계·구현 순서는 이 둘을 앞단에서 검증하도록 배치했다.

---

## Systems Enumeration

| # | System Name | Category | Priority | Status | Design Doc | Depends On |
|---|-------------|----------|----------|--------|------------|------------|
| 1 | 입력 시스템 (Enhanced Input) | Core | MVP | Designed | design/gdd/input-system.md | — |
| 2 | 이동 & 카메라 (탑다운) | Core | MVP | Designed | design/gdd/movement-camera.md | 입력 |
| 3 | 전투 시스템 (평타·데미지·체력·상태이상·히트판정) | Gameplay | MVP | Designed | design/gdd/combat-system.md | 이동, (넷코드) |
| 4 | 아이템/전리품 시스템 (아이템 DB·드롭 테이블·희귀도) | Economy | MVP | Not Started | — | 세이브 |
| 5 | 인벤토리/장비 슬롯 (inferred) | Gameplay | MVP | Not Started | — | 아이템, 세이브 |
| 6 | 스킬 시스템 (6키·3시전타입) | Gameplay | MVP | Not Started | — | 전투, 입력 |
| 7 | 장비=스킬 시스템 ⭐USP | Gameplay | MVP | Not Started | — | 스킬, 인벤토리, 아이템 |
| 8 | 클래스리스 빌드/계열·스탯 | Progression | MVP | Not Started | — | 장비=스킬, 아이템 |
| 9 | 적 AI/몬스터 시스템 (inferred) | Gameplay | MVP | Not Started | — | 전투, 이동 |
| 10 | HUD (체력·스킬·미니맵·FoW렌더·데미지·마킹) (inferred) | UI | MVP | Not Started | — | 전투, 시야, 이동 |
| 11 | 네트워킹 레이어 (Dedicated Server·리플리케이션 백본 미확정: Iris vs RepGraph·서버권위) (inferred) | Core (Netcode) | Vertical Slice | Not Started | — | — |
| 12 | 세이브/영속성 (캐릭터·창고·계정레이어·시즌레이어) (inferred) | Persistence | Vertical Slice | Not Started | — | — |
| 13 | 시야/릴러번시 시스템 (전장의 안개·부시·서버권위 가시성) | Gameplay (Netcode) | Vertical Slice | Not Started | — | 네트워킹 |
| 14 | 매치메이킹/인스턴스 관리 (구역당 12~16인) (inferred) | Core (Netcode) | Vertical Slice | Not Started | — | 네트워킹 |
| 15 | 성장 시스템 (레벨18·경험치·스킬포인트) | Progression | Vertical Slice | Not Started | — | 전투, 세이브 |
| 16 | 세트 효과 시스템 (계열 세트 + 성기사 시그니처) | Progression | Vertical Slice | Not Started | — | 계열·스탯, 장비=스킬 |
| 17 | 파티/협동/배신 시스템 (노 프렌들리파이어·로컬 낙인) | Gameplay (Social) | Vertical Slice | Not Started | — | 네트워킹, 전투 |
| 18 | 던전/레벨 레이아웃 (심리스 월드·솔로레인·석탑·POI) (inferred) | Gameplay (World) | Vertical Slice | Not Started | — | 시야, 네트워킹 |
| 19 | 사망/로스트 시스템 (가방드랍·부분사망·구출비·보험) | Gameplay | Vertical Slice | Not Started | — | 인벤토리, 성장, 경제 |
| 20 | 탈출(익스트랙션) 시스템 (마킹·석탑·3분 수성전) | Gameplay | Vertical Slice | Not Started | — | 파티, 시야, 던전 |
| 21 | 고독한 길 (솔로 콘텐츠·자급자족 빌드) | Gameplay | Vertical Slice | Not Started | — | 던전, 파티, 세트, 시야 |
| 22 | 경제/길드 마켓 (골드·매입/재판매·싱크) | Economy | Vertical Slice | Not Started | — | 아이템, 세이브 |
| 23 | 베이스캠프 (창고·팀결성·안전지대) | Economy (Hub) | Vertical Slice | Not Started | — | 세이브, 경제, 파티 |
| 24 | 게임플레이 UI 묶음 (파티·인벤·상점·탈출·래더) (inferred) | UI | Vertical Slice | Not Started | — | 인벤토리, 파티, 경제, 탈출 |
| 25 | 안티치트 (서버권위 강제·기초) (inferred) | Core (Security) | Vertical Slice | Not Started | — | 네트워킹, 시야 |
| 26 | 구역 시스템 (4구역 레벨대 매칭 분리) | Progression | Alpha | Not Started | — | 성장, 매치메이킹 |
| 27 | 시즌 & 프레스티지 (인구순환·래더·하이브리드 리셋) | Meta | Alpha | Not Started | — | 세이브, 성장, 경제 |
| 28 | 코스메틱/프레스티지 영속 (계정레이어) (inferred) | Meta | Full Vision | Not Started | — | 시즌, 세이브 |
| 29 | 튜토리얼/온보딩 (inferred) | Meta | Full Vision | Not Started | — | (다수 코어 시스템) |

> **참고 — 점진 구현 시스템**: `아이템/전리품`·`인벤토리`·`클래스리스 계열·스탯`·`경제`·`고독한 길`·`안티치트`는 위 티어에서 **최소 버전**으로 시작하고 **Alpha에서 완성**한다. 표의 Priority는 "1차 필요 티어".
>
> **제거/변경 이력 (2026-07-18)**: `바운티/평판` 시스템 제거(전역 배신자 추적 → 로컬 낙인 철학과 불일치). `배신 낙인`은 별도 시스템이 아니라 `파티/협동/배신` 시스템의 **로컬 속성**(배신당한 파티원에게만 표시)으로 흡수. `관심관리/릴러번시`는 `시야` 시스템에 병합.

---

## Categories

| Category | Description | 이 게임의 시스템 |
|----------|-------------|-----------------|
| **Core** | 모든 것이 의존하는 기반 | 입력, 이동&카메라, 네트워킹, 매치메이킹, 안티치트 |
| **Gameplay** | 재미를 만드는 시스템 | 전투, 스킬, 장비=스킬, 시야, 인벤토리, 적AI, 던전, 파티/배신, 사망, 탈출, 고독한 길 |
| **Progression** | 성장 | 클래스리스 계열·스탯, 세트 효과, 성장, 구역 시스템 |
| **Economy** | 자원 생성·소비 | 아이템/전리품, 경제/길드마켓, 베이스캠프 |
| **Persistence** | 저장·연속성 | 세이브/영속성 |
| **UI** | 플레이어 대면 정보 | HUD, 게임플레이 UI 묶음 |
| **Meta** | 코어 루프 밖 | 시즌 & 프레스티지, 코스메틱/프레스티지 영속, 튜토리얼/온보딩 |

---

## Priority Tiers

| Tier | 정의 | 목표 마일스톤 | 설계 긴급도 |
|------|------|---------------|-------------|
| **MVP** | 코어 루프가 작동하는 최소 — "조작이 재밌나?"를 **싱글로** 검증 | 첫 플레이 가능 프로토타입 (로드맵 Phase 1) | 설계 FIRST |
| **Vertical Slice** | 네트워크 1존에서 전체 루프 완주 — "루프 전체가 재밌나?" | 버티컬 슬라이스 (로드맵 P2~3) | 설계 SECOND |
| **Alpha** | 전 기능 러프 완성 (4존·시즌 등) | 알파 (로드맵 P4) | 설계 THIRD |
| **Full Vision** | 폴리시·라이브옵스·엣지케이스 | 베타/출시 (로드맵 P5~6) | 필요 시 설계 |

---

## Dependency Map

### Foundation Layer (의존 없음)

1. **네트워킹 레이어** — 서버권위 멀티의 근간. 거의 모든 시스템이 의존 (최고 병목)
2. **입력 시스템** — 6키+우클릭+3시전타입, 조작 정체성의 출발점
3. **세이브/영속성** — 캐릭터·창고·계정/시즌 레이어 저장 인프라

### Core Layer (Foundation 의존)

1. **이동 & 카메라** — depends on: 입력, 네트워킹
2. **시야/릴러번시** — depends on: 네트워킹 (서버권위 가시성)
3. **전투 시스템** — depends on: 이동, 네트워킹
4. **아이템/전리품** — depends on: 세이브
5. **인벤토리/장비 슬롯** — depends on: 아이템, 세이브
6. **매치메이킹/인스턴스** — depends on: 네트워킹

### Feature Layer (Core 의존)

1. **스킬 시스템** — depends on: 전투, 입력
2. **장비=스킬 시스템** ⭐ — depends on: 스킬, 인벤토리, 아이템 (USP 병목)
3. **클래스리스 계열·스탯** — depends on: 장비=스킬, 아이템
4. **세트 효과** — depends on: 계열·스탯, 장비=스킬
5. **성장 시스템** — depends on: 전투, 세이브
6. **적 AI/몬스터** — depends on: 전투, 이동
7. **던전/레벨 레이아웃** — depends on: 시야, 네트워킹
8. **파티/협동/배신** — depends on: 네트워킹, 전투
9. **사망/로스트** — depends on: 인벤토리, 성장, 경제
10. **구역 시스템** — depends on: 성장, 매치메이킹

### Higher Feature / Social Layer

1. **탈출(익스트랙션)** — depends on: 파티, 시야, 던전
2. **고독한 길(솔로)** — depends on: 던전, 파티, 세트, 시야
3. **경제/길드 마켓** — depends on: 아이템, 세이브
4. **베이스캠프** — depends on: 세이브, 경제, 파티
5. **시즌 & 프레스티지** — depends on: 세이브, 성장, 경제

### Presentation Layer

1. **HUD** — depends on: 전투, 시야, 이동
2. **게임플레이 UI 묶음** — depends on: 인벤토리, 파티, 경제, 탈출, 시즌

### Polish Layer

1. **안티치트** — depends on: 네트워킹, 시야 (상시 개선)
2. **코스메틱/프레스티지 영속** — depends on: 시즌, 세이브
3. **튜토리얼/온보딩** — depends on: 다수 코어 시스템

---

## Recommended Design Order

| Order | System | Priority | Layer | Agent(s) | Est. Effort |
|-------|--------|----------|-------|----------|-------------|
| 1 | 입력 시스템 | MVP | Foundation | game-designer, unreal-specialist | S |
| 2 | 이동 & 카메라 | MVP | Core | game-designer, unreal-specialist | S |
| 3 | 전투 시스템 | MVP | Core | systems-designer, game-designer | L |
| 4 | 아이템/전리품 | MVP | Core | economy-designer, systems-designer | M |
| 5 | 인벤토리/장비 슬롯 | MVP | Core | systems-designer | M |
| 6 | 스킬 시스템 | MVP | Feature | systems-designer, ue-gas-specialist | L |
| 7 | 장비=스킬 시스템 ⭐ | MVP | Feature | game-designer, systems-designer | L |
| 8 | 클래스리스 계열·스탯 | MVP | Feature | systems-designer, economy-designer | M |
| 9 | 적 AI/몬스터 | MVP | Feature | ai-programmer, game-designer | M |
| 10 | HUD (최소) | MVP | Presentation | ux-designer, ui-programmer | S |
| 11 | 네트워킹 레이어 | VS | Foundation | technical-director, ue-replication-specialist | L |
| 12 | 세이브/영속성 | VS | Foundation | systems-designer, technical-director | M |
| 13 | 시야/릴러번시 | VS | Core | ue-replication-specialist, technical-director | L |
| 14 | 매치메이킹/인스턴스 | VS | Core | network-programmer | M |
| 15 | 성장 시스템 | VS | Feature | economy-designer, systems-designer | M |
| 16 | 세트 효과 | VS | Feature | systems-designer | M |
| 17 | 파티/협동/배신 | VS | Feature | game-designer, ue-replication-specialist | M |
| 18 | 던전/레벨 레이아웃 | VS | Feature | level-designer | L |
| 19 | 사망/로스트 | VS | Feature | game-designer, economy-designer | M |
| 20 | 탈출(익스트랙션) | VS | Social | game-designer, level-designer | M |
| 21 | 고독한 길(솔로) | VS | Social | game-designer, level-designer | M |
| 22 | 경제/길드 마켓 | VS | Social | economy-designer | L |
| 23 | 베이스캠프 | VS | Social | game-designer, ux-designer | S |
| 24 | 게임플레이 UI 묶음 | VS | Presentation | ux-designer, ue-umg-specialist | L |
| 25 | 안티치트 (기초) | VS | Polish | security-engineer | M |
| 26 | 구역 시스템 (4존) | Alpha | Feature | game-designer, economy-designer | M |
| 27 | 시즌 & 프레스티지 | Alpha | Social | live-ops-designer, economy-designer | L |
| 28 | 코스메틱/프레스티지 영속 | Full Vision | Polish | systems-designer | S |
| 29 | 튜토리얼/온보딩 | Full Vision | Polish | ux-designer | M |

> Effort: S = 1 세션, M = 2~3 세션, L = 4+ 세션 (1 세션 = GDD 하나를 완성하는 집중 설계 대화).
> 같은 레이어의 독립 시스템은 병렬 설계 가능 (예: 4·5, 8·9).

---

## Circular Dependencies

- **None found** — 모든 의존이 단방향. (`사망→경제`, `파티→탈출`, `장비=스킬→스킬` 등)
- 주의: `시야`와 `네트워킹 릴러번시`는 강결합이라 **하나의 시스템으로 병합**해 순환 소지를 사전 제거함.

---

## High-Risk Systems

| System | Risk Type | Risk Description | Mitigation |
|--------|-----------|-----------------|------------|
| 네트워킹 레이어 | Technical | 심리스 12~16인 + 리플리케이션 백본. 인디 최대 리스크. **UE5.8: RepGraph deprecated, Iris 프로덕션 레디** — 백본 미확정(ADR-0001) | Phase 0-B PoC로 Iris vs RepGraph 실측 비교 후 채택, 안 되면 인스턴스형 재설계. 데디서버 = 소스엔진 빌드 필요 |
| 시야/릴러번시 | Technical | 서버권위 FoW = 넷코드에서 가장 어려움. 프레임 예산 폭식·안티치트 핵심 | Phase 0-B PoC (숨은 액터 미전송 검증, 5~10Hz 스태거링) |
| 장비=스킬 시스템 | Design | USP. 재미없으면 게임 정체성 붕괴 | 로드맵 Phase 1 싱글 전투 프로토타입에서 손맛 우선 검증 |
| 사망/로스트 | Design | feel-bad 리스크 — 페널티가 스릴이 아니라 이탈 유발일 수 있음 | 버티컬 슬라이스 플레이테스트로 실측, 부분사망/구출비 다이얼 튜닝 |
| 경제/길드 마켓 | Design/Scope | faucet/sink 불균형, NPC 아비트리지 익스플로잇 | 밸런스 모델링, 골드 스프레드/싱크 사전 설계 |
| 시즌 & 프레스티지 | Scope | 라이브옵스 복잡도, 리셋 범위 결정 난이도 | 세부는 live-ops 단계로 이연, 하이브리드 리셋 원칙만 선고정 |
| 파티/협동/배신 | Technical/Design | 서버권위 파티 상태·로컬 낙인, "farm 후 배신" 익스플로잇 | 서버권위 강제 + 협동 보너스 공동탈출 정산(밸런싱 검토) |

---

## Progress Tracker

| Metric | Count |
|--------|-------|
| Total systems identified | 29 |
| Design docs started | 1 |
| Design docs reviewed | 0 |
| Design docs approved | 0 |
| MVP systems designed | 1 / 10 |
| Vertical Slice systems designed | 0 / 15 |

---

## Next Steps

- [ ] 이 시스템 열거·의존성·우선순위 검토 및 승인 (완료 — 2026-07-18)
- [ ] MVP 티어부터 설계: `/design-system 입력 시스템` (또는 `/map-systems next`)
- [ ] 각 GDD 완성 후 `/design-review design/gdd/[system].md`
- [ ] 병행: 로드맵 Phase 0-B 넷코드 PoC (`/prototype`) — 최고위험 넷코드 실현성 조기 검증
- [ ] MVP GDD 완성 시 `/gate-check pre-production`
