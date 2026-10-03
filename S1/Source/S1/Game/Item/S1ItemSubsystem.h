// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "TimerManager.h"
#include "Game/Item/S1ItemTypes.h"
#include "Game/Item/S1ItemLogic.h"
#include "S1ItemSubsystem.generated.h"

namespace Protocol
{
	class S_INVENTORY_SYNC;
	class S_ITEM_RESULT;
	class S_BAG_CONTENTS;
	class ObjectInfo;
}

DECLARE_MULTICAST_DELEGATE(FOnS1ItemsChanged);
DECLARE_MULTICAST_DELEGATE_OneParam(FOnS1BagChanged, uint64 /*BagId*/);
DECLARE_MULTICAST_DELEGATE_TwoParams(FOnS1BagClosed, uint64 /*BagId*/, ES1BagCloseReason);
DECLARE_MULTICAST_DELEGATE_ThreeParams(FOnS1ItemNotice, ES1ItemNotice, uint64 /*InstanceId*/, int32 /*Count*/);
DECLARE_MULTICAST_DELEGATE(FOnS1KeysChanged);

/**
 * The one place the client keeps item state. design/ux/item-ui.md § Data Requirements.
 *
 * Packets come in through the On* entry points — called from the netcode hooks (H1~H5),
 * which belong to the user. Widgets read the queries and listen to the delegates; they
 * never hold item state of their own.
 *
 * 🔴 No optimistic updates. A request changes nothing here until the server answers.
 */
UCLASS(Config = Game)
class S1_API US1ItemSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:

	/** Null before the game instance exists. Same lookup as the existing packet handlers. */
	static US1ItemSubsystem* Get();

	/** String Table with every item UI string — Content/Data/S1ItemStrings.csv. */
	static const FName StringTableId;

	/** Loads the item catalog and registers the String Table. */
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	// ── Incoming (netcode hooks) ─────────────────────────────────────────

	/** H1. Replaces Carried and Equipped. Keeps Vaulted — see the .cpp. */
	void OnInventorySync(const Protocol::S_INVENTORY_SYNC& Pkt);

	/** H2. Applies `changed`, then words a notice for our own requests. */
	void OnItemResult(const Protocol::S_ITEM_RESULT& Pkt);

	/** H3. Contents of a bag this client has open. Arrives again on every change. */
	void OnBagContents(const Protocol::S_BAG_CONTENTS& Pkt);

	/** H4. A bag appeared in the world. Only its location is known. */
	void OnBagSpawned(const Protocol::ObjectInfo& Info);

	/** H5. A bag left the world — expiry or removal. Closes the window if it was open. */
	void OnBagDespawned(uint64 BagId);

	/** H6. S_EQUIP_SYNC arrived — the six keys changed. The state itself stays in US1GameInstance::SkillSlots. */
	void OnEquipSync();

	// ── Outgoing (UI) ────────────────────────────────────────────────────

	void RequestEquip(uint64 InstanceId, int32 Slot);
	void RequestUnequip(uint64 InstanceId);
	void RequestDrop(uint64 InstanceId);
	void RequestStash(uint64 InstanceId, bool bToVault);
	void RequestOpenBag(uint64 BagId);
	void RequestLoot(uint64 BagId, uint64 InstanceId);

	/** "전체 획득" — one C_LOOT per item, settled together (AC-UI-05). */
	void RequestLootAll(uint64 BagId);

	/** Client-side close. There is no close packet; the server drops viewers who leave range. */
	void CloseBag();

	// ── Queries ──────────────────────────────────────────────────────────

	const TMap<uint64, FS1ItemView>& GetItems() const { return Items; }
	const FS1ItemView* FindItem(uint64 InstanceId) const;

	/** Sorted by instance id so a grid does not reshuffle between redraws. */
	TArray<const FS1ItemView*> GetByState(ES1ItemState State) const;

	const FS1ItemView* GetEquipped(int32 Slot) const;

	const FS1BagView* FindBag(uint64 BagId) const;
	uint64 GetOpenBagId() const { return OpenBagId; }

	/** The bag F would open from here, or 0. */
	uint64 FindNearestBag(const FVector& From) const;

	bool WasEquippedThisSession(uint64 InstanceId) const { return EquippedThisSession.Contains(InstanceId); }
	ES1DiscardRule DiscardRuleFor(uint64 InstanceId) const;

	/** A request about this item is waiting on the server — dim it and ignore repeat clicks (AC-UI-22). */
	bool IsPending(uint64 InstanceId) const;

	/** Slots in the carried grid (entities.yaml inventory_slots). */
	int32 GetCarriedCapacity() const { return CarriedCapacity; }

	/** How long a discard confirm waits for the second Ctrl+click. A data value — § Accessibility. */
	float GetDiscardConfirmSeconds() const { return DiscardConfirmSeconds; }

	/** Item types read from Server/Data. Empty when the files could not be read — see the log. */
	const FS1ItemCatalog& GetCatalog() const { return Catalog; }

	/** Common, Rare, Epic, Legendary — entities.yaml grade_multiplier. */
	TArrayView<const float> GetGradeMultipliers() const { return GradeMultipliers; }

	/** The six keys as of the last S_EQUIP_SYNC, without protobuf. Empty before the first one. */
	TArray<FS1KeySlotView> GetKeySlots() const;

	// ── Change notification ──────────────────────────────────────────────

	FOnS1ItemsChanged	OnItemsChanged;
	FOnS1BagChanged		OnBagChanged;
	FOnS1BagClosed		OnBagClosed;
	FOnS1ItemNotice		OnItemNotice;
	FOnS1KeysChanged	OnKeysChanged;

	// ── Debug (console: s1.ItemDump · s1.ItemFill) ───────────────────────

	void DumpToLog() const;

	/** Local-only fake items in all four grades, for UI screenshots. The next sync clears them. */
	void FillFakeItems();

