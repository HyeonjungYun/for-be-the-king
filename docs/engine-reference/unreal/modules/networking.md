# Networking — ⚠️ 이 프로젝트는 UE 네트워킹을 사용하지 않습니다

**Last verified:** 2026-08-11 (코드 직접 대조)
**이전 버전:** 이 문서는 5.7 기준 UE 내장 리플리케이션 안내서였습니다. **폐기하고 교체했습니다.**

> **2026-08-11 정정**: 워커 스레드 수(6→5)와 JobQueue 사용 여부("미사용"→**라이브**)가 틀렸습니다.
> 권위 모델·대역폭 재계산 필요성·AOI 실제 상태를 추가했습니다.

---

## 🔴 먼저 읽으세요

**이 프로젝트는 Unreal의 리플리케이션 시스템을 일절 쓰지 않습니다.**

| 쓰지 않는 것 | 이유 |
|---|---|
| `bReplicates = true` | 자체 프로토콜 사용 |
| `DOREPLIFETIME` / `GetLifetimeReplicatedProps` | 동일 |
| `UFUNCTION(Server/Client/NetMulticast)` RPC | 동일 |
| `IsNetRelevantFor` / relevancy 시스템 | AOI를 서버가 직접 계산 |
| **Iris** 리플리케이션 | 선택지에 없음 |
| **ReplicationGraph** | 선택지에 없음 |
| Dedicated Server 타겟 (`TargetType.Server`) | 서버가 UE가 아님 |
| `ACharacter` 의 CMC 네트워크 예측 (`SavedMove` 등) | 원격 프록시는 시뮬레이션 안 함 |

> **AI 에이전트 주의**: 넷코드 관련 자문 시 `bReplicates`·RPC·Iris 필터를 제안하지 마세요.
> 이 프로젝트에서는 **컴파일은 되지만 아무 의미가 없습니다.**

---

## 실제 아키텍처

```
┌─────────────────────────┐         ┌──────────────────────────────┐
│  S1/  (Unreal 5.8)      │  TCP    │  Server/  (자체 C++ IOCP)    │
│  순수 클라이언트          │◀───────▶│  별도 프로세스 · UE 무관       │
│                         │Protobuf │                              │
│  PacketSession          │         │  IocpCore · Session · Service│
│  RecvWorker/SendWorker  │         │  Room · Player               │
│  (FRunnable 전용 스레드)  │         │  JobQueue · GlobalQueue      │
│  ClientPacketHandler    │         │  ServerPacketHandler         │
│  S1GameInstance         │         │                              │
└─────────────────────────┘         └──────────────────────────────┘
              ▲                                    ▲
              └──── Protocol.proto ────────────────┘
                    PacketGenerator가 양측 핸들러 자동 생성
```

### 코드 위치

| 역할 | 경로 |
|---|---|
| 클라 소켓 계층 | `S1/Source/S1/Network/` |
| 클라 패킷 처리 | `S1/Source/S1/ClientPacketHandler.cpp` |
| 서버 코어 (정적 lib) | `Server/ServerCore/` |
| 서버 게임 로직 | `Server/GameServer/` |
| 부하 테스트 | `Server/DummyClient/` |
| 프로토콜 정의 | `.proto` → `PacketGenerator` |

---

## 프로토콜

Protobuf over TCP. `[size(2)][id(2)][payload...]` 헤더 형식.

**구현된 패킷 12종** (`Server/GameServer/ServerPacketHandler.h`)

| ID | 패킷 | ID | 패킷 |
|---|---|---|---|
| 1000 | `C_LOGIN` | 1001 | `S_LOGIN` |
| 1002 | `C_ENTER_GAME` | 1003 | `S_ENTER_GAME` |
| 1004 | `C_LEAVE_GAME` | 1005 | `S_LEAVE_GAME` |
| — | — | 1006 | `S_SPAWN` |
| — | — | 1007 | `S_DESPAWN` |
| 1008 | `C_MOVE` | 1009 | `S_MOVE` |
| 1010 | `C_CHAT` | 1011 | `S_CHAT` |

패킷 추가 시 **`.proto` 만 수정**하고 제너레이터를 돌립니다. 양쪽 핸들러를 손으로 고치지 마세요 — 규약 불일치의 원인이 됩니다.

---

## 클라이언트 측 UE 관련 주의사항

