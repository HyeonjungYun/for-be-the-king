// Fill out your copyright notice in the Description page of Project Settings.

#include "Game/Item/S1ItemSubsystem.h"
#include "Game/BaseCamp/S1BaseCampSubsystem.h"
#include "Game/S1SkillTable.h"
#include "S1.h"
#include "Protocol.pb.h"
#include "Engine/World.h"
#include "Engine/GameInstance.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/Pawn.h"
#include "HAL/IConsoleManager.h"
#include "Internationalization/StringTableCore.h"
#include "Internationalization/StringTableRegistry.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"

// S1ItemTypes.h mirrors Enum.proto by value so the translation below is a cast.
static_assert(static_cast<int32>(ES1BindState::Shadowed) == Protocol::BIND_SHADOWED, "ES1BindState drifted from Enum.proto");
static_assert(static_cast<int32>(ES1ItemGrade::Legendary) == Protocol::ITEM_GRADE_LEGENDARY, "ES1ItemGrade drifted from Enum.proto");
static_assert(static_cast<int32>(ES1ItemState::Carried) == Protocol::ITEM_STATE_CARRIED, "ES1ItemState drifted from Enum.proto");
static_assert(static_cast<int32>(ES1ItemState::InBag) == Protocol::ITEM_STATE_IN_BAG, "ES1ItemState drifted from Enum.proto");
static_assert(static_cast<int32>(ES1ItemResult::InventoryFull) == Protocol::ITEM_INVENTORY_FULL, "ES1ItemResult drifted from Enum.proto");
static_assert(static_cast<int32>(ES1ItemResult::DbError) == Protocol::ITEM_DB_ERROR, "ES1ItemResult drifted from Enum.proto");

namespace
{
	FS1ItemView ToView(const Protocol::ItemInstance& Item)
	{
		FS1ItemView View;
		View.InstanceId	= Item.instance_id();
		View.ItemTypeId	= Item.item_type_id();
		View.Grade		= static_cast<ES1ItemGrade>(Item.grade());
		View.Level		= Item.level();
		View.State		= static_cast<ES1ItemState>(Item.state());
		View.Slot		= static_cast<int32>(Item.slot());

		for (const Protocol::SkillInfo& Skill : Item.skills())
			View.Skills.Add(FS1ItemSkill{ static_cast<int32>(Skill.slot()), Skill.skill_id() });

		return View;
	}

	FString SkillsToString(const FS1ItemView& View)
	{
		FString Out;
		for (const FS1ItemSkill& Skill : View.Skills)
			Out += FString::Printf(TEXT("%s%d:%u"), Out.IsEmpty() ? TEXT("") : TEXT(","), Skill.Slot, Skill.SkillId);
		return Out.IsEmpty() ? TEXT("-") : Out;
	}

	/** Fake ids live far above anything the server issues, so a dump tells them apart. */
	constexpr uint64 FakeIdBase = 0xF000000000000000ull;
}

const FName US1ItemSubsystem::StringTableId(TEXT("S1Items"));

US1ItemSubsystem* US1ItemSubsystem::Get()
{
	if (GWorld == nullptr)
		return nullptr;

	UGameInstance* GameInstance = GWorld->GetGameInstance();
	return GameInstance != nullptr ? GameInstance->GetSubsystem<US1ItemSubsystem>() : nullptr;
}

