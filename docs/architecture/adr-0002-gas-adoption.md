# ADR-0002: GAS(Gameplay Ability System) 채택 — 전투·스킬 primitive 구현 기반

## Status
**Rejected (2026-08-11)** — 아래 § Rejection 참조. Accepted에 도달한 적 없음(계속 Proposed였음).

## Date
2026-07-19 (제안) · **2026-08-11 (기각)**

---

## Rejection — 2026-08-11

> **GAS를 채택하지 않는다.** 아래 본문은 기각 근거 보존을 위해 원문 그대로 남긴다.
> 본문의 전제 대부분이 무효이므로 **구현 지침으로 읽지 말 것.**

### 무효가 된 전제

이 ADR은 **ADR-0001(UE 리플리케이션 백본)에 의존**하도록 작성되었다(`Depends On: ADR-0001`).
그런데 이 프로젝트는 **UE 리플리케이션을 전혀 쓰지 않는다.** 서버는 별도 프로세스의
**자체 C++ IOCP 서버 + Protobuf/TCP**다. ADR-0001이 함께 기각되면서 이 ADR의 토대가 사라졌다.

무효 항목 — §Decision 1(FoW relevancy) · 8(예측 범위) · 9(ReplicationMode Mixed/Minimal) ·
10(Push Model) · §Risks R-GAS-Iris · R-FoW · R-DualPredict. **결정 11개 중 5개가 순수 UE 네트워킹.**

### 기각 사유

**1. GAS의 절반이 UE 리플리케이션이다.**
`UAbilitySystemComponent`는 ReplicationMode · `FScopedPredictionWindow` · PredictionKey ·
AttributeSet RepNotify에 깊게 묶여 있다. UE 서버가 없으면 ASC는 항상 standalone으로 돌고
**예측 시스템은 대응할 서버가 없어 아무 일도 하지 않는다.** 절반을 켜놓고 못 쓴다.

**2. 스킬을 두 번, 다른 패러다임으로 쓰게 된다.** ← 결정적

```
서버(권위)   순수 C++ 클래스로 스킬 판정
클라(GAS)    UGameplayAbility + UGameplayEffect 로 같은 스킬
             ↑ 같은 로직, 완전히 다른 표현 방식 → 어긋나면 디싱크
```

솔로 개발 + **서버는 사용자 소유 / 클라는 에이전트 소유**(`technical-preferences.md`
§ 작업 소유권 경계)라 두 구현의 저자까지 갈린다. 최악의 조합이다.

**3. 클라가 실제로 할 일에 비해 과잉이다.**
권위가 서버에 있으므로 클라의 몫은 `입력 → 스킬 사용 패킷 → 결과 수신 → 애니메이션·VFX·쿨다운 UI`
뿐이다. `UGameplayAbility`의 진짜 값어치인 "예측 실행 + 서버 확인/롤백"의 인프라가 없다.

**4. 장비→스킬 결속(USP)도 GAS가 필요 없다.**
`GiveAbility`/`ClearAbility`가 맞아 보이지만 **그것도 서버가 권위**다. 클라는 "지금 내 Q는
화염구다"만 알면 되고 `TMap<슬롯, 스킬데이터>` 하나로 충분하다.

**5. 비용이 남는 기간에 비해 크다.** 학습 1~2주(가용 12주 중) · MVP 스킬 2~3개 규모.
GAS의 구조적 이점은 스킬 수십 개 규모에서 나온다.

### 채택 대안

| | |
| ---- | ---- |
| ❌ **GAS 전체** | ASC · GameplayAbility · GameplayEffect · AttributeSet — 미사용 |
| ✅ **GameplayTags 모듈 단독** | GAS 없이 사용 가능. CC 상태 · 면역 · 태그 쿼리에 유용 |
| ❌ **GameplayCue** | GAS 의존이라 사용 불가 → Niagara 직접 호출로 대체 |
| ✅ **공유 스킬 데이터 테이블** | 서버·클라가 같은 정의를 읽는다. **로직은 서버에만**, 클라는 연출만 재생 |

**정식 구현 ADR(ADR-0003)은 전투 시스템 GDD 작성 시점에 함께 쓴다.** 지금은 앵커가 될 GDD가 없다.

### 이 기각이 되살아나는 조건

- 스킬이 **30개 이상**으로 늘고, 버프/디버프 상호작용이 태그 없이 관리 불가해질 때
- 또는 서버를 UE Dedicated Server로 갈아탈 때 (= 프로젝트 정체성 변경)

---

## Engine Compatibility

