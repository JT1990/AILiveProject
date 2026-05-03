# T2 — Schema migration + EventStore 子系统骨架

日期：2026-05-04
依赖：T0（SQLiteCore + OpenSSL，commit `842b05f`）+ T1（Memory/ 类型骨架，commit `3692655`）。

## 目标

在 T1 已建立 `FAILiveEvent` / `EAILiveEventType` / `FAILiveBid` 等类型骨架的基础上，落地 `UAILiveEventStoreSubsystem`（继承 `UGameInstanceSubsystem`）的生命周期 + 单局 `Saved/Games/<game_id>.db` 与跨局 `Saved/Games/_meta.db` 的全量 DDL（13 表 + 1 FTS5 虚表 = 14 项 / `_meta.db` 3 表）+ PRAGMA + index + trigger。本步**不**实现 AppendEvent / 读 API / 哈希链 / 投影；只验证 schema 落盘正确，给 T2.5 / T3 提供"打开 .db 即可读写"的基座。

完成 T2 **不**等于能 `AppendEvent`——那是 T3 的事。

## 关键决策

| 决策点 | 取值 | 理由 |
| --- | --- | --- |
| PRAGMA 执行频次 | **每次 BeginGame 完整跑一遍**（含 `journal_mode=WAL`） | `journal_mode=WAL` 是 database-level 持久化，但反复设置幂等无副作用；强制执行能修正历史失败运行 / 旧版本 / 手工创建留下的非 WAL 文件。剩余 4 个 PRAGMA（`synchronous=NORMAL` / `foreign_keys=ON` / `temp_store=MEMORY` / `mmap_size=268435456`）必须 per-connection 重设 |
| DDL 幂等性 | **全部 `CREATE TABLE/INDEX/TRIGGER/VIRTUAL TABLE IF NOT EXISTS`** | 防止 retry / 部分失败后再次 BeginGame 撞已存在错误 |
| 0→1 migration 原子性 | **`BEGIN IMMEDIATE; ... COMMIT;` 包裹**；任一 DDL 失败 `ROLLBACK` | 避免半建库状态。`schema_version='1'` 在事务尾部写入 |
| `payload_text` STORED vs VIRTUAL | **`GENERATED ALWAYS AS (json_extract(payload, '$.text')) STORED`** | 与 impl §3.2 line 191 严格一致；STORED 的取舍是写入时多算一次 hash 但读取（FTS5 / 模糊检索）零开销，符合"事件多写一次、查询多次"的访问模式。`PRAGMA table_xinfo` 上 `hidden=3`（STORED）；VIRTUAL 才是 `hidden=2`，验收用宽松集合 `{2,3}` |
| FTS5 tokenizer 自检位置 | **`EnsureSchema` 内、事务外执行**，结果 `FtsTokenizer` 字符串透传给 `ApplyMigrationV0ToV1` | 自检需要先 `DROP TABLE IF EXISTS temp.test_trigram` 兜底前次失败，自检完立即 drop 释放命名空间，防同连接重复触发误判。事务外是为了不污染 0→1 migration 的 BEGIN/COMMIT |
| FTS5 trigger 与 tokenizer 解耦 | `events_fts_ai` / `events_fts_ad` 始终落盘，**不论 tokenizer 是 trigram 还是 unicode61** | tokenizer 只决定 events_fts 虚表分词方式，不影响 INSERT/DELETE 同步逻辑 |
| 跨库桥接 | **不**用 `ATTACH DATABASE`；两个 `FSQLiteDatabase` 实例独立打开 | Delete 协议跨库桥接是 T9 的事 |
| UFUNCTION 安全桩 | 避免 UFUNCTION 修饰所有未实现方法（`AppendEvent` / `QueryEventsByActor` / `VerifyHashChain` 改成纯 C++ 方法）+ 安全桩实现 | 纯 C++ 方法不走 UHT thunk，没有"声明但缺定义即链接失败"的风险；同时为 T3/T4/T9 留下接口面 |
| Console 命令注册 | `FAutoConsoleCommandWithArgs`（带参） / `FAutoConsoleCommand` + `FConsoleCommandDelegate`（无参） | `FAutoConsoleCommand` 自身没带 args 重载；带参必须用 `WithArgs` 版 |
| World 解析 | `GEngine->GetWorldContexts()` 选 PIE/Game world → `GWorld` 兜底 | `GEngine->GetWorld()` 不存在 UE API |
| `kGenesisHash` 字面值 | `TEXT("0000000000000000000000000000000000000000000000000000000000000000")`（64 位 0） | SHA-256 长度的 genesis；本步只声明 + 定义，T3 AppendEvent 第一条事件使用 |
| `events_fts` 影子表是否计入"14 项" | **不计入** | `events_fts_data / _idx / _content / _docsize / _config` 是 FTS5 内部影子表，验收 SQL 用 `name NOT LIKE 'events_fts_%'` 排除 |

