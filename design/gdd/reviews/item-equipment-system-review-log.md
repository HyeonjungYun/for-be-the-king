# Review Log — `design/gdd/item-equipment-system.md`

> `/design-review` 실행 기록. 새 항목은 위에 추가한다.

## Review — 2026-09-03 — Verdict: MAJOR REVISION NEEDED
Scope signal: XL (P2가 담당하는 MVP subset은 L)
Specialists: game-designer, systems-designer, economy-designer, qa-lead, narrative-director, ux-designer, network-programmer, creative-director (senior)
Blocking items: 14 | Recommended: 12
Summary: creative-director — "설계는 좋다. 프레임이 틀렸다." 문서는 완성된 시스템을 기술했는데 3주 뒤 착수하는 P2는 그 절반 이하(강화·창고·고유 아이템이 T3)를 만든다. 그 결과 Player Fantasy 대부분과 AC 절반이 MVP에서 도달 불가였고, `crit_multiplier`를 선형 공식에 넣으면 MVP 운영점에서 치명타가 데미지를 깎는 공식 결함이 있었으며, 이 문서가 낳아야 할 서버 산출물(`.proto` 아이템 메시지·스키마·Room↔DB 동기화 경로)이 0이었다. 되돌릴 수 없는 결함은 없고, 병목은 사용자 결정 2건과 서버 선행 4건이다.
Prior verdict resolved: First review

### 전문가 판정

| 전문가 | 판정 | 핵심 |
|---|---|---|
| game-designer | MAJOR | Player Fantasy 90%가 MVP에 없음 · "안 들고 갈 자유" 부재 · 물리/마법 축 대칭 · D4는 천장만 검증 |
| systems-designer | MAJOR | 마법 경병기 DPS 0.8~1.1로 붕괴 · `crit_multiplier` 대체 규칙 미명문 · `GM_전설` 2.6에서 상한 초과 · base_stat 비정수 정밀도 |
| economy-designer | MAJOR | 강화 페이싱이 사망률 무시(10%면 281런) · MVP 보관소 부재 → 로그아웃 최적 · 마력정 MVP 드랍 여부 · `floor_item_pool` MVP subset · 개별 아트 예산 |
| network-programmer | MAJOR | Room↔GDBQueue 동기화 경로 미정 · 멱등 누락 · `.proto` 아이템 메시지 0개 · `instance_id` 발급 · 가방 타이머 재시작 |
| qa-lead (1차 중단, 재시도) | MAJOR | AC-13/30·AC-04·AC-23/24 모순 · 강화 AC 티어 오분류 · AC-20 재현 불가 · AC-36 도달 불가 · 신규 AC 9건 |
| narrative-director | NEEDS REVISION | C1/C2 vs D1 · 던전 기록물이 슬롯·드랍 규칙에서 미제외 · 명명 금지어 부재 · "마법 유물 10년 이내" 가드레일 |
| ux-designer | NEEDS REVISION | D1·D2 미반영 · 가득 참 피드백 · 가방 실시간 동기화 · 버리기 확인 non-blocking · 아키타입 라벨 |

### 의견 충돌과 판정

| 쟁점 | 양측 | 판정 |
|---|---|---|
| 강화 티어 | network(GDD 내부 기준 MVP 핵심) vs 나머지(roadmap 권위) | roadmap이 권위 — 🔒T3. network의 4건은 루팅·착용에 걸려 살아남음 |
| 고유 `skill_id` | economy(고정) vs game·narrative·qa(추첨) | 추첨 유지. 서사 기물 3종만 예외 |
| 물리/마법 축 | game(가짜 선택) vs systems·qa(동등성 먼저) | 최종값 동일 + 로드아웃 스케일링으로 재서술. **MVP 포함(사용자 결정 D5, 선임 제외 권고 미채택)** |
| 고유 아트 | economy(공유+화이트리스트) vs ux(고유 실루엣) | **전부 개별 아트(사용자 결정 D6)** → 예산 검증 Q10 |
| F14 격상 | game·economy(차단) vs qa·ux(명문화) | 차단, **출정 선택(사용자 결정 D3)** |
| F4 범위 | 주 리뷰 vs qa | qa 맞음 — DPS축 4건만 미분해 |
| F16 | economy vs systems·game | 둘 다 사실. GDD가 잘못된 키를 지목 |

