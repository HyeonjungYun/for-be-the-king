// Fill out your copyright notice in the Description page of Project Settings.

#include "Game/UI/S1TestModeWidget.h"
#include "Game/UI/S1ItemUIStyle.h"
#include "Game/Item/S1ItemSubsystem.h"
#include "Game/Item/S1ItemCatalog.h"
#include "Game/Monster/S1MonsterCatalog.h"
#include "Game/S1MyPlayer.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/SizeBox.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "S1.h"
#include "Protocol.pb.h"

using namespace S1ItemUIStyle;

namespace
{
	// 🔴 Unity build - anonymous-namespace names share one TU with the other S1 files. Keep them unique (TestMode*)
	constexpr float		TestModePanelWidth	= 380.f;
	constexpr float		TestModeLabelWidth	= 118.f;
	constexpr float		TestModeAwaitSeconds = 1.5f;
	constexpr int32		TestModeLevelMax	= 20;
	constexpr int32		TestModeDummyMax	= 10;

	FString TestModeDataDirectory()
	{
		FString Relative;
		GConfig->GetString(TEXT("/Script/S1.S1ItemSubsystem"), TEXT("ItemDataDirectory"), Relative, GGameIni);
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), Relative));
	}

	// Server/Data rows - comma separated, no quotes, header first
	bool TestModeReadCsv(const FString& FileName, TArray<FString>& OutHeader, TArray<TArray<FString>>& OutRows)
	{
		FString Text;
		if (FFileHelper::LoadFileToString(Text, *FPaths::Combine(TestModeDataDirectory(), FileName)) == false)
			return false;

		TArray<FString> Lines;
		Text.ParseIntoArrayLines(Lines, true);
		if (Lines.Num() == 0)
			return false;

		Lines[0].TrimStartAndEndInline();
		Lines[0].RemoveFromStart(TEXT("\xFEFF"));
		Lines[0].ParseIntoArray(OutHeader, TEXT(","), false);

		for (int32 i = 1; i < Lines.Num(); i++)
		{
			Lines[i].TrimStartAndEndInline();
			if (Lines[i].IsEmpty())
				continue;
			TArray<FString> Cells;
			Lines[i].ParseIntoArray(Cells, TEXT(","), false);
			OutRows.Add(MoveTemp(Cells));
		}
		return true;
	}

	int32 TestModeWrap(int32 Index, int32 Num)
	{
		return Num > 0 ? ((Index % Num) + Num) % Num : 0;
	}
}

// ── Lists ─────────────────────────────────────────────────────────────────

void US1TestModeWidget::LoadLists()
{
	WeaponTypes.Reset();
	for (TArray<uint32>& Types : PartTypes)
	{
		Types.Reset();
		Types.Add(0);		// "그대로"
	}

	if (const US1ItemSubsystem* Items = US1ItemSubsystem::Get())
	{
		for (const TPair<uint32, FS1ItemTypeDef>& Pair : Items->GetCatalog().GetTypes())
		{
			// (not "Slot" - UWidget::Slot, C4458)
			const int32 EquipSlot = Pair.Value.EquipSlot;
			if (EquipSlot == 1)
				WeaponTypes.Add(Pair.Key);
			else if (EquipSlot >= 3 && EquipSlot <= 6)
				PartTypes[EquipSlot - 3].Add(Pair.Key);
		}
	}
	WeaponTypes.Sort();
	for (TArray<uint32>& Types : PartTypes)
		Types.Sort();

	Pools.Reset();
	{
		TArray<FString> Header;
		TArray<TArray<FString>> Rows;
		if (TestModeReadCsv(TEXT("skill_pools.csv"), Header, Rows))
		{
			const int32 ColPool = Header.IndexOfByKey(TEXT("pool_id"));
			const int32 ColSkill = Header.IndexOfByKey(TEXT("skill_id"));
			for (const TArray<FString>& Row : Rows)
			{
				if (ColPool == INDEX_NONE || ColSkill == INDEX_NONE || Row.Num() <= FMath::Max(ColPool, ColSkill))
					continue;
				Pools.FindOrAdd(FName(*Row[ColPool])).AddUnique(static_cast<uint32>(FCString::Atoi(*Row[ColSkill])));
			}
		}
	}
	for (TPair<FName, TArray<uint32>>& Pair : Pools)
		Pair.Value.Sort();

	RequiresWeapon.Reset();
	{
		TArray<FString> Header;
		TArray<TArray<FString>> Rows;
		if (TestModeReadCsv(TEXT("skills.csv"), Header, Rows))
		{
			const int32 ColId = Header.IndexOfByKey(TEXT("skill_id"));
			const int32 ColReq = Header.IndexOfByKey(TEXT("requires_weapon"));
			for (const TArray<FString>& Row : Rows)
			{
				if (ColId == INDEX_NONE || ColReq == INDEX_NONE || Row.Num() <= ColReq || Row[ColReq].IsEmpty())
					continue;
				// "sword:ghost" - the weapon part is enough for the label
				FString Weapon;
				FString Tag;
				if (Row[ColReq].Split(TEXT(":"), &Weapon, &Tag) == false)
					Weapon = Row[ColReq];
				RequiresWeapon.Add(static_cast<uint32>(FCString::Atoi(*Row[ColId])), FName(*Weapon));
			}
		}
	}

	MonsterTypes.Reset();
	for (const TPair<uint32, FS1MonsterTypeDef>& Pair : FS1MonsterCatalog::Get().GetTypes())
		MonsterTypes.Add(Pair.Key);
	MonsterTypes.Sort();

	UE_LOG(LogTemp, Log, TEXT("[TESTMODE] lists weapons=%d helmets=%d armors=%d boots=%d trinkets=%d pools=%d monsters=%d"),
		WeaponTypes.Num(), PartTypes[0].Num() - 1, PartTypes[1].Num() - 1, PartTypes[2].Num() - 1, PartTypes[3].Num() - 1, Pools.Num(), MonsterTypes.Num());
}