UE 리플리케이션을 안 쓰기 때문에 생기는 고유 규칙입니다.

### 원격 플레이어 = 시뮬레이션 금지, 재생만

```cpp
// AS1Player::BeginPlay — 원격 프록시
GetCharacterMovement()->SetMovementMode(MOVE_None);
GetCharacterMovement()->bOrientRotationToMovement = false;
```

```cpp
// AS1Player::TickRemotePlayer
SetActorLocation(NewLocation, /* bSweep = */ false);   // ← false 필수
```

**`bSweep = false` 가 핵심입니다.** true로 두면 프록시가 자체 충돌 판정을 다시 하게 되고, 클라이언트마다 결과가 갈립니다. "벽에 막힌 판정"은 조작 주인의 클라이언트에서 이미 끝났습니다. 나머지는 재생만 합니다.

### 애니메이션 구동 — 수동 주입 필요

`MOVE_None` 이면 `CharacterMovement`가 `Velocity`·`Acceleration`을 갱신하지 않습니다.
`ABP_Unarmed`의 판정식이 둘 다 봅니다:

```
ShouldMove = (GroundSpeed > 0.01) AND (GetCurrentAcceleration() != 0)
```

따라서 둘 다 만들어줘야 합니다.

```cpp
// 1) Velocity — 직접 대입
GetCharacterMovement()->Velocity = FrameVelocity;

// 2) Acceleration — protected 라 직접 못 씀.
//    AddMovementInput 호출 → CMC가 ControlledCharacterMove에서 계산
//    (bRunPhysicsWithNoController = true 라 컨트롤러 없는 프록시도 이 경로를 탐)
//    MOVE_None 이라 PerformMovement는 조기 반환 → 실제 이동 없음
if (FrameVelocity.SizeSquared() > 1.f)
    AddMovementInput(FrameVelocity.GetSafeNormal(), 1.f);
```

**엔진 소스 근거** (`CharacterMovementComponent.cpp`)

| 행 | 내용 |
|---|---|
| 1751 | `bShouldPerformControlledCharMove = IsLocallyControlled() \|\| (!GetController() && bRunPhysicsWithNoController)` |
| 6451 | `Acceleration = ScaleInputAcceleration(ConstrainInputAcceleration(InputVector));` ← 여기서 채워짐 |
| 6457 | `PerformMovement(DeltaSeconds);` |
| 2791 | `if (MovementMode == MOVE_None \|\| ...) return;` ← 실제 이동은 안 함 |

`APawn::IsMoveInputIgnored()` (`Pawn.cpp:812`)가 `GetController() != nullptr && ...` 형태라, **컨트롤러 없는 프록시의 입력은 무시되지 않습니다.** `bForce = true` 불필요.

---

## AOI / 시야 — 직접 구현해야 합니다

UE가 주던 relevancy가 없습니다. 서버가 전부 계산합니다.

| 단계 | 방식 | 상태 (2026-08-11) |
|---|---|---|
| MVP (12주) | — | **FoW 없음.** 소수 클라이언트 검증에 불필요 → Tier 2로 연기 |
| Tier 2 | **클라이언트 FoW** (시각 마스킹만) | 계획. ⚠️ **월핵 가능** — 서버가 정보를 숨기지 않음 |
| 상시 | **거리 기반 컬링** | 🔴 **"필수"로 선언돼 있으나 구현 0줄.** 12주 일정에도 없음 |
| Tier 3 | **방 단위 PVS** — 집(방+복도)에 적용 | 계획. 여기서야 P4가 실제로 강제됨 |
| 필요 시 | 정밀 LOS 레이캐스트 | 보류 |

> **클라이언트 FoW는 P4를 검증하지 못합니다.** 클라이언트는 이미 전체 상태를 받고 화면에만 마스킹합니다.
> LoL의 대표적 치트가 맵핵인 것과 같은 구조입니다.

> **자체 서버에는 콜리전 지오메트리도 레이캐스트도 없습니다.** 정밀 LOS를 하려면
> 레벨 지오메트리 추출 + BVH + 레이캐스터를 전부 만들어야 합니다.
> 그래서 **방 단위 PVS**(오프라인 베이크 + O(1) 조회)가 절충안입니다.

### Room = 층

층 1개 = 서버 `Room` 1개. 브로드캐스트 범위가 층 단위로 제한됩니다.