void US1ItemSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	// The registry outlives a game instance (PIE restarts), so register once per process.
	// The macro wants a literal — it must say the same thing as StringTableId.
	if (FStringTableRegistry::Get().FindStringTable(StringTableId).IsValid() == false)
		LOCTABLE_FROMFILE_GAME("S1Items", "S1Items", "Data/S1ItemStrings.csv");

	const FString Directory = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), ItemDataDirectory));

	FString Error;
	if (ItemDataDirectory.IsEmpty() || Catalog.LoadFromDirectory(Directory, Error) == false)
	{
		// Not fatal: every item shows as "알 수 없는 아이템 #id" and cannot be equipped by click.
		UE_LOG(LogTemp, Error, TEXT("[ITEMUI] item catalog not loaded - dir=%s error=%s"),
			*Directory, ItemDataDirectory.IsEmpty() ? TEXT("ItemDataDirectory is not set") : *Error);
	}
	else
	{
		UE_LOG(LogTemp, Log, TEXT("[ITEMUI] item catalog loaded - %d types from %s"), Catalog.Num(), *Directory);
	}

	if (Catalog.GetGradeMultipliers().Num() < 4)
		UE_LOG(LogTemp, Error, TEXT("[ITEMUI] grade multipliers not loaded (item_grades.csv) - every grade counts as x1.0"));

	// Skills come from the same directory - skills.csv + skill_effects.csv, what the server fights with (2026-10-10 roster)
	if (ItemDataDirectory.IsEmpty() == false)
	{
		FString SkillError;
		if (FS1SkillTable::LoadFromDirectory(Directory, SkillError) == false)
		{
			UE_LOG(LogTemp, Error, TEXT("[SKILL] skill table not loaded - no key can aim or fire. dir=%s error=%s"), *Directory, *SkillError);
		}
		else
		{
			UE_LOG(LogTemp, Log, TEXT("[SKILL] skill table loaded - %d skills"), FS1SkillTable::Num());
		}
	}

	CheckStrings();

	OnItemsChanged.AddUObject(this, &US1ItemSubsystem::RecomputeMoveSpeedBonus);
}

TArrayView<const float> US1ItemSubsystem::GetGradeMultipliers() const
{
	return Catalog.GetGradeMultipliers();
}

void US1ItemSubsystem::RecomputeMoveSpeedBonus()
{
	constexpr int32 WornParts[] = { 1, 3, 4, 5, 6 };		// weapon · helmet · armor · boots · trinket (slot 2 is a key, not a part)

	float Sum = 0.f;
	for (int32 Part : WornParts)
	{
		const FS1ItemView* Worn = GetEquipped(Part);
		const FS1ItemTypeDef* Type = Worn != nullptr ? Catalog.Find(Worn->ItemTypeId) : nullptr;
		if (Type != nullptr)
			Sum += S1ItemLogic::MoveSpeedBonus(*Type, Worn->Grade, GetGradeMultipliers(), Worn->Level);
	}

	if (FMath::IsNearlyEqual(Sum, MoveSpeedBonus) == false)
		UE_LOG(LogTemp, Log, TEXT("[ITEMUI] move speed bonus %.4f -> %.4f"), MoveSpeedBonus, Sum);
	MoveSpeedBonus = Sum;
}

void US1ItemSubsystem::CheckStrings() const
{
	FStringTableConstPtr Table = FStringTableRegistry::Get().FindStringTable(StringTableId);
	if (Table.IsValid() == false)
	{
		UE_LOG(LogTemp, Error, TEXT("[ITEMUI] String Table %s not found - Content/Data/S1ItemStrings.csv"), *StringTableId.ToString());
		return;
	}

	TArray<FString> Missing;
	for (const TPair<uint32, FS1ItemTypeDef>& Pair : Catalog.GetTypes())
	{
		const FS1ItemTypeDef& Type = Pair.Value;
		for (const FString& Key : { Type.NameKey, FString(TEXT("archetype.")) + Type.Archetype.ToString() })
		{
			if (Table->FindEntry(Key).IsValid() == false)
				Missing.AddUnique(Key);
		}

		for (const FS1ItemStat& Stat : Type.Stats)
		{
			const FString Key = FString(TEXT("stat.")) + Stat.Stat.ToString();
			if (Table->FindEntry(Key).IsValid() == false)
				Missing.AddUnique(Key);
		}
	}

	if (Missing.Num() > 0)
		UE_LOG(LogTemp, Warning, TEXT("[ITEMUI] String Table is missing %d keys: %s"), Missing.Num(), *FString::Join(Missing, TEXT(", ")));
}

