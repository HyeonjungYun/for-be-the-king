// Fill out your copyright notice in the Description page of Project Settings.

#include "Game/UI/S1BaseCampWidget.h"
#include "Game/UI/S1EntryMapWidget.h"
#include "Game/UI/S1ItemDetailWidget.h"
#include "Game/UI/S1ItemSlotWidget.h"
#include "Game/UI/S1ItemUIComponent.h"
#include "Game/UI/S1ItemUIStyle.h"
#include "Game/UI/S1PartyWidgets.h"
#include "Game/BaseCamp/S1BaseCampSubsystem.h"
#include "Game/Party/S1PartySubsystem.h"
#include "Game/Item/S1ItemLogic.h"
#include "Game/Item/S1ItemSubsystem.h"
#include "Game/S1SkillTable.h"
#include "Blueprint/WidgetTree.h"
#include "Brushes/SlateColorBrush.h"
#include "Components/Border.h"
#include "Components/EditableTextBox.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/SizeBox.h"
#include "Components/TextBlock.h"
#include "Components/UniformGridPanel.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Engine/GameInstance.h"
#include "TimerManager.h"

using namespace S1ItemUIStyle;

namespace S1BaseCampLayout
{
	/** Worn parts, left to right. The weapon cell covers two keys (Shift, Q). */
	constexpr int32 StripParts[] = { 1, 3, 4, 5, 6 };

	/** § Layout Zones — 보관 | 나 | 반입 */
	constexpr float ColumnWeights[] = { 0.28f, 0.44f, 0.28f };

	constexpr float ScreenPadding = 24.f;

	/** Vault capacity — Server ServerPacketHandler.cpp VAULT_SLOT_COUNT (entities.yaml vault_slots). */
	constexpr int32 VaultCapacity = 25;

	TArray<int32> KeysOfPart(int32 Part)
	{
		return Part == 1 ? TArray<int32>{ 1, 2 } : TArray<int32>{ Part };
	}

	/** Name order, or skill-name order with skill-less items last. Ties go to the instance id so nothing jumps. */
	void SortForGrid(TArray<const FS1ItemView*>& Items, const FS1ItemCatalog& Catalog, bool bBySkill)
	{
		auto SortKey = [&Catalog, bBySkill](const FS1ItemView& Item) -> FString
		{
			const FS1ItemTypeDef* Type = Catalog.Find(Item.ItemTypeId);
			if (bBySkill)
			{
				const TArray<FS1KeySkill> Keys = Type != nullptr ? S1ItemLogic::KeysFor(*Type, Item) : TArray<FS1KeySkill>();
				return Keys.Num() > 0 ? SkillName(Keys[0].SkillId).ToString() : FString::ChrN(1, static_cast<TCHAR>(0xFFFF));
			}
			return ItemName(Type, Item.ItemTypeId).ToString();
		};

		Items.Sort([&SortKey](const FS1ItemView& A, const FS1ItemView& B)
		{
			const FString KeyA = SortKey(A);
			const FString KeyB = SortKey(B);
			return KeyA != KeyB ? KeyA < KeyB : A.InstanceId < B.InstanceId;
		});
	}

	// ── 스킬 제원 — atomic attributes as the player reads them (skill-system.md B1a) ──

	/** "4m", "2.5m" — centimetres from the skill table, one decimal at most. */
	FText Meters(float Cm)
	{
		FNumberFormattingOptions Options;
		Options.MaximumFractionalDigits = 1;
		return FText::AsNumber(Cm / 100.f, &Options);
	}

	/** "8", "0.3", "0.06" — milliseconds as seconds. */
	FText Seconds(uint32 Ms)
	{
		FNumberFormattingOptions Options;
		Options.MaximumFractionalDigits = 2;
		return FText::AsNumber(Ms / 1000.f, &Options);
	}

	/** How the key is used — the one thing a player must know before the fight (skill-system.md aim_type). */
	FText AimLabel(ES1SkillAimType Aim)
	{
		switch (Aim)
		{
		case ES1SkillAimType::SelfArea:		return Text(TEXT("aim.self_area"));
		case ES1SkillAimType::Target:		return Text(TEXT("aim.target"));
		case ES1SkillAimType::Direction:	return Text(TEXT("aim.direction"));
		case ES1SkillAimType::Point:		return Text(TEXT("aim.point"));
		default:							return Text(TEXT("aim.none"));
		}
	}

	/**
	 * The hit shape — left out when it would mislead: a dash is "CircleSelf, radius 0" only so it
	 * has a shape at all; it hits nothing (skill-system.md R12b).
	 */
	FText ShapeLabel(const FS1SkillDef& Def)
	{
		switch (Def.Shape)
		{
		case ES1SkillShape::Single:			return Text(TEXT("shape.single"));
		case ES1SkillShape::Line:			return Text(TEXT("shape.line"));
		case ES1SkillShape::Cone:			return Text(TEXT("shape.cone"));
		case ES1SkillShape::CirclePoint:	return Text(TEXT("shape.circle_point"));
		case ES1SkillShape::Projectile:		return Text(TEXT("shape.projectile"));
		case ES1SkillShape::None:			return FText::GetEmpty();
		default:							return Def.RadiusCm > 0.f ? Text(TEXT("shape.circle_self")) : FText::GetEmpty();
		}
	}

	/** One line: aim · shape · reach · cast · cooldown. No derived numbers — no damage, no DPS (B1a). */
	FText AtomicLine(const FS1SkillDef& Def)
	{
		TArray<FText> Parts;
		Parts.Add(AimLabel(Def.AimType));

		const FText Shape = ShapeLabel(Def);
		if (Shape.IsEmpty() == false)
			Parts.Add(Shape);

		// A dash's range is its travel — say "이동", not "사거리". A cone's range is its radius — said once, below
		if (Def.HasMovement())
			Parts.Add(FText::Format(Text(TEXT("atomic.move")), Meters(Def.MoveDistCm)));
		else if (Def.RangeCm > 0.f && Def.Shape != ES1SkillShape::Cone)
			Parts.Add(FText::Format(Text(TEXT("atomic.range")), Meters(Def.RangeCm)));

		if (Def.WidthCm > 0.f)
			Parts.Add(FText::Format(Text(TEXT("atomic.width")), Meters(Def.WidthCm)));
		if (Def.RadiusCm > 0.f && Def.Shape != ES1SkillShape::Cone)
			Parts.Add(FText::Format(Text(TEXT("atomic.radius")), Meters(Def.RadiusCm)));
		if (Def.Shape == ES1SkillShape::Cone)
			Parts.Add(FText::Format(Text(TEXT("atomic.cone")), Meters(Def.RadiusCm), FText::AsNumber(static_cast<int32>(Def.AngleDeg))));

		// Passive / hold / recast keys are used differently - say so before the numbers
		switch (Def.Kind)
		{
		case ES1SkillKind::Passive:	Parts.Add(Text(TEXT("kind.passive"))); break;
		case ES1SkillKind::Hold:	Parts.Add(Text(TEXT("kind.hold"))); break;
		case ES1SkillKind::Recast:	Parts.Add(Text(TEXT("kind.recast"))); break;
		default: break;
		}

		if (Def.bHasHardCc)
			Parts.Add(Text(TEXT("atomic.hard_cc")));

		if (Def.Kind != ES1SkillKind::Passive)
		{
			Parts.Add(Def.CastMs == 0 ? Text(TEXT("atomic.cast_instant")) : FText::Format(Text(TEXT("atomic.cast")), Seconds(Def.CastMs)));
			Parts.Add(FText::Format(Text(TEXT("atomic.cooldown")), Seconds(Def.CooldownMs)));
		}

		return FText::Join(FText::AsCultureInvariant(TEXT(" · ")), Parts);
	}
}

