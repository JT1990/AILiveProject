# T9 落地：VerifyHashChain + Resume 协议 + Delete 跨库桥接 + agent_registry 同步

## 范围

把 §11 L2 收尾三件事补齐：
1. `VerifyHashChain` 不再是 stub —— 走主连接重算 hash 链定位 first_bad_seq。
2. `ResumeFromGameId` 实装 —— 扫未配对 `system.llm_inflight` → 全部转 `system.agent_timeout`（MVP 保守策略）→ `RebuildProjections()`。
3. `TriggerDeleteExecuted` 实装 —— 三步链：写 `_meta.db.agent_lifecycle_events` → `AILiveAgentRegistry::SyncRegistryFromLifecycle` → 在该局 `.db` 写 `system.delete_executed`。

依赖：T8 已落地（DevLog `2026-05-04_projector_rebuild.md` 入库；本步 Resume 直接复用 `RebuildProjections()`）。

---

## 设计决策

| # | 决策 | 选择 | 出处 |
|---|------|------|------|
| 1 | VerifyHashChain 实现策略 | 直接转发到既有 `RecomputeHashChainOnMainConnection`（line 2202）—— 避免在 const 方法里复刻一份 | T3 已实装的主连接重算路径，无需第二条独立实现 |
| 2 | Resume in-flight 配对策略 | **MVP 保守**：新鲜（<60s）+ 过期（≥60s）**统一**写 `system.agent_timeout`，不重发；payload 含 `age_seconds` 供诊断 | impl §5.4 line 1772「下阶段才考虑做 prompt cache 重发」 |
| 3 | Resume 完成集合判定 | 扫"非 system.llm_inflight"事件的 `payload.request_id` —— 包括 `system.agent_timeout`，使 Resume **幂等**（再跑一次不会重复写 timeout） | principles §5.4 单点配对，但放宽到含 timeout 行 |
| 4 | timeout 事件的 tick_no | 复用原 in-flight 的 `tick_no`（写前 `CachedCurrentTickNo = inflight.tick_no`，AppendEvent 自动用此值填 events.tick_no）—— 让 `WHERE tick_no=N AND event_type='system.agent_timeout'` 正确归位到原拍 | 不还原 cache：Director 下次 BeginTick 自然覆盖 |
| 5 | timeout 事件的 phase / round_no | 从原 in-flight 行直接复制（payload 无关，行级别字段） | 避免 Resume 期间依赖 Director 的 ResolvePhase |
| 6 | timeout 事件 visibility | `["system"]` —— 任何 NPC 视角不可见 | impl §5.4 line 1727 + principles §5.4 |
| 7 | game_state 行加载 | **不加载** —— 工程内**无任何代码**写入 game_state 行（grep 全工程 zero match）；MVP 范围 Resume 仅依赖 RebuildProjections 重建派生表 | 任务卡"涉及文件"段提到"加载 game_state"是文档前瞻性表述；当前工程无该路径，记为下阶段 work |
| 8 | Delete 三步链顺序 | 先写 `_meta.db` lifecycle → 调 `SyncRegistryFromLifecycle` → 在该局 `.db` 写 `system.delete_executed` | impl §3.2bis line 479-483 显式约束顺序 |
| 9 | Delete 跨库一致性 | **顺序保证而非事务原子**——两 SQLite 连接不支持跨库事务；崩溃发生在 lifecycle 已写但 system event 未写时，下次 Resume 不补偿（MVP 范围外） | 任务卡"风险点 §3" + 本 DevLog 显式声明 |
| 10 | `SyncRegistryFromLifecycle` 类型分派 | 5 类 lifecycle_event_type 各自映射：`delete_executed` / `created` / `revived` / `archived` 改 status；`delete_proposed` / `delete_vetoed` 仅 bump last_updated_at | impl §3.2bis line 485 |
| 11 | agent_registry 行不存在的兜底 | 先 `INSERT OR IGNORE` 创建 active 行，再按类型 UPDATE —— 让 registry 是 lifecycle 的**单调投影**，不假设 caller 预先 INSERT 过 | schema.yaml line 475「agent_registry 是投影」 |
| 12 | 业务层禁直接 UPDATE agent_registry | 仅在 `AILiveAgentRegistry.cpp` 内允许；CI grep 守护（详见下方） | schema.yaml line 476 |

---

## 涉及文件