| Field | Value |
|-------|-------|
| **Engine** | Unreal Engine 5.8 |
| **Domain** | Gameplay Ability System (Core / Networking) |
| **Knowledge Risk** | MEDIUM — GAS는 UE4부터 안정이나 UE5.8 개선은 학습 데이터 밖. 결정적 미검증: GAS↔Iris 호환(ADR-0001 R-GAS). GE Components(5.3+)·NetSecurityPolicy 세부는 5.8 문서 재확인 필요 |
| **References Consulted** | `docs/engine-reference/unreal/plugins/gameplay-ability-system.md`, `current-best-practices.md`(GAS), `PLUGINS.md`, ADR-0001(R-GAS), combat-system.md, ue-gas-specialist 검토(2026-07-19) |
| **Post-Cutoff APIs Used** | GAS(`UAbilitySystemComponent`, `UAttributeSet`, `UGameplayAbility`, `UGameplayEffect`, `UGameplayEffectExecutionCalculation`, `FGameplayTag`, `FScopedPredictionWindow`, `SetSetByCallerMagnitude`, `UAbilitySystemGlobals::InitGlobalData`) — GAS 자체는 프로덕션 레디, **Iris 조합·GE Components·NetSecurityPolicy는 검증 대상** |
| **Verification Required** | (ADR-0001 PoC 공동) ① GAS 어트리뷰트 Push Model이 Iris에서 정상 ② FastArray 호환 — `FActiveGameplayEffectsContainer`+`FGameplayAbilitySpecContainer`+`FGameplayTagCountContainer` ③ FoW 재진입 시 ASC 상태 리싱크 ④ `InitAbilityActorInfo` 서버+클라+리스폰 정상 ⑤ 예측(발동)→서버 롤백이 지연 하에서 수용 가능 |

## ADR Dependencies