TArray<uint32> US1TestModeWidget::SkillOptions(uint32 ItemTypeId) const
{
	TArray<uint32> Options;
	Options.Add(0);

	const US1ItemSubsystem* Items = US1ItemSubsystem::Get();
	const FS1ItemTypeDef* Type = (Items != nullptr && ItemTypeId != 0) ? Items->GetCatalog().Find(ItemTypeId) : nullptr;
	if (Type == nullptr)
		return Options;

	if (const TArray<uint32>* Pool = Pools.Find(Type->SkillPoolId))
		Options.Append(*Pool);
	return Options;
}

uint32 US1TestModeWidget::StepSkill(const TArray<uint32>& Options, uint32 Current, int32 Delta, uint32 Avoid)
{
	if (Options.Num() == 0)
		return 0;

	int32 Index = Options.IndexOfByKey(Current);
	if (Index == INDEX_NONE)
		Index = 0;

	// Shift and Q cannot hold the same skill (the server rejects it) - skip over the other key's pick
	for (int32 Tries = 0; Tries < Options.Num(); Tries++)
	{
		Index = TestModeWrap(Index + Delta, Options.Num());
		if (Options[Index] == 0 || Options[Index] != Avoid)
			return Options[Index];
	}
	return Current;
}

uint32 US1TestModeWidget::CurrentWeaponType() const
{
	return WeaponTypes.IsValidIndex(WeaponIndex) ? WeaponTypes[WeaponIndex] : 0;
}

uint32 US1TestModeWidget::CurrentPartType(int32 Part) const
{
	return PartTypes[Part].IsValidIndex(PartIndex[Part]) ? PartTypes[Part][PartIndex[Part]] : 0;
}

// ── Build ─────────────────────────────────────────────────────────────────

void US1TestModeWidget::AddSection(UVerticalBox* Parent, const TCHAR* Key)
{
	UTextBlock* Header = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
	Header->SetFont(Font(FontBody));
	Header->SetColorAndOpacity(FSlateColor(TextTertiary()));
	Header->SetText(Text(Key));
	if (UVerticalBoxSlot* HeaderSlot = Parent->AddChildToVerticalBox(Header))
		HeaderSlot->SetPadding(FMargin(0.f, 10.f, 0.f, 3.f));
}

