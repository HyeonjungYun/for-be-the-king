# 입력 시스템 (Input System)

> **Status**: Designed (pending /design-review)
> **Author**: guswnd9971 + game-designer/ux-designer agents
> **Last Updated**: 2026-07-18
> **Implements Pillar**: LoL식 탑다운 조작 (우클릭 평타 + 부위별 스킬) — USP 기반 계층

> **Quick reference** — Layer: `Foundation` · Priority: `MVP` · Key deps: `None (기반)`

## Overview

입력 시스템은 플레이어의 물리적 입력(키보드 WASD · 마우스 좌/우클릭 · 6개 스킬키 `Shift`/`Q`/`E`/`R`/`Z`/`X`)을 게임의 추상 입력 액션으로 변환하고, 이동·전투·스킬 시스템이 소비할 표준 신호로 제공하는 **기반 계층**이다. 단순 키 매핑을 넘어, 이 게임의 조작 정체성인 **3가지 스킬 시전 방식**(버프형 = 키를 떼는 순간 발동 / 타겟형 = 키 홀드 + 좌클릭으로 대상 지정 / 논타겟 범위기 = 키 홀드로 범위 표시 후 떼는 순간 시전)을 입력 레이어에서 **press·hold·release 상태로 구분·처리**한다. UE 5.8 **Enhanced Input**을 토대로 런타임 리바인딩과 컨텍스트 전환(전투 중 vs 베이스캠프/UI)을 지원하며, 키보드/마우스를 1차·게임패드를 2차로 다룬다. 플레이어는 이 시스템을 직접 "보지" 않지만 **"내가 의도한 대로 캐릭터가 즉각 반응한다"는 조작의 즉각성·정확성**으로 그 존재를 느낀다 — USP인 LoL식 탑다운 조작감은 입력 레이어의 정밀함을 전제로 하기 때문이다. 이 시스템이 없으면 어떤 시스템도 플레이어 의도를 받을 수 없다.

## Player Fantasy

**"내 손이 곧 캐릭터다."** 키를 누른 그 순간, 지연이나 씹힘 없이 캐릭터가 정확히 내 의도대로 반응한다. 플레이어는 입력 시스템을 *의식*하지 않지만, 조작이 손에 착 붙는 그 감각을 통해 **자기효능감**을 느낀다 — "내가 잘 눌렀다"는 확신.

특히 이 게임의 3가지 시전 방식은 각각 **다른 촉각적 리듬**을 준다:
- **버프형** — 키를 "탁" 떼서 즉발하는 경쾌한 쾌감
- **타겟형** — 키를 홀드한 채 좌클릭으로 "딱" 조준하는 정밀함
- **논타겟 범위기** — 범위를 신중히 겨냥하다 떼는 순간의 **결단**

6개 스킬키(`Shift`/`Q`/`E`/`R`/`Z`/`X`)가 손가락에 각인되어, 나중엔 화면을 보며 무의식적으로 콤보를 흘려보내는 **마스터리감**이 궁극의 목표.

**레퍼런스**: League of Legends의 스킬샷 — 논타겟기를 조준했다 꽂았을 때의 손맛, 고정 키셋을 수백 판 반복해 체득하는 숙련의 쾌감.
**안티 레퍼런스**: 입력이 씹히거나(drop), 눌렀는데 반응이 늦거나(latency), 의도와 다른 스킬이 나가는 **"미끄럽고 뭉개지는"** 느낌. 절대 이래선 안 됨.

## Detailed Design

### Core Rules

**1. 입력 아키텍처** — 모든 입력은 UE 5.8 **Enhanced Input**으로 처리(레거시 금지). 물리 입력 → **Input Action(IA)** → 게임 시스템. 입력 시스템은 IA 신호 생성·라우팅까지 책임지고, 신호의 *의미 해석*(이동/공격/시전)은 소비 시스템이 담당한다.

**2. Input Action 목록**

| IA | Value Type | 기본 바인딩 | 소비 시스템 |
|----|-----------|-------------|-------------|
| `IA_Move` | Axis2D | W/A/S/D | 이동&카메라 |
| `IA_BasicAttack` | Digital | 마우스 우클릭(RMB) | 전투 |
| `IA_Skill_1`~`IA_Skill_6` | Digital | `Shift`/`Q`/`E`/`R`/`Z`/`X` | 스킬 |
| `IA_ConfirmTarget` | Digital | 마우스 좌클릭(LMB) | 스킬(대상 지정), UI |
| `IA_Interact` | Digital | `F`(기본) | 상호작용(석탑·상점) |

