# ADR-0001: 넷코드 리플리케이션 백본 — Iris vs ReplicationGraph 실측 비교 후 채택

## Status
**Rejected (2026-08-11)** — 전제 무효. Accepted에 도달한 적 없음.

## Date
2026-07-18 (제안) · **2026-08-11 (기각)**

---

## Rejection — 2026-08-11

> **질문 자체가 이 프로젝트에 성립하지 않는다.** 아래 본문은 기록 보존용이며
> **구현 지침으로 읽지 말 것.**

이 ADR은 **"UE 리플리케이션 백본으로 Iris를 쓸까 ReplicationGraph를 쓸까"** 를 묻는다.
그런데 이 프로젝트는 **UE 리플리케이션을 전혀 쓰지 않는다.**

```
S1/        Unreal Engine 5.8 — 순수 클라이언트. 리플리케이션 미사용
Server/    자체 C++ IOCP 서버 — 별도 프로세스, UE와 무관
통신       Protobuf over TCP, PacketGenerator가 양측 핸들러 자동 생성
```

Iris도 ReplicationGraph도 **UE Dedicated Server를 전제**한다. 여기엔 UE 서버가 없다.
`bUseIris` · `ReplicationDriverClassName` · `UNetObjectGridFilter` 전부 적용 대상이 없다.

**대체 문서**: `docs/engine-reference/unreal/modules/networking.md` (프로젝트 전용으로 교체됨, 2026-08-10)

### 연쇄 영향

- **ADR-0002(GAS 채택)** 가 이 ADR에 `Depends On`으로 묶여 있었다 → **함께 기각** (2026-08-11)
- Phase 0-B PoC 게이트(16 NetConnection 부하 실측)는 **소멸**.
  대체 검증은 `production/roadmap.md` **SV-1**(Room JobQueue Flush P95 < 16.6ms) ·
  **SV-5**(대역폭 실측)가 넘겨받았다

---

## Engine Compatibility

| Field | Value |
|-------|-------|
| **Engine** | Unreal Engine 5.8 |
| **Domain** | Networking / Replication |
| **Knowledge Risk** | HIGH — 5.8은 LLM 학습 데이터(≈5.3)보다 4버전 이상 앞섬. 모든 API는 웹/공식문서 교차검증 필수 |
| **References Consulted** | `docs/engine-reference/unreal/modules/networking.md` (5.7 기준, ReplicationGraph·Iris 미포함), `docs/engine-reference/unreal/deprecated-apis.md`, Epic 5.8 공식문서(Iris/ReplicationGraph), StraySpark "Iris Opt-In 2026", ue-replication-specialist 아키텍처 검토(2026-07-18) |
| **Post-Cutoff APIs Used** | Iris: `bUseIris`, `SetupIrisSupport()`, `net.Iris.UseIrisReplication`, Push Model(`DOREPLIFETIME_WITH_PARAMS_FAST`/`MARK_PROPERTY_DIRTY`), 내장 필터 `UNetObjectGridFilter`/`UNetObjectConnectionFilter`/`UFilterOutNetObjectFilter`, 내장 프라이오리타이저 `SphereWithOwnerBoostNetObjectPrioritizer`/`NetObjectCountLimiter` — **Day-0 확인 완료(§Day-0), 셀 크기 config 키만 엔진 소스에서 확인 잔여**. RepGraph: `UReplicationGraph`, `UReplicationGraphNode_GridSpatialization2D`, `ReplicationDriverClassName` ini 키 — **PoC에서 실제 컴파일/실행으로 검증** |
| **Verification Required** | 16 **실제 NetConnection** 부하에서 (1) 서버 프레임 < 16.6ms(60Hz) (2) 커넥션당 대역폭 + per-connection 메모리 (3) FoW: 시야 밖 액터 미전송 실증(패킷 캡처) (4) GAS 어트리뷰트/FastArray/GameplayCue/재진입 리싱크 호환 (5) 패킷 손실·지연 조건 하 상태 리싱크. **1차 계측 = Networking Insights 트레이스**(`stat net`은 보조) |

## ADR Dependencies