**목표: 층당 30명 × 4층 = 120 동접** (2026-08-11 확정. 이전 목표는 400이었습니다).
현재 검증치는 3명.

```
1개 Room, 30명 : 30 × 29 × 20Hz = 초당 17,400 Send()   ← 단일 스레드로 충분
```

대역폭 추정 (위치 20Hz, 패킷 40B)

| 층당 인원 | 클라 1인 수신 | 서버 업로드 (층 1개) |
|---|---|---|
| **30 (확정)** | **186 kbps** | **5.6 Mbps** |
| ~~100~~ | ~~634 kbps~~ | ~~63 Mbps~~ |

> 🔴 **이 수치는 과소평가입니다.** 플레이어 위치 패킷만 계산했습니다. 누락된 것:
> **몬스터 위치 동기화 · 전투 패킷(스킬·데미지·CC) · 아이템 이벤트(드랍백 스폰/루팅/소멸).**
> 30명이라 여유가 크지만 **숫자 자체는 정확하지 않습니다.** 재계산 필요.

### ✅ 층 크기 확정 (2026-08-11)

```
층 280 m × 280 m  =  78,400 m²
30명 ÷ 78,400     =  2,613 m² 당 1명
시야 반경 40 m     =  5,027 m²
→ 균등 분포 시 동시 가시  ≈  1.9명
```

**개활지는 의도적으로 희박합니다.** 교전은 집(5개, MVP는 3개)에서 일어나고,
개활지는 이동·저수익 파밍·PvP 회피의 공간입니다. 상세는 `design/gdd/game-concept.md` § 공간.

### ⬇️ 인원 축소가 없앤 것

| 항목 | 100명 (이전) | **30명 (확정)** |
|---|---|---|
| 초당 `Send()` | 약 198,000 | **17,400** |
| **거리 컬링(AOI)** | **필수** | **선택 — 최적화 항목** |
| DummyClient 하네스 | 필수 | 있으면 좋음 |
| 서버 메모리 (총 동접) | 400명 → 256 MB | **120명 → 77 MB** |

**"AOI를 안 만들어도 된다"는 뜻은 아닙니다.** 30명 전원 상호 가시라도 단일 스레드가 감당하므로
**12주 일정 밖으로 미뤄도 안전하다**는 뜻입니다. `Room::Broadcast`는 여전히 거리 필터가 없습니다.

---

## 권위 모델 — 서버 권위 + 클라이언트 예측 (2026-08-11 확정)

```
클라  →  이동 입력 즉시 화면에 반영 (예측)  →  좌표를 서버에 전송
서버  →  검증:  Δ거리 ÷ Δ시간  ≤  340 cm/s × 1.15
           통과 → 브로드캐스트
           실패 → 거부 + 스냅백
```

> ⚠️ **"LoL은 클라이언트 권위"는 오해입니다.** LoL은 완전한 서버 권위이며,
> 클라가 하는 것은 **예측(prediction)**입니다. 위치의 진실은 항상 서버가 갖습니다.
> LoL에서 위치 핵이 통하지 않는 이유입니다.

**현재 코드는 이 모델이 아닙니다.** 클라가 보낸 좌표를 서버가 그대로 신뢰하며 **검증이 0줄**입니다.

```cpp
// ServerPacketHandler.cpp:83
// TODO : 이동 패킷이 진짜 플레이어 범위 내에서 온 것인지 Validation 체크
GRoom->DoAsync(&Room::HandleMove, pkt);
```

풀루팅 PvP에서 이는 **순간이동 강탈이 가능하다**는 뜻입니다.
이동속도 상수(**340 cm/s**, 라이즈 기준)가 확정되었으므로 속도 상한 검증은 **지금 구현 가능**합니다.

건드릴 곳: 위 TODO 자리 + `Room::HandleMove`.
~~선행 조건: `LockQueue::PopAll` 빌드 검증~~ → ✅ **2026-08-11 완료. 선행 조건 없음 — 바로 착수 가능.**

---

## 스레드 모델

**2026-08-11 코드 확인 결과** (이전 기술은 틀렸습니다 — 갱신했습니다)

