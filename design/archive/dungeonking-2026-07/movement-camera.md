# 이동 & 카메라 시스템 (Movement & Camera)

> **Status**: In Design
> **Author**: guswnd9971 + game-designer/ux-designer agents
> **Last Updated**: 2026-07-18
> **Implements Pillar**: LoL식 탑다운 조작감 (WASD 이동 + 커서 조준) — USP 기반 계층
> **Engine Framing**: UE 5.8 · CharacterMovementComponent(CMC) 기반 · 탑다운 SpringArm 카메라 · 서버권위(ADR-0001)

> **Quick reference** — Layer: `Core` · Priority: `MVP` · Key deps: `입력 시스템`

## Overview

이동 & 카메라 시스템은 입력 시스템이 제공하는 `IA_Move` raw 벡터와 커서 월드좌표를 받아, 탑다운 시점에서 캐릭터의 **WASD 8방향 직접 이동**과 **커서를 향한 조준(facing)**으로 변환하고, 플레이어를 따라가는 **탑다운 SpringArm 카메라**를 구동하는 Core 계층이다. UE 5.8 `CharacterMovementComponent`(CMC) 위에 구현되며, 클릭이동(UE 탑다운 템플릿 기본)이 아니라 **이동 방향과 바라보는 방향을 분리**한다 — WASD로 움직이면서 마우스 커서로 겨냥하는 LoL식 조작(strafe/kiting)이 핵심이다. 모든 이동은 **서버권위**로 클라이언트 예측 + 서버 재검증을 따르며(ADR-0001), 이동값은 넷코드의 relevancy·안티치트 대상이 된다. 플레이어는 이 시스템을 직접 체감한다 — 누른 방향으로 지연 없이 미끄러지듯 붙는 이동, 적을 등지고 도망치며 몸은 뒤로 겨냥하는 **카이팅의 손맛**. 이 시스템이 없으면 어떤 공간적 플레이(추격·회피·매복·조준)도 성립하지 않는다.

## Player Fantasy

**"몸은 도망치고, 손은 겨눈다."** 이동 & 카메라의 판타지는 **분리의 쾌감**이다 — 다리는 WASD로 안전하게 빠지면서, 눈과 커서는 적을 놓지 않는 **카이팅**. 초보는 이동과 조준이 뒤엉키지만, 숙련자는 둘을 독립적으로 부려 "쫓기면서 역으로 꽂는" 순간을 만든다. 이 분리를 손에 익히는 것 자체가 실력 표현이 되고, 그 순간의 **자기효능감**이 이 시스템의 핵심 감정이다.

카메라는 정반대의 판타지다 — **의식되지 않는 것이 성공**이다. 좋은 탑다운 카메라는 플레이어가 "카메라를 조작한다"고 느끼게 하지 않고, 언제나 필요한 만큼의 시야를 정확한 각도로 프레이밍해 준다. 플레이어가 카메라와 *싸우는* 순간(가려짐·못 따라옴·멀미)이 곧 실패다.

**레퍼런스**: Battlerite · League of Legends — 이동으로 빠지며 커서로 스킬샷을 꽂는 손맛. 이동과 조준의 분리가 실력이 되는 구조.
**안티 레퍼런스**: 이동이 커서 방향에 강제로 묶여 도망치며 뒤를 못 보는 조작, 캐릭터가 벽·적에 걸려 멈칫하는 뭉개짐, 플레이어를 놓치거나 흔들려 멀미를 유발하는 카메라.

## Detailed Design

### Core Rules

**1. 이동 입력 해석** — `IA_Move`(Axis2D raw, 입력 시스템 제공)를 **카메라/월드 기준** 방향으로 해석(고정 탑다운이라 화면-상단 = 월드-전방). 대각선은 정규화. `AddMovementInput`으로 CMC에 전달. *입력=raw, 이동=정규화·가속.*

**2. 속도·가감속** — 이동속도는 CMC `MaxWalkSpeed`(노브 `base_move_speed`). 가속/제동은 CMC 지상 마찰(노브 `accel`/`braking`). **strafe 속도 페널티 없음**(전 방향 균일, MVP).