**3. 이동 입력** — `IA_Move`(Axis2D)를 매 틱 방출, WASD를 8방향 벡터로 합성. 입력 시스템은 **raw 벡터만** 제공(정규화·가속은 이동 시스템 소관).

**4. 기본 공격(우클릭)** — RMB 입력 시 **커서 아래에 공격 가능 오브젝트가 있으면** 그 대상 기본공격 명령을 전투 시스템에 전달. 대상이 없으면(땅·비공격 오브젝트) **무동작**. 이동에는 사용 안 함. 커서→월드 판정은 입력 시스템의 **커서 타게팅 서비스**(`GetHitResultUnderCursor`)가 제공.

**5. 스킬 입력(6키·3시전 방식)** — 입력 시스템은 각 스킬키의 press/hold/release + 커서 정보를 **이벤트로 방출**하고, *장착 스킬의 유형 해석*은 스킬 시스템이 수행한다. 방출 이벤트:
- `OnSkillPressed(slot)` / `OnSkillHeld(slot, heldTime, cursorWorldPos)` / `OnSkillReleased(slot, cursorWorldPos)` / `OnConfirmTarget(slot, cursorTarget)`

| 유형 | 입력 흐름 (스킬 시스템이 구현) |
|------|-------------------------------|
| 버프형 | Pressed → Held → **Released 시 시전** |
| 타겟형 | Pressed → Held(대기) → **LMB 시 대상 지정 시전** / LMB 없이 Released면 **취소** |
| 논타겟 범위기 | Pressed → Held(HUD가 범위 표시) → **Released 시 커서 방향/위치로 시전** |

**6. 스킬키 배타성** — 동시에 하나의 스킬키만 "홀드(조준)" 상태. 홀드 중 다른 스킬키를 누르면 기존 홀드는 **취소되고 새 키로 전환**(기본값; 큐잉 여부는 Tuning).

**7. 입력 컨텍스트(IMC)**

| IMC | 활성 상황 | 활성 액션 |
|-----|-----------|-----------|
| `IMC_Combat` | 던전 플레이 | 전부 |
| `IMC_Basecamp` | 베이스캠프(안전지대) | 이동·상호작용 (전투/스킬 비활성) |
| `IMC_Menu` | 메뉴/인벤토리 | UI 네비게이션 (게임플레이 입력 억제) |

컨텍스트 전환 시 진행 중 스킬 홀드는 취소.

**8. 리바인딩** — 모든 게임플레이 IA는 런타임 리바인딩 가능(`PlayerMappableKeySlot`). 키보드/마우스 1차, 게임패드 2차(부분) — IMC 매핑 추가로 지원, 스틱 조준 방식은 후속(❓).

**9. 서버 권위 경계** — 입력 시스템은 **로컬 입력 캡처만** 담당. 이동/공격/시전의 권위 검증은 네트워킹·전투·스킬 시스템이 서버에서 수행(입력 좌표·타임스탬프는 서버 재검증 대상). 입력 레이어는 게임 상태를 확정하지 않는다.

### States and Transitions

입력 시스템이 소유하는 상태는 **스킬 슬롯별 입력 상태**(6슬롯 독립, §6 배타성 적용). 시전 유형별 분기는 스킬 시스템이 이 이벤트를 받아 자체 상태머신으로 처리(경계: **입력=신호, 스킬=해석**).

| State | Entry Condition | Exit Condition | Behavior |
|-------|----------------|----------------|----------|
| `Idle` | 기본 / Released 직후 | 스킬키 Pressed | 대기, 이벤트 없음 |
| `Held` | 스킬키 Pressed | 스킬키 Released / 컨텍스트 전환 / 다른 스킬키 Pressed | 매 틱 `OnSkillHeld` 방출, 커서 월드좌표 갱신, (논타겟) HUD 범위 표시 트리거 |

- `Held` → `Idle` 전환 시: `OnSkillReleased` 1회 방출. 타겟형은 그 전에 LMB로 `OnConfirmTarget`을 방출하고, LMB 없이 Released면 취소(시전 이벤트 없음).
- 컨텍스트 전환·다른 스킬키 Pressed로 인한 `Held` 이탈은 **취소**로 처리(시전 이벤트 없음).

### Interactions with Other Systems

