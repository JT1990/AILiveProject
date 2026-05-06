// =============================================================================
// 中文教学：AILiveProjectEditor.Target.cs —— UBT「编辑器」目标配置
//
// 与 AILiveProject.Target.cs 的关系：
//   一个项目通常配两个 target：
//     - Game target   → 打包出玩家运行的 .exe（无编辑器 UI、无热重载、无菜单）
//     - Editor target → 跑在 UE 编辑器里、支持 PIE（Play In Editor）+ Live
//                       Coding 热重载，平时开发都用这个。
//   日常 UBT 命令（CLAUDE.md 里有原文）：
//     Build.bat AILiveProjectEditor Win64 Development -Project=...
//     ↑ 这里要的就是 *Editor* target，命中本文件。
//
// 注意点（与 Game target 唯一不同的字段）：
//   - Type = TargetType.Editor: 让 UBT 链接 UnrealEd / UMGEditor 等仅编辑器
//     用的模块；运行时游戏不会带上这些。
//   - 类名必须 <项目名>EditorTarget；构造签名同 Game target。
//   - 其它字段（BuildSettingsVersion / IncludeOrderVersion / ExtraModuleNames）
//     与 Game target 保持一致 —— 否则 Editor 跑通的代码可能在 Game 包里翻车。
// =============================================================================

using UnrealBuildTool;
using System.Collections.Generic;  // 同 Game target，模板默认引入未使用，留着不动

public class AILiveProjectEditorTarget : TargetRules
{
	public AILiveProjectEditorTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Editor;                                // 「编辑器 target」：能在 UE Editor 中加载、PIE、Live Coding
		DefaultBuildSettings = BuildSettingsVersion.V6;          // ⚠️ 与 Game target 必须一致；CLAUDE.md 红线
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;  // 头文件顺序约定，与 Game target 同步
		ExtraModuleNames.Add("AILiveProject");                   // 编辑器 target 同样需要本项目的主模块
	}
}