**3. 페이싱(항상 커서)** — 캐릭터 yaw는 매 틱 **커서 월드좌표**(입력의 커서 서비스 → 캐릭터 Z평면 투영)를 향해 회전. `bOrientRotationToMovement=false`·`bUseControllerRotationYaw=false`로 두고 **수동 회전**(`face_interp_speed`로 보간, 0이면 즉시). → 이동방향과 무관한 **strafe 운동**.

**4. 로코모션 블렌드** — 애니메이션은 "페이싱 기준 속도 벡터"(전/후/좌우)를 8방향 strafe 블렌드스페이스에 매핑(시청각 섹션에서 상세). 도망치며 뒤로 겨냥 = 후방 strafe 애님.

**5. 기본 회피 없음** — 보편 이동은 **걷기만**. 대시·블링크·구르기는 **장비=스킬**(신발 E 등)에서 오며 스킬 시스템 소유. 이동 시스템은 스킬이 호출할 **위치이동 API**(`RequestDash(dir, dist, speed)`)만 제공.

**6. 콜리전** — CMC 캡슐 콜리전. 지오메트리(벽)에 막힘, 관통 없음. **폰 간 상호 블로킹**(적/타 플레이어를 밀거나 통과 못 함; 노브 `pawn_collision`).

**7. CC(군중제어) 반응** — 전투/스킬이 root/stun 적용 시: **Root** = 이동 입력 무시(속도 0), 페이싱은 유지. **Stun** = 이동·페이싱 모두 정지. 이동 시스템은 `can_move`/`can_turn` 플래그를 **읽기만** 하고, CC 상태 권위는 전투/스킬 소유.

**8. 카메라(고정 팔로우)** — SpringArm 고정 길이(`cam_arm_length`)·고정 pitch(`cam_pitch`, 예 −60°)·고정 yaw. 타깃=플레이어, 부드러운 추적 위해 카메라 lag(`cam_lag_speed`). MVP에 회전·줌 조작 없음.

**9. 서버 권위(ADR-0001)** — 이동은 CMC 내장 네트워크 예측(autonomous proxy 예측 → 서버 권위 → 불일치 시 보정). 페이싱/조준 좌표는 서버 전송·재검증(안티치트). 예측/보정 상세는 **넷코드 ADR로 이연**. 이동 결과는 relevancy·FoW 대상.

### States and Transitions

| State | Entry | Exit | Behavior |
|-------|-------|------|----------|
| `Idle` | 지상, `IA_Move`=0 | `IA_Move`≠0 / CC | 정지, **커서 페이싱 지속** |
| `Moving` | 지상, `IA_Move`≠0 | `IA_Move`=0 / CC | CMC 이동, 커서 페이싱 지속, strafe 블렌드 |
| `Rooted` | 전투/스킬 root | root 해제 | 이동 입력 무시(속도 0), **페이싱 지속** |
| `Stunned` | 전투/스킬 stun | stun 해제 | 이동+페이싱 **모두 정지** |

> 대시/블링크는 스킬 시스템이 `RequestDash`로 이동 시스템에 위임 → 일시적 이동 오버라이드(스킬 GDD에서 상세). 점프/공중 상태는 MVP 제외(❓ Open Questions).

### Interactions with Other Systems

| System | Direction | Interface (in/out) | 소유 경계 |
|--------|-----------|--------------------|-----------|
| 입력 시스템 | ← depends on 입력 | `IA_Move` raw 벡터 in, 커서 월드좌표 쿼리 in | 입력=raw/커서, 이동=변환 |
| 전투 시스템 | ↔ | `can_move`/`can_turn`(CC) in; 평타가 이동을 자동정지하지 않음(전투가 결정) | CC 권위=전투, 이동=반응 |
| 스킬 시스템 | ← 스킬이 이동 호출 | `RequestDash(dir,dist,speed)` out(제공) | 이동기 판정=스킬, 위치변경 실행=이동 |
| 네트워킹 | 이동 → 네트워킹 | CMC 예측/보정, 이동값 relevancy/FoW 대상 | 권위 검증=서버(ADR-0001) |
| 시야/릴러번시 | ↔ | 카메라 프레이밍 vs FoW 마스킹(별개) | 카메라=이동 소유, FoW=시야 소유 |
| HUD | ← depends on 이동 | 플레이어 월드좌표 out(미니맵) | 이동=좌표, HUD=렌더 |
| 던전/월드 | ← depends on 월드 | 콜리전 지오메트리 in | 월드=지오메트리 |

