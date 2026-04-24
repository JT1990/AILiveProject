using UnrealBuildTool;
using System.Collections.Generic;

public class AILiveProjectEditorTarget : TargetRules
{
	public AILiveProjectEditorTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Editor;
		DefaultBuildSettings = BuildSettingsVersion.V6;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		ExtraModuleNames.Add("AILiveProject");
	}
}
