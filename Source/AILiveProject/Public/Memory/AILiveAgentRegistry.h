#pragma once

// =============================================================================
// 中文教学：AILiveAgentRegistry.h —— Agent 全局注册表的「唯一合法写入器」
//
// 这是什么：
//   `_meta.db` 中有两张表：
//     - agent_lifecycle_events：所有跨局生命周期事件（created/deleted/archived/...）的
//       追加日志，类似当前局的 events 表
//     - agent_registry：每个 agent 的当前状态快照（status / deleted_at / last_seen_game_id）
//
//   `agent_registry` 是 lifecycle_events 的「投影」。本函数 `SyncRegistryFromLifecycle`
//   是该投影的**唯一合法写入路径** —— 业务代码 MUST NOT 直接 UPDATE agent_registry。
//   原因：每条 lifecycle 事件需要按规则映射到 status/deleted_at，否则状态会乱。
//
// 设计：
//   - 单事务（BEGIN IMMEDIATE → SELECT lifecycle_event → UPDATE agent_registry → COMMIT）
//   - 失败 ROLLBACK；agent_registry 不会半状态写入
//   - DevLog 记录了 CI grep guard：扫源码 `UPDATE agent_registry` 字符串，
//     只允许出现在本 .cpp，否则 PR 拒绝合并
//
// C++ 知识点：
//   - 前向声明 `class FSQLiteDatabase;`：本头只用 FSQLiteDatabase 引用，
//     不需要 #include 其完整定义，让头依赖更少
// =============================================================================

#include "CoreMinimal.h"

class FSQLiteDatabase;

namespace AILiveAgentRegistry
{
	/**
	 * Read the indicated row from `_meta.db.agent_lifecycle_events` and project
	 * its lifecycle_event_type onto `_meta.db.agent_registry`:
	 *   delete_executed → status='deleted', deleted_at=<wall_clock>
	 *   created / revived → status='active', deleted_at=NULL
	 *   archived → status='archived'
	 *   delete_proposed / delete_vetoed → status untouched (only last_updated_at bumped)
	 *
	 * Caller must already have MetaDb open (BeginGame opens it). This function
	 * manages its own BEGIN IMMEDIATE / COMMIT / ROLLBACK on MetaDb.
	 *
	 * Business code MUST NOT issue `UPDATE agent_registry` directly — this is
	 * the only authorized writer (CI grep guard documented in DevLog).
	 */
	AILIVEPROJECT_API bool SyncRegistryFromLifecycle(FSQLiteDatabase& MetaDb,
		const FString& LifecycleEventId);
}