| System | Direction | Interface (in/out) | 소유 경계 |
|--------|-----------|--------------------|-----------|
| 이동 & 카메라 | ← depends on 입력 | `IA_Move` raw 벡터 out; 커서 월드좌표 쿼리 제공 | 입력=raw, 이동=변환 |
| 전투 시스템 | ← depends on 입력 | 기본공격 명령(RMB+타겟) out | 입력=캡처, 전투=판정 |
| 스킬 시스템 | ← depends on 입력 | 슬롯 이벤트(Pressed/Held/Released/ConfirmTarget)+커서 out | 입력=신호, 스킬=시전 |
| HUD/UI | ← depends on 입력 | 논타겟 홀드 시 "범위 표시 요청", 컨텍스트 전환 알림 | 입력=트리거, HUD=렌더 |
| 네트워킹 | 입력 → 네트워킹 | 로컬 입력 신호를 서버 전송용으로 전달 (권위는 서버) | 네트워킹=권위 검증 |
| 상호작용(탈출/상점) | ← depends on 입력 | `IA_Interact` out | 입력=캡처 |

## Formulas

### 1. 타겟 판정 관용 반경 (Target Acquisition Leniency)

RMB 기본공격/타겟형 LMB 확정 시, 커서가 히트박스를 정확히 안 찍어도 판정된다. 획득 가능한 대상 중 커서에 **가장 가까운** 것을 선택.

`acquirable = (cursor_to_target_dist ≤ target_screen_radius + base_leniency)`

| 변수 | 타입 | 범위 | 설명 |
|------|------|------|------|
| `base_leniency` | float | 0–60 px | 히트박스 밖 추가 관용 반경 |
| `target_screen_radius` | float | — | 대상 히트박스의 스크린 투영 반경 |
| `cursor_to_target_dist` | float | ≥0 px | 커서~대상중심 스크린 거리 |

**출력**: bool(획득 가능) + 최근접 선택. **예시**: `target_screen_radius`=25px, `base_leniency`=30px → 커서가 대상 중심 55px 이내면 선택 가능.

### 2. 입력 버퍼 윈도우 (Input Buffer Window)

캐스팅/락 중 들어온 입력을 짧게 큐잉했다가 락 해제 시 실행 → "입력 씹힘" 제거. 입력 레이어는 **윈도우 길이·버퍼링 계약**만 정의하고, 실제 소비는 전투/스킬 시스템이 수행.

`buffered = 0 ≤ (t_state_clear − t_input) ≤ buffer_window`

| 변수 | 타입 | 범위 | 설명 |
|------|------|------|------|
| `buffer_window` | float | 100–250 ms | 락 해제 전 입력을 유효 큐잉하는 시간 |
| `t_input` | ms | — | 입력이 들어온 시각 |
| `t_state_clear` | ms | — | 액션 가능 상태로 전환되는 시각 |

**출력**: bool(버퍼 or 드롭). 슬롯당 **최신 1개만** 유지(스택 없음). **예시**: `buffer_window`=150ms, 캐스팅 종료 1000ms·스킬키 900ms 입력 → 100≤150 → 버퍼 후 1000ms 실행. 800ms(200ms 전)면 드롭.

### 3. 스킬 슬롯 전환 디바운스 (Slot Switch Debounce)

스킬 홀드 중 다른 키 입력 시 전환하되, 물리적 거의-동시 입력에 의한 오취소를 방지.

`switch_accepted = (t_B_press − t_A_press) ≥ debounce_window`

| 변수 | 타입 | 범위 | 설명 |
|------|------|------|------|
| `debounce_window` | float | 20–60 ms | 슬롯 전환을 유효로 인정하는 최소 간격 |
| `t_A_press`, `t_B_press` | ms | — | 각 키 눌림 시각 |

**출력**: bool(전환 인정). **예시**: `debounce_window`=40ms, A=500ms·B=520ms(20ms 후) → 무시(A 홀드 유지). B=560ms(60ms 후) → 전환.

## Edge Cases

| 상황 | 처리 | 근거 |
|------|------|------|
| 스킬 홀드 중 컨텍스트 전환(메뉴/사망/텔레포트) | 홀드 즉시 취소, 시전 이벤트 없음, 자원·쿨다운 미소모 | 안전지대/메뉴 오발 방지 |
| 스킬 홀드 중 캐릭터 CC(스턴/기절) | 홀드 취소(시전 없음). CC 판정은 전투 시스템 소유 | 무력화 중 입력이 살아있으면 안 됨 |
| 두 스킬키 거의 동시 입력 | `debounce_window` 미만이면 두 번째 무시, 먼저 키가 Held (공식 3) | 물리 노이즈 오취소 방지 |
| 창 포커스 상실(alt-tab) 중 키 홀드 | 포커스 상실 시 모든 홀드 강제 release=취소 | release 유실로 인한 stuck key 방지 |
| 논타겟 스킬을 tap(즉시 뗌) | `heldTime`≈0으로 현재 커서 위치 즉시 시전 (v1: 최소 홀드 게이트 없음) | 빠른 시전 허용; 최소 홀드는 후속 |
| RMB 공격 대상이 명령 해석 전 사망/소멸 | 입력은 명령만 전달, 전투가 무효 대상 명령 폐기 | 입력은 상태 확정 안 함(§9 경계) |
| 커서가 화면 밖에서 조준 | 커서를 화면 경계로 clamp, 마지막 유효 월드좌표 사용 | 조준 좌표 무효화 방지 |
| 리바인딩으로 한 키에 2개 액션 충돌 | 리바인딩 UI가 충돌 감지·거부(기존 매핑 해제 요구) | 모호한 입력 방지 |
| 버퍼된 입력의 상태 해제가 `buffer_window` 초과 | 입력 드롭(공식 2) | 오래된 의도의 실행 방지 |