US1ItemSubsystem* US1BaseCampWidget::GetItems() const
{
	UGameInstance* GameInstance = GetGameInstance();
	return GameInstance != nullptr ? GameInstance->GetSubsystem<US1ItemSubsystem>() : nullptr;
}

US1BaseCampSubsystem* US1BaseCampWidget::GetBaseCamp() const
{
	UGameInstance* GameInstance = GetGameInstance();
	return GameInstance != nullptr ? GameInstance->GetSubsystem<US1BaseCampSubsystem>() : nullptr;
}

FText US1BaseCampWidget::KeyName(int32 KeySlot) const
{
	// Our pawn may be gone in base camp — the resolver falls back to the fixed names then.
	return US1ItemUIComponent::KeyNameForController(GetOwningPlayer(), KeySlot);
}

// ── Build ────────────────────────────────────────────────────────────────

void US1BaseCampWidget::NativeOnInitialized()
{
	Super::NativeOnInitialized();
	SetIsFocusable(true);		// Esc closes a camp map NPC window (NativeOnKeyDown)

	auto MakeText = [this](int32 Size, const FLinearColor& Color) -> UTextBlock*
	{
		UTextBlock* Text = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
		Text->SetFont(Font(Size));
		Text->SetColorAndOpacity(FSlateColor(Color));
		return Text;
	};

	// A notice line whose height is always reserved, so nothing below it jumps when it fills.
	auto MakeNotice = [this, &MakeText](UVerticalBox* Column)
	{
		USizeBox* Box = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
		Box->SetHeightOverride(NoticeLineHeight);
		UTextBlock* Text = MakeText(FontBody, TextPrimary());
		Text->SetVisibility(ESlateVisibility::HitTestInvisible);
		Box->SetContent(Text);
		if (UVerticalBoxSlot* BoxSlot = Column->AddChildToVerticalBox(Box))
			BoxSlot->SetPadding(FMargin(0.f, 0.f, 0.f, 4.f));
		Notices.Add(Text);
	};

	auto MakeGrid = [this](UVerticalBox* Column, int32 Cells, TArray<TObjectPtr<US1ItemSlotWidget>>& OutSlots)
	{
		UUniformGridPanel* Grid = WidgetTree->ConstructWidget<UUniformGridPanel>(UUniformGridPanel::StaticClass());
		Grid->SetSlotPadding(FMargin(CellGap * 0.5f));
		for (int32 i = 0; i < Cells; i++)
		{
			US1ItemSlotWidget* Cell = CreateWidget<US1ItemSlotWidget>(this, US1ItemSlotWidget::StaticClass());
			Cell->SetCellSize(GridCellSize);
			Cell->OnClicked.BindUObject(this, &US1BaseCampWidget::HandleSlotClicked);
			Cell->OnHovered.BindUObject(this, &US1BaseCampWidget::HandleSlotHovered);
			Grid->AddChildToUniformGrid(Cell, i / GridColumns, i % GridColumns);
			OutSlots.Add(Cell);
		}
		if (UVerticalBoxSlot* GridSlot = Column->AddChildToVerticalBox(Grid))
			GridSlot->SetPadding(FMargin(0.f, 0.f, 0.f, 12.f));
	};

	auto MakeHeader = [this, &MakeText](UVerticalBox* Column, TObjectPtr<UTextBlock>& OutHeader, TObjectPtr<UTextBlock>& OutSort,
		TObjectPtr<UEditableTextBox>& OutFilter)
	{
		UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
		OutHeader = MakeText(FontEmphasis, TextPrimary());
		OutSort = MakeText(FontSmall, TextTertiary());
		if (UHorizontalBoxSlot* HeaderSlot = Row->AddChildToHorizontalBox(OutHeader))
			HeaderSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		if (UHorizontalBoxSlot* SortSlot = Row->AddChildToHorizontalBox(OutSort))
			SortSlot->SetVerticalAlignment(VAlign_Center);
		if (UVerticalBoxSlot* RowSlot = Column->AddChildToVerticalBox(Row))
			RowSlot->SetPadding(FMargin(0.f, 0.f, 0.f, 4.f));

		// Skill-name filter — which of the near-identical 환도 carries 짓쳐들기 is otherwise a hover-by-hover
		// search (equipment-skill-binding.md § 인벤토리 — 알려진 부담). Display only: nothing is sent.
		OutFilter = WidgetTree->ConstructWidget<UEditableTextBox>(UEditableTextBox::StaticClass());
		const FSlateColorBrush Plain(FLinearColor::White);
		FEditableTextBoxStyle Style = OutFilter->GetWidgetStyle();
		Style
			.SetFont(Font(FontBody))
			.SetForegroundColor(FSlateColor(TextPrimary()))
			.SetBackgroundColor(FSlateColor(CellBackground()))
			.SetBackgroundImageNormal(Plain)
			.SetBackgroundImageHovered(Plain)
			.SetBackgroundImageFocused(Plain)
			.SetPadding(FMargin(6.f, 3.f));
		OutFilter->SetWidgetStyle(Style);
		OutFilter->SetHintText(Text(TEXT("label.filter_hint")));
		if (UVerticalBoxSlot* FilterSlot = Column->AddChildToVerticalBox(OutFilter))
			FilterSlot->SetPadding(FMargin(0.f, 0.f, 0.f, 4.f));
	};

	// ── The whole screen — base camp has nothing behind it worth seeing ──
	Background = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass());
	// Opaque: the dungeon HUD panels may let the world show through, but in base camp there is no
	// world to show — after a death the camera points at the sky. (Camp map NPC windows let the camp show - SetSection)
	FLinearColor Opaque = PanelBackground();
	Opaque.A = 1.f;
	Background->SetBrushColor(Opaque);
	Background->SetPadding(FMargin(S1BaseCampLayout::ScreenPadding));
	WidgetTree->RootWidget = Background;

	UVerticalBox* Page = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
	Background->SetContent(Page);

	// Camp map NPC window title - "창고 · 창고지기 오르트" … "Esc 닫기" (hidden on the whole screen)
	{
		UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
		TitleText = MakeText(FontEmphasis, TextPrimary());
		if (UHorizontalBoxSlot* TitleSlot = Row->AddChildToHorizontalBox(TitleText))
			TitleSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));

		// Vault gold (2026-10-17 economy) - on every window, the whole screen too
		GoldText = MakeText(FontEmphasis, FLinearColor(FColor(0xE8, 0xC1, 0x6A)));
		if (UHorizontalBoxSlot* GoldSlot = Row->AddChildToHorizontalBox(GoldText))
		{
			GoldSlot->SetVerticalAlignment(VAlign_Center);
			GoldSlot->SetPadding(FMargin(0.f, 0.f, 24.f, 0.f));
		}

		CloseHint = MakeText(FontSmall, TextTertiary());
		CloseHint->SetText(Text(TEXT("camp.close_hint")));
		if (UHorizontalBoxSlot* HintSlot = Row->AddChildToHorizontalBox(CloseHint))
			HintSlot->SetVerticalAlignment(VAlign_Center);
		if (UVerticalBoxSlot* RowSlot = Page->AddChildToVerticalBox(Row))
			RowSlot->SetPadding(FMargin(0.f, 0.f, 0.f, 12.f));
		TitleRow = Row;
	}

	UHorizontalBox* Columns = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
	if (UVerticalBoxSlot* ColumnsSlot = Page->AddChildToVerticalBox(Columns))
		ColumnsSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));

	UVerticalBox* Column[3];
	for (int32 i = 0; i < 3; i++)
	{
		Column[i] = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
		ColumnBoxes.Add(Column[i]);
		if (UHorizontalBoxSlot* ColumnSlot = Columns->AddChildToHorizontalBox(Column[i]))
		{
			FSlateChildSize Size(ESlateSizeRule::Fill);
			Size.Value = S1BaseCampLayout::ColumnWeights[i];
			ColumnSlot->SetSize(Size);
			ColumnSlot->SetPadding(FMargin(i == 0 ? 0.f : S1BaseCampLayout::ScreenPadding * 0.5f, 0.f, i == 2 ? 0.f : S1BaseCampLayout::ScreenPadding * 0.5f, 0.f));
		}
	}

	// ── B-L 보관 ──
	MakeHeader(Column[0], VaultHeader, VaultSort, VaultFilter);
	VaultFilter->OnTextChanged.AddDynamic(this, &US1BaseCampWidget::HandleVaultFilterChanged);
	MakeNotice(Column[0]);
	MakeGrid(Column[0], S1BaseCampLayout::VaultCapacity, VaultSlots);

	// ── B-C 나 — keys first: this screen is choosing keys, not gear (SK:1463) ──
	MakeNotice(Column[1]);

	UHorizontalBox* Strip = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
	for (int32 Part : S1BaseCampLayout::StripParts)
	{
		UVerticalBox* Cell = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());

		US1ItemSlotWidget* PartSlot = CreateWidget<US1ItemSlotWidget>(this, US1ItemSlotWidget::StaticClass());
		PartSlot->SetCellSize(GridCellSize);
		PartSlot->OnClicked.BindUObject(this, &US1BaseCampWidget::HandleSlotClicked);
		PartSlot->OnHovered.BindUObject(this, &US1BaseCampWidget::HandleSlotHovered);
		Cell->AddChildToVerticalBox(PartSlot);

		// Under the cell: the worn item's archetype, or the part's name when nothing is worn (§ Information Hierarchy ②).
		UTextBlock* PartLine = MakeText(FontSmall, TextTertiary());
		Cell->AddChildToVerticalBox(PartLine);
		StripPartLabels.Add(PartLine);

		for (int32 Key : S1BaseCampLayout::KeysOfPart(Part))
		{
			UTextBlock* KeyLine = MakeText(FontSmall, TextSecondary());
			KeyLine->SetAutoWrapText(true);
			Cell->AddChildToVerticalBox(KeyLine);

			StripKeys.Add(KeyLine);
			StripKeySlots.Add(Key);
		}

		if (UHorizontalBoxSlot* CellSlot = Strip->AddChildToHorizontalBox(Cell))
		{
			CellSlot->SetPadding(FMargin(0.f, 0.f, CellGap * 2.f, 0.f));
			CellSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		}

		StripSlots.Add(PartSlot);
	}
	if (UVerticalBoxSlot* StripSlot = Column[1]->AddChildToVerticalBox(Strip))
		StripSlot->SetPadding(FMargin(0.f, 0.f, 0.f, 16.f));

	// 장비 합산 — named so it does not read as the final combat value (2026-10-02 decision)
	TotalsTitle = MakeText(FontEmphasis, TextPrimary());
	TotalsTitle->SetText(Text(TEXT("label.equip_total")));
	if (UVerticalBoxSlot* TitleSlot = Column[1]->AddChildToVerticalBox(TotalsTitle))
		TitleSlot->SetPadding(FMargin(0.f, 0.f, 0.f, 4.f));

	TotalLines = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
	if (UVerticalBoxSlot* LinesSlot = Column[1]->AddChildToVerticalBox(TotalLines))
		LinesSlot->SetPadding(FMargin(0.f, 0.f, 0.f, 16.f));

	// 스킬 제원 — how each key is operated: aim, reach, cast, cooldown (skill-system.md B1a "원자 속성은 보인다").
	// The minimum ahead of design/ux/loadout.md, which owns the full panel.
	UTextBlock* AtomicTitle = MakeText(FontEmphasis, TextPrimary());
	AtomicTitle->SetText(Text(TEXT("label.atomic")));
	if (UVerticalBoxSlot* AtomicTitleSlot = Column[1]->AddChildToVerticalBox(AtomicTitle))
		AtomicTitleSlot->SetPadding(FMargin(0.f, 0.f, 0.f, 4.f));

	AtomicLines = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
	if (UVerticalBoxSlot* AtomicSlot = Column[1]->AddChildToVerticalBox(AtomicLines))
		AtomicSlot->SetPadding(FMargin(0.f, 0.f, 0.f, 16.f));

	Detail = CreateWidget<US1ItemDetailWidget>(this, US1ItemDetailWidget::StaticClass());
	Detail->SetKeyNameResolver([this](int32 KeySlot) { return KeyName(KeySlot); });
	if (UVerticalBoxSlot* DetailSlot = Column[1]->AddChildToVerticalBox(Detail))
		DetailSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	Detail->SetClipping(EWidgetClipping::ClipToBounds);

	// ── B-R 반입 — and [입장] under it, so "what I take" is the last thing read (§ Layout Zones) ──
	MakeHeader(Column[2], CarriedHeader, CarriedSort, CarriedFilter);
	CarriedFilter->OnTextChanged.AddDynamic(this, &US1BaseCampWidget::HandleCarriedFilterChanged);
	MakeNotice(Column[2]);

	const US1ItemSubsystem* Items = GetItems();
	MakeGrid(Column[2], Items != nullptr ? Items->GetCarriedCapacity() : 25, CarriedSlots);

	// Mode row (PVE, ADR-0014) - which room [입장] goes to, and the co-op party window. Small buttons, one line,
	// so the 720p height budget of the column (§ Layout Zones) is not broken
	{
		UHorizontalBox* ModeRow = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
		const TCHAR* ModeKeys[] = { TEXT("mode.pvp"), TEXT("mode.pve_solo"), TEXT("mode.pve_coop") };
		for (const TCHAR* Key : ModeKeys)
		{
			UBorder* Button = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass());
			Button->SetPadding(FMargin(8.f, 4.f));
			UTextBlock* Label = MakeText(FontSmall, TextPrimary());
			Label->SetText(Text(Key));
			Button->SetContent(Label);
			if (UHorizontalBoxSlot* ButtonSlot = ModeRow->AddChildToHorizontalBox(Button))
				ButtonSlot->SetPadding(FMargin(0.f, 0.f, 4.f, 0.f));
			ModeButtons.Add(Button);
			ModeTexts.Add(Label);
		}

		UTextBlock* Spacer = MakeText(FontSmall, TextSecondary());
		if (UHorizontalBoxSlot* SpacerSlot = ModeRow->AddChildToHorizontalBox(Spacer))
			SpacerSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));

		PartyButton = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass());
		PartyButton->SetPadding(FMargin(8.f, 4.f));
		PartyButton->SetBrushColor(CellHover());
		PartyText = MakeText(FontSmall, TextPrimary());
		PartyButton->SetContent(PartyText);
		ModeRow->AddChildToHorizontalBox(PartyButton);

		if (UVerticalBoxSlot* RowSlot = Column[2]->AddChildToVerticalBox(ModeRow))
			RowSlot->SetPadding(FMargin(0.f, 4.f, 0.f, 4.f));
		ModeRowWidget = ModeRow;
	}

	// The server dresses a character with nothing at all - worn, carried and vault empty - in the starter kit
	// on entry (account-login-mvp Core Rule 8). Said here so entering empty-handed is not a surprise either way
	StarterNotice = MakeText(FontSmall, TextSecondary());
	StarterNotice->SetText(Text(TEXT("label.starter_kit")));
	StarterNotice->SetAutoWrapText(true);
	StarterNotice->SetVisibility(ESlateVisibility::Collapsed);
	if (UVerticalBoxSlot* StarterSlot = Column[2]->AddChildToVerticalBox(StarterNotice))
		StarterSlot->SetPadding(FMargin(0.f, 0.f, 0.f, 4.f));

	// [강화 +1] - 상세가 보여 주는(마지막으로 올렸던) 아이템의 레벨을 하나 올린다. 재화 없음 (item-enhance-2026-10-12 R8)
	EnhanceButton = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass());
	EnhanceButton->SetPadding(FMargin(16.f, 8.f));
	EnhanceButton->SetHorizontalAlignment(HAlign_Center);
	EnhanceText = MakeText(FontBody, TextPrimary());
	EnhanceButton->SetContent(EnhanceText);
	if (UVerticalBoxSlot* EnhanceSlot = Column[2]->AddChildToVerticalBox(EnhanceButton))
		EnhanceSlot->SetPadding(FMargin(0.f, 0.f, 0.f, 6.f));

	EnterButton = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass());
	EnterButton->SetPadding(FMargin(16.f, 10.f));
	EnterButton->SetHorizontalAlignment(HAlign_Center);
	EnterText = MakeText(FontEmphasis, TextPrimary());
	EnterButton->SetContent(EnterText);
	Column[2]->AddChildToVerticalBox(EnterButton);

	SetSection(Section);		// 화면에 붙기 전이다 - 첫 Refresh 는 NativeConstruct 가 한다
}

