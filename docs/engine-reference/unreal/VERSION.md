# Unreal Engine — Version Reference

| Field | Value |
|-------|-------|
| **Engine Version** | **Unreal Engine 5.8.1** (Changelist 56057345, `++UE5+Release-5.8`) |
| **Release Date** | June 17, 2026 |
| **Project Pinned** | 2026-08-10 |
| **Last Docs Verified** | **2026-08-10** |
| **LLM Knowledge Cutoff** | May 2025 |
| **Risk Level** | HIGH — version is well beyond LLM training data |

## 검증 방법 (중요)

이 참조 문서는 **웹 검색 추측이 아니라 로컬 엔진 소스를 직접 grep해서** 작성했습니다.

| 설치 | 경로 | 용도 |
|---|---|---|
| 런처 바이너리 | `C:\Unreal5.8\UE_5.8` | **5.8.1 정식 빌드.** 본 문서의 검증 기준 |
| 소스 빌드 | `C:\UnrealEngine` | 5.8.1 소스 (`IsPromotedBuild: 0`) |

> **API가 의심스러우면 추측하지 말고 직접 확인하세요.**
> ```bash
> grep -rn "함수명" /c/Unreal5.8/UE_5.8/Engine/Source/Runtime --include=*.h
> ```
> 이 세션에서 `FRotator`·`KINDA_SMALL_NUMBER` 문제를 이 방식으로 확정했습니다.

## Knowledge Gap Warning

LLM 학습 데이터는 UE 5.3 / 초기 5.4까지만 신뢰할 수 있습니다.
UE 5.8은 **4버전 이상 앞섭니다.** 5.4·5.5·5.6·5.7·5.8 세부를 모릅니다.

**5.8에만 `UE_DEPRECATED(5.8, ...)` 표시가 1,059건** 있습니다 (5.7은 558건, 5.6은 537건).
학습 데이터에 없는 API 변경이 그만큼 많다는 뜻입니다.

> **UE 5.8은 UE5의 마지막 메이저 릴리스입니다.** Epic은 이후 UE6로 갑니다.
> 의도적인 UE6 마이그레이션 전까지 여기 고정합니다.

## 5.8 deprecation 밀집 모듈 (검증됨)

| 모듈 | 5.8 deprecation 파일 수 |
|---|---|
| Engine | 116 |
| Experimental | 46 |
| Core | 29 |
| CoreUObject | 21 |
| RenderCore | 15 |

**Engine 모듈 내 상위 파일**

| 파일 | 건수 | 이 프로젝트 영향 |
|---|---|---|
| `Sound/SoundWave.h` | 37 | 낮음 |
| `Materials/Material.h` | 29 | 낮음 |
| **`GameFramework/CharacterMovementComponent.h`** | **16** | **높음** |
| **`GameFramework/Character.h`** | **16** | **높음** |
| `SceneView.h` | 11 | 낮음 |

→ 상세는 `deprecated-apis.md` 참조.

## 이 프로젝트의 감사 결과 (2026-08-10)

`S1/Source/S1` 전체를 검증된 5.8 deprecation 목록으로 스캔했습니다.

```
✓ GetMovementBase          없음
✓ MovementBaseUtility      없음
✓ BasedMovement            없음
✓ KINDA_SMALL_NUMBER       없음 (UE_KINDA_SMALL_NUMBER 사용 중 — 올바름)
✓ MAX_FLT / SMALL_NUMBER   없음
✓ UInputTriggerCombo       없음
✓ bCollideWithAttachedChildren 없음
```

**deprecated API 사용 0건.** 현재 클라이언트 코드는 5.8에서 경고 없이 컴파일됩니다.

## Key Themes in UE 5.8

| 기능 | 검증 상태 |
|---|---|
| **MegaLights** | ✅ 정식 — `BaseScalability.ini`에 `r.MegaLights.*` cvar 존재 |
| **Iris 리플리케이션** | ✅ **`Runtime/Experimental/Iris` → `Runtime/Net/Iris` 로 승격.** 더 이상 Experimental 아님 |
| **ReplicationGraph** | ⚠️ 플러그인 여전히 존재(`Type: Runtime`). **헤더에 deprecation 표시 없음.** "deprecated"라는 서술은 코드로 확인되지 않음 — Epic의 투자 방향이 Iris로 이동한 것과 구분할 것 |
| **Substrate** | ✅ 이 프로젝트 활성화됨 (`DefaultEngine.ini: r.Substrate=True`) |
| **Lumen** | ✅ 이 프로젝트 활성화됨 (`r.DynamicGlobalIlluminationMethod=1`) |
| **Enhanced Input** | ✅ 표준. 단 **Combo Trigger는 5.8에서 deprecated** (`UInputTriggerCombo`) |
| Lumen Lite (Beta) | 미검증 — 공식 문서 확인 필요 |
| Mesh Terrain (Experimental) | 미검증 — 공식 문서 확인 필요 |

> ⚠️ **이 프로젝트는 UE 리플리케이션(Iris/ReplicationGraph)을 사용하지 않습니다.**
> 자체 C++ IOCP 서버 + Protobuf/TCP 구조입니다. `modules/networking.md` 를 반드시 먼저 읽으세요.

## 프로젝트 아키텍처 요약 (엔진 참조 시 전제)

```
S1/          Unreal Engine 5.8 — 순수 클라이언트 (리플리케이션 미사용)
Server/      자체 C++ IOCP 서버 — 별도 프로세스, UE와 무관
통신         Protobuf over TCP, PacketGenerator로 양측 핸들러 자동 생성
```

## Migration Notes — 5.7 → 5.8

- **릴리스 노트**: https://dev.epicgames.com/documentation/unreal-engine/unreal-engine-5-8-release-notes
- **최대 변경 클러스터**: `Character` / `CharacterMovementComponent` 의 **MovementBase 인터페이스 전환**
  (`UPrimitiveComponent` 기반 → `FMovementBaseInterfaceData` 기반). 32건.
  이 프로젝트는 해당 API를 쓰지 않아 영향 없음.
- **EngineAssociation**: `S1/S1.uproject` → `"5.8"` (설정 완료)

## Verified Sources

- **로컬 엔진 소스** — `C:\Unreal5.8\UE_5.8\Engine\Source` (본 문서의 1차 근거)
- 공식 문서: https://dev.epicgames.com/documentation/en-us/unreal-engine
- 5.8 릴리스 노트: https://dev.epicgames.com/documentation/unreal-engine/unreal-engine-5-8-release-notes
- API 레퍼런스: https://dev.epicgames.com/documentation/en-us/unreal-engine/API

## 참조 문서 세트

| 파일 | 상태 |
|---|---|
| `VERSION.md` | ✅ 5.8 검증 (2026-08-10) |
| `deprecated-apis.md` | ✅ 5.8 검증 (2026-08-10) |
| `breaking-changes.md` | ✅ 5.8 검증 (2026-08-10) |
| `modules/networking.md` | ✅ 프로젝트 전용으로 교체 (2026-08-10) |
| `current-best-practices.md` | ⚠️ 5.7 기준 — 부분 갱신 |
| `modules/*.md` (7개) | ⚠️ 5.7 기준 — 헤더만 갱신, 내용 미검증 |
| `PLUGINS.md` | ⚠️ 5.7 기준 |

⚠️ 표시된 문서는 **5.7 시점 내용을 그대로 들고 있습니다.** 참고는 하되,
API를 쓰기 전에 반드시 로컬 엔진 소스로 확인하세요.
