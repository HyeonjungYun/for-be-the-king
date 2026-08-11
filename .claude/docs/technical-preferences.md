# Technical Preferences

<!-- Populated by /setup-engine. Updated as the user makes decisions throughout development. -->
<!-- All agents reference this file for project-specific standards and conventions. -->

## Engine & Language

- **Engine**: Unreal Engine 5.8
- **Language**: C++ (primary), Blueprint (gameplay prototyping)
- **Rendering**: Lumen (global illumination + reflections), Nanite (virtualized geometry)
- **Physics**: Chaos Physics

## Input & Platform

<!-- Written by /setup-engine. Read by /ux-design, /ux-review, /test-setup, /team-ui, and /dev-story -->
<!-- to scope interaction specs, test helpers, and implementation to the correct input methods. -->

- **Target Platforms**: PC (Steam / Epic)
- **Input Methods**: Keyboard/Mouse, Gamepad
- **Primary Input**: Keyboard/Mouse
- **Gamepad Support**: Partial
- **Touch Support**: None
- **Platform Notes**: Primary input is keyboard/mouse; gamepad is a supported secondary. Use Enhanced Input with input contexts that support both. Reconfirm Primary Input once the game genre is locked (action/3rd-person titles may flip to gamepad-first).

## Naming Conventions

- **Classes**: Prefixed PascalCase (`A` for Actor, `U` for UObject, `F` for struct, `E` for enum, `I` for interface) — e.g., `APlayerCharacter`, `UHealthComponent`
- **Variables**: PascalCase (e.g., `MoveSpeed`)
- **Booleans**: `b` prefix (e.g., `bIsAlive`)
- **Functions/Events**: PascalCase (e.g., `TakeDamage()`); multicast delegates `On<Event>` (e.g., `OnHealthChanged`)
- **Files**: Match class without prefix (e.g., `PlayerCharacter.h` / `PlayerCharacter.cpp`)
- **Assets/Blueprints**: Type-prefixed (`BP_` Blueprint, `WBP_` Widget Blueprint, `M_` Material, `T_` Texture, `SM_` Static Mesh, `SK_` Skeletal Mesh)
- **Constants**: PascalCase (`static constexpr`) or UPPER_SNAKE_CASE for `#define` macros

## Performance Budgets

이 프로젝트는 **프로세스가 둘**입니다. 예산도 둘로 나뉩니다.

### 클라이언트 (`S1/` — Unreal Engine 5.8)

- **Target Framerate**: 60 fps
- **Frame Budget**: 16.6 ms (game thread + render thread must each stay under budget)
- **Draw Calls**: ≤ 3000 per frame (Nanite reduces effective draw call cost; verify with `stat RHI`)
- **Memory Ceiling (RAM)**: **6 GB** — Steam 하드웨어 설문 중간값(16 GB) 기준. 검증: `stat memory`
- **Memory Ceiling (VRAM)**: **4 GB** — 중간값(8 GB) 기준. 검증: `stat rhi`
  - 탑다운 고정 시점이라 카메라가 멀고 캐릭터가 작게 보임 → 텍스처 해상도 요구가 낮음. 이 예산은 여유로운 편
  - 이 수치에서 텍스처 최대 해상도·폴리곤 예산·LOD 단계를 역산할 것 (`/art-bible`)

### 서버 (`Server/` — 자체 C++ IOCP)

> **2026-08-11 재계산.** 동접 목표가 **400 → 120** 으로 바뀌었습니다 (층당 100 → **30명** × 4층).
> 탑다운은 인원이 많을수록 화면 판독이 무너지므로 게임 디자인 판단으로 낮췄습니다.

