# Active Session State

<!-- STATUS -->
Epic: Concept & Systems Design
Feature: 넷코드 Phase 0-B PoC
Task: [사용자가 서버 C++ 직접 작성 중] → 이후 PoC 하네스 S1~S4
<!-- /STATUS -->

<!-- 📍 세션 마무리 지점 (2026-07-19): 사용자가 서버 코드를 직접 작성하기로 함. 이 세션 종료.
     재개 시: 사용자가 짠 서버 코드 검토/자문 + PoC 하네스 협업.
     서버 실행법: Binaries\Win64\DungeonKingServer.exe /Game/TopDown/Lvl_TopDown -log -port=7777
       클라 접속: 에디터 PIE 콘솔 `open 127.0.0.1:7777` 또는 DungeonKing.exe 127.0.0.1:7777 -game
       빠른 반복: 에디터 Play 드롭다운 → Net Mode "Play As Client" + N players
     ⚠️ 미해결: .uproject는 런처엔진(5.8) 연결인데 서버빌드는 소스엔진(C:\UnrealEngine).
        일관 루프 위해 소스엔진 등록+재지정 권장(로드맵 6단계). -->


<!-- ✅✅ 이정표 완료 (2026-07-19): UE5.8 소스엔진 빌드 성공 + DungeonKingServer.exe(312MB) 생성.
     "Server targets not supported" 에러 해소 = 데디케이티드 서버 타겟 빌드 가능 확정.
     절전 15분 복원 완료. 넷코드 PoC 실행 기반 완성.
     다음: Phase 0-B 비교 하네스(Iris vs RepGraph, 16 NetConn, 시나리오4종) 또는 GAS 채택 ADR. -->


<!-- ✅ 이정표 (2026-07-19): UE5.8 소스엔진 빌드 성공(UnrealEditor.exe 생성, ~4.3h).
     DungeonKingServer 타겟 빌드가 "not supported" 에러 없이 정상 컴파일 시작 [996 액션] = 서버타겟 지원 확인!
     남은 자동작업(YOLO): 서버타겟 빌드 완료 확인 → 절전 15분 복원 → 결과보고 -->


<!-- ⏰ 빌드 완료 후 필수 작업: 절전 복원 (사용자 요청 2026-07-19)
   빌드 중 절전을 끔(powercfg /change standby-timeout-ac 0 등, AC/DC standby+hibernate 전부 0).
   사용자 지정: 절전 15분으로 복원. 완료 후 실행:
     MSYS_NO_PATHCONV=1 powercfg /change standby-timeout-ac 15; powercfg /change standby-timeout-dc 15; powercfg /change hibernate-timeout-ac 180; powercfg /change hibernate-timeout-dc 30 -->

<!-- 빌드 복구 노트 (세션 재시작으로 빌드 죽으면):
1. 정리: taskkill /F /IM dotnet.exe /T; taskkill /F /IM cl.exe /T; taskkill /F /IM UnrealBuildTool.exe /T (잔여 프로세스 상호간섭이 UBA 정체 유발)
2. 재실행: cd /c/UnrealEngine && Build.bat UnrealEditor Win64 Development -WaitMutex (run_in_background, 증분캐시 보존됨)
3. cl.exe 프로세스 뜨고 [N/M] 오르면 정상. "UbaServer Listening"에서 멈추고 cl.exe 0개면 잔여프로세스 재정리 필요
4. 완료(EXIT 마커) 후: Build.bat DungeonKingServer Win64 Development -Project="...DungeonKing.uproject" 로 서버타겟 검증
※ 스케줄러/-NoUBA 시도는 효과없었음. 클린 정리 후 원래 명령이 정답. -->


## Current Task
넷코드 PoC 재개. 백본 결정을 ADR로 선행 문서화 완료. 다음: ADR의 Day-0 선행 스파이크
(Iris 스페이셜 필터 실재 확인) → 비교 PoC 하네스 구축.

