// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "S1TestModeWidget.generated.h"

class UBorder;
class UTextBlock;
class UVerticalBox;

/**
 * 테스트 모드 패널 — design/quick-specs/test-mode-2026-10-11.md (사용자 2026-10-11 "더미 적을 세우거나 아이템과 스킬을
 * 마음대로 갈아끼워서 테스트 · 더미는 고정 / 몬스터 로직을 고를 수 있게 · 전부 UI 로").
 *
 *   테스트 모드                         F7 닫기 · F6 커서 위치에 소환
 *   ── 장비 ──
 *   무기          [◀]  검              [▶]
 *   Shift 스킬    [◀]  휘두르기        [▶]       ← 그 무기 풀의 스킬
 *   …  모자 · 갑옷 · 신발 · 장신구 (종류 + 스킬) · 등급 · 강화 레벨
 *   [장비 적용]
 *   ── 더미 적 ──   종류 · 수 · 행동(고정 / 몬스터 AI)   [앞에 소환] [모두 제거] [전부 고정] [전부 AI]
 *   ── 기타 ──      [체력 회복] [쿨다운 초기화] / [무적] [FoW] (FoW 는 내 시야만 - 2026-10-11 사용자 추가 요청)
 *   message line
 *
 * 🔴 Display and input only. Every button sends a "/t …" chat command; the server (FBTK_TEST_MODE=1) decides and the
 * result comes back the usual way — S_EQUIP_SYNC / S_INVENTORY_SYNC for gear, S_SPAWN for dummies. A server without
 * test mode ignores the commands, which this panel notices as "no S_EQUIP_SYNC within 1.5 s".
 *
 * The lists are read from the same Server/Data files the server loads: item_types.csv (via the item catalog),
 * skill_pools.csv and the requires_weapon column of skills.csv (read here — nothing else on the client needs them),
 * monster_types.csv (monster catalog).
 */
UCLASS()
class S1_API US1TestModeWidget : public UUserWidget
{
	GENERATED_BODY()

public:

	/** Reads the gear we wear into the rows, so "적용" without touching anything changes nothing. On every open. */
	void SyncFromEquipment();

	/** F6 — the selected dummies at a world point (the cursor on the ground). */
	void SpawnDummiesAt(const FVector& Where);

protected:

	virtual void NativeOnInitialized() override;
	virtual void NativeConstruct() override;
	virtual void NativeDestruct() override;
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;
	virtual FReply NativeOnMouseButtonDown(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent) override;

private:

	enum class ERow : uint8
	{
		Weapon, Shift, Q,
		HelmetType, HelmetSkill, ArmorType, ArmorSkill, BootsType, BootsSkill, TrinketType, TrinketSkill,
		Grade, Level,
		DummyType, DummyCount, DummyMode,
		Count
	};

	enum class EButton : uint8
	{
		Apply, Spawn, Clear, AllFixed, AllAi, Heal, Cooldown, God, Fow,
		Count
	};

	void LoadLists();
	void BuildRow(UVerticalBox* Parent, ERow Row, const TCHAR* LabelKey);
	UBorder* MakeButton(class UHorizontalBox* Row, EButton Button, const TCHAR* Key);
	void AddSection(UVerticalBox* Parent, const TCHAR* Key);

	/** One step of a row's ◀ / ▶. */
	void Step(ERow Row, int32 Delta);
	void Press(EButton Button);
	void Refresh();

	/** The skills the type's pool offers, 0 ("없음") first. */
	TArray<uint32> SkillOptions(uint32 ItemTypeId) const;
	static uint32 StepSkill(const TArray<uint32>& Options, uint32 Current, int32 Delta, uint32 Avoid);

	uint32 CurrentWeaponType() const;
	uint32 CurrentPartType(int32 Part) const;
	FText SkillLabel(uint32 SkillId, bool bCheckWeapon) const;

	void SendCommand(const FString& Command);
	void SetStatus(const TCHAR* Key);
	void HandleKeysChanged();

	// ── Lists (Server/Data) ──
	TArray<uint32> WeaponTypes;
	TArray<uint32> PartTypes[4];				// helmet · armor · boots · trinket - 0 ("그대로") first
	TMap<FName, TArray<uint32>> Pools;			// skill_pools.csv
	TMap<uint32, FName> RequiresWeapon;			// skills.csv requires_weapon - pool id of the weapon it needs
	TArray<uint32> MonsterTypes;

	// ── Selection ──
	int32 WeaponIndex = 0;
	uint32 WeaponSkills[2] = {};
	int32 PartIndex[4] = {};
	uint32 PartSkills[4] = {};
	int32 Grade = 1;
	int32 Level = 0;
	int32 DummyTypeIndex = 0;
	int32 DummyCount = 1;
	bool bDummyAi = false;
	bool bGod = false;
	bool bFowOn = true;		// 내 시야만 (/t fow) - 끄면 거리 · 벽과 상관없이 보이고 안개가 사라진다. 은신은 그대로 숨는다

	// ── Feedback ──
	bool bAwaitingGear = false;
	float AwaitUntil = 0.f;
	FText Status;

	UPROPERTY() TArray<TObjectPtr<UBorder>>		PrevButtons;
	UPROPERTY() TArray<TObjectPtr<UBorder>>		NextButtons;
	UPROPERTY() TArray<TObjectPtr<UTextBlock>>	Values;
	UPROPERTY() TArray<TObjectPtr<UBorder>>		Buttons;
	UPROPERTY() TArray<TObjectPtr<UTextBlock>>	ButtonTexts;
	UPROPERTY() TObjectPtr<UTextBlock>			StatusLine;

	FDelegateHandle KeysChangedHandle;
};