void US1TestModeWidget::BuildRow(UVerticalBox* Parent, ERow Row, const TCHAR* LabelKey)
{
	UHorizontalBox* Line = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
	if (UVerticalBoxSlot* LineSlot = Parent->AddChildToVerticalBox(Line))
		LineSlot->SetPadding(FMargin(0.f, 1.f));

	USizeBox* LabelBox = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
	LabelBox->SetWidthOverride(TestModeLabelWidth);
	UTextBlock* Label = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
	Label->SetFont(Font(FontSmall));
	Label->SetColorAndOpacity(FSlateColor(TextSecondary()));
	Label->SetText(Text(LabelKey));
	LabelBox->SetContent(Label);
	if (UHorizontalBoxSlot* LabelSlot = Line->AddChildToHorizontalBox(LabelBox))
		LabelSlot->SetVerticalAlignment(VAlign_Center);

	auto MakeArrow = [this, Line](const TCHAR* Glyph)
		{
			UBorder* Arrow = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass());
			Arrow->SetPadding(FMargin(7.f, 2.f));
			Arrow->SetBrushColor(CellHover());
			UTextBlock* GlyphText = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
			GlyphText->SetFont(Font(FontBody));
			GlyphText->SetColorAndOpacity(FSlateColor(TextPrimary()));
			GlyphText->SetText(FText::AsCultureInvariant(Glyph));
			Arrow->SetContent(GlyphText);
			Line->AddChildToHorizontalBox(Arrow);
			return Arrow;
		};

	UBorder* Prev = MakeArrow(TEXT("<"));

	UTextBlock* Value = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
	Value->SetFont(Font(FontSmall));
	Value->SetColorAndOpacity(FSlateColor(TextPrimary()));
	Value->SetJustification(ETextJustify::Center);
	if (UHorizontalBoxSlot* ValueSlot = Line->AddChildToHorizontalBox(Value))
	{
		ValueSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		ValueSlot->SetVerticalAlignment(VAlign_Center);
		ValueSlot->SetPadding(FMargin(4.f, 0.f));
	}

	UBorder* Next = MakeArrow(TEXT(">"));

	const int32 Index = static_cast<int32>(Row);
	PrevButtons[Index] = Prev;
	NextButtons[Index] = Next;
	Values[Index] = Value;
}

UBorder* US1TestModeWidget::MakeButton(UHorizontalBox* Row, EButton Button, const TCHAR* Key)
{
	UBorder* Border = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass());
	Border->SetPadding(FMargin(8.f, 5.f));
	Border->SetHorizontalAlignment(HAlign_Center);
	Border->SetBrushColor(CellHover());

	UTextBlock* Label = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
	Label->SetFont(Font(FontSmall));
	Label->SetColorAndOpacity(FSlateColor(TextPrimary()));
	Label->SetText(Text(Key));
	Border->SetContent(Label);

	if (UHorizontalBoxSlot* ButtonSlot = Row->AddChildToHorizontalBox(Border))
	{
		ButtonSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		ButtonSlot->SetPadding(FMargin(0.f, 0.f, 4.f, 0.f));
	}

	Buttons[static_cast<int32>(Button)] = Border;
	ButtonTexts[static_cast<int32>(Button)] = Label;
	return Border;
}