### ⚠️ 핵심 발견 (2026-07-18, 웹 검증)
- UE 5.8: **Iris 리플리케이션 프로덕션 레디**, **ReplicationGraph는 deprecated/레거시**
- 로드맵의 "ReplicationGraph 전제"가 뒤집힘 → ADR-0001은 Iris vs RepGraph **실측 비교 후 채택**으로 결정
- 백본 선택은 Phase 0-B PoC 게이트로 확정(현재 ADR Status=Proposed)

### Day-0 스파이크 결과 (완료)
- ✅ Iris 내장 `UNetObjectGridFilter` 실재 → 커스텀 필터 불필요 → A/B 동일공수 비교 전제 성립
- ⚠️ 거리 전용, LOS 없음 → 방-가시성 레이어 두 백본 공통 추가(대칭)
- ⚠️ 함정: 데디 서버 스프링암/카메라 틱 비활성화(~1ms/frame), 그리드 셀 크기 config 키는 엔진 소스 확인 잔여

## Progress Checklist
- [x] 게임 컨셉 임포트 (`design/gdd/game-concept.md`)
- [x] 디자인 리뷰 (full 3-에이전트) → NEEDS REVISION
- [x] /brainstorm 개선: 사망 페널티·스케일·솔로("고독한 길")·시체루팅·시즌제 확정 → 사실상 APPROVED
- [x] 로드맵 작성·갱신 (`production/roadmap.md`)
- [x] 시스템 분해 (`design/gdd/systems-index.md`) — 29개 시스템, 의존맵, 설계순서, 고위험표
- [x] 입력 시스템 GDD 작성 완료 (design/gdd/input-system.md, Designed — 리뷰 대기)
- [ ] MVP GDD 계속 (설계순서: 이동&카메라 → 전투 → 아이템 → …)
- [ ] Phase 0-B 넷코드 PoC (병행 권장)

## Key Decisions (v2, 2026-07-18)
- 사망: 아이템 가방 드랍(10분 루팅) + 부분사망(구역 바닥) + 스킬포인트 부분 보존 + 길드 구출 수수료
- 스케일: 심리스 12~16인 (Phase 0-B 실측으로 확정)
- 솔로: "고독한 길" 1급 경로 (솔로 전용 콘텐츠 + 자급자족 빌드), 협동은 선택적 고천장
- 배신 낙인: 전역 → **로컬**(배신당한 파티원에게만 표시). 성기사 시그니처 유지. 바운티/평판 제거
- 인구 순환: 시즌 & 프레스티지제 (하이브리드 리셋)

## Files Modified This Session
- design/gdd/game-concept.md — v2 개선 반영 (§0/§3/§5)
- production/roadmap.md — 확정 결정으로 갱신
- design/gdd/systems-index.md — 신규 (29 시스템)

## Open Questions (밸런싱/후속 단계)
- "farm 후 배신" 익스플로잇 방지책 (협동 보너스 공동탈출 정산?)
- 시즌 길이·영구보존 자원 범위·프레스티지 보상
- 각종 수치(구역 하락 폭·스킬포인트 보존율·구출 수수료·보험료)
- [x] 컨셉 §3/§4/§5 구버전 사망 페널티·배신 낙인 잔재 청소 완료 (2026-07-18)

