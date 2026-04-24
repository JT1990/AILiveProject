using UnrealBuildTool;
using System.Collections.Generic;

public class AILiveProjectTarget : TargetRules
{
	public AILiveProjectTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Game;
		DefaultBuildSettings = BuildSettingsVersion.V6;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		ExtraModuleNames.Add("AILiveProject");
	}
}