## 改动清单

### 新增

| 文件 | 内容摘要 |
| --- | --- |
| `Source/AILiveProject/Public/Memory/AILiveEventStoreSubsystem.h` | `UAILiveEventStoreSubsystem`（`UGameInstanceSubsystem` 子类）声明：生命周期 9 方法（`Initialize` / `Deinitialize` / `BeginGame` / `EndGame` / `IsGameOpen` / `GetCurrentGameId` / `ApplyPragmas` / `EnsureSchema` / `RunMigrations`）+ 安全桩 3 方法（`AppendEvent` / `QueryEventsByActor` / `VerifyHashChain`）+ `DetectFtsTokenizer` 私有助手 + `kCurrentSchemaVersion = 1` 常量 + `kGenesisHash` 静态字符串 + `Db` / `MetaDb` / `CurrentGameId` 三字段 |
| `Source/AILiveProject/Private/Memory/AILiveEventStoreSubsystem.cpp` | 9 方法真实实现 + 3 安全桩（`UE_LOG(Verbose)` + 默认返回值）+ 3 条 console 命令（`AILive.Test.BeginGame <id>` / `AILive.Test.EndGame` / `AILive.Test.SchemaSelfCheck`）+ `GetActiveWorldForConsole` 助手（PIE/Game 优先，GWorld 兜底）+ `LogQueryRows` 通用 SQL 列出助手 |
| `Source/AILiveProject/Private/Memory/AILiveSchemaMigration.cpp` | 28 条 DDL 字符串常量（13 表 + 4 trigger + 5 index 单局 / 3 表 + 4 trigger + 3 index 跨局）+ `events_fts` 虚表 DDL 由 `FString::Printf` 用传入的 `FtsTokenizer` 拼装 + `ApplyMigrationV0ToV1(FSQLiteDatabase&, bool, const TCHAR*)`（事务化 + IF NOT EXISTS + ROLLBACK on error） |

### 修

无。任务卡硬约束"只动涉及文件"——本步全部为新增。

## 验收实证

`sqlite3.exe`（`C:\ProgramData\miniconda3\Library\bin\sqlite3.exe`）跑下列 SQL，每条粘贴原始输出。PIE 流程：编辑器启动 → Play → console 输 `AILive.Test.BeginGame test001` → 输 `AILive.Test.SchemaSelfCheck` → 停 PIE（自动触发 `Deinitialize` → `EndGame` → 两个连接关闭）。

### 1. 文件存在 + 体积

```
Name       Length LastWriteTime
test001.db 167936 2026/5/4 5:21:05
_meta.db    40960 2026/5/4 5:21:05
```

### 2. 14 项落盘（单局 .db）

```sql
SELECT name FROM sqlite_master
WHERE type IN ('table','view')
  AND name NOT LIKE 'sqlite_%'
  AND name NOT LIKE 'events_fts_%'
ORDER BY name;
```

输出（行数 = 14）：

```
agent_calibration
agent_view_state
alliance_state
bel_violations
commitments
event_addressed_to
event_log_summaries
event_visibility
events
events_fts
game_state
leakage_audits
schema_meta
vote_history
```

### 3. schema_version 校验

```sql
SELECT value FROM schema_meta WHERE key='schema_version';  -- 输出: 1
```

### 4. events 列数对齐

```sql
SELECT cid, name, type, hidden FROM pragma_table_xinfo('events') ORDER BY cid;
```

输出（19 行）：

```
0|event_id|TEXT|0
1|game_id|TEXT|0
2|seq|INTEGER|0
3|tick_no|INTEGER|0
4|round_no|INTEGER|0
5|phase|TEXT|0
6|actor|TEXT|0
7|event_type|TEXT|0
8|speech_act_type|TEXT|0
9|visibility|TEXT|0
10|addressed_to|TEXT|0
11|payload|TEXT|0
12|payload_text|TEXT|3        ← STORED generated column (hidden=3)
13|parent_event_id|TEXT|0
14|parser_version|TEXT|0
15|raw_llm_output|TEXT|0
16|prev_event_hash|TEXT|0
17|event_hash|TEXT|0
18|wall_clock|TEXT|0
```