void US1TestModeWidget::NativeOnInitialized()
{
	Super::NativeOnInitialized();

	const int32 RowCount = static_cast<int32>(ERow::Count);
	const int32 ButtonCount = static_cast<int32>(EButton::Count);
	PrevButtons.SetNum(RowCount);
	NextButtons.SetNum(RowCount);
	Values.SetNum(RowCount);
	Buttons.SetNum(ButtonCount);
	ButtonTexts.SetNum(ButtonCount);

	// ── Full-screen root that lets clicks through - only the panel itself catches the mouse ──
	UBorder* Root = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass());
	Root->SetBrushColor(FLinearColor::Transparent);
	Root->SetHorizontalAlignment(HAlign_Left);
	Root->SetVerticalAlignment(VAlign_Center);
	Root->SetPadding(FMargin(16.f, 0.f));
	Root->SetVisibility(ESlateVisibility::SelfHitTestInvisible);
	WidgetTree->RootWidget = Root;

	USizeBox* Size = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
	Size->SetWidthOverride(TestModePanelWidth);
	Root->SetContent(Size);

	UBorder* Panel = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass());
	FLinearColor Background = PanelBackground();
	Background.A = 0.92f;
	Panel->SetBrushColor(Background);
	Panel->SetPadding(FMargin(PanelPadding));
	Size->SetContent(Panel);

	UVerticalBox* Column = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
	Panel->SetContent(Column);

	UTextBlock* Title = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
	Title->SetFont(Font(FontEmphasis + 2));
	Title->SetColorAndOpacity(FSlateColor(TextPrimary()));
	Title->SetText(Text(TEXT("test.title")));
	Column->AddChildToVerticalBox(Title);

	UTextBlock* Hint = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
	Hint->SetFont(Font(FontSmall));
	Hint->SetColorAndOpacity(FSlateColor(TextTertiary()));
	Hint->SetText(Text(TEXT("test.hint")));
	Column->AddChildToVerticalBox(Hint);

	// ── 장비 ──
	AddSection(Column, TEXT("test.section.gear"));
	BuildRow(Column, ERow::Weapon, TEXT("test.weapon"));
	BuildRow(Column, ERow::Shift, TEXT("test.skill.shift"));
	BuildRow(Column, ERow::Q, TEXT("test.skill.q"));
	BuildRow(Column, ERow::HelmetType, TEXT("test.helmet"));
	BuildRow(Column, ERow::HelmetSkill, TEXT("test.helmet.skill"));
	BuildRow(Column, ERow::ArmorType, TEXT("test.armor"));
	BuildRow(Column, ERow::ArmorSkill, TEXT("test.armor.skill"));
	BuildRow(Column, ERow::BootsType, TEXT("test.boots"));
	BuildRow(Column, ERow::BootsSkill, TEXT("test.boots.skill"));
	BuildRow(Column, ERow::TrinketType, TEXT("test.trinket"));
	BuildRow(Column, ERow::TrinketSkill, TEXT("test.trinket.skill"));
	BuildRow(Column, ERow::Grade, TEXT("test.grade"));
	BuildRow(Column, ERow::Level, TEXT("test.level"));

	UHorizontalBox* GearButtons = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
	if (UVerticalBoxSlot* RowSlot = Column->AddChildToVerticalBox(GearButtons))
		RowSlot->SetPadding(FMargin(0.f, 6.f, 0.f, 0.f));
	MakeButton(GearButtons, EButton::Apply, TEXT("test.apply"));

	// ── 더미 적 ──
	AddSection(Column, TEXT("test.section.dummy"));
	BuildRow(Column, ERow::DummyType, TEXT("test.dummy.type"));
	BuildRow(Column, ERow::DummyCount, TEXT("test.dummy.count"));
	BuildRow(Column, ERow::DummyMode, TEXT("test.dummy.mode"));

	UHorizontalBox* DummyButtons = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
	if (UVerticalBoxSlot* RowSlot = Column->AddChildToVerticalBox(DummyButtons))
		RowSlot->SetPadding(FMargin(0.f, 6.f, 0.f, 0.f));
	MakeButton(DummyButtons, EButton::Spawn, TEXT("test.dummy.spawn"));
	MakeButton(DummyButtons, EButton::Clear, TEXT("test.dummy.clear"));

	UHorizontalBox* ModeButtons = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
	if (UVerticalBoxSlot* RowSlot = Column->AddChildToVerticalBox(ModeButtons))
		RowSlot->SetPadding(FMargin(0.f, 4.f, 0.f, 0.f));
	MakeButton(ModeButtons, EButton::AllFixed, TEXT("test.dummy.allfixed"));
	MakeButton(ModeButtons, EButton::AllAi, TEXT("test.dummy.allai"));

	// ── 기타 ──
	AddSection(Column, TEXT("test.section.misc"));
	UHorizontalBox* MiscButtons = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
	Column->AddChildToVerticalBox(MiscButtons);
	MakeButton(MiscButtons, EButton::Heal, TEXT("test.heal"));
	MakeButton(MiscButtons, EButton::Cooldown, TEXT("test.cooldown"));

	UHorizontalBox* ToggleButtons = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
	if (UVerticalBoxSlot* RowSlot = Column->AddChildToVerticalBox(ToggleButtons))
		RowSlot->SetPadding(FMargin(0.f, 4.f, 0.f, 0.f));
	MakeButton(ToggleButtons, EButton::God, TEXT("test.god.off"));
	MakeButton(ToggleButtons, EButton::Fow, TEXT("test.fow.on"));

	// ── Message line - height kept when empty ──
	USizeBox* StatusBox = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
	StatusBox->SetHeightOverride(NoticeLineHeight * 2.f + 4.f);
	StatusLine = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
	StatusLine->SetFont(Font(FontSmall));
	StatusLine->SetColorAndOpacity(FSlateColor(TextSecondary()));
	StatusLine->SetAutoWrapText(true);
	StatusBox->SetContent(StatusLine);
	if (UVerticalBoxSlot* StatusSlot = Column->AddChildToVerticalBox(StatusBox))
		StatusSlot->SetPadding(FMargin(0.f, 8.f, 0.f, 0.f));

	LoadLists();
	SyncFromEquipment();
}

