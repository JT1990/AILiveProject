// =============================================================================
// 中文教学：AILiveProject.cpp —— 模块入口实现
//
// 关键宏：
//   IMPLEMENT_PRIMARY_GAME_MODULE(ModuleClass, ModuleName, GameName)
//     ↑↑↑ 整个项目最重要的一行宏，必须出现且只出现一次。
//   作用：
//     1) 实例化模块对象（FAILiveProjectModule）
//     2) 注册到 UE 模块管理器（FModuleManager）
//     3) 标识本模块是「主游戏模块」（Primary）—— 项目入口模块
//   每个独立 Game Module 用 IMPLEMENT_GAME_MODULE；只有项目主模块用 IMPLEMENT_PRIMARY_GAME_MODULE。
//
//   DEFINE_LOG_CATEGORY_STATIC(LogAILiveProject, Log, All)
//     声明一个仅本 .cpp 可见的日志类别。如果想跨文件共用，要改用
//     DECLARE_LOG_CATEGORY_EXTERN（头）+ DEFINE_LOG_CATEGORY（cpp）。
//     这里仅本文件用，所以选 STATIC 版。
// =============================================================================

#include "AILiveProject.h"

DEFINE_LOG_CATEGORY_STATIC(LogAILiveProject, Log, All);

void FAILiveProjectModule::StartupModule()
{
	// 模块加载完成。这里可以做：注册 Subsystem hooks、加载配置、初始化全局状态等。
	// 当前仅打一条 Log 表示模块上线。
	UE_LOG(LogAILiveProject, Log, TEXT("AILiveProject module started"));
}

void FAILiveProjectModule::ShutdownModule()
{
	// 模块卸载。清理 StartupModule 里申请的资源。当前 Startup 没分配，所以空。
}

// 项目主模块入口宏。第三个参数 "AILiveProject" 必须与 .uproject 里的模块名一致。
IMPLEMENT_PRIMARY_GAME_MODULE(FAILiveProjectModule, AILiveProject, "AILiveProject");