void US1BaseCampWidget::SetSection(ES1CampSection InSection)
{
	Section = (InSection == ES1CampSection::None) ? ES1CampSection::All : InSection;
	if (Background == nullptr)
		return;

	// The whole screen hides the world; an NPC window lets the camp show at its edges
	FLinearColor Fill = PanelBackground();
	Fill.A = (Section == ES1CampSection::All) ? 1.f : 0.985f;		// 0.94 let the bright camp ground wash the grids out (2026-10-17 screenshot)
	Background->SetBrushColor(Fill);

	const TCHAR* TitleKey = Section == ES1CampSection::Stash ? TEXT("camp.title.stash")
		: Section == ES1CampSection::Enhance ? TEXT("camp.title.enhance")
		: Section == ES1CampSection::Gate ? TEXT("camp.title.gate")
		: Section == ES1CampSection::Sell ? TEXT("camp.title.sell") : nullptr;
	// The row stays for the gold (2026-10-17); the whole screen has no title and nothing to close with Esc
	TitleRow->SetVisibility(ESlateVisibility::SelfHitTestInvisible);
	TitleText->SetText(TitleKey != nullptr ? Text(TitleKey) : FText::GetEmpty());
	CloseHint->SetVisibility(TitleKey != nullptr ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
	RefreshGold();

	auto Show = [](UWidget* W, bool bOn) { if (W != nullptr) W->SetVisibility(bOn ? ESlateVisibility::Visible : ESlateVisibility::Collapsed); };
	Show(ColumnBoxes[static_cast<int32>(EColumn::Vault)], ShowsVault());
	Show(ModeRowWidget, ShowsEnter());
	Show(EnterButton, ShowsEnter());
	Show(EnhanceButton, ShowsEnhance());
	if (ShowsEnter() == false)
		StarterNotice->SetVisibility(ESlateVisibility::Collapsed);

	// A pick-only window never leaves a discard waiting
	CancelConfirm();
	// 화면에 붙기 전이면 NativeConstruct 가 곧 새로 그린다 - 처음 열 때 Refresh 가 다섯 번 돌았다 (2026-10-17 리뷰)
	if (IsInViewport())
		Refresh();
}

FReply US1BaseCampWidget::NativeOnKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent)
{
	// An NPC window closes with Esc - back to walking around the camp (S4 and the party window take their own Esc first)
	if (InKeyEvent.GetKey() == EKeys::Escape && Section != ES1CampSection::All)
	{
		if (US1BaseCampSubsystem* BaseCamp = GetBaseCamp())
			BaseCamp->CloseCampPanel();
		return FReply::Handled();
	}
	return Super::NativeOnKeyDown(InGeometry, InKeyEvent);
}