### 修
- `Source/AILiveProject/Public/Memory/AILiveEventStoreSubsystem.h`
  - `VerifyHashChain` 声明保留（T2 已写）
  - 新增 `ResumeFromGameId(const FString&)` 声明
  - 新增 `TriggerDeleteExecuted(...)` 声明（5 入参 + 1 出参 lifecycle_event_id）

- `Source/AILiveProject/Private/Memory/AILiveEventStoreSubsystem.cpp`
  - `VerifyHashChain` 改写为转发到 `RecomputeHashChainOnMainConnection`
  - 新增 `ResumeFromGameId` 实装（含匿名命名空间内 `ExtractRequestIdFromPayload` + `FInflightRow` 辅助）
  - 新增 `TriggerDeleteExecuted` 实装
  - 头部 include 加 `Memory/AILiveAgentRegistry.h`
  - 末尾追加 4 个 console command：
    - `AILive.Memory.VerifyHashChain`
    - `AILive.Memory.ResumeFromGameId <game_id>`
    - `AILive.Memory.TriggerDeleteExecuted <agent_id> [reason_summary]`
    - `AILive.Test.AppendBadAddressedTo`（覆盖 L2 addressed_to ⊆ visibility 用例）

### 新增
- `Source/AILiveProject/Public/Memory/AILiveAgentRegistry.h` —— `namespace AILiveAgentRegistry { bool SyncRegistryFromLifecycle(FSQLiteDatabase&, const FString&); }`
- `Source/AILiveProject/Private/Memory/AILiveAgentRegistry.cpp` —— SELECT lifecycle row → INSERT OR IGNORE registry → UPDATE registry by type；BEGIN/COMMIT/ROLLBACK 自管

---

## 算法摘要

### ResumeFromGameId(game_id)
1. `BeginGame(game_id)` —— 已幂等（关闭旧局、打开 / 创建 .db、Schema、`LoadHashChainTail`）。
2. `SELECT seq, tick_no, round_no, phase, payload FROM events WHERE event_type='system.llm_inflight' ORDER BY seq ASC` → 解析 payload 得 `(request_id, npc_index, started_at)` 列表。
3. `SELECT payload FROM events WHERE event_type != 'system.llm_inflight' AND payload LIKE '%request_id%'` → 解析 `payload.request_id` → `TSet<FString> CompletedRequestIds`。
4. 对每条未配对 in-flight：
   - `age_seconds = (UtcNow - ParseIso8601(started_at)).GetTotalSeconds()`
   - `CachedCurrentTickNo = Row.TickNo`（让 AppendEvent 用原拍号）
   - `AppendEvent(SystemAgentTimeout, visibility=["system"], payload={text, npc_index, request_id, age_seconds, resumed_at, source_inflight_seq})`
5. `RebuildProjections()` 重建派生表。

### TriggerDeleteExecuted(agent_id, reason_summary, reason_payload, tombstone_visibility, affects_persona_continuity, &out_eid)
1. 校验：`IsGameOpen` + agent_id 非空 + `IsValidViewerString(agent_id)` 通过 `ValidateVisibility` 兜底。
2. `LifecycleEventId = GenerateUuidV7()`。
3. `_meta.db` BEGIN → INSERT agent_lifecycle_events (lifecycle_event_type='delete_executed', triggered_at_seq=CachedLastSeq, ...) → COMMIT。
4. `AILiveAgentRegistry::SyncRegistryFromLifecycle(MetaDb, LifecycleEventId)` —— BEGIN → INSERT OR IGNORE registry row → UPDATE status='deleted', deleted_at=wall_clock → COMMIT。
5. 构造 `FAILiveEvent` (EventType=SystemDeleteExecuted, visibility=["public"], payload={text, lifecycle_event_id, agent_id, reason_summary}) → `AppendEvent`。
6. 返回 system event 的 seq；out_eid 写回 lifecycle event_id。

### SyncRegistryFromLifecycle(MetaDb, eid)
- 按 lifecycle_event_type 分派：
  - `delete_executed` → `status='deleted', deleted_at=<wall_clock>, last_seen_game_id, last_updated_at`
  - `created` / `revived` → `status='active', deleted_at=NULL`
  - `archived` → `status='archived'`
  - `delete_proposed` / `delete_vetoed` → 仅 bump `last_updated_at`
- 不存在的 agent_id：`INSERT OR IGNORE` 一行 active，再 UPDATE，让 registry 是 lifecycle 的单调投影。

---

## CI grep 守护约定

业务层不可直接 UPDATE `agent_registry`：

```
rg "UPDATE\s+agent_registry" Source/AILiveProject -t cpp
```