## Formulas

### 1. `effective_move_speed` — 최종 이동속도 (⭐ 크로스시스템)

장비 이동속도 보너스는 **가산 스택 후 최종 1회 곱 + 하드캡**. 곱연산(`×(1+x)` 순차)은 아이템 5개 각 +20%에서 `1.2^5≈2.49배`로 지수 폭주 → PvP 무한 카이팅 유발이라 배제. 가산은 예측 가능하고, 하드캡으로 상·하한을 강제.

`effective_move_speed = clamp((base_move_speed + flat_ms_bonus) × (1 + Σ percent_ms_bonus_i), ms_min_ratio × base_move_speed, ms_max_ratio × base_move_speed)`

| 변수 | 타입 | 범위 | 설명 |
|------|------|------|------|
| `base_move_speed` | float | 500–700 cm/s (기본 600, **PT 필요**) | CMC `MaxWalkSpeed` 기본값 |
| `flat_ms_bonus` | float | 0–150 cm/s | 장비 고정치 이동속도 합 |
| `percent_ms_bonus_i` | float | 0.00–0.15 (아이템당) | 장비 % 이동속도, **가산 스택** |
| `ms_min_ratio` | float | const 0.5 | 하한 비율(둔화 대비 최소 보장) |
| `ms_max_ratio` | float | const 1.5 (**PT 필요**, 1.3–1.8, 2.0 초과 금지) | PvP 카이팅 억제 + 넷코드 예측 안정성 캡 |

**출력 범위**: `[ms_min_ratio×base, ms_max_ratio×base]` 하드 클램프(base=600 기준 [300,900] cm/s). PvP 공정성 + CMC 예측 오차(고속일수록 재보정 튐↑)가 캡 근거.
**예시**: base=600, flat=30, percent 합=0.15 → `(600+30)×1.15=724.5` → 범위 내 → **724.5 cm/s**.

### 2. `cursor_facing_yaw` — 커서 페이싱 보간

`target_yaw = atan2(cursor.Y − actor.Y, cursor.X − actor.X)` (입력 시스템의 캐릭터 Z평면 투영 좌표 기준)

- `face_interp_speed == 0` → `new_yaw = target_yaw` (즉시 스냅)
- `face_interp_speed > 0` → `new_yaw = actor_yaw + wrap180(target_yaw − actor_yaw) × clamp(dt × face_interp_speed, 0, 1)` (`RInterpTo` 방식)
- `cursor_to_actor_dist < dead_radius` → `new_yaw = actor_yaw` (직전 유지)

| 변수 | 타입 | 범위 | 설명 |
|------|------|------|------|
| `target_yaw` / `actor_yaw` / `new_yaw` | float(deg) | [−180,180] | 목표/현재/이번 틱 yaw |
| `face_interp_speed` | float | 0–40 (기본 20, **PT 필요**) | 0=즉시, 클수록 스냅에 근접 |
| `dt` | float | >0 (60fps≈0.0167) | 프레임 델타타임 |
| `cursor_to_actor_dist` | float | ≥0 cm | 커서-캐릭터 평면거리 |
| `dead_radius` | float | 15–50 cm (기본 25, **PT 필요**) | 이 반경 내 커서는 방향 미정의(atan2 특이점) → 미갱신 |

**출력 범위**: 항상 [−180,180] 정규화(wrap). dead_radius 미달 시 미갱신 → NaN/스냅튐 방지.
**예시**: actor=0°, target=90°, speed=20, dt=1/60 → alpha=0.333 → `new_yaw=30°`. ≈10프레임(166ms)에 수렴.

