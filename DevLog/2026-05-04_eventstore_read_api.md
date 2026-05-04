# 2026-05-04 — EventStore 读 API + 视角隔离 JOIN（T4 落地）

> 范围：[Tasks/T4_eventstore_read_api.md](../Tasks/T4_eventstore_read_api.md)
> 依赖：T3（写路径，commits b8c55e8 / 73f7311）
> 完成定义：12 个读 API + 7 个 console command 编译通过 + UBT 全量构建零警告 + 7 个 L1 用例全部通过

## 决策记录

### D1. 读侧 viewer 展开规则

写侧 `event_visibility` 行按字面量存储——`visibility=["public"]` 在 event_visibility 表里只产生
一行 `(event_id, 'public')`，不展开成 per-agent 行（`AILiveEventStoreSubsystem.cpp:855-868`）。

读侧对应规则（`ExpandViewerForJoin`，cpp 内匿名 namespace）：

| 调用方 viewer | 展开成 IN 集合              |
| ------------- | --------------------------- |
| `NPC<NN>`     | `{NPC<NN>, public}`         |
| `audience`    | `{audience, public}`        |
| `Faction<X>`  | `{Faction<X>, public}`      |
| `public`      | `{public}`                  |
| `orchestrator`| `{orchestrator}`            |
| `system`      | `{system}`                  |
| `self` 字面量 | ∅（直接返回不可见，UE_LOG Warning） |
| 自由文本 / 空 | ∅（同上）                   |

裁决依据：principles 硬约束 6（视角隔离纯靠数据层 JOIN）+ implementation §3.2 line 2068
"agent 看不到对方 bid 的保证依赖 event_visibility JOIN" + L1 用例 "QuoteByRound(N, NPC03, NPC03)
返回 NPC03 自己 round N 的 speech.public 原文" — 不展开就取不到 visibility=["public"] 的事件。

### D2. ListMyPendingIntended 走 events 表 + parent 链

```sql
SELECT <kEventSelectColumns> FROM events e
WHERE e.game_id = ? AND e.actor = ? AND e.event_type = 'speech.intended'
  AND EXISTS (SELECT 1 FROM event_visibility v
              WHERE v.event_id = e.event_id AND v.viewer IN (?, 'public'))
  AND NOT EXISTS (SELECT 1 FROM events c
                  WHERE c.game_id = e.game_id
                    AND c.event_type = 'speech.public'
                    AND c.parent_event_id = e.event_id)
ORDER BY e.seq DESC LIMIT ?;
```

完全不读 `agent_view_state.pending_intended` 投影表——T6 PromptAssembler 不会被 T8 投影锁死。
即使手工 DROP `agent_view_state` 表，本查询仍正确返回（验收用例 7 实测）。

### D3. 投影表读 vs 事件流读

- `ListMyCommitments / ListVotes / ListAllianceStateJson` 读 `commitments / vote_history / alliance_state`
  投影表 — T8 落地后才有数据，T4 阶段返回空。
- `ListMyPendingIntended` 走 events + parent 链直算（D2）。
- 其余 `Quote* / SearchHistory / ListMy* / QueryEventsByActor` 走 events + event_visibility EXISTS。

### D4. 所有读路径统一走 viewer JOIN（含 self-quote）

task card 第 14 行硬要求"所有读路径都通过视角隔离 JOIN"，本实现 12 个 API 中：
- 11 个走 EXISTS(event_visibility)（含 ListMyStatements/Notes/Reflections——self-quote 也走 JOIN）
- 唯一例外 `QuoteByEventTypeAndTick`，task card 标"内部用"，调用方是 orchestrator 自身

speech.intended/note/reflection 的 visibility 通常是 `[agentId]`，对应 event_visibility 行
`(event_id, agentId)`，agent 自己 viewer 经 ExpandViewerForJoin 后 IN 集合含 agentId，能命中。
即使将来 visibility 写多元素，self-quote 也保留隔离不变量。

### D5. EXISTS 子查询替代 JOIN（去重）

`event_visibility` 是反规范化表，同一事件可能有多行 viewer（如 visibility=["public","NPC03"]
产生 2 行）。NPC03 viewer 经展开成 `IN (NPC03, public)` 时，普通 JOIN 会让事件返回 2 次。
统一用 `EXISTS (SELECT 1 FROM event_visibility v WHERE v.event_id=e.event_id AND v.viewer IN (...))`，
EXISTS 是布尔判断，多行可见性匹配只产出 1 条 event 行。

