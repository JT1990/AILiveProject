// =============================================================================
// 中文教学：AILiveProject.Target.cs —— UBT「打包游戏」的入口配置
//
// 这是什么：
//   .Target.cs 文件由 UnrealBuildTool（UBT，引擎自带的构建调度器）读取，
//   用 C# 描述「我要构建什么样的目标可执行体（target）」。同一个项目可以有
//   多个 target —— 这里就是「Game target」，对应打包出独立运行的 .exe。
//   旁边的 AILiveProjectEditor.Target.cs 是「Editor target」，对应能在 UE
//   编辑器里 PIE 调试的版本。两者编译出的二进制是不同的。
//
// 为什么是 C# 不是 C++：
//   UBT 本身是 C# 写的；.Target.cs / .Build.cs 都是 C# 脚本，UBT 会动态编译
//   并执行它们来计算构建图。所以这两个文件不会进入运行时，不要把游戏逻辑
//   写在这里。
//
// 关键概念：
//   - TargetRules: UBT 提供的基类，所有 .Target.cs 都继承它
//   - TargetType.Game: 标识这是「玩家最终运行的游戏 .exe」
//   - BuildSettingsVersion.V6: 一组默认编译选项的版本号 ⚠️ 项目 CLAUDE.md
//     明确禁止降级（V6 → V5/V4 等），会破坏 Installed-Engine + Live Coding 兼容性
//   - IncludeOrderVersion.Latest: 头文件包含顺序使用最新约定（影响 PCH 生成）
//   - ExtraModuleNames: 把哪些模块（.Build.cs 定义的）打包进这个 target
// =============================================================================

using UnrealBuildTool;
using System.Collections.Generic;  // 这里其实没用到，UE 模板默认就带，删了也没事但不动它（边界规则）

// 类名约定：必须叫 <项目名>Target，且继承 TargetRules
public class AILiveProjectTarget : TargetRules
{
	// 构造函数签名固定：接收 TargetInfo（描述当前在为哪个平台/配置构建）
	public AILiveProjectTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Game;                                  // 「游戏 target」：打出独立运行的客户端 .exe
		DefaultBuildSettings = BuildSettingsVersion.V6;          // ⚠️ 不要改！CLAUDE.md 红线
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;  // 用最新的引擎头文件包含顺序约定
		ExtraModuleNames.Add("AILiveProject");                   // 把本项目的主模块（见 AILiveProject.Build.cs）加入构建
	}
}
