#pragma once

// =============================================================================
// 中文教学：AILiveProject.h —— 模块入口
//
// 这是什么：
//   每个 UE Module 都需要一个「模块入口类」，继承 IModuleInterface。UE 启动加载
//   模块时会创建实例并调 StartupModule；卸载时调 ShutdownModule。
//   AILive 模块在 Startup 时只是打一行日志，没干别的；Shutdown 啥都不做。
//
// 与 Build.cs 的关系：
//   - Build.cs：告诉 UBT「这个模块怎么编译，依赖谁」
//   - AILiveProject.h/.cpp：告诉 UE 运行时「模块加载/卸载的钩子」
//   - .uproject 里的 Modules 数组：声明项目里有这些模块
//   三者协同。
//
// IModuleInterface 还有其它生命周期点：
//   - PreUnloadCallback / PostLoadCallback：更精细的时序控制
//   - SupportsDynamicReloading：是否支持热重载（Live Coding 默认 true）
//   本模块不需要重写这些，沿用默认。
// =============================================================================

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"      // IModuleInterface

class FAILiveProjectModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
};
