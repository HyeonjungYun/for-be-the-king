# 도입 계획 (Adoption Plan)

> **생성일**: 2026-08-10
> **프로젝트 단계**: Concept (`production/stage.txt` 기준 — Step 4에서 재평가 필요)
> **엔진**: Unreal Engine 5.8 (클라이언트) + 자체 C++ IOCP 서버
> **템플릿 버전**: v1.0+
> **리뷰 모드**: `lean` (이미 설정됨)

순서대로 진행하시고, 완료할 때마다 체크하세요.
언제든 `/adopt`을 다시 실행해 남은 갭을 확인할 수 있습니다.

---

## 0. 이 계획의 전제 — 컨셉 전면 재작성

> **사용자 결정 (2026-08-10)**: "기초적인 컨셉부터 다시 처음부터 작성하고 싶다"

이 결정이 계획 전체의 순서를 바꿉니다. **컨셉이 바뀌면 그 아래 모든 것이 흔들리기 때문입니다.**

```
컨셉 (game-concept.md)
  └─ 시스템 분해 (systems-index.md)
      └─ GDD 3건 (input / movement-camera / combat)
          └─ ADR 2건 (netcode / GAS)
              └─ TR 레지스트리 · 컨트롤 매니페스트
                  └─ 에픽 · 스토리
```

**따라서 인프라 부트스트랩(TR 레지스트리·컨트롤 매니페스트)을 지금 돌리면 안 됩니다.**
곧 바뀔 문서에서 요구사항 ID를 뽑아 박아버리는 셈이고, TR-ID는 규칙상 "영구, 재번호 금지"라 되돌리기 번거롭습니다.

기존 자산은 **삭제하지 않고 보존**한 뒤, 새 컨셉이 확정되면 선별 계승합니다.

---

## 형식 감사 결과 (참고)

`/adopt`은 형식 준수만 검사합니다. 결과는 **양호**했습니다.

| 항목 | 결과 |
|---|---|
| GDD 3건 8개 필수 섹션 | ✅ 전부 존재, 플레이스홀더 아닌 실제 내용 |
| GDD Status 필드 | ✅ 3건 모두 존재 (`In Design` ×2, `Designed` ×1) |
| ADR 2건 필수 섹션 | ✅ Status·ADR Dependencies·Engine Compatibility·GDD Requirements·Performance 전부 존재 |
| systems-index Status 컬럼 | ✅ 괄호 오염 없음, 유효값(`Designed`/`Not Started`)만 사용 |
| 엔진 설정 | ✅ UE 5.8, GDD·VERSION.md 간 버전 일치 |

**BLOCKING 갭 0건.** 문서 품질 자체는 높습니다 — 문제는 형식이 아니라 **전제**입니다.

---

## Step 1: BLOCKING 갭

**해당 없음.** 건너뜁니다.

---

## Step 2: 컨셉 재작성 (최우선)

### 2a. 기존 설계 자산 아카이브

컨셉을 새로 쓰기 전에 기존 문서를 보존합니다. 새 컨셉이 기존 아이디어를 상당 부분 계승할 가능성이 높고, 그때 참조할 원본이 필요합니다.

**수동 작업**:
```
design/archive/dungeonking-2026-07/
  ├─ game-concept.md
  ├─ systems-index.md
  ├─ input-system.md
  ├─ movement-camera.md
  ├─ combat-system.md
  ├─ entities.yaml
  └─ README.md   ← "왜 아카이브했는지" 한 문단
```

> ⚠️ **삭제하지 마세요.** GDD 3건에는 공식 15개 이상, 승인 기준 90여 개가 들어 있습니다. 새 컨셉이 탑다운 액션을 유지한다면 대부분 그대로 재사용 가능합니다.

**시간**: 10분
- [ ] `design/archive/dungeonking-2026-07/` 생성 및 이동
- [ ] README.md에 아카이브 사유 기록

### 2b. 새 컨셉 작성

```
/brainstorm
```

가이드 이데이션으로 컨셉 문서를 새로 만듭니다.

**시작 전에 정리해두면 좋은 것** (`/brainstorm`이 물어볼 항목):
- 기존 컨셉에서 **살릴 것**과 **버릴 것**
- 실제 기술 제약: 자체 IOCP 서버 + Protobuf/TCP, 1인 개발, 4개월
- 발표자료에 쓴 스코프("For be the King", 하드코어 탑다운 익스트랙션)와의 관계

**시간**: 1세션
- [ ] `design/gdd/game-concept.md` 새로 작성
- [ ] `/design-review design/gdd/game-concept.md` 통과

### 2c. 시스템 재분해

```
/map-systems
```

새 컨셉을 시스템으로 분해하고 의존맵·설계 순서를 다시 뽑습니다.

**기존 systems-index는 29개 시스템**이었습니다. 1인 4개월 기준으로는 과합니다 — 이번엔 스코프를 현실화하세요.