void US1ItemSubsystem::Deinitialize()
{
	if (UGameInstance* GameInstance = GetGameInstance())
	{
		for (TPair<int32, FBatch>& Pair : Batches)
			GameInstance->GetTimerManager().ClearTimer(Pair.Value.Timer);
	}

	Batches.Reset();
	Pending.Reset();

	Super::Deinitialize();
}

// ── Incoming ─────────────────────────────────────────────────────────────

void US1ItemSubsystem::OnInventorySync(const Protocol::S_INVENTORY_SYNC& Pkt)
{
	// Dungeon syncs — entry, reattach, death (empty) — carry Carried and Equipped only, so what
	// we know of the vault stays. A base-camp sync (login, back from a run) carries the vault
	// too and replaces everything. The packet does not say which; where we are does — the
	// server sends the base-camp one only after S_LOGIN or S_LEAVE_GAME.
	const US1BaseCampSubsystem* BaseCamp = GetGameInstance() != nullptr ? GetGameInstance()->GetSubsystem<US1BaseCampSubsystem>() : nullptr;
	const bool bBaseCamp = BaseCamp != nullptr && BaseCamp->IsInBaseCamp();

	for (auto It = Items.CreateIterator(); It; ++It)
	{
		if (bBaseCamp || It->Value.State != ES1ItemState::Vaulted)
			It.RemoveCurrent();
	}

	for (const Protocol::ItemInstance& Item : Pkt.items())
		Apply(ToView(Item));

	UE_LOG(LogTemp, Log, TEXT("[ITEMUI] sync - %d items, model now %d, %s"), Pkt.items_size(), Items.Num(), bBaseCamp ? TEXT("base camp (full)") : TEXT("dungeon"));

	OnItemsChanged.Broadcast();
	OnInventorySynced.Broadcast(bBaseCamp);
}

void US1ItemSubsystem::OnItemResult(const Protocol::S_ITEM_RESULT& Pkt)
{
	const FString RequestId = UTF8_TO_TCHAR(Pkt.request_id().c_str());
	const ES1ItemResult Result = static_cast<ES1ItemResult>(Pkt.result());

	// Clear the pending entry before telling anyone: listeners redraw on the broadcast, and a
	// cell must not still read "waiting" once its answer is in.
	FPending Request;
	const bool bOurs = Pending.RemoveAndCopyValue(RequestId, Request);

	bool bChanged = false;
	for (const Protocol::ItemInstance& Item : Pkt.changed())
	{
		if (Apply(ToView(Item)))
			bChanged = true;
	}

	// A rejection changes no item but still ends the wait — the dim has to go.
	if (bChanged || bOurs)
		OnItemsChanged.Broadcast();

	if (bOurs == false)
	{
		// Debug console commands send their own packets, and late replies to a settled batch
		// land here too. The changes above still apply; there is no action to word a notice
		// with, so log only.
		UE_LOG(LogTemp, Log, TEXT("[ITEMUI] result=%d changed=%d - not a pending request"),
			static_cast<int32>(Result), Pkt.changed_size());
		return;
	}

	if (Request.BatchId != 0)
	{
		if (FBatch* Batch = Batches.Find(Request.BatchId))
		{
			Batch->Tally.Record(Result);
			if (Batch->Tally.IsComplete())
				SettleBatch(Request.BatchId);
		}
		return;
	}

	const ES1ItemNotice Notice = S1ItemLogic::NoticeFor(Result, Request.Action);

	UE_LOG(LogTemp, Log, TEXT("[ITEMUI] result=%d action=%d id=%llu notice=%d"),
		static_cast<int32>(Result), static_cast<int32>(Request.Action), Request.InstanceId, static_cast<int32>(Notice));

	if (Notice != ES1ItemNotice::None)
		OnItemNotice.Broadcast(Notice, Request.InstanceId, 1);

	// TODO(stage 2): the spec asks for an inventory resync on NotFound. No request packet for
	// that exists yet — the next server-pushed sync corrects the model.
}