期望命中**唯一**：`Source/AILiveProject/Private/Memory/AILiveAgentRegistry.cpp` 的 4 条 UPDATE 语句（按类型分派）。任何其他 .cpp / .h 命中即违例。下阶段把这条 grep 接入 CI script。

---

## 编译验证

UBT 增量构建：

```
"D:/Software/UE_5.7/Engine/Build/BatchFiles/Build.bat" \
  AILiveProjectEditor Win64 Development \
  -Project="D:/Project/Unreal/AILiveProject/AILiveProject.uproject" \
  -WaitMutex -FromMsBuild
```

结果：`Result: Succeeded` / `Total execution time: 8.28 seconds`。
- `[Adaptive Build] Excluded from AILiveProject unity file: AILiveAgentRegistry.cpp, AILiveEventStoreSubsystem.cpp`
- 编译通过：`Compile [x64] AILiveAgentRegistry.cpp` + `Compile [x64] AILiveEventStoreSubsystem.cpp`
- 链接通过：`Link [x64] UnrealEditor-AILiveProject.lib` / `.dll`

---

## §11 验收 runbook

PIE 验收要"机械验证"——下面是 console + DB Browser 的复跑步骤。每条对应 §11 一条 L2/L3 用例。**用 `AILive.Test.BeginGame T9_<utc_ts>` 各开新局**，避免互相污染。

### L2.1 「超时恢复」+ Resume 一致性

```
> AILive.Test.BeginGame T9_resume1
> AILive.Test.AppendOne hello-1
> AILive.Test.AppendOne hello-2
# 直接构造一条裸 SystemLLMInflight：用 ExecDebugSql 跳验证写入（在编辑器内手工构造也可——
# 简单做法：跑一次 Act02 RunTick 让 Director 自然写一对 SystemLLMInflight 然后立刻 EndGame 模拟崩溃）
> # 模拟"崩溃"：
> AILive.Test.EndGame
> AILive.Memory.ResumeFromGameId T9_resume1
# 期望日志：
#   "ResumeFromGameId OK game=T9_resume1 inflights=N completed=M timeouts_written=K"
> AILive.Test.SchemaSelfCheck   # 保险：确保 schema_version=1
```
**SQL 验证**（DB Browser 打开 `Saved/Games/T9_resume1.db`）：
```sql
SELECT seq, event_type, json_extract(payload,'$.request_id'), json_extract(payload,'$.age_seconds')
FROM events
WHERE event_type IN ('system.llm_inflight','system.agent_timeout')
ORDER BY seq ASC;
-- 期望：所有 system.llm_inflight 都有同 request_id 的 system.agent_timeout（或正常完成事件）
SELECT seq, prev_event_hash, event_hash FROM events ORDER BY seq DESC LIMIT 5;
-- 期望：seq 严格 +1，无跳号
```

### L2.2 哈希链

```
> AILive.Test.BeginGame T9_hash1
> AILive.Test.AppendOne msg-1
> AILive.Test.AppendOne msg-2
> AILive.Test.AppendOne msg-3
> AILive.Test.AppendOne msg-4
> AILive.Test.AppendOne msg-5
> AILive.Memory.VerifyHashChain
# 期望：VerifyHashChain -> OK
> AILive.Test.ExecDebugSql DROP TRIGGER events_no_update
> AILive.Test.ExecDebugSql DROP TRIGGER events_fts_au
> AILive.Test.ExecDebugSql UPDATE events SET payload = '{"text":"x"}' WHERE seq = 3
> AILive.Memory.VerifyHashChain
# 期望：VerifyHashChain -> FAIL first_bad_seq=3
# （重算 hash 后再 verify 应当 OK——下阶段离线工具实现）
```

### L2.3 DB Browser 实时只读

UE 跑 ACT02 中 → 外部 DB Browser 用 Read-only 模式打开同一 `.db` → SQL `SELECT count(*) FROM events;` 多次 F5 刷新；数字递增 → WAL 模式正确。
T2 已设 `journal_mode=WAL`，无需在本步操作。

### L2.4 Delete 协议跨库桥接 + agent_registry 同步

