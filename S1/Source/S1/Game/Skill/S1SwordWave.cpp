// Fill out your copyright notice in the Description page of Project Settings.

#include "Game/Skill/S1SwordWave.h"

namespace
{
	constexpr float ECHO_ALPHA[] = { 1.f, 0.5f, 0.25f };	// the wave, then its afterimages
	constexpr float BODY_AT = 0.3f;							// the body row sits this far back across the thickness
	constexpr float BODY_ALPHA = 0.9f;						// the body carries the skill's colour
	constexpr float TRAIL_EDGE_ALPHA = 0.15f;				// the trailing (inner) edge - the blade thins out behind its body
	constexpr float CORE_WHITEN = 0.75f;					// the leading edge goes toward white
}

bool S1SwordWave::ParseLooks(const FString& Csv, TMap<uint32, FS1ProjectileLook>& OutLooks, FString& OutError)
{
	OutLooks.Reset();
	TArray<FString> Lines;
	Csv.ParseIntoArrayLines(Lines);
	if (Lines.Num() == 0)
	{
		OutError = TEXT("skill_vfx.csv is empty");
		return false;
	}

	TArray<FString> Header;
	Lines[0].TrimStartAndEnd().ParseIntoArray(Header, TEXT(","), false);
	const int32 IdCol = Header.IndexOfByKey(TEXT("skill_id"));
	const int32 KindCol = Header.IndexOfByKey(TEXT("projectile"));
	const int32 HeightCol = Header.IndexOfByKey(TEXT("height_cm"));
	const int32 DepthCol = Header.IndexOfByKey(TEXT("depth_cm"));
	const int32 ThickCol = Header.IndexOfByKey(TEXT("thickness_cm"));
	const int32 ColourCol = Header.IndexOfByKey(TEXT("colour"));
	const int32 SinkCol = Header.IndexOfByKey(TEXT("sink_pct"));		// optional
	if (IdCol < 0 || KindCol < 0 || HeightCol < 0 || DepthCol < 0 || ThickCol < 0 || ColourCol < 0)
	{
		OutError = TEXT("skill_vfx.csv needs skill_id,projectile,height_cm,depth_cm,thickness_cm,colour");
		return false;
	}

	for (int32 i = 1; i < Lines.Num(); i++)
	{
		const FString Line = Lines[i].TrimStartAndEnd();
		if (Line.IsEmpty())
			continue;
		TArray<FString> Cells;
		Line.ParseIntoArray(Cells, TEXT(","), false);
		const int32 Need = FMath::Max(FMath::Max(IdCol, KindCol), FMath::Max(FMath::Max(HeightCol, DepthCol), FMath::Max(ThickCol, ColourCol)));
		if (Cells.Num() <= Need)
		{
			OutError = FString::Printf(TEXT("skill_vfx.csv line %d: too few columns"), i + 1);
			return false;
		}

		const uint32 Id = static_cast<uint32>(FCString::Strtoui64(*Cells[IdCol], nullptr, 10));
		FS1ProjectileLook Look;
		Look.Kind = FName(*Cells[KindCol].TrimStartAndEnd());
		Look.HeightCm = FCString::Atof(*Cells[HeightCol]);
		Look.DepthCm = FCString::Atof(*Cells[DepthCol]);
		Look.ThicknessCm = FCString::Atof(*Cells[ThickCol]);
		TArray<FString> Rgb;
		Cells[ColourCol].TrimStartAndEnd().ParseIntoArray(Rgb, TEXT(" "), true);
		const bool bColour = Rgb.Num() == 3;
		if (bColour)
			Look.Colour = FLinearColor(FCString::Atof(*Rgb[0]), FCString::Atof(*Rgb[1]), FCString::Atof(*Rgb[2]));
		if (SinkCol >= 0 && Cells.IsValidIndex(SinkCol) && Cells[SinkCol].TrimStartAndEnd().IsEmpty() == false)
			Look.SinkPct = FCString::Atof(*Cells[SinkCol]);

		// The circle through the tips and the front needs a real bulge; a blade thicker than its bulge would turn inside out.
		// Sunk 90 % or more, nothing would show
		if (Id == 0 || Look.Kind != FName(TEXT("crescent_v")) || Look.HeightCm <= 0.f || Look.DepthCm <= 0.f
			|| Look.ThicknessCm <= 0.f || Look.ThicknessCm >= Look.DepthCm || bColour == false || Look.SinkPct < 0.f || Look.SinkPct > 90.f
			|| OutLooks.Contains(Id))
		{
			OutError = FString::Printf(TEXT("skill_vfx.csv line %d: skill_id > 0 (once), projectile crescent_v, 0 < thickness < depth, height > 0, colour \"r g b\", sink_pct 0..90"), i + 1);
			return false;
		}
		OutLooks.Add(Id, Look);
	}
	return true;
}

