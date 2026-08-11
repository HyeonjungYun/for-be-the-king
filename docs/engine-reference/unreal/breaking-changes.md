# Unreal Engine 5.8 — Breaking Changes

**Last verified:** 2026-08-10
**검증 방법:** 로컬 엔진 소스 grep (`C:\Unreal5.8\UE_5.8\Engine\Source`)

프로젝트 고정 버전: **5.8.1** (Changelist 56057345)

---

## 규모 감각

| 버전 | `UE_DEPRECATED(X,...)` 표시 건수 |
|---|---|
| 5.6 | 537 |
| 5.7 | 558 |
| **5.8** | **1,059** |

5.8은 이전 두 버전을 합친 것과 비슷한 양의 API가 정리됐습니다.
LLM 학습 데이터(≈5.3)와의 간극이 그만큼 큽니다.

---

## 🔴 1. MovementBase 인터페이스 전환 (최대 변경)

**영향 모듈**: `Engine/Classes/GameFramework`
**건수**: `Character.h` 16 + `CharacterMovementComponent.h` 16 = **32**

### 무엇이 바뀌었나

캐릭터가 "무엇 위에 서 있는가"를 표현하는 방식이 바뀌었습니다.

```
이전: UPrimitiveComponent*  기반
이후: FMovementBaseInterfaceData  구조체 기반
```

`UPrimitiveComponent`를 직접 참조하던 12개 이상의 `MovementBaseUtility` 오버로드가 전부 deprecated 되고, 구조체를 받는 버전으로 대체됐습니다.

### 왜 바뀌었나

`Character.h:90` 의 deprecation 메시지가 힌트를 줍니다.

> "Please use PhysicsObjectOwner instead and use that to create a FMovementBaseInterfaceData."

Chaos 물리 오브젝트를 이동 베이스로 다루기 위한 추상화로 보입니다. 컴포넌트가 아닌 물리 오브젝트 위에도 설 수 있게 하는 방향입니다.

### 마이그레이션

| Before | After |
|---|---|
| `Character->GetMovementBase()` | `GetMovementBaseObject()` / `GetMovementBaseInterfaceData()` |
| `MovementBaseUtility::GetMovementBaseVelocity(Comp, ...)` | 동명 함수의 `FMovementBaseInterfaceData` 오버로드 |
| `CMC->GetLastServerMovementBase()` | `GetLastServerMovementBaseInterfaceData()` |

### 이 프로젝트 영향

**없음.** `S1/Source/S1` 스캔 결과 해당 API 사용 0건.

단, **움직이는 발판·엘리베이터·이동 플랫폼**을 구현하는 순간 마주칩니다. 던전에 그런 요소를 넣을 계획이면 처음부터 새 API로 쓰세요.

---

## 🔴 2. Iris 리플리케이션 승격 (경로 변경)

```
5.7 이전:  Engine/Source/Runtime/Experimental/Iris
5.8:       Engine/Source/Runtime/Net/Iris        ← 검증됨
```

**Experimental에서 정식 Runtime 모듈로 이동**했습니다. 프로덕션 사용을 전제한 위치입니다.

### ReplicationGraph 상태 — 주의해서 읽으세요

| 확인 항목 | 결과 |
|---|---|
| 플러그인 존재 | ✅ `Engine/Plugins/Runtime/ReplicationGraph` |
| `.uplugin` Type | `Runtime` |
| 헤더에 `UE_DEPRECATED` 표시 | ❌ **없음** |

**"ReplicationGraph는 deprecated"라는 서술은 코드로 확인되지 않았습니다.** 플러그인은 정상 상태이고 deprecation 마커도 없습니다.

Epic의 신규 투자가 Iris로 이동한 것과, 기존 시스템이 폐기 표시된 것은 **다른 얘기**입니다. 이 프로젝트의 `ADR-0001`(폐기 예정)에 "RepGraph deprecated"로 기술돼 있는데, 근거가 약합니다.

> **어차피 이 프로젝트는 둘 다 안 씁니다.** 자체 IOCP 서버 구조입니다. `modules/networking.md` 참조.

---

## 🟡 3. Enhanced Input — Combo Trigger 폐기

