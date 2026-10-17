// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "InputCoreTypes.h"
#include "Game/Item/S1ItemTypes.h"
#include "Game/BaseCamp/S1CampLayout.h"
#include "S1BaseCampWidget.generated.h"

class UBorder;
class UEditableTextBox;
class UTextBlock;
class UVerticalBox;
class US1BaseCampSubsystem;
class US1EntryMapWidget;
class US1ItemDetailWidget;
class US1ItemSlotWidget;
class US1ItemSubsystem;
enum class ES1Place : uint8;

/**
 * S3 — 출정 선택, the base-camp screen. design/ux/item-ui.md (B-L | B-C | B-R).
 *
 *   B-L 28%  보관 n/25 · sort      B-C 44%  six keys over the five worn parts    B-R 28%  반입 n/25 · sort
 *            skill-name filter              장비 합산 — the worn stats, summed           skill-name filter
 *            notice                         스킬 제원 — each key's atomic attributes     notice
 *            vault grid 5×5                 detail pane                                 carried grid 5×5
 *                                                                                       [입장]
 *
 * Click grammar (§ Interaction Map): left = move between vault and carried, or take off a worn
 * part; right = wear from carried — from the vault it says why not (AC-UI-30); Ctrl+left = discard.
 * Every action goes straight to the DB on the server, and nothing changes here until it answers.
 *
 * Camp map (2026-10-17, basecamp-map): each NPC opens one part of this screen (SetSection) over the world —
 *   Stash   창고지기   vault | me | carried, the click grammar above. No mode row, no [강화], no [입장]
 *   Enhance 대장장이   vault | me | carried, a click only picks the item; [강화 +1]
 *   Gate    길드장     me | carried (what I take), a click only picks; mode row · party · [입장]
 *   All     the old whole screen — a server without the camp map
 * Esc closes an NPC window (back to walking).
 */
UCLASS()
class S1_API US1BaseCampWidget : public UUserWidget
{
	GENERATED_BODY()

public:

	void SetSection(ES1CampSection InSection);
	ES1CampSection GetSection() const { return Section; }

	/** 길드장 "파티를 꾸린다" — the gate window with the party window over it. */
	void OpenPartyFromCamp() { OpenPartyPanel(); }

protected:

	virtual void NativeOnInitialized() override;
	virtual void NativeConstruct() override;
	virtual void NativeDestruct() override;
	virtual FReply NativeOnMouseButtonDown(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent) override;
	virtual FReply NativeOnKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent) override;