void US1BaseCampWidget::NativeConstruct()
{
	Super::NativeConstruct();

	if (US1ItemSubsystem* Items = GetItems())
	{
		ItemsChangedHandle = Items->OnItemsChanged.AddUObject(this, &US1BaseCampWidget::Refresh);
		KeysChangedHandle = Items->OnKeysChanged.AddUObject(this, &US1BaseCampWidget::Refresh);
		NoticeHandle = Items->OnItemNotice.AddUObject(this, &US1BaseCampWidget::HandleNotice);
	}

	if (US1BaseCampSubsystem* BaseCamp = GetBaseCamp())
	{
		PlaceChangedHandle = BaseCamp->OnPlaceChanged.AddUObject(this, &US1BaseCampWidget::HandlePlaceChanged);
		ReadyHandle = BaseCamp->OnInventoryReady.AddUObject(this, &US1BaseCampWidget::Refresh);
		RejectedHandle = BaseCamp->OnEnterRejected.AddUObject(this, &US1BaseCampWidget::HandleEnterRejected);
	}

	if (US1PartySubsystem* Party = US1PartySubsystem::Get())
		PartyChangedHandle = Party->OnPartyChanged.AddUObject(this, &US1BaseCampWidget::RefreshEnter);

	// S3 opens fresh — at login and after every run. A filter is a search, not a setting: left in
	// place it would hide what came back from the run. The sort order is a preference and stays.
	VaultFilterText.Reset();
	CarriedFilterText.Reset();
	for (UEditableTextBox* Box : { VaultFilter.Get(), CarriedFilter.Get() })
	{
		if (Box != nullptr)
			Box->SetText(FText::GetEmpty());
	}

	Refresh();
}

void US1BaseCampWidget::NativeDestruct()
{
	if (US1ItemSubsystem* Items = GetItems())
	{
		Items->OnItemsChanged.Remove(ItemsChangedHandle);
		Items->OnKeysChanged.Remove(KeysChangedHandle);
		Items->OnItemNotice.Remove(NoticeHandle);
	}

	if (US1BaseCampSubsystem* BaseCamp = GetBaseCamp())
	{
		BaseCamp->OnPlaceChanged.Remove(PlaceChangedHandle);
		BaseCamp->OnInventoryReady.Remove(ReadyHandle);
		BaseCamp->OnEnterRejected.Remove(RejectedHandle);
	}

	if (US1PartySubsystem* Party = US1PartySubsystem::Get())
		Party->OnPartyChanged.Remove(PartyChangedHandle);

	// A co-op member pulled into the run by the leader leaves base camp from under the party window too
	ClosePartyPanel();

	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(ConfirmTimer);
		World->GetTimerManager().ClearTimer(NoticeTimer);
	}

	// S3 leaves when the run starts (or the world goes) — S4 sits on top of it and goes too
	CloseEntryMap();

	Super::NativeDestruct();
}

// ── Refresh ──────────────────────────────────────────────────────────────

bool US1BaseCampWidget::IsLocked() const
{
	const US1BaseCampSubsystem* BaseCamp = GetBaseCamp();
	return BaseCamp == nullptr || BaseCamp->IsInventoryReady() == false || BaseCamp->GetPlace() != ES1Place::BaseCamp;
}

void US1BaseCampWidget::Refresh()
{
	if (GetItems() == nullptr || VaultHeader == nullptr)
		return;

	RefreshGrid(true);
	RefreshGrid(false);
	RefreshStrip();
	RefreshTotals();
	RefreshAtomic();
	RefreshEnter();
	RefreshGold();

	const US1ItemSubsystem* Items = GetItems();
	if (HoveredId != 0 && Items->FindItem(HoveredId) == nullptr)
		HoveredId = 0;
	if (ConfirmId != 0 && Items->FindItem(ConfirmId) == nullptr)
		ConfirmId = 0;

	RefreshDetail();
}

