# 아카이브 — DungeonKing 설계 자산 (2026-07)

> **아카이브 일자**: 2026-08-10
> **사유**: 컨셉 전면 재작성 결정. 새 컨셉이 확정될 때까지 원본 보존.
> **상태**: 참조용. **이 폴더의 문서는 더 이상 권위값이 아닙니다.**

---

## 왜 아카이브했나

이 문서들은 `C:\Users\guswn\Documents\Unreal Projects\DungeonKing` 프로젝트에서 작성되어
2026-08-10에 이 저장소(`C:\Server\MMO`)로 이관됐습니다.

이관 후 두 가지가 드러났습니다.

**1. 기술 아키텍처가 완전히 다름**

| | 문서 전제 | 실제 |
|---|---|---|
| 서버 | UE 데디케이티드 서버 | **자체 C++ IOCP 서버** (`Server/`, 별도 프로세스) |
| 리플리케이션 | Iris vs ReplicationGraph 비교 후 채택 | **Protobuf over TCP 자체 프로토콜** — 둘 다 선택지에 없음 |
| UE 역할 | 서버 + 클라이언트 | **순수 클라이언트** (`S1/`) |
| GAS | 서버 ASC 권위 | 현재 미사용 |

`adr-0001-netcode-replication-backbone.md` 는 전제 자체가 무효라 `Superseded` 대상입니다.

**2. 컨셉 재작성 결정**

사용자가 기초 컨셉부터 다시 작성하기로 결정했습니다. 컨셉이 바뀌면 그 아래 시스템 분해·GDD·ADR이 모두 흔들리므로, 새로 쓰기 전에 원본을 보존합니다.

---

## 아카이브 내용

| 파일 | 상태 | 분량 |
|---|---|---|
| `game-concept.md` | v2 (2026-07-18 /brainstorm 개선 반영) | 컨셉 8개 절 |
| `systems-index.md` | Draft | 29개 시스템, 의존맵, 설계순서, 고위험표 |
| `input-system.md` | Designed (리뷰 대기) | 8섹션 완성, 공식 3개, 승인기준 15개 |
| `movement-camera.md` | In Design | 8섹션 완성, 공식 5개, 승인기준 29개 |
| `combat-system.md` | In Design | 8섹션 완성, 공식 6개, 승인기준 33개 |
| `entities.yaml` | — | 크로스시스템 공식 6개 등록 |

**공식 15개 이상, 승인 기준 77개.** 상당한 설계 자산입니다.

---

## 계승 가이드

새 컨셉이 **탑다운 액션**을 유지한다면 대부분 재사용 가능합니다.
전면 재작성이 아니라 **넷코드/GAS 전제 절만 수정**하면 됩니다.

| 문서 | 계승 가능성 | 수정 필요 지점 |
|---|---|---|
| `input-system.md` | **높음** | 거의 없음. Enhanced Input 기반이라 서버 구조와 무관. 이미 `S1/Source/S1/Game/`에 구현 진행 중 |
| `movement-camera.md` | **높음** | §Core Rules 9 "서버 권위(ADR-0001)" — CMC 내장 네트워크 예측 전제를 자체 서버 기준으로 재작성. 나머지 공식 5개는 그대로 유효 |
| `combat-system.md` | **중간** | Engine Framing의 "GAS 유력", Dependencies의 "R-GAS 교차", 서버권위 히트판정 절. 공식 D1~D6은 그대로 유효 |
| `systems-index.md` | **낮음** | 29개 시스템은 1인 4개월 기준으로 과함. #11 네트워킹 항목은 실제와 불일치. 재분해 권장 |
| `game-concept.md` | **선별** | 사망 페널티·시즌제·고독한 길 등 v2 개선 결정은 검토 가치 있음 |
| `entities.yaml` | **높음** | 계승하는 GDD의 공식만 골라 새 레지스트리로 이전 |

---

## 관련 문서

- 이행 계획: `docs/adoption-plan-2026-08-10.md`
- 폐기 예정 ADR: `docs/architecture/adr-0001-netcode-replication-backbone.md`
- 재검토 대상 ADR: `docs/architecture/adr-0002-gas-adoption.md`
- 이전 세션 상태: `production/session-state/archive/dungeonking-netcode-2026-07-19.md`
- 현재 프로젝트 상태: `production/session-state/active.md`