## Files Modified (넷코드 세션, 2026-07-18)
- docs/architecture/adr-0001-netcode-replication-backbone.md — 신규(Proposed) + Day-0 결과 기입
- docs/registry/architecture.yaml — 2건 추가(client_side_fow_masking 금지, fow_relevancy_decision 계약)
- Source/DungeonKingServer.Target.cs — 신규(Dedicated Server 빌드 타겟, TargetType.Server)
- design/gdd/systems-index.md — 넷코드 정합: "Replication Graph 확정" → "백본 미확정(Iris vs RepGraph, ADR-0001)" 2곳 (game-concept은 백본 미명시라 수정 불필요)
- design/gdd/movement-camera.md — **완성(Designed, 8섹션 전부)**. CMC·탑다운SpringArm·서버권위·WASD+커서페이싱·기본회피없음(스킬로)·고정팔로우카메라. 공식5개(qa-lead 승인기준 29개). 리뷰 대기(/design-review 새 세션)
- design/registry/entities.yaml — 크로스시스템 공식 2개 등록(effective_move_speed, dash_kinematics)
- design/gdd/systems-index.md — 이동&카메라 상태 Not Started→Designed, 문서 링크
- design/gdd/combat-system.md — **완성(Designed, 8섹션)**. GAS유력·서버권위·물리/마법2종·크리·CC(Stun/Root/Slow/Silence)+DR·점감완화(armor/(armor+K)). 공식6개(systems-designer)·승인기준33개(qa-lead). 리뷰 대기
- design/registry/entities.yaml — 전투 공식 4개 등록(damage_dealt·crit_damage·attack_speed_stacking·cc_duration_dr) + effective_move_speed에 combat 참조·Slow노트
- design/gdd/systems-index.md — 전투 #3 Not Started→Designed
- docs/architecture/adr-0002-gas-adoption.md — 신규(Proposed). GAS 채택 확정: ASC on Character·서버전용 데미지/CC·예측은 발동만·DR(ASC트래커+SetByCaller)·Push Model. ue-gas-specialist 검증 반영(BLOCKING 9건)
- docs/registry/architecture.yaml — 4스탠스 추가(character_attributes·cc_authority 소유, combat_ability_framework=GAS, predict_authoritative_gameplay 금지)
- design/gdd/combat-system.md — Open Q "GAS 채택" 해결 표기(ADR-0002)
- ✅ 서버타겟 검증 완료(DungeonKingServer.exe) + 절전 15분 복원됨
- 후속 ADR 필요: 스킬샷 타겟데이터, 리스폰 ASC/로드아웃 생명주기

### ⚠️ 서버타겟 빌드 검증 결과 (2026-07-18)
- `DungeonKingServer` UBT 빌드 실패: "Server targets are not currently supported from this engine distribution" (EXIT 6)
- 원인: 현 UE5.8은 런처 설치본(`InstalledBuild.txt` 존재). **데디 서버 타겟 = 소스 엔진 필수**
- 환경: C: 1.1TB 여유, git 2.54, VS2022 Community 설치됨 — 소스 빌드 가능
- 결정: **UE5.8 소스 엔진 빌드 진행** (사용자 선택)

## Next — UE5.8 소스 엔진 빌드
1. **[사용자] Epic↔GitHub 계정 연동** — github.com/EpicGames 접근 권한 (브라우저 계정 작업, 내가 못 함)
2. **[사용자/공동] 소스 취득** — `git clone -b 5.8 https://github.com/EpicGames/UnrealEngine.git` (GitHub 인증 필요)
3. **[내가 실행 가능] Setup.bat** → 의존성 다운로드(~50-60GB)
4. **[내가 실행 가능] GenerateProjectFiles.bat** → VS 솔루션 생성
5. **[내가 실행 가능] Build** — UnrealEditor Win64 Development (소스 빌드 = 서버타겟 지원 획득, 1~3시간)
6. 소스 엔진 등록 후 DungeonKing.uproject의 EngineAssociation 재지정 → `DungeonKingServer` 재빌드
7. 이후: 비교 PoC 하네스(16 NetConn + 몹/루팅, 시나리오 4종), Iris/RepGraph 경로

> ADR 커버리지 검증은 **새 세션에서 `/architecture-review`**

<!-- CONSISTENCY-CHECK: 2026-07-18 | GDDs checked: 2 | Conflicts found: 0 | Verdict: PASS -->
<!-- CONSISTENCY-CHECK: 2026-07-18 | GDDs checked: 3 | Conflicts found: 0 | Verdict: PASS (combat vs input/movement cross-refs verified) -->