void US1ItemSubsystem::OnBagContents(const Protocol::S_BAG_CONTENTS& Pkt)
{
	const uint64 BagId = Pkt.bag_id();

	FS1BagView& Bag = Bags.FindOrAdd(BagId);
	Bag.BagId = BagId;
	Bag.bHasContents = true;
	Bag.Items.Reset();

	for (const Protocol::ItemInstance& Item : Pkt.items())
		Bag.Items.Add(ToView(Item));

	Bag.RemainingMsAtReceive = Pkt.remaining_ms();
	Bag.ReceivedAtSeconds = FPlatformTime::Seconds();

	UE_LOG(LogTemp, Log, TEXT("[ITEMUI] bag contents id=%llu items=%d remaining_ms=%llu open=%d"),
		BagId, Bag.Items.Num(), Bag.RemainingMsAtReceive, OpenBagId == BagId ? 1 : 0);

	OnBagChanged.Broadcast(BagId);
}

void US1ItemSubsystem::OnBagSpawned(const Protocol::ObjectInfo& Info)
{
	const uint64 BagId = Info.object_id();
	const Protocol::PosInfo& Pos = Info.pos_info();

	FS1BagView& Bag = Bags.FindOrAdd(BagId);
	Bag.BagId = BagId;
	Bag.bHasLocation = true;
	Bag.Location = FVector(Pos.x(), Pos.y(), Pos.z());

	OnBagChanged.Broadcast(BagId);
}

void US1ItemSubsystem::OnChestState(const TArray<uint64>& LootedIds, bool bSnapshot)
{
	if (bSnapshot)
		LootedChests.Reset();

	for (uint64 Id : LootedIds)
	{
		LootedChests.Add(Id);
		OnBagChanged.Broadcast(Id);
	}

	UE_LOG(LogTemp, Log, TEXT("[CHEST] state %s - %d looted (now %d)"), bSnapshot ? TEXT("snapshot") : TEXT("diff"), LootedIds.Num(), LootedChests.Num());
}

void US1ItemSubsystem::OnBagDespawned(uint64 BagId)
{
	// A chest that resets despawns and spawns again — Sealed (server UpdateChests)
	LootedChests.Remove(BagId);

	if (Bags.Remove(BagId) == 0)
		return;

	// A take-all still in flight settles on its own — the remaining replies come back NotFound.
	if (OpenBagId == BagId)
	{
		OpenBagId = 0;
		OnBagClosed.Broadcast(BagId, ES1BagCloseReason::Gone);
	}

	OnBagChanged.Broadcast(BagId);
}

void US1ItemSubsystem::OnEquipSync()
{
	OnKeysChanged.Broadcast();
}

// ── Outgoing ─────────────────────────────────────────────────────────────

void US1ItemSubsystem::RequestEquip(uint64 InstanceId, int32 Slot)
{
	Protocol::C_EQUIP Pkt;
	Pkt.set_request_id(TCHAR_TO_UTF8(*AddPending(ES1ItemAction::Equip, InstanceId, 0)));
	Pkt.set_instance_id(InstanceId);
	Pkt.set_slot(static_cast<Protocol::EquipSlot>(Slot));
	SEND_PACKET(Pkt);
}

void US1ItemSubsystem::RequestUnequip(uint64 InstanceId)
{
	Protocol::C_UNEQUIP Pkt;
	Pkt.set_request_id(TCHAR_TO_UTF8(*AddPending(ES1ItemAction::Unequip, InstanceId, 0)));
	Pkt.set_instance_id(InstanceId);
	SEND_PACKET(Pkt);
}

void US1ItemSubsystem::RequestDrop(uint64 InstanceId)
{
	Protocol::C_DROP Pkt;
	Pkt.set_request_id(TCHAR_TO_UTF8(*AddPending(ES1ItemAction::Drop, InstanceId, 0)));
	Pkt.set_instance_id(InstanceId);
	SEND_PACKET(Pkt);
}

