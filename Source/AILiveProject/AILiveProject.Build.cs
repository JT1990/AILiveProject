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
			"GameplayTasks",
			"UMG",
			"Slate",
			"SlateCore",
			"SmartObjectsModule",
			"GameplayInteractionsModule",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
		});
	}
}