void US1BaseCampWidget::RefreshGrid(bool bVault)
{
	US1ItemSubsystem* Items = GetItems();
	const US1BaseCampSubsystem* BaseCamp = GetBaseCamp();
	const bool bReady = BaseCamp != nullptr && BaseCamp->IsInventoryReady();

	const FS1ItemCatalog& Catalog = Items->GetCatalog();
	TArray<const FS1ItemView*> List = Items->GetByState(bVault ? ES1ItemState::Vaulted : ES1ItemState::Carried);
	const int32 Capacity = bVault ? S1BaseCampLayout::VaultCapacity : Items->GetCarriedCapacity();
	const int32 Total = List.Num();

	// The filter hides cells, not items — the count and "가득 참" stay about the whole column.
	const FString& Filter = bVault ? VaultFilterText : CarriedFilterText;
	if (Filter.IsEmpty() == false)
		List = List.FilterByPredicate([&Filter](const FS1ItemView* Item) { return MatchesFilter(*Item, Filter); });

	S1BaseCampLayout::SortForGrid(List, Catalog, bVault ? bVaultBySkill : bCarriedBySkill);

	// Header: count, and "가득 참" spelled out — never color alone. Before the sync, "불러오는 중".
	FText Header;
	if (bReady == false)
	{
		Header = FText::Format(INVTEXT("{0} · {1}"), Text(bVault ? TEXT("label.vault") : TEXT("label.carry_in")), Text(TEXT("label.loading")));
	}
	else
	{
		Header = FText::Format(INVTEXT("{0} {1}/{2}"), Text(bVault ? TEXT("label.vault") : TEXT("label.carry_in")),
			FText::AsNumber(Total), FText::AsNumber(Capacity));
		if (Total >= Capacity)
			Header = FText::Format(INVTEXT("{0} · ■ {1}"), Header, Text(TEXT("label.full")));
		if (Filter.IsEmpty() == false)
			Header = FText::Format(INVTEXT("{0} · {1}"), Header, FText::Format(Text(TEXT("label.filter_count")), FText::AsNumber(List.Num())));
	}

	(bVault ? VaultHeader : CarriedHeader)->SetText(Header);
	(bVault ? VaultSort : CarriedSort)->SetText(FText::Format(INVTEXT("⇅ {0}"),
		Text((bVault ? bVaultBySkill : bCarriedBySkill) ? TEXT("label.sort_skill") : TEXT("label.sort_name"))));

	TArray<TObjectPtr<US1ItemSlotWidget>>& Cells = bVault ? VaultSlots : CarriedSlots;
	for (int32 i = 0; i < Cells.Num(); i++)
	{
		if (bReady && List.IsValidIndex(i))
		{
			const FS1ItemView& Item = *List[i];
			Cells[i]->SetItem(Item.InstanceId, Item.Grade, SlotLabel(Catalog.Find(Item.ItemTypeId), Item), Items->IsPending(Item.InstanceId));
		}
		else
		{
			Cells[i]->SetEmpty(false);
		}
	}
}

void US1BaseCampWidget::RefreshStrip()
{
	US1ItemSubsystem* Items = GetItems();
	const FS1ItemCatalog& Catalog = Items->GetCatalog();

	for (int32 i = 0; i < StripSlots.Num(); i++)
	{
		const FS1ItemView* Worn = Items->GetEquipped(S1BaseCampLayout::StripParts[i]);
		const FS1ItemTypeDef* Type = Worn != nullptr ? Catalog.Find(Worn->ItemTypeId) : nullptr;

		if (Worn != nullptr)
			StripSlots[i]->SetItem(Worn->InstanceId, Worn->Grade, SlotLabel(Type, *Worn), Items->IsPending(Worn->InstanceId));
		else
			StripSlots[i]->SetEmpty(true);

		if (StripPartLabels.IsValidIndex(i))
			StripPartLabels[i]->SetText(Type != nullptr ? ArchetypeLabel(Type->Archetype) : PartLabel(S1BaseCampLayout::StripParts[i]));
	}

	const TMap<int32, uint32> KeySkills = ComputeKeySkills();

	for (int32 i = 0; i < StripKeys.Num(); i++)
	{
		const int32 Key = StripKeySlots[i];
		const uint32 SkillId = KeySkills.FindRef(Key);

		// Shadowed: the same skill already sits on an earlier key. S3 is where the reason shows (EB:537-542).
		const int32 Shadower = ShadowedBy(KeySkills, Key);

		const FText Name = SkillId != 0 ? SkillName(SkillId) : FText::AsCultureInvariant(TEXT("—"));
		FText Line = FText::Format(INVTEXT("[{0}] {1}"), KeyName(Key), Name);
		if (Shadower != 0)
			Line = FText::Format(INVTEXT("{0}\n{1}"), Line, FText::Format(Text(TEXT("label.shadowed_by")), KeyName(Shadower)));

		StripKeys[i]->SetText(Line);
		StripKeys[i]->SetColorAndOpacity(FSlateColor(Shadower != 0 ? TextShadowed() : TextSecondary()));
	}
}

TMap<int32, uint32> US1BaseCampWidget::ComputeKeySkills() const
{
	// The keys come from S_EQUIP_SYNC, which only the dungeon sends. In base camp they are worked
	// out from what is worn — the same part + order rule the server binds by (S1ItemLogic::KeysFor).
	TMap<int32, uint32> KeySkills;
	const US1ItemSubsystem* Items = GetItems();
	if (Items == nullptr)
		return KeySkills;

	for (int32 Part : S1BaseCampLayout::StripParts)
	{
		const FS1ItemView* Worn = Items->GetEquipped(Part);
		const FS1ItemTypeDef* Type = Worn != nullptr ? Items->GetCatalog().Find(Worn->ItemTypeId) : nullptr;
		if (Type == nullptr)
			continue;

		for (const FS1KeySkill& KeySkill : S1ItemLogic::KeysFor(*Type, *Worn))
			KeySkills.Add(KeySkill.KeySlot, KeySkill.SkillId);
	}
	return KeySkills;
}

int32 US1BaseCampWidget::ShadowedBy(const TMap<int32, uint32>& KeySkills, int32 Key)
{
	const uint32 SkillId = KeySkills.FindRef(Key);
	if (SkillId == 0)
		return 0;

	for (int32 Earlier = 1; Earlier < Key; Earlier++)
	{
		if (KeySkills.FindRef(Earlier) == SkillId)
			return Earlier;
	}
	return 0;
}

bool US1BaseCampWidget::MatchesFilter(const FS1ItemView& Item, const FString& Filter)
{
	if (Filter.IsEmpty())
		return true;

	return Item.Skills.ContainsByPredicate([&Filter](const FS1ItemSkill& Skill)
	{
		return Skill.SkillId != 0 && SkillName(Skill.SkillId).ToString().Contains(Filter, ESearchCase::IgnoreCase);
	});
}

void US1BaseCampWidget::RefreshAtomic()
{
	AtomicLinePool.Begin(AtomicLines);

	auto AddLine = [this](const FText& Line, int32 Size, const FLinearColor& Color, float Bottom)
	{
		UTextBlock* Text = AtomicLinePool.Next(WidgetTree, AtomicLines, FMargin(0.f, 0.f, 0.f, Bottom));
		Text->SetFont(Font(Size));
		Text->SetColorAndOpacity(FSlateColor(Color));
		Text->SetAutoWrapText(true);
		Text->SetText(Line);
	};

	// Live keys only — a shadowed key does nothing, so its numbers would be noise (the strip already says why).
	const TMap<int32, uint32> KeySkills = ComputeKeySkills();
	int32 Shown = 0;
	for (int32 Key = 1; Key <= 6; Key++)
	{
		const uint32 SkillId = KeySkills.FindRef(Key);
		const FS1SkillDef* Def = SkillId != 0 ? FS1SkillTable::Find(SkillId) : nullptr;
		if (Def == nullptr || ShadowedBy(KeySkills, Key) != 0)
			continue;

		AddLine(FText::Format(INVTEXT("[{0}] {1}  {2}"), KeyName(Key), SkillName(SkillId), S1BaseCampLayout::AtomicLine(*Def)),
			FontBody, TextSecondary(), 0.f);
		AddLine(SkillDesc(SkillId), FontSmall, TextTertiary(), 6.f);
		Shown++;
	}

	if (Shown == 0)
		AddLine(FText::AsCultureInvariant(TEXT("—")), FontBody, TextTertiary(), 0.f);
}