void US1ItemSubsystem::RequestStash(uint64 InstanceId, bool bToVault)
{
	const ES1ItemAction Action = bToVault ? ES1ItemAction::Stash : ES1ItemAction::Unstash;

	Protocol::C_STASH Pkt;
	Pkt.set_request_id(TCHAR_TO_UTF8(*AddPending(Action, InstanceId, 0)));
	Pkt.set_instance_id(InstanceId);
	Pkt.set_to_vault(bToVault);
	SEND_PACKET(Pkt);
}

void US1ItemSubsystem::RequestEnhance(uint64 InstanceId)
{
	Protocol::C_ENHANCE Pkt;
	Pkt.set_request_id(TCHAR_TO_UTF8(*AddPending(ES1ItemAction::Enhance, InstanceId, 0)));
	Pkt.set_instance_id(InstanceId);
	SEND_PACKET(Pkt);
}

void US1ItemSubsystem::RequestOpenBag(uint64 BagId)
{
	// Out of range, the server answers nothing at all — the window simply never fills.
	OpenBagId = BagId;

	Protocol::C_BAG_OPEN Pkt;
	Pkt.set_bag_id(BagId);
	SEND_PACKET(Pkt);
}

void US1ItemSubsystem::RequestLoot(uint64 BagId, uint64 InstanceId)
{
	Protocol::C_LOOT Pkt;
	Pkt.set_request_id(TCHAR_TO_UTF8(*AddPending(ES1ItemAction::Loot, InstanceId, BagId)));
	Pkt.set_bag_id(BagId);
	Pkt.set_instance_id(InstanceId);
	SEND_PACKET(Pkt);
}

void US1ItemSubsystem::RequestLootAll(uint64 BagId)
{
	const FS1BagView* Bag = Bags.Find(BagId);
	if (Bag == nullptr || Bag->bHasContents == false || Bag->Items.Num() == 0)
		return;

	// Snapshot first — a push can replace the bag's list while we are still sending.
	TArray<uint64> InstanceIds;
	for (const FS1ItemView& Item : Bag->Items)
		InstanceIds.Add(Item.InstanceId);

	const int32 BatchId = NextBatchId++;

	FBatch& Batch = Batches.Add(BatchId);
	Batch.BagId = BagId;
	Batch.Tally.Expected = InstanceIds.Num();

	if (UGameInstance* GameInstance = GetGameInstance())
	{
		GameInstance->GetTimerManager().SetTimer(Batch.Timer,
			FTimerDelegate::CreateUObject(this, &US1ItemSubsystem::SettleBatch, BatchId),
			LootBatchTimeoutSeconds, false);
	}

	for (uint64 InstanceId : InstanceIds)
	{
		Protocol::C_LOOT Pkt;
		Pkt.set_request_id(TCHAR_TO_UTF8(*AddPending(ES1ItemAction::Loot, InstanceId, BagId, BatchId)));
		Pkt.set_bag_id(BagId);
		Pkt.set_instance_id(InstanceId);
		SEND_PACKET(Pkt);
	}

	UE_LOG(LogTemp, Log, TEXT("[ITEMUI] take-all bag=%llu sent=%d batch=%d"), BagId, InstanceIds.Num(), BatchId);
}

void US1ItemSubsystem::CloseBag()
{
	OpenBagId = 0;
}

// ── Queries ──────────────────────────────────────────────────────────────

const FS1ItemView* US1ItemSubsystem::FindItem(uint64 InstanceId) const
{
	return Items.Find(InstanceId);
}

TArray<const FS1ItemView*> US1ItemSubsystem::GetByState(ES1ItemState State) const
{
	TArray<const FS1ItemView*> Out;
	for (const TPair<uint64, FS1ItemView>& Pair : Items)
	{
		if (Pair.Value.State == State)
			Out.Add(&Pair.Value);
	}

	Out.Sort([](const FS1ItemView& A, const FS1ItemView& B) { return A.InstanceId < B.InstanceId; });
	return Out;
}

