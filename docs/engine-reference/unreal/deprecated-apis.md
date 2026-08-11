# Unreal Engine 5.8 — Deprecated APIs

**Last verified:** 2026-08-10
**검증 방법:** 로컬 엔진 소스 grep (`C:\Unreal5.8\UE_5.8\Engine\Source`) — 웹 추측 아님

형식: **쓰지 말 것 X** → **대신 Y**

> **5.8에 `UE_DEPRECATED(5.8, ...)` 1,059건이 있습니다.** 아래는 이 프로젝트에
> 실제로 닿을 수 있는 것만 추렸습니다. 전수 목록이 아닙니다.
>
> 여기 없는 API가 의심되면:
> ```bash
> grep -rn "UE_DEPRECATED" /c/Unreal5.8/UE_5.8/Engine/Source/Runtime --include=*.h | grep "함수명"
> ```

---

## 🔴 Core Math — 상수 접두사 (5.1부터, 여전히 유효)

`UnrealMathUtility.h:65-82` 에서 **매크로 자체가 deprecated 처리**되어 있습니다.

| Deprecated | Replacement |
|---|---|
| `KINDA_SMALL_NUMBER` | `UE_KINDA_SMALL_NUMBER` |
| `SMALL_NUMBER` | `UE_SMALL_NUMBER` |
| `BIG_NUMBER` | `UE_BIG_NUMBER` |
| `PI` | `UE_PI` |
| `HALF_PI` / `TWO_PI` / `INV_PI` / `PI_SQUARED` | `UE_HALF_PI` / `UE_TWO_PI` / `UE_INV_PI` / `UE_PI_SQUARED` |
| `MAX_FLT` | `UE_MAX_FLT` |
| `EULERS_NUMBER` | `UE_EULERS_NUMBER` |
| `FLOAT_NON_FRACTIONAL` | `UE_FLOAT_NON_FRACTIONAL` |
| `DOUBLE_*` 전체 | `UE_DOUBLE_*` |

**실제 겪은 사례 (2026-08-10)**: `AS1Player::TickRemotePlayer` 작성 중 `KINDA_SMALL_NUMBER`를 쓰면 deprecation 경고가 발생. `UE_KINDA_SMALL_NUMBER`로 교체함.

---

## 🔴 LWC — 기본 수학 타입이 `double` 입니다

UE5부터 Large World Coordinates로 전환되어, **접미사 없는 타입은 전부 `double` 기반**입니다.
`MathFwd.h:47-57` 에서 확인:

```cpp
using FVector    = UE::Math::TVector<double>;
using FVector2D  = UE::Math::TVector2<double>;
using FQuat      = UE::Math::TQuat<double>;
using FMatrix    = UE::Math::TMatrix<double>;
using FPlane     = UE::Math::TPlane<double>;
using FTransform = UE::Math::TTransform<double>;
using FBox       = UE::Math::TBox<double>;
using FRotator   = UE::Math::TRotator<double>;
```

| 함정 | 결과 | 대처 |
|---|---|---|
| `float f = GetActorRotation().Yaw;` | **C4244 축소 변환 경고** | `static_cast<float>(...)` |
| `float x = GetActorLocation().X;` | 동일 | 동일 |
| `float`/`double` 혼용 산술 | 암묵적 승격 | 타입을 통일 |

**명시적 정밀도가 필요하면**: `FVector3f`(float) / `FVector3d`(double), `FRotator3f` / `FRotator3d`

**실제 겪은 사례**: `float InterpStartYaw = GetActorRotation().Yaw;` 에서 경고. `static_cast<float>` 추가로 해결.

> **또한**: `FRotator::Yaw`는 **멤버 변수**입니다. `Yaw()` 처럼 괄호를 붙이면 컴파일 에러입니다.

---

## 🔴 Character / CharacterMovement — MovementBase 인터페이스 전환

**5.8 최대 변경 클러스터.** `Character.h` 16건 + `CharacterMovementComponent.h` 16건.

`UPrimitiveComponent` 포인터로 이동 베이스를 다루던 방식이 **`FMovementBaseInterfaceData` 구조체 기반**으로 바뀌었습니다.

| Deprecated | Replacement | 위치 |
|---|---|---|
| `ACharacter::GetMovementBase()` | `GetMovementBaseObject()` 또는 `GetMovementBaseInterfaceData()` | `Character.h:797` |
| `MovementBaseUtility::*` (UPrimitiveComponent 버전, 12개 오버로드) | `FMovementBaseInterfaceData` 받는 버전 | `Character.h:151-200` |
| `ACharacter::BasedMovement` 관련 프로퍼티 | `MovementBaseInterfaceData` | `Character.h:293` |
| `ACharacter` 의 이동 베이스 프로퍼티 | `PhysicsObjectOwner` + `FMovementBaseInterfaceData` | `Character.h:90` |
| `UCharacterMovementComponent::GetLastServerMovementBase()` | `GetLastServerMovementBaseInterfaceData()` | `CharacterMovementComponent.h:906` |
| `UCharacterMovementComponent::LastMovementBase` | `LastMovementBaseInterfaceData` | `CharacterMovementComponent.h:922` |
| `UCharacterMovementComponent::GetMovementBase()` | `GetMovementBaseObject()` (필요 시 캐스트) | `CharacterMovementComponent.h:1403` |