void US1BaseCampWidget::HandleVaultFilterChanged(const FText& InText)
{
	VaultFilterText = InText.ToString().TrimStartAndEnd();
	if (GetItems() != nullptr && VaultHeader != nullptr)
		RefreshGrid(true);
}

void US1BaseCampWidget::HandleCarriedFilterChanged(const FText& InText)
{
	CarriedFilterText = InText.ToString().TrimStartAndEnd();
	if (GetItems() != nullptr && VaultHeader != nullptr)
		RefreshGrid(false);
}

void US1BaseCampWidget::RefreshTotals()
{
	US1ItemSubsystem* Items = GetItems();
	TotalLinePool.Begin(TotalLines);

	// Stats in the order they first appear across the parts, summed over everything worn.
	TArray<FName> Order;
	TMap<FName, float> Sums;
	for (int32 Part : S1BaseCampLayout::StripParts)
	{
		const FS1ItemView* Worn = Items->GetEquipped(Part);
		const FS1ItemTypeDef* Type = Worn != nullptr ? Items->GetCatalog().Find(Worn->ItemTypeId) : nullptr;
		if (Type == nullptr)
			continue;

		for (const FS1ItemStat& Stat : Type->Stats)
		{
			if (Sums.Contains(Stat.Stat) == false)
				Order.Add(Stat.Stat);

			Sums.FindOrAdd(Stat.Stat) += S1ItemLogic::StatValue(Stat, Worn->Grade, Items->GetGradeMultipliers(), Worn->Level);
		}
	}

	auto AddLine = [this](const FText& Line, const FLinearColor& Color)
	{
		UTextBlock* Text = TotalLinePool.Next(WidgetTree, TotalLines);
		Text->SetFont(Font(FontBody));
		Text->SetColorAndOpacity(FSlateColor(Color));
		Text->SetText(Line);
	};

	if (Order.Num() == 0)
	{
		AddLine(FText::AsCultureInvariant(TEXT("—")), TextTertiary());
		return;
	}

	for (const FName& Stat : Order)
		AddLine(FText::Format(INVTEXT("{0}  {1}"), StatLabel(Stat), StatValueText(Stat, Sums[Stat])), TextSecondary());
}