void US1TestModeWidget::NativeConstruct()
{
	Super::NativeConstruct();

	if (US1ItemSubsystem* Items = US1ItemSubsystem::Get())
		KeysChangedHandle = Items->OnKeysChanged.AddUObject(this, &US1TestModeWidget::HandleKeysChanged);
}

void US1TestModeWidget::NativeDestruct()
{
	if (US1ItemSubsystem* Items = US1ItemSubsystem::Get())
		Items->OnKeysChanged.Remove(KeysChangedHandle);

	Super::NativeDestruct();
}

// ── State ─────────────────────────────────────────────────────────────────

void US1TestModeWidget::SyncFromEquipment()
{
	const US1ItemSubsystem* Items = US1ItemSubsystem::Get();

	// The skill on each key comes from S_EQUIP_SYNC (the six keys). ItemInstance.skills carries no key - just primary, secondary
	TMap<int32, uint32> Keys;
	if (Items != nullptr)
	{
		for (const FS1KeySlotView& Key : Items->GetKeySlots())
			Keys.Add(Key.KeySlot, Key.SkillId);
	}
	auto SkillOn = [&Keys](const FS1ItemView* Item, int32 KeySlot) -> uint32
		{
			const uint32* Found = Item != nullptr ? Keys.Find(KeySlot) : nullptr;
			return Found != nullptr ? *Found : 0u;
		};

	const FS1ItemView* Weapon = Items != nullptr ? Items->GetEquipped(1) : nullptr;
	WeaponIndex = Weapon != nullptr ? FMath::Max(0, WeaponTypes.IndexOfByKey(Weapon->ItemTypeId)) : 0;
	WeaponSkills[0] = SkillOn(Weapon, 1);
	WeaponSkills[1] = SkillOn(Weapon, 2);
	if (Weapon == nullptr)
	{
		// Nothing worn - the first two skills of the first weapon, so 적용 does something useful
		const TArray<uint32> Options = SkillOptions(CurrentWeaponType());
		WeaponSkills[0] = Options.IsValidIndex(1) ? Options[1] : 0;
		WeaponSkills[1] = Options.IsValidIndex(2) ? Options[2] : 0;
	}

	for (int32 Part = 0; Part < 4; Part++)
	{
		const FS1ItemView* Worn = Items != nullptr ? Items->GetEquipped(Part + 3) : nullptr;
		PartIndex[Part] = Worn != nullptr ? FMath::Max(0, PartTypes[Part].IndexOfByKey(Worn->ItemTypeId)) : 0;
		PartSkills[Part] = SkillOn(Worn, Part + 3);
	}

	Grade = (Weapon != nullptr && Weapon->Grade != ES1ItemGrade::None) ? static_cast<int32>(Weapon->Grade) : 1;
	Level = Weapon != nullptr ? static_cast<int32>(Weapon->Level) : 0;

	Refresh();
}

