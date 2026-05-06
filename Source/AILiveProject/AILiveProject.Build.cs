// =============================================================================
// 中文教学：AILiveProject.Build.cs —— UBT「模块」构建脚本
//
// .Target.cs vs .Build.cs 怎么分工：
//   - .Target.cs 描述「我要打的 target 是什么样（Game / Editor / Server…）」
//   - .Build.cs 描述「这个模块（一组 .h/.cpp）依赖哪些其它模块、怎么编译」
//   一个 target 可以由多个模块组成，本项目 ExtraModuleNames 里就只挂了一个
//   "AILiveProject" 模块，对应的就是当前这个文件。
//
// 关键概念：
//   - ModuleRules: UBT 提供的基类，所有 .Build.cs 都继承它
//   - PCHUsageMode: 预编译头（Pre-Compiled Header）策略，提速编译
//   - PublicDependencyModuleNames: 「公开依赖」—— 头文件中也会用到的模块。
//     依赖本模块的其它模块会跟着拿到这些依赖（传递性）。
//   - PrivateDependencyModuleNames: 「私有依赖」—— 仅 .cpp 实现里需要、
//     不出现在 Public/ 头文件中的模块。不会传递给上层。
//   - 选 Public 还是 Private 的判断方法：如果你 Public/*.h 里 #include 了某模块的
//     头，就放 Public；如果只在 Private/*.cpp 里 include，就放 Private。
//     原则：尽量放 Private 减少耦合，本项目目前几乎都放 Public 是因为很多
//     共享类型在 Public/Memory/、Public/LLM/ 头中直接暴露给蓝图/外部调用。
//
// 改 .Build.cs 是大事 —— CLAUDE.md 注明：改 .Build.cs / .Target.cs / .uproject
// 才需要全量重建，否则 .cpp/.h 改动走 Live Coding 即可。
// =============================================================================

using UnrealBuildTool;

// 类名必须与本目录名 AILiveProject 一致（UBT 通过这条约定找到模块）
public class AILiveProject : ModuleRules
{
	public AILiveProject(ReadOnlyTargetRules Target) : base(Target)
	{
		// 用「显式或共享 PCH」模式：每个 .cpp 顶部要 #include 自己的主头文件，
		// UBT 才能把它纳入 PCH 优化。这是 UE 5.x 的推荐模式（最快编译）。
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		// ── 公开依赖（Public/*.h 里 include 到的都要列在这里）──────────────────
		PublicDependencyModuleNames.AddRange(new string[]
		{
			// 引擎基础三件套，几乎所有模块都要
			"Core",            // FString / TArray / TMap / FCriticalSection 等基础类型
			"CoreUObject",     // UObject / UClass / UPROPERTY 反射运行时
			"Engine",          // AActor / UActorComponent / UWorld / GameInstance 等游戏对象

			"InputCore",       // 输入相关（FKey / EKeys::）

			// 网络与序列化
			"HTTP",            // FHttpModule / IHttpRequest，用于调用 OpenAI / MiniMax HTTP API
			"Json",             // FJsonObject / FJsonSerializer 解析/序列化 JSON
			"JsonUtilities",   // USTRUCT 与 JSON 字符串互转的便利封装

			// NVIDIA Audio2Face-3D 集成（A2F：把音频驱动成嘴型动画曲线）
			"ACERuntime",      // ACE 运行时核心（FACERuntimeModule、AnimateFromAudioSamples）
			"ACECore",         // ACE 通用类型（音频流、provider）

			// AI 与导航
			"AIModule",          // AIController / UAIPerceptionComponent / 感知刺激
			"NavigationSystem",  // 导航网格 / NavMover 寻路
			"SmartObjectsModule",// SmartObject 智能对象框架（NPC 与场景物件交互）

			// 元数据
			"GameplayTags",    // FGameplayTag / FGameplayTagContainer，事件与行为打标签

			// 媒体（Visual override 时可能用到的视频/图片资源管线）
			"MediaAssets",
			"MediaPlate",

			// 持久化与加密
			"SQLiteCore",      // FSQLiteDatabase，AILiveEventStoreSubsystem 用它存事件流
			"OpenSSL",         // SHA256 计算（AILiveSha256.cpp 直接用 <openssl/sha.h>）
		});

		// ── 私有依赖（仅 Private/*.cpp 内 include 的）─────────────────────
		// 当前为空，所有依赖都是 Public。后续如果新增「仅实现需要、不暴露给
		// 外部模块」的依赖（例如某个仅本地用的工具模块），加在这里。
		PrivateDependencyModuleNames.AddRange(new string[]
		{
		});
	}
}
