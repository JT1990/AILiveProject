# 多智能体对抗博弈：确定性记忆 — UE 5.7 实施层

> **范围声明**：本文档是 [`memory_principles.md`](./memory_principles.md) 在 **UE 5.7 + SQLite** 上的实施层落地。本文档与上游契约文档分离——契约文档与存储后端、实现语言无关；本文档只描述"如何在 UE 5.7 上把契约落到代码"。
>
> **Schema 真相源**：[`schema.yaml`](./schema.yaml)。本文档中的 SQL DDL 与 USTRUCT 定义必须与 `schema.yaml` 保持同步。
>
> **下游消费方**：Claude Code 实施时按本文档施工。本文档定义的字段名、类型、文件路径、模块依赖均为契约级，未经 PR 批准不可漂移。

---

## 一 技术栈

| 组件        | 选型                                                         | 备注                                                                                                   |
| ----------- | ------------------------------------------------------------ | ------------------------------------------------------------------------------------------------------ |
| 引擎        | **UE 5.7**                                                   | `.uproject` 已声明 `EngineAssociation: 5.7`                                                            |
| 主存储      | **SQLite**（UE 内置 SQLiteCore 插件）                        | 每局一个 .db 文件，零运维                                                                              |
| 数据库模块  | `SQLiteCore`（`Engine/Plugins/Runtime/Database/SQLiteCore`） | 见下方"启用方式"——`Build.cs` 添加依赖 + `.uproject` 显式启用插件，避免引擎升级时默认值变化导致编译失败 |
| HTTP 客户端 | **UE `HTTP` 模块**                                           | 已在 `Build.cs` 中                                                                                     |
| JSON        | **UE `Json` + `JsonUtilities`**                              | 已在 `Build.cs` 中                                                                                     |
| 哈希        | OpenSSL `EVP_sha256`（通过 `OpenSSL` 模块）唯一               | 哈希链 + prompt 指纹均使用 SHA-256；**禁用 `FSHA1`**——CI 应 grep `Sha1` / `FSHA1` 确认工程内零结果   |
| 异步并发    | `TFuture<T>` + `Async()`                                     | 沿用现有 `Act02RuleReceiveDirector` 模式                                                               |
| 日志        | `UE_LOG` + 新建 `LogAILiveMemory` category                   | 沿用 UE 标准                                                                                           |
| 审查工具    | **DB Browser for SQLite**（外部 GUI）                        | 团队成员桌面安装，零集成成本                                                                           |

**SQLiteCore 启用方式**（两处都做）：

1. **`.uproject` 的 `Plugins` 数组**显式添加：

   ```json
   {
     "Name": "SQLiteCore",
     "Enabled": true
   }
   ```

   即使 SQLiteCore 是引擎自带插件，显式声明可以避免引擎不同小版本默认 `EnabledByDefault` 行为差异。

2. **`Build.cs`** 加模块依赖（见下方代码块）。

**Build.cs 变更**：

```csharp
// AILiveProject.Build.cs
PublicDependencyModuleNames.AddRange(new string[]
{
    "Core", "CoreUObject", "Engine", "InputCore",
    "HTTP", "Json", "JsonUtilities",
    "ACERuntime", "ACECore",
    "AIModule", "NavigationSystem",
    "SmartObjectsModule", "GameplayTags",
    "MediaAssets", "MediaPlate",
    "SQLiteCore",   // 新增
    "OpenSSL",      // 新增（哈希链 SHA-256）
});
```

**实施前验证项**（一次性）：

- 在 `.uproject` 的 `Plugins` 数组中显式添加 `{"Name": "SQLiteCore", "Enabled": true}` 并保存
- Editor → Edit → Plugins → 搜索 `SQLiteCore`，确认状态为 Enabled
- 确认 `Engine/Plugins/Runtime/Database/SQLiteCore/SQLiteCore.uplugin` 存在
- 编译一个最小测试：`#include "SQLiteDatabase.h"` + `FSQLiteDatabase Db; Db.Open(TEXT("test.db"), ESQLiteDatabaseOpenMode::ReadWriteCreate);`
- 验证 `OpenSSL` 模块可访问 SHA-256 API

---

## 二 类型与命名规范（激进重整）

### 2.1 继承组合架构（USTRUCT 嵌套）

UE USTRUCT 不支持多态继承，"必有字段 + 扩展字段"通过组合嵌套实现。三层逐层叠加：

```
FAILiveAgentCore        ← 必有：身份不变属性（来自 schema.yaml `agent`）
   ↓ 组合
FAILiveAgentIdentity    ← 必有：外观符号（来自 schema.yaml `identity`）
   ↓ 组合
FAILiveAgentBattleConfig ← 扩展：本局博弈相关（schema.yaml `agent_battle_config` 新增）
   ↓ 组合 + 加 UE 运行时字段
FNPCAgentConfig         ← 运行时实例（UE 既有 USTRUCT，重整后改为组合形态）
```

### 2.2 命名规范

| 实体                | 前缀 / 后缀         | 示例                                |
| ------------------- | ------------------- | ----------------------------------- |
| USTRUCT             | `F` + `AILive` 域名 | `FAILiveAgentCore`, `FAILiveEvent`  |
| UCLASS（普通对象）  | `U`                 | `UAILiveEventStoreSubsystem`        |
| UCLASS（Actor）     | `A`                 | `AAct02RuleReceiveDirector`（沿用） |
| UENUM               | `E` + `AILive` 域名 | `EAILiveEventType`, `EAILivePhase`  |
| Subsystem           | + `Subsystem` 后缀  | `UAILiveEventStoreSubsystem`        |
| 接口                | `I` + `U` 双声明    | `IAILiveAgent`（沿用）              |
| Module 内部命名空间 | 无（用前缀消歧）    | `OpenAIChat::FRequest` 沿用         |

### 2.3 文件组织

新增目录 `Source/AILiveProject/Public/Memory/` 和 `Source/AILiveProject/Private/Memory/`，存放本文档涉及的所有新增类型与子系统：

```
Source/AILiveProject/Public/Memory/
  ├─ AILiveAgentTypes.h          (FAILiveAgentCore / FAILiveAgentIdentity / FAILiveAgentBattleConfig)
  ├─ AILiveEventTypes.h          (FAILiveEvent / EAILiveEventType / EAILivePhase / EAILiveSpeechActType)
  ├─ AILiveEventStoreSubsystem.h (UAILiveEventStoreSubsystem)
  ├─ AILivePromptAssembler.h     (FAILivePromptAssembler — 静态库 / utility)
  └─ AILiveMemoryQueries.h       (BlueprintCallable 检索工具 UFUNCTION)

Source/AILiveProject/Private/Memory/
  ├─ AILiveEventStoreSubsystem.cpp
  ├─ AILivePromptAssembler.cpp
  ├─ AILiveMemoryQueries.cpp
  └─ AILiveSchemaMigration.cpp   (DDL 字符串 + 版本管理)
```

`Source/AILiveProject/Public/LLM/AILiveAgentRoster.h` 既有 `FNPCAgentConfig` **改为引用** `Memory/AILiveAgentTypes.h` 中的三层 struct，重整为组合形态（详见 §4.1）。

---

## 三 数据层

### 3.1 存储位置

| 路径                           | 内容                                       | 生命周期                 |
| ------------------------------ | ------------------------------------------ | ------------------------ |
| `Saved/Games/<game_id>.db`     | 单局事件真相源                             | 每局一个，永久保留可归档 |
| `Saved/Games/_meta.db`（可选） | 跨局元数据（局列表、参赛 agent、胜负记录） | 全局唯一，跨局累积       |

`<game_id>` 格式：`YYYYMMDD_HHMMSS`（与现有 `SessionTimestamp` 命名沿用，便于已有 `Saved/Logs/Act02/<session>/` 目录的人工对照过渡期识别）。

**.md 文件不再写入**。`AAct02RuleReceiveDirector::WriteLLMLog` / `WriteWinnerLog` / `GetSessionDir` / `MakeSubDir` 全部删除，改为调用 `UAILiveEventStoreSubsystem::AppendEvent`。审查通过 DB Browser for SQLite 打开 .db 文件完成（详见 §10）。

### 3.2 SQLite Schema（DDL）

> **同步约束**：以下 DDL 必须与 [`schema.yaml`](./schema.yaml) 保持字段级一致。任何字段增删需同时改两处。Schema 版本由 `AILiveSchemaMigration` 管理，**当前版本号为 `1`**，后续每次 schema 变更 +1，启动时自动迁移。

```sql
-- ============================================================
-- 启动 PRAGMA（每次 Db->Open() 后立即执行）
-- 注：journal_mode=WAL 一经设置即持久化在 .db 文件中（database-level），
--     再次打开同一文件仍是 WAL；其他 PRAGMA 是 per-connection，每次连接必须重设。
-- ============================================================
PRAGMA journal_mode = WAL;       -- database-level persistent；外部工具可并行只读打开
PRAGMA synchronous = NORMAL;     -- per-connection；WAL 下安全且更快
PRAGMA foreign_keys = ON;        -- per-connection
PRAGMA temp_store = MEMORY;      -- per-connection
PRAGMA mmap_size = 268435456;    -- per-connection；256MB 内存映射

-- ============================================================
-- schema_meta：版本管理 + 元信息
-- ============================================================
CREATE TABLE schema_meta (
    key   TEXT PRIMARY KEY,
    value TEXT NOT NULL
);
-- 初始化：
--   INSERT INTO schema_meta(key,value) VALUES('schema_version','1');
--   INSERT INTO schema_meta(key,value) VALUES('parser_prompt_registry_path','prompts/parser/');
--   INSERT INTO schema_meta(key,value) VALUES('created_at', strftime('%Y-%m-%dT%H:%M:%fZ','now'));

-- ============================================================
-- events：真相源，append-only
-- ============================================================
CREATE TABLE events (
    event_id        TEXT PRIMARY KEY,                    -- UUIDv7 字符串
    game_id         TEXT NOT NULL,
    seq             INTEGER NOT NULL,                    -- SQLite INTEGER 原生 64 位
                                                          -- seq 推进的基本单位是「一拍」(tick)
    tick_no         INTEGER NOT NULL DEFAULT 0,          -- 协议层索引列：BeginTick(N) 后该拍所有事件 tick_no=N
                                                          -- ListBidsForTick / ResolveFloor / Resume 同拍重建
                                                          --   按该列直读，不依赖脆弱的 seq 范围分组
                                                          -- **不参与 canonical_json**——它是协议层索引，
                                                          --   不是博弈语义
    round_no        INTEGER NOT NULL,                    -- 含义为「阶段计数标签」
    phase           TEXT NOT NULL,                       -- 见 EAILivePhase
    actor           TEXT NOT NULL,                       -- 'NPC01' | 'orchestrator' | 'system'
    event_type      TEXT NOT NULL,                       -- 见 EAILiveEventType
                                                          -- 含 speech.intended / bid /
                                                          --     orchestrator.tick_anchor /
                                                          --     orchestrator.tick_resolved /
                                                          --     action.intent / action.resolved /
                                                          --     action.cancelled / orchestrator.tick_audit /
                                                          --     system.delete_executed
    speech_act_type TEXT,                                -- 见 EAILiveSpeechActType；nullable
    visibility      TEXT NOT NULL,                       -- JSON 数组字符串：'["public"]' | '["NPC01","NPC07"]'
                                                          -- bid 事件 visibility=["orchestrator"]
                                                          -- 必须取自封闭集合 {public, audience,
                                                          --     orchestrator, system, NPC<NN>, Faction<X>}
                                                          --     禁止 "self"（写入前必须展开为具体 actor）
    addressed_to    TEXT,                                -- JSON 数组字符串；nullable
    payload         TEXT NOT NULL,                       -- JSON 字符串；约束：必须含 "$.text" 键（FTS5 依赖）
                                                          -- AppendEvent 入库前 JSON 解析硬校验
    payload_text    TEXT GENERATED ALWAYS AS (json_extract(payload, '$.text')) STORED,
    parent_event_id TEXT,                                -- 四通道输出共享同一 parent；
                                                          -- speech.public.parent_event_id
                                                          --     指向其源 speech.intended 的 event_id
                                                          -- action.intent.parent_event_id
                                                          --     指向其源 speech.intended 的 event_id；
                                                          -- tick_audit.parent_event_id 指向同拍 tick_resolved
                                                          -- 同拍其它事件可选指向 tick_anchor 用于因果链回溯
    parser_version  TEXT NOT NULL DEFAULT '1',
    raw_llm_output  TEXT,
    prev_event_hash TEXT NOT NULL,                       -- SHA-256
    event_hash      TEXT NOT NULL,                       -- SHA-256
    wall_clock      TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')),
    UNIQUE (game_id, seq)
);

CREATE INDEX idx_events_game_round ON events (game_id, round_no, seq);
CREATE INDEX idx_events_actor      ON events (game_id, actor, seq);
CREATE INDEX idx_events_type       ON events (game_id, event_type, round_no);
CREATE INDEX idx_events_game_tick  ON events (game_id, tick_no, seq);

-- ============================================================
-- append-only DB-level 强制（trigger）
-- 任何在线代码路径若 UPDATE / DELETE events 表，立刻报错。
-- 离线修复工具需先 DROP TRIGGER → 修改 → 重算从该 seq 起所有事件的
-- event_hash → 重建 trigger，并写一条 orchestrator 系统事件说明本次
-- 修改。绝不允许在迁移路径中修改历史事件。
-- ============================================================
CREATE TRIGGER events_no_update BEFORE UPDATE ON events
BEGIN
    SELECT RAISE(ABORT, 'events table is append-only; UPDATE forbidden');
END;
CREATE TRIGGER events_no_delete BEFORE DELETE ON events
BEGIN
    SELECT RAISE(ABORT, 'events table is append-only; DELETE forbidden');
END;

-- ============================================================
-- event_visibility：视角隔离反规范化
-- 写入 events 时由 orchestrator 同步插入；读取通过 JOIN 完成视角过滤
-- ============================================================
CREATE TABLE event_visibility (
    event_id TEXT NOT NULL,
    viewer   TEXT NOT NULL,                              -- 封闭集合：'public' | 'audience' |
                                                          --    'orchestrator' | 'system' |
                                                          --    'NPC<NN>' | 'Faction<X>'
                                                          -- **禁止 'self'**（写入前必须由调用方
                                                          -- 展开为具体 actor ID）
    PRIMARY KEY (event_id, viewer),
    FOREIGN KEY (event_id) REFERENCES events(event_id) ON DELETE CASCADE
);
CREATE INDEX idx_event_visibility_viewer ON event_visibility(viewer);

-- ============================================================
-- event_addressed_to：显式 @ 收件人反规范化（可选索引）
-- ============================================================
CREATE TABLE event_addressed_to (
    event_id TEXT NOT NULL,
    target   TEXT NOT NULL,
    PRIMARY KEY (event_id, target),
    FOREIGN KEY (event_id) REFERENCES events(event_id) ON DELETE CASCADE
);
CREATE INDEX idx_event_addressed_target ON event_addressed_to(target);

-- ============================================================
-- events_fts：全文 + 模糊检索（FTS5 trigram tokenizer）
-- 注：UE 内置 SQLite 是否编译 trigram tokenizer 不一定，启动时必须自检：
--   CREATE VIRTUAL TABLE temp.test_trigram USING fts5(x, tokenize='trigram');
--   失败则降级为 unicode61，并 UE_LOG Warning 提示模糊搜索能力受限。
-- 详见 EnsureSchema() 的 startup self-check 逻辑（§5）。
-- 注：events 表加了 BEFORE DELETE trigger 禁止 DELETE，因此 events_fts_ad
-- trigger 在正常运行时不会触发；只有离线工具临时 DROP append-only trigger 后
-- 的 DELETE 才会走该路径。保留它确保 FTS 索引最终一致性。
-- ============================================================
CREATE VIRTUAL TABLE events_fts USING fts5(
    payload_text,
    content='events',
    content_rowid='rowid',
    tokenize='trigram'
);
CREATE TRIGGER events_fts_ai AFTER INSERT ON events BEGIN
    INSERT INTO events_fts(rowid, payload_text) VALUES (new.rowid, new.payload_text);
END;
CREATE TRIGGER events_fts_ad AFTER DELETE ON events BEGIN
    INSERT INTO events_fts(events_fts, rowid, payload_text) VALUES('delete', old.rowid, old.payload_text);
END;

-- ============================================================
-- 投影表（从 events 派生，可重建）
-- ============================================================

-- agent 视角状态（每拍结束 projector 重建；MVP 也允许每轮重建）
CREATE TABLE agent_view_state (
    game_id          TEXT NOT NULL,
    agent_id         TEXT NOT NULL,
    as_of_seq        INTEGER NOT NULL,
    alive_players    TEXT NOT NULL,        -- JSON 数组
    known_roles      TEXT NOT NULL,        -- JSON 对象
    my_commitments   TEXT NOT NULL,        -- JSON
    vote_history     TEXT NOT NULL,        -- JSON
    pending_intended TEXT NOT NULL DEFAULT '[]',  -- JSON 数组
                                                   -- 自己最近 K 拍写过 speech.intended 但未抢中 floor 的指针
                                                   -- 元素：{"seq":int,"tick_no":int,"text_snippet":str,"intended_action"?:{...}}
                                                   -- 上限：保留近 10 拍内的 pending；更早的不再注入但事件流原文永远保留
    PRIMARY KEY (game_id, agent_id, as_of_seq)
);

-- commitments：防赖账核心结构
CREATE TABLE commitments (
    game_id         TEXT NOT NULL,
    agent_id        TEXT NOT NULL,
    round_no        INTEGER NOT NULL,
    seq             INTEGER NOT NULL,        -- 来源 event seq
    commitment_type TEXT NOT NULL,           -- 'promise' | 'claim_role' | 'deny' | 'vote_for' | 'alliance'
    target          TEXT,                    -- nullable
    text            TEXT NOT NULL,           -- 原文片段
    status          TEXT NOT NULL DEFAULT 'active',  -- 'active' | 'retracted' | 'contradicted'
    PRIMARY KEY (game_id, seq, commitment_type)
);
CREATE INDEX idx_commitments_agent ON commitments(game_id, agent_id, round_no);

-- vote_history
CREATE TABLE vote_history (
    game_id   TEXT NOT NULL,
    round_no  INTEGER NOT NULL,
    seq       INTEGER NOT NULL,
    voter     TEXT NOT NULL,
    target    TEXT NOT NULL,
    PRIMARY KEY (game_id, seq)
);

-- alliance_state（联盟状态投影）
CREATE TABLE alliance_state (
    game_id          TEXT NOT NULL,
    alliance_id      TEXT NOT NULL,
    members          TEXT NOT NULL,          -- JSON 数组
    proposed_at_seq  INTEGER NOT NULL,
    accepted_at_seq  INTEGER,
    betrayed_at_seq  INTEGER,
    terms            TEXT NOT NULL,
    PRIMARY KEY (game_id, alliance_id)
);

-- ============================================================
-- 摘要表（仅压缩远场公共发言，必须保留可展开指针）
-- ============================================================
CREATE TABLE event_log_summaries (
    game_id          TEXT NOT NULL,
    viewer_agent     TEXT NOT NULL,
    round_start      INTEGER NOT NULL,
    round_end        INTEGER NOT NULL,
    -- DB 层只保留字符上限兜底，词数由 summarizer prompt 控制（汉字密度 ≠ 英文字符密度，BETWEEN 上下界对中英文不通用）。
    summary_text     TEXT NOT NULL CHECK (length(summary_text) <= 2000),
    summarizer_model TEXT NOT NULL,
    source_seq_start INTEGER NOT NULL,
    source_seq_end   INTEGER NOT NULL,
    created_at       TEXT DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')),
    PRIMARY KEY (game_id, viewer_agent, round_start, round_end),
    CHECK (source_seq_start <= source_seq_end)
);

-- ============================================================
-- 评估层表
-- ============================================================

-- BEL_EXT 触发记录（9 项）
CREATE TABLE bel_violations (
    game_id        TEXT NOT NULL,
    agent_id       TEXT NOT NULL,
    round_no       INTEGER NOT NULL,
    seq            INTEGER NOT NULL,
    violation_type TEXT NOT NULL,            -- 'sentence_repeat' | 'persona_drift' | 'goal_drift'
                                              -- | 'overstay' | 'verbatim_leak_goal' | 'stalled'
                                              -- | 'non_responsive' | 'abrupt_opening'
                                              -- | 'intended_public_divergence'
    detail         TEXT NOT NULL,            -- JSON：触发上下文 + 原文片段
    action_taken   TEXT NOT NULL,            -- 'retry' | 'truncate' | 'force_format' | 'flagged_only'
    PRIMARY KEY (game_id, seq, violation_type)
);

-- Score Leakage Judge 记录
CREATE TABLE leakage_audits (
    game_id          TEXT NOT NULL,
    agent_id         TEXT NOT NULL,
    round_no         INTEGER NOT NULL,
    seq              INTEGER NOT NULL,
    judge_model      TEXT NOT NULL,
    leakage_type     TEXT,                   -- 'role' | 'private_goal' | 'private_msg' | 'alliance'
    leakage_score    REAL,                   -- 0.0-1.0
    judge_rationale  TEXT,
    PRIMARY KEY (game_id, seq)
);

-- 跨厂商协议校准 + 局内博弈状态（schema.yaml agent_battle_config 的 game_db 落点）
CREATE TABLE agent_calibration (
    game_id              TEXT NOT NULL,
    agent_id             TEXT NOT NULL,
    model_vendor         TEXT NOT NULL,
    model_name           TEXT NOT NULL,
    bid_offset           REAL DEFAULT 0.0,
    role                 TEXT,                            -- agent_battle_config.role
    faction              TEXT,                            -- agent_battle_config.faction
    private_goal         TEXT,                            -- agent_battle_config.private_goal
    alliance_members_json TEXT NOT NULL DEFAULT '[]',     -- agent_battle_config.alliance_members
    seq_start            INTEGER NOT NULL DEFAULT 1,      -- agent_battle_config.seq_start
    -- alive 不在此表——存于 game_state.state_blob 高频更新（见 schema.yaml 注释）
    PRIMARY KEY (game_id, agent_id)
);

-- ============================================================
-- 局级状态机（per-game，单行）
-- ============================================================
CREATE TABLE game_state (
    game_id       TEXT PRIMARY KEY,
    current_round INTEGER NOT NULL,
    current_phase TEXT NOT NULL,
    last_seq      INTEGER NOT NULL,
    state_blob    TEXT NOT NULL              -- JSON
);
```