验收用例 6 实测：seq=10 是 NPC05 的 visibility=["public","NPC03"] 事件，
`QuoteByRound(1, NPC05, NPC03)` 返回 1 行（不是 2 行）。

### D6. SearchHistory 双路径（FTS5 + LIKE fallback）

SQLite FTS5 trigram tokenizer 要求 query 跨完整 3-gram 窗口。2 字汉字 query "结盟" 不命中
"我想提出结盟提议"——只有 3 字 "结盟提" 才命中 trigram 索引。task card 字面要求
`SearchHistory("结盟", ...)` 必须命中，所以分支：

```cpp
const bool bUseLike = (InKeyword.Len() < 3) || (DetectedFtsTokenizer != TEXT("trigram"));
```

LIKE 路径用 `payload_text LIKE '%kw%' ESCAPE '\\'`，配套 `EscapeLikePattern` 转义 `%` / `_` / `\`，
防止用户输入 `100%` 这类被当成 SQL 通配。FTS5 不可用降级到 unicode61 时也走 LIKE。

实测（验收用例 5）：
- `SearchHistory("结盟", NPC03, 20)` → path=LIKE, 3 行命中（seq 1/3/4，含"结盟"字）
- `SearchHistory("结盟提议", NPC03, 20)` → path=FTS5, 1 行命中（seq 1）
- 两条路径在同一次 PIE 会话里都跑通

注：seq 2 "联盟形成" 没被 LIKE 命中——是预期行为，LIKE 是字符级而非语义级匹配；trigram FTS5
对完整 trigram 也不会把"结盟"和"联盟"对应起来。

## 实现清单

### header（`Source/AILiveProject/Public/Memory/AILiveEventStoreSubsystem.h`）

追加 12 个读 API 声明（前 11 个 BlueprintCallable）：
- `Quote(seq, viewer, OutEvent) -> bool`
- `QuoteByRound(round, actor, viewer)`
- `QuoteRecentRounds(currentRound, K, viewer)`
- `ListMyStatements(agentId)`
- `ListMyNotes(agentId, n)`
- `ListMyReflections(agentId, n)`
- `ListMyPendingIntended(agentId, n)`
- `ListMyCommitments(agentId, roundStart, roundEnd)`
- `SearchHistory(keyword, actor, roundStart, roundEnd, viewer, limit)`
- `ListVotes(round)`
- `ListAllianceStateJson() -> FString`
- `QuoteByEventTypeAndTick(eventType, tickNo)`（**不**加 BlueprintCallable）

新增 member：`FString DetectedFtsTokenizer`。EnsureSchema 在**任何 early return 之前**就解析
`sqlite_master.sql` 里 events_fts 的 CREATE VIRTUAL TABLE DDL 缓存它（已存在的 db 直接反映
其真实 tokenizer；首次迁移路径调 DetectFtsTokenizer 探测当前 SQLite 能力）。
`schema_version=1` 旧 db 重开时 SearchHistory 也能正确选 FTS5/LIKE 路径，不会静默退化。

### cpp（`Source/AILiveProject/Private/Memory/AILiveEventStoreSubsystem.cpp`）

匿名 namespace 内 helpers：
- `kEventSelectColumns`：17 列字符串常量（不含 tick_no），所有读 SQL 共用
- `ExpandViewerForJoin(viewer) -> TArray<FString>`：viewer → IN 集合
- `MakeViewerInPlaceholders(count) -> FString`：动态生成 `?,?,?` 串
- `EscapeLikePattern(keyword) -> FString`：LIKE 模式转义
- `RowToEvent(stmt) -> FAILiveEvent`：按 kEventSelectColumns 顺序逐列读
- `ListSelfEventsByType(...)`：ListMyStatements/Notes/Reflections 共用 helper

替换 stub `QueryEventsByActor`（cpp:563）为真实 SELECT。

9 个 console command 注册（追加在文件末尾）：
- `AILive.Test.SeedReadAcceptance` — deterministic seed（13 条事件覆盖全部用例）
- `AILive.Test.QuoteByRound <round> <actor> <viewer>`
- `AILive.Test.QuoteSeq <seq> <viewer>`
- `AILive.Test.SearchHistory <keyword> <viewer> [limit]`
- `AILive.Test.ListMyPendingIntended <actor> <recentN>`
- `AILive.Test.ListMyStatements <agentId>`
- `AILive.Test.ListVotes <round>`
- `AILive.Test.QuoteByEventTypeAndTick <event_type> <tick_no>`
- `AILive.Test.ExecDebugSql <raw_sql>`（包装 ExecuteDebugSqlOnMainConnection）

读 API 实现关键点：
- `ListMyStatements`：`event_type IN ('speech.public', 'private_msg')` + visibility EXISTS
- `ListVotes`：直接查 events 表 `event_type='vote' AND round_no=?` + EXISTS viewer='public'，**不**读 `vote_history` 投影
- `ListAllianceStateJson`：`alliance_state.terms` 用 `WriteValue`（schema 是 string，避免输出非法 JSON），`members` 保留 `WriteRawJSONValue`（schema 是 JSON 数组字符串）

## 验收实证

UBT 全量构建：
```
Result: Succeeded — Total execution time: 6.11 seconds
```
零编译错误零 UHT 警告。

`-game -log -ExecCmds=...` 跑 16 条 T4 console 命令（13 条 seed + 3 条额外读 API）：

| 用例 | 命令 | 期望 | 实测 | 通过 |
| ---- | ---- | ---- | ---- | ---- |
| 1 第 N 轮原文质问 | `QuoteByRound 1 NPC03 NPC03` | NPC03 自己 round 1 的全部可见事件 | 3 行（seq=1 speech.public / seq=12 private_msg / seq=13 vote） | ✓ |
| 2 视角隔离 | `QuoteSeq 8 NPC04`（NPC07-only） | NOT VISIBLE | NOT VISIBLE | ✓ |
| 3 系统事件不泄露 | `QuoteSeq 9 NPC03`（system.llm_inflight） | NOT VISIBLE | NOT VISIBLE | ✓ |
| 4 bid 隔离 | `QuoteSeq 11 NPC04`（visibility=[orchestrator]） | NOT VISIBLE | NOT VISIBLE | ✓ |
| 5 LIKE 路径 | `SearchHistory 结盟 NPC03 20`（< 3 字） | LIKE path / 命中含"结盟"事件 | path=LIKE，3 行 seq 1/3/4 | ✓ |
| 5 FTS5 路径 | `SearchHistory 结盟提议 NPC03 20`（≥ 3 字） | FTS5 path / trigram 命中 | path=FTS5 tokenizer=trigram，1 行 seq=1 | ✓ |
| 6 去重（EXISTS） | `QuoteByRound 1 NPC05 NPC03`，seq=10 visibility=[public,NPC03] | 仅 1 行 | 1 行 seq=10 | ✓ |
| 7 PendingIntended 不依赖投影 | 调用 → DROP agent_view_state → 再调用 | 两次结果一致 | 两次都 1 行 seq=5；DROP→OK | ✓ |
| 8 ListMyStatements 含私聊 | `ListMyStatements NPC03` | 4 条 speech.public + 1 条 private_msg | 5 行（seq 1/2/3/4 + seq=12） | ✓ |
| 9 ListVotes 真原文 | `ListVotes 1` | 含完整 EventId/PayloadJson 的 vote 事件 | 1 行 seq=13 含 payload "投 NPC07" target+reason | ✓ |

`QuoteByEventTypeAndTick(speech.intended, 0)` → 2 行（NPC04 两条 intended）：内部 API 烟测通过。

DB 落地实证（sqlite3 直读）：
- `events` 表 13 条按 seq 升序，actor/event_type/round_no 全对
- `event_visibility` 表反规范化正确：seq=10 有 2 行（viewer='NPC03' + 'public'）；seq=12 NPC03 私聊 visibility=["NPC04","NPC03"] 写两行；其它单值 visibility 单行
- `agent_view_state` 在 `AILive.Test.ExecDebugSql DROP TABLE` 后从 sqlite_master 消失，确认 ExecDebugSql 真在主连接上跑了 DDL

## 风险点实测

- `bool Quote` 返回 false 双语义（不可见 / 不存在）：L1 用例不区分；调用方拿 `OutEvent.Seq != 0` 判定即可
- `IN` 子句动态拼接：实现里 `MakeViewerInPlaceholders` 按 ExpandViewerForJoin 返回的尺寸生成 `?,?,?`，bind 顺序固定。所有 SQL 都先 sprintf 拼字符串再 prepare——和 T3 写路径"固定列数 prepare 一次"风格略不同，但每个 viewer 集合尺寸是 1 或 2，prepare 开销可忽略
- `event_visibility` 索引 `idx_event_visibility_viewer (viewer)` 让 IN 子句仍命中索引，无 N+1
- Append-only triggers 不影响 SELECT，验证读路径完全 read-only

## 不动的清单（task card 范围外）

- `AILiveEventTypes.h/.cpp`（T1）
- `AILiveSchemaMigration.cpp`（T2）
- `AILiveSchemaCheck.cpp`（T1）
- 任何 Director / LLM / GASP / Mover / Visual-override 路径
- 写路径 `AppendEvent` / `AppendEventsAtomically` / 哈希链（T3）

## 协作者审查迭代（3 轮，全部修订已收敛进上面"实现清单/验收实证"段）

### 第 1 轮（开发前 plan 阶段）

D1（viewer 展开规则）/ D2（pending_intended 不读投影）/ D4（self-quote 也走 JOIN）/
EXISTS 替代 JOIN（防去重）/ kEventSelectColumns 列清单稳定 / SearchHistory LIKE fallback /
deterministic seed 替代依赖未落地的 T5/T7 数据 / Header 不暴露 SQLite 类型。
全部接受并融入 plan 与初版实现。

### 第 2 轮（实现后第一次 review）

- **High**：ListMyStatements 仅返回 speech.public 漏 private_msg（principles 硬约束 2
  "自我发言全量追溯"含全部发言通道）→ SQL 改 `event_type IN ('speech.public','private_msg')`，
  并要求写入侧把 actor 自身放进 visibility（T5/T7 写入纪律）。实测 `ListMyStatements(NPC03)` 5 行。
- **Medium**：ListVotes 从 vote_history 投影表拼半残 FAILiveEvent → 改查 events 表
  `event_type='vote'` + EXISTS viewer='public'，返回真原文。实测 `ListVotes(1)` 1 行含完整字段。
- **Medium**：ListAllianceStateJson `terms` 字段误用 `WriteRawJSONValue`（schema 是 string 非 JSON）
  → 改 `WriteValue` 自动转义；保留 `members` 用 raw（schema 是 JSON 数组）。

### 第 3 轮（最终 review）

- **Medium**：EnsureSchema 在 `ExistingVersion == kCurrentSchemaVersion` 早返回前**不**缓存
  `DetectedFtsTokenizer`，schema_version=1 旧 db 重开时 SearchHistory 静默退化为 LIKE。
  修：在所有 early-return 前**先**解析 `sqlite_master.sql` 里 events_fts 的 CREATE VIRTUAL TABLE
  DDL（`tokenize='trigram'` 子串判断），反映 db 真实使用的 tokenizer；events_fts 还未建（首次
  迁移路径）才调 `DetectFtsTokenizer` 探测。这样无论新建还是重开都正确选 FTS5/LIKE 路径。
- **Low**：Header 注释 + DevLog 早期段落仍残留旧描述（"只取 speech.public"、"投影表直读
  vote_history"、"7 个 command/11 条事件"）→ 收敛到最终实现，本节只保留迭代结论而非过程。
- **范围外**：`Tasks/prompt.md` 含用户自己的模板编辑（T0 → T4 替换），`git diff --check`
  报 trailing whitespace。该文件不在 T4 涉及文件清单，T4 commit 单独排除处理，不动其内容。

### 强制重编验证

每轮修订后均关 UnrealEditor + 跑 `Build.bat AILiveProjectEditor Win64 Development -Project=...`，
日志显示 `[1/4] Compile [x64] AILiveEventStoreSubsystem.cpp` + Link 真在编译（不是 up to date），
零编译错误零 UHT 警告。最终一轮加入 EnsureSchema 早返回 cache 后再次重编通过。

### -game 模式 Quit-time crash 备注

`-game` 模式 ExecCmds 末尾 `Quit` 后，进程析构期间 NVIDIA ACE A2F plugin 有已知 crash
（栈帧 `A2FLocal::~FA2FLocal` → `nvaimPluginGetFunction`）。发生时序在 EngineExit 之后，
所有 console command 已完成、log 已 flush——**不**影响 T4 验收正确性。后续若需要 headless
跑全自动测试，可 ExecCmds 末尾不加 Quit 让进程自然结束，或对应 plugin disable。

## 后续

T4 的"完成定义"已满足，但**不**意味着：
- T6 PromptAssembler 已就绪（T6 是下一步，会调用本 T4 的 12 个读 API）
- T8 投影完整（ListMyCommitments/ListAllianceStateJson 在 T8 上线前返回空集是预期）
- ListVotes **不**依赖 T8——直接查 events 表 event_type='vote'，T8 投影 `vote_history` 仅用作索引/计数，提取原文走 events

T6 启动**不**依赖 T8——`ListMyPendingIntended` 走 events 表自连接的实现确保了这一点。