| 항목 | 실제 | 근거 |
|---|---|---|
| 워커 스레드 | **5개** | `GameServer.cpp:47` `for (int32 i = 0; i < 5; i++)` |
| 메인 스레드 워커 참여 | **없음** | `GameServer.cpp:56` `//DoWorkerJob(service);` 주석 처리 |
| Room | **`class Room : public JobQueue`** | `Room.h:4` |
| 패킷 핸들러 | 전부 `GRoom->DoAsync(...)` | `ServerPacketHandler.cpp:85` 등 |

**JobQueue 전환은 이미 완료되어 라이브입니다.** "구현돼 있으나 미사용"이라는 이전 기술은 사실이 아닙니다.

> ✅ **`LockQueue::PopAll` 자기 재귀 락 — 해결 확인 (2026-08-11 03:39).**
> ServerCore → GameServer 순으로 재빌드 후 3인 접속·이동·퇴장에서 크래시 없음.
> 구현은 `PopNoLock()` 헬퍼 분리 방식 — 락 획득 지점이 `Pop`/`PopAll` 두 곳으로 모여 있고
> 내부 로직은 공유합니다.
>
> ⚠️ 단, **다중 스레드 스트레스 테스트는 아직 없습니다.** 정상 경로만 확인된 상태입니다.

### ⚠️ Room 분할과 JobQueue가 무엇을 푸는지 정확히 알 것

- **Room 4분할은 층 인원 문제에 적용 불가.** "층당 N명"은 애초에 *하나의 Room 안* 인원입니다.
  Room을 4개로 늘려도 **한 Room이 담당하는 부하는 그대로**입니다.
  Room 분할이 푸는 것은 **여러 층이 동시에 돌아갈 때의 병렬성**입니다.
- **JobQueue는 한 Room을 항상 단일 스레드로 실행합니다.** 락을 없애는 것이지 팬아웃을 없애는 게 아닙니다.

```
30명 × 20 Hz × 29명 팬아웃  ≈  초당 17,400회 Send()  — 단일 스레드로 충분
```

**30명에서는 단일 스레드 팬아웃이 병목이 아닙니다.** 100명이었다면 초당 198,000회로
컬링 없이는 성립하지 않았을 것입니다. **인원 축소가 이 문제를 없앴습니다.**

거리 컬링은 여전히 미구현이지만, 이제 **성능 필수 요건이 아니라 최적화 항목**입니다.

---

## 알려진 함정

| # | 내용 |
|---|---|
| 1 | **`GameServer.vcxproj`에 ServerCore 프로젝트 참조 0개.** GameServer만 빌드하면 `Libraries/Libs/ServerCore/Debug/ServerCore.lib` 구버전이 링크됨. **ServerCore 먼저 빌드할 것** |
| 2 | `USE_LOCK` = `std::mutex` (비재귀). 같은 스레드 재획득 시 `_RESOURCE_DEADLOCK_WOULD_OCCUR` 예외 |
| 3 | `Session::Send()`는 **락 안에서** `RegisterSend()`를 불러야 함. `RegisterSend` 내부 락이 주석 처리된 이유 |
| 4 | ~~`Room::Broadcast`가 보낸 사람에게도 감~~ → ✅ **해결됨** (2026-08-11 확인). `Room.h:27` 기본값 `exceptId = 0`, 호출부 3곳(`Room.cpp:52·101·141`) 전부 전달 중 |
| 5 | 🔴 **`GameSession::OnDisconnected`가 Room에서 안 나감** → 유령 플레이어 누적. `GSessionManager.Remove()`만 부르고 `Room::Leave`를 부르지 않음 — **미수정** |
| 6 | 🔴 **`Room::Broadcast`에 거리 필터가 없음.** 방 전원에게 무조건 전송. AOI 미구현 — **12주 일정에도 없음** |
| 7 | 🔴 **서버 이동 검증 0줄.** `ServerPacketHandler.cpp:83` TODO 그대로 — **미수정** |

---

## UE 내장 네트워킹 참고가 필요하다면

이 프로젝트에서는 쓰지 않지만, 비교·학습 목적이라면 공식 문서를 보세요.
**이 저장소의 문서를 근거로 삼지 마세요** — 프로젝트 아키텍처와 상충합니다.

- https://dev.epicgames.com/documentation/en-us/unreal-engine/networking-and-multiplayer-in-unreal-engine
- Iris: `C:\Unreal5.8\UE_5.8\Engine\Source\Runtime\Net\Iris` (5.8에서 Experimental → Runtime 승격)