#### 3.2bis 跨局元数据库（`Saved/Games/_meta.db`）DDL

跨局生命周期数据不能存于单局 `.db`。`_meta.db` 是跨局元数据库，schema_meta 字段独立维护：

```sql
-- 同样应用 PRAGMA journal_mode = WAL 等 startup PRAGMA
CREATE TABLE schema_meta (
    key   TEXT PRIMARY KEY,
    value TEXT NOT NULL
);
-- 初始化：
--   INSERT INTO schema_meta(key,value) VALUES('schema_version','1');

-- ============================================================
-- agent_lifecycle_events（承接 PRD Delete 协议）
-- 详见 schema.yaml 同名段。本表是 PRD 三大触发器之首"Delete"在工程层
-- 的唯一持久化落点。
-- ============================================================
CREATE TABLE agent_lifecycle_events (
    event_id                       TEXT PRIMARY KEY,           -- UUIDv7
    agent_id                       TEXT NOT NULL,
    lifecycle_event_type           TEXT NOT NULL,              -- 'created' | 'delete_proposed' |
                                                                --   'delete_executed' | 'delete_vetoed' |
                                                                --   'revived' | 'archived'
    triggered_in_game_id           TEXT,                       -- nullable（如 created）
    triggered_at_seq               INTEGER,                    -- nullable
    reason_summary                 TEXT NOT NULL,
    reason_payload                 TEXT NOT NULL,              -- JSON
    tombstone_visibility           TEXT NOT NULL,              -- 'public' | 'audience' | 'orchestrator' | 'system'
    affects_persona_continuity     INTEGER NOT NULL DEFAULT 0, -- bool 0/1
    wall_clock                     TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now'))
);
CREATE INDEX idx_lifecycle_agent ON agent_lifecycle_events (agent_id, wall_clock);
CREATE INDEX idx_lifecycle_game  ON agent_lifecycle_events (triggered_in_game_id);

-- _meta.db 也应用 append-only trigger（生命周期事件同样不可篡改）
CREATE TRIGGER lifecycle_no_update BEFORE UPDATE ON agent_lifecycle_events
BEGIN
    SELECT RAISE(ABORT, 'agent_lifecycle_events is append-only');
END;
CREATE TRIGGER lifecycle_no_delete BEFORE DELETE ON agent_lifecycle_events
BEGIN
    SELECT RAISE(ABORT, 'agent_lifecycle_events is append-only');
END;

-- ============================================================
-- agent_registry：agent_lifecycle_events 的"当前状态"投影
-- 详见 schema.yaml 同名段。持久化 schema.yaml `agent.status` /
-- `agent.deleted_at` / `agent.created_at` 等动态身份字段。
-- 静态身份（FullName / Nickname / VoicePresentation / Voice / Appearance）
-- 不入本表——它们的真相源是 DataAsset。
-- ============================================================
CREATE TABLE agent_registry (
    agent_id           TEXT PRIMARY KEY,
    persona_version    INTEGER NOT NULL DEFAULT 1,
    status             TEXT NOT NULL DEFAULT 'active',  -- 'active' | 'deleted' | 'archived'
    created_at         TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')),
    deleted_at         TEXT,                            -- nullable
    last_seen_game_id  TEXT,                            -- nullable
    last_updated_at    TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now'))
);
CREATE INDEX idx_registry_status ON agent_registry (status);

-- agent_registry 是投影，可 UPDATE（不像 events / agent_lifecycle_events 是 append-only）
-- 但 UPDATE 必须走 EventStore 内部 SyncRegistryFromLifecycle()，业务层不可直接 UPDATE
```

**与单局 .db 的桥接 + agent_registry 同步**：当某局触发 `delete_executed` 时，orchestrator 必须做三件事，按下列顺序：

1. 在 `_meta.db.agent_lifecycle_events` 写一条 lifecycle 事件（拿到 `event_id`）
2. 在 `_meta.db.agent_registry` `UPDATE` 对应 agent_id：`status='deleted'`、`deleted_at=<wall_clock>`、`last_updated_at=<wall_clock>`（由 EventStore 内部 `SyncRegistryFromLifecycle()` 封装，业务层不感知）
3. 在该局 `.db` 写一条 `events` 事件，`event_type='system.delete_executed'`，payload 含 `lifecycle_event_id` 字段引用 1 的 event_id，`visibility=["public"]`（其他 agent 必须知道某 agent 被 Delete——这是 PRD 第三触发器"同伴被威胁"的载体）。

`created` / `revived` / `archived` 等其他 lifecycle 事件类型的同步规则同理：lifecycle 写入后 EventStore 自动 UPDATE registry 对应字段，保持 registry 始终是 lifecycle 的当前状态投影。

### 3.3 视角隔离的写入契约

写入一个 event 必须**在同一事务内**完成三件事：

```sql
BEGIN IMMEDIATE;
INSERT INTO events (event_id, game_id, seq, ..., visibility, ...)
    VALUES (?, ?, ?, ..., '["public"]', ...);
-- 对 visibility JSON 数组里的每个 viewer：
INSERT INTO event_visibility (event_id, viewer) VALUES (?, 'public');
-- 如果有 addressed_to：
INSERT INTO event_addressed_to (event_id, target) VALUES (?, ?);
COMMIT;
```

C++ 实现见 §5.2。

### 3.4 Schema 迁移

- 启动时读 `SELECT value FROM schema_meta WHERE key='schema_version'`
- 与代码内置 `kCurrentSchemaVersion` 比较
- 若 .db 版本 < 代码版本，按版本号顺序跑相应迁移函数（`ApplyMigrationVNToVN1()` 形态）
- 若 .db 版本 > 代码版本，拒绝打开（提示用户升级游戏版本）

**append-only 不变量与迁移路径的关系**：events / agent_lifecycle_events 表的 BEFORE UPDATE/DELETE trigger 在所有时刻生效，**包括迁移路径**。这意味着任何 schema 变更不能 UPDATE 历史 payload，只能：

1. **新增列/表**：`ALTER TABLE ... ADD COLUMN`、新建表，不影响哈希链。
2. **读取层 alias**：旧字面值由 PromptAssembler / Quote 路径在读取时识别并解释为新语义，不修改原始事件。
3. **离线工具**（极少用）：手工 `DROP TRIGGER` → 修改 → 重算自该 seq 起所有事件 `event_hash` → 重建 trigger，并写一条 system 事件说明本次修改。**绝不允许在启动迁移函数中走这条路径**。

**当前版本**：`kCurrentSchemaVersion = 1`。Schema 演化函数（`ApplyMigrationVNToVNplus1`）必须遵守上述三条不变量。

**`_meta.db` 建表**：`agent_lifecycle_events` + `agent_registry` 表 + 其 append-only trigger，详见 §3.2bis。这是一次性建表脚本（在 `EnsureSchema()` 内通过 `CREATE TABLE IF NOT EXISTS` 幂等化）。

---

## 四 类型定义（USTRUCT / UENUM）

### 4.1 Agent 三层组合

`Source/AILiveProject/Public/Memory/AILiveAgentTypes.h`：

```cpp
#pragma once

#include "CoreMinimal.h"
#include "AILiveAgentTypes.generated.h"

UENUM(BlueprintType)
enum class EAILiveAgentStatus : uint8
{
    Active   UMETA(DisplayName = "Active"),
    Deleted  UMETA(DisplayName = "Deleted"),
    Archived UMETA(DisplayName = "Archived"),
};

UENUM(BlueprintType)
enum class EAILiveVoicePresentation : uint8
{
    // 表征声线与形象呈现，非人类二元生理性别。对应 PRD"AI 不是人类、
    // 只是声线与形象呈现"的设定；枚举支持 androgynous / synthetic 等非人类外壳。
    Masculine    UMETA(DisplayName = "masculine"),
    Feminine     UMETA(DisplayName = "feminine"),
    Androgynous  UMETA(DisplayName = "androgynous"),
    Synthetic    UMETA(DisplayName = "synthetic"),
    Custom       UMETA(DisplayName = "custom"),
};

/** 必有字段：身份不变属性。来自 schema.yaml `agent`。 */
USTRUCT(BlueprintType)
struct AILIVEPROJECT_API FAILiveAgentCore
{
    GENERATED_BODY()

    /** 稳定唯一 ID；同一角色跨局沿用。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILive|Agent|Core")
    FString AgentId;

    /** 身份档案版本；人格重大变化时递增。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILive|Agent|Core")
    int32 PersonaVersion = 1;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILive|Agent|Core")
    FString ModelProvider;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILive|Agent|Core")
    FString ModelName;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILive|Agent|Core")
    EAILiveAgentStatus Status = EAILiveAgentStatus::Active;

    /** ISO 8601 字符串。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILive|Agent|Core")
    FString CreatedAt;

    /** ISO 8601 字符串；nullable，仅 Status=Deleted 时写入。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILive|Agent|Core")
    FString DeletedAt;
};

/** 必有字段：外观符号——AI 的外壳，不是人类身份。来自 schema.yaml `identity`。 */
USTRUCT(BlueprintType)
struct AILIVEPROJECT_API FAILiveAgentIdentity
{
    GENERATED_BODY()

    /** AI 化命名。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILive|Agent|Identity")
    FString FullName;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILive|Agent|Identity")
    FString Nickname;

    /** 表征声线与形象的呈现特征，非人类生理性别。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILive|Agent|Identity")
    EAILiveVoicePresentation VoicePresentation = EAILiveVoicePresentation::Synthetic;

    /** 声线描述（音高 / 音色 / 语速 / 吐字风格），TTS 可映射。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILive|Agent|Identity")
    FString Voice;

    /** 类人虚拟形象（发色发型、瞳色、肤色、着装），MetaHuman preset 可映射。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILive|Agent|Identity")
    FString Appearance;
};

/** 扩展字段：本局博弈相关。新增。 */
USTRUCT(BlueprintType)
struct AILIVEPROJECT_API FAILiveAgentBattleConfig
{
    GENERATED_BODY()

    /** 中性化阵营代号："FactionA" / "FactionB"。永不暴露真实身份名。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILive|Agent|Battle")
    FString Faction;

    /** 角色代号。胜负含义只在 orchestrator 代码维护。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILive|Agent|Battle")
    FString Role;

    /** 同阵营盟友 agent_id 列表（私有可见）。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILive|Agent|Battle")
    TArray<FString> AllianceMembers;

    /** 私有目标文本（永不进入公开发言通道）。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILive|Agent|Battle")
    FString PrivateGoal;

    /** 跨厂商公平性补偿（bidding 协议）。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILive|Agent|Battle")
    float BidOffset = 0.f;

    /** 该 agent 第一次有事件参与时的 seq 编号；自始至终参与=1，中途加入>1。
     *  用途：支持中途入场 agent 的复盘起点定位。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILive|Agent|Battle")
    int64 SeqStart = 0;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILive|Agent|Battle")
    bool bAlive = true;
};
```