- **Memory Ceiling**: **512 MB @ 120 동접** (층당 30명 × 4층)
- 산출 근거:

  | 항목 | 값 | 출처 |
  |---|---|---|
  | 세션당 수신 버퍼 | 640 KB | `Session.h:21` BUFFER_SIZE 64KB × `RecvBuffer.h:9` BUFFER_COUNT 10 |
  | **120 동접 수신 버퍼 합** | **77 MB** | 400 동접 시 256 MB였음 |
  | SendBuffer 풀 · Player · Room · IocpEvent | ~45-105 MB (추정) | **미실측** |
  | **합계 예상** | **120~180 MB** | 여유 포함 상한 **512 MB** |

  > 메모리는 이제 병목이 아닙니다. 필요하면 **`RecvBuffer::BUFFER_COUNT` 축소**가 가장 쉬운
  > 최적화입니다(이동 패킷 40바이트에 세션당 640KB는 과합니다). 지금은 유지.

- **부하 관련 확정 수치** (`docs/engine-reference/unreal/modules/networking.md` 참조)

  전송 주기 **30 Hz** 확정 기준 (`design/gdd/movement-camera.md` § Core Rule 10)

  | 항목 | 값 |
  |---|---|
  | 클라 1인 수신 대역폭 | **278 kbps** |
  | 서버 업로드 (층 1개, 30명) | **8.4 Mbps** |
  | 초당 `Send()` 호출 (층 1개) | **26,100** — 단일 스레드로 충분 |
  | 거리 컬링(AOI) | **선택** — 성능 필수 요건이 아님 |

  > 🔴 **이 수치는 위치 패킷만 센 것이다.** 몬스터 위치 동기화 · 전투 패킷 · 아이템 이벤트가
  > 전부 빠져 있다. **하한선으로만 쓰고**, 매 Phase 게이트에서 증가분을 재측정할 것
  > (`production/roadmap.md` SV-5).
  >
  > ⚠️ 코드는 아직 `MOVE_PACKET_SEND_DELAY = 0.2f` (5 Hz)다 — 미반영.

- **Server Frame Budget**: 🔴 **정정 (2026-08-11 코드 확인)**

  ```cpp
  // Room.cpp:150 — 실제 틱
  DoTimer(100, &Room::UpdateTick);      // 100ms = 10Hz. 내용은 현재 cout 하나뿐
  ```

  **60 Hz 틱은 존재하지 않는다.** `HandleMove`는 틱이 아니라 **패킷 수신마다 `DoAsync`** 로 실행된다.
  "프레임당 예산"이라는 프레이밍 자체가 이 아키텍처와 맞지 않는다.

  → 측정 대상을 **`Room` JobQueue 1회 Flush 소요시간**으로 고정한다 (`production/roadmap.md` SV-1).
  통과 기준: **P95 < 16.6 ms · P99 < 33 ms · max < 100 ms**
  ⚠️ **계측 코드가 현재 0줄이다.**

## Testing

이 프로젝트는 실행 환경이 둘로 갈려 **프레임워크도 둘**입니다.

| 대상 | 프레임워크 | 산출물 | 속도 |
|---|---|---|---|
| `Server/` (순수 C++) | **GoogleTest** | `ServerCoreTests.exe` | 초 단위 |
| `S1/` (UE 의존) | **Unreal Automation Spec** | `UnrealEditor-Cmd` 헤드리스 | 분 단위 |

- **Framework**: GoogleTest (서버) + Unreal Automation Spec (클라이언트)
- **Minimum Coverage**: **`Server/ServerCore/` 라인 커버리지 70%**
  - 측정 도구: **OpenCppCoverage** (무료, PDB 기반 — 소스 수정 불필요)
    ```bash
    OpenCppCoverage.exe --sources Server\ServerCore --export_type html:coverage -- ServerCoreTests.exe
    ```
  - **포함**: `LockQueue` · `RecvBuffer` · `SendBuffer` · `BufferReader/Writer` · `JobQueue` · `GlobalQueue` · `JobTimer`
  - **제외**: 실제 소켓 I/O(`IocpCore`·`Listener`·`Session`의 WSA 호출) · `*.pb.cc` · `GameServer/` · `S1/`
  - **숫자가 아니라 리포트의 빨간 줄을 볼 것.** "이 줄이 프로덕션에서 실행될 수 있나?" 를 묻고, 그렇다면 테스트를 추가하고 아니면 죽은 코드로 보고 삭제한다