const FS1ItemView* US1ItemSubsystem::GetEquipped(int32 Slot) const
{
	for (const TPair<uint64, FS1ItemView>& Pair : Items)
	{
		if (Pair.Value.State == ES1ItemState::Equipped && Pair.Value.Slot == Slot)
			return &Pair.Value;
	}
	return nullptr;
}

const FS1BagView* US1ItemSubsystem::FindBag(uint64 BagId) const
{
	return Bags.Find(BagId);
}

uint64 US1ItemSubsystem::FindNearestBag(const FVector& From) const
{
	return S1ItemLogic::FindNearestBag(Bags, From, LootRangeCm);
}

bool US1ItemSubsystem::IsPending(uint64 InstanceId) const
{
	for (const TPair<FString, FPending>& Pair : Pending)
	{
		if (Pair.Value.InstanceId == InstanceId)
			return true;
	}
	return false;
}

TArray<FS1KeySlotView> US1ItemSubsystem::GetKeySlots() const
{
	TArray<FS1KeySlotView> Out;

	const US1GameInstance* GameInstance = Cast<US1GameInstance>(GetGameInstance());
	if (GameInstance == nullptr)
		return Out;

	for (const Protocol::SkillInfo& Info : GameInstance->SkillSlots)
	{
		FS1KeySlotView Key;
		Key.KeySlot		= static_cast<int32>(Info.slot());
		Key.SkillId		= Info.skill_id();
		Key.Bind		= static_cast<ES1BindState>(Info.bind_state());
		Key.ShadowedBy	= static_cast<int32>(Info.shadowed_by());
		Out.Add(Key);
	}

	return Out;
}

ES1DiscardRule US1ItemSubsystem::DiscardRuleFor(uint64 InstanceId) const
{
	const FS1ItemView* Item = Items.Find(InstanceId);
	if (Item == nullptr)
		return ES1DiscardRule::Reject;

	return S1ItemLogic::DiscardRuleFor(*Item, EquippedThisSession.Contains(InstanceId));
}

// ── Internals ────────────────────────────────────────────────────────────

FString US1ItemSubsystem::AddPending(ES1ItemAction Action, uint64 InstanceId, uint64 BagId, int32 BatchId)
{
	const FString RequestId = FGuid::NewGuid().ToString();
	Pending.Add(RequestId, FPending{ Action, InstanceId, BagId, BatchId });
	return RequestId;
}

bool US1ItemSubsystem::Apply(const FS1ItemView& View)
{
	switch (View.State)
	{
	case ES1ItemState::Equipped:
		EquippedThisSession.Add(View.InstanceId);
		[[fallthrough]];

	case ES1ItemState::Carried:
	case ES1ItemState::Vaulted:
		Items.Add(View.InstanceId, View);
		return true;

	default:	// None = destroyed, InBag = no longer ours
		return Items.Remove(View.InstanceId) > 0;
	}
}

void US1ItemSubsystem::SettleBatch(int32 BatchId)
{
	FBatch Batch;
	if (Batches.RemoveAndCopyValue(BatchId, Batch) == false)
		return;

	if (UGameInstance* GameInstance = GetGameInstance())
		GameInstance->GetTimerManager().ClearTimer(Batch.Timer);

	// Forget replies that never came, so a late one is not counted against the next batch.
	for (auto It = Pending.CreateIterator(); It; ++It)
	{
		if (It->Value.BatchId == BatchId)
			It.RemoveCurrent();
	}

	const FS1LootBatch& Tally = Batch.Tally;

	UE_LOG(LogTemp, Log, TEXT("[ITEMUI] take-all settled bag=%llu expected=%d received=%d full=%d kept=%d close=%d"),
		Batch.BagId, Tally.Expected, Tally.Received, Tally.Full, Tally.Kept, Tally.ShouldClose() ? 1 : 0);

	if (Tally.Full > 0)
		OnItemNotice.Broadcast(ES1ItemNotice::LootPartial, 0, Tally.Full);

	if (Tally.ShouldClose() && OpenBagId == Batch.BagId)
	{
		OpenBagId = 0;
		OnBagClosed.Broadcast(Batch.BagId, ES1BagCloseReason::AllTaken);
	}
}