### 3. `dash_kinematics` — RequestDash 운동학 (⭐ 크로스시스템, 스킬 참조)

`dash_duration = dash_dist / dash_speed`
`dash_pos(t) = dash_start_pos + normalize(dash_dir) × dash_speed × t`, `0 ≤ t ≤ dash_duration`

대시 발동 시 CMC 이동모드를 일시 오버라이드(걷기 입력 무시, CC와 별개의 최우선 오버라이드) → `dash_duration` 경과 시 `Walking` 복귀, 잔여 오차는 `dash_start_pos + dir×dash_dist`로 스냅 보정.

| 변수 | 타입 | 범위 | 설명 |
|------|------|------|------|
| `dash_dir` | Vector2D(정규화) | \|v\|=1 | 대시 방향(스킬 지정) |
| `dash_dist` | float | 200–800 cm (스킬별) | 대시 거리(이동은 API만 소유) |
| `dash_speed` | float | 800–2400 cm/s | 대시 속도(순간 오버라이드) |
| `t` | float | 0–dash_duration | 대시 경과시간 |
| `dash_duration` | float | 0.15–0.4s (**PT 필요**) | 대시 지속시간 |

**출력 범위**: 실제 이동거리는 벽 충돌 시 `dash_dist` 미만 조기 종료 가능(Edge Cases). 권장범위 초과 시 스킬 밸런스 리뷰.
**예시**: dist=400, speed=1600 → duration=0.25s. 틱당 이동 ≈26.7cm.

### 4. `camera_follow_lag` — 카메라 위치 보간

고정 탑다운도 lag 필요: ① CMC 서버 보정 튐(rubber-banding) 화면 노출 방지, ② 급가감속·대시에 1:1 반응 시 로봇 같은 카메라 방지(판타지 "의식되지 않는 카메라"), ③ 급방향전환 스냅 멀미 방지.

`cam_pos_new = cam_pos_prev + (player_pos − cam_pos_prev) × clamp(dt × cam_lag_speed, 0, 1)`

리쉬 클램프(대시 등 고속 시 놓침 방지):
`if |player_pos − cam_pos_new| > cam_max_lag_dist: cam_pos_new = player_pos − normalize(player_pos − cam_pos_new) × cam_max_lag_dist`

| 변수 | 타입 | 범위 | 설명 |
|------|------|------|------|
| `cam_pos_prev` / `player_pos` / `cam_pos_new` | Vector | 월드좌표 | 이전 카메라 / 플레이어 / 이번 틱 카메라 |
| `cam_lag_speed` | float | 5–15 (기본 10, **PT 필요**) | 클수록 빠르게 따라붙음 |
| `cam_max_lag_dist` | float | 150–500 cm (기본 300, **PT 필요**, 고속 대시와 조합 검증) | 카메라-플레이어 최대 이격(리쉬) |
| `dt` | float | >0 | 프레임 델타타임 |

**출력 범위**: 리쉬로 카메라-플레이어 거리 항상 `[0, cam_max_lag_dist]`.
**예시**: lag_speed=10, dt=1/60 → alpha≈0.167. 300cm 이동 직후 정지 시 ≈10~11프레임(180ms) 수렴. 400cm 순간 대시 시 리쉬 발동(300cm 상한까지 당김).

### 5. `diagonal_input_normalization` — 대각선 입력 정규화

`move_vector = raw_input_vector / max(|raw_input_vector|, 1)`

| 변수 | 타입 | 범위 | 설명 |
|------|------|------|------|
| `raw_input_vector` | Vector2D | \|v\|∈[0,√2] 디지털 / [0,1] 아날로그 | `IA_Move` raw(입력 제공) |
| `move_vector` | Vector2D | \|v\|≤1 | 정규화 입력, `effective_move_speed`와 곱해 최종 속도 |