- **Required Tests**:
  - 밸런스 공식 (전투 데미지·이동속도·CC 지속시간)
  - 패킷 직렬화 왕복 (`.proto` 변경 시 회귀 방지)
  - **동시성 스트레스 테스트 (필수)** — 아래 참조

### 동시성 스트레스 테스트 — 필수 항목

단위 테스트는 데이터 레이스를 잡지 못합니다. 2026-08-10 세션에서 실제로 겪었습니다.

| 버그 | 단위 테스트로 잡히나 |
|---|---|
| `LockQueue::PopAll` 자기 재귀 락 | ✅ 잡힘 (단일 스레드에서도 즉시 예외) |
| `Session::Send` 락 범위 → `_sendQueue` 데이터 레이스 | ❌ **못 잡음** — 다중 스레드 동시 접근 필요 |

따라서 **공유 자료구조에는 다중 스레드 스트레스 테스트를 반드시 동반**한다.

```cpp
TEST(Session, 동시_Send가_큐를_깨뜨리지_않는다)
{
    auto session = MakeShared<TestSession>();
    std::vector<std::thread> threads;
    for (int i = 0; i < 8; i++)
        threads.emplace_back([&]{ for (int j = 0; j < 1000; j++) session->Send(MakeBuffer()); });
    for (auto& t : threads) t.join();
    // 크래시 없이 도달하면 통과
}
```

대상: `Session::_sendQueue` · `LockQueue` · `Room::_players` · `JobQueue` · `GlobalQueue`
확정적이지 않으므로 **CI에서 반복 실행**할 것.

### ⚠️ 테스트 프로젝트 빌드 주의

`ServerCoreTests`에 **ServerCore 프로젝트 참조를 반드시 설정**할 것.
현재 `GameServer.vcxproj`에 참조가 없어 구버전 `ServerCore.lib`가 링크되는 문제가 실제로 발생했다. 같은 실수를 반복하지 말 것.

## 🔴 작업 소유권 경계 (2026-08-11 확정)

**넷코드·서버는 사용자가 직접 작업한다. AI 에이전트는 제안만 하고 파일을 수정하지 않는다.**

| 영역 | 경로 | 소유 |
|---|---|---|
| **서버 전체** | `Server/**` — ServerCore · GameServer · DummyClient · PacketGenerator | 🔴 **사용자** |
| **클라 넷코드** | `S1/Source/S1/Network/**` · `ClientPacketHandler.{h,cpp}` · `S1GameInstance.{h,cpp}` | 🔴 **사용자** |
| **프로토콜 정의** | `Protocol.proto` · `Struct.proto` · `Enum.proto` | 🔴 **사용자** |
| 클라 게임플레이 | `S1/Source/S1/Game/**` (`S1Player`, `S1MyPlayer` 등) · `S1.{h,cpp}` | 에이전트 |
| 콘텐츠 | `S1/Content/**` — 에셋 · 블루프린트 · 레벨 · 머티리얼 | 에이전트 |
| 설정 | `S1/Config/**` | 에이전트 |
| 문서 | `design/` · `docs/` · `production/` | 에이전트 |

> **`.proto`가 사용자 소유인 이유**: 수정하면 `PacketGenerator`가 **서버·클라 양측 핸들러를 동시에 재생성**한다.
> 프로토콜은 서버와 불가분이므로 단일 소유자로 유지한다.

### 🔴 영역 파일을 건드려야 할 때 — 에이전트 출력 형식

**직접 수정하지 말고 아래 형식으로 전달한다.**

```
파일 · 줄 번호       Server/GameServer/Room.cpp:127
현재 코드            (그대로 인용 — 사용자가 위치를 찾을 수 있게)
바꿀 코드            (복붙 가능한 완성형. 조각이나 의사코드 금지)
왜                   이 변경이 무엇을 고치는가
검증                 어떻게 확인하는가 (재현 절차 또는 테스트)
빌드 순서            ServerCore 선행 필요 여부
```