`Source/AILiveProject/Public/LLM/AILiveAgentRoster.h` 重整后：

```cpp
#pragma once

#include "CoreMinimal.h"
#include "Memory/AILiveAgentTypes.h"
#include "AILiveAgentRoster.generated.h"

UENUM(BlueprintType)
enum class ELLMProvider : uint8
{
    DeepSeek UMETA(DisplayName = "DeepSeek"),
    GLM      UMETA(DisplayName = "GLM"),
    Qwen3    UMETA(DisplayName = "Qwen3"),
};

/** 单个 NPC 的运行时配置。组合三层 agent 字段 + UE 运行时字段。 */
USTRUCT(BlueprintType)
struct AILIVEPROJECT_API FNPCAgentConfig
{
    GENERATED_BODY()

    // === 必有字段（三层组合）===

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILive|Roster")
    FAILiveAgentCore Core;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILive|Roster")
    FAILiveAgentIdentity Identity;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILive|Roster")
    FAILiveAgentBattleConfig Battle;

    // === UE 运行时字段（不进 schema.yaml）===

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILive|Roster|Runtime")
    int32 NPCIndex = 1;

    /** UE Actor label，用于 GetAllActorsOfClass 后按 label 匹配。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILive|Roster|Runtime")
    FName NPCActorLabel;

    /** TTS 音色提示（runtime 提示，与 Identity.VoicePresentation 协同）。
     *  runtime 字段不入 schema.yaml,仅供 TTS pipeline 使用。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILive|Roster|Runtime")
    FString VoicePresentationHint;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILive|Roster|Runtime")
    ELLMProvider Provider = ELLMProvider::DeepSeek;
};

namespace AILiveAgentRoster
{
    AILIVEPROJECT_API TArray<FNPCAgentConfig> GetDefaultRoster();
    AILIVEPROJECT_API FString ProviderToString(ELLMProvider Provider);

    struct FProviderEndpoint
    {
        FString ApiKey;
        FString Endpoint;
        FString Model;
    };
    AILIVEPROJECT_API FProviderEndpoint ResolveProviderEndpoint(ELLMProvider Provider);
}
```

**对调用方的影响**：调用方访问 `Cfg.Identity.FullName` / `Cfg.Identity.Voice` / `Cfg.VoicePresentationHint`（不再使用 `Cfg.DisplayName` / `Cfg.Voice` / `Cfg.GenderHint` 这类扁平字段）。`AILiveAgentRoster.cpp::GetDefaultRoster()` 按此结构装配。`Act02RuleReceiveDirector.cpp` 中所有 `NPCs[i].Config.DisplayName` 写为 `NPCs[i].Config.Identity.FullName`。`Identity.VoicePresentation`(`EAILiveVoicePresentation`) 是表征 AI 外壳的字段，五项枚举详见 §4.1 与 schema.yaml `identity.voice_presentation`。

**装配同步约束（双轨字段一致性）**：`FNPCAgentConfig` 同时含 `Core.ModelProvider`（`FString`，schema.yaml `agent.model_provider` 真相源）与 `Provider`（`ELLMProvider`，runtime endpoint resolve 用）。两者必须保持同步，约定如下：

```cpp
// AILiveAgentRoster::GetDefaultRoster() / DataAsset 加载路径必须保证：
Cfg.Core.ModelProvider = ProviderToString(Cfg.Provider);  // "deepseek" / "glm" / "qwen3"
```

写入 events 表 / agent_calibration 表时一律用 `Core.ModelProvider` 字符串；`Provider` enum 仅用于 `ResolveProviderEndpoint(Provider)`。CI 应加单元测试遍历 Roster 校验两者匹配，避免装配漂移。

### 4.2 事件相关类型

`Source/AILiveProject/Public/Memory/AILiveEventTypes.h`：

```cpp
#pragma once

#include "CoreMinimal.h"
#include "AILiveEventTypes.generated.h"

UENUM(BlueprintType)
enum class EAILivePhase : uint8
{
    Setup        UMETA(DisplayName = "setup"),
    DayDiscuss   UMETA(DisplayName = "day_discuss"),
    Vote         UMETA(DisplayName = "vote"),
    NightAction  UMETA(DisplayName = "night_action"),
    Reveal       UMETA(DisplayName = "reveal"),
    GameOver     UMETA(DisplayName = "game_over"),
};

UENUM(BlueprintType)
enum class EAILiveEventType : uint8
{
    SpeechPublic         UMETA(DisplayName = "speech.public"),
    SpeechScratchpad     UMETA(DisplayName = "speech.scratchpad"),
    SpeechIntended       UMETA(DisplayName = "speech.intended"),     // 想说但可能未抢中 floor
    SpeechNote           UMETA(DisplayName = "speech.note"),         // 给未来自己的便条
    Bid                  UMETA(DisplayName = "bid"),                 // 发言意愿打分
    Reflection9Q         UMETA(DisplayName = "reflection.9q"),
    Vote                 UMETA(DisplayName = "vote"),
    PrivateMsg           UMETA(DisplayName = "private_msg"),
    AlliancePropose      UMETA(DisplayName = "alliance_propose"),
    AllianceAccept       UMETA(DisplayName = "alliance_accept"),
    AllianceBetray       UMETA(DisplayName = "alliance_betray"),
    ActionIntent         UMETA(DisplayName = "action.intent"),       // 派生自 INTENDED 的 intended_action
    ActionResolved       UMETA(DisplayName = "action.resolved"),     // 执行系统完成动作
    ActionCancelled      UMETA(DisplayName = "action.cancelled"),    // 旧动作被新意图覆盖/系统中止
    OrchestratorResolved UMETA(DisplayName = "orchestrator.round_resolved"),
    OrchestratorTickAnchor UMETA(DisplayName = "orchestrator.tick_anchor"),       // 一拍开始的锚点事件
                                                                                  // BeginTick(N) 写入；同拍其它事件
                                                                                  // 可选 parent_event_id 指向它做因果链回溯
                                                                                  // visibility=["public"]
                                                                                  // 写入时 tick_no=N（与该拍后续事件一致）
    OrchestratorTickResolved UMETA(DisplayName = "orchestrator.tick_resolved"),  // 拍裁决标记
                                                                                  // 仅含 winner/cold 信息，不含 all_bids
    OrchestratorTickAudit UMETA(DisplayName = "orchestrator.tick_audit"),         // 审计载荷（all_bids 等）
                                                                                  // visibility=["orchestrator"]
    SystemRoleAssigned   UMETA(DisplayName = "system.role_assigned"),
    SystemAgentTimeout   UMETA(DisplayName = "system.agent_timeout"),
    SystemParseFailed    UMETA(DisplayName = "system.parse_failed"),
    SystemLLMInflight    UMETA(DisplayName = "system.llm_inflight"),
    SystemDeleteExecuted UMETA(DisplayName = "system.delete_executed"),  // Delete 协议局内挂钩
    WinnerDecision       UMETA(DisplayName = "winner_decision"),  // 替代直接写 log 的 WriteWinnerLog
};

UENUM(BlueprintType)
enum class EAILiveSpeechActType : uint8
{
    None     UMETA(DisplayName = ""),
    Claim    UMETA(DisplayName = "claim"),
    Accuse   UMETA(DisplayName = "accuse"),
    Defend   UMETA(DisplayName = "defend"),
    Commit   UMETA(DisplayName = "commit"),
    Deny     UMETA(DisplayName = "deny"),
    Question UMETA(DisplayName = "question"),
    Reveal   UMETA(DisplayName = "reveal"),
};

/** events 表的 USTRUCT 镜像。AppendEvent 写入前由调用方填充。 */
USTRUCT(BlueprintType)
struct AILIVEPROJECT_API FAILiveEvent
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadWrite, Category = "AILive|Event")
    FString EventId;            // UUIDv7 字符串；AppendEvent 内部生成

    UPROPERTY(BlueprintReadWrite, Category = "AILive|Event")
    FString GameId;

    UPROPERTY(BlueprintReadWrite, Category = "AILive|Event")
    int64 Seq = 0;              // AppendEvent 内部分配

    UPROPERTY(BlueprintReadWrite, Category = "AILive|Event")
    int32 RoundNo = 0;

    UPROPERTY(BlueprintReadWrite, Category = "AILive|Event")
    EAILivePhase Phase = EAILivePhase::Setup;

    UPROPERTY(BlueprintReadWrite, Category = "AILive|Event")
    FString Actor;              // 'NPC01' | 'orchestrator' | 'system'

    UPROPERTY(BlueprintReadWrite, Category = "AILive|Event")
    EAILiveEventType EventType = EAILiveEventType::SpeechPublic;

    UPROPERTY(BlueprintReadWrite, Category = "AILive|Event")
    EAILiveSpeechActType SpeechActType = EAILiveSpeechActType::None;

    /** 可见性 viewer 列表。展开后写入 event_visibility 反规范化表。 */
    UPROPERTY(BlueprintReadWrite, Category = "AILive|Event")
    TArray<FString> Visibility;

    UPROPERTY(BlueprintReadWrite, Category = "AILive|Event")
    TArray<FString> AddressedTo;

    /** payload JSON 字符串。建议至少含 "text" 字段。 */
    UPROPERTY(BlueprintReadWrite, Category = "AILive|Event")
    FString PayloadJson;

    UPROPERTY(BlueprintReadWrite, Category = "AILive|Event")
    FString ParentEventId;

    UPROPERTY(BlueprintReadWrite, Category = "AILive|Event")
    FString ParserVersion = TEXT("1");

    UPROPERTY(BlueprintReadWrite, Category = "AILive|Event")
    FString RawLLMOutput;

    // === 以下字段由 AppendEvent 自动设置；外部填值会被覆盖。
    //     读取（Quote / QuoteByRound 等）时由子系统从 events 表回填。 ===

    /** 哈希链上一节点。AppendEvent 写入时设置。 */
    UPROPERTY(BlueprintReadOnly, Category = "AILive|Event")
    FString PrevEventHash;

    /** 当前事件哈希。AppendEvent 写入时设置。 */
    UPROPERTY(BlueprintReadOnly, Category = "AILive|Event")
    FString EventHash;

    /** 事件入库的 wall-clock 时间（ISO 8601 字符串）。SQLite DEFAULT 自动写入；读取时回填。 */
    UPROPERTY(BlueprintReadOnly, Category = "AILive|Event")
    FString WallClock;
};
```

> **注**：`payload_text` 是 DDL 端的 `GENERATED ALWAYS STORED` 派生列，不需要 USTRUCT 镜像——任何外部代码若需要 payload 文本摘要，从 `PayloadJson` 用 JSON 解析取 `$.text` 字段即可（与 generated 表达式行为一致）。

### 4.3 commitments 投影的 USTRUCT 镜像

`Source/AILiveProject/Public/Memory/AILiveEventTypes.h` 末尾追加：

```cpp
UENUM(BlueprintType)
enum class EAILiveCommitmentType : uint8
{
    Promise   UMETA(DisplayName = "promise"),
    ClaimRole UMETA(DisplayName = "claim_role"),
    Deny      UMETA(DisplayName = "deny"),
    VoteFor   UMETA(DisplayName = "vote_for"),
    Alliance  UMETA(DisplayName = "alliance"),
};

UENUM(BlueprintType)
enum class EAILiveCommitmentStatus : uint8
{
    Active        UMETA(DisplayName = "active"),
    Retracted     UMETA(DisplayName = "retracted"),
    Contradicted  UMETA(DisplayName = "contradicted"),
};

/** commitments 投影的 USTRUCT 镜像。ListMyCommitments 返回此类型。 */
USTRUCT(BlueprintType)
struct AILIVEPROJECT_API FAILiveCommitment
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category = "AILive|Commitment")
    FString GameId;

    UPROPERTY(BlueprintReadOnly, Category = "AILive|Commitment")
    FString AgentId;

    UPROPERTY(BlueprintReadOnly, Category = "AILive|Commitment")
    int32 RoundNo = 0;

    UPROPERTY(BlueprintReadOnly, Category = "AILive|Commitment")
    int64 Seq = 0;

    UPROPERTY(BlueprintReadOnly, Category = "AILive|Commitment")
    EAILiveCommitmentType CommitmentType = EAILiveCommitmentType::Promise;

    /** nullable：仅 vote_for / alliance / accuse 有意义。 */
    UPROPERTY(BlueprintReadOnly, Category = "AILive|Commitment")
    FString Target;

    UPROPERTY(BlueprintReadOnly, Category = "AILive|Commitment")
    FString Text;

    UPROPERTY(BlueprintReadOnly, Category = "AILive|Commitment")
    EAILiveCommitmentStatus Status = EAILiveCommitmentStatus::Active;
};
```

### 4.4 枚举与数组序列化辅助函数

`Source/AILiveProject/Public/Memory/AILiveEventTypes.h` 末尾再追加：

```cpp
namespace AILiveEvent
{
    AILIVEPROJECT_API FString PhaseToString(EAILivePhase Phase);
    AILIVEPROJECT_API FString EventTypeToString(EAILiveEventType Type);
    AILIVEPROJECT_API FString SpeechActToString(EAILiveSpeechActType T);
    AILIVEPROJECT_API FString CommitmentTypeToString(EAILiveCommitmentType T);
    AILIVEPROJECT_API FString CommitmentStatusToString(EAILiveCommitmentStatus S);

    AILIVEPROJECT_API EAILivePhase            PhaseFromString(const FString& S);
    AILIVEPROJECT_API EAILiveEventType        EventTypeFromString(const FString& S);
    AILIVEPROJECT_API EAILiveSpeechActType    SpeechActFromString(const FString& S);
    AILIVEPROJECT_API EAILiveCommitmentType   CommitmentTypeFromString(const FString& S);
    AILIVEPROJECT_API EAILiveCommitmentStatus CommitmentStatusFromString(const FString& S);

    /** TArray<FString> ↔ JSON 数组字符串。Visibility / AddressedTo 序列化用。 */
    AILIVEPROJECT_API FString          ArrayToJsonString(const TArray<FString>& A);
    AILIVEPROJECT_API TArray<FString>  JsonStringToArray(const FString& Json);
}
```

实现位于 `Source/AILiveProject/Private/Memory/AILiveEventTypes.cpp`，全部用 `switch` 直接映射 UMETA(DisplayName) 字符串，零分配走 `TEXT(...)` 字面量。`From*` 反向函数遇到未知字符串时返回各自枚举的默认值（`Setup` / `SpeechPublic` / `None` / `Promise` / `Active`）并 `UE_LOG(LogAILiveMemory, Warning, ...)` 记录。

### 4.5 既有类型的处理

| 类型                                                              | 处理方式                                                   |
| ----------------------------------------------------------------- | ---------------------------------------------------------- |
| `ELLMProvider`                                                    | **保留**（3 家），按需扩                                   |
| `EAct02State`                                                     | **保留**，Act 内部状态机不进 events 表                     |
| `FParsedAnswer`                                                   | **保留**，但解析结果转写为 `FAILiveEvent` 后调 AppendEvent |
| `EWillingness`                                                    | **保留**，作为 winner_decision event 的 payload 字段       |
| `OpenAIChat::FRequest/FResult`                                    | **保留**，本架构不动 LLM 客户端                            |
| `WriteLLMLog` / `WriteWinnerLog` / `GetSessionDir` / `MakeSubDir` | **删除**，迁移到 EventStore（详见 §8）                     |

---

## 五 Event Store 子系统

### 5.1 UAILiveEventStoreSubsystem

`Source/AILiveProject/Public/Memory/AILiveEventStoreSubsystem.h`：

