# 전투 시스템 (Combat System)

> **Status**: In Design
> **Author**: guswnd9971 + game-designer/systems-designer/qa-lead agents
> **Last Updated**: 2026-07-18
> **Implements Pillar**: LoL식 탑다운 전투 조작 (우클릭 평타 + 서버권위 히트판정) — USP 코어
> **Engine Framing**: UE 5.8 · GAS(AttributeSet/GameplayEffect/GameplayTag) 유력 · Chaos 히트판정 · 서버권위(ADR-0001, R-GAS 교차)
> **Scope**: 전투 primitive — 평타·데미지·체력·상태이상(CC)·히트판정. 6키 스킬 자체는 스킬 시스템(#6) 소유(이 primitive 사용).

> **Quick reference** — Layer: `Gameplay` · Priority: `MVP` · Key deps: `이동&카메라, 입력, 넷코드`

## Overview

전투 시스템은 이 게임의 전투 primitive — **평타(우클릭)·데미지 적용·체력/방어 어트리뷰트·상태이상(CC)·서버권위 히트판정** — 을 제공하는 Gameplay 코어다. 6키 스킬(스킬 시스템 #6 소유)도 자신의 효과를 이 primitive를 통해 해소한다(전투=데미지/체력/CC의 **단일 권위**). 평타는 커서 아래 유효 대상(입력의 `target_acquisition_leniency` 30px 관용)에 우클릭 시 장착 무기 기본공격으로 발동하며, 데미지는 공격력·방어력 어트리뷰트로 계산되고 체력 소진 시 사망(사망/루팅 시스템으로 위임)한다. CC(스턴·루트·둔화)는 상태이상으로 적용되고, 전투가 **CC 권위**를 소유해 이동 시스템에 `can_move`/`can_turn`을 공급한다. 모든 히트판정은 **서버권위**(Chaos 트레이스/오버랩)로 캐스트 창·좌표를 서버가 재검증한다(안티치트, ADR-0001) — 특히 체력/데미지/CC를 GAS 어트리뷰트·GameplayEffect로 구현하면 **ADR-0001의 R-GAS(Iris 호환)**에 직결되어 넷코드 PoC 검증 대상이 된다. 클래스가 아니라 **장비 스탯이 전투를 규정**(장비=스킬)한다. 플레이어는 우클릭 평타의 타격감과, 풀루팅 PvPvE에서 매 순간 체력을 관리하는 긴장을 직접 체감한다. 이 primitive가 없으면 스킬·적AI·사망 등 어떤 전투 파생도 성립하지 않는다.

## Player Fantasy

**"한 방 한 방이 판돈이다."** 전투의 판타지는 **대가가 걸린 교전의 긴장**이다. 풀루팅이라 지면 반입물 전부를 잃는다 — 그래서 모든 평타, 모든 체력 한 칸이 무겁다. 플레이어는 우클릭으로 정확히 대상을 물고, 체력을 자원처럼 아끼며, 상대의 스킬을 읽고 받아친다. LoL식 클릭 전투의 **정확성**(내가 노린 대상에 정확히 꽂힌다)과 익스트랙션의 **판돈**(이기면 상대 것까지, 지면 내 것까지)이 겹쳐, 매 교전이 계산된 도박이 된다.

**레퍼런스**: League of Legends 교전의 read-and-punish 심리전, Dark and Darker의 "이 싸움을 걸까 말까"의 무게.
**안티 레퍼런스**: 평타가 대상에 안 꽂히는 부정확함, 체력이 순식간에 녹아 대응 불가한 즉사, 서버-클라 불일치로 "분명 피했는데 맞은" 억울함(넷코드 히트검증·R-GAS와 직결).

## Detailed Design

### Core Rules

**1. 평타(기본공격)** — RMB로 커서 아래 **적대 대상** 획득(입력 `target_acquisition_leniency` 30px 관용, 최근접 1개). 대상 락 → `attack_windup`(공격속도 기반) → **즉시 히트**(대상이 사거리·LoS 유효 시 데미지 적용, 투사체 없음). windup 중 대상이 사거리 이탈·LoS 상실·사망 시 **취소**. 이동 중 평타 허용(strafe 전투, 정지 강제 없음).

**2. 데미지 타입 2종** — `physical`/`magic`. 완화는 각각 `armor`/`magic_resist`. 무기가 평타의 데미지 타입 결정(마법사 무기=magic). 데미지 = 공격력 기반 − 완화(공식 D1).

**3. 치명타** — `crit_chance` 확률로 `crit_multiplier` 배 데미지(공식 D2). 확률·배율은 장비 어트리뷰트.

**4. 체력/사망** — `health` 어트리뷰트, 0 도달 시 **`OnDied(instigator, victim)` 이벤트 방출** → 사망/루팅 시스템이 소비. 전투는 "사망 확정"만 담당(가방 드랍·부분사망·구출은 사망 시스템 소관). downed/bleedout도 사망 시스템.

**5. 상태이상(CC)** — 전투가 **CC 권위** 소유. GameplayEffect+Tag로 적용, `duration`·중첩 규칙(공식 D4).

| CC | 이동 | 회전 | 평타 | 스킬 |
|----|------|------|------|------|
| `Stun` | ✗ | ✗ | ✗ | ✗ |
| `Root` | ✗ | ✓ | ✓ | ✓ |
| `Slow` | 감속(mult) | ✓ | ✓ | ✓ |
| `Silence` | ✓ | ✓ | ✓ | ✗ |

→ 이동에 `can_move`/`can_turn`, 스킬에 `can_cast` 공급.

**6. 히트판정(서버권위)** — 평타=타겟락 즉시(서버가 사거리·LoS 트레이스 재검증). 스킬샷 투사체·범위는 **스킬 시스템**이 전투의 `ApplyDamage(instigator, target, base, type, bCanCrit)` API를 호출해 해소. 서버가 캐스트 창·좌표·사거리 재검증(안티치트, ADR-0001).

**7. 공격속도** — `attack_speed`(회/초) 어트리뷰트가 `attack_windup`+후딜 결정(공식 D3).

**8. 팀/적대 판정** — 몹·비파티 플레이어=적(평타 가능), 파티원=아군(평타 불가, friendly fire 없음). **파티원 공격(배신)은 별도 배신 시스템 토글** — MVP 전투는 기본 아군 보호(❓ Open Q).

### States and Transitions

| State | Entry | Exit | Behavior |
|-------|-------|------|----------|
| `Alive/Idle` | 스폰 | 평타 입력 / 피격 / 사망 | 대기 |
| `Attacking` | RMB 대상 락 | windup 완료(히트) / 취소(대상 무효) | windup→즉시히트→후딜. 이동 병행 가능 |
| `Dead` | `health`=0 | (부활은 사망 시스템) | `OnDied` 방출, 입력·전투 비활성 |

> CC(Stun/Root/Slow/Silence)는 배타 상태가 아니라 **동시 적용 가능한 status 태그**(GameplayEffect). Stun은 `Attacking`을 즉시 취소.

### Interactions with Other Systems

| System | Direction | Interface (in/out) | 소유 경계 |
|--------|-----------|--------------------|-----------|
| 입력 | ← 전투 의존 | RMB 평타 명령 + 커서 대상(leniency 30px) in | 입력=캡처, 전투=판정 |
| 이동&카메라 | 전투 → 이동 | `can_move`/`can_turn`(CC) out | CC 권위=전투 |
| 스킬 | ↔ | `ApplyDamage(...)` API out(제공) + `can_cast` out; 스킬이 데미지/CC를 이 API로 해소 | primitive=전투, 어빌리티=스킬 |
| 장비/성장 | 장비 → 전투 | 어트리뷰트 공급(attack·armor·magic_resist·crit·attack_speed·max_health) | 스탯 소유=장비/성장 |
| 넷코드 | 전투 → 넷코드 | 서버 히트검증·relevancy, GAS 어트리뷰트가 R-GAS 대상 | 권위=서버(ADR-0001) |
| 사망/루팅 | 전투 → 사망 | `OnDied(instigator,victim)` out | 확정=전투, 결과=사망 |
| 적 AI | 적AI → 전투 | AI가 평타/스킬로 전투 primitive 사용 | AI 판단=적AI |
| HUD | 전투 → HUD | 체력·데미지 넘버·CC 아이콘 out | 전투=데이터, HUD=렌더 |

## Formulas

> **완화 모델 근거**: 플랫 차감(저스탯에 방어 무효 체감)·무보정%(스택 시 완전 무적) 양극단 회피 위해 **점감 곡선 `armor/(armor+K)`** 채택. 점당 방어 가치가 수확체감 → 방어 스노우볼 억제, `armor=0`도 데미지 정상 → 저항 무효 없음. 점근적으로만 1에 수렴(완전 무적 불가, 하드클램프 불필요).

### D1. `damage_dealt` — 최종 데미지 (⭐ 크로스시스템)

`mitigation_pct = defense_stat / (defense_stat + K_mitigation)`
`damage_dealt = base_damage × (1 − mitigation_pct)`

| 변수 | 타입 | 범위 | 설명 |
|------|------|------|------|
| `base_damage` | float | 0–∞ | 완화 전 데미지(physical→armor 참조, magic→magic_resist 참조) |
| `defense_stat` | float | 0–300(소프트) | physical=armor, magic=magic_resist. 장비/성장 공급 |
| `K_mitigation` | const | 50–150, 기본 **100** | 곡선 기울기. armor=K일 때 정확히 50% 감소 |
| `mitigation_pct` | float | [0, 1) | 점근 상한 — 100% 무적 불가 |

**출력 범위**: `(0, base_damage]`. **예시**: base=100·armor=100·K=100 → 50; armor=200 → 33.3; armor=0 → 100.

### D2. `crit_damage` — 치명타 (⭐ 크로스시스템)

크리는 **완화 이전(pre-mitigation)** 적용 → 크리빌드가 방어를 무시하지 못함.
`effective_base = base_damage × (crit_roll ? crit_multiplier : 1.0)`
`damage_dealt = effective_base × (1 − mitigation_pct)`

| 변수 | 타입 | 범위 | 설명 |
|------|------|------|------|
| `crit_chance` | float | 0–1 | 장비 어트리뷰트 |
| `crit_roll` | bool | {0,1} | `random(0,1) < crit_chance` |
| `crit_multiplier` | float | 1.0–3.0, 기본 **1.75** | 장비 어트리뷰트 |

**출력 범위**: `(0, base_damage×crit_multiplier]`. **예시**: base=100·armor=100(mit 0.5)·crit 0.25·mult 1.75 → 비크리 50, 크리 87.5.

### D3. `attack_windup / attack_interval` — 공격속도

공속 보너스는 **가산% 후 최종 1회 곱 + 캡**(effective_move_speed 패턴, 폭주·예측 일관성). windup 최소값 = `input_buffer_window`(150ms) 정합.
`effective_attack_speed = clamp(base_attack_speed×(1+Σpercent_as_bonus_i), as_min_ratio×base, as_max_ratio×base)`
`attack_interval = 1 / effective_attack_speed`
`attack_windup = max(windup_floor_ms, windup_ratio × attack_interval)` ; `attack_recovery = attack_interval − attack_windup`

| 변수 | 타입 | 범위 | 설명 |
|------|------|------|------|
| `base_attack_speed` | float | 0.5–2.0/s | 무기 정의 |
| `percent_as_bonus_i` | float | −1–+X | 장비/성장 가산 스택 |
| `as_min_ratio`/`as_max_ratio` | const | 0.5× / 2.5× | 하드캡 |
| `windup_ratio` | const | 0–1, 기본 0.5 | interval 중 히트 전 비중 |
| `windup_floor_ms` | const | 기본 150ms | `input_buffer_window`와 동일값(**PT 재검토**) |

**출력 범위**: interval ∈ [1/(2.5×base), 1/(0.5×base)]; windup ≥150ms. **예시**: base 1.0/s·+40% → 1.4/s → interval 0.714s → windup 357ms.

### D4. `cc_duration_dr` — CC 지속 + 점감(DR) (⭐ 크로스시스템)

체인스턴 락 방지 = PvP 공정성 핵심. **하드CC(Stun/Root/Silence)만 DR**, Slow는 D5 별도.
`DR_stack_count = min(recent_hardCC_hits_in_window, DR_max_stacks)`
`DR_multiplier = max(DR_floor, DR_factor ^ DR_stack_count)`
`effective_cc_duration = base_cc_duration × DR_multiplier`

| 변수 | 타입 | 범위 | 설명 |
|------|------|------|------|
| `base_cc_duration` | float | 0.5–3.0s | 스킬/평타원 정의 |
| `DR_factor` | const | 0–1, 기본 0.5 | 스택당 곱 감쇠 |
| `DR_max_stacks` | const | 기본 3 | 이후 바닥 유지 |
| `DR_floor` | const | 0–1, 기본 0.25 | 최소 지속비율 |
| `DR_window` | sec | 기본 15s(**PT**, 빠른교전 8–10s 검토) | 무피격 시 스택 리셋 |

**출력 범위**: [base×0.25, base]. **예시**: 스턴 1.5s → 1회 1.5s → 2회 0.75s → 3회 0.375s(바닥).

### D5. `slow_effect` — Slow CC (이동 공식 재사용, 신규 아님)

Slow는 신규 곱 레이어 없이 이동의 **등록된** `effective_move_speed` 공식의 `percent_ms_bonus_i`에 **음수항**으로 편입. 다중 Slow는 **비가산(최댓값 1개만)** — 가산 시 Slow만으로 Root급 고착이 나와 CC표(Root≠Slow)와 상충하므로 배제.
`slow_term = −max(slow_pct_i for active Slows, default 0)`
`percent_ms_bonus_total = Σ(장비/스킬 percent) + slow_term` → `effective_move_speed`(movement-camera.md 그대로)

| 변수 | 타입 | 범위 | 설명 |
|------|------|------|------|
| `slow_pct_i` | float | 0–1(전형 0.2–0.7) | CC 소스별 강도 |
| `slow_term` | float | [−1, 0] | 활성 Slow 최댓값만, 비가산 |

**출력 범위**: 이동 등록값과 동일 [300,900]cm/s — Slow 단독으론 하드클램프(0.5×)로 완전 고착 불가(고착=Root 전담). **예시**: base 600·장비+10%·Slow 40% → −0.3 → 420cm/s. Slow 40%+60% 동시 → max=0.6만 적용.

### D6. `effective_health / TTK` — 내부 튜닝 지표 (등록 불필요)

`effective_health = health / (1 − mitigation_pct)`
`dps_avg = base_damage×(1−mitigation_pct)×(1+crit_chance×(crit_multiplier−1)) × effective_attack_speed`
`TTK_avg = effective_health / dps_avg`

**목표 밴드**: 1v1 균형장비 TTK **4–8s**(**PT 확정**). **예시**: health 500·armor 50(mit 0.333)·base_dmg 60·crit 0.2/1.75·공속 1.2 → dps≈55.4 → TTK≈9.0s → ⚠️ 다소 느림, **base_damage 상향 또는 K_mitigation 하향 검토**(PT).

> **크로스시스템 등록 대상**: `damage_dealt`·`crit_damage`·`attack_speed_stacking`(D3 클램프)·`cc_duration_dr` → Phase 5 등록. `slow_effect`(D5)는 신규 아님 → 기존 `effective_move_speed`의 referenced_by에 combat 추가 + Slow 편입 노트.

## Edge Cases

| 상황 | 처리 | 근거 |
|------|------|------|
| 이미 사망한 대상에 데미지 | 무시(이중 사망·`OnDied` 재발 없음) | 사망은 종결 상태 |
| 동시 치사(서로 같은 틱에 죽임) | 양쪽 사망, 각 `OnDied` 발생, victim별 instigator 기록 | 상호 확정 |
| 오버킬(데미지 > 잔여 체력) | health 0으로 클램프, `OnDied` 1회 | 단일 사망 |
| 고방어로 데미지가 0에 수렴 | **최소 1 데미지**(`min_damage`) 바닥 적용 | 무한 교착(0딜) 방지 |
| 크리티컬이 base_damage=0에 적용 | 0(없는 것의 크리) | 곱연산 결과 0 |
| CC/힐을 사망 대상에 적용 | 무시(부활은 사망 시스템) | 전투에서 사망은 종결 |
| windup 중 **Stun** | 평타 취소(히트 없음, 쿨다운 미소모) | Stun은 행동 봉쇄(CC표) |
| windup 중 **Root** | 평타 지속(Root는 평타 허용) | CC표 Root≠Stun |
| windup 중 대상 사망/사거리이탈/LoS상실 | 평타 취소, 재타겟 필요 | 타겟락 유효성 상실 |
| Silence 중 평타 시도 | 평타 허용(Silence는 스킬만 봉쇄), 스킬은 차단 | CC표 |
| 다중 Slow 동시 | 최댓값 1개만(비가산), 이동 하드클램프 0.5× 하한(D5) | Slow 단독 완전고착 방지 |
| 하드CC 연쇄(체인) | DR 적용(D4), 바닥 25%까지 감쇠 | 체인스턴 락 방지 |
| 파티원 대상 평타 시도 | 유효 대상 미획득(friendly fire 없음) | 아군 보호(MVP) |
| CC 면역 대상(일부 보스 등) | 면역 태그가 CC 차단 | 태그 기반 면역 |
| instigator가 히트 확정 전 접속종료 | 서버가 데미지·`OnDied` 정상 처리, instigator=해당 id | 서버권위(ADR-0001) |
| 같은 프레임 다중 데미지원 | 서버 수신 순서로 순차 적용, health 순차 감소 | 서버 결정론 |

> `min_damage`(기본 1)는 Tuning Knobs에 추가. 면역 태그·다중원 순서는 넷코드 구현 시 서버 결정론으로 확정.

## Dependencies

| System | 방향 | 성격 | 인터페이스 |
|--------|------|------|-----------|
| 입력 | 전투 → 의존 | **하드** | RMB 평타 명령 + 커서 대상(`target_acquisition_leniency` 30px) |
| 이동 & 카메라 | 전투 ↔ 이동 | **하드** | 위치·facing in(히트판정·사거리); `can_move`/`can_turn`(CC) out |
| 넷코드 | 전투 → 넷코드 | **하드(MP)** | 서버 히트검증·relevancy, GAS 어트리뷰트가 R-GAS 대상(ADR-0001) |
| 스킬 | 스킬 → 전투 | 역방향(하드) | 스킬이 `ApplyDamage(...)` 호출·`can_cast` 소비 |
| 장비/성장 | 장비 → 전투 | 역방향(하드) | 어트리뷰트 공급(attack·armor·magic_resist·crit·attack_speed·max_health) |
| 사망/루팅 | 전투 → 사망 | 역방향(하드) | `OnDied(instigator, victim)` 공급 |
| 적 AI | 적AI → 전투 | 역방향(하드) | AI가 평타/스킬로 전투 primitive 사용 |
| HUD | HUD → 전투 | 소프트 | 체력·데미지 넘버·CC 아이콘 데이터 |
| 배신 시스템 | 배신 → 전투 | 역방향(소프트) | 파티원 공격 허용 토글(기본 아군 보호) |

> **양방향 정합성**: `movement-camera.md`는 이미 "전투 → 이동(can_move/can_turn 공급)"을 명시함. `input-system.md`도 "전투 → 입력(RMB 평타 캡처)" 명시함. 하위 의존(스킬·장비/성장·사망·적AI·HUD·배신)은 미설계 — 각 GDD 작성 시 "depends on 전투"를 명시해야 함.

## Tuning Knobs

| 파라미터 | 기본값 | 안전 범위 | ↑ 높이면 | ↓ 낮추면 |
|----------|--------|-----------|----------|----------|
| `K_mitigation` | 100 | 50–150 | 방어 가치↓·딜 잘 들어감·TTK↓ | 방어 가치↑·탱커 강세·TTK↑ |
| `crit_multiplier` | 1.75 | 1.0–3.0 | 크리빌드 폭딜·분산↑ | 크리 무의미 |
| `windup_ratio` | 0.5 | 0–1 | 느린 히트·카운터 여지↑ | 즉발·반응 어려움 |
| `windup_floor_ms` | 150 | 100–250 | 평타 하한 느림 | 초고속 평타(버퍼 충돌 위험) |
| `as_max_ratio` | 2.5× | 2.0–3.0 | 공속 상한↑·폭주 위험 | 공속 성장 제한 |
| `DR_factor` | 0.5 | 0.3–0.7 | CC 덜 감쇠·체인 강함 | 빨리 CC 무의미 |
| `DR_floor` | 0.25 | 0.1–0.4 | 연쇄 CC도 여전히 유효 | 연쇄 CC 무력화 |
| `DR_window` | 15s | 8–20s | 오래 DR 유지 | 빨리 리셋(체인 쉬움) |
| `min_damage` | 1 | 1–5 | 고방어도 확실히 아픔 | 0딜 교착 위험 |

> **상호작용 주의**: `K_mitigation`×`base_damage`가 실질 TTK 결정(목표 밴드 **4–8s**, D6). `crit_chance`(장비)×`crit_multiplier`가 폭딜 분산. `DR_factor`×`DR_floor`×`DR_window`가 체인CC 락 체감을 함께 결정 — 따로 튜닝 금물.
>
> **하드코딩 금지**: 모든 노브는 외부 config(GAS라면 GameplayEffect/DataTable/CurveTable)에서 로드. `crit_chance`·`attack_speed`·`armor` 등 개별 어트리뷰트는 장비/성장 시스템이 소유(여긴 전역 노브만).

## Visual/Audio Requirements

| 요소 | 요구 | 소유 |
|------|------|------|
| 히트 피드백 | 피격 시 히트 플래시·임팩트 VFX·히트스톱(선택)·타격 SFX — **타격감의 핵심** | 전투/VFX |
| 데미지 넘버 | 수치 팝업(크리 색·크기 강조), 힐 구분 | HUD |
| 데미지 타입 구분 | physical/magic 시각 구분(색/이펙트) | VFX |
| CC 시각 | Stun(기절)·Root(속박)·Slow(감속 오라)·Silence(침묵) VFX + 아이콘 | VFX/HUD |
| 평타 모션 | 무기별 windup→히트 애님(근접 스윙/원거리 발사 포즈) | 애니메이션 |
| 사망 연출 | 래그돌/디졸브는 **사망 시스템** 소유 | 사망 |
| 서버-클라 | 클라 예측 피드백 즉각(체감), 서버 불일치 보정 시 튐 최소화 | 넷코드/전투 |

> 📌 **Asset Spec**: 아트바이블 승인 후 `/asset-spec system:combat-system`로 히트 VFX·CC 이펙트 스펙 생성. `art-director` 협의는 아트바이블 확정 후(현재 lean·미작성으로 생략 — 프로덕션 전 필수).

## UI Requirements

| 정보 | 위치 | 갱신 | 조건 |
|------|------|------|------|
| 체력바(본인·대상) | HUD/헤드업 | 실시간 | 체력 변화 시 |
| 데미지 넘버 | 월드공간 팝업 | 히트 시 | 크리/힐 시각 구분 |
| CC 상태 아이콘 | 대상/본인 | CC 적용·해제 | 지속시간 표시 |

> 📌 **UX Flag — 전투 시스템**: 전투 HUD(체력바·데미지 넘버·CC 아이콘)는 Pre-Production에서 `/ux-design`으로 UX 스펙 작성 대상. 스토리는 `design/ux/[screen].md` 참조. systems-index 반영 권장.

## Acceptance Criteria

> `[통합-보류: X]` = 미설계 시스템 X 확정 전까지 스텁/목업 부분 검증만. 27건은 전투 단독 즉시 검증.

### 평타 (Rule 1)
1. **GIVEN** RMB 홀드·커서가 적 중심 30px 이내, **WHEN** RMB 클릭, **THEN** 해당 적 락온·평타 시퀀스 시작.
2. **GIVEN** windup 중 타겟이 사거리+LoS 유지, **WHEN** windup 경과, **THEN** 종료와 동시(지연 0) 데미지 판정.
3. **GIVEN** windup 중, **WHEN** 타겟 health=0, **THEN** 평타 취소·데미지 없음.
4. **GIVEN** windup 중, **WHEN** 타겟 사거리 이탈, **THEN** 평타 취소.
5. **GIVEN** windup 중, **WHEN** 타겟 LoS 상실(장애물), **THEN** 평타 취소.
6. **GIVEN** windup 중 이동 입력, **WHEN** 캐릭터 이동, **THEN** windup 미취소·정상 타격(strafe 전투).

### 데미지/크리 (Rule 2·3, D1·D2)
7. **GIVEN** 물리 base 100·armor 100·K 100, **WHEN** 적용, **THEN** 최종 50 (=100×(1−100/200)).
8. **GIVEN** 마법 base 100·magic_resist 100·K 100, **WHEN** 적용, **THEN** 최종 50(동일 공식, magic_resist).
9. **GIVEN** armor 0, **WHEN** base 100 적용, **THEN** 최종 100(무감쇄).
10. **GIVEN** armor 초고값, **WHEN** base 100 적용, **THEN** 계산값<1이어도 `min_damage`=1 고정.
11. **GIVEN** base 100·크리 발동·mult 1.75·armor 100, **WHEN** 계산, **THEN** effective_base 175 **선산출** 후 감쇄 → 최종 87.5(무크리 50 대비 증가).

### 공격속도 (D3)
12. **GIVEN** base_as 1.0·Σ%=0, **WHEN** 계산, **THEN** eff_as 1.0 → interval 1000ms → windup 500ms.
13. **GIVEN** eff_as가 2.5×base 초과, **WHEN** 계산, **THEN** 2.5×로 클램프.
14. **GIVEN** eff_as가 0.5×base 미만, **WHEN** 계산, **THEN** 0.5×로 클램프.
15. **GIVEN** eff_as 매우 높아 0.5×interval<150ms, **WHEN** 계산, **THEN** windup 150ms floor 고정.

### 사망 (Rule 4)
16. **[통합-보류: 사망]** **GIVEN** health 0 도달, **WHEN** 피해 직후, **THEN** `OnDied(instigator,victim)` 정확히 1회.
17. **[통합-보류: 사망]** **GIVEN** 잔여 10·피해 500, **WHEN** 적용, **THEN** health 음수 없이 0 클램프·`OnDied` 1회.
18. **[통합-보류: 사망]** **GIVEN** 이미 health 0, **WHEN** 추가 피해, **THEN** 무시(변화 없음).

### CC (Rule 5, D4·D5)
19. **GIVEN** Stun, **WHEN** 이동/회전/평타/스킬 입력, **THEN** 모두 차단(can_move/can_turn/can_cast=false).
20. **GIVEN** Root, **WHEN** 입력, **THEN** 이동만 차단·회전/평타/스킬 정상.
21. **GIVEN** base_move_speed 600·Slow X%, **WHEN** 계산, **THEN** 600×(1−X%)·300cm/s(0.5×) 미만 불가.
22. **[통합-보류: 스킬]** **GIVEN** Silence, **WHEN** 평타·스킬 입력, **THEN** 평타 정상·스킬 차단.
23. **GIVEN** windup 중, **WHEN** Stun 적용, **THEN** 평타 즉시 취소·쿨다운 미소모.
24. **GIVEN** windup 중, **WHEN** Root 적용, **THEN** windup 지속·정상 타격.
25. **GIVEN** 15초 내 하드CC 2회차 원지속 2.0s, **WHEN** DR 적용, **THEN** 0.5^1=0.5 → 1.0s.
26. **GIVEN** 15초 내 하드CC 3회+, **WHEN** DR 적용, **THEN** mult 0.25 미만 불가(floor).
27. **GIVEN** 15초 내 Slow 다회, **WHEN** DR 확인, **THEN** Slow는 DR 제외·매회 원지속.
28. **GIVEN** Slow A(−20%)+B(−40%) 동시, **WHEN** 계산, **THEN** 합산(−60%) 아닌 최댓값(−40%)만.

### 팀 (Rule 8)
29. **GIVEN** 동일 파티·배신 토글 OFF, **WHEN** 공격 시도, **THEN** 피해 미적용.
30. **GIVEN** 커서가 파티원 30px 이내, **WHEN** RMB, **THEN** 락온 미발생.

### 서버권위·성능·설정 (Rule 6)
31. **[통합-보류: 넷코드]** **GIVEN** 클라 히트 요청, **WHEN** 서버가 사거리/LoS/캐스트창 재검증, **THEN** 실패 시 서버 히트 거부·클라 미반영.
32. **[통합-보류: 넷코드]** **GIVEN** PoC 목표 동시 전투 유닛 부하, **WHEN** Unreal Insights 프로파일, **THEN** 게임스레드 프레임 16.6ms 이내(ADR-0001 완료 후 측정).
33. **하드코딩 없음**: **GIVEN** `K_mitigation`·`crit_multiplier`·DR 배율/floor·`min_damage`가 config, **WHEN** 재컴파일 없이 변경, **THEN** 전투 계산에 즉시 반영·매직넘버 없음.

## Open Questions

| 질문 | 소유 | 목표 시점 | 현재 상태 |
|------|------|-----------|-----------|
| ~~GAS 채택 확정(vs 커스텀 컴포넌트)~~ | ~~technical-director~~ | ✅ 해결 | **확정: GAS 채택(ADR-0002)**. ASC on Character·서버전용 데미지/CC·예측은 발동만·Push Model 필수 |
| 배신(파티원 공격) 토글 세부 규약 | game-designer | 배신 시스템 GDD 시 | MVP 기본 아군 보호 |
| TTK 밴드 최종 확정(D6 ≈9s 다소 느림) | systems-designer + PT | 버티컬 슬라이스 PT | base_damage/K 조정 검토 |
| 힐/회복 primitive가 전투 소유인지(치유 스킬) | game-designer | 스킬 GDD 시 | 미정(전투 ApplyHeal vs 스킬) |
| CC 저항(tenacity) 스탯 도입 여부 | game-designer | 밸런싱 | 미정 |
| 원거리 평타 즉시히트 유지 vs 투사체 | game-designer + PT | 플레이테스트 | 즉시히트(결정), PT 재검증 |
| 데미지 넘버 반올림 규칙 | gameplay-programmer | 구현 | 미정(87.5 등 소수 처리) |