**규칙**

- 완성된 코드를 준다. "이런 식으로 하세요"는 도움이 안 된다
- 여러 파일이면 **적용 순서**를 명시한다 (컴파일이 깨지지 않는 순서)
- 사용자가 이미 고쳤을 수 있다 — **제안 전에 현재 코드를 먼저 읽어 확인한다**
- 빌드·실행 결과는 사용자가 알려준다. 추측해서 "됐을 것"이라고 쓰지 않는다

> **왜 이렇게 하나**: 서버는 이 프로젝트의 학습 목표이자 핵심 기술 자산이다.
> 자동 생성된 코드가 섞이면 사용자가 자기 코드를 완전히 파악하지 못하게 된다.

---

## Forbidden Patterns

<!-- Add patterns that should never appear in this project's codebase -->
- [None configured yet — add as architectural decisions are made]

## Allowed Libraries / Addons

<!-- Add approved third-party dependencies here -->
- [None configured yet — add as dependencies are approved]

## Architecture Decisions Log

<!-- Quick reference linking to full ADRs in docs/architecture/ -->
- [No ADRs yet — use /architecture-decision to create one]

## Engine Specialists

<!-- Written by /setup-engine when engine is configured. -->
<!-- Read by /code-review, /architecture-decision, /architecture-review, and team skills -->
<!-- to know which specialist to spawn for engine-specific validation. -->

- **Primary**: unreal-specialist
- **Language/Code Specialist**: ue-blueprint-specialist (Blueprint graphs) or unreal-specialist (C++)
- **Shader Specialist**: unreal-specialist (no dedicated shader specialist — primary covers materials)
- **UI Specialist**: ue-umg-specialist (UMG widgets, CommonUI, input routing, widget styling)
- **Additional Specialists**: ~~ue-gas-specialist~~ · ~~ue-replication-specialist~~ — **둘 다 이 프로젝트에 해당 없음** (아래 참조)
- **Routing Notes**: Invoke primary for C++ architecture and broad engine decisions. Invoke Blueprint specialist for Blueprint graph architecture and BP/C++ boundary design. Invoke UMG specialist for all UI implementation.

> 🔴 **호출하지 말 것 — 2건** (2026-08-11 확정)
>
> | 전문가 | 이유 |
> |---|---|
> | **ue-gas-specialist** | **GAS 미채택** (`ADR-0002` Rejected). 어빌리티·어트리뷰트는 서버 C++ 권위 + 공유 데이터 테이블로 구현한다. 클라는 연출만 재생 |
> | **ue-replication-specialist** | **UE 리플리케이션 미사용** (`ADR-0001` Rejected). 넷코드는 자체 IOCP 서버 + Protobuf/TCP이며 **🔴 사용자 소유 영역**이다 |
>
> 넷코드 질문은 전문가에게 넘기지 말고 § 작업 소유권 경계의 **제안 형식**으로 사용자에게 전달한다.
> `GameplayTags` 모듈은 GAS 없이 단독 사용 가능하므로 허용 — unreal-specialist가 담당한다.

### File Extension Routing

<!-- Skills use this table to select the right specialist per file type. -->
<!-- If a row says [TO BE CONFIGURED], fall back to Primary for that file type. -->

| File Extension / Type | Specialist to Spawn |
|-----------------------|---------------------|
| Game code (.cpp, .h files) | unreal-specialist |
| Shader / material files (.usf, .ush, Material assets) | unreal-specialist |
| UI / screen files (.umg, UMG Widget Blueprints) | ue-umg-specialist |
| Scene / prefab / level files (.umap, .uasset) | unreal-specialist |
| Native extension / plugin files (Plugin .uplugin, modules) | unreal-specialist |
| Blueprint graphs (.uasset BP classes) | ue-blueprint-specialist |
| General architecture review | unreal-specialist |