| Field | Value |
|-------|-------|
| **Depends On** | ADR-0001 (넷코드 백본) — GAS 리플리케이션이 채택 백본(Iris/RepGraph)에서 동작해야 함. R-GAS로 강결합, 동일 PoC 공동 검증 |
| **Enables** | 전투(#3) 구현, 스킬(#6), 장비=스킬(#7), 클래스리스 빌드(#8) 어트리뷰트 |
| **Blocks** | 전투 에픽·스킬 에픽 — GAS 미확정 시 착수 불가 |
| **Ordering Note** | ADR-0001과 병행 Proposed, R-GAS 공동 검증 후 함께 Accepted 권장. **후속 ADR 2건 필요**: (a) 스킬샷 타겟 데이터 아키텍처, (b) 리스폰 시 ASC/로드아웃 생명주기 |

## Context

### Problem Statement
combat-system.md의 primitive(어트리뷰트·데미지·CC·평타)와 스킬(6키 어빌리티)을 **무엇으로 구현할지** 확정해야 한다. 클래스리스 장비=스킬 구조라 능력·버프·쿨다운·자원·상태이상 조합이 핵심 — 능력 기반 게임(RPG/MOBA)의 전형. combat-system.md는 "GAS 유력"·Open Question("GAS 채택 확정")을 남겼고, ADR-0001은 R-GAS를 PoC 검증 대상으로 명시했다.

### Constraints
- **엔진**: UE 5.8. GAS built-in(`GameplayAbilities`), 프로덕션 레디
- **서버권위**(ADR-0001): 데미지·CC·어트리뷰트·사망은 서버 확정
- **FoW**: 시야 밖 어트리뷰트 미전송(안티치트, client_side_fow_masking 금지)
- **Iris 호환 미검증**(R-GAS)
- **인력**: 인디 — GAS 러닝커브 있으나 커스텀 재구현보다 총비용 낮음

### Requirements
- 어트리뷰트(체력·방어·마방·공격·크리·공속·이동속도) 관리·리플
- 데미지(D1/D2) 서버 권위 계산
- CC(스턴/루트/슬로우/사일런스)+DR(D4)·태그 상태
- 6키 스킬+평타 어빌리티, 쿨다운·자원
- LoL식 즉각 **발동감**(클라 예측) + 데미지/판정 서버 확정
- FoW relevancy 제어

## Decision

**전투·스킬을 GAS로 구현한다.** 구체:

1. **ASC = Character 부착**(PlayerState 아님). 이유: PlayerState는 `bAlwaysRelevant=true`라 relevancy 게이팅을 우회해 어트리뷰트가 전 커넥션에 전송됨(FoW 위반). Character는 `IsNetRelevantFor()` 경로를 타므로 FoW 제어 가능. **단 FoW는 자동이 아님** — 커스텀 relevancy(override/컴포넌트)를 시야 시스템(#13)·ue-replication-specialist와 함께 구현해야 함(§Risks R-FoW).
2. **ASC 생명주기 명시** — `InitAbilityActorInfo`를 **서버**(`PossessedBy`)와 **소유 클라**(`OnRep_PlayerState`/`PawnClientRestart`) 양쪽에서 호출. **리스폰 시 ASC는 새 Pawn과 함께 재생성**되므로 어빌리티를 매 스폰 재부여해야 함 → **로드아웃(장착 장비=스킬)은 ASC가 아니라 외부 권위(PlayerState/GameState 소유 Loadout 컴포넌트)에 보관**하고 새 ASC에 재부여. UI 델리게이트도 새 ASC에 재바인딩(ue-umg-specialist). → 후속 ADR로 상세화.
3. **어트리뷰트 = `UAttributeSet`** — health, max_health, armor, magic_resist, attack, crit_chance, crit_multiplier, attack_speed, **move_speed**. 초기화는 **DataTable/기본 GE 주도**(생성자 하드코딩 금지, coding-standards). 각 프로퍼티 Push Model.
4. **데미지 = `UGameplayEffectExecutionCalculation`**(MMC 아님) — D1(`armor/(armor+K)`)·D2(크리 pre-mitigation)·min_damage. **ExecCalc는 서버 전용 실행**(크리 RNG 포함) → 결과만 클라 리플. 크리를 예측 경로에 넣지 않음(desync·치트 방지).
5. **CC = Duration `UGameplayEffect` + `FGameplayTag`** — `State.Stunned/Rooted/Slowed/Silenced`. **DR(D4) 구체 기법**: 커스텀 `UDungeonKingAbilitySystemComponent`가 **서버 전용** `TMap<CC태그, {Count, LastTimestamp}>` 유지 → DR 배율(0.5^n, floor 0.25, 15s는 타임스탬프 비교로 타이머리스) 계산 → **Set-by-Caller magnitude**로 GE 지속시간 주입. CC 적용·DR 부기 모두 서버 전용(예측 금지).
6. **can_move/can_turn/can_cast = 태그 파생** — 어빌리티 Activation Blocked Tags + 이동/스킬의 **이벤트 구동 태그 조회**(`RegisterGameplayTagEvent`로 캐시, 매틱 폴링 금지).
7. **어빌리티 = `UGameplayAbility`** — 평타 1개 + 6키 스킬. 쿨다운·자원 = Cost/Cooldown GE.
8. **예측 범위 명확화**(과장 금지):
   - **예측함(클라 선반영)**: 어빌리티 발동·코스트·쿨다운·캐스트 몽타주/큐 시작
   - **서버 전용(계산 후 리플)**: 데미지 크기·크리 결과·CC 적용/지속·사망/킬 확정
   - → "LoL식 즉각 반응"은 **발동감**에 한정. 데미지 넘버·CC는 왕복 후 표시. 예측 거부 시 `EndAbility(bWasCancelled=true)`로 몽타주/태스크 정리
9. **리플리케이션 모드 = Mixed**(플레이어) / **Minimal**(몬스터/AI). 주의: 모드는 GE/큐 대상만 결정, **어트리뷰트 값 자체는 별도 `DOREPLIFETIME` 조건**(적 체력바용 `COND_None` 가능)이며 FoW는 relevancy로만 막음.
10. **Push Model 규율(필수)** — 모든 어트리뷰트/리플 프로퍼티 `DOREPLIFETIME_WITH_PARAMS_FAST`+`MARK_PROPERTY_DIRTY`. Iris 대비(R-GAS). 컨트롤 매니페스트 명문화.
11. **`UAbilitySystemGlobals::InitGlobalData()` 호출**(엔진 초기화) + 프로젝트 AbilitySystemGlobals 서브클래스 설정. Target Data·예측키 직렬화에 필수.

### Architecture Diagram
```
[Character] ── owns ──> [ASC(커스텀)] ──┬─ AttributeSet (health/.../move_speed)
                                         ├─ GameplayAbilities (평타, 6키)
                                         ├─ GameplayEffects (데미지/CC/쿨/자원)
                                         ├─ GameplayTags (State.*)
                                         └─ (서버) DR 트래커 TMap
  로드아웃(장비=스킬) = PlayerState/GameState 소유 → 매 스폰 새 ASC에 어빌리티 재부여
  입력 → Ability 발동(예측: 발동/쿨/큐) → 서버: ExecCalc(데미지·크리 서버전용) → 대상 health↓
       → CC GE(+Set-by-Caller DR 지속) → State.* 태그 → 이동/스킬이 이벤트로 조회해 차단
  relevancy(커스텀 IsNetRelevantFor, 시야 시스템) → 시야 밖 어트리뷰트 미전송 = FoW
```

### Key Interfaces
- **`UDungeonKingAbilitySystemComponent : UAbilitySystemComponent`** — DR 트래커 보유
- **`UDungeonKingAttributeSet : UAttributeSet`** — 어트리뷰트 + Push Model, DataTable 초기화
- **`ApplyDamage(...)`(combat)** = 서버 전용 Damage GE(ExecCalc) 적용으로 구현
- **CC 계약** = GE + `State.*` 태그, DR = Set-by-Caller 지속시간
- **move_speed 브리지** = Slow GE → move_speed 어트리뷰트 → **어트리뷰트 변경 델리게이트**로 CMC `MaxWalkSpeed` 갱신(틱 폴링 금지) → movement `effective_move_speed`가 읽음(D5). GAS 예측 vs CMC SavedMove 예측 상호작용은 §Risks R-DualPredict

## Alternatives Considered

### Alternative 1: 커스텀 어빌리티/어트리뷰트 컴포넌트
- **설명**: 자체 컴포넌트·어빌리티 구조체·데미지 함수 직접 구현
- **Pros**: 완전 통제, 넷코드 추론 단순, 러닝커브 회피, Iris 호환 직접 설계
- **Cons**: 예측·쿨다운·태그·GE 스택·리플을 전부 재구현(막대). 생태계 상실. 결국 GAS 비슷하게 재발명
- **Rejection Reason**: 능력 기반 조합 게임에서 GAS 재발명은 인디에 비효율. 프로덕션 레디를 버릴 이유 부족

### Alternative 2: 하이브리드(어트리뷰트 커스텀 + 어빌리티 GAS)
- **설명**: 데미지/체력 경량 커스텀, 어빌리티만 GAS
- **Pros**: 넷코드 민감부 단순 통제
- **Cons**: GE·ExecCalc가 AttributeSet 전제 → GAS 어빌리티가 커스텀 어트리뷰트 못 읽음. 경계 접착·이중 상태·이중 리플 모델
- **Rejection Reason**: GAS의 GE↔Attribute 통합을 스스로 끊는 자충수

### Alternative 3: ASC를 PlayerState에 부착
- **설명**: GAS 교과서 기본
- **Pros**: 사망/리스폰·빙의 건너 스탯 유지 편함
- **Cons**: PlayerState `bAlwaysRelevant` → 어트리뷰트가 relevancy 무시하고 전송 → 시야 밖 적 체력 유출(FoW·안티치트 위반)
- **Rejection Reason**: FoW 필러·안티치트 우선. relevancy 되는 Character 부착 채택, 사망 리싱크·로드아웃 재부여 비용은 수용(§Decision 2)

## Consequences

### Positive
- 어트리뷰트·데미지·CC·어빌리티·쿨다운·자원·태그를 검증된 프레임워크로 일괄 → 구현 가속
- 클라 예측으로 **발동감** 즉각(USP), 데미지/판정은 서버 확정(공정)
- 태그 기반 CC 계약이 이동/스킬/전투 간 깔끔
- ASC-on-Character로 FoW relevancy 정렬(단 커스텀 relevancy 필요)

### Negative
- GAS 러닝커브 + Push Model 규율 강제(세터마다 MARK_PROPERTY_DIRTY)
- 예측→롤백 예외처리, 이중 예측(GAS/CMC) 조율 복잡도
- 리스폰 시 ASC 재생성 → 로드아웃 외부 보관·재부여·UI 재바인딩 필요
- FoW·스킬샷 타겟데이터는 별도 작업(후속 ADR)

### Risks
- **R-GAS-Iris**(ADR-0001 공동): 어트리뷰트 Push Model·FastArray(`FActiveGameplayEffectsContainer`+`FGameplayAbilitySpecContainer`+`FGameplayTagCountContainer`)가 Iris 미검증 → *완화*: ADR-0001 PoC에 GAS 더미(어트리뷰트+스택GE+GameplayCue+태그+어빌리티스펙+FoW 재진입) 포함
- **R-FoW**: FoW는 ASC 배치만으론 불충분, 커스텀 `IsNetRelevantFor()` 필요 → *완화*: 시야 시스템(#13)+ue-replication-specialist 공동 설계, PoC 실증
- **R-DualPredict**: GAS 예측과 CMC SavedMove 예측이 move_speed에서 충돌(Slow 서버 거부 시 CMC가 틀린 속도로 수프레임 시뮬 후 보정→러버밴딩) → *완화*: Slow는 서버 전용(§8), 어트리뷰트 변경 델리게이트로 CMC 갱신, PoC 지연 테스트
- **R-Respawn**: ASC-on-Character라 리스폰 시 어트리뷰트·GE·어빌리티 재설정 누락 위험 → *완화*: 로드아웃 외부 권위 + 후속 ADR로 생명주기 명시
- **R-예측롤백**: 예측 발동 거부 시 몽타주/큐 튐 → *완화*: cancel-aware Ability Task, `EndAbility(bWasCancelled)`
- **R-Skillshot**: 6키 스킬샷의 타겟 데이터(내장 `AbilityTask_WaitTargetData` vs 서버 히트검증 RPC)가 미결정 → *완화*: **후속 ADR**로 분리(스킬 시스템 착수 전)
- **R-러닝커브**: 인디 GAS 미숙 → *완화*: ue-gas-specialist 라우팅, 평타 1개 수직 슬라이스 먼저

## GDD Requirements Addressed

| GDD System | Requirement | How This ADR Addresses It |
|------------|-------------|--------------------------|
| combat-system.md | 어트리뷰트 관리 | `UAttributeSet` + Push Model + DataTable 초기화 |
| combat-system.md | 데미지 D1/D2 | `UGameplayEffectExecutionCalculation`(서버 전용, 크리 포함) |
| combat-system.md | CC+DR(D4) | Duration GE + `State.*` 태그 + ASC 서버 트래커 + Set-by-Caller |
| combat-system.md | `ApplyDamage`, can_move/turn/cast | 서버 Damage GE + 이벤트 구동 태그 파생 |
| combat-system.md | GAS 채택 확정(Open Q) | **본 ADR로 확정** |
| movement-camera.md | Slow→effective_move_speed(D5) | move_speed 어트리뷰트 + 델리게이트로 CMC 갱신 |
| (예정) 스킬/장비=스킬/클래스리스 | 6키 어빌리티·조합 스탯·로드아웃 | `UGameplayAbility` + 외부 로드아웃 재부여(후속 ADR) |

## Performance Implications
- **CPU**: ASC 틱·GE 평가·예측 오버헤드. 16인 부하 ADR-0001 PoC로 측정. 태그 쿼리 이벤트 구동으로 폴링 회피
- **Memory**: per-connection ASC 상태(Mixed/Minimal 제한). 몬스터 다수 Minimal 필수
- **Network**: 어트리뷰트·GE·큐 대역폭. relevancy로 FoW 컬링, Push Model로 dirty만
- **Load Time**: 무시

## Migration Plan
신규 코드. **`GameplayAbilities`**(주의: 모듈명은 `AbilitySystem` 아님)·`GameplayTags`·`GameplayTasks` 모듈 의존 추가 + `GameplayAbilities` 플러그인 활성화. `UAbilitySystemGlobals::InitGlobalData()` 호출(엔진 시작). 커스텀 ASC·AttributeSet를 `DungeonKingCharacter`에 부착. 어트리뷰트 초기화는 DataTable/기본 GE. **처음부터 Push Model 규율**(사후 전환 회피, R-GAS). 컨트롤 매니페스트에 "GAS 어트리뷰트는 반드시 MARK_PROPERTY_DIRTY / 데미지·크리·CC·사망은 서버 전용 / 로드아웃은 외부 권위" 명문화.

## Validation Criteria
1. 평타 어빌리티가 예측(발동)→서버 확정으로 대상 health 감소(D1 수치 일치), 데미지 넘버는 서버 확정값
2. CC GE가 `State.*` 부착·이동/스킬이 이벤트로 차단, DR(D4) Set-by-Caller로 적용
3. 시야 밖 액터 어트리뷰트 미전송(패킷 캡처, 커스텀 relevancy)
4. (ADR-0001 PoC 공동) GAS 어트리뷰트+스택GE+GameplayCue+태그+어빌리티스펙+FoW 재진입이 채택 백본에서 정상
5. `InitAbilityActorInfo`가 서버+소유클라+리스폰에서 정상(어빌리티 발동·큐 정상)
6. 예측 발동 거부 시 롤백이 지연(net.PktLag) 하에서 수용 가능, 크리/데미지 예측 없음 확인
→ 1~6 충족 시 Accepted(ADR-0001과 함께 권장).

## Related Decisions
- ADR-0001 (넷코드 백본 — R-GAS 공동 검증)
- combat-system.md, movement-camera.md(move_speed 브리지)
- (후속) ADR — 스킬샷 타겟 데이터 아키텍처
- (후속) ADR — 리스폰 시 ASC/로드아웃 생명주기
- (예정) 스킬 시스템 GDD