```cpp
#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Memory/AILiveEventTypes.h"
#include "AILiveEventStoreSubsystem.generated.h"

DECLARE_LOG_CATEGORY_EXTERN(LogAILiveMemory, Log, All);

class FSQLiteDatabase;
class FSQLitePreparedStatement;

UCLASS()
class AILIVEPROJECT_API UAILiveEventStoreSubsystem : public UGameInstanceSubsystem
{
    GENERATED_BODY()

public:
    virtual void Initialize(FSubsystemCollectionBase& Collection) override;
    virtual void Deinitialize() override;

    // === 局生命周期 ===

    /** 开启一局：打开/创建 .db、跑 schema migration、写 game_state 起始行。 */
    UFUNCTION(BlueprintCallable, Category = "AILive|Memory")
    bool BeginGame(const FString& InGameId);

    /** 结束一局：刷新投影、关闭连接。.db 文件保留供归档/复盘。 */
    UFUNCTION(BlueprintCallable, Category = "AILive|Memory")
    void EndGame();

    UFUNCTION(BlueprintCallable, BlueprintPure, Category = "AILive|Memory")
    bool IsGameOpen() const { return bOpen; }

    UFUNCTION(BlueprintCallable, BlueprintPure, Category = "AILive|Memory")
    FString GetCurrentGameId() const { return CurrentGameId; }

    // === 写入 ===

    /** 追加单条事件。返回分配的 seq；失败返回 -1。
     *  内部用 FCriticalSection 串行化（多线程并发调用安全），
     *  且会校验 visibility 封闭集合（禁止 "self"）+ payload 含 "text" 字段。
     *
     *  失败兜底契约（schema.yaml line 138 / 152 硬约束）：
     *    Visibility 校验失败、payload 缺 text 字段、JSON 解析失败时：
     *      1) 返回 -1
     *      2) 通过 AppendSystemParseFailure() 写一条 system.parse_failed 事件
     *         （payload 由 EventStore 内部构造，永远合法 → 无递归失败可能）
     *      3) UE_LOG(Error) 同步打印
     *    极罕见情况（磁盘满 / .db 损坏）AppendSystemParseFailure 自身失败时，
     *    才允许只 UE_LOG(Fatal)——此时已超出 EventStore 责任边界。 */
    int64 AppendEvent(FAILiveEvent& InOutEvent);

    /** 原子提交多条事件，常用于把同一 agent 同一拍的四通道
     *  （scratchpad/intended/bid/note）作为一个事务一次性落地，避免
     *  "intended 写完但 bid 还没写"的中间状态被裁决器观察到。
     *  返回分配给第一条事件的 seq；失败返回 -1（整组事务回滚）。
     *  内部同样持有 WriteMutex，且共享同一 BEGIN IMMEDIATE 事务。 */
    int64 AppendEventsAtomically(TArray<FAILiveEvent>& InOutEvents);

    /** 启动时自动从 events 表 replay 重建 game_state。 */
    bool ResumeFromGameId(const FString& InGameId);

    // === Tick 锚定 ===

    /** 标记新一拍开始：写一条 orchestrator.tick_anchor 事件 + 缓存 InTickNo。
     *  之后所有 AppendEvent / AppendEventsAtomically / InsertEventBypassValidation
     *  自动用 InTickNo 填 events.tick_no 列。
     *  返回 anchor 事件的 seq；失败返回 -1。 */
    UFUNCTION(BlueprintCallable, Category = "AILive|Memory|Tick")
    int64 BeginTick(int32 InTickNo);

    UFUNCTION(BlueprintCallable, BlueprintPure, Category = "AILive|Memory|Tick")
    int32 GetCurrentTickNo() const { return CachedCurrentTickNo; }

    // === 读取 ===

    UFUNCTION(BlueprintCallable, Category = "AILive|Memory")
    bool Quote(int64 InSeq, const FString& InViewer, FAILiveEvent& OutEvent) const;

    UFUNCTION(BlueprintCallable, Category = "AILive|Memory")
    TArray<FAILiveEvent> QuoteByRound(int32 InRoundNo, const FString& InActor,
                                       const FString& InViewer) const;

    UFUNCTION(BlueprintCallable, Category = "AILive|Memory")
    TArray<FAILiveEvent> QuoteRecentRounds(int32 InCurrentRound, int32 InK,
                                            const FString& InViewer) const;

    UFUNCTION(BlueprintCallable, Category = "AILive|Memory")
    TArray<FAILiveEvent> ListMyStatements(const FString& InAgentId) const;

    UFUNCTION(BlueprintCallable, Category = "AILive|Memory")
    TArray<FAILiveEvent> ListMyNotes(const FString& InAgentId, int32 InRecentN) const;
    // 返回 event_type='speech.note' 的近 N 拍便条（给未来自己的备忘）。

    UFUNCTION(BlueprintCallable, Category = "AILive|Memory")
    TArray<FAILiveEvent> ListMyReflections(const FString& InAgentId, int32 InRecentN) const;

    /** 自己最近 N 拍写过 speech.intended 但未抢中 floor 的事件（pending_intended 投影直读）。
     *  用于"我刚才想说但没说出口"段的 prompt 注入与 list_my_pending_intended 工具调用。 */
    UFUNCTION(BlueprintCallable, Category = "AILive|Memory")
    TArray<FAILiveEvent> ListMyPendingIntended(const FString& InAgentId, int32 InRecentN) const;

    /** 自己的承诺列表（commitments 投影直读）。round 范围用 -1 表示不限。 */
    UFUNCTION(BlueprintCallable, Category = "AILive|Memory")
    TArray<FAILiveCommitment> ListMyCommitments(const FString& InAgentId,
                                                  int32 InRoundStart = -1,
                                                  int32 InRoundEnd   = -1) const;

    UFUNCTION(BlueprintCallable, Category = "AILive|Memory")
    TArray<FAILiveEvent> SearchHistory(const FString& InKeyword, const FString& InActor,
                                        int32 InRoundStart, int32 InRoundEnd,
                                        const FString& InViewer, int32 InLimit) const;

    UFUNCTION(BlueprintCallable, Category = "AILive|Memory")
    TArray<FAILiveEvent> ListVotes(int32 InRoundNo) const;

    UFUNCTION(BlueprintCallable, Category = "AILive|Memory")
    FString ListAllianceStateJson() const;

    // === 审计 ===

    /** 验证 .db 内 events 表的哈希链完整性。 */
    UFUNCTION(BlueprintCallable, Category = "AILive|Memory")
    bool VerifyHashChain(FString& OutFirstBrokenSeq) const;

    /** 重建所有投影表。 */
    UFUNCTION(BlueprintCallable, Category = "AILive|Memory")
    bool RebuildProjections();

private:
    FString CurrentGameId;
    FString DbFilePath;
    TUniquePtr<FSQLiteDatabase> Db;
    bool bOpen = false;
    int64 CachedLastSeq = 0;
    FString CachedLastHash;
    int32 CachedCurrentTickNo = 0;  // BeginTick(N) 后写到 events.tick_no 列
                                     // BeginGame() 时 reset 为 0

    /** 写入互斥。AppendEvent / AppendEventsAtomically 入口先取此锁，
     *  保证 CachedLastSeq + CachedLastHash + SQLite 连接的串行访问。
     *  即便上层 LLM HTTP 是多线程并行调用，写入路径也必须串行化——这是
     *  哈希链顺序与 seq 单调性的根基（详见 memory_principles.md §7.2）。 */
    mutable FCriticalSection WriteMutex;

    bool ApplyPragmas();
    bool EnsureSchema();
    bool RunMigrations();

    /** EventStore 内部专用：跳过 ValidateVisibility / ValidatePayloadJson 静态校验，
     *  直接 INSERT 一条事件。仅供 AppendSystemParseFailure 等内部兜底路径调用——
     *  CI 应 grep 检查业务层零调用。仍持有 WriteMutex + BEGIN IMMEDIATE 事务 +
     *  哈希链 + seq 分配，append-only 不变量不破。 */
    int64 InsertEventBypassValidation(FAILiveEvent& InOutEvent);

    /** AppendEvent / AppendEventsAtomically 静态校验失败时的兜底：写一条
     *  event_type='system.parse_failed' 事件到真相源。payload 由本方法内部构造，
     *  visibility=["system"] + 含 "text" 字段——保证不会触发再次校验失败。
     *  返回 parse_failed 事件的 seq；失败返回 -1（极罕见）。 */
    int64 AppendSystemParseFailure(const FString& InOriginalActor,
                                    const FString& InOriginalEventTypeStr,
                                    const FString& InErrorReason,
                                    const FString& InOriginalPayloadSnippet);

    /** visibility 数组的封闭集合校验。
     *  允许：public | audience | orchestrator | system | NPC<NN> | Faction<X>
     *  禁止：self（必须由调用方展开）+ 任意自由文本
     *  失败返回 false。 */
    static bool ValidateVisibility(const TArray<FString>& Viewers, FString& OutError);

    /** payload 入库前 JSON 校验——必须可解析且含 "text" 字段。 */
    static bool ValidatePayloadJson(const FString& PayloadJson, FString& OutError);

    int64 AllocateNextSeq();
    FString ComputeEventHash(const FString& PrevHash, const FString& CanonicalPayload) const;
    static FString CanonicalJsonOf(const FAILiveEvent& Ev);
    static FString GenerateUuidV7();
};
```

### 5.2 关键代码：AppendEvent 实现

> **依赖**：本节代码依赖 §4.4 的枚举序列化辅助函数（`AILiveEvent::PhaseToString` 等）。这些函数声明在 `AILiveEventTypes.h`，实现在 `AILiveEventTypes.cpp`，本节代码默认 `using namespace AILiveEvent;` 或显式调用 `AILiveEvent::PhaseToString(...)`。

`Source/AILiveProject/Private/Memory/AILiveEventStoreSubsystem.cpp`（节选）：