**시간**: 1세션
- [ ] `design/gdd/systems-index.md` 재작성

### 2d. GDD 선별 계승

새 systems-index가 나오면, 아카이브한 GDD 3건 중 살아남는 것을 판단합니다.

| GDD | 계승 가능성 | 판단 근거 |
|---|---|---|
| `input-system.md` | **높음** | Enhanced Input 기반, 서버 구조와 무관. 이미 구현도 진행 중 |
| `movement-camera.md` | **높음** | CMC·탑다운 카메라. 단 §9 "서버 권위(ADR-0001)" 절은 자체 서버 기준으로 재작성 필요 |
| `combat-system.md` | **중간** | 공식 6개는 유효. 단 "GAS 유력"·"ADR-0001 R-GAS" 전제가 무효 |

계승할 GDD는 아카이브에서 되살린 뒤 **넷코드 관련 절만 수정**하면 됩니다. 전면 재작성 불필요.

**시간**: GDD당 30분
- [ ] 계승 대상 확정
- [ ] 각 GDD의 넷코드/GAS 전제 절 수정

---

## Step 3: ADR 정리

### 3a. ADR-0001 폐기

`docs/architecture/adr-0001-netcode-replication-backbone.md`

**문제**: 제목부터 "Iris vs ReplicationGraph 실측 비교"입니다. UE 내장 리플리케이션 + 데디케이티드 서버를 전제로 작성됐습니다.

**실제**: `Server/` 에 자체 C++ IOCP 서버가 있고, UE 클라이언트는 Protobuf over TCP로 붙습니다. Iris도 RepGraph도 **선택지에 존재하지 않습니다.**

**조치**: `## Status`를 `Superseded`로 변경하고 대체 ADR을 가리키게 합니다. 파일은 삭제하지 마세요 — 넷코드 요구사항 분석(FoW·LOS·부하 시나리오 4종)은 자체 서버에서도 유효한 사고 자산입니다.

**시간**: 5분
- [ ] Status → `Superseded by ADR-000X`

### 3b. 대체 ADR 작성

```
/architecture-decision
```

제목 예시: **"넷코드 아키텍처 — UE 리플리케이션 대신 자체 IOCP 서버 채택"**

다뤄야 할 것:
- 왜 UE 데디케이티드 서버가 아닌가 (학습 목적 = 서버 개발 역량 확보)
- Protobuf/TCP 프로토콜 경계
- 서버 권위 범위 — 현재는 클라가 좌표를 보내고 서버가 검증 없이 중계 (**PvP 게임에서는 결국 불가**)
- 클라이언트 프록시 = 시뮬레이션이 아닌 재생 (`MOVE_None` + 보간)
- 부하 목표 재설정 (기존 "16인 심리스"는 UE 데디 서버 기준 수치)

**시간**: 1세션
- [ ] 대체 ADR 작성 및 `Accepted` 전환

### 3c. ADR-0002 재검토

`Depends On: ADR-0001` 로 명시 연결돼 있어 연쇄 영향을 받습니다.

핵심 논점이 바뀝니다 — **"서버 권위 판정을 자체 C++ 서버가 하는데, 클라이언트 GAS를 어떻게 붙일 것인가?"** 기존 ADR은 UE 서버에 ASC가 있는 전제입니다. 자체 서버 구조에서는 GAS가 클라 전용 표현 레이어가 되거나, 아예 안 쓰거나 둘 중 하나입니다.

**시간**: 30분 (판단) + 1세션 (재작성 시)
- [ ] 유지 / 수정 / 폐기 결정
- [ ] `Depends On` 을 대체 ADR로 갱신

### 3d. 아키텍처 레지스트리 정리

`docs/registry/architecture.yaml` 에 ADR-0001/0002 참조 스탠스 4건이 등록돼 있습니다.

| 항목 | 줄 | 참조 ADR |
|---|---|---|
| `character_attributes` | 47 | ADR-0002 |
| `cc_authority` | 59 | ADR-0002 |
| (넷코드 스탠스) | 105 | ADR-0001 |
| `client_side_fow_masking` (금지) | 210 | ADR-0001 |
| `predict_authoritative_gameplay` (금지) | 219 | ADR-0002 |

규칙상 **삭제 금지** — `status: superseded_by: ADR-000X` 로 표시합니다.

**시간**: 15분
- [ ] 5개 항목 superseded 표기

---

## Step 4: 인프라 부트스트랩

> ⚠️ **Step 2·3 완료 후에 실행하세요.** 컨셉·ADR이 확정되지 않은 상태에서 돌리면 무효한 전제로 영구 ID를 발급하게 됩니다.

### 4a. TR 레지스트리 채우기
```
/architecture-review
```
현재 `docs/architecture/tr-registry.yaml` 은 빈 껍데기(`requirements: []`)입니다. GDD들이 "크로스시스템 등록 대상 → Phase 5 등록"이라 명시해뒀지만 실제 등록이 한 건도 안 됐습니다.