### 같은 세션 수정 (2026-09-03)

사용자 선택 [A] 지금 수정. 결정 6건(D1~D6)을 헤더 표에 편입하고 차단 14건 전부 반영.

| 차단 | 적용 |
|---|---|
| 1 MVP 범위 경계 | 전 섹션 `[MVP]/[T2]/🔒T3` 태그 · Player Fantasy "티어별로 전달되는 것" 표 |
| 2 AC-13 vs 30 | States·AC-13에서 `Equipped` 제거. AC-30 `[unit]` |
| 3 물리/마법 예산 | C3-b 재작성 — `magic_weapon_offset` 10 + 동일 base_stat. D4 8조합 |
| 4 `crit_multiplier` | 기본 1.75 + 가산 보너스, [1.0, 3.0]. entities.yaml `crit_multiplier_base` 등록. AC-48 |
| 5 보관소 부재 | 신설 C6 출정 선택(`Vaulted` 25 MVP 최소판). AC-50 |
| 6 서버 산출물 0 | § Dependencies "서버 측 선행 작업 S1~S4" + ADR 2건 명시. Interactions 서버 전용 목록 확장 |
| 7 던전 기록물 | Core Rules 제외 조항 · Interactions 행 · AC-44 `[보류]` |
| 8 명명·가드레일 | 신설 C5-a — 층별 허용/금지 표 · 캐논 제약 · Provisional 절차 |
| 9 D1·D2 스키마 | C1 `grade_mode`/`fixed_grade`/`lore_record_ids` · 신설 C5 · 고유 base_stat ≤ 범용 · AC-41/42/43/51 |
| 10 `attack_range` | C3-b 175cm 잠정 · AC-38/39 · pending `weapon_attack_range` |
| 11 실행 불가 AC | AC-11/34 clock injection · AC-21 ceil 970 · AC-23 독립 시행 · AC-33a/b 분리 · AC-36 합성 입력 · AC-40 재시작 |
| 12 드랍 테이블·마력정 | Interactions 몬스터 AI 행 MVP subset · D6 마력정 MVP 드랍 없음 · Q9 |
| 13 UI 요건 | UI 표 재작성(티어·피드백·non-blocking·아키타입 라벨·loot-all) |
| 14 base_stat 원값 | D3 8아키타입 float 원값 + MVP 운영점 표 · `GM` 재유도 경고 |

함께 갱신: `entities.yaml`(note ② 정정 · `crit_multiplier_base` · `magic_weapon_offset` · `item_grade_model` · ceil · 마력정 T3 · pending 3건) · `equipment-skill-binding.md`(Dependencies 본문·최대 위험·EQ1 해소) · `combat-system.md`(C7b 종결 링크 · `percent_as_bonus_i` 0.35) · `production/session-state/active.md`.

### 남은 항목 (⏳)

- 🔴 서버 선행 4건 S1~S4 — 사용자, P2(09-22) 전
- `art-bible.md` § 4 ② · § 8 — 고유 아이템 개별 아트 반영·예산 검증 (Q10)
- `roadmap.md` § P2 — 출정 선택 +1일 · `floor_item_pool` MVP subset · S1~S4
- `skill-system.md` S5 — 풀장비 `attack_power` 26 기준 재계산
- `systems-index.md` — 상태 갱신
- 권고 12건 중 미반영: 장신구 비스탯 차별화(Q16) · 강화 마일스톤 연출(T3) · 버리기 확인 최종 기준(Q3) · 상대 장비 노출 범위(Q13)
- 재리뷰: 수정 규모가 커서 새 세션에서 `/design-review` 권장