private:

	enum class EColumn : uint8 { Vault = 0, Center, Carried, Count };

	US1ItemSubsystem* GetItems() const;
	US1BaseCampSubsystem* GetBaseCamp() const;

	/** Redraws everything from the item model. Cheap enough to run on every change. */
	void Refresh();
	void RefreshGrid(bool bVault);
	void RefreshStrip();
	void RefreshTotals();
	void RefreshAtomic();
	void RefreshEnter();
	void RefreshDetail();
	void RefreshEnhance();		// [강화 +1] 버튼 글자 · 색 (item-enhance 2026-10-12)

	/**
	 * Which skill each key would get from what is worn — key slot → skill id. In base camp there is
	 * no S_EQUIP_SYNC, so this follows the server's part + order rule (S1ItemLogic::KeysFor).
	 */
	TMap<int32, uint32> ComputeKeySkills() const;

	/** The earlier key holding the same skill, or 0. Shadowed keys do nothing (EB B5). */
	static int32 ShadowedBy(const TMap<int32, uint32>& KeySkills, int32 Key);

	/** Empty filter matches everything. Otherwise one of the item's skill names contains it. */
	static bool MatchesFilter(const FS1ItemView& Item, const FString& Filter);

	UFUNCTION() void HandleVaultFilterChanged(const FText& InText);
	UFUNCTION() void HandleCarriedFilterChanged(const FText& InText);

	void HandleSlotClicked(uint64 InstanceId, FKey Button, bool bCtrl);
	void HandleSlotHovered(uint64 InstanceId);
	void HandleNotice(ES1ItemNotice Notice, uint64 InstanceId, int32 Count);
	void HandlePlaceChanged(ES1Place Place);
	void HandleEnterRejected();

	/** [입장] → S4, the entry-point map (design/ux/entry-point.md). S4 sends C_ENTER_GAME, not S3. */
	void OpenEntryMap();
	void CloseEntryMap();

	void Discard(const FS1ItemView& Item);
	void CancelConfirm();
	void ShowNotice(EColumn Column, const FText& Text, uint64 InstanceId);
	void ClearNotices();
	void ShakeSlot(uint64 InstanceId);

	/** Before the base-camp inventory arrives, and while [입장] is in flight — no clicks count. */
	bool IsLocked() const;

	FText KeyName(int32 KeySlot) const;

	UPROPERTY() TObjectPtr<UTextBlock>						VaultHeader;
	UPROPERTY() TObjectPtr<UTextBlock>						VaultSort;
	UPROPERTY() TObjectPtr<UTextBlock>						CarriedHeader;
	UPROPERTY() TObjectPtr<UTextBlock>						CarriedSort;
	UPROPERTY() TObjectPtr<UEditableTextBox>				VaultFilter;
	UPROPERTY() TObjectPtr<UEditableTextBox>				CarriedFilter;
	UPROPERTY() TArray<TObjectPtr<UTextBlock>>				Notices;		// indexed by EColumn
	UPROPERTY() TArray<TObjectPtr<US1ItemSlotWidget>>		VaultSlots;
	UPROPERTY() TArray<TObjectPtr<US1ItemSlotWidget>>		CarriedSlots;
	UPROPERTY() TArray<TObjectPtr<US1ItemSlotWidget>>		StripSlots;		// parts 1, 3, 4, 5, 6
	UPROPERTY() TArray<TObjectPtr<UTextBlock>>				StripPartLabels;	// archetype, or part name when empty
	UPROPERTY() TArray<TObjectPtr<UTextBlock>>				StripKeys;		// one line per key
	TArray<int32>											StripKeySlots;
	UPROPERTY() TObjectPtr<UTextBlock>						TotalsTitle;
	UPROPERTY() TObjectPtr<UVerticalBox>					TotalLines;
	UPROPERTY() TObjectPtr<UVerticalBox>					AtomicLines;
	UPROPERTY() TObjectPtr<US1ItemDetailWidget>				Detail;
	UPROPERTY() TObjectPtr<UTextBlock>						StarterNotice;	// above [입장] - "기본 장비 3종을 받고 입장한다"
	UPROPERTY() TObjectPtr<UBorder>							EnterButton;
	UPROPERTY() TObjectPtr<UTextBlock>						EnterText;
	UPROPERTY() TObjectPtr<UBorder>							EnhanceButton;		// 베이스캠프 강화 (item-enhance 2026-10-12)
	UPROPERTY() TObjectPtr<UTextBlock>						EnhanceText;
	UPROPERTY() TObjectPtr<US1EntryMapWidget>				EntryMap;

	// PVE (ADR-0014) - mode row above [입장]: [PVP] [PVE 솔로] [PVE 협동] … [파티 n/3]
	UPROPERTY() TArray<TObjectPtr<UBorder>>					ModeButtons;	// ES1RunMode order
	UPROPERTY() TArray<TObjectPtr<UTextBlock>>				ModeTexts;
	UPROPERTY() TObjectPtr<UBorder>							PartyButton;
	UPROPERTY() TObjectPtr<UTextBlock>						PartyText;
	UPROPERTY() TObjectPtr<class US1PartyPanelWidget>		PartyPanel;
	FDelegateHandle											PartyChangedHandle;

	void	RefreshMode();
	void	OpenPartyPanel();
	void	ClosePartyPanel();

	// Camp map sections (2026-10-17)
	bool	ShowsVault() const { return Section != ES1CampSection::Gate; }
	bool	ShowsEnhance() const { return Section == ES1CampSection::All || Section == ES1CampSection::Enhance; }
	bool	ShowsEnter() const { return Section == ES1CampSection::All || Section == ES1CampSection::Gate; }
	bool	PickOnly() const { return Section == ES1CampSection::Enhance || Section == ES1CampSection::Gate; }

	ES1CampSection											Section = ES1CampSection::All;
	UPROPERTY() TObjectPtr<UBorder>							Background;
	UPROPERTY() TObjectPtr<UWidget>							TitleRow;
	UPROPERTY() TObjectPtr<UTextBlock>						TitleText;
	UPROPERTY() TArray<TObjectPtr<UVerticalBox>>			ColumnBoxes;		// EColumn order
	UPROPERTY() TObjectPtr<UWidget>							ModeRowWidget;

	bool			bVaultBySkill = false;
	bool			bCarriedBySkill = false;
	FString			VaultFilterText;
	FString			CarriedFilterText;
	uint64			HoveredId = 0;
	uint64			ShownId = 0;		// 마지막으로 올렸던 아이템 - 상세 · [강화] 버튼은 마우스가 칸을 떠나도 이것을 본다 (sticky)
	uint64			ConfirmId = 0;
	FTimerHandle	ConfirmTimer;
	FTimerHandle	NoticeTimer;

	FDelegateHandle	ItemsChangedHandle;
	FDelegateHandle	KeysChangedHandle;
	FDelegateHandle	NoticeHandle;
	FDelegateHandle	PlaceChangedHandle;
	FDelegateHandle	ReadyHandle;
	FDelegateHandle	RejectedHandle;
};