> **이 프로젝트 영향: 없음.** `S1/Source/S1` 전체 스캔 결과 위 API 사용 0건.
> 다만 **움직이는 발판·엘리베이터·이동하는 배** 같은 걸 구현하면 반드시 마주칩니다.
> 그때는 처음부터 `FMovementBaseInterfaceData` 쪽 API를 쓰세요.

---

## 🟡 Enhanced Input — Combo Trigger 폐기

| Deprecated | 위치 | 비고 |
|---|---|---|
| `UInputTriggerCombo` | `InputTriggers.h:571` | 클래스 전체 deprecated |
| `FInputComboStepData` | `InputTriggers.h:530` | |
| `FInputCancelAction` | `InputTriggers.h:551` | |

Enhanced Input 자체는 **여전히 표준**입니다. 콤보 트리거만 빠집니다.
콤보 입력이 필요하면 어빌리티/스테이트 머신 레이어에서 직접 처리하세요.

> **이 프로젝트 영향: 없음.** `IA_Move`/`IA_Look`/`IA_Jump`/`IA_MouseLook` 만 사용 중.

---

## 🟡 Components

| Deprecated | Replacement | 위치 |
|---|---|---|
| `USkeletalMeshComponent::bCollideWithAttachedChildren` | `CollisionSources` | `SkeletalMeshComponent.h:807` |
| `CopyClothCollisionsToChildren()` / `CopyChildrenClothCollisionsToParent()` / `FindClothCollisions()` | `CollisionSources` | `SkeletalMeshComponent.h:2629-2637` |
| `UPrimitiveComponent` 터치 이벤트 (구 오버로드 3종) | Touch ID + PlayerController + HitResult 받는 오버로드 | `PrimitiveComponent.h:3228-3239` |
| `UInstancedStaticMeshComponent::InstanceBodies` | `GetInstanceBodies()` | `InstancedStaticMeshComponent.h:524` |
| ISM 네비게이션 바운드 수동 갱신 | **불필요** — lazy 갱신으로 변경 | `InstancedStaticMeshComponent.h:717` |
| `USplineComponent::SynchronizeSplines()` / `PopulateFromLegacy()` | — | `SplineComponent.h:1013-1016` |
| `USplineComponent::SetFloatPropertyAtSplineInputKey()` | `AddFloatPropertyAtSplineInputKey()` | `SplineComponent.h:1019` |
| `UActorComponent::AsyncRegisterLevelContext` | **제거됨** | `ActorComponent.h:79` |

---

## 🟡 Actor

| Deprecated | Replacement | 위치 |
|---|---|---|
| 항상 보이는 컴포넌트 카운트 (구 방식) | `HasAlwaysVisibleComponents()` / `IncrementAlwaysVisibleComponents()` / `DecrementAlwaysVisibleComponents()` | `Actor.h:155` |
| `AActor` 의 DataLayer 구 프로퍼티 | `DataLayerAssets` | `Actor.h:1115` |
| `FActorDataLayer` | **사용 안 함** | `Actor.h:1500` |

---

## 입력 시스템 (UE4 레거시 — 여전히 금지)

| Deprecated | Replacement |
|---|---|
| `InputComponent->BindAction()` (레거시) | Enhanced Input `UEnhancedInputComponent::BindAction()` |
| `InputComponent->BindAxis()` | Enhanced Input `UInputAction` (Axis2D) |
| `PlayerController->GetInputAxisValue()` | `FInputActionValue` |

이 프로젝트는 이미 Enhanced Input을 씁니다 (`DefaultInput.ini`: `DefaultInputComponentClass=/Script/EnhancedInput.EnhancedInputComponent`).

---

## 월드 빌딩 (UE4 → UE5)

| Deprecated | Replacement |
|---|---|
| World Composition | World Partition |
| Level Streaming Volumes | World Partition Data Layers |

---

## ⚠️ 이 문서에 없는 것을 확인하는 법

```bash
# 특정 API가 deprecated 인지
grep -rn "UE_DEPRECATED" /c/Unreal5.8/UE_5.8/Engine/Source/Runtime --include=*.h | grep "SetActorLocation"

# 특정 헤더의 5.8 deprecation 전부
grep -n "UE_DEPRECATED(5.8," /c/Unreal5.8/UE_5.8/Engine/Source/Runtime/Engine/Classes/GameFramework/Pawn.h

# 프로젝트 코드 감사
grep -rn "의심API" /c/Server/MMO/S1/Source/S1 --include=*.h --include=*.cpp
```

**추측하지 마세요.** 이 엔진 버전은 LLM 학습 데이터 밖입니다.