void US1TestModeWidget::Step(ERow Row, int32 Delta)
{
	switch (Row)
	{
	case ERow::Weapon:
	{
		WeaponIndex = TestModeWrap(WeaponIndex + Delta, WeaponTypes.Num());
		// A new weapon has a new pool - keep the picks that are still in it, else its first two
		const TArray<uint32> Options = SkillOptions(CurrentWeaponType());
		for (int32 Key = 0; Key < 2; Key++)
		{
			if (Options.Contains(WeaponSkills[Key]) == false || WeaponSkills[Key] == 0)
				WeaponSkills[Key] = Options.IsValidIndex(Key + 1) ? Options[Key + 1] : 0;
		}
		break;
	}
	case ERow::Shift:
		WeaponSkills[0] = StepSkill(SkillOptions(CurrentWeaponType()), WeaponSkills[0], Delta, WeaponSkills[1]);
		break;
	case ERow::Q:
		WeaponSkills[1] = StepSkill(SkillOptions(CurrentWeaponType()), WeaponSkills[1], Delta, WeaponSkills[0]);
		break;
	case ERow::HelmetType: case ERow::ArmorType: case ERow::BootsType: case ERow::TrinketType:
	{
		const int32 Part = (static_cast<int32>(Row) - static_cast<int32>(ERow::HelmetType)) / 2;
		PartIndex[Part] = TestModeWrap(PartIndex[Part] + Delta, PartTypes[Part].Num());
		if (SkillOptions(CurrentPartType(Part)).Contains(PartSkills[Part]) == false)
			PartSkills[Part] = 0;
		break;
	}
	case ERow::HelmetSkill: case ERow::ArmorSkill: case ERow::BootsSkill: case ERow::TrinketSkill:
	{
		const int32 Part = (static_cast<int32>(Row) - static_cast<int32>(ERow::HelmetSkill)) / 2;
		// "그대로" has no pool - pick a type first. With nothing worn the first real type is chosen for you
		if (CurrentPartType(Part) == 0)
			PartIndex[Part] = FMath::Min(1, PartTypes[Part].Num() - 1);
		PartSkills[Part] = StepSkill(SkillOptions(CurrentPartType(Part)), PartSkills[Part], Delta, 0);
		break;
	}
	case ERow::Grade:
		Grade = 1 + TestModeWrap(Grade - 1 + Delta, 4);
		break;
	case ERow::Level:
		Level = TestModeWrap(Level + Delta, TestModeLevelMax + 1);
		break;
	case ERow::DummyType:
		DummyTypeIndex = TestModeWrap(DummyTypeIndex + Delta, MonsterTypes.Num());
		break;
	case ERow::DummyCount:
		DummyCount = 1 + TestModeWrap(DummyCount - 1 + Delta, TestModeDummyMax);
		break;
	case ERow::DummyMode:
		bDummyAi = !bDummyAi;
		break;
	default:
		break;
	}

	Refresh();
}

FText US1TestModeWidget::SkillLabel(uint32 SkillId, bool bCheckWeapon) const
{
	if (SkillId == 0)
		return Text(TEXT("test.none"));

	const FText Name = SkillName(SkillId);
	if (bCheckWeapon == false)
		return Name;

	// 갑옷 스킬의 "○○을 들고 있을 때만" - 지금 고른 무기와 다르면 붙여 보여 준다 (서버가 거부한다)
	const FName* Needs = RequiresWeapon.Find(SkillId);
	const US1ItemSubsystem* Items = US1ItemSubsystem::Get();
	if (Needs == nullptr || Items == nullptr)
		return Name;

	const FS1ItemTypeDef* Weapon = Items->GetCatalog().Find(CurrentWeaponType());
	if (Weapon != nullptr && Weapon->SkillPoolId == *Needs)
		return Name;

	FText WeaponName = FText::FromName(*Needs);
	for (uint32 TypeId : WeaponTypes)
	{
		const FS1ItemTypeDef* Type = Items->GetCatalog().Find(TypeId);
		if (Type != nullptr && Type->SkillPoolId == *Needs)
		{
			WeaponName = ItemName(Type, TypeId);
			break;
		}
	}
	return FText::Format(FText::FromString(TEXT("{0} ({1})")), Name, FText::Format(Text(TEXT("test.needs")), WeaponName));
}

