# Consistency Failure Log

<!-- Auto-maintained by /consistency-check. Do not edit manually. -->
<!-- One entry per detected conflict, in chronological order. -->

| Date | GDD A | GDD B | Conflict Type | Status |
|------|-------|-------|---------------|--------|
| 2026-08-20 | combat-system.md | skill-system.md | 수치 (잘못된 파생 공식) | 해결 |
| 2026-08-20 | combat-system.md | skill-system.md | 명명 (동일 스탯 다른 이름) | 해결 |

### [2026-08-20] — /consistency-check — 🔴 CONFLICT
**Domain**: 전투 · 스킬 (DPS/HP 예산)
**Documents involved**: `combat-system.md`(source, 2026-08-14 확정) vs `skill-system.md` S5(2026-08-20 신규 작성)
**What happened**: `combat-system.md`가 이미 `full_gear_effective_hp_cap=2.0` · `full_gear_dps_cap=3.33`
로 확정·등록해 둔 장비 스케일 상한을, `skill-system.md` S5가 존재하지 않는 별도 제약
("DPS배율 × HP배율 ≤ 2.0")을 새로 만들어 재도출했다. 그 결과 `attack_power(구 attack_damage)
18.3 · max_hp 547 · DPS≤1.83`이라는, 등록값(DPS≤3.33)과 전혀 다른 중간값을 표에 실었다.
**Resolution**: S5를 등록값(3.33·2.0) 직접 사용으로 교체. `max_burst`가 완화 이전 값임에 착안해
분모를 raw HP가 아니라 **effective HP**(=raw_HP×완화계수)로 잡아, C7b(armor 배분 미결)를
건드리지 않고 계산했다. 최종 결론(Σ계수 ≤ 10.0)은 우연히 거의 그대로 유지되어 `ad_ratio` 상한·
EB 테스트·Tuning Knobs·레지스트리 `instant_burst_cap` 값은 변경 불필요했다. 레지스트리
`instant_burst_cap` 노트의 잘못된 1.83×/1.09× 언급도 3.33×/2.0×로 함께 정정.
**Pattern**: 크로스시스템 예산(HP·DPS 배율 상한)은 이미 레지스트리에 등록돼 있어도, 새 GDD를
쓸 때 다시 손으로 유도하면 별도 공식을 만들어버리기 쉽다. **레지스트리에 상수가 있으면 그 상수를
직접 대입하고, 재유도하지 말 것.**

### [2026-08-20] — /consistency-check — 🔴 CONFLICT (1차 수정 방향 오류, 같은 날 재정정)
**Domain**: 전투 · 스킬 (스탯 명명)
**Documents involved**: `combat-system.md`/`entities.yaml`(`base_stats_naked.attack_damage`) vs
`skill-system.md`(전역에서 `attack_power`로 지칭, 12곳) vs **실제 서버 코드**
(`Server/GameServer/Creature.h:7` `BaseStats::ATTACK_POWER`)
**What happened**: 같은 스탯(맨몸 10, 장비로 상승)을 GDD 두 곳이 다른 이름으로 불렀다.
**1차 수정에서 반대 방향으로 고쳤다** — `skill-system.md`의 `attack_power`를 `attack_damage`로
바꿨는데, 이는 `combat-system.md`/레지스트리가 먼저 등록됐다는 이유만으로 판단한 것이었다.
**서버 소스를 확인하지 않고 GDD 간 선후관계만으로 정답을 판단한 것이 오류였다.**
곧이어 `production/server-tasks/2026-08-14-combat-packets.md:263`에서 `attack_power`라는
표현을 발견해 실제 `Creature.h`를 확인했고, **서버가 이미 `ATTACK_POWER`로 구현·동작 중**임을
확인해 방향을 다시 뒤집었다.
**Resolution**: `skill-system.md`(12) · `combat-system.md`(4) · `game-concept.md`(1) ·
`entities.yaml`(6) 전부 **`attack_power`로 통일** — 실제 실행 중인 서버 코드에 맞춘다.
`magic_power`(스킬 시스템이 맨몸 0으로 전제, 서버엔 아직 없음)는 `base_stats_naked`와
`game-concept.md`에 신규 추가.
**Pattern**: 🔴 **크로스시스템 스탯 명명 충돌은 "어느 GDD가 먼저 썼는가"가 아니라 "실제
구현이 이미 있는가"로 판단한다.** 이 프로젝트는 서버가 사용자 소유·이미 부분 구현된
상태([[server-code-is-user-owned]])이므로, GDD 간 이름이 갈리면 **`Server/**`에 이미 그
필드가 구현돼 있는지부터 확인**해야 한다. GDD끼리만 비교하고 서버 소스를 건너뛰면 정답을
반대로 고칠 수 있다 — 이번이 그 경우였다.