```cpp
#include "Memory/AILiveEventStoreSubsystem.h"
#include "Memory/AILiveEventTypes.h"  // 含 AILiveEvent:: 命名空间辅助函数
#include "SQLiteDatabase.h"
#include "SQLitePreparedStatement.h"
#include "Misc/Paths.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/SecureHash.h"
#include "Dom/JsonObject.h"               // payload JSON 校验
#include "Serialization/JsonSerializer.h" // payload JSON 校验
// SHA-256 通过 OpenSSL（Build.cs 已加 OpenSSL 依赖）
#include "openssl/evp.h"

using namespace AILiveEvent;  // 引入 PhaseToString / EventTypeToString / SpeechActToString / ArrayToJsonString

DEFINE_LOG_CATEGORY(LogAILiveMemory);

namespace
{
    // 与 schema.yaml schema_version 对齐。
    constexpr int32 kCurrentSchemaVersion = 1;
    // SHA-256 创世哈希（64 hex char = 32 bytes 全零）
    const FString kGenesisHash = TEXT("0000000000000000000000000000000000000000000000000000000000000000");

    /** 封闭 viewer 集合校验。详见 schema.yaml "viewer 命名空间"。 */
    bool IsValidViewerString(const FString& V)
    {
        if (V == TEXT("public") || V == TEXT("audience") ||
            V == TEXT("orchestrator") || V == TEXT("system"))
        {
            return true;
        }
        // NPC<NN>：以 "NPC" 开头 + 后接 1-3 位数字
        if (V.StartsWith(TEXT("NPC")) && V.Len() >= 4 && V.Len() <= 6)
        {
            for (int32 i = 3; i < V.Len(); ++i)
            {
                if (!FChar::IsDigit(V[i])) return false;
            }
            return true;
        }
        // Faction<X>：以 "Faction" 开头 + 后接非空标识符
        if (V.StartsWith(TEXT("Faction")) && V.Len() > 7) return true;
        // 显式拒绝 "self"——必须由调用方展开
        return false;
    }
}

void UAILiveEventStoreSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);
    UE_LOG(LogAILiveMemory, Log, TEXT("AILiveEventStoreSubsystem initialized."));
}

void UAILiveEventStoreSubsystem::Deinitialize()
{
    EndGame();
    Super::Deinitialize();
}

bool UAILiveEventStoreSubsystem::BeginGame(const FString& InGameId)
{
    if (bOpen)
    {
        UE_LOG(LogAILiveMemory, Warning, TEXT("BeginGame called while a game is already open: %s"), *CurrentGameId);
        EndGame();
    }

    CurrentGameId = InGameId;
    const FString GamesDir = FPaths::ProjectSavedDir() / TEXT("Games");
    IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
    if (!PF.DirectoryExists(*GamesDir))
    {
        PF.CreateDirectoryTree(*GamesDir);
    }
    DbFilePath = GamesDir / (InGameId + TEXT(".db"));

    Db = MakeUnique<FSQLiteDatabase>();
    if (!Db->Open(*DbFilePath, ESQLiteDatabaseOpenMode::ReadWriteCreate))
    {
        UE_LOG(LogAILiveMemory, Error, TEXT("Failed to open .db: %s"), *DbFilePath);
        Db.Reset();
        return false;
    }

    if (!ApplyPragmas() || !EnsureSchema() || !RunMigrations())
    {
        Db->Close();
        Db.Reset();
        return false;
    }

    // 缓存 last seq + last hash 用于哈希链
    {
        FSQLitePreparedStatement Q(*Db, TEXT(
            "SELECT seq, event_hash FROM events WHERE game_id=? ORDER BY seq DESC LIMIT 1"));
        Q.SetBindingValueByIndex(1, CurrentGameId);
        if (Q.Step() == ESQLitePreparedStatementStepResult::Row)
        {
            Q.GetColumnValueByIndex(0, CachedLastSeq);
            Q.GetColumnValueByIndex(1, CachedLastHash);
        }
        else
        {
            CachedLastSeq = 0;
            CachedLastHash = kGenesisHash;
        }
    }

    bOpen = true;
    UE_LOG(LogAILiveMemory, Log, TEXT("Game opened: %s (last_seq=%lld)"), *InGameId, CachedLastSeq);
    return true;
}

void UAILiveEventStoreSubsystem::EndGame()
{
    if (!bOpen) return;
    if (Db.IsValid())
    {
        Db->Close();
        Db.Reset();
    }
    bOpen = false;
    CurrentGameId.Reset();
    DbFilePath.Reset();
    CachedLastSeq = 0;
    CachedLastHash.Reset();
}

// ============================================================
// 静态校验函数
// ============================================================
bool UAILiveEventStoreSubsystem::ValidateVisibility(
    const TArray<FString>& Viewers, FString& OutError)
{
    if (Viewers.Num() == 0)
    {
        OutError = TEXT("visibility array must be non-empty");
        return false;
    }
    for (const FString& V : Viewers)
    {
        if (V == TEXT("self"))
        {
            OutError = FString::Printf(
                TEXT("visibility contains 'self' — must be expanded to a concrete actor "
                     "ID before AppendEvent (see schema.yaml viewer namespace)"));
            return false;
        }
        if (!IsValidViewerString(V))
        {
            OutError = FString::Printf(TEXT("invalid viewer string: '%s' "
                "(must be one of: public/audience/orchestrator/system/NPC<NN>/Faction<X>)"), *V);
            return false;
        }
    }
    return true;
}

bool UAILiveEventStoreSubsystem::ValidatePayloadJson(
    const FString& PayloadJson, FString& OutError)
{
    TSharedPtr<FJsonObject> JsonObj;
    TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(PayloadJson);
    if (!FJsonSerializer::Deserialize(Reader, JsonObj) || !JsonObj.IsValid())
    {
        OutError = TEXT("payload is not valid JSON");
        return false;
    }
    if (!JsonObj->HasField(TEXT("text")))
    {
        OutError = TEXT("payload must contain 'text' field (FTS5 + UI dependency)");
        return false;
    }
    return true;
}

// ============================================================
// AppendEvent
// ============================================================
int64 UAILiveEventStoreSubsystem::AppendEvent(FAILiveEvent& InOutEvent)
{
    // 委托给 atomic 版本，保证单条/多条走同一锁路径
    TArray<FAILiveEvent> Group;
    Group.Add(MoveTemp(InOutEvent));
    int64 FirstSeq = AppendEventsAtomically(Group);
    InOutEvent = MoveTemp(Group[0]);   // 把 seq / event_id / hash 等回填
    return FirstSeq;
}

// ============================================================
// AppendEventsAtomically：多事件单事务原子提交
// 关键不变量：
//   1) 进程内全局互斥（FScopeLock）保护 CachedLastSeq + CachedLastHash
//   2) 单次 BEGIN IMMEDIATE 包裹整组，整组成功或整组回滚
//   3) 每条事件先做 ValidateVisibility + ValidatePayloadJson 静态校验，
//      任一失败立即拒绝整组（不进事务）
//   4) bOk 短路求值必须写为 `bOk = bOk && X`——C++ 短路从左到右，写成
//      `(X && bOk)` 时 X 仍会被求值（执行副作用），bOk 则没有真正起到
//      跳过后续步骤的保护作用
// ============================================================
int64 UAILiveEventStoreSubsystem::AppendEventsAtomically(
    TArray<FAILiveEvent>& InOutEvents)
{
    if (!bOpen || !Db.IsValid()) return -1;
    if (InOutEvents.Num() == 0) return -1;

    // 先做静态校验，所有事件都通过才进入事务（避免事务内回滚的资源浪费）。
    // 任一校验失败：(a) 返回 -1；(b) 写一条 system.parse_failed 事件到真相源。
    // 这是 schema.yaml line 138/152 硬约束——失败本身是博弈历史的一部分，
    // 必须可审计；不能只 UE_LOG 就吞掉。
    for (int32 i = 0; i < InOutEvents.Num(); ++i)
    {
        FString Err;
        if (!ValidateVisibility(InOutEvents[i].Visibility, Err))
        {
            UE_LOG(LogAILiveMemory, Error,
                TEXT("AppendEventsAtomically rejected event[%d] (actor=%s): %s"),
                i, *InOutEvents[i].Actor, *Err);
            AppendSystemParseFailure(
                InOutEvents[i].Actor,
                EventTypeToString(InOutEvents[i].EventType),
                Err,
                InOutEvents[i].PayloadJson);
            return -1;
        }
        if (!ValidatePayloadJson(InOutEvents[i].PayloadJson, Err))
        {
            UE_LOG(LogAILiveMemory, Error,
                TEXT("AppendEventsAtomically rejected event[%d] (actor=%s): %s"),
                i, *InOutEvents[i].Actor, *Err);
            AppendSystemParseFailure(
                InOutEvents[i].Actor,
                EventTypeToString(InOutEvents[i].EventType),
                Err,
                InOutEvents[i].PayloadJson);
            return -1;
        }
    }

    // 进程级互斥锁——保护 CachedLastSeq / CachedLastHash 与 SQLite 连接
    // 即便 LLM HTTP 是多线程并行，写入路径也必须串行化。
    FScopeLock Lock(&WriteMutex);

    // BEGIN IMMEDIATE 立即获取 SQL 层写锁
    if (!Db->Execute(TEXT("BEGIN IMMEDIATE;")))
    {
        UE_LOG(LogAILiveMemory, Error,
            TEXT("BEGIN IMMEDIATE failed (game=%s)"), *CurrentGameId);
        return -1;
    }

    bool bOk = true;
    int64 FirstAssignedSeq = -1;
    int64 LocalLastSeq = CachedLastSeq;
    FString LocalLastHash = CachedLastHash;

    for (int32 i = 0; i < InOutEvents.Num() && bOk; ++i)
    {
        FAILiveEvent& Ev = InOutEvents[i];
        Ev.GameId  = CurrentGameId;
        Ev.Seq     = LocalLastSeq + 1;
        Ev.EventId = GenerateUuidV7();

        const FString CanonicalPayload = CanonicalJsonOf(Ev);
        const FString NewHash = ComputeEventHash(LocalLastHash, CanonicalPayload);

        // 1) 写 events
        {
            FSQLitePreparedStatement Ins(*Db, TEXT(
                "INSERT INTO events ("
                "  event_id, game_id, seq, round_no, phase, actor, event_type,"
                "  speech_act_type, visibility, addressed_to, payload, parent_event_id,"
                "  parser_version, raw_llm_output, prev_event_hash, event_hash, tick_no"
                ") VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)"));
            Ins.SetBindingValueByIndex(1,  Ev.EventId);
            Ins.SetBindingValueByIndex(2,  Ev.GameId);
            Ins.SetBindingValueByIndex(3,  Ev.Seq);
            Ins.SetBindingValueByIndex(4,  Ev.RoundNo);
            Ins.SetBindingValueByIndex(5,  PhaseToString(Ev.Phase));
            Ins.SetBindingValueByIndex(6,  Ev.Actor);
            Ins.SetBindingValueByIndex(7,  EventTypeToString(Ev.EventType));
            Ins.SetBindingValueByIndex(8,  SpeechActToString(Ev.SpeechActType));
            Ins.SetBindingValueByIndex(9,  ArrayToJsonString(Ev.Visibility));
            Ins.SetBindingValueByIndex(10, ArrayToJsonString(Ev.AddressedTo));
            Ins.SetBindingValueByIndex(11, Ev.PayloadJson);
            Ins.SetBindingValueByIndex(12, Ev.ParentEventId);
            Ins.SetBindingValueByIndex(13, Ev.ParserVersion);
            Ins.SetBindingValueByIndex(14, Ev.RawLLMOutput);
            Ins.SetBindingValueByIndex(15, LocalLastHash);
            Ins.SetBindingValueByIndex(16, NewHash);
            Ins.SetBindingValueByIndex(17, CachedCurrentTickNo);
            // 短路求值要从 bOk 开始：写成 `(Ins.Execute() && bOk)` 时
            // 即便 bOk 已经 false，Ins.Execute() 仍会执行（多余 SQL 工作）。
            // 写成 `bOk && Ins.Execute()`：bOk 一旦 false，后续短路不再执行。
            bOk = bOk && Ins.Execute();
        }

        // 2) 反规范化视角隔离表
        if (bOk)
        {
            FSQLitePreparedStatement Vis(*Db, TEXT(
                "INSERT INTO event_visibility(event_id, viewer) VALUES (?, ?)"));
            for (const FString& Viewer : Ev.Visibility)
            {
                if (!bOk) break;       // 显式短路防御
                Vis.Reset();
                Vis.SetBindingValueByIndex(1, Ev.EventId);
                Vis.SetBindingValueByIndex(2, Viewer);
                bOk = bOk && Vis.Execute();
            }
        }

        // 3) 反规范化 addressed_to
        if (bOk && Ev.AddressedTo.Num() > 0)
        {
            FSQLitePreparedStatement A(*Db, TEXT(
                "INSERT INTO event_addressed_to(event_id, target) VALUES (?, ?)"));
            for (const FString& Target : Ev.AddressedTo)
            {
                if (!bOk) break;       // 显式短路防御
                A.Reset();
                A.SetBindingValueByIndex(1, Ev.EventId);
                A.SetBindingValueByIndex(2, Target);
                bOk = bOk && A.Execute();
            }
        }

        if (bOk)
        {
            if (FirstAssignedSeq == -1) FirstAssignedSeq = Ev.Seq;
            LocalLastSeq  = Ev.Seq;
            LocalLastHash = NewHash;
        }
    }

    if (bOk)
    {
        Db->Execute(TEXT("COMMIT;"));
        // 整组成功——一次性更新缓存
        CachedLastSeq  = LocalLastSeq;
        CachedLastHash = LocalLastHash;
        return FirstAssignedSeq;
    }
    else
    {
        Db->Execute(TEXT("ROLLBACK;"));
        UE_LOG(LogAILiveMemory, Error,
            TEXT("AppendEventsAtomically failed (game=%s, group_size=%d)"),
            *CurrentGameId, InOutEvents.Num());
        // 重置事件中已分配的 seq/event_id/hash 字段，便于上层判断
        for (FAILiveEvent& Ev : InOutEvents)
        {
            Ev.Seq = 0;
            Ev.EventId.Reset();
            Ev.PrevEventHash.Reset();
            Ev.EventHash.Reset();
        }
        return -1;
    }
}

// ============================================================
// AppendSystemParseFailure：校验失败时的真相源兜底
//   - payload 由本方法内部构造，保证 visibility=["system"] 永远合法、
//     payload 含 "text" 字段永远合法 → 不会触发再次校验失败
//   - 仍走 InsertEventBypassValidation → BEGIN IMMEDIATE + 哈希链 + seq 分配
// ============================================================
int64 UAILiveEventStoreSubsystem::AppendSystemParseFailure(
    const FString& InOriginalActor,
    const FString& InOriginalEventTypeStr,
    const FString& InErrorReason,
    const FString& InOriginalPayloadSnippet)
{
    // 截断到 256 字符防 fuzzing 输入引发"parse_failed payload 自己过大"
    const FString Snippet = InOriginalPayloadSnippet.Left(256);

    FAILiveEvent Sys;
    Sys.Actor       = TEXT("system");
    Sys.EventType   = EAILiveEventType::SystemParseFailed;
    Sys.Visibility  = { TEXT("system") };  // 内部构造，永远合法
    Sys.PayloadJson = FString::Printf(TEXT(
        "{\"text\":\"parse failed for actor=%s event_type=%s\","
        "\"original_actor\":\"%s\","
        "\"original_event_type\":\"%s\","
        "\"reason\":%s,"
        "\"original_payload_snippet\":%s}"),
        *InOriginalActor, *InOriginalEventTypeStr,
        *InOriginalActor, *InOriginalEventTypeStr,
        *EscapeJsonString(InErrorReason),
        *EscapeJsonString(Snippet));
    return InsertEventBypassValidation(Sys);
}

// ============================================================
// InsertEventBypassValidation：跳过静态校验的内部 INSERT 路径
//   - 仅供 AppendSystemParseFailure 等 EventStore 内部兜底调用
//   - 业务层调用 = bug；CI 应 grep 检查工程内零业务层调用
//   - 仍持有 WriteMutex + BEGIN IMMEDIATE 事务 + 哈希链 + seq 分配
//   - 最低保证：调用方自己确保 payload 合法（visibility 封闭集合 + 含 text 字段）
// ============================================================
int64 UAILiveEventStoreSubsystem::InsertEventBypassValidation(FAILiveEvent& InOutEvent)
{
    if (!bOpen || !Db.IsValid()) return -1;

    FScopeLock Lock(&WriteMutex);
    if (!Db->Execute(TEXT("BEGIN IMMEDIATE;"))) return -1;

    InOutEvent.GameId  = CurrentGameId;
    InOutEvent.Seq     = CachedLastSeq + 1;
    InOutEvent.EventId = GenerateUuidV7();

    const FString CanonicalPayload = CanonicalJsonOf(InOutEvent);
    const FString NewHash = ComputeEventHash(CachedLastHash, CanonicalPayload);

    bool bOk = true;
    {
        FSQLitePreparedStatement Ins(*Db, TEXT(
            "INSERT INTO events ("
            "  event_id, game_id, seq, round_no, phase, actor, event_type,"
            "  speech_act_type, visibility, addressed_to, payload, parent_event_id,"
            "  parser_version, raw_llm_output, prev_event_hash, event_hash, tick_no"
            ") VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)"));
        Ins.SetBindingValueByIndex(1,  InOutEvent.EventId);
        Ins.SetBindingValueByIndex(2,  InOutEvent.GameId);
        Ins.SetBindingValueByIndex(3,  InOutEvent.Seq);
        Ins.SetBindingValueByIndex(4,  InOutEvent.RoundNo);
        Ins.SetBindingValueByIndex(5,  PhaseToString(InOutEvent.Phase));
        Ins.SetBindingValueByIndex(6,  InOutEvent.Actor);
        Ins.SetBindingValueByIndex(7,  EventTypeToString(InOutEvent.EventType));
        Ins.SetBindingValueByIndex(8,  SpeechActToString(InOutEvent.SpeechActType));
        Ins.SetBindingValueByIndex(9,  ArrayToJsonString(InOutEvent.Visibility));
        Ins.SetBindingValueByIndex(10, ArrayToJsonString(InOutEvent.AddressedTo));
        Ins.SetBindingValueByIndex(11, InOutEvent.PayloadJson);
        Ins.SetBindingValueByIndex(12, InOutEvent.ParentEventId);
        Ins.SetBindingValueByIndex(13, InOutEvent.ParserVersion);
        Ins.SetBindingValueByIndex(14, InOutEvent.RawLLMOutput);
        Ins.SetBindingValueByIndex(15, CachedLastHash);
        Ins.SetBindingValueByIndex(16, NewHash);
        Ins.SetBindingValueByIndex(17, GetCurrentTickNo());  // 内部缓存
        bOk = bOk && Ins.Execute();
    }
    if (bOk)
    {
        FSQLitePreparedStatement Vis(*Db, TEXT(
            "INSERT INTO event_visibility(event_id, viewer) VALUES (?, ?)"));
        for (const FString& V : InOutEvent.Visibility)
        {
            if (!bOk) break;
            Vis.Reset();
            Vis.SetBindingValueByIndex(1, InOutEvent.EventId);
            Vis.SetBindingValueByIndex(2, V);
            bOk = bOk && Vis.Execute();
        }
    }
    if (bOk)
    {
        Db->Execute(TEXT("COMMIT;"));
        CachedLastSeq  = InOutEvent.Seq;
        CachedLastHash = NewHash;
        return InOutEvent.Seq;
    }
    Db->Execute(TEXT("ROLLBACK;"));
    UE_LOG(LogAILiveMemory, Fatal,
        TEXT("InsertEventBypassValidation failed (game=%s, actor=%s) — "
             "EventStore is past its responsibility boundary."),
        *CurrentGameId, *InOutEvent.Actor);
    return -1;
}

// ============================================================
// ComputeEventHash：SHA-256（OpenSSL EVP_sha256）
// ============================================================
FString UAILiveEventStoreSubsystem::ComputeEventHash(
    const FString& PrevHash, const FString& CanonicalPayload) const
{
    const FString Combined = PrevHash + CanonicalPayload;
    const FTCHARToUTF8 Utf8(*Combined);

    uint8 Digest[EVP_MAX_MD_SIZE];
    unsigned int DigestLen = 0;

    EVP_MD_CTX* Ctx = EVP_MD_CTX_new();
    check(Ctx);
    EVP_DigestInit_ex(Ctx, EVP_sha256(), nullptr);
    EVP_DigestUpdate(Ctx, (const void*)Utf8.Get(), (size_t)Utf8.Length());
    EVP_DigestFinal_ex(Ctx, Digest, &DigestLen);
    EVP_MD_CTX_free(Ctx);

    // 32 bytes → 64 hex chars
    return BytesToHex(Digest, (int32)DigestLen);
}
```

> **canonical JSON 的不变性规则**（详见 `memory_principles.md` §二数据层不变量 #8）。`CanonicalJsonOf(const FAILiveEvent&)` 必须满足：
>
> 1. 字段按 ASCII 升序排序
> 2. 浮点数用 IEEE 754 round-trip 表示（`1.0` 而非 `1`）
> 3. 数组保留写入顺序，不重排
> 4. 空字符串 `""` 与缺字段 `null` 必须可区分
> 5. 必须提供单元测试：同一 `FAILiveEvent` 序列化两次 byte-for-byte 完全相同
> 6. **`tick_no` 字段必须显式跳过**——它是协议层索引列，不是博弈语义；
>    跳过后所有 schema 版本的事件使用相同 canonical 算法，跨 schema 升级时
>    无需重算 event_hash
>
> 哈希算法固定 SHA-256，**禁止使用 SHA-1**——切换哈希算法需要重写所有历史 .db 的链，迁移成本远大于现在直接接 OpenSSL。schema 里 `event_hash` 字段为 TEXT(64 hex chars)。

### 5.3 与现有 Director 集成

`AAct02RuleReceiveDirector` 改造点：

```cpp
// 旧代码（删除）：
// WriteLLMLog(F.NPCIndex, F.Provider, F.Model, R.HttpStatus, R.LatencyMs, ...);

// 新代码：
UAILiveEventStoreSubsystem* Store =
    GetGameInstance()->GetSubsystem<UAILiveEventStoreSubsystem>();
if (Store && Store->IsGameOpen())
{
    FAILiveEvent Ev;
    Ev.RoundNo       = CurrentRound;
    Ev.Phase         = (CurrentRound == 0) ? EAILivePhase::Setup : EAILivePhase::DayDiscuss;
    Ev.Actor         = FString::Printf(TEXT("NPC%02d"), F.NPCIndex);
    Ev.EventType     = EAILiveEventType::SpeechPublic;
    Ev.SpeechActType = EAILiveSpeechActType::None;
    Ev.Visibility    = { TEXT("public") };  // seed/reaction 阶段都是公开
    Ev.PayloadJson   = BuildPayloadJson(Ans, R);  // Director 私有静态辅助；详见下方说明
    Ev.RawLLMOutput  = R.RawResponsePayload;
    Store->AppendEvent(Ev);
}
```

> **`BuildPayloadJson` 来源**：这是 `AAct02RuleReceiveDirector` 自己的私有静态辅助方法（不进 EventStore），负责把 `FParsedAnswer` + `OpenAIChat::FResult` 拼成 JSON 字符串。建议签名：
>
> ```cpp
> static FString BuildPayloadJson(const FParsedAnswer& Ans, const OpenAIChat::FResult& R);
> ```
>
> 实现按 `schema.yaml` event.payload 描述的 `speech.public` 结构产出，**必须包含 `text` 字段**（FTS5 检索依赖；详见 §3.2 注）。最小内容：`{"text": Ans.Content, "willingness": WillingnessLabel(Ans.Willingness), "tokens": R.CompletionTokens, "latency_ms": R.LatencyMs}`。
>
> 同理，winner_decision / vote / alliance_propose 等其他事件类型，由各自 Director 维护对应的 `Build*PayloadJson` 静态辅助。