| Field | Value |
|-------|-------|
| **Depends On** | None (프로젝트 첫 ADR) |
| **Enables** | 네트워킹 레이어(#11), 시야/릴러번시(#13), 안티치트(#25) 시스템 ADR·구현 |
| **Blocks** | 네트워킹 레이어 에픽 및 서버권위에 의존하는 모든 Vertical Slice 시스템 — 백본 미확정 시 프로덕션 착수 불가 |
| **Ordering Note** | 이 ADR은 백본을 *즉시 확정하지 않고* Phase 0-B PoC 게이트를 통해 확정한다. PoC 결과가 나오면 Status를 Accepted로 올리며 채택된 백본을 명시한다. **게이트 통과 후 백본 재변경은 사실상 전체 리라이트에 준하는 일방향 결정이다(§Risks R-OneWay).** |

## Context

### Problem Statement
DungeonKing은 UE 5.8 기반 **PvPvE 익스트랙션 심리스 던전 크롤러**로, 구역당 **12~16인**이 하나의 심리스 월드를 공유하며 **서버권위 + 전장의 안개(FoW) + 풀루팅**을 요구한다. 이는 인디 기준 최고 난도의 넷코드 조합이며, 프로젝트 전체 스코프(인원 수·심리스 여부)가 이 넷코드의 실현성에 종속된다. 따라서 **어떤 리플리케이션 백본을 프로덕션 근간으로 삼을지**를 코드 착수 전에 결정해야 한다.

로드맵(Phase 0-B)은 원래 `UReplicationGraph` 스페이셜 그리드를 전제했으나, 검증 결과 **UE 5.8에서 ReplicationGraph는 레거시/deprecated**가 되었고 신규 **Iris 리플리케이션 시스템**이 프로덕션 레디로 승격되었다. 두 시스템은 상호 배타적이라 하나를 골라야 한다. 문서·훈련데이터만으로는 우리 부하 프로파일에서 어느 쪽이 예산을 지키는지 단정할 수 없다.

### Constraints
- **엔진**: UE 5.8 고정. Iris·ReplicationGraph 모두 LLM 학습 데이터 밖 → 실측 없이 신뢰 불가
- **프레임 예산**: 서버 게임스레드 < 16.6ms @ 60Hz, 16인 동시 시뮬레이션 부하에서
- **심리스**: 인스턴스 분할 없이 단일 월드 공유(수직 확장 한계 존재)
- **인력**: 인디 규모 — 커뮤니티 예제/문서 성숙도가 실질 리스크
- **UE5 마지막 릴리스**: 5.8 이후 UE6 이행 예정 → 전방 호환 경로 선호

### Requirements
- 16인 + 스킬 난사 + 몹·루팅 밀도 핫스팟에서 서버 프레임 예산 유지
- **서버권위 FoW**: 시야 밖 액터를 클라에 *아예 리플리케이트하지 않음*(클라 마스킹 금지 = 안티치트 근간)
- **LOS/차폐 인지**: 거리 기반 그리드 필터만으로는 불충분 — 던전의 방·복도·다층 구조에서 벽 너머 액터가 새면 안티치트 구멍이 된다. 방-가시성/LOS 레이어가 필터 위에 별도로 필요하며 **이는 두 백본 모두에 동일한 추가 부담**이다.
- 5~10Hz 시야 갱신 스태거링으로 relevancy 재계산 부하를 프레임 예산 내 유지
- GAS 기반 장비=스킬 시스템(어트리뷰트·GE·GameplayCue)과의 리플리케이션 호환
- 채택된 백본이 UE6 이행 시 재작성 리스크를 최소화

## Decision

**백본을 지금 단정하지 않는다. Phase 0-B에서 Iris와 ReplicationGraph를 동일 부하로 벤치마크하고, 프레임·대역폭 예산을 지키면서 서버권위 FoW를 구현 가능한 쪽을 프로덕션 백본으로 채택한다.** 이 ADR은 그 비교 방법론·측정 기준·게이트를 확정한다. PoC 종료 시 승자를 명시하며 Status를 Accepted로 전환한다.

### Day-0 선행 스파이크 — 완료 (2026-07-18, 웹 검증)
비교 PoC의 전제는 "A안(Iris)과 B안(RepGraph)이 동등 공수"라는 것이었고, 이는 **Iris에 스페이셜 필터가 실재하는지**에 달려 있었다. **확인 결과: 실재한다. 전제 성립.**

- ✅ **`UNetObjectGridFilter` 내장** — "월드를 셀로 분할, 플레이어 시야 근처 셀의 액터만 리플리케이트". RepGraph `UReplicationGraphNode_GridSpatialization2D`와 직접 대응. 위치: `Engine/Source/Runtime/Experimental/Iris/Core/Public/Iris/ReplicationSystem/Filtering/`
- ✅ 내장 필터 3종: `UFilterOutNetObjectFilter`(리플 차단), `UNetObjectConnectionFilter`(커넥션별), `UNetObjectGridFilter`(셀 기반 스페이셜)
- ✅ config 배정: `[/Script/IrisCore.ObjectReplicationBridgeConfig] +FilterConfigs=(ClassName=..., DynamicFilterName=Spatial)`. 필터 정의: `[/Script/IrisCore.NetObjectFilterDefinitions]`
- ✅ 내장 프라이오리타이저: `SphereWithOwnerBoostNetObjectPrioritizer`(InnerRadius/OuterRadius/OwnerPriorityBoost 등), `NetObjectCountLimiter`(MaxObjectCount 등)
- **결론: 기본 거리 컬링은 커스텀 필터 없이 내장 `UNetObjectGridFilter` + config로 가능 → A/B 동일 공수 비교 성립.**
- ⚠️ **거리 전용, LOS/차폐 없음** — 방-가시성 레이어는 두 백본 공통 추가(R-LOS, 대칭). 그리드 셀 크기 등 정확한 config 키는 문서에 없어 엔진 소스/에디터에서 확인(PoC 착수 시 잔여 항목)
- ⚠️ 커스텀 `UNetObjectFilter`는 IrisCore 모듈 의존성 링커 이슈 보고됨 → 내장 필터 우선 사용으로 회피

**실측 참고(BorMor, 100인 수렴, 컬링 없음)**: 기본 리플 83.9ms → Iris 60.1ms(≈31%↓) → 패치 Iris 45.5ms. 컬링 없이도 Iris 우위 → 16인 목표엔 고무적. **함정**: 데디 서버에서 스프링암/카메라 불필요 틱(~1ms/frame 낭비) → 서버 틱 비활성화 필요. Iris 스케줄링 프레임당 524KB 할당(해제됨).

### 비교 PoC 설계 (버릴 코드)
공통 테스트 하네스:
- **DungeonKingServer.Target.cs** 신설 (Dedicated Server 빌드 타겟 — 현재 없음)
- **실제 16 NetConnection** (데디케이티드 서버 + 16 클라/헤드리스 클라 인스턴스). 서버 로컬 봇만 스폰하면 FoW 컬링·필터 평가가 발생하지 않아 벤치마크가 무의미하므로 반드시 실접속으로 구성
- 부하 구성: **플레이어 16 + 몹 N + 루팅 아이템 M + 던전 다이나믹 오브젝트**(문·함정·프로젝타일). relevancy/필터 비용은 "커넥션 × 관심 액터 수"로 스케일하므로 플레이어 외 다이나믹 액터를 실제 밀도로 채운다
- 계측 분리: **지속 이동(Push Model dirty flag) + 어트리뷰트 변경**을 GameplayCue RPC와 분리 계측. 백본 차이는 RPC 경로가 아니라 프로퍼티 dirty-tracking/직렬화에서 나온다
- 시나리오 4종:
  1. **밀집 핫스팟** — 좁은 방에 16인 + 스킬 난사
  2. **분산 배치** — 넓은 월드에 흩어짐(relevancy 최소)
  3. **가시성 churn** — 플레이어가 방을 넘나들며 relevancy가 계속 뒤집힘(심리스 특유 비용)
  4. **루팅 버스트** — 보스 사망 시 다수 아이템 액터 동시 스폰(dormancy wake 스파이크)
- 열화 조건: `net.PktLag`·`net.PktLoss`·`net.PktOrder`로 인위적 패킷 손실/지연 하에서 FoW 재진입 리싱크·GAS 상태 동기화·안티치트 이벤트(루팅/익스트랙션 확정) 최소 1회 검증

**A안 — Iris 경로**
- `bUseIris=true`(Target.cs) + `SetupIrisSupport()`(Build.cs) + `net.Iris.UseIrisReplication=1`(DefaultEngine.ini)
- 리플리케이트 프로퍼티를 Push Model로 구현(`DOREPLIFETIME_WITH_PARAMS_FAST` + `MARK_PROPERTY_DIRTY`)
- FoW: Iris **network object filter/prioritizer**로 시야 밖 액터 컬링. 5~10Hz 스태거링은 필터 내부 캐싱/쿨다운으로 직접 구현
- 계측: Iris 전용 stat 카테고리(정확명 Day-0 확인) + Networking Insights

**B안 — ReplicationGraph 경로**
- ReplicationGraph 플러그인 + `UReplicationGraph` 서브클래스, `ReplicationDriverClassName` ini 지정
- `UReplicationGraphNode_GridSpatialization2D`로 심리스 월드를 그리드 셀 분할 → FoW 구역에 직결
- 커스텀 노드로 시야 밖 액터 relevancy 차단. 5~10Hz 스태거링은 노드 틱 스로틀로 구현
- 계측: `net.RepGraph.*` cvar/노드 리스트 크기 + Networking Insights

### 채택 기준 (게이트)
1. **예산 통과 필수**: 16 커넥션 부하에서 서버 프레임 < 16.6ms + 대역폭·메모리 실측치 기록
2. **FoW 실증 필수**: 패킷 캡처로 시야 밖 액터가 클라에 전송되지 않음을 확인(LOS/방 차폐 포함)
3. **GAS 호환 필수**: 어트리뷰트·스택형 GE·GameplayCue·FoW 재진입 리싱크 모두 정상 동기화
4. **둘 다 통과 시**: Iris 채택(전방 경로·비-deprecated·async 리플). ReplicationGraph는 5.8 레거시라 동점 시 탈락
5. **둘 다 실패 시**: 로드맵 게이트 발동 → **인원 추가 하향 또는 인스턴스형(비-심리스) 재설계**

### Architecture Diagram
```
[Client Input] --Server RPC--> [Dedicated Server (권위)]
                                    |
                    +---------------+----------------+
                    | 리플리케이션 백본 (택1, PoC로 결정) |
                    |   A. Iris: filter/prioritizer   |
                    |   B. RepGraph: GridSpatial2D    |
                    +---------------+----------------+
                                    | (거리 필터 1차 컬링)
                            [방-가시성/LOS 레이어]  ← 두 백본 공통 추가
                          5~10Hz 스태거링 재계산
                                    | (시야 내 액터만)
                    [Client A 뷰]   ...   [Client N 뷰]
              (시야 밖 액터는 애초에 미수신 = 안티치트)
```

### Key Interfaces
- **`DungeonKingServer.Target.cs`** (신규 빌드 타겟, `TargetType.Server`)
- **백본 추상화 경계**: 게임플레이 코드는 백본 API를 직접 호출하지 않고, 서버권위 이동/전투/시야 결과만 리플리케이트 대상으로 표시한다. relevancy 판정은 시야 시스템(#13) 뒤로 캡슐화한다.
- **FoW relevancy 계약**: "서버가 액터 X를 커넥션 C에 리플리케이트할지"는 시야 시스템의 단일 판정 함수를 통과한다(백본별 어댑터가 이를 호출).
- **⚠️ 추상화 비대칭 주의**: 이 경계는 이진 relevancy(보임/안 보임)엔 타당하나 **얇은 래퍼가 아니다**. RepGraph는 스폰 시 그래프 등록 + 노드 틱 스로틀, Iris는 커넥션별 매-틱(캐시 주기) 필터 평가로 lifecycle 훅이 근본적으로 다르다. 두 어댑터는 사실상 독립적인 relevancy 구현이며 **공수가 동등하지 않을 수 있다**(§Risks R-Leaky). priority/대역폭 스로틀링까지는 단일 함수로 못 덮으므로 캐덴스 제어를 각 어댑터가 흡수한다.

## Alternatives Considered

### Alternative 1: 즉시 Iris 단독 채택 (실측 없이)
- **설명**: PoC 없이 Iris로 바로 프로덕션 착수
- **Pros**: Phase 0-B 공수 절감, 전방 경로 확정
- **Cons**: Iris 스페이셜 필터 성숙도·GAS 호환성이 우리 부하에서 미검증. 예제 부족. 실측 없이 심리스 16인 예산 확신 불가
- **Rejection Reason**: 프로젝트 전체 스코프가 이 결정에 종속되는데 실측 없는 채택은 리스크 과다. 로드맵이 PoC 게이트를 명시적으로 요구

### Alternative 2: 즉시 ReplicationGraph 단독 채택
- **설명**: 검증된 레거시 경로로 안전하게 진행
- **Pros**: Fortnite/Satisfactory 검증, GridSpatialization2D가 FoW 구역에 직결, 커뮤니티 예제 풍부
- **Cons**: UE 5.8에서 **deprecated** — Epic이 더는 투자 안 함, 기본 구현은 "예제 수준". UE6 이행 시 Iris 재작성 불가피(dead-end)
- **Rejection Reason**: 신규 프로젝트를 시작부터 deprecated 경로에 고정하는 것은 전략적 부채. 단, 실측 비교 대상으로는 유지

### Alternative 3: 기본 리플리케이션 + 커스텀 `IsNetRelevantFor`
- **설명**: 백본 플러그인 없이 액터별 relevancy 오버라이드
- **Pros**: 가장 단순, 소규모에 충분
- **Cons**: 16인 심리스 + 다수 다이나믹 액터 밀도에서 서버 CPU 병목(Epic 문서 명시). 액터별 거리검사 O(N²) 경향
- **Rejection Reason**: 심리스 12~16인 목표에서 확장성 부족이 명백. PoC 대상에서도 제외

## Consequences

### Positive
- 프로젝트 최고위험(넷코드) 실현성을 **코드 대량 투입 전에** 실측으로 판정
- 백본 선택이 근거 데이터에 기반 → 이후 모든 넷코드 ADR의 반석
- 백본을 시야 시스템 뒤로 캡슐화 → 어느 쪽을 골라도 게임플레이 레이어 영향 최소화

### Negative
- Phase 0-B 공수 증가(두 경로 하네스 + 실접속 16 클라 + LOS 레이어) — 단, PoC는 버릴 코드라 완성도 부담은 낮음
- 백본 확정 전까지 네트워킹 레이어 프로덕션 착수가 게이트에 묶임(의도된 순서)

### Risks
- **R-Iris**: Iris 스페이셜 필터/프라이오리타이저 API가 우리 FoW 요구를 못 채우거나 커스텀 필터 작성이 필요할 수 있음 → *완화*: **Day-0 선행 스파이크**로 필터 실재 확인, FoW 실증을 게이트 기준에 포함, 미달 시 B안 채택
- **R-LOS**: 거리 필터만으론 벽/다층 차폐 불가 → 벽 너머 액터 노출 = 안티치트 구멍 → *완화*: 방-가시성/LOS 레이어를 두 백본 공통으로 필터 위에 구현, PoC에서 실증
- **R-GAS**: GAS 어트리뷰트 세터의 `MARK_PROPERTY_DIRTY` 개조, `FActiveGameplayEffectsContainer` FastArray의 Iris 호환, FoW 재진입 시 ASC 풀 상태 catch-up이 백본별로 다를 수 있음 → *완화*: PoC에 어트리뷰트+스택형 GE+GameplayCue+재진입 리싱크 4항목 포함
- **R-Budget**: 두 백본 모두 16인 예산 실패 → *완화*: 로드맵 게이트대로 인원 하향/인스턴스형 재설계 즉시 발동
- **R-Leaky**: 백본 추상화가 얇은 래퍼가 아니라 두 독립 relevancy 구현이 되어 어댑터 공수 비대칭 → *완화*: Key Interfaces에 비대칭 명시, 캐덴스 제어를 어댑터가 흡수
- **R-OneWay**: 게이트 통과 후 백본 재변경은 사실상 전체 리라이트(Iris 채택 시 전 프로퍼티 Push Model 세터를 되돌려야 함). 롤백 경로 없음 → *완화*: 이는 일회성 결정임을 명문화, PoC 게이트를 신중히(4항목 전부 통과)
- **R-Docs**: Iris 커뮤니티 자료 빈약으로 구현 지연 → *완화*: Epic 공식문서 우선, PoC 단계에서 러닝커브 흡수
- **R-EngineDist** *(검증 완료 2026-07-18)*: **Dedicated Server 타겟은 런처(설치형) 엔진에서 빌드 불가** — `DungeonKingServer` UBT 빌드가 `"Server targets are not currently supported from this engine distribution"`로 실패(EXIT 6). 현 UE 5.8은 런처 바이너리 빌드(`InstalledBuild.txt` 존재, `SourceDistribution.txt` 없음). 데디 서버 프레임 예산 게이트를 실측하려면 **Epic GitHub UE 5.8 소스 빌드가 선행 필수** → *완화*: 소스 엔진 빌드 후 PoC 착수. `DungeonKingServer.Target.cs`는 유효하므로 유지(소스 엔진 확보 시 그대로 빌드). 리슨서버/네트워크 PIE로는 설치형에서도 relevancy·기능 검증 일부 가능(단 데디 서버 프레임 수치는 아님)

## GDD Requirements Addressed

| GDD System | Requirement | How This ADR Addresses It |
|------------|-------------|--------------------------|
| systems-index.md #11 네트워킹 레이어 | 심리스 12~16인 Dedicated Server + 서버권위 | 백본 후보를 실측 비교해 예산 통과 백본을 근간으로 확정 |
| systems-index.md #13 시야/릴러번시 | 서버권위 FoW — 시야 밖 액터 미전송, 5~10Hz 스태거링 | relevancy를 시야 시스템 뒤로 캡슐화, FoW 미전송 + LOS 차폐를 PoC 게이트 필수 기준으로 |
| systems-index.md #25 안티치트 | 서버권위 강제(클라 마스킹 금지) | FoW를 클라 시각효과가 아닌 서버 리플리케이션 컬링으로 구현하도록 백본 판정 기준에 포함, 패킷 열화 조건 검증 |
| game-concept.md (넷코드/보안 축) | 서버권위 심리스 멀티 + 전장의 안개 + 풀루팅 | 프로젝트 스코프를 좌우하는 넷코드 실현성을 코드 착수 전 게이트로 검증 |

## Performance Implications
- **CPU (서버 게임스레드)**: 핵심 측정 대상. 목표 < 16.6ms @ 60Hz, 16 커넥션 부하. relevancy 재계산이 최대 변수 → 5~10Hz 스태거링. 1차 계측은 Networking Insights 트레이스(`-trace=net,cpu`)
- **Memory**: 백본별 자료구조 + **per-connection 리플리케이션 상태**(16커넥션 × 액터 수로 스케일) 실측
- **Network (대역폭)**: 커넥션당 in/out 실측. FoW 컬링으로 시야 밖 액터 미전송 → 대역폭 절감이 핵심 이점
- **Load Time**: PoC 범위 밖(측정 안 함)

## Migration Plan
신규 코드라 기존 코드 마이그레이션 없음. 단 **Iris 채택 시** 모든 리플리케이트 프로퍼티를 처음부터 Push Model(`DOREPLIFETIME_WITH_PARAMS_FAST` + `MARK_PROPERTY_DIRTY`)로 작성하고 GAS 어트리뷰트 세터에 `MARK_PROPERTY_DIRTY`를 통합하는 규율을 컨트롤 매니페스트에 명문화해야 한다(사후 감사 비용 회피, 롤백 불가 리스크 R-OneWay). 현재 `DungeonKing` 모듈의 템플릿 클래스(Character/GameMode/Controller)는 PoC와 분리된 채 유지.

## Validation Criteria
1. 16 실접속 부하에서 채택 백본의 서버 프레임 < 16.6ms 실측 로그(Networking Insights 트레이스)
2. 커넥션당 대역폭 + per-connection 메모리 실측치가 목표 범위 내(목표치는 PoC에서 확정)
3. 패킷 캡처로 시야 밖 액터 미전송 실증(FoW 서버권위, LOS/방 차폐 포함)
4. GAS: 어트리뷰트 변경 + 스택형 GE + GameplayCue 멀티캐스트 + FoW 재진입 리싱크가 채택 백본에서 정상 동기화
5. `net.PktLag`/`net.PktLoss` 열화 조건 하 리싱크·안티치트 이벤트 최소 1회 검증
6. PoC에서 실제 사용한 필터/노드 클래스명·config 키를 ADR에 사후 기입
→ 1~5 모두 충족 시 이 ADR을 Accepted로 전환하고 채택 백본 명시. 하나라도 두 백본 모두 실패 시 로드맵 스코프 재설계 게이트 발동.

## Related Decisions
- (예정) ADR — 시야/FoW relevancy + 방-가시성/LOS 상세 설계 (이 ADR의 Enables)
- (예정) ADR — GAS 장비=스킬 리플리케이션(Push Model 규율)
- (예정) ADR — 안티치트 서버 재검증
- 로드맵 Phase 0-B (`production/roadmap.md`), 시스템 인덱스(`design/gdd/systems-index.md`)