**출력 범위**: `|move_vector|≤1` 보장. 정면(|raw|=1) 유지, 대각선(√2) 1로 축소, 아날로그 절반(0.5)은 보존.
**예시**: W+D → raw=(1,1), |raw|=1.414 → `(0.707,0.707)` → 대각선도 카디널과 동일 크기(대각 이속버그 방지).

> **크로스시스템 등록 대상**: `effective_move_speed`(장비/성장이 보너스 공급), `dash_kinematics`(스킬이 `RequestDash`로 참조) → Phase 5에서 레지스트리 등록. 나머지 3개는 내부 소유.

## Edge Cases

| 상황 | 처리 | 근거 |
|------|------|------|
| 커서가 캐릭터 위(dead_radius 내) | 페이싱 미갱신, 직전 yaw 유지 | atan2 특이점·스냅튐 방지(공식2) |
| 커서 화면 밖 | 입력이 화면경계 clamp, 이동은 마지막 유효 월드좌표 | 조준 무효화 방지(입력 GDD 정합) |
| 커서 아래 지오메트리 없음(허공/구멍) | 페이싱은 `GetHitResultUnderCursor`가 아닌 **캐릭터 Z평면 해석적 교차**로 산출 → 항상 유효 | 허공에서 페이싱 소실 방지 |
| 대시가 벽에 충돌 | 스윕이 벽면 정지, 대시 조기 종료(관통 없음), 잔여거리 취소 | 벽 관통 방지 |
| 대시가 다른 폰에 충돌 | 폰 블로킹 정지(밀치기 없음, MVP), 조기 종료 | 폰 관통·강제밀치 방지 |
| 대시 중 **Stun** | 대시 즉시 취소→Walking(잔여 변위 없음) | 하드 CC는 강제이동도 끊음 |
| 대시 중 **Root** | 진행 중 대시 유지(변위 완료), 종료 후 걷기 봉쇄 | Root=신규 이동 봉쇄지 기존 변위 취소 아님 |
| 반대 입력 동시(W+S / A+D) | 합벡터 0 → Idle | 상쇄 정상 |
| 걷다가 벽 충돌 | CMC 벽면 슬라이드(정지 아님) | 표준 이동감 |
| 두 폰이 같은 공간으로 밀어넣음 | 콜리전 해소, 오버랩 없음 | 겹침 방지 |
| 서버 보정(rubber-band) 큰 튐 | 카메라 lag 흡수, `cam_max_lag_dist` 내 스냅 | 화면 떨림·멀미 방지(공식4) |
| 넉백/강제이동(전투) | 외부 속도가 입력 이동에 우선, 이동은 양보 | 넉백 권위=전투 |
| 사망 중 | 이동·페이싱 비활성(사망/전투 시스템 처리) | 사망 상태 이동 방지 |
| 대형 dt 스파이크(프레임드랍) | 보간 alpha가 clamp(0,1)이라 오버슛 없음 | 저프레임 안정성(공식2/4) |

## Dependencies

| System | 방향 | 성격 | 인터페이스 |
|--------|------|------|-----------|
| 입력 시스템 | 이동 → 의존 | **하드** | `IA_Move` raw 벡터 + 커서 월드좌표 없이 이동/페이싱 불가 |
| 던전/월드 | 이동 → 의존 | **하드** | 콜리전 지오메트리(벽·바닥) 없이 이동 판정 불가 |
| 네트워킹 | 이동 ↔ 네트워킹 | **소프트(MP)** | CMC 예측/보정, 이동값이 relevancy/FoW 대상 (ADR-0001) |
| 전투 시스템 | 전투 → 이동 | 역방향(하드) | 전투가 `can_move`/`can_turn`(CC) 공급; 이동은 읽기·반응 |
| 스킬 시스템 | 스킬 → 이동 | 역방향(하드) | 스킬이 `RequestDash(dir,dist,speed)` 호출; 이동이 실행 |
| 장비/성장 | 장비 → 이동 | 역방향(소프트) | 장비 이동속도 스탯이 `effective_move_speed`에 공급 |
| HUD | HUD → 이동 | 소프트 | 플레이어 월드좌표(미니맵) |
| 적 AI | 적AI → 이동 | 역방향(소프트) | AI 경로이동은 NavMesh(별도 소유), CMC 로코모션 규약만 공유 |