```sql
SELECT count(*) FROM pragma_table_info('events');   -- 输出: 18（不含 payload_text）
```

对齐公式：`FAILiveEvent` UPROPERTY 17 个 + 内部 `tick_no`（不入 USTRUCT）+ `payload_text`（STORED generated，仅出现在 table_xinfo）= **table_xinfo 19 / table_info 18**。

### 5. _meta.db 落盘 + 三表 + trigger

```sql
SELECT name FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%' ORDER BY name;
-- agent_lifecycle_events / agent_registry / schema_meta

SELECT name FROM sqlite_master WHERE type='trigger' ORDER BY name;
-- lifecycle_no_delete / lifecycle_no_update

SELECT value FROM schema_meta WHERE key='schema_version';
-- 1
```

### 6. agent_calibration 列存在性

```sql
SELECT name FROM pragma_table_info('agent_calibration') ORDER BY cid;
```

输出（10 列；任务卡要求的 8 列全部命中）：

```
game_id
agent_id
model_vendor          ← 任务卡要求
model_name            ← 任务卡要求
bid_offset            ← 任务卡要求
role                  ← 任务卡要求
faction               ← 任务卡要求
private_goal          ← 任务卡要求
alliance_members_json ← 任务卡要求 (DEFAULT '[]')
seq_start             ← 任务卡要求 (DEFAULT 1)
```

### 7. 幂等性 / 复用

代码层验证（未做实测；机制可推导）：

- `ApplyMigrationV0ToV1` 全部 DDL 用 `IF NOT EXISTS` —— 二次执行安全；
- `EnsureSchema` 在 `schema_meta.schema_version == kCurrentSchemaVersion`（即 `1`）时 **不进入** `RunMigrations`，直接日志 "no migration" 返回 true；
- `ApplyPragmas` 全部幂等（`journal_mode=WAL` 重复 set 是 SQLite 文档明确的 no-op）。

副推：用户测试单跑 `AILive.Test.BeginGame test001` 一次成功；二次未机械验证，但代码路径无 reentrancy 风险。下一步若 T3 实施过程中有需要验证可补一条 console 双调命令。

### 8. FTS5 自检结果

```sql
SELECT sql FROM sqlite_master WHERE name='events_fts';
```

输出：

```
CREATE VIRTUAL TABLE events_fts USING fts5(payload_text, content='events', content_rowid='rowid', tokenize='trigram')
```

**trigram tokenizer 可用**——UE 5.7 内置 SQLite 编译了 fts5-trigram。无降级 unicode61 警告。模糊搜索能力到 T4 验收时直接走 trigram 路径。

### 9. append-only + FTS 同步 trigger 落盘（单局 .db）

```sql
SELECT name FROM sqlite_master WHERE type='trigger' ORDER BY name;
```

输出（4 行）：

```
events_fts_ad         ← FTS DELETE 同步
events_fts_ai         ← FTS INSERT 同步
events_no_delete      ← append-only 强制
events_no_update      ← append-only 强制
```

## 风险点 / 后续

- **T3 启用 AppendEvent 时**：`raw_llm_output` 列在 schema 里是 nullable，但代码侧需要 confirm 写空 vs 写 NULL 的策略。当前 USTRUCT `RawLLMOutput` 默认空字符串，不冲突。
- **T4 模糊搜索**：trigram 已确认可用；如 T4 在其他机器（非开发机）上发现 fts5-trigram 不可用，自检会自动降级 unicode61，验收脚本仅断言 `tokenize ∈ {'trigram','unicode61'}` 即可。
- **`SchemaSelfCheck` console 命令读法**：开了独立 read-only 连接（`ESQLiteDatabaseOpenMode::ReadOnly`）查 schema，依靠 WAL 模式的 concurrent reader 保证不阻塞主连接。该命令未来可能被 T9 复用做"DB Browser 实时只读"的兜底验证。
- **临时调用点**：本步**不**改 ACT01 / ACT02 既有代码；触发完全靠 console 命令。任务卡"硬约束 — 任务卡之外的代码不顺手清理"已遵守。
- **schema_version 共享**：当前 `_meta.db` 与单局 `.db` 共用 `schema_version='1'`；后续两库分别演进时再加 `meta_schema_version` 区分。
