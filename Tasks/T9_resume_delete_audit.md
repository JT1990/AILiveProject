# T9 — VerifyHashChain + Resume 协议 + Delete 跨库桥接 + agent_registry 同步

> 总览：[00_overview.md](00_overview.md)
> 依赖：blockedBy = [T8](T8_projector_rebuild.md)（Resume 依赖 RebuildProjections）
> 改动量：**中**（1–4h；但 L3 性能可能引发回看 T3 / T6 优化）
> §11 验收：L2「超时恢复 / Resume 一致性 / 哈希链 / DB Browser 实时只读 / Delete 协议跨库桥接 / agent_registry 同步」、L3「性能 / 并发」

## 预检

本步纯 C++ + 跨库 SQLite，**不需要** Monolith MCP；Resume 用例需要先跑一段 ACT02 再 kill UE，重启后用 Bash + DB Browser 验证。

## 目标

闭环审计与跨局生命周期 —— 哈希链可验证、UE 崩溃可恢复、Delete 协议可触发并同步 _meta.db。

## 涉及文件

### 修

- `Source/AILiveProject/Public/Memory/AILiveEventStoreSubsystem.h` —— 暴露 `VerifyHashChain` / `ResumeFromGameId` / `TriggerDeleteExecuted(agentId, ...)`（声明在 T2 已写）
- `Source/AILiveProject/Private/Memory/AILiveEventStoreSubsystem.cpp` —— 实现：
  - `VerifyHashChain(OutFirstBrokenSeq)` —— 顺序扫 events 重算 hash 链
  - `ResumeFromGameId(InGameId)` —— BeginGame 复用 + 扫 SystemLLMInflight 找未配对的 → 按 wall_clock 分类（< 60s 新鲜 / ≥ 60s 过期）→ MVP 统一写 SystemAgentTimeout（payload 含原 request_id + npc_index + age_seconds）→ 调 `RebuildProjections()` → 加载 game_state
  - `TriggerDeleteExecuted` —— 写 `_meta.db.agent_lifecycle_events` 1 行（拿到 lifecycle_event_id）→ 调 `AILiveAgentRegistry::SyncRegistryFromLifecycle(event_id)` → 在该局 .db 写 `system.delete_executed`（payload 含 lifecycle_event_id，visibility=["public"]）

### 新增

- `Source/AILiveProject/Public/Memory/AILiveAgentRegistry.h` + `.cpp` —— `namespace AILiveAgentRegistry { void SyncRegistryFromLifecycle(MetaDb*, FString lifecycleEventId); }` —— UPDATE `_meta.db.agent_registry` 的 status / deleted_at / last_updated_at；业务层不可直接 UPDATE（CI grep 强制）

## 依赖

blockedBy = [T8](T8_projector_rebuild.md)（Resume 依赖 RebuildProjections）

## 验收方式

对齐 §11 **L2 + L3**：

- L2「超时恢复」：LLM 超时 → SystemAgentTimeout 事件写入；seq 不跳号
- L2「Resume 一致性」：跑到 ACT02 中段 → kill UE → 重启调 ResumeFromGameId → game_state 与 kill 前一致；in-flight 未完成项已全部转 SystemAgentTimeout
- L2「哈希链」：手工 `UPDATE events SET payload='x' WHERE seq=N`（先 DROP append-only trigger）→ VerifyHashChain 报错并定位到 seq；重算 seq 起所有 hash 后再 verify 通过（对应 L2「append-only 与离线工具路径」）
- L2「DB Browser 实时只读」：UE 游戏运行中外部 DB Browser Read-only 模式打开同一 .db 能看到新事件刷新（验证 WAL 模式正确）
- L2「Delete 协议跨库桥接」：调 `TriggerDeleteExecuted("NPC03", ...)` → `_meta.db.agent_lifecycle_events` 写入 1 行（lifecycle_event_type='delete_executed'）+ 该局 .db 写入 1 条 `system.delete_executed`（payload.lifecycle_event_id 引用前者）+ 任一 NPC 调 `Quote(...)` 都能看到该 system 事件（visibility=public）
- L2「agent_registry 同步」：`SELECT status, deleted_at FROM agent_registry WHERE agent_id='NPC03'` 返回 `'deleted'` + 非空 ISO 8601
- L2「addressed_to ⊆ visibility」：传 `addressed_to=["NPC07"], visibility=["NPC03"]` → AppendEvent 仍写入但记 UE_LOG(Warning) + 1 条 system.parse_failed（非拒绝写入；与 schema.yaml event_addressed_to 注释对齐）
- L3「单 AppendEvent < 5ms / 单拍 prompt 拼装 < 100ms / 100 线程并发 30 秒 events 表 = 400×N」

## 风险点

- Resume 协议 MVP 选保守策略（过期 + 新鲜统一写 SystemAgentTimeout，**不**重发）—— impl §5.4 明确 MVP 范围；下一阶段引入 prompt cache 重发由独立 PR 处理
- Delete 协议在工程层只是"挂钩"—— 谁有权提议、决策机制由后续 orchestrator 在 ZombieGame 内部实现（不在本拆解）
- 跨库写：Delete 触发的两次写（_meta.db lifecycle + 单局 .db system event）**不**在同一事务内 —— 两个 SQLite connection 不支持跨库事务；通过先写 lifecycle + 拿到 event_id + 再写 system event 的顺序保证一致性；崩溃恢复时若发现 lifecycle 已写但 system 未写，Resume 时补写 system event（L2 验收覆盖此场景）
- L3 性能指标若不达标，回看：(a) 哈希链计算是否每次都 alloc 新 EVP_MD_CTX → 复用；(b) 反规范化 INSERT 是否批量 → 改 `INSERT ... VALUES (?,?), (?,?), ...`；(c) prompt 拼装是否 N+1 → 用 IN 子句一次性拉

## 里程碑 DevLog

`DevLog/2026-MM-DD_resume_delete_audit.md`，记录 Resume 协议（in-flight 过期分类）+ Delete 跨库桥接 + L3 性能基线。

**额外**：本步完成后追加一篇 `DevLog/2026-MM-DD_memory_system_e2e.md` 总结整套对抗博弈记忆系统的端到端验收（10 个 §11 验收用例的实际跑出结果）

## 完成定义

哈希链可验证 + Resume 后状态正确 + Delete 跨库桥接完整 + L3 性能达标 + 两篇里程碑 DevLog 入库。**这是整个拆解的最后一步。**