**시간**: 1세션
- [ ] tr-registry.yaml 에 실제 항목 등록

### 4b. 컨트롤 매니페스트 생성
```
/create-control-manifest
```
`docs/architecture/control-manifest.md` 가 없습니다. 프로그래머용 "해야 할 것 / 하면 안 되는 것" 규칙 시트입니다.

**시간**: 30분
- [ ] control-manifest.md 생성

### 4c. 스프린트 추적 파일
```
/sprint-plan update
```
**시간**: 5분
- [ ] `production/sprint-status.yaml` 생성

### 4d. 단계 권위 기록
```
/gate-check
```
**시간**: 5분
- [ ] `production/stage.txt` 갱신

---

## Step 5: MEDIUM 갭

### 5a. `stage.txt` 단계 재평가
현재 `Concept` 인데, 실제로는 **3인 동시 접속 이동 동기화가 동작하는 통신 프로토타입**이 있습니다. 컨셉 문서 단계와 코드 단계가 크게 어긋나 있습니다.

> 이 프로젝트는 "설계 → 구현" 순서가 아니라 **"구현 선행, 설계 후행"** 상태입니다. 단계 판정 시 이 점을 고려하세요. Step 4d의 `/gate-check`가 근거를 갖고 판정합니다.

- [ ] 단계 재평가 후 stage.txt 갱신

### 5b. `systems-index.md` Progress Tracker 부정확
```
Design docs started: 1     ← 실제 3건
MVP systems designed: 1/10 ← 실제 3건 Designed
```
Step 2c 재작성 시 자동 해소됩니다.
- [ ] 재작성 시 확인

### 5c. `systems-index.md` #11 네트워킹 항목 불일치
```
| 11 | 네트워킹 레이어 (Dedicated Server·리플리케이션 백본 미확정: Iris vs RepGraph·서버권위) |
```
실제 구조와 다릅니다. Step 2c에서 자체 IOCP 서버 기준으로 재기술.
- [ ] 재작성 시 반영

### 5d. `.claude/docs/technical-preferences.md` 미설정 3곳
| 줄 | 항목 |
|---|---|
| 40 | Memory Ceiling |
| 44 | Testing Framework |
| 45 | Minimum Coverage |

Testing Framework는 특히 중요합니다 — 서버(C++ 콘솔)와 클라(UE Automation)가 서로 다른 프레임워크를 쓰게 됩니다.

**시간**: 15분
- [ ] 3개 항목 설정

### 5e. `.claude/docs/directory-structure.md` 실제 경로 반영
템플릿은 코드가 `src/` 에 있다고 가정하지만, 실제는:
```
S1/Source/S1/     # UE 클라이언트
Server/ServerCore/  Server/GameServer/  Server/DummyClient/
```
이걸 안 적어두면 `/dev-story`나 프로그래머 에이전트가 코드를 못 찾거나 `src/` 에 새로 만들려 합니다.

**시간**: 10분
- [ ] directory-structure.md 에 실제 경로 명시
- [ ] `src/CLAUDE.md` 의 코딩 표준을 실제 코드 경로로 이전 후 `src/` 제거 검토

### 5f. 에이전트 메모리 갱신
`.claude/agent-memory/economy-designer/project_dungeonking_concept.md` 가 옛 컨셉을 담고 있습니다. 새 컨셉 확정(Step 2b) 후 갱신하세요.

**시간**: 10분
- [ ] 새 컨셉 기준으로 갱신 또는 삭제

---

## Step 6: LOW 갭

### 6a. 세션 로그 아카이브
`production/session-logs/session-log.md` · `agent-audit.log` 가 DungeonKing 기록입니다.
`production/session-logs/archive/` 로 옮기거나 새로 시작하세요.

**시간**: 5분
- [ ] 아카이브 또는 초기화

---

## 기존 스토리에 대해

스토리 파일이 **0건**이라 해당 사항 없습니다. 앞으로 `/create-stories` 로 생성되는 스토리는 처음부터 TR-ID와 매니페스트 버전을 갖게 됩니다 — 다만 그건 **Step 4 완료 후**여야 합니다.

---

## 권장 순서 요약

```
2a 아카이브 ──▶ 2b /brainstorm ──▶ 2c /map-systems ──▶ 2d GDD 선별계승
                                                          │
                                                          ▼
                                    3a ADR-0001 폐기 ──▶ 3b 대체 ADR ──▶ 3c/3d 정리
                                                          │
                                                          ▼
                                    4a /architecture-review ──▶ 4b~4d
                                                          │
                                                          ▼
                                                    5a~5f, 6a
```

5d·5e·6a는 순서와 무관하니 **틈날 때 먼저 처리해도 됩니다.**

---

## 재실행

Step 4 완료 후 `/adopt`을 다시 실행해 남은 갭을 확인하세요.
새 실행은 그 시점의 프로젝트 상태를 반영하며, 이전 계획과 diff 하지 않습니다.