> **양방향 정합성**: `input-system.md`는 이미 "이동 & 카메라 → 입력(하드)"을 명시함. 하위 의존 시스템(전투·스킬·장비·HUD·적AI)은 아직 미설계 — 각 GDD 작성 시 "depends on 이동 & 카메라"를 명시해야 함(설계 순서상 이동이 먼저라 정상).

## Tuning Knobs

| 파라미터 | 기본값 | 안전 범위 | ↑ 높이면 | ↓ 낮추면 |
|----------|--------|-----------|----------|----------|
| `base_move_speed` | 600 cm/s | 500–700 | 빠름·PvP 추격 쉬움 | 느림·카이팅 난이도↑ |
| `ms_max_ratio` | 1.5 | 1.3–1.8 | 카이팅 강화·도주 밸붕 위험 | 추격 쉬움·이속 아이템 무가치화 |
| `ms_min_ratio` | 0.5 | 0.4–0.6 | 둔화 약화 | 둔화가 스턴급으로 강력 |
| `accel` / `braking` (CMC) | (튜닝) | — | 민첩·즉각 정지 | 관성감·굼뜬 반응 |
| `face_interp_speed` | 20 | 0–40 | 스냅·정밀 조준 / 부자연 | 부드러움 / 조준 지연 |
| `dead_radius` | 25 cm | 15–50 | 근접 커서 안정 / 데드존 체감↑ | 민감 / atan2 특이점 위험 |
| `pawn_collision` | on | bool | 몸싸움·막힘(끼임 가능) | 통과·겹침 허용 |
| `cam_arm_length` | (튜닝) | — | 넓은 시야·캐릭터 작아짐 | 좁은 시야·몰입↑ |
| `cam_pitch` | −60° | −50~−75° | 순수 탑다운·원근감↓ | 비스듬·원근감↑ |
| `cam_lag_speed` | 10 | 5–15 | 타이트 추적 / 서버 튐 노출 | 부드러움 / 놓침 위험 |
| `cam_max_lag_dist` | 300 cm | 150–500 | 느슨·대시 시 원경 | 타이트 / 급스냅 |

> **상호작용 주의**: `base_move_speed` × `ms_max_ratio`가 실질 최고속. `cam_lag_speed`·`cam_max_lag_dist`는 최고속·대시속도와 함께 튜닝해야 카메라가 안 놓침. 대시 노브(`dash_dist`/`dash_speed`/`dash_duration`)는 **스킬 시스템 소유** — 여기 아님(공식3은 계약만 정의).
>
> **하드코딩 금지**: 위 모든 노브는 외부 config(DataTable/DeveloperSettings)에서 로드. 코드 상수 금지(coding-standards).

## Visual/Audio Requirements

| 요소 | 요구 | 소유 |
|------|------|------|
| 로코모션 | 8방향 **strafe 블렌드스페이스**(페이싱 기준 전/후/좌우/대각), Idle→Walk/Run 전이. 도망+뒤조준 = 후방 strafe(**문워크 금지**) | 애니메이션 |
| 조준 포즈 | 상체가 커서를 향하는 **aim offset**(하체는 이동방향과 독립) | 애니메이션 |
| 발소리 | 지면 표면별 footstep SFX, gait/speed로 트리거 | 오디오 |
| 대시 VFX/SFX | 대시 연출은 **스킬 소유**(이동은 모션만 제공) | 스킬 |
| 카메라 필 | MVP 셰이크 없음(전투 히트 셰이크는 전투 소유), lag만. 멀미 방지 우선 | 카메라 |
| CC 시각(root/stun) | 루팅/스턴 시각효과는 전투/스킬 소유 | 전투/스킬 |

> 📌 **Asset Spec**: 아트바이블 승인 후 `/asset-spec system:movement-camera`로 로코모션 세트 스펙 생성. `art-director` 협의는 아트바이블 확정 후(현재 lean·아트바이블 미작성으로 생략 — 프로덕션 전 검토 필요).