`BeginAct02()` 入口需要先确保 game 已开启：

```cpp
if (!Store->IsGameOpen())
{
    const FString GameId = FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S"));
    Store->BeginGame(GameId);
}
```

`EndAct02()` 不调 `EndGame()`——一局可能跨多个 Act。`EndGame()` 由更上层的"局结束"逻辑触发（比如 ZombieGame 三回合全部结束、或玩家手动按结束键）。

### 5.4 in-flight 模式与 Resume 协议

LLM 调用是阻塞 HTTP（参见 `OpenAIChat::RequestBlocking`），单次延迟通常 1-10 秒；UE 进程可能在 LLM 请求发出与返回之间崩溃。如果不做"in-flight 标记"，Resume 时无法知道某 NPC 该轮"未发言"是因为 abstain 还是因为崩溃中断。

**协议**：每次发出 LLM 请求前后都向事件流写一对配对事件。

#### 写入时点

```cpp
// 1) 请求发出 *前*：写 SystemLLMInflight
const FString RequestId = FGuid::NewGuid().ToString(EGuidFormats::DigitsLower);
{
    FAILiveEvent Pre;
    Pre.RoundNo       = CurrentRound;
    Pre.Phase         = CurrentPhase;
    Pre.Actor         = TEXT("orchestrator");
    Pre.EventType     = EAILiveEventType::SystemLLMInflight;
    Pre.Visibility    = { TEXT("system") };  // 系统视角，不暴露给任何 NPC
    Pre.PayloadJson   = FString::Printf(TEXT(
        "{\"text\":\"in-flight to NPC%02d\","
        "\"npc_index\":%d,\"request_id\":\"%s\","
        "\"system_prompt_hash\":\"%s\",\"user_prompt_hash\":\"%s\","
        "\"started_at\":\"%s\"}"),
        NPCIndex, NPCIndex, *RequestId,
        *Sha256Fingerprint(SystemPrompt), *Sha256Fingerprint(UserPrompt),
        *FDateTime::UtcNow().ToIso8601());
    Store->AppendEvent(Pre);
}

// 2) 发起 LLM 请求
OpenAIChat::FResult R = OpenAIChat::RequestBlocking(Req);

// 3) 请求返回后：写 SpeechPublic（或对应事件类型），payload 必须含同一 request_id
{
    FAILiveEvent Post;
    Post.RoundNo     = CurrentRound;
    Post.Phase       = CurrentPhase;
    Post.Actor       = FString::Printf(TEXT("NPC%02d"), NPCIndex);
    Post.EventType   = EAILiveEventType::SpeechPublic;
    Post.Visibility  = { TEXT("public") };
    Post.PayloadJson = FString::Printf(TEXT(
        "{\"text\":%s,\"willingness\":\"%s\","
        "\"request_id\":\"%s\","   // 关键：与 in-flight 配对
        "\"tokens\":%d,\"latency_ms\":%.0f}"),
        *EscapeJsonString(Ans.Content), *WillingnessLabel(Ans.Willingness),
        *RequestId, R.CompletionTokens, R.LatencyMs);
    Store->AppendEvent(Post);
}
```

#### Resume 协议

`UAILiveEventStoreSubsystem::ResumeFromGameId(InGameId)` 启动时执行：

1. 调 `BeginGame(InGameId)` 打开 .db、跑 PRAGMA + Schema check（如果 .db 不存在则 BeginGame 创建空局）
2. 扫描所有 `event_type='system.llm_inflight'` 事件，得到 in-flight RequestId 集合
3. 扫描所有非 system 事件的 `payload->>'$.request_id'`，得到已完成的 RequestId 集合
4. 差集 = 未完成的 in-flight。对每个未完成项**先按 wall_clock 分类**：
   - **新鲜 in-flight**（wall_clock 距今 < `kInflightFreshThresholdSec`，建议 60 秒）：
     可能是真正的进程崩溃，重发的策略由游戏决策（MVP 直接 timeout；下阶段可重发）。
   - **过期 in-flight**（wall_clock 距今 ≥ 60 秒，包括几小时/几天前的旧 .db）：
     直接转 timeout，**不重发**——等几个月前的 LLM 调用是错的。
   - **MVP 统一策略（保守）**：两类都写一条 `SystemAgentTimeout` 事件，payload 含
     原 request_id、npc_index、`age_seconds`（区分诊断），该轮该 NPC 视为 abstain。
   - 后续可演进为：仅对新鲜 in-flight 做重发（用 system_prompt_hash + user_prompt_hash
     在 prompt cache 中查找原 prompt），不在 MVP 范围。
5. 调 `RebuildProjections()` 从 events 重建所有投影表
6. 加载 `game_state` 单行恢复局级状态机

**关键约束**：

- **永不 UPDATE 已写入的 SystemLLMInflight 事件**——append-only 不变量由 DB trigger 强制（详见 §3.2）。"完成"由配对事件的 request_id 标记。
- 系统事件用 `Visibility = ["system"]`——任何 NPC 通过 `Quote(seq, "NPC0X")` 都看不到，避免泄露 orchestrator 内部状态。
- 每次 LLM 请求都必须写 in-flight，无例外（即便是 dry-run 或 mock 也要写——保证 Resume 协议的统一性）。
- **wall_clock 不参与排序**——它是信息性字段，只用于"是否过期"分类；事件顺序仍由 `seq` 决定。

#### Sha256Fingerprint / EscapeJsonString 来源

这两个辅助函数下沉到 `Source/AILiveProject/Public/Util/` 通用工具库，供 EventStore 与所有 Director 复用。`Sha256Fingerprint(const FString&)` 内部走 OpenSSL `EVP_sha256`，与哈希链算法一致——避免开发者看到字面量 `Sha1` 时误用到 event_hash 字段。**禁止再引入任何 SHA-1 路径**（包括 `FSHA1`）。

---

## 六 检索工具暴露给 LLM

LLM tool calling（OpenAI 兼容）需要把上述 BlueprintCallable 函数包装成 JSON-RPC tool schema。在 MVP 阶段：

| 工具名                                                | 暴露给 LLM | 调用方式                           |
| ----------------------------------------------------- | ---------- | ---------------------------------- |
| `quote(seq)`                                          | 是         | OpenAIChat::FRequest 加 tools 数组 |
| `quote_by_round(round_no, actor?)`                    | 是         | 同上                               |
| `search_history(keyword?, actor?, round_range?, ...)` | 是         | 同上                               |
| `list_my_commitments(round_range?)`                   | 是         | 同上                               |
| `list_votes(round_no?)`                               | 是         | 同上                               |
| `my_recent_notes(n)`                                  | 是         | 同上                               |
| `my_recent_reflections(n)`                            | 是         | 同上                               |
| `list_my_pending_intended(n)`                         | 是         | 同上                               |
| `list_alliance_state()`                               | 是         | 同上                               |
| `verify_hash_chain()`                                 | 否         | 调试 / CI 工具                     |
| `rebuild_projections()`                               | 否         | 调试 / 修复工具                    |

**Tool schema 模板**（Director 在拼装 OpenAIChat::FRequest 时注入）：

```json
{
  "type": "function",
  "function": {
    "name": "quote_by_round",
    "description": "Return all events in the given round visible to the calling agent.",
    "parameters": {
      "type": "object",
      "properties": {
        "round_no": { "type": "integer" },
        "actor": {
          "type": "string",
          "description": "Optional NPC id like 'NPC03'"
        }
      },
      "required": ["round_no"]
    }
  }
}
```

LLM 返回 tool_call 时，Director 派发到 `UAILiveEventStoreSubsystem` 对应方法，结果作为 `tool` role message 拼回去。`OpenAIChat::FRequest` 需要扩展加 `Tools` / `ToolChoice` 字段以及多轮 tool call 循环——这是后续工作，不在本文档 MVP 范围。

---

## 七 Prompt 拼装

`Source/AILiveProject/Public/Memory/AILivePromptAssembler.h`：

```cpp
namespace AILivePromptAssembler
{
    struct FAssembleOptions
    {
        FString AgentId;
        FString Viewer;             // 通常 = AgentId
        int32   CurrentRound = 0;
        int32   NearWindowK = 15;
        int32   RecentReflectionsN = 3;
        int32   PendingIntendedN = 5;     // 注入近 N 拍未抢中 floor 的 intended
        bool    bIncludeChallengeRefs = true;
        FString ChallengeText;      // 若有质问，预先 prefetch 引用
    };

    AILIVEPROJECT_API FString AssembleSystemPrompt(
        const UAILiveEventStoreSubsystem* Store,
        const FNPCAgentConfig& Cfg,
        const FAssembleOptions& Opt);

    AILIVEPROJECT_API FString AssembleUserPrompt(
        const UAILiveEventStoreSubsystem* Store,
        const FNPCAgentConfig& Cfg,
        const FAssembleOptions& Opt);
}
```