// ── Debug ────────────────────────────────────────────────────────────────

void US1ItemSubsystem::DumpToLog() const
{
	UE_LOG(LogTemp, Warning, TEXT("[ITEMUI] dump - carried=%d equipped=%d vaulted=%d bags=%d open=%llu pending=%d"),
		GetByState(ES1ItemState::Carried).Num(),
		GetByState(ES1ItemState::Equipped).Num(),
		GetByState(ES1ItemState::Vaulted).Num(),
		Bags.Num(), OpenBagId, Pending.Num());

	for (ES1ItemState State : { ES1ItemState::Equipped, ES1ItemState::Carried, ES1ItemState::Vaulted })
	{
		for (const FS1ItemView* Item : GetByState(State))
		{
			const FS1ItemTypeDef* Type = Catalog.Find(Item->ItemTypeId);
			const FString Name = Type != nullptr ? FText::FromStringTable(StringTableId, Type->NameKey).ToString() : TEXT("?");

			FString Keys;
			if (Type != nullptr)
			{
				for (const FS1KeySkill& Key : S1ItemLogic::KeysFor(*Type, *Item))
					Keys += FString::Printf(TEXT("%s%d:%u"), Keys.IsEmpty() ? TEXT("") : TEXT(","), Key.KeySlot, Key.SkillId);
			}

			UE_LOG(LogTemp, Warning, TEXT("[ITEMUI]  id=%llu%s type=%u name=%s part=%d grade=%d state=%d slot=%d skills=%s keys=%s session_equipped=%d"),
				Item->InstanceId, Item->InstanceId >= FakeIdBase ? TEXT("(fake)") : TEXT(""),
				Item->ItemTypeId, *Name, Type != nullptr ? Type->EquipSlot : 0,
				static_cast<int32>(Item->Grade), static_cast<int32>(Item->State), Item->Slot,
				*SkillsToString(*Item), Keys.IsEmpty() ? TEXT("-") : *Keys, WasEquippedThisSession(Item->InstanceId) ? 1 : 0);
		}
	}

	for (const FS1KeySlotView& Key : GetKeySlots())
	{
		UE_LOG(LogTemp, Warning, TEXT("[ITEMUI]  key=%d skill=%u bind=%d shadowed_by=%d"),
			Key.KeySlot, Key.SkillId, static_cast<int32>(Key.Bind), Key.ShadowedBy);
	}

	const double Now = FPlatformTime::Seconds();
	for (const TPair<uint64, FS1BagView>& Pair : Bags)
	{
		const FS1BagView& Bag = Pair.Value;
		UE_LOG(LogTemp, Warning, TEXT("[ITEMUI]  bag=%llu pos=%s contents=%s remaining_ms=%llu"),
			Bag.BagId,
			Bag.bHasLocation ? *FString::Printf(TEXT("(%.0f, %.0f)"), Bag.Location.X, Bag.Location.Y) : TEXT("?"),
			Bag.bHasContents ? *FString::FromInt(Bag.Items.Num()) : TEXT("?"),
			S1ItemLogic::RemainingMs(Bag, Now));
	}
}

