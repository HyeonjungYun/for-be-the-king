// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class S1 : ModuleRules
{
	public S1(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
	
		PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine", "InputCore", "EnhancedInput", "Sockets", "Networking", "UMG", "Slate" });

		PrivateDependencyModuleNames.AddRange(new string[] { "ProtobufCore" });

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