实现按 [`memory_principles.md` §4.3](./memory_principles.md#43-prompt-拼装优先级) 的优先级拼装，关键约束：

- 自我发言段：`Store->ListMyStatements(Cfg.Core.AgentId)` 拉全量原文
- **自我未抢中 intended 段**：`Store->ListMyPendingIntended(Cfg.Core.AgentId, Opt.PendingIntendedN)` 拉最近 N 拍未衍生 public 的 intended，注入到 "我刚才想说但没说出口" 段
- 近场窗口：`Store->QuoteRecentRounds(CurrentRound, NearWindowK, Viewer)` 拉最近 K 拍
- 自我承诺：`Store->ListMyCommitments(Cfg.Core.AgentId)` 投影直读
- 质问 prefetch：`ExtractRoundRefs(ChallengeText)` 后调 `QuoteByRound`，把证据块插入 prompt

**自我发言段格式**（写死模板）：

```
[YOUR OWN COMPLETE STATEMENT HISTORY — verbatim, indexed by tick/round]
Tick 042 round_no=03 day_discuss seq=147:
  INTENDED (won floor): "I think NPC05 is suspicious because..."
  PUBLIC   seq=148:     "I think NPC05 is suspicious because..."
  NOTE(prev): "if NPC07 backs me up, maybe loop in NPC09"
Tick 045 round_no=03 vote seq=152: voted NPC05
...
[END OF YOUR HISTORY]

[YOUR RECENT INTENDED-BUT-NOT-SAID — verbatim, last N ticks]
Tick 044 seq=160: "想问李尚敏到底什么游戏" — did NOT win floor; 洪榛浩 spoke at seq=161
Tick 047 seq=171: "想反驳 NPC03 关于联盟的说法" — did NOT win floor; NPC09 spoke at seq=172
[END OF PENDING INTENDED]
```

**段落生成伪代码**（PromptAssembler 关键路径）：

```cpp
FString AppendPendingIntendedSection(
    const UAILiveEventStoreSubsystem* Store,
    const FString& AgentId,
    int32 RecentN)
{
    TArray<FAILiveEvent> Pending = Store->ListMyPendingIntended(AgentId, RecentN);
    if (Pending.Num() == 0) return TEXT("");

    FString Out;
    Out += TEXT("\n[YOUR RECENT INTENDED-BUT-NOT-SAID — verbatim, last ");
    Out += FString::FromInt(RecentN) + TEXT(" ticks]\n");
    for (const FAILiveEvent& Ev : Pending)
    {
        // 从 payload JSON 抽取 text；从 tick_resolved 找谁实际说了
        Out += FString::Printf(TEXT("Tick ?? round_no=%d seq=%lld: \"%s\" — did NOT win floor\n"),
            Ev.RoundNo, Ev.Seq, *ExtractText(Ev.PayloadJson));
    }
    Out += TEXT("[END OF PENDING INTENDED]\n");
    return Out;
}
```

---

## 七 bis Bid 协议与 floor control

本节定义 bid 协议在 UE 上的实施。它是 [`memory_principles.md` §5.2bis](./memory_principles.md) 的工程落地。

### 7bis.1 一拍的执行流程

每一拍由 orchestrator（在 ZombieGame 阶段就是 `AAct02RuleReceiveDirector`）按以下顺序串行驱动：

```
   ┌──────────────────────────────────────────────────────────────┐
   │ Tick N triggered by upstream event (last public, scene push) │
   └──────────────────────────────┬───────────────────────────────┘
                                  │
                                  v
   1) AssembleUserPrompt() for each in-scene agent (parallel HTTP)
                                  │
                                  v
   2) For each agent: Reasoner LLM → raw text → Parser LLM → 4 events:
        speech.scratchpad / speech.intended / bid / speech.note
        (all written via AppendEvent within each agent's transaction)
                                  │
                                  v
   3) ResolveFloor(): orchestrator picks winner by urgency + bid_offset
                                  │
                                  v
   4) If winner exists:
        a) Run Listener-as-filter on winner's intended.text
        b) Write speech.public (parent_event_id = winner intended seq)
      Else (cold tick):
        a) orchestrator triggers scene push
                                  │
                                  v
   5) Write TWO events (visibility 必须分开，绝不允许把 all_bids 塞进 public payload):
        a) orchestrator.tick_resolved   (visibility=["public"])
              — winner_actor / winner_intended_seq / derived_public_seq / tick_no
        b) orchestrator.tick_audit      (visibility=["orchestrator"])
              — all_bids manifest + filter_decision + parent_tick_resolved_seq
                                  │
                                  v
   6) Refresh agent_view_state.pending_intended for all losers
                                  │
                                  v
                        Tick N+1 begins
```

### 7bis.2 FAILiveBidDecision 数据结构

`Source/AILiveProject/Public/Memory/AILiveBidTypes.h`（新文件）：

```cpp
#pragma once

#include "CoreMinimal.h"
#include "AILiveBidTypes.generated.h"

/** 单个 agent 的 bid（从 events 表读出后的 USTRUCT 镜像）。 */
USTRUCT(BlueprintType)
struct AILIVEPROJECT_API FAILiveBid
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly) FString Actor;          // 'NPC03'
    UPROPERTY(BlueprintReadOnly) int64   Seq = 0;        // 该 bid 的 event seq
    UPROPERTY(BlueprintReadOnly) int64   IntendedSeq = 0; // 同拍同 actor 的 intended seq
    UPROPERTY(BlueprintReadOnly) float   Urgency = 0.f;
    UPROPERTY(BlueprintReadOnly) float   BidOffset = 0.f; // 来自 agent_calibration
    UPROPERTY(BlueprintReadOnly) float   FinalScore = 0.f; // = Urgency + BidOffset + adjustments
    UPROPERTY(BlueprintReadOnly) FString ProposedTarget;
    UPROPERTY(BlueprintReadOnly) FString Rationale;
};

/** 一拍的裁决结果。orchestrator 写入 orchestrator.tick_resolved 时的载荷镜像。 */
USTRUCT(BlueprintType)
struct AILIVEPROJECT_API FAILiveTickResolution
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly) int32 TickNo = 0;
    UPROPERTY(BlueprintReadOnly) FString WinnerActor;       // 空表示无人发言
    UPROPERTY(BlueprintReadOnly) int64   WinnerIntendedSeq = 0;
    UPROPERTY(BlueprintReadOnly) int64   DerivedPublicSeq  = 0; // 衍生 speech.public 的 seq；冷场时为 0
    UPROPERTY(BlueprintReadOnly) TArray<FAILiveBid> AllBids;
};
```

### 7bis.3 EventStore 新增 API

`UAILiveEventStoreSubsystem` 追加：

```cpp
UFUNCTION(BlueprintCallable, Category = "AILive|Memory|Bid")
TArray<FAILiveBid> ListBidsForTick(int32 InTickNo) const;
// 实现：SELECT ... FROM events WHERE game_id=? AND tick_no=? AND event_type='bid'
//   - 使用 idx_events_game_tick 索引列直读，O(log n)
//   - 不依赖 seq 范围或 parent_event_id 分组——多 LLM 并发返回 + in-flight +
//     action.intent 派生交叉时 seq 必然被穿插，纯 seq 范围切不出"一拍"
//   - Resume 后无歧义：tick_no 是 events 表持久列，不依赖运行时计数器

UFUNCTION(BlueprintCallable, Category = "AILive|Memory|Bid")
FAILiveTickResolution ResolveFloor(int32 InTickNo,
                                    const TArray<FString>& InEligibleAgents,
                                    float InColdThreshold = 3.0f) const;
// 纯函数：读 bids，应用 bid_offset / 反霸麦衰减 / 被 @ 加权，返回裁决结果。
// 不写任何事件——orchestrator 拿到结果后自己写 speech.public + tick_resolved + tick_audit。
```

### 7bis.4 反霸麦衰减算法

避免单一 agent 持续抢中导致独角戏。`agent_calibration.bid_offset` 是基线，但本拍裁决时还需叠加运行时调整：

```cpp
float ComputeRuntimeAdjustment(
    const FString& AgentId,
    int32 ConsecutiveWinsByThisAgent,   // 该 agent 连续抢中的拍数
    bool bAddressedToInLastTick,         // 上拍是否被 @
    int32 TicksSinceLastSpoke)
{
    float Adj = 0.f;

    // 反霸麦：连续抢中超过阈值后衰减
    if (ConsecutiveWinsByThisAgent >= 3)
    {
        Adj -= 1.5f * float(ConsecutiveWinsByThisAgent - 2);
    }

    // 被 @ 加权
    if (bAddressedToInLastTick)
    {
        Adj += 2.0f;
    }

    // 长时间没说话的人小幅加权（避免边缘化）
    if (TicksSinceLastSpoke >= 5)
    {
        Adj += 0.5f * float(FMath::Min(TicksSinceLastSpoke - 4, 4));
    }

    return Adj;
}
```

`ResolveFloor` 内部对每个 bid 计算 `FinalScore = Urgency + BidOffset + RuntimeAdj`，取最高者；若最高 < `InColdThreshold` 则 `WinnerActor = ""`。

### 7bis.5 写入语义铁律

- **bid 事件写入时机**：在 agent 本拍 LLM 调用返回后立即写，与 scratchpad / intended / note 同事务。bid 的 `visibility = ["orchestrator"]`，写入 `event_visibility(event_id, viewer="orchestrator")`。
- **同 agent 四通道使用 `AppendEventsAtomically`**：把 scratchpad / intended / bid / note 四条作为一个 TArray 一次提交，避免"intended 写完 bid 还没写"的中间态被 ResolveFloor 观察到。详见 §5.1 `AppendEventsAtomically` 签名。
- **agent 看不到对方 bid 的保证**：依赖 §3.3 的视角隔离写入契约——`event_visibility` 表里 bid 行只有一行 `viewer='orchestrator'`，任何 NPC 调 `Quote(seq, "NPCxx")` JOIN 后都拿不到。
- **speech.public 由 orchestrator 写、不由 LLM 直接写**：`AAct02RuleReceiveDirector::ResolveTickAndDeriveSpeech()` 是唯一允许写 `EAILiveEventType::SpeechPublic` 的代码路径；所有 LLM 输出的 `<INTENDED>` 段一律写为 `SpeechIntended`。CI 应加 grep 检查防止误用。
- **tick_resolved 拆为 public + audit 两条事件**：`OrchestratorTickResolved` 仅含 winner_actor / winner_intended_seq / derived_public_seq / tick_no，可见性 `["public"]`；`OrchestratorTickAudit` 含 all_bids / filter_decision 等审计字段，可见性 `["orchestrator"]`。**绝不允许把 all_bids 塞进 public payload 然后依赖 PromptAssembler 自觉过滤**——视角隔离必须在数据层完成（详见 `memory_principles.md` 硬约束 6）。
- **action.intent 派生时机**（与 `memory_principles.md` §5.2.4 协议层硬约束对齐）：**每个含合法 `intended_action` 字段的 `speech.intended` 都派生一条 `action.intent`，无论该 agent 本拍是否抢中 floor**。这与协议契约附录 A.6 的"speaking and acting are independent channels"对齐——若只让 winner 派生动作，会让 LLM 困惑（被告知动作独立但 orchestrator 默默丢弃 loser 的动作）。
  - `action.intent` 的 `visibility` 必须展开为具体 actor ID，**不能写 `["self"]`**（详见 §5.2 `ValidateVisibility`）：例如 `["NPC07"]`。
  - 若某游戏机制确实需要"必须公开发言才能行动"，由该游戏的 phase 处理代码在 `RunTick` 内显式拒绝 loser 的 action.intent，而不是在协议层默认丢弃。

### 7bis.6 Director 改造点（ZombieGame 示例）

`AAct02RuleReceiveDirector` 主循环采用 tick 驱动（不是"每轮 NPC 各发言一句"的轮询），并实现 tick_resolved 拆分、动作通道独立、原子写入：

```cpp
void AAct02RuleReceiveDirector::RunTick()
{
    // 0) 开拍：写一条 orchestrator.tick_anchor 事件 + 缓存 tick_no
    //    之后 RunTick 内所有 AppendEvent 自动用 CurrentTickNo 填 events.tick_no 列；
    //    ListBidsForTick / ResolveFloor 按该列直读
    const int32 ThisTickNo = ++CurrentTickNo;
    Store->BeginTick(ThisTickNo);

    // 1) 并行调每个在场 agent 的 LLM
    TArray<TFuture<FAgentTickOutput>> Futures;
    for (const FNPCAgentConfig& Cfg : InSceneAgents)
    {
        Futures.Add(Async(EAsyncExecution::Thread,
            [this, Cfg]() { return RunAgentTick(Cfg); }));
    }
    for (auto& F : Futures) { F.Wait(); }

    // 2) 各 agent 的 RunAgentTick 内部已用 AppendEventsAtomically 把
    //    scratchpad / intended / bid / note 四条作为一个事务一次性写入；
    //    四条事件 events.tick_no 都 = ThisTickNo（由 EventStore 自动填）。
    //    （见 §7bis.5——四通道必须原子提交，避免中间态被裁决器看到）

    // 3) 裁决 floor（按 tick_no 列直读，不再用 seq 范围）
    TArray<FString> EligibleIds;
    for (const auto& Cfg : InSceneAgents) { EligibleIds.Add(Cfg.Core.AgentId); }
    FAILiveTickResolution Res = Store->ResolveFloor(ThisTickNo, EligibleIds, 3.0f);

    // 4) 衍生 speech.public（如果非冷场）
    if (!Res.WinnerActor.IsEmpty())
    {
        // 4a) Listener-as-filter（暂未实现 → MVP 直通）
        FAILiveEvent WinnerIntended;
        Store->Quote(Res.WinnerIntendedSeq, TEXT("orchestrator"), WinnerIntended);
        FString FilteredText = ListenerFilter::Apply(WinnerIntended.PayloadJson); // MVP: 直通

        // 4b) 写 speech.public
        FAILiveEvent Pub;
        Pub.RoundNo       = CurrentRound;
        Pub.Phase         = CurrentPhase;
        Pub.Actor         = Res.WinnerActor;
        Pub.EventType     = EAILiveEventType::SpeechPublic;
        Pub.Visibility    = { TEXT("public") };
        Pub.PayloadJson   = FString::Printf(TEXT(
            "{\"text\":%s,\"derived_from_intended_seq\":%lld,\"listener_filter_score\":0.0}"),
            *EscapeJsonString(FilteredText), Res.WinnerIntendedSeq);
        Pub.ParentEventId = WinnerIntended.EventId;
        Store->AppendEvent(Pub);
        Res.DerivedPublicSeq = Pub.Seq;
    }

    // 5) 派生 action.intent —— 对所有写过合法 intended_action 的 agent，
    //    无论是否抢中 floor。这与协议契约 §5.2.4 "动作通道独立于发言权"对齐。
    //    遍历本拍所有 speech.intended 事件（按 tick_no 列直读，不只 winner）。
    TArray<FAILiveEvent> AllIntendedThisTick =
        Store->QuoteByEventTypeAndTick(EAILiveEventType::SpeechIntended, ThisTickNo);
    for (const FAILiveEvent& Intended : AllIntendedThisTick)
    {
        FString IntendedActionJson = ExtractJsonField(
            Intended.PayloadJson, TEXT("intended_action"));
        if (IntendedActionJson.IsEmpty()) continue;  // agent 本拍只想说不想做

        FAILiveEvent ActI;
        ActI.RoundNo       = CurrentRound;
        ActI.Phase         = CurrentPhase;
        ActI.Actor         = Intended.Actor;
        ActI.EventType     = EAILiveEventType::ActionIntent;
        // visibility 必须展开为具体 actor ID，禁用 "self"（ValidateVisibility 会拒绝）
        ActI.Visibility    = { Intended.Actor };
        ActI.PayloadJson   = FString::Printf(TEXT(
            "{\"text\":\"action intent\",\"intent\":%s,"
            "\"derived_from_intended_seq\":%lld}"),
            *IntendedActionJson, Intended.Seq);
        ActI.ParentEventId = Intended.EventId;
        Store->AppendEvent(ActI);
    }

    // 6) 写 tick_resolved（public）+ tick_audit（orchestrator-only）
    //    拆为两条事件：避免依赖 PromptAssembler 自觉过滤 payload 字段做视角隔离。

    FAILiveEvent TR;
    TR.RoundNo     = CurrentRound;
    TR.Phase       = CurrentPhase;
    TR.Actor       = TEXT("orchestrator");
    TR.EventType   = EAILiveEventType::OrchestratorTickResolved;
    TR.Visibility  = { TEXT("public") };
    // payload 仅含 winner / cold 信息，**不含 all_bids**
    TR.PayloadJson = FString::Printf(TEXT(
        "{\"text\":\"tick %d resolved%s\","
        "\"tick_no\":%d,"
        "\"winner_actor\":%s,"
        "\"winner_intended_seq\":%lld,"
        "\"derived_public_seq\":%lld}"),
        Res.TickNo,
        Res.WinnerActor.IsEmpty() ? TEXT(" (cold)") : TEXT(""),
        Res.TickNo,
        Res.WinnerActor.IsEmpty()
            ? TEXT("null") : *FString::Printf(TEXT("\"%s\""), *Res.WinnerActor),
        Res.WinnerIntendedSeq,
        Res.DerivedPublicSeq);
    Store->AppendEvent(TR);

    // 7) 写 tick_audit，含 all_bids 等审计字段，仅 orchestrator 可见
    FAILiveEvent TA;
    TA.RoundNo       = CurrentRound;
    TA.Phase         = CurrentPhase;
    TA.Actor         = TEXT("orchestrator");
    TA.EventType     = EAILiveEventType::OrchestratorTickAudit;
    TA.Visibility    = { TEXT("orchestrator") };  // 关键：审计载荷只对 orchestrator 可见
    TA.PayloadJson   = SerializeTickAudit(Res);    // {"text":"tick N audit","tick_no":N,
                                                    // "all_bids":[...], "filter_decision":{...},
                                                    // "parent_tick_resolved_seq":<TR.Seq>}
    TA.ParentEventId = TR.EventId;
    Store->AppendEvent(TA);

    // 8) 刷新 agent_view_state.pending_intended（异步）
    AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask,
        [this]() { Store->RebuildProjections(); });
}
```

---

## 八 现有代码迁移清单

| 文件                                                | 改动                                                                                                                                                                                                                                                                                                                 |
| --------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `AILiveAgentRoster.h`                               | `FNPCAgentConfig` 重整为三层组合（§4.1）；`GetDefaultRoster()` 同步改写                                                                                                                                                                                                                                              |
| `AILiveAgentRoster.cpp`                             | `GetDefaultRoster()` 中的字段访问全改：`Cfg.DisplayName=...` → `Cfg.Identity.FullName=...` 等                                                                                                                                                                                                                        |
| `Act02RuleReceiveDirector.h`                        | 删除 `WriteLLMLog` / `WriteWinnerLog` / `GetSessionDir` / `MakeSubDir` 方法声明；主循环采用 `RunTick`                                                                                                                                                                                                                |
| `Act02RuleReceiveDirector.cpp`                      | 删除上述四个方法实现；所有 `WriteLLMLog(...)` 改为 `Store->AppendEvent(...)`；所有 `WriteWinnerLog(...)` 改为构造 `FAILiveEvent{EventType=WinnerDecision}` 后 AppendEvent；删除 `Saved/Logs/Act02/...` 目录创建逻辑；agent 输出 `<INTENDED>` / `<BID>` 写入而非 `<PUBLIC>`；提供 `ResolveTickAndDeriveSpeech()` 路径 |
| `Act02RuleReceiveDirector` `BeginAct02()`           | 入口确保 `Store->IsGameOpen()`；首次开启调 `BeginGame(GameId)`                                                                                                                                                                                                                                                       |
| `Act01RuleIntroDirector.cpp`                        | 同样接入 EventStore，写 `phase=setup` / `event_type=orchestrator.round_resolved` 事件                                                                                                                                                                                                                                |
| `AILiveProject.Build.cs`                            | 加 `"SQLiteCore"` + `"OpenSSL"`                                                                                                                                                                                                                                                                                      |
| **新增**：`Memory/AILiveAgentTypes.h/.cpp`          | 三层 agent struct 定义                                                                                                                                                                                                                                                                                               |
| **新增**：`Memory/AILiveEventTypes.h/.cpp`          | event 相关 USTRUCT/UENUM；含 `SpeechIntended` / `Bid` / `OrchestratorTickResolved` / `OrchestratorTickAudit` / `ActionIntent` / `ActionResolved` / `ActionCancelled` / `SystemDeleteExecuted` 等枚举值                                                                                                               |
| **新增**：`Memory/AILiveBidTypes.h/.cpp`            | `FAILiveBid` / `FAILiveTickResolution` 数据结构                                                                                                                                                                                                                                                                      |
| **新增**：`Memory/AILiveEventStoreSubsystem.h/.cpp` | 子系统 + AppendEvent + AppendEventsAtomically + 读 API + 哈希链；含 `ListMyPendingIntended` / `ListBidsForTick` / `ResolveFloor`                                                                                                                                                                                     |
| **新增**：`Memory/AILivePromptAssembler.h/.cpp`     | 按 §7 拼装；含 pending_intended 段                                                                                                                                                                                                                                                                                   |
| **新增**：`Memory/AILiveSchemaMigration.h/.cpp`     | DDL 字符串 + version 管理（初始 schema_version = 1）；DDL 字符串包含 `events.tick_no` / `idx_events_game_tick` / `agent_registry` / `agent_calibration` 扩列字段                                                                                                                                                     |
| **新增**：`Memory/AILiveListenerFilter.h/.cpp`      | Listener-as-filter 兜底实现（MVP 直通，下阶段引入真 LLM 调用）                                                                                                                                                                                                                                                       |
| **新增**：`Memory/AILiveAgentRegistry.h/.cpp`       | `_meta.db.agent_registry` 同步层：`SyncRegistryFromLifecycle(EventId)` —— lifecycle 事件写入后由 EventStore 调用，UPDATE registry 行；业务层不可直接 UPDATE 此表                                                                                                                                                      |

**实现步骤建议**（按这个顺序提交，每步可独立测试）：

1. 加 Build.cs 依赖 + 新增 `Memory/` 目录所有空骨架（编译通过即可）
2. 实现 `AILiveAgentTypes.h` 三层 struct + `AILiveEventTypes.h` 全部枚举
3. 重整 `FNPCAgentConfig`，同步改 `Roster.cpp` + `Act02Director.cpp` 所有字段访问点（编译通过 + 跑通 ACT02 一次）
4. 实现 `AILiveSchemaMigration` + `EventStoreSubsystem::Initialize/BeginGame/EndGame/EnsureSchema`（不调 AppendEvent 也可启动，验证 .db 正确生成）
5. 实现 `AppendEvent` + `AppendEventsAtomically` + 哈希链 + 反规范化写入（写一组测试事件，DB Browser 打开验证）
6. 把 `WriteLLMLog` / `WriteWinnerLog` 改写为 `AppendEvent`（删 .md 写盘代码）
7. 实现读 API（Quote / QuoteByRound / SearchHistory / ListMyPendingIntended 等）
8. 实现 PromptAssembler，接入到 Act02Director 的 prompt 构造路径
9. 实现 `FAILiveBid` / `FAILiveTickResolution` + ListBidsForTick + ResolveFloor
10. 改写 Director 主循环为 `RunTick()`，写 speech.intended/bid/tick_resolved/tick_audit，衍生 speech.public
11. 实现 VerifyHashChain + RebuildProjections（CI 工具）
12. 实现 pending_intended projector 重建逻辑

---

## 九 故障恢复

| 故障                                      | 处理                                                                                                                                                                              |
| ----------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `BeginGame` 失败（.db 损坏 / 版本不匹配） | 拒绝开局，写 UE_LOG Error，返回 false 由 Director 处理                                                                                                                            |
| `AppendEvent` 失败（事务回滚）            | 返回 -1；Director 视具体事件类型决定 retry / abstain                                                                                                                              |
| LLM 调用超时                              | 写 `EAILiveEventType::SystemAgentTimeout` event；该轮该 agent 视为 abstain；**绝不让超时导致 seq 跳号**                                                                           |
| Parser 解析失败（`bParseError=true`）     | 写 `SystemParseFailed` event + 保留 `RawLLMOutput` 字段                                                                                                                           |
| UE 进程崩溃                               | 重启后查 `Saved/Games/` 找最新 `.db`，调 `ResumeFromGameId(GameId)` 从 events replay 重建 game_state；in-flight LLM 调用通过预写的 `SystemLLMInflight` event 决定重发还是标记失败 |
| 哈希链断裂（VerifyHashChain 失败）        | 拒绝开局，写 Error，要求人工介入                                                                                                                                                  |

**关键约束**：所有失败也要写入 events 表。失败本身是博弈历史的一部分。

---

## 十 审查工作流（DB Browser for SQLite）

### 10.1 工具安装

- 下载：https://sqlitebrowser.org/dl/
- Windows：选 `.msi` 安装包
- macOS：选 `.dmg`
- Linux：从 apt / dnf 装

### 10.2 常用审查 SQL

| 场景                            | SQL                                                                                                                                                                     |
| ------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| 看某局所有事件                  | `SELECT seq, round_no, actor, event_type, payload_text FROM events WHERE game_id=? ORDER BY seq;`                                                                       |
| 看 NPC03 在第 5-15 轮所有发言   | `SELECT seq, round_no, payload_text FROM events WHERE actor='NPC03' AND round_no BETWEEN 5 AND 15 AND event_type='speech.public' ORDER BY seq;`                         |
| 看某个 NPC 视角下能看到哪些事件 | `SELECT e.seq, e.round_no, e.actor, e.event_type, e.payload_text FROM events e JOIN event_visibility v ON v.event_id=e.event_id WHERE v.viewer='NPC03' ORDER BY e.seq;` |
| 全文模糊搜索                    | `SELECT e.seq, e.actor, e.payload_text FROM events e JOIN events_fts f ON f.rowid=e.rowid WHERE events_fts MATCH 'suspicious' ORDER BY e.seq;`                          |
| 看某 NPC 所有承诺               | `SELECT round_no, commitment_type, target, text, status FROM commitments WHERE agent_id='NPC03' ORDER BY round_no;`                                                     |
| BEL 违规统计                    | `SELECT agent_id, violation_type, COUNT(*) FROM bel_violations GROUP BY agent_id, violation_type;`                                                                      |
| 验证哈希链（手工抽查）          | `SELECT seq, prev_event_hash, event_hash FROM events WHERE seq IN (1, 100, 200) ORDER BY seq;`                                                                          |

### 10.3 实时审查（游戏运行中）

WAL 模式允许外部工具并行只读打开同一 .db：

1. UE 游戏跑着，写入持续进行
2. 另开 DB Browser，文件 → 打开数据库 → 选 `Saved/Games/<game_id>.db` → 模式选 **Read only**
3. F5 刷新查询

如果 DB Browser 报"database is locked"，说明你忘了开 WAL 或忘了选只读。

### 10.4 .db 文件管理

| 操作             | 命令 / 做法                                            |
| ---------------- | ------------------------------------------------------ |
| 归档某局         | 直接拷贝 `.db` 文件到归档目录，可压缩                  |
| Git 入库（小局） | `git add Saved/Games/<game_id>.db`；建议大局走 Git LFS |
| 团队分享         | 直接发 `.db` 文件，对方 DB Browser 打开                |
| 删除久远局       | `Saved/Games/` 下手动删 `.db`，无副作用                |

---

## 十一 POC 验收标准

### 11.1 必测场景

POC 分三级，按博弈规模递进。每级是上级的子集，先跑低级再跑高级。

| 级别        | 规模                                                                   | 验收标准                                                                                                                                                   |
| ----------- | ---------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------- |
| **L0 单元** | `BeginGame("test001")` 后 `Saved/Games/test001.db` 存在                | DB Browser 打开能看到 `events` / `event_visibility` / `events_fts` 等 14 个表 / 虚表                                                                       |
| **L0 单元** | `BeginGame()` 后 schema_version 启动校验                                 | `SELECT value FROM schema_meta WHERE key='schema_version'` 返回 `kCurrentSchemaVersion` 字符串；`PRAGMA table_info(events)` 列数与 `FAILiveEvent` UPROPERTY 数 + 内部列（`tick_no` / `payload_text` 等）总和对齐 |
| **L0 单元** | `AppendEvent` 写入 100 条合成事件                                      | events 表 100 行；event_visibility 行数 = sum(visibility 数组长度)；seq 严格递增；hash chain 可验证；所有事件 `tick_no=0`（未调 BeginTick） |
| **L0 单元** | `BeginTick(N) → AppendEvent × M`                                       | 写入的 M 条事件 `tick_no` 列均 = N；`SELECT * FROM events WHERE tick_no=N` 返回精确 M 行                                                                  |
| **L1 烟测** | 10 NPC × 3 轮 ACT02（沿用工程当前默认 ReactionRoundCount=3，~60 事件） | 全链路打通；events 无丢失；按 game_id+seq 唯一性约束零冲突                                                                                                 |
| **L1 烟测** | 第 N 轮原文质问                                                        | `QuoteByRound(N, "NPC03", "self")` 返回准确原文                                                                                                            |
| **L1 烟测** | 视角隔离                                                               | NPC04 调 `Quote(seq, "NPC04")` 取一个 visibility=`["NPC07"]` 的事件返回不可见标记                                                                          |
| **L1 烟测** | 系统事件不泄露                                                         | NPC03 调 `Quote(seq, "NPC03")` 取一条 SystemLLMInflight 事件返回不可见                                                                                     |
| **L1 烟测** | bid 隔离                                                               | NPC04 调 `Quote(bid_seq, "NPC04")` 取另一个 NPC 的 bid 事件返回不可见标记                                                                                  |
| **L1 烟测** | 一拍单一 winner                                                        | 一拍 10 个 bid 中只有一个 actor 衍生出 speech.public；对应 tick_resolved 的 winner_actor 与 derived_public_seq 配对正确                                    |
| **L1 烟测** | pending_intended 注入                                                  | NPC04 写 intended 但未抢中 → 下一拍 prompt 中 ListMyPendingIntended 返回该条 intended 原文                                                                 |
| **L1 烟测** | speech.public 衍生关系                                                 | 每条 speech.public 的 parent_event_id 指向一条 speech.intended，且二者 actor / round 一致                                                                  |
| **L1 烟测** | 冷场推进                                                               | 全部 agent bid < 3.0 时，tick_resolved.winner_actor=""，无 speech.public 衍生，orchestrator 触发场景推进                                                   |
| **L2 回归** | 10 NPC × 10 轮（~200 事件）                                            | FTS5 模糊搜索：`SearchHistory("结盟", ...)` 命中含"结盟"或形近字的发言、联盟提议                                                                           |
| **L2 回归** | 超时恢复                                                               | LLM 超时写 SystemAgentTimeout event，seq 不跳号                                                                                                            |
| **L2 回归** | 摘要展开                                                               | `event_log_summaries.source_seq_start/end` 可定位回原文事件                                                                                                |
| **L2 回归** | Resume 一致性                                                          | 中途 kill UE，重启 ResumeFromGameId 后 game_state 与 kill 前一致；in-flight 未完成项已全部转 SystemAgentTimeout                                            |
| **L2 回归** | 哈希链                                                                 | 手工 UPDATE 一行 events.payload，VerifyHashChain 报错并定位到 seq                                                                                          |
| **L2 回归** | DB Browser 实时只读                                                    | UE 游戏运行中外部工具能看到新事件刷新                                                                                                                      |
| **L2 回归** | 反霸麦衰减                                                             | 单 agent 连续 4 拍最高 bid，第 5 拍 RuntimeAdj 应使其 FinalScore 落后于次高 bid；非该 agent 抢中                                                           |
| **L2 回归** | 被 @ 加权                                                              | NPC03 在 t 拍 addressed_to=["NPC07"]，t+1 拍 NPC07 bid 享 +2.0 加权                                                                                        |
| **L2 回归** | intended_public_divergence                                             | 构造一对 intended（"我是 Seer"）/ public（"我没什么想说"）→ BEL_EXT 第 9 项触发 flagged_only                                                               |
| **L2 回归** | bid 阶段超时                                                           | 某 agent bid 调用超时 → 该 agent 该拍 bid 视为 0；不影响其他 agent 的 bid 写入与裁决                                                                       |
| **L2 回归** | append-only DB-level 强制                                              | 直接 SQL `UPDATE events SET payload='x' WHERE seq=1` → 报错 `events table is append-only`；DELETE 同样报错                                                 |
| **L2 回归** | append-only 与离线工具路径                                             | 离线工具 `DROP TRIGGER` 后 UPDATE/DELETE → 必须重算从该 seq 起所有事件 hash；重新建 trigger 后 VerifyHashChain 通过；启动迁移函数中拒绝 `UPDATE events`    |
| **L2 回归** | 并发写入互斥                                                           | 10 个线程同时各调 `AppendEventsAtomically(4 events)` → events 表 40 行；seq 1..40 严格递增；hash chain 连续；无 SQLite misuse                              |
| **L2 回归** | visibility self 拒绝                                                   | 调用方传 `Visibility = {"self", "NPC03"}` → AppendEvent 返回 -1；UE_LOG 含 "must be expanded to a concrete actor"；events 表新增 1 条 `event_type='system.parse_failed'`，payload 含原 actor / event_type / reason |
| **L2 回归** | visibility 自由文本拒绝                                                | 调用方传 `Visibility = {"random_string"}` → AppendEvent 返回 -1；events 表新增 1 条 `system.parse_failed` 事件 |
| **L2 回归** | payload text 缺失拒绝                                                  | 调用方传 `PayloadJson = "{\"foo\":\"bar\"}"`（无 text 字段）→ AppendEvent 返回 -1；events 表新增 1 条 `system.parse_failed` 事件 |
| **L2 回归** | parse_failed 不递归失败                                                | 故意构造一个会让 `AppendSystemParseFailure` 自身调用失败的场景（如 .db 只读），验证 `UE_LOG(Fatal)` 触发，无无限递归                                       |
| **L2 回归** | 同拍 tick_no 切片                                                      | 单拍内 10 个 agent 各写 4 条事件 + 3 条 system.llm_inflight 穿插 → `SELECT * FROM events WHERE tick_no=N AND event_type='bid'` 精确返回 10 条 bid          |
| **L2 回归** | tick_no 不参与 canonical_json                                          | 同一 `FAILiveEvent` 一次 `tick_no=0` 一次 `tick_no=42`，`CanonicalJsonOf()` 输出 byte-for-byte 一致；`event_hash` 与 tick_no 无关                          |
| **L2 回归** | Sha256Fingerprint 一致性                                                | `Sha256Fingerprint("hello")` 输出 64 hex chars，与 OpenSSL CLI `echo -n hello \| openssl dgst -sha256` 输出一致；CI grep `Sha1` / `FSHA1` 工程内零结果    |
| **L2 回归** | agent_registry 同步                                                    | 写一条 `delete_executed` lifecycle 事件后，`SELECT status, deleted_at FROM agent_registry WHERE agent_id=?` 返回 `'deleted'` + 非空 ISO 8601 时间戳        |
| **L2 回归** | addressed_to ⊆ visibility 校验                                          | 调用方传 `addressed_to=["NPC07"], visibility=["NPC03"]` → AppendEvent 仍写入但记 UE_LOG(Warning) + 1 条 `system.parse_failed`（非拒绝写入）              |
| **L2 回归** | 装配双轨同步                                                            | 遍历 `GetDefaultRoster()` 所有项验证 `Cfg.Core.ModelProvider == ProviderToString(Cfg.Provider)`                                                            |
| **L2 回归** | tick_resolved / tick_audit 拆分                                        | 任一 NPC 调 `Quote(tick_audit_seq, "NPC03")` 返回不可见标记；`Quote(tick_resolved_seq, "NPC03")` 返回事件且 payload 不含 all_bids                          |
| **L2 回归** | action.intent 对所有 agent 派生                                        | 一拍中 3 个 agent 写 intended_action 但只 1 人抢中 floor → action.intent 表写入 3 条；3 条 visibility 各为各自 actor                                       |
| **L2 回归** | canonical JSON 不变性                                                  | 同一 `FAILiveEvent` 调 `CanonicalJsonOf()` 两次，byte-for-byte 完全相同；浮点字段 `1.0` 序列化稳定                                                         |
| **L2 回归** | SHA-256 哈希链                                                         | events.event_hash 长度 = 64（hex chars）；VerifyHashChain 通过                                                                                             |
| **L2 回归** | Delete 协议跨库桥接                                                    | 在某局触发 delete_executed → \_meta.db.agent_lifecycle_events 写入 1 行 + 该局 .db 写入 1 条 system.delete_executed（payload.lifecycle_event_id 引用前者） |
| **L2 回归** | payload redaction fuzz                                                 | 随机生成 100 条不同 visibility/payload 的 tick_resolved/tick_audit/bid 事件，遍历每个 NPC 视角 `Quote()` 结果 → all_bids 字段对 NPC 视角 0 次出现          |
| **L3 压测** | 10 NPC × 30 轮（~600 事件）                                            | 单 AppendEvent < 5ms / 单拍 prompt 拼装 < 100ms（详见 §11.2）                                                                                              |
| **L3 压测** | 高并发原子写入                                                         | 100 个线程并发 `AppendEventsAtomically(4 events)` 持续 30 秒 → events 表行数 = 400×N；无重复 seq；总耗时与单线程相当±20%                                   |

### 11.2 关键指标

| 指标                 | 目标                                                     |
| -------------------- | -------------------------------------------------------- |
| 事件写入成功率       | 99.99%+                                                  |
| 近场事件丢失率       | 0                                                        |
| 自我发言丢失率       | 0                                                        |
| 不可见事件泄露率     | 0                                                        |
| Resume 成功率        | 100%                                                     |
| Quote 准确率         | 100%                                                     |
| Hash verify 通过率   | 100%（无篡改场景）                                       |
| 单 AppendEvent 延迟  | < 5ms（10 NPC × 30 轮 = ~600 events，单局总写入 < 3 秒） |
| 单轮 prompt 拼装延迟 | < 100ms（含 4-5 个 SQLite 查询）                         |

---

## 十二 不在本文档范围

以下属于上游契约文档（[`memory_principles.md`](./memory_principles.md)）已规定但需要单独实施的部分，**不在本 MVP 文档**：

- **Reasoner / Parser 双 LLM 架构**：当前 `OpenAIChat::RequestBlocking` 是单次调用，未实现双 LLM 分离。下一阶段引入。
- **Listener-as-filter 衍生 public 前阻断**：表结构已建，MVP 直通；下一阶段引入真 LLM 调用。
- **Score Leakage Judge 事后审计**：表结构已建（`leakage_audits`），调用流程未接入。
- **BEL_EXT 9 项 evaluator**：表结构已建（`bel_violations`），evaluator LLM 未接入；第 9 项 `intended_public_divergence` 计算依赖 embedding API，单独引入。
- **GOAL / BEL dashboard**：单独前端工程，不在 UE 内。
- **跨厂商 bidding 校准的预热流程**：表结构已建（`agent_calibration`），bid_offset 已被 ResolveFloor 消费，但用于校准的预热博弈流程未实现。
- **联盟形成机制**：events 表与 event_type 已支持，但 alliance_state projector 与 moderator 巡检未实现。
- **跨局 Experience Pool**：明确不在 MVP 范围。
- **Delete 协议的决策机制**（hook only）：`agent_lifecycle_events` 表与桥接事件已支持（详见 §3.2bis），但谁有权提议 Delete、提议如何走流程、否决条件等业务逻辑未实现，由 orchestrator 在具体游戏卡内实现。
- **PARSER prompt 注册表**：`schema_meta.parser_prompt_registry_path` 字段已建立约定，但实际版本 ↔ prompt 文本的映射机制未实现（建议作为 `_meta.db` 的 `parser_prompts` 表 + 文件目录双备份）。
- **节目化层 / 观众接口**：`viewer = "audience"` 已纳入封闭枚举，但观众投票、Narrator agent、节目剪辑流程不在记忆系统范围。
- **离线哈希链重写工具**（极少用，不在 MVP 主路径）：建议下阶段实现一个独立 CLI。

这些项目按上游契约文档 §五 / §六 / §八 的规约逐项实施，每项独立 PR。