void US1ItemSubsystem::FillFakeItems()
{
	struct FFake
	{
		uint32			TypeId;
		ES1ItemGrade	Grade;
		ES1ItemState	State;
		int32			Slot;
		uint32			SkillId;
	};

	// Every grade at least once — the server only issues Common and Rare in the MVP.
	const FFake Fakes[] =
	{
		{ 101, ES1ItemGrade::Legendary,	ES1ItemState::Equipped, 1, 1007 },
		{ 301, ES1ItemGrade::Epic,		ES1ItemState::Equipped, 3, 1002 },
		{ 401, ES1ItemGrade::Rare,		ES1ItemState::Equipped, 4, 0 },
		{ 102, ES1ItemGrade::Common,	ES1ItemState::Carried,  0, 1005 },
		{ 103, ES1ItemGrade::Rare,		ES1ItemState::Carried,  0, 1018 },
		{ 302, ES1ItemGrade::Epic,		ES1ItemState::Carried,  0, 1002 },
		{ 402, ES1ItemGrade::Legendary,	ES1ItemState::Carried,  0, 0 },
		{ 501, ES1ItemGrade::Common,	ES1ItemState::Carried,  0, 1003 },
		{ 502, ES1ItemGrade::Rare,		ES1ItemState::Vaulted,  0, 1003 },
		{ 601, ES1ItemGrade::Epic,		ES1ItemState::Vaulted,  0, 1002 },
	};

	uint64 NextId = FakeIdBase + 1;
	for (const FFake& Fake : Fakes)
	{
		FS1ItemView View;
		View.InstanceId = NextId++;
		View.ItemTypeId = Fake.TypeId;
		View.Grade = Fake.Grade;
		View.State = Fake.State;
		View.Slot = Fake.Slot;
		View.Skills.Add(FS1ItemSkill{ Fake.Slot != 0 ? Fake.Slot : static_cast<int32>(Fake.TypeId / 100), Fake.SkillId });

		Apply(View);
	}

	UE_LOG(LogTemp, Warning, TEXT("[ITEMUI] fill - %d fake items added (local only)"), static_cast<int32>(UE_ARRAY_COUNT(Fakes)));

	OnItemsChanged.Broadcast();
}

// ── Console ──────────────────────────────────────────────────────────────

namespace
{
	US1ItemSubsystem* FromWorld(UWorld* World)
	{
		UGameInstance* GameInstance = World != nullptr ? World->GetGameInstance() : nullptr;
		return GameInstance != nullptr ? GameInstance->GetSubsystem<US1ItemSubsystem>() : nullptr;
	}

	FAutoConsoleCommandWithWorldAndArgs GItemDumpCommand(
		TEXT("s1.ItemDump"),
		TEXT("Log every item and bag the client item model holds."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>&, UWorld* World)
		{
			if (US1ItemSubsystem* Items = FromWorld(World))
				Items->DumpToLog();
		}));

	FAutoConsoleCommandWithWorldAndArgs GItemFillCommand(
		TEXT("s1.ItemFill"),
		TEXT("Add local-only fake items in all four grades. The next inventory sync clears them."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>&, UWorld* World)
		{
			if (US1ItemSubsystem* Items = FromWorld(World))
				Items->FillFakeItems();
		}));

	FAutoConsoleCommandWithWorldAndArgs GItemOpenNearestCommand(
		TEXT("s1.ItemOpenNearest"),
		TEXT("Open the bag F would open from the local pawn (nearest within loot range)."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>&, UWorld* World)
		{
			US1ItemSubsystem* Items = FromWorld(World);
			APlayerController* PC = World != nullptr ? World->GetFirstPlayerController() : nullptr;
			APawn* Pawn = PC != nullptr ? PC->GetPawn() : nullptr;
			if (Items == nullptr || Pawn == nullptr)
				return;

			const uint64 BagId = Items->FindNearestBag(Pawn->GetActorLocation());
			UE_LOG(LogTemp, Warning, TEXT("[ITEMUI] open-nearest -> bag=%llu"), BagId);

			if (BagId != 0)
				Items->RequestOpenBag(BagId);
		}));

	FAutoConsoleCommandWithWorldAndArgs GItemLootAllCommand(
		TEXT("s1.ItemLootAll"),
		TEXT("\"Take all\" on the open bag, through the item model."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>&, UWorld* World)
		{
			if (US1ItemSubsystem* Items = FromWorld(World))
				Items->RequestLootAll(Items->GetOpenBagId());
		}));
}