## UI Requirements

이동 & 카메라는 **직접 UI를 그리지 않는다.** 플레이어 월드좌표를 미니맵용으로 HUD에 제공할 뿐(HUD 소유). MVP 카메라는 고정이라 카메라 설정 UI 없음(줌/회전 옵션은 후속). → **UX Flag 없음.**

## Acceptance Criteria

> `[통합-보류]` = 의존 시스템(스킬·전투CC·넷코드) 미설계로 종단 검증은 해당 시스템 완성 후. 현재는 Logic/Integration 단위까지 검증.

### 핵심 규칙 (Rule 1~6)
1. **GIVEN** 지상 정지, **WHEN** W+D 동시 입력, **THEN** 월드 대각 방향 이동 + 대각 정규화로 속도가 단일축(≤600cm/s base)을 초과하지 않음. *(Rule 1)*
2. **GIVEN** W 전진 중, **WHEN** 커서를 이동방향과 다른 곳으로, **THEN** 이동방향은 W 유지·facing만 커서로 회전(strafe 성립). *(Rule 2)*
3. **GIVEN** 정지·커서 dead_radius 밖, **WHEN** 커서 반대편 순간이동, **THEN** yaw가 즉시 스냅 않고 `RInterpTo`(face_interp_speed=20)로 점진 회전. *(Rule 2)*
4. **GIVEN** 특정 yaw 정지, **WHEN** 커서가 25cm(dead_radius) 이내 진입, **THEN** facing 미변경·기존 yaw 유지. *(Rule 2)*
5. **GIVEN** 스킬 미보유, **WHEN** 어떤 이동키 조합, **THEN** 자체 대시/회피 없음(대시는 `RequestDash`로만). *(Rule 3)*
6. **GIVEN** 하네스에서 `RequestDash(dir=(1,0), dist=500, speed=1000)`, **WHEN** 실행, **THEN** 지정방향 500cm 이동·duration=0.5s 내 완료·CMC 오버라이드 후 위치 스냅 보정. *(Rule 3, D3)*
7. **[통합-보류: 스킬]** **GIVEN** 대시 유발 스킬 구현됨, **WHEN** 스킬 사용, **THEN** 스킬이 `RequestDash` 호출→대시. *(Rule 3)*
8. **GIVEN** 두 폰 인접, **WHEN** 한 폰이 상대 쪽 이동, **THEN** 통과 못 하고 충돌 반응(정지/슬라이드). *(Rule 4)*
9. **GIVEN** 카메라 스폰 pitch/yaw/arm, **WHEN** 캐릭터 이동·회전, **THEN** SpringArm pitch/yaw/arm 고정 유지. *(Rule 4)*
10. **GIVEN** Root 적용, **WHEN** WASD 입력, **THEN** velocity 0 유지·커서 facing 회전은 정상. *(Rule 5)*
11. **GIVEN** Stun 적용, **WHEN** 이동+커서 입력, **THEN** velocity·yaw 모두 고정. *(Rule 5)*
12. **[통합-보류: 전투/스킬]** **GIVEN** `can_move=false` 설정, **WHEN** 이동 시스템이 플래그 조회, **THEN** 자체 CC 판정 없이 플래그 신뢰해 이동 차단. *(Rule 5)*
13. **[통합-보류: 넷코드]** **GIVEN** 인위적 지연 ≥100ms, **WHEN** 이동 입력, **THEN** 클라 로컬 예측 즉시 표시·서버 불일치 시 보정. *(Rule 6)*