## Dependencies

| System | 방향 | 성격 |
|--------|------|------|
| *(없음)* | 입력이 의존하는 상위 | **없음** — Foundation 레이어, 어떤 시스템도 선행 요구 안 함 |
| 이동 & 카메라 | 이동&카메라 → 입력 | **하드** — `IA_Move` raw 벡터 + 커서 월드좌표 없이 이동 불가 |
| 전투 시스템 | 전투 → 입력 | **하드** — RMB 기본공격 명령 캡처 |
| 스킬 시스템 | 스킬 → 입력 | **하드** — 슬롯 이벤트(Pressed/Held/Released/ConfirmTarget) 없이 시전 트리거 불가 |
| 상호작용(탈출/상점) | 상호작용 → 입력 | **하드** — `IA_Interact` |
| HUD/UI | HUD → 입력 | **소프트** — 논타겟 범위 표시·컨텍스트 전환 알림 트리거 |
| 네트워킹 | 네트워킹 → 입력 | **소프트(MP 전용)** — 로컬 입력 신호를 서버 전송용으로 수신; 권위 검증은 네트워킹 |

> **양방향 정합성**: 위 의존 시스템들은 아직 미설계 — 각 GDD 작성 시 "depends on 입력 시스템"을 명시해야 함 (설계 순서상 입력이 먼저라 정상).

## Tuning Knobs

| 파라미터 | 기본값 | 안전 범위 | ↑ 높이면 | ↓ 낮추면 |
|----------|--------|-----------|----------|----------|
| `base_leniency` | 30 px | 0–60 px | 클릭 관대 / 너무 크면 엉뚱한 대상 선택 | 정밀 요구↑ / 0이면 픽셀퍼펙트 강요 |
| `buffer_window` | 150 ms | 100–250 ms | 씹힘 체감↓ / 너무 크면 의도 안 한 지연 실행 | 반응 정확 / 씹힘 체감↑ |
| `debounce_window` | 40 ms | 20–60 ms | 오취소 방지↑ / 너무 크면 빠른 전환 막힘 | 빠른 전환 허용 / 노이즈 오취소↑ |
| `min_hold_before_aim` | 0 ms | 0–120 ms | tap 오시전 방지 / 즉발감↓ | 0이면 tap 즉발 (v1 기본) |
| `skill_hold_queuing` | false | bool | 홀드 중 다음 키 큐잉 허용 | 즉시 전환(기본) |

> **상호작용 주의**: `buffer_window`는 전투/스킬의 캐스팅 락 길이와 함께 튜닝해야 의미 있음(락보다 지나치게 길면 안 됨). `base_leniency`는 대상 히트박스 크기와 연동 — 대상이 작을수록 관용 반경 체감이 커짐.

## Visual/Audio Requirements

입력 시스템 자체는 시청각 자산을 **소유하지 않는다**. 입력에 대한 피드백은 각 소비 시스템이 소유하며, 입력 레이어는 이를 트리거하는 신호만 제공한다.

| 이벤트 | 피드백 | 소유 |
|--------|--------|------|
| 논타겟 홀드 | 범위 인디케이터(본인에게만) | HUD |
| 타겟 획득 | 대상 하이라이트 | HUD/전투 |
| 기본공격/시전 | 클릭·시전 사운드 | 전투/스킬 |

> 입력 GDD에는 별도 에셋 스펙이 없으므로 `/asset-spec` 대상 아님.

## UI Requirements

입력 시스템은 UI를 직접 그리지 않지만, 두 UI 요소를 요구한다(구현은 UI 시스템 소관).

| 정보 | 위치 | 갱신 | 조건 |
|------|------|------|------|
| 키 리바인딩 설정(충돌 감지 포함) | 설정 메뉴 | 변경 시 | 모든 게임플레이 IA 매핑 표시·재할당 |
| 현재 입력 컨텍스트/버튼 프롬프트 | HUD 코너(선택) | 컨텍스트/장치 전환 시 | 키보드↔게임패드 프롬프트 스왑 |

