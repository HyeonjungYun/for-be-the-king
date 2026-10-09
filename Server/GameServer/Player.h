#pragma once
#include "Creature.h"
#include "MoveClock.h"
#include "EquipStats.h"

class GameSession;
class Room;

struct ItemEntry
{
	uint64				instanceId = 0;
	uint32				itemTypeId = 0;
	Protocol::ItemGrade	grade = Protocol::ITEM_GRADE_NONE;
	uint32				level = 0;

	uint32				skillIdPrimary = 0;
	uint32				skillIdSecondary = 0;

	// 이번 런에 서버가 만들었다(상자 · 롤) - DB 행이 아직 없다. 탈출 · 사망 커밋이 UPDATE 대신 INSERT 한다.
	// 커밋으로 행이 생기면 false 로 돌린다. 메모리 전용이라 클라로 보내지 않는다.
	bool				isNew = false;

	Protocol::ItemInstance ToProto(Protocol::ItemState state, Protocol::EquipSlot slot = Protocol::SLOT_NONE) const;
};

class Player : public Creature
{
public:
				Player();
	virtual		~Player();

public:
	float		GetEffectiveMoveSpeed() const;
	float		GetSpeedCeiling(uint64 nowUs) const;

private:
	float		ComputeMoveSpeed(float slow)const;

public:
	void	RebuildSkillSlotsFromEquipment();

	// 착용 6칸 → 전투 스탯 (EquipStats). refill = 입장 - 체력을 새 최대치로 채운다.
	// 아니면 체력 비율을 유지한다. 체력이나 최대 체력이 바뀌었으면 true - 부른 쪽이 S_DAMAGE 0 (+ max_hp) 으로 알린다
	bool	ApplyEquipmentStats(bool refill);

	// 장비 합산(equipTotals) 위에 스킬 버프(status)를 얹어 전투 스탯을 다시 만든다. 최대 체력은 건드리지 않는다.
	// 비율 버프는 가산 뒤 1회 곱 (entities.yaml 과 같은 원칙) - 공격력 · 마력 · 방어 · 마법 저항 × (1 + 합), 치명 확률 · 공속 · 이속은 + 합
	void	RebuildCombatStats();
	virtual void OnBuffsChanged() override { RebuildCombatStats(); }

	// 슬롯이 든 아이템의 강화 레벨 - 보조 무기 슬롯은 주무기 아이템의 것. 비었으면 0
	uint32	ItemLevelOfSlot(Protocol::EquipSlot slot) const;

	ItemEntry* FindInInventory(uint64 instanceId);
	bool IsInventoryFull() const { return inventory.size() >= INVENTORY_SLOT_COUNT; }

public:
	weak_ptr<GameSession>	session;
	
public:
	uint64					lastMoveUs = 0;
	double					moveBudget = 0.0;
	MoveClock				moveClock;		// 이동 검증 경과 시간 - C_MOVE.client_ms 기준 (MoveClock.h)
	float					moveExceptionSpeed = 0.f;
	uint64					moveExceptionEndUs = 0;

	uint64 lastSaveUs = 0;

public:
	int32					nakedMaxHp = BaseStats::MAX_HP;		// characters.max_hp - 장비 합산의 바닥
	float					percentMsBonus = 0.f;				// 장비 합산 + 이속 버프 - 이동 검증 상한에 들어간다
	EquipStatTotals			equipTotals;						// 마지막 장비 합산 - 버프가 바뀔 때 여기서 다시 출발한다

public:

	static constexpr int32	INVENTORY_SLOT_COUNT = 25;

	vector<ItemEntry>		inventory;
	ItemEntry				equipped[SKILL_SLOT_COUNT];
	vector<uint64>			droppedIds;
	uint64					detachedUntilUs = 0;

	// 진입 지점에서 시작한다 - Room::EnterRoom 이 그 둘레 빈자리로 옮기고 내린다 (entry-point.md AC-EP-04)
	bool					spreadOnEnter = false;

public:
	// ── 런 (design/gdd/escape-timer.md) ── Room JobQueue 안에서만 읽고 쓴다
	static constexpr int32	MAX_TOWERS = 4;

	uint64					runStartUs = 0;			// 방에 놓인 순간 (EnterRoom). 재접속은 바꾸지 않는다
	uint64					runDeadlineUs = 0;		// 0 = 런 밖
	uint64					runGold = 0;			// 던전 메모리 골드 - MVP 는 /gold 로만 는다 (Core Rule 18)

	// 진입 직후 무적 (game-concept AC-10 · registry spawn_invulnerability_duration 3 s) - 이동 · 공격 · 스킬로 즉시 풀린다.
	// 그동안 피해 · CC · 몬스터 감지의 대상이 아니다. 0 = 무적 아님
	uint64					spawnInvulnUntilUs = 0;
	bool					IsSpawnInvulnerable(uint64 nowUs) const { return spawnInvulnUntilUs != 0 && nowUs < spawnInvulnUntilUs; }
	uint32					entryPointId = 0;
	int32					zoneTower = -1;			// 점유 중인 타워 순번, -1 = 없음 (이력 판정용)
	uint8					gaugeAudience[MAX_TOWERS] = {};		// EscapeLogic::Audience - 집합에 새로 들어왔는지 본다
	uint64					lastGaugeSentUs[MAX_TOWERS] = {};

	// 텔레메트리 (§ G) - 런 시작에 0
	uint32					statBroughtInSum = 0;
	uint32					statZoneVisits = 0;
	uint32					statZoneLeavesVoluntary = 0;
	uint32					statZoneLeavesForced = 0;
	uint64					statZoneDwellUs = 0;
	uint32					statGaugeReversals = 0;
	uint64					statAlertDwellUs = 0;
	string					statLastTowerId;
};