```
> AILive.Test.BeginGame T9_del1
> AILive.Test.AppendOne setup
> AILive.Memory.TriggerDeleteExecuted NPC03 "test-delete-from-console"
# 期望：TriggerDeleteExecuted -> OK system_seq=N lifecycle_event_id=<uuid>
```
**SQL 验证**（DB Browser 同时打开 `_meta.db` 和 `T9_del1.db`）：
```sql
-- _meta.db
SELECT event_id, agent_id, lifecycle_event_type, triggered_in_game_id, reason_summary
FROM agent_lifecycle_events ORDER BY wall_clock DESC LIMIT 1;
-- 期望：1 行，lifecycle_event_type='delete_executed', agent_id='NPC03', triggered_in_game_id='T9_del1'

SELECT agent_id, status, deleted_at, last_updated_at
FROM agent_registry WHERE agent_id='NPC03';
-- 期望：status='deleted', deleted_at 非空 ISO 8601, last_updated_at 非空

-- T9_del1.db
SELECT seq, event_type, visibility,
       json_extract(payload,'$.lifecycle_event_id') AS leid,
       json_extract(payload,'$.agent_id')           AS aid
FROM events WHERE event_type='system.delete_executed';
-- 期望：1 行；leid 与 _meta.db 主键匹配；aid='NPC03'；visibility 含 'public'
```
**视角验证**（任一 NPC 都能看到）：
```
> AILive.Test.QuoteByRound 0 NPC07
# events 列表里能看到 system.delete_executed 行
```

### L2.5 addressed_to ⊆ visibility（写入仍成功 + parse_failed 旁写）

```
> AILive.Test.BeginGame T9_addr1
> AILive.Test.AppendBadAddressedTo
# 期望：log 出现 "Warning: addressed_to target 'NPC07' not present in visibility set"
#       AppendBadAddressedTo -> seq=N （非 -1，写入仍成功）
```
**SQL 验证**：
```sql
SELECT seq, event_type FROM events WHERE event_type='system.parse_failed';
-- 期望：1 行（addressed_to 兜底审计）
SELECT seq, event_type, visibility, addressed_to FROM events WHERE event_type='speech.public';
-- 期望：原始事件也写入了，visibility=["NPC03"], addressed_to=["NPC07"]
```

### L3.1 单 AppendEvent < 5 ms

跑 50 次 `AILive.Test.AppendOne foo`；T3 阶段已有 ConcurrentAppend 计时基线（`elapsed_ms` 字段）。本任务**不**改 AppendEvent 路径，性能由 T3 已验。

### L3.2 单拍 prompt 拼装 < 100 ms

T6 验收（DevLog `2026-05-04_t6_prompt_assembler.md`）已基线。本任务不改 PromptAssembler 路径。

### L3.3 100 线程并发 30 秒

```
> AILive.Test.BeginGame T9_l3
> AILive.Test.ConcurrentAppend 100 4
# 期望：ConcurrentAppend(100 x 4) done: success=100, failed=0, elapsed_ms<2000
> AILive.Test.ConcurrentAppend 100 4   # 跑多轮累计 30 秒+
SELECT COUNT(*) FROM events WHERE event_type='speech.public';
-- 期望：400 × 累计轮数；无重复 seq；VerifyHashChain OK
> AILive.Memory.VerifyHashChain
```

---

## 实证回填

> **状态**：本节占位待用户驱动 PIE / DB Browser 跑 runbook 后回填实证日志 / SQL 输出。
> code 已落地、UBT 通过、console 已注册；§11 PIE 用例的"机械实证"由下次 PIE 会话填写。

```
[L2.1 超时恢复 + Resume 一致性]      —— pending PIE
[L2.2 哈希链]                        —— pending PIE
[L2.3 DB Browser 实时只读]           —— pending PIE
[L2.4 Delete 协议 + agent_registry]  —— pending PIE
[L2.5 addressed_to ⊆ visibility]      —— pending PIE
[L3.1/2/3 性能]                       —— pending PIE
```

---

## 不在本步范围

1. **Resume 重发新鲜 in-flight**：MVP 保守统一 timeout；下阶段引入 prompt cache 重发由独立 PR 处理（impl §5.4 line 1772）。
2. **Delete 协议崩溃补偿**：lifecycle 写但 system event 未写时的"Resume 时补写 system event"（任务卡风险点 §3 提到）—— 当前 Resume 路径仅扫 in-flight，不扫"orphan lifecycle"。下阶段做。
3. **game_state 行的写入 / 读取**：当前工程内零写入方；Resume 仅 `RebuildProjections` 重建派生。下阶段 game_state 单行表落地后再补 Resume 末段。
4. **CI 接入 grep 守护**：本 DevLog 记录约定 + 命令；实际 CI script 接入由 infra 层独立处理。