private:

	struct FPending
	{
		ES1ItemAction	Action = ES1ItemAction::Loot;
		uint64			InstanceId = 0;
		uint64			BagId = 0;
		int32			BatchId = 0;
	};

	struct FBatch
	{
		uint64			BagId = 0;
		FS1LootBatch	Tally;
		FTimerHandle	Timer;
	};

	/** Registers the request and returns its request_id. */
	FString AddPending(ES1ItemAction Action, uint64 InstanceId, uint64 BagId, int32 BatchId = 0);

	/** Returns true when our item list changed. */
	bool Apply(const FS1ItemView& View);

	/** On the last reply, or on the timeout when replies went missing. */
	void SettleBatch(int32 BatchId);

	/** Mirror of the server's LOOT_RANGE_CM. Change both until the server sends it. */
	UPROPERTY(Config)
	float LootRangeCm = 200.f;

	/** How long "전체 획득" waits for missing replies before reporting what it has. */
	UPROPERTY(Config)
	float LootBatchTimeoutSeconds = 3.f;

	/**
	 * Where item_types.csv lives, relative to the project directory.
	 * 🔴 Dev only — reads the server's copy so there is one source. Packaging needs its own answer.
	 */
	UPROPERTY(Config)
	FString ItemDataDirectory;

	/** Indexed by grade − 1. Comes from config; a missing entry counts as ×1.0 and is logged at load. */
	UPROPERTY(Config)
	TArray<float> GradeMultipliers;

	UPROPERTY(Config)
	int32 CarriedCapacity = 25;

	UPROPERTY(Config)
	float DiscardConfirmSeconds = 3.f;

	/** Logs every catalog key the String Table lacks. Missing text shows as the key itself. */
	void CheckStrings() const;

	FS1ItemCatalog				Catalog;

	TMap<uint64, FS1ItemView>	Items;
	TMap<uint64, FS1BagView>	Bags;
	uint64						OpenBagId = 0;

	/** Items seen equipped while this process lived. Survives reattach. AC-UI-03. */
	TSet<uint64>				EquippedThisSession;

	TMap<FString, FPending>		Pending;
	TMap<int32, FBatch>			Batches;
	int32						NextBatchId = 1;
};
