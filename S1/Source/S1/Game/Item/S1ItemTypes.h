// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"

/**
 * Client-side item vocabulary. Plain structs and enums only.
 *
 * Widgets must never include protobuf headers. US1ItemSubsystem translates
 * Protocol::ItemInstance into FS1ItemView once, and everything above it reads these
 * types. design/ux/item-ui.md § Data Requirements.
 *
 * The enums that mirror Enum.proto keep the same numeric values so the translation is a
 * cast. S1ItemSubsystem.cpp static_asserts that they still agree.
 */

/** Mirrors Protocol::ItemGrade. */
enum class ES1ItemGrade : uint8
{
	None = 0,
	Common,
	Rare,
	Epic,
	Legendary,
};

/** Mirrors Protocol::ItemState. */
enum class ES1ItemState : uint8
{
	None = 0,		// Destroyed. A dropped item comes back in this state
	Carried,
	Equipped,
	Vaulted,
	InBag,			// No longer ours — it went into a bag
};

/** Mirrors Protocol::ItemResult. */
enum class ES1ItemResult : uint8
{
	Ok = 0,
	Already,		// Idempotent duplicate. Already applied
	NotFound,
	InvalidState,
	InventoryFull,
	BadRequest,
	DbError,
	LonePathInParty,	// 고독한 길 - cannot be worn while in a party (2026-10-17)
	NotEnoughGold,		// enhance costs more vault gold than there is (2026-10-17 economy)
};

/** Which request produced a result. The same result code reads differently per action. */
enum class ES1ItemAction : uint8
{
	Loot = 0,
	Equip,
	Unequip,
	Drop,
	Stash,			// Carried -> Vaulted
	Unstash,		// Vaulted -> Carried
	Enhance,		// base camp level + 1 (item-enhance 2026-10-12) - answered with S_ITEM_RESULT like the others
	Sell,			// base camp guild buyer (2026-10-17 economy) - C_SELL, one cell
};

/**
 * What the player is told after a request. The wording lives in a String Table (stage 2);
 * this enum is the key, so the mapping is testable without any UI.
 * design/ux/item-ui.md § Events Fired — the ItemResult table.
 */
enum class ES1ItemNotice : uint8
{
	None = 0,
	InventoryFull,		// 인벤토리가 가득 찼습니다
	VaultFull,			// 보관이 가득 찼습니다
	CarriedFull,		// 반입이 가득 찼습니다
	NotFound,			// 아이템을 찾을 수 없습니다
	MustUnequipFirst,	// 먼저 벗어야 합니다
	TooFar,				// 너무 멉니다
	CannotNow,			// 지금은 할 수 없습니다
	BadRequest,			// 할 수 없는 동작입니다
	RetryLater,			// 잠시 후 다시 시도해 주세요
	LootPartial,		// n개를 가져오지 못했습니다(인벤토리 가득 참) — carries a count
	LonePathInParty,	// 고독한 길은 파티 중에 착용할 수 없습니다 (2026-10-17)
	NotEnoughGold,		// 골드가 부족합니다 (2026-10-17 경제)
	Sold,				// n 골드에 팔았습니다 — Count = gold gained
	LootNotEquippable,	// 전리품은 착용 · 강화할 수 없습니다
	SellEquipped,		// 착용 중인 장비는 팔 수 없습니다
};

/** Why an open bag window closed. The window closes the same way for all of them — no reason line. */
enum class ES1BagCloseReason : uint8
{
	Gone = 0,		// Expired or removed
	Refilled,		// Chest reset. No packet tells us yet
	AllTaken,		// "전체 획득" emptied it
	OutOfRange,		// The player walked away
};

/** What a discard click does. design/ux/item-ui.md § Interaction Map, AC-UI-03. */
enum class ES1DiscardRule : uint8
{
	Immediate = 0,
	Confirm,		// Second Ctrl+click on the same slot within the confirm window
	Reject,			// Equipped items cannot be dropped. Unequip first
};

struct FS1ItemSkill
{
	int32	Slot = 0;		// Protocol::EquipSlot value — the key this skill lands on
	uint32	SkillId = 0;	// 0 = the key stays empty
};

/** One item instance as the client knows it. Copied out of the packet, never a view into it. */
struct FS1ItemView
{
	uint64					InstanceId = 0;
	uint32					ItemTypeId = 0;
	ES1ItemGrade			Grade = ES1ItemGrade::None;
	uint32					Level = 0;
	ES1ItemState			State = ES1ItemState::None;
	int32					Slot = 0;		// Protocol::EquipSlot while equipped, 0 otherwise
	TArray<FS1ItemSkill>	Skills;
	bool					bLonePath = false;	// 고독한 길 (2026-10-17) - worn: no party; more worn: stronger when outnumbered
	uint32					StackCount = 1;		// loot (sell-only) stacks - how many in this cell (2026-10-17 economy)
};

/**
 * A bag in the world. Chests reuse this once the server has them (quick-spec S-3).
 *
 * The location comes from S_SPAWN. Contents exist only after this client opened it.
 */
/** Mirrors Protocol::BagKind - what dropped the bag (monster-ai.md OQ-3, 2026-10-18). */
enum class ES1BagKind : uint8
{
	Unknown = 0,	// a chest, or an old server
	Death,			// a player died here - P4 information
	Monster,		// a monster's drop - not "someone died here"
};

struct FS1BagView
{
	uint64					BagId = 0;
	ES1BagKind				Kind = ES1BagKind::Unknown;

	bool					bHasLocation = false;
	FVector					Location = FVector::ZeroVector;

	bool					bHasContents = false;
	TArray<FS1ItemView>		Items;

	/** The server's remaining time when the contents arrived. 0 = no timer (chests). */
	uint64					RemainingMsAtReceive = 0;
	double					ReceivedAtSeconds = 0.0;
};

/** Mirrors Protocol::SlotBindState. */
enum class ES1BindState : uint8
{
	Unbound = 0,	// Nothing on the key
	Bound,
	Shadowed,		// Same skill already on another key — the key is dead, not broken (EB B5)
};

/** One of the six keys as the server last described it (S_EQUIP_SYNC). */
struct FS1KeySlotView
{
	int32			KeySlot = 0;		// 1 Shift · 2 Q · 3 E · 4 R · 5 Z · 6 X
	uint32			SkillId = 0;
	ES1BindState	Bind = ES1BindState::Unbound;
	int32			ShadowedBy = 0;		// The key that holds the same skill, when Shadowed
};