FS1SwordWaveMesh S1SwordWave::BuildCrescent(const FS1ProjectileLook& Look, int32 Segments, int32 Echoes, float EchoGapCm)
{
	FS1SwordWaveMesh Mesh;
	Segments = FMath::Max(Segments, 2);
	Echoes = FMath::Clamp(Echoes, 0, static_cast<int32>(UE_ARRAY_COUNT(ECHO_ALPHA)) - 1);

	// Circle through (0, ±h) and (D, 0): centre (c, 0), radius R = D - c, c² + h² = R²  →  c = (D² - h²) / 2D
	const float H = Look.HeightCm * 0.5f;
	const float D = Look.DepthCm;
	const float C = (D * D - H * H) / (2.f * D);
	const float R = D - C;
	const float MaxAngle = FMath::Atan2(H, -C);		// the tips

	const FLinearColor Core = FMath::Lerp(Look.Colour, FLinearColor::White, CORE_WHITEN);
	for (int32 Echo = 0; Echo <= Echoes; Echo++)
	{
		const float Back = EchoGapCm * Echo;
		const float Fade = ECHO_ALPHA[Echo];
		const int32 Base = Mesh.Vertices.Num();
		for (int32 s = 0; s <= Segments; s++)
		{
			const float T = static_cast<float>(s) / Segments;			// 0 bottom tip .. 1 top tip
			const float Angle = FMath::Lerp(-MaxAngle, MaxAngle, T);
			const float Taper = FMath::Cos((T - 0.5f) * PI);				// 1 in the middle, 0 at the tips
			const FVector Dir(FMath::Cos(Angle), 0.f, FMath::Sin(Angle));
			const FVector Centre(C - Back, 0.f, 0.f);
			const float Thick = Look.ThicknessCm * Taper;
			Mesh.Vertices.Add(Centre + Dir * R);							// leading edge
			Mesh.Vertices.Add(Centre + Dir * (R - Thick * BODY_AT));		// body
			Mesh.Vertices.Add(Centre + Dir * (R - Thick));					// trailing edge
			FLinearColor Lead = Core;
			Lead.A = Fade * (0.3f + 0.7f * Taper);
			FLinearColor Body = Look.Colour;
			Body.A = Fade * BODY_ALPHA * (0.2f + 0.8f * Taper);
			FLinearColor Trail = Look.Colour;
			Trail.A = Fade * TRAIL_EDGE_ALPHA * Taper;
			Mesh.Colours.Add(Lead);
			Mesh.Colours.Add(Body);
			Mesh.Colours.Add(Trail);
		}
		for (int32 s = 0; s < Segments; s++)
		{
			// Two bands per segment: lead → body, body → trail (vertex = Base + step * 3 + row)
			for (int32 Row = 0; Row < 2; Row++)
			{
				const int32 A = Base + s * 3 + Row;		// this step, row / row + 1
				const int32 B = A + 3;					// next step
				Mesh.Triangles.Append({ A, B, A + 1, A + 1, B, B + 1 });
			}
		}
	}
	return Mesh;
}