### 공식 (D1~D5)
14. **GIVEN** base=600·Σpercent=+100%, **WHEN** 계산, **THEN** `clamp(1200,300,900)=900`(상한 1.5×). *(D1)*
15. **GIVEN** base=600·Σpercent=−90%, **WHEN** 계산, **THEN** `clamp(60,300,900)=300`(하한 0.5×). *(D1)*
16. **GIVEN** yaw=175°·목표=−175°, **WHEN** RInterpTo, **THEN** ±180 경계 최단경로(10° 이내) wrap·350° 우회 없음. *(D2)*
17. **GIVEN/WHEN/THEN** — AC-6과 동일 케이스로 `dash_duration=dist/speed` 산식 명시 검증. *(D3)*
18. **GIVEN** cam_lag_speed=10·카메라 정지에서 캐릭터 600cm/s 출발, **WHEN** 매 프레임 lag 적용, **THEN** 즉시 스냅 않고 지수 근사로 추격(프레임 위치 로그 확인). *(D4)*
19. **GIVEN** 캐릭터가 지속적으로 더 빠름, **WHEN** 카메라-캐릭터 거리 300cm 도달, **THEN** `cam_max_lag_dist`=300 초과 않도록 클램프. *(D4)*
20. **GIVEN** raw=(1,1), **WHEN** `raw/max(|raw|,1)`, **THEN** 크기 1.0 정규화·대각 속도가 단일축 초과 안 함. *(D5)*

### 엣지 케이스
21. **GIVEN** 커서가 raycast 히트 없는 void 위, **WHEN** 커서 월드좌표 계산, **THEN** 캐릭터 Z평면 해석적 교차 사용·널참조/크래시 없음.
22. **GIVEN** 대시 중 경로에 벽, **WHEN** 벽 충돌, **THEN** 목표거리 전 조기 정지·관통 없음.
23. **[통합-보류: 전투/스킬]** **GIVEN** 대시 중, **WHEN** Stun 적용, **THEN** 대시 즉시 취소·현위치 정지.
24. **[통합-보류: 전투/스킬]** **GIVEN** 대시 중, **WHEN** Root 적용, **THEN** 대시 완료 후 추가 이동 차단.
25. **GIVEN** 이동 가능, **WHEN** W+S 동시, **THEN** 상쇄로 raw=0·Idle 유지.
26. **[통합-보류: 넷코드]** **GIVEN** 서버 rubber-band 발생, **WHEN** 위치 보정, **THEN** 카메라가 lag(10)로 흡수·급격한 시각 튐 없음.
27. **GIVEN** 프레임드랍으로 dt=0.5s, **WHEN** `alpha=clamp(dt×lag,0,1)`, **THEN** alpha≤1로 카메라 오버슛 없음.

### 성능 / 설정
28. **성능**: **GIVEN** 60fps(16.6ms 예산)·서버권위, **WHEN** Unreal Insights로 이동/카메라 로직(Tick·CMC·facing·camera lag) 트레이스, **THEN** 게임스레드 합산이 프레임 예산의 **10% 이내**·60fps 초과 안 함.
29. **하드코딩 없음**: **GIVEN** `base_move_speed`/`face_interp_speed`/`dead_radius`/`cam_max_lag_dist`/`cam_lag_speed` 등 모든 노브, **WHEN** 선언부 검사, **THEN** 리터럴 아닌 외부 config(DataAsset/DataTable)에서 로드·재컴파일 없이 변경 반영.

## Open Questions

| 질문 | 소유 | 목표 시점 | 현재 상태 |
|------|------|-----------|-----------|
| 점프/공중 이동 도입 여부 | game-designer | 플레이테스트 후 | MVP 제외(지상 전용) |
| `pawn_collision` on/off 최종(몸싸움 vs 통과) | game-designer | 플레이테스트 | 기본 on |
| `face_interp_speed` 즉시(0) vs 보간(20) 최종 감각 | game-designer + PT | 플레이테스트 | 기본 20(제안) |
| Mover 2.0 재검토 시점 | technical-director | Mover 프로덕션 승격(5.8/5.9 창) 시 | CMC로 시작 확정 |
| 넉백/강제이동 세부 규약(전투↔이동 인터페이스) | 전투 담당 | 전투 GDD 작성 시 | 전투 소유, 이동은 양보 |
| `base_move_speed`·카메라 노브 수치 확정 | systems-designer + PT | 버티컬 슬라이스 플레이테스트 | 제안값(PT 필요 표시) |
