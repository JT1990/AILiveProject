# T2 — Schema migration + EventStore 子系统骨架 + agent_calibration 全字段落地

> 总览：[00_overview.md](00_overview.md)
> 依赖：blockedBy = [T1](T1_memory_types_skeleton.md)
> 改动量：**中**（1–4h）
> §11 验收：L0「`.db` 存在 + 14 表 + schema_version 校验」+ 新增「`agent_calibration` 列存在性 SQL 检查」

## 预检

纯 C++ + 新建 .db 文件，不动 Blueprint/资产，**不需要** Monolith MCP；本机 SQLite 状态用 Bash + DB Browser 直接查。

## 目标

建立 `UAILiveEventStoreSubsystem` 的生命周期管理、`Saved/Games/<game_id>.db` + `Saved/Games/_meta.db` 的全部 14 表 + trigger + index + PRAGMA。**不**实现任何写入/读取业务方法，只验证 .db 文件能正确生成。**关键修订**：`agent_calibration` 表的 DDL 必须显式含 `private_goal TEXT` / `alliance_members_json TEXT NOT NULL DEFAULT '[]'` / `seq_start INTEGER NOT NULL DEFAULT 1` 列（与 schema.yaml `agent_battle_config` 段对齐，与 implementation §3.2 DDL line 384-398 对齐）。

## 涉及文件

### 新增

- `Source/AILiveProject/Public/Memory/AILiveEventStoreSubsystem.h` —— `UAILiveEventStoreSubsystem`（继承 `UGameInstanceSubsystem`）的完整声明（含写入 / 读取 / 审计接口），但本步只实现 `Initialize` / `Deinitialize` / `BeginGame` / `EndGame` / `IsGameOpen` / `GetCurrentGameId` / `ApplyPragmas` / `EnsureSchema` / `RunMigrations`
- `Source/AILiveProject/Private/Memory/AILiveEventStoreSubsystem.cpp` —— 上述方法实现 + `DEFINE_LOG_CATEGORY(LogAILiveMemory)` + `kCurrentSchemaVersion = 1` + `kGenesisHash`
- `Source/AILiveProject/Private/Memory/AILiveSchemaMigration.cpp` —— DDL 字符串常量集合（events / event_visibility / event_addressed_to / events_fts + trigger / agent_view_state / commitments / vote_history / alliance_state / event_log_summaries / bel_violations / leakage_audits / agent_calibration / game_state / schema_meta = 13 个表 + events_fts 虚表 = 14 项；`_meta.db` 的 schema_meta + agent_lifecycle_events + agent_registry + 各自 trigger）+ `ApplyMigrationV0ToV1()` 占位 + FTS5 trigram tokenizer 自检（失败降级 unicode61 + UE_LOG Warning）

## 依赖

blockedBy = [T1](T1_memory_types_skeleton.md)

## 验收方式

1. PIE 启动 → 调一次 `Store->BeginGame("test001")` → `Saved/Games/test001.db` 文件存在
2. DB Browser 打开 `test001.db`：能看到 14 个表 / 虚表（events / event_visibility / event_addressed_to / events_fts / agent_view_state / commitments / vote_history / alliance_state / event_log_summaries / bel_violations / leakage_audits / agent_calibration / game_state / schema_meta）
3. `SELECT value FROM schema_meta WHERE key='schema_version'` 返回 `'1'`
4. `PRAGMA table_info(events)` 列数 = `FAILiveEvent` UPROPERTY 数 + 内部列（tick_no / payload_text / wall_clock）总和；`tick_no` 列存在且 NOT NULL DEFAULT 0
5. `Saved/Games/_meta.db` 存在；含 `agent_lifecycle_events` + `agent_registry` 两表 + 各自 append-only trigger
6. **新增 `agent_calibration` 列存在性检查**：`PRAGMA table_info(agent_calibration)` 含列 `private_goal` / `alliance_members_json` / `seq_start` / `bid_offset` / `role` / `faction` / `model_vendor` / `model_name`

对齐 §11 **L0 单元** 前两项。

## 风险点

- `journal_mode = WAL` 是 database-level，一经设置持久化在 .db 文件中；其他 PRAGMA 是 per-connection 必须每次 BeginGame 重设（impl §3.2 注）
- FTS5 trigram tokenizer 在 UE 内置 SQLite 不一定编译；自检失败时必须降级 unicode61 + UE_LOG Warning，**不**在此步立即修，留到 T4 验证模糊搜索时回看
- 跨库 attach：单局 .db 与 `_meta.db` 是两个独立文件；EventStore 内部要持有两个 `FSQLiteDatabase` 实例（建议字段命名 `Db` + `MetaDb`）

## 完成定义

能 BeginGame / EndGame 而不写任何 events；DB Browser 看到正确的表结构；schema_version=1；`agent_calibration` 列齐。**不**意味着能 AppendEvent。