void US1TestModeWidget::Refresh()
{
	const US1ItemSubsystem* Items = US1ItemSubsystem::Get();
	auto Set = [this](ERow Row, const FText& Value)
		{
			if (UTextBlock* Block = Values[static_cast<int32>(Row)])
				Block->SetText(Value);
		};
	auto TypeName = [Items](uint32 TypeId) -> FText
		{
			if (TypeId == 0)
				return Text(TEXT("test.keep"));
			return ItemName(Items != nullptr ? Items->GetCatalog().Find(TypeId) : nullptr, TypeId);
		};

	Set(ERow::Weapon, TypeName(CurrentWeaponType()));
	Set(ERow::Shift, SkillLabel(WeaponSkills[0], false));
	Set(ERow::Q, SkillLabel(WeaponSkills[1], false));

	const ERow TypeRows[4] = { ERow::HelmetType, ERow::ArmorType, ERow::BootsType, ERow::TrinketType };
	const ERow SkillRows[4] = { ERow::HelmetSkill, ERow::ArmorSkill, ERow::BootsSkill, ERow::TrinketSkill };
	for (int32 Part = 0; Part < 4; Part++)
	{
		Set(TypeRows[Part], TypeName(CurrentPartType(Part)));
		Set(SkillRows[Part], CurrentPartType(Part) == 0 ? Text(TEXT("test.keep")) : SkillLabel(PartSkills[Part], true));
	}

	Set(ERow::Grade, GradeLabel(static_cast<ES1ItemGrade>(Grade)));
	Set(ERow::Level, FText::AsCultureInvariant(FString::Printf(TEXT("+%d"), Level)));

	if (MonsterTypes.IsValidIndex(DummyTypeIndex))
	{
		const FS1MonsterTypeDef* Monster = FS1MonsterCatalog::Get().Find(MonsterTypes[DummyTypeIndex]);
		Set(ERow::DummyType, Monster != nullptr
			? FText::Format(FText::FromString(TEXT("{0} (HP {1})")), Text(*Monster->NameKey), FText::AsNumber(Monster->MaxHp))
			: FText::AsNumber(MonsterTypes[DummyTypeIndex]));
	}
	Set(ERow::DummyCount, FText::AsNumber(DummyCount));
	Set(ERow::DummyMode, Text(bDummyAi ? TEXT("test.dummy.ai") : TEXT("test.dummy.fixed")));

	if (UTextBlock* GodText = ButtonTexts[static_cast<int32>(EButton::God)])
		GodText->SetText(Text(bGod ? TEXT("test.god.on") : TEXT("test.god.off")));
	if (UBorder* GodButton = Buttons[static_cast<int32>(EButton::God)])
		GodButton->SetBrushColor(bGod ? GradeColor(ES1ItemGrade::Legendary) * 0.6f : CellHover());

	// FoW 는 켜진 것이 기본 - 끈 상태를 눈에 띄게
	if (UTextBlock* FowText = ButtonTexts[static_cast<int32>(EButton::Fow)])
		FowText->SetText(Text(bFowOn ? TEXT("test.fow.on") : TEXT("test.fow.off")));
	if (UBorder* FowButton = Buttons[static_cast<int32>(EButton::Fow)])
		FowButton->SetBrushColor(bFowOn ? CellHover() : GradeColor(ES1ItemGrade::Legendary) * 0.6f);

	if (StatusLine != nullptr)
		StatusLine->SetText(Status);
}

// ── Commands ──────────────────────────────────────────────────────────────

void US1TestModeWidget::SendCommand(const FString& Command)
{
	Protocol::C_CHAT Pkt;
	Pkt.set_msg(TCHAR_TO_UTF8(*Command));
	SEND_PACKET(Pkt);
	UE_LOG(LogTemp, Log, TEXT("[TESTMODE] %s"), *Command);
}

void US1TestModeWidget::SetStatus(const TCHAR* Key)
{
	Status = Text(Key);
	Refresh();
}

