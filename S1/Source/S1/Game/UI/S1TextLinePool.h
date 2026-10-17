// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/WidgetTree.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"

/**
 * Reuses the text lines of a vertical box that is rebuilt on every refresh (TD-010, 2026-10-18).
 *
 * The item detail and the base-camp skill / stat summaries used to ClearChildren() and construct a new UTextBlock per
 * line on every hover or change. Begin() collapses every line; Next() rewrites the next existing line in place (or makes
 * one when the box grows) and shows it again. Lines past the last Next() stay collapsed for the next refresh.
 * Visibility, wrap and padding are reset on reuse; the caller sets text, font and colour every time as before.
 *
 * 🔴 The box must hold only lines made by this pool.
 */
struct FS1TextLinePool
{
	void Begin(UVerticalBox* Box)
	{
		Used = 0;
		if (Box == nullptr)
			return;
		for (int32 i = 0; i < Box->GetChildrenCount(); i++)
		{
			if (UWidget* Child = Box->GetChildAt(i))
				Child->SetVisibility(ESlateVisibility::Collapsed);
		}
	}

	UTextBlock* Next(UWidgetTree* Tree, UVerticalBox* Box, const FMargin& Padding = FMargin(0.f))
	{
		UTextBlock* Line = (Used < Box->GetChildrenCount()) ? Cast<UTextBlock>(Box->GetChildAt(Used)) : nullptr;
		if (Line == nullptr)
		{
			Line = Tree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
			Box->AddChildToVerticalBox(Line);
		}
		// What a freshly constructed line would have - the class default, not a guess
		Line->SetVisibility(GetDefault<UTextBlock>()->GetVisibility());
		Line->SetAutoWrapText(false);
		if (UVerticalBoxSlot* LineSlot = Cast<UVerticalBoxSlot>(Line->Slot))
			LineSlot->SetPadding(Padding);
		Used++;
		return Line;
	}

	int32 Used = 0;
};
