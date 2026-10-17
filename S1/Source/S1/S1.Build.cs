// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class S1 : ModuleRules
{
	public S1(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
	
		PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine", "InputCore", "EnhancedInput", "Sockets", "Networking", "UMG", "Slate" });

		// SlateCore — the item UI draws grade outlines (FSlateDrawElement) and picks fonts (FCoreStyle) itself.
		PrivateDependencyModuleNames.AddRange(new string[] { "ProtobufCore", "SlateCore" });

		// Json — the level tools read the floor layout (tools/level/floor1.json, design/levels/floor-1.md).
		PrivateDependencyModuleNames.Add("Json");

		// PhysicsCore — the level export reads each server-wall mesh's collision trace flag (UBodySetupCore, ADR-0006 E2).
		PrivateDependencyModuleNames.Add("PhysicsCore");

		// HTTP — the login screen talks to the auth server (ADR-0011). Windows uses the Curl backend; plain http works in Shipping too.
		PrivateDependencyModuleNames.Add("HTTP");

		// ProceduralMeshComponent - skill effects build their own meshes (the upright sword wave of 1009, skill_vfx.csv, 2026-10-15).
		PrivateDependencyModuleNames.Add("ProceduralMeshComponent");

		PrivateIncludePaths.AddRange(new string[]
		{
			"S1/",
			"S1/Network/",
            "S1/Game/",
            "S1/TP_ThirdPerson/",
		});

		// Uncomment if you are using Slate UI
		// PrivateDependencyModuleNames.AddRange(new string[] { "Slate", "SlateCore" });
		
		// Uncomment if you are using online features
		// PrivateDependencyModuleNames.Add("OnlineSubsystem");

		// To include OnlineSubsystemSteam, add it to the plugins section in your uproject file with the Enabled attribute set to true
	}
}