void US1BaseCampWidget::RefreshEnter()
{
	const US1BaseCampSubsystem* BaseCamp = GetBaseCamp();
	const bool bCanEnter = IsLocked() == false && BaseCamp != nullptr && BaseCamp->CanEnter();

	// Empty-handed entry is normal (GC:644-646) — "not loaded yet", "already going" and a mode that does not fit the
	// party (PVE 협동 only for a party of 2+, a member waits for the leader) disable it. The label says which (ADR-0014)
	EnterText->SetText(Text(BaseCamp != nullptr ? BaseCamp->GetEnterLabelKey() : TEXT("label.enter")));
	EnterText->SetColorAndOpacity(FSlateColor(bCanEnter ? TextPrimary() : TextShadowed()));
	EnterButton->SetBrushColor(bCanEnter ? CellHover() : CellBackground());

	RefreshMode();

	// Same condition the server checks (ServerPacketHandler LoadPlayerItems ownedRows == 0) - only once the
	// base-camp sync is in, so a half-loaded S3 never promises a kit the server will not give
	const US1ItemSubsystem* Items = GetItems();
	const bool bOwnsNothing = Items != nullptr
		&& Items->CountByState(ES1ItemState::Vaulted) == 0
		&& Items->CountByState(ES1ItemState::Carried) == 0
		&& Items->CountByState(ES1ItemState::Equipped) == 0;
	const bool bReady = BaseCamp != nullptr && BaseCamp->IsInventoryReady();
	StarterNotice->SetVisibility(bReady && bOwnsNothing && ShowsEnter() ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
}

void US1BaseCampWidget::RefreshDetail()
{
	US1ItemSubsystem* Items = GetItems();
	if (Items == nullptr || Detail == nullptr)
		return;

	if (ConfirmId != 0)
	{
		if (const FS1ItemView* Item = Items->FindItem(ConfirmId))
		{
			Detail->ShowDiscardConfirm(*Items, *Item);
			return;
		}
	}

	const FS1ItemView* Item = ShownId != 0 ? Items->FindItem(ShownId) : nullptr;
	if (Item == nullptr)
	{
		Detail->Clear();
		RefreshEnhance();
		return;
	}

	const FS1ItemTypeDef* Type = Items->GetCatalog().Find(Item->ItemTypeId);
	const FS1ItemView* Worn = Type != nullptr ? Items->GetEquipped(Type->EquipSlot) : nullptr;
	Detail->ShowItem(*Items, *Item, Worn);
	RefreshEnhance();
}

void US1BaseCampWidget::RefreshGold()
{
	if (GoldText == nullptr)
		return;
	const US1ItemSubsystem* Items = GetItems();
	const int64 Gold = Items != nullptr ? Items->GetGold() : -1;
	GoldText->SetText(Gold >= 0 ? FText::Format(Text(TEXT("label.gold")), FText::AsNumber(Gold)) : FText::GetEmpty());
}

void US1BaseCampWidget::RefreshEnhance()
{
	if (EnhanceButton == nullptr || EnhanceText == nullptr)
		return;
	US1ItemSubsystem* Items = GetItems();
	const FS1ItemView* Item = (Items != nullptr && ShownId != 0) ? Items->FindItem(ShownId) : nullptr;
	const bool bLoot = Item != nullptr && Items->IsLoot(Item->ItemTypeId);
	const bool bMax = Item != nullptr && Item->Level >= S1ItemLogic::ItemLevelMax;

	// The cost (2026-10-17 economy): 20 × 1.25^level - shown on the button, dimmed when the vault cannot pay
	const uint64 Cost = (Item != nullptr && Items != nullptr) ? S1Economy::EnhanceCost(Item->Level, Items->GetEconomy()) : 0;
	const bool bAfford = Items != nullptr && Items->GetGold() >= static_cast<int64>(Cost);
	const bool bCan = Item != nullptr && bLoot == false && bMax == false && bAfford && Items->IsPending(ShownId) == false && IsLocked() == false;

	if (Item == nullptr)
		EnhanceText->SetText(Text(TEXT("button.enhance_pick")));
	else if (bLoot)
		EnhanceText->SetText(Text(TEXT("button.enhance_loot")));
	else if (bMax)
		EnhanceText->SetText(FText::Format(Text(TEXT("button.enhance_max")), FText::AsNumber(static_cast<int32>(Item->Level))));
	else
		EnhanceText->SetText(FText::Format(Text(TEXT("button.enhance_cost")), FText::AsNumber(static_cast<int32>(Item->Level)),
			FText::AsNumber(static_cast<int32>(Item->Level + 1)), FText::AsNumber(Cost)));
	EnhanceText->SetColorAndOpacity(FSlateColor(bCan ? TextPrimary() : S1ItemUIStyle::TextTertiary()));
	EnhanceButton->SetBrushColor(bCan ? CellHover() : CellBackground());
}

// ── Input ────────────────────────────────────────────────────────────────

FReply US1BaseCampWidget::NativeOnMouseButtonDown(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent)
{
	const FVector2D At = InMouseEvent.GetScreenSpacePosition();
	auto IsUnder = [&At](const UWidget* Widget) { return Widget != nullptr && Widget->GetCachedGeometry().IsUnderLocation(At); };

	// A camp map NPC window shows only its part - a hidden button keeps its old geometry, so never ask it (2026-10-17)
	const bool bEnterShown = ShowsEnter();

	// S3 is focusable now (Esc closes an NPC window), so a click here takes the keyboard by itself - a window opened
	// from the click must take it back in the reply, or S4 loses 1~9 · R · Esc (2026-10-17 live check)
	auto FocusTo = [](UUserWidget* Opened) { return Opened != nullptr ? FReply::Handled().SetUserFocus(Opened->TakeWidget(), EFocusCause::SetDirectly) : FReply::Handled(); };

	// Mode row and party window (PVE, ADR-0014) - changing the mode is a base-camp action like the others
	for (int32 i = 0; i < ModeButtons.Num() && bEnterShown; i++)
	{
		if (InMouseEvent.GetEffectingButton() == EKeys::LeftMouseButton && IsUnder(ModeButtons[i]))
		{
			if (US1BaseCampSubsystem* BaseCamp = GetBaseCamp(); BaseCamp != nullptr && IsLocked() == false)
			{
				BaseCamp->SetRunMode(static_cast<ES1RunMode>(i));
				RefreshEnter();
			}
			return FReply::Handled();
		}
	}
	if (bEnterShown && InMouseEvent.GetEffectingButton() == EKeys::LeftMouseButton && IsUnder(PartyButton))
	{
		OpenPartyPanel();
		return FocusTo(PartyPanel);
	}

	if (ShowsEnhance() && InMouseEvent.GetEffectingButton() == EKeys::LeftMouseButton && IsUnder(EnhanceButton))
	{
		// [강화 +1] (item-enhance-2026-10-12 R8): 상세가 보여 주는 아이템, 16 이면 아무것도 안 한다. 응답 전엔 pending 으로 흐려진다
		US1ItemSubsystem* Items = GetItems();
		const FS1ItemView* Item = (Items != nullptr && ShownId != 0) ? Items->FindItem(ShownId) : nullptr;
		if (Item == nullptr)
			ShowNotice(EColumn::Center, Text(TEXT("notice.enhance_pick")), 0);
		else if (Items->IsLoot(Item->ItemTypeId))
			ShowNotice(EColumn::Center, Notice(ES1ItemNotice::LootNotEquippable, 0), ShownId);
		else if (Items->GetGold() < static_cast<int64>(S1Economy::EnhanceCost(Item->Level, Items->GetEconomy())))
			ShowNotice(EColumn::Center, Notice(ES1ItemNotice::NotEnoughGold, 0), ShownId);
		else if (Item->Level < S1ItemLogic::ItemLevelMax && Items->IsPending(ShownId) == false && IsLocked() == false)
		{
			CancelConfirm();
			Items->RequestEnhance(ShownId);
			Refresh();
		}
		return FReply::Handled();
	}

	if (bEnterShown && InMouseEvent.GetEffectingButton() == EKeys::LeftMouseButton && IsUnder(EnterButton))
	{
		// [입장] — no shortcut on purpose (§ Interaction Map: 실수 방지). It opens S4, the entry-point map;
		// C_ENTER_GAME goes out from there once a point or [무작위] is picked (design/ux/entry-point.md)
		const US1BaseCampSubsystem* BaseCamp = GetBaseCamp();
		if (IsLocked() == false && BaseCamp != nullptr && BaseCamp->CanEnter())
		{
			CancelConfirm();
			ClearNotices();
			OpenEntryMap();
			return FocusTo(EntryMap);
		}
		else if (IsLocked() == false && BaseCamp != nullptr)
		{
			// Why not - the same rule the server would refuse with
			const S1PartyLogic::EEnterBlock Block = BaseCamp->GetEnterBlock();
			ShowNotice(EColumn::Carried, Text(Block == S1PartyLogic::EEnterBlock::CoopOnly ? TEXT("notice.party_coop_only")
				: Block == S1PartyLogic::EEnterBlock::WaitForParty ? TEXT("notice.party_wait_back") : TEXT("notice.party_wait_leader")), 0);
		}
	}
	else if (ShowsVault() && IsUnder(VaultSort))
	{
		bVaultBySkill = !bVaultBySkill;
		RefreshGrid(true);
	}
	else if (IsUnder(CarriedSort))
	{
		bCarriedBySkill = !bCarriedBySkill;
		RefreshGrid(false);
	}
	else
	{
		CancelConfirm();
	}

	return FReply::Handled();
}

void US1BaseCampWidget::HandleSlotHovered(uint64 InstanceId)
{
	HoveredId = InstanceId;
	// sticky - 상세와 [강화] 버튼은 마우스가 떠나도 마지막 아이템을 본다 (item-enhance 2026-10-12).
	// 떠날 때(0) · 같은 칸에 다시 들어올 때는 보여 줄 것이 그대로다 - 칸을 넘을 때마다 상세를 두 번 새로 만들었다 (2026-10-17)
	if (InstanceId == 0 || InstanceId == ShownId)
		return;
	ShownId = InstanceId;
	RefreshDetail();
}

void US1BaseCampWidget::HandleSlotClicked(uint64 InstanceId, FKey Button, bool bCtrl)
{
	US1ItemSubsystem* Items = GetItems();
	if (Items == nullptr || IsLocked())
		return;

	const FS1ItemView* Item = InstanceId != 0 ? Items->FindItem(InstanceId) : nullptr;
	if (Item == nullptr)
	{
		CancelConfirm();
		return;
	}

	// Waiting on the server — the cell is dimmed and repeat clicks send nothing (AC-UI-22).
	if (Items->IsPending(InstanceId))
		return;

	// 대장장이 · 길드장 windows (camp map, 2026-10-17): a click picks the item for the detail and [강화 +1] - moving
	// things between vault, carried and worn is the 창고지기's job
	if (PickOnly())
	{
		ShownId = InstanceId;
		RefreshDetail();
		return;
	}

	// 매입 행수 (2026-10-17 economy, user decision: a click sells at once) - the whole cell. Worn gear comes off first
	if (SellsOnClick())
	{
		ShownId = InstanceId;
		if (Item->State == ES1ItemState::Equipped)
		{
			ShowNotice(EColumn::Center, Notice(ES1ItemNotice::SellEquipped, 0), InstanceId);
			ShakeSlot(InstanceId);
			return;
		}
		CancelConfirm();
		Items->RequestSell(InstanceId);
		Refresh();
		return;
	}

	if (bCtrl && Button == EKeys::LeftMouseButton)
	{
		Discard(*Item);
		return;
	}

	if (ConfirmId != 0)
		CancelConfirm();

	const bool bLeft = Button == EKeys::LeftMouseButton;
	const bool bRight = Button == EKeys::RightMouseButton;

	switch (Item->State)
	{
	case ES1ItemState::Vaulted:
		if (bLeft)
		{
			Items->RequestStash(InstanceId, /*bToVault*/ false);
		}
		else if (bRight)
		{
			// Not silently ignored — say why (AC-UI-30, 원칙 3)
			ShowNotice(EColumn::Vault, Text(TEXT("notice.vault_equip")), InstanceId);
			ShakeSlot(InstanceId);
		}
		break;

	case ES1ItemState::Carried:
		if (bLeft)
		{
			Items->RequestStash(InstanceId, /*bToVault*/ true);
		}
		else if (bRight)
		{
			// Loot is sell-only (2026-10-17 economy) - say where it goes instead of "할 수 없는 동작"
			if (Items->IsLoot(Item->ItemTypeId))
			{
				ShowNotice(EColumn::Carried, Notice(ES1ItemNotice::LootNotEquippable, 0), InstanceId);
				ShakeSlot(InstanceId);
				return;
			}

			const FS1ItemTypeDef* Type = Items->GetCatalog().Find(Item->ItemTypeId);
			if (Type == nullptr)
			{
				ShowNotice(EColumn::Carried, Notice(ES1ItemNotice::BadRequest, 0), InstanceId);
				ShakeSlot(InstanceId);
				return;
			}
			Items->RequestEquip(InstanceId, Type->EquipSlot);
		}
		break;

	case ES1ItemState::Equipped:
		if (bLeft || bRight)
			Items->RequestUnequip(InstanceId);
		break;

	default:
		break;
	}

	Refresh();	// Show the pending dim right away
}

void US1BaseCampWidget::Discard(const FS1ItemView& Item)
{
	US1ItemSubsystem* Items = GetItems();
	if (Items == nullptr)
		return;

	const EColumn Column = Item.State == ES1ItemState::Vaulted ? EColumn::Vault
		: Item.State == ES1ItemState::Carried ? EColumn::Carried : EColumn::Center;

	switch (Items->DiscardRuleFor(Item.InstanceId))
	{
	case ES1DiscardRule::Reject:
		CancelConfirm();
		ShowNotice(Column, Notice(ES1ItemNotice::MustUnequipFirst, 0), Item.InstanceId);
		ShakeSlot(Item.InstanceId);
		return;

	case ES1DiscardRule::Immediate:
		CancelConfirm();
		Items->RequestDrop(Item.InstanceId);
		Refresh();
		return;

	case ES1DiscardRule::Confirm:
		if (ConfirmId == Item.InstanceId)
		{
			// Second Ctrl+click on the same cell inside the window — it goes. In base camp it is gone for good.
			CancelConfirm();
			Items->RequestDrop(Item.InstanceId);
			Refresh();
			return;
		}

		ConfirmId = Item.InstanceId;
		if (UWorld* World = GetWorld())
		{
			World->GetTimerManager().SetTimer(ConfirmTimer, FTimerDelegate::CreateUObject(this, &US1BaseCampWidget::CancelConfirm),
				Items->GetDiscardConfirmSeconds(), false);
		}
		RefreshDetail();
		return;
	}
}

void US1BaseCampWidget::CancelConfirm()
{
	if (ConfirmId == 0)
		return;

	ConfirmId = 0;
	if (UWorld* World = GetWorld())
		World->GetTimerManager().ClearTimer(ConfirmTimer);

	RefreshDetail();
}

// ── Model and flow events ────────────────────────────────────────────────

void US1BaseCampWidget::HandleNotice(ES1ItemNotice InNotice, uint64 InstanceId, int32 Count)
{
	const US1BaseCampSubsystem* BaseCamp = GetBaseCamp();
	if (BaseCamp == nullptr || BaseCamp->IsInBaseCamp() == false)
		return;

	// The reason goes to the column the item belongs to; a full column names itself (§ Events Fired).
	EColumn Column = EColumn::Center;
	ES1ItemNotice Shown = InNotice;

	if (InNotice == ES1ItemNotice::VaultFull)
	{
		Column = EColumn::Vault;
	}
	else if (InNotice == ES1ItemNotice::CarriedFull || InNotice == ES1ItemNotice::InventoryFull)
	{
		// In base camp the carried grid is "반입" — taking a part off into a full one says so.
		Column = EColumn::Carried;
		Shown = ES1ItemNotice::CarriedFull;
	}
	else if (const US1ItemSubsystem* Items = GetItems())
	{
		if (const FS1ItemView* Item = Items->FindItem(InstanceId))
		{
			Column = Item->State == ES1ItemState::Vaulted ? EColumn::Vault
				: Item->State == ES1ItemState::Carried ? EColumn::Carried : EColumn::Center;
		}
	}

	ShowNotice(Column, Notice(Shown, Count), InstanceId);
	ShakeSlot(InstanceId);
}

void US1BaseCampWidget::HandlePlaceChanged(ES1Place Place)
{
	if (Place == ES1Place::BaseCamp)
	{
		// Back from a run — what was hovered or being discarded belongs to the old view.
		HoveredId = 0;
		CancelConfirm();
	}

	Refresh();
}

void US1BaseCampWidget::HandleEnterRejected()
{
	// The lock is already off (place is BaseCamp again). S4 goes away and S3 says why next to the button.
	CloseEntryMap();
	ShowNotice(EColumn::Carried, Text(TEXT("notice.enter_rejected")), 0);
	Refresh();
}

void US1BaseCampWidget::OpenEntryMap()
{
	if (EntryMap == nullptr)
		EntryMap = CreateWidget<US1EntryMapWidget>(GetOwningPlayer(), US1EntryMapWidget::StaticClass());

	if (EntryMap == nullptr)
		return;

	// Over S3 (base camp sits at 20). S4 takes the keyboard for 1~9, R and Esc
	constexpr int32 EntryMapZOrder = 30;
	if (EntryMap->IsInViewport() == false)
		EntryMap->AddToViewport(EntryMapZOrder);

	EntryMap->SetKeyboardFocus();
}

void US1BaseCampWidget::CloseEntryMap()
{
	if (EntryMap != nullptr && EntryMap->IsInViewport())
		EntryMap->Close();
}

void US1BaseCampWidget::RefreshMode()
{
	const US1BaseCampSubsystem* BaseCamp = GetBaseCamp();
	const ES1RunMode Mode = BaseCamp != nullptr ? BaseCamp->GetRunMode() : ES1RunMode::Pvp;
	for (int32 i = 0; i < ModeButtons.Num(); i++)
	{
		const bool bOn = static_cast<int32>(Mode) == i;
		ModeButtons[i]->SetBrushColor(bOn ? FLinearColor(FColor(0x39, 0x87, 0xE5)) : CellBackground());
		ModeTexts[i]->SetColorAndOpacity(FSlateColor(bOn ? TextPrimary() : TextSecondary()));
	}

	if (PartyText != nullptr)
	{
		const US1PartySubsystem* Party = US1PartySubsystem::Get();
		const int32 Size = Party != nullptr ? Party->Size() : 0;
		PartyText->SetText(Size > 0 ? FText::Format(Text(TEXT("party.open_n")), FText::AsNumber(Size)) : Text(TEXT("party.open")));
	}
}

void US1BaseCampWidget::OpenPartyPanel()
{
	if (PartyPanel == nullptr)
		PartyPanel = CreateWidget<US1PartyPanelWidget>(GetOwningPlayer(), US1PartyPanelWidget::StaticClass());
	if (PartyPanel == nullptr)
		return;

	// Over S3 like S4 (30); the invite popup (40) stays above both
	constexpr int32 PartyPanelZOrder = 30;
	if (PartyPanel->IsInViewport() == false)
		PartyPanel->AddToViewport(PartyPanelZOrder);
	PartyPanel->SetKeyboardFocus();
}

void US1BaseCampWidget::ClosePartyPanel()
{
	if (PartyPanel != nullptr && PartyPanel->IsInViewport())
		PartyPanel->Close();
}

void US1BaseCampWidget::ShowNotice(EColumn Column, const FText& InText, uint64 InstanceId)
{
	ClearNotices();

	const int32 Index = static_cast<int32>(Column);
	if (Notices.IsValidIndex(Index) && Notices[Index] != nullptr)
		Notices[Index]->SetText(InText);

	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().SetTimer(NoticeTimer, FTimerDelegate::CreateUObject(this, &US1BaseCampWidget::ClearNotices),
			NoticeSeconds, false);
	}
}

void US1BaseCampWidget::ClearNotices()
{
	for (UTextBlock* Line : Notices)
	{
		if (Line != nullptr)
			Line->SetText(FText::GetEmpty());
	}
}

void US1BaseCampWidget::ShakeSlot(uint64 InstanceId)
{
	if (InstanceId == 0)
		return;

	for (TArray<TObjectPtr<US1ItemSlotWidget>>* Cells : { &VaultSlots, &CarriedSlots, &StripSlots })
	{
		for (US1ItemSlotWidget* Cell : *Cells)
		{
			if (Cell->GetInstanceId() == InstanceId)
				Cell->Shake();
		}
	}
}
