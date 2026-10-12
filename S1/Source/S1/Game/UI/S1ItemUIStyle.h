// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Fonts/SlateFontInfo.h"
#include "Game/Item/S1ItemTypes.h"

struct FS1ItemTypeDef;

/**
 * Every color, size and string the item UI uses, in one place.
 *
 * Colors are tokens (design/ux/item-ui.md header). When art-director settles the grade
 * palette, only GradeColor() changes. Text always comes from the S1Items String Table —
 * no widget holds a literal (AC-UI-26).
 */
namespace S1ItemUIStyle
{
	// ── art-bible § 4 ⑤ UI palette ───────────────────────────────────────
	FLinearColor PanelBackground();		// #1A1A19
	FLinearColor CellBackground();
	FLinearColor CellHover();
	FLinearColor Divider();				// #383835
	FLinearColor TextPrimary();			// #FFFFFF
	FLinearColor TextSecondary();		// #C3C2B7
	FLinearColor TextTertiary();		// #898781

	/** A Shadowed key — clearly darker than tertiary so it reads as "dead", but never red (EB:539). */
	FLinearColor TextShadowed();

	/** Betrayer red (#E54B3C) — the dungeon-party betrayer label and [배신하고 나가기] (party-betrayal R6 · R10; art-bible 정정 메모 2026-10-12). */
	FLinearColor BetrayerRed();

	/**
	 * GradeColor.* tokens. The outline shape is the primary code (art-bible § 4 ②); color backs it up.
	 * 🔴 Rare and Epic are TEMP. The art-bible values (#199E70 · #3987E5) are the monster and party
	 *    colors, which art-bible line 436 forbids reusing. art-director owns the replacement.
	 */
	FLinearColor GradeColor(ES1ItemGrade Grade);

	FSlateFontInfo Font(int32 Size);

	// ── Layout, in 1080p units. UMG DPI scaling carries it to 720p ─────────
	inline constexpr float GridCellSize		= 64.f;		// 72 overflowed the 70% height at 720p (§ Layout Zones budget)
	inline constexpr float StripCellSize	= 56.f;
	inline constexpr float CellGap			= 4.f;
	inline constexpr float PanelPadding		= 12.f;
	inline constexpr float NoticeLineHeight	= 18.f;		// Reserved even when empty — no layout jump
	inline constexpr float NoticeSeconds	= 2.f;		// How long a notice line stays (§ Component Inventory — 알림 줄)
	inline constexpr int32 GridColumns		= 5;
	inline constexpr int32 LootGridCells	= 30;		// A death bag holds up to 25 carried + 5 worn (GDD AC-IE-31)
	inline constexpr int32 FontSmall		= 11;
	inline constexpr int32 FontBody			= 13;
	inline constexpr int32 FontEmphasis		= 15;

	// ── Text ──────────────────────────────────────────────────────────────
	FText Text(const TCHAR* Key);
	FText ItemName(const FS1ItemTypeDef* Type, uint32 TypeId);
	FText GradeLabel(ES1ItemGrade Grade);
	FText PartLabel(int32 EquipSlot);
	FText ArchetypeLabel(FName Archetype);
	FText StatLabel(FName Stat);
	FText SkillName(uint32 SkillId);
	FText SkillDesc(uint32 SkillId);
	FText SkillFamily(uint32 SkillId);
	FText Notice(ES1ItemNotice Notice, int32 Count);

	/** Ratio stats read as percentages; the rest as plain numbers. */
	FText StatValueText(FName Stat, float Value);

	/** Signed, for "(+1.65)". */
	FText StatDeltaText(FName Stat, float Delta);
}