```cpp
// InputTriggers.h:571
class UE_DEPRECATED(5.8, "UInputTriggerCombo has been deprecated.") UInputTriggerCombo
```

`FInputComboStepData`(530), `FInputCancelAction`(551)도 함께 폐기.

Enhanced Input 프레임워크 자체는 **표준 유지**. 콤보 트리거 기능만 제거됩니다.

**영향 없음** — 이 프로젝트는 단순 Digital/Axis2D 액션만 사용.

---

## 🟡 4. LWC — 접미사 없는 수학 타입은 `double`

UE5 전반의 변경이지만, 5.8 코드를 쓰면서 **가장 자주 걸리는 함정**입니다.

```cpp
// MathFwd.h
using FVector    = UE::Math::TVector<double>;
using FRotator   = UE::Math::TRotator<double>;
using FTransform = UE::Math::TTransform<double>;
```

```cpp
float Yaw = GetActorRotation().Yaw;                    // ⚠️ C4244 축소 변환
float Yaw = static_cast<float>(GetActorRotation().Yaw); // ✅
```

명시적 정밀도: `FVector3f`/`FVector3d`, `FRotator3f`/`FRotator3d`

---

## 🟡 5. Components — 정리 항목

| 영역 | 변경 |
|---|---|
| `USkeletalMeshComponent` | 클로스 콜리전 API → `CollisionSources` 통합 |
| `UPrimitiveComponent` | 터치 이벤트 시그니처 변경 (Touch ID + PC + HitResult) |
| `UInstancedStaticMeshComponent` | `InstanceBodies` → `GetInstanceBodies()`, 네비 바운드 **lazy 갱신** |
| `USplineComponent` | 레거시 마이그레이션 함수 제거 |
| `UActorComponent` | `AsyncRegisterLevelContext` **완전 제거** |

---

## 🟡 6. Actor — DataLayer / 가시성

| 변경 | 대체 |
|---|---|
| DataLayer 구 프로퍼티 | `DataLayerAssets` |
| `FActorDataLayer` | 사용 안 함 |
| 항상 보이는 컴포넌트 카운트 | `HasAlwaysVisibleComponents()` 계열 |

---

## 🟢 7. 렌더링 — 이 프로젝트 설정 상태

| 기능 | 5.8 상태 | 프로젝트 설정 |
|---|---|---|
| **MegaLights** | 정식 (`BaseScalability.ini` 에 cvar 존재) | 미설정 |
| **Substrate** | 정식 | ✅ `r.Substrate=True` |
| **Lumen** | 기본 GI | ✅ `r.DynamicGlobalIlluminationMethod=1` |
| **Nanite** | 기본 | ✅ `r.GenerateMeshDistanceFields=True` |
| Lumen Lite (Beta) | 미검증 | — |
| Mesh Terrain (Experimental) | 미검증 | — |

> 탑다운 100인 게임입니다. **MegaLights 도입은 신중하게** — 프레임 예산이 서버 동기화와 경합합니다.
> `game-concept.md`의 P4(정보가 자원) 때문에 **가독성이 시각적 화려함보다 우선**합니다.

---

## 5.7 → 5.8 마이그레이션 체크리스트

- [x] `EngineAssociation` = `"5.8"` (`S1.uproject` 확인됨)
- [x] deprecated API 사용 감사 — **0건**
- [x] 수학 상수 `UE_` 접두사 — 준수 중
- [ ] LWC 축소 변환 경고 — 신규 코드 작성 시 주의
- [ ] MovementBase — 이동 플랫폼 구현 시 새 API 사용
- [ ] MegaLights 도입 여부 — `/art-bible` 에서 결정

---

## 검증 명령

```bash
UE=/c/Unreal5.8/UE_5.8/Engine/Source

# 특정 버전의 deprecation 전수
grep -rc "UE_DEPRECATED(5.8," $UE/Runtime --include=*.h | grep -v ":0$" | sort -t: -k2 -rn

# 특정 헤더
grep -n "UE_DEPRECATED(5.8," $UE/Runtime/Engine/Classes/GameFramework/Character.h

# 프로젝트 감사
grep -rn "의심API" /c/Server/MMO/S1/Source/S1 --include=*.h --include=*.cpp
```