> **📌 UX Flag — 입력 시스템**: 리바인딩 설정 화면은 Pre-Production에서 `/ux-design`으로 UX 스펙을 작성해야 함. 스토리는 GDD가 아니라 `design/ux/[screen].md`를 참조. systems-index에 반영 권장.

## Acceptance Criteria

> **범위 주의**: 스킬 시스템이 아직 미설계이므로 시전 관련 기준은 **입력 시스템의 "이벤트 방출 계약"까지** 검증한다(실제 시전·자원 소모는 스킬 GDD 완료 후 통합 테스트). 설계 순서상 정상.

1. **GIVEN** 커서 아래 공격 가능 대상, **WHEN** RMB, **THEN** 그 대상 기본공격 명령이 전투에 정확히 1회 전달.
2. **GIVEN** 커서 아래 땅/비공격 오브젝트, **WHEN** RMB, **THEN** 무동작(명령·이벤트 없음).
3. **GIVEN** `target_screen_radius`=25·`base_leniency`=30, **WHEN** 커서~대상중심 ≤55px에서 확정, **THEN** 대상 선택; >55px면 미선택; 겹치면 최근접 1개만.
4. **GIVEN** 슬롯 `Idle`, **WHEN** 스킬키 Pressed, **THEN** `OnSkillPressed` 1회 방출 + `Held` 전이.
5. **GIVEN** 슬롯 `Held`, **WHEN** Released, **THEN** `OnSkillReleased` 1회 방출 + `Idle` 복귀.
6. **GIVEN** 타겟형 `Held`, **WHEN** LMB, **THEN** `OnConfirmTarget` 방출; LMB 없이 Released면 `OnConfirmTarget` 없이 취소.
7. **GIVEN** 논타겟 `Held`, **WHEN** 매 틱, **THEN** HUD 범위 표시 트리거 + `OnSkillHeld`(`heldTime` 단조 증가).
8. **GIVEN** `debounce_window`=40ms·A=500ms, **WHEN** B=520ms, **THEN** B 무시(A 유지); **WHEN** B=560ms, **THEN** A 취소·B 전환.
9. **GIVEN** `buffer_window`=150ms·클리어=1000ms, **WHEN** 입력 900ms, **THEN** 버퍼 후 1000ms 실행신호; 800ms면 드롭. 슬롯당 최신 1개만 유지.
10. **GIVEN** `IMC_Combat`·슬롯 `Held`, **WHEN** `IMC_Basecamp`/`Menu` 전환, **THEN** 홀드 즉시 취소(자원·쿨다운 미소모).
11. **GIVEN** `IMC_Menu` 활성, **WHEN** WASD/RMB/스킬키, **THEN** 게임플레이 입력 억제·UI 네비게이션만 반응.
12. **GIVEN** 스킬키 홀드 중, **WHEN** 창 포커스 상실(alt-tab), **THEN** 모든 홀드 강제 취소(포커스 복귀 후 stuck key 없음).
13. **GIVEN** 커서가 화면 밖, **WHEN** 좌표 쿼리, **THEN** 경계로 clamp된 값 반환·마지막 유효 월드좌표 유지(NaN 없음).
14. **성능**: 물리 입력 → IA 콜백 도달 지연 **≤1프레임(16.6ms)**, 100샘플 **P95** 기준(Unreal Insights로 raw HW 타임스탬프 vs IA 콜백 비교).
15. **하드코딩 없음**: 모든 튜닝 노브(`base_leniency`/`buffer_window`/`debounce_window`/`min_hold_before_aim`/`skill_hold_queuing`)는 외부 config에서 로드.


## Open Questions

| 질문 | 소유 | 목표 시점 | 현재 상태 |
|------|------|-----------|-----------|
| 게임패드 스틱 조준 방식(타겟/논타겟 조준) | ux-designer + 게임패드 | 게임패드 지원 확정 시 | 미정 (❓, §C-8) |
| `skill_hold_queuing`=true 시 상세 동작 명세 | game-designer | 큐잉 도입 결정 시 | Detailed Rules에 미명세 |
| `min_hold_before_aim` 비0 값 도입 여부(tap 오시전 방지) | game-designer | 플레이테스트 후 | v1 기본 0 |
| 입력 지연 임계값(≤1프레임) 확정 및 측정법 | technical-director | Phase 0-B/구현 | 제안값(Unreal Insights) |
| 시전 결과(자원·쿨다운) 통합 테스트 | qa-lead | 스킬 시스템 GDD 완료 후 | 스킬 미설계로 보류 |