void US1TestModeWidget::Press(EButton Button)
{
	switch (Button)
	{
	case EButton::Apply:
	{
		FString Command = FString::Printf(TEXT("/t gear %u %u %u"), CurrentWeaponType(), WeaponSkills[0], WeaponSkills[1]);
		for (int32 Part = 0; Part < 4; Part++)
			Command += FString::Printf(TEXT(" %u %u"), CurrentPartType(Part), CurrentPartType(Part) != 0 ? PartSkills[Part] : 0u);
		Command += FString::Printf(TEXT(" %d %d"), Grade, Level);
		SendCommand(Command);

		// The answer is S_EQUIP_SYNC (HandleKeysChanged). Nothing within the window = the server is not in test mode
		bAwaitingGear = true;
		AwaitUntil = GetWorld() != nullptr ? GetWorld()->GetTimeSeconds() + TestModeAwaitSeconds : 0.f;
		SetStatus(TEXT("test.status.sent"));

		// Our own predicted cooldowns go too - the server zeroes the keys' cooldowns with new gear
		if (AS1MyPlayer* Me = Cast<AS1MyPlayer>(GetOwningPlayerPawn()))
			Me->ResetAllSlotCooldowns();
		return;
	}
	case EButton::Spawn:
		// 0 0 = the server puts them 4 m ahead of us, where we face
		SendCommand(FString::Printf(TEXT("/t spawn %u %d %d 0 0"), MonsterTypes.IsValidIndex(DummyTypeIndex) ? MonsterTypes[DummyTypeIndex] : 0u, DummyCount, bDummyAi ? 1 : 0));
		SetStatus(TEXT("test.status.sent"));
		return;
	case EButton::Clear:
		SendCommand(TEXT("/t clear"));
		SetStatus(TEXT("test.status.sent"));
		return;
	case EButton::AllFixed:
	case EButton::AllAi:
		bDummyAi = (Button == EButton::AllAi);
		SendCommand(FString::Printf(TEXT("/t mode %d"), bDummyAi ? 1 : 0));
		SetStatus(TEXT("test.status.sent"));
		return;
	case EButton::Heal:
		SendCommand(TEXT("/t heal"));
		SetStatus(TEXT("test.status.sent"));
		return;
	case EButton::Cooldown:
		SendCommand(TEXT("/t cd"));
		if (AS1MyPlayer* Me = Cast<AS1MyPlayer>(GetOwningPlayerPawn()))
			Me->ResetAllSlotCooldowns();
		SetStatus(TEXT("test.status.sent"));
		return;
	case EButton::God:
		bGod = !bGod;
		SendCommand(FString::Printf(TEXT("/t god %d"), bGod ? 1 : 0));
		SetStatus(TEXT("test.status.sent"));
		return;
	case EButton::Fow:
		// The answer is S_VISION (radius 0 = no fog) and the S_SPAWN of everything that comes into view
		bFowOn = !bFowOn;
		SendCommand(FString::Printf(TEXT("/t fow %d"), bFowOn ? 1 : 0));
		SetStatus(TEXT("test.status.sent"));
		return;
	default:
		return;
	}
}

void US1TestModeWidget::SpawnDummiesAt(const FVector& Where)
{
	SendCommand(FString::Printf(TEXT("/t spawn %u %d %d %.0f %.0f"), MonsterTypes.IsValidIndex(DummyTypeIndex) ? MonsterTypes[DummyTypeIndex] : 0u,
		DummyCount, bDummyAi ? 1 : 0, Where.X, Where.Y));
	SetStatus(TEXT("test.status.sent"));
}

void US1TestModeWidget::HandleKeysChanged()
{
	if (bAwaitingGear == false)
		return;

	// No SyncFromEquipment here - S_EQUIP_SYNC comes before S_INVENTORY_SYNC, so the items still read as the old gear
	// (found 2026-10-11: the weapon row jumped back to 검 after 활 was applied). The server applies all or nothing,
	// so the rows already show what it now holds; the next F7 reads the gear again
	bAwaitingGear = false;
	SetStatus(TEXT("test.status.applied"));
}

void US1TestModeWidget::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);

	if (bAwaitingGear && GetWorld() != nullptr && GetWorld()->GetTimeSeconds() > AwaitUntil)
	{
		bAwaitingGear = false;
		SetStatus(TEXT("test.status.noreply"));
	}
}

FReply US1TestModeWidget::NativeOnMouseButtonDown(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent)
{
	if (InMouseEvent.GetEffectingButton() != EKeys::LeftMouseButton)
		return Super::NativeOnMouseButtonDown(InGeometry, InMouseEvent);

	const FVector2D At = InMouseEvent.GetScreenSpacePosition();
	auto IsUnder = [&At](const UWidget* Widget)
		{
			return Widget != nullptr && Widget->IsVisible() && Widget->GetCachedGeometry().IsUnderLocation(At);
		};

	for (int32 Row = 0; Row < static_cast<int32>(ERow::Count); Row++)
	{
		if (IsUnder(PrevButtons[Row]))
		{
			Step(static_cast<ERow>(Row), -1);
			return FReply::Handled();
		}
		if (IsUnder(NextButtons[Row]))
		{
			Step(static_cast<ERow>(Row), +1);
			return FReply::Handled();
		}
	}

	for (int32 Button = 0; Button < static_cast<int32>(EButton::Count); Button++)
	{
		if (IsUnder(Buttons[Button]))
		{
			Press(static_cast<EButton>(Button));
			return FReply::Handled();
		}
	}

	// A click on the panel's background is still ours - it must not fall through to an attack or a skill confirm
	return FReply::Handled();
}
