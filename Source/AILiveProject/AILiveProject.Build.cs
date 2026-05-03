using UnrealBuildTool;

public class AILiveProject : ModuleRules
{
	public AILiveProject(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"InputCore",
			"HTTP",
			"Json",
			"JsonUtilities",
			"ACERuntime",
			"ACECore",
			"AIModule",
			"NavigationSystem",
			"SmartObjectsModule",
			"GameplayTags",
			"MediaAssets",
			"MediaPlate",
			"SQLiteCore",
			"OpenSSL",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
		});
	}
}
