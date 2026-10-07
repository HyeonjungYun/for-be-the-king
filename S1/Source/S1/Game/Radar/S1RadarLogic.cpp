// Fill out your copyright notice in the Description page of Project Settings.

#include "Game/Radar/S1RadarLogic.h"

bool S1RadarLogic::IsInRadarRange(const FVector2D& Self, const FVector2D& Target, float RadiusCm)
{
	const double Dx = Target.X - Self.X;
	const double Dy = Target.Y - Self.Y;
	const double D2 = Dx * Dx + Dy * Dy;
	if (FMath::IsFinite(D2) == false)
		return false;

	return D2 <= static_cast<double>(RadiusCm) * RadiusCm;
}

FVector2D S1RadarLogic::WorldToRadarOffset(const FVector2D& Self, const FVector2D& Target, float RadiusCm, float RingRadiusPx)
{
	if (RadiusCm <= 0.f)
		return FVector2D::ZeroVector;

	const double Dx = Target.X - Self.X;		// north
	const double Dy = Target.Y - Self.Y;		// east
	return FVector2D(Dy / RadiusCm * RingRadiusPx, -Dx / RadiusCm * RingRadiusPx);
}

float S1RadarLogic::EdgeAlpha(float DistanceCm, float RadiusCm, float FadeCm)
{
	if (FadeCm <= 0.f)
		return DistanceCm <= RadiusCm ? 1.f : 0.f;

	return FMath::Clamp((RadiusCm - DistanceCm) / FadeCm, 0.f, 1.f);
}

bool S1RadarLogic::IsChestObjectId(uint64 ObjectId, uint64 Base, uint64 End)
{
	return ObjectId >= Base && ObjectId < End;
}

bool S1RadarLogic::ChestLootedFromContents(int32 PreviousCount, int32 NewCount)
{
	if (NewCount == 0)
		return true;

	return PreviousCount >= 0 && NewCount < PreviousCount;
}

TArray<FS1RadarBlip> S1RadarLogic::CollectBlips(const FS1RadarInput& Input)
{
	TArray<FS1RadarBlip> Blips;

	auto Add = [&Input, &Blips](ES1RadarBlip Kind, uint64 Id, const FVector2D& Position)
	{
		if (IsInRadarRange(Input.Self, Position, Input.RadiusCm) == false)
			return;

		FS1RadarBlip& Blip = Blips.AddDefaulted_GetRef();
		Blip.Kind = Kind;
		Blip.Id = Id;
		Blip.DistanceCm = static_cast<float>(FVector2D::Distance(Input.Self, Position));
		Blip.OffsetPx = WorldToRadarOffset(Input.Self, Position, Input.RadiusCm, Input.RingRadiusPx);
		Blip.Alpha = EdgeAlpha(Blip.DistanceCm, Input.RadiusCm, Input.FadeCm);
	};

	for (const TPair<uint32, FVector2D>& Tower : Input.Towers)
		Add(ES1RadarBlip::Tower, Tower.Key, Tower.Value);

	for (const FS1RadarChest& Chest : Input.Chests)
	{
		// Looted chests leave the radar — no hollow icon either (radar.md D5). A bag id is never a chest
		if (Chest.bKnownLooted == false && IsChestObjectId(Chest.Id))
			Add(ES1RadarBlip::Chest, Chest.Id, Chest.Position);
	}

	for (const TPair<uint64, FVector2D>& Member : Input.Party)
		Add(ES1RadarBlip::Party, Member.Key, Member.Value);

	Blips.StableSort([](const FS1RadarBlip& A, const FS1RadarBlip& B)
	{
		return A.Kind != B.Kind ? static_cast<uint8>(A.Kind) < static_cast<uint8>(B.Kind) : A.Id < B.Id;
	});
	return Blips;
}

FS1RadarLayout S1RadarLogic::ComputeLayout(float ViewportHeightPx, float RingDiameterRef, float PaddingRef, float MarginRef)
{
	FS1RadarLayout Layout;
	Layout.Scale = ViewportHeightPx / 1080.f;
	Layout.RingDiameterPx = RingDiameterRef * Layout.Scale;
	Layout.WidgetSizePx = (RingDiameterRef + 2.f * PaddingRef) * Layout.Scale;
	Layout.MarginPx = MarginRef * Layout.Scale;
	Layout.BottomPx = Layout.MarginPx + Layout.WidgetSizePx;
	Layout.bFitsFreeBand = (MarginRef + RingDiameterRef + 2.f * PaddingRef) <= 0.15f * 1080.f;
	return Layout;
}

bool S1RadarLogic::IsRadarVisible(bool bInDungeon, bool bAlive, bool bHasLocation, bool bEscaping)
{
	return bInDungeon && bAlive && bHasLocation && bEscaping == false;
}
