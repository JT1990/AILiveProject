# 阶段 2 — 中央枢纽 API：UAILiveEventStoreSubsystem

只读一个文件：`Public/Memory/AILiveEventStoreSubsystem.h`。**不进 .cpp**。

读完这一阶段你应当能回答（计划里的 3 题 + 加 1 题）：

1. 为什么 `AppendEvent` 拿的是 **non-const ref**？
2. `RebuildProjections` 是幂等的吗？什么时候会调？
3. `TriggerDeleteExecuted` 涉及哪两个 SQLite 连接？为什么不能合到一个事务？
4. （加题）为什么 `Quote` 必须传 `viewer`，传 `"self"` 字面量会发生什么？

---

## 0. 这是什么

```cpp
UCLASS()
class AILIVEPROJECT_API UAILiveEventStoreSubsystem : public UGameInstanceSubsystem
```

它是一个 **UGameInstanceSubsystem**：

- **谁创建的**：UE 引擎；和 `GameInstance` 一同诞生、一同销毁
- **怎么拿**：`UGameInstance::GetSubsystem<UAILiveEventStoreSubsystem>()`
- **生命周期**：从启动 PIE / 游戏到退出，全程一个实例
- **跨关卡保留**：是（GameInstance 不随 LevelTravel 销毁）

这就是为什么"事件存储"挂在这里而不是某个 Actor——它必须**比关卡活得久**，否则跨场景的事件链就断了。

---

## 1. 内部状态（先看 private 成员）

```cpp
private:
    FSQLiteDatabase Db;          // game-specific：Saved/Games/<game_id>.db
    FSQLiteDatabase MetaDb;      // cross-game：Saved/Games/_meta.db

    FString CurrentGameId;

    mutable FCriticalSection WriteMutex;   // 所有写入串行化
    int64   CachedLastSeq = 0;             // 哈希链尾 seq
    FString CachedLastHash;                // 哈希链尾 hash
    int64   CachedCurrentTickNo = 0;       // BeginTick 设定，AppendEvent 自动回填

    FString DetectedFtsTokenizer;          // "trigram" 或 "unicode61"
```

**两条 SQLite 连接是关键**——记住这张图：

```
┌────────────────────────────────────────────────────────────────┐
│  UAILiveEventStoreSubsystem                                    │
│                                                                │
│   Db ─────► Saved/Games/<game_id>.db                           │
│             ├─ events / events_fts                             │
│             ├─ commitments (T8 投影)                            │
│             ├─ vote_history (T8 投影)                           │
│             ├─ alliance_state (T8 投影)                         │
│             └─ agent_view_state (T8 投影)                       │
│                                                                │
│   MetaDb ──► Saved/Games/_meta.db                              │
│             ├─ agent_registry          (跨局 agent 状态)        │
│             ├─ agent_lifecycle_events  (跨局：注册/删除审计)     │
│             └─ schema_meta             (元数据 KV)              │
└────────────────────────────────────────────────────────────────┘
```

**两条连接独立、不共享事务**——这就是 `TriggerDeleteExecuted` 不能原子的根本原因（自检题 #3）。

`mutable FCriticalSection WriteMutex` 之所以加 `mutable`：const 读路径有时也要拿锁（避免读到半完成的写）。这是 UE 里常见的 "logical const" 模式。

---

## 2. 五组 API 概览

```
1. 生命周期      BeginGame / EndGame / IsGameOpen / GetCurrentGameId / ResumeFromGameId
2. 写入          AppendEvent / AppendEventsAtomically / BeginTick / GetCurrentTickNo
                + TriggerDeleteExecuted（跨 DB 桥）
3. 读 + 可见性    Quote* / ListMy* / SearchHistory / ListVotes / ListAllianceStateJson
                + DebugReadPendingIntendedJson / QuoteByEventTypeAndTick
4. 投影          RebuildProjections
5. 完整性 / 调试  VerifyHashChain / RecomputeHashChainOnMainConnection
                / ExecuteDebugSqlOnMainConnection / SetGameDbQueryOnly
                / QueryMetaSchemaRegistry / UpsertMetaSchemaKV

外加 T7 内部：ListBidsForTick / ResolveFloor （**非 BlueprintCallable**）
外加 静态工具：ValidateVisibility / ValidatePayloadJson / IsAddressedToSubsetOfVisibility
              / CanonicalJsonOf / ComputeEventHash
```

下面逐组讲。

---

## 3. 生命周期组

### 3.1 `BeginGame(GameId)` / `EndGame()`

```cpp
bool BeginGame(const FString& InGameId);
void EndGame();
bool IsGameOpen() const { return Db.IsValid(); }
const FString& GetCurrentGameId() const;
```

- `BeginGame` 打开/创建 `Saved/Games/<GameId>.db` + `_meta.db`，跑 schema 初始化，加载哈希链尾 (`LoadHashChainTail` 填 `CachedLastSeq` / `CachedLastHash`)
- 任意时刻只能开**一局**——`Db.IsValid()` 是布尔锁
- `EndGame` 关闭连接，但**不删数据**

### 3.2 `ResumeFromGameId(GameId)` —— T9 关键

```cpp
bool ResumeFromGameId(const FString& InGameId);
```

注释（带回阶段 4a 仔细看）说的是：

1. 打开 `<GameId>.db`（不存在则等价于新建）
2. 扫所有 `system.llm_inflight` 事件 → 收集 `payload.request_id`
3. 扫所有非 `system` 事件，凡是 payload 含相同 `request_id` 的算"配对完成"
4. **未配对的就是悬空的 in-flight**——为每条写一条 `system.agent_timeout`（visibility=`["system"]`，载荷含 npc_index / request_id / age_seconds / resumed_at）
5. 跑 `RebuildProjections()` 让派生表与新尾对齐

**MVP 范围**：不区分"新鲜（< 60s）"和"陈旧（≥ 60s）"in-flight，**一律按 timeout 处理**。重发请求 + prompt cache 留给后续。

> 失败语义：任一步骤失败 → log Error + return false。**调用方不应假设部分恢复可用**——下次启动重新 Resume。

---

## 4. 写入组

### 4.1 `AppendEvent` —— 自检题 #1

```cpp
int64 AppendEvent(FAILiveEvent& InOutEvent);
```

参数是 **non-const ref**。原因写在注释里：

> *"On success, fills `InOutEvent.{Seq, EventId, PrevEventHash, EventHash}` and returns Seq."*

也就是 4 个字段是 **EventStore 帮你回填的**：

| 字段 | 谁填 | 怎么填 |
|---|---|---|
| `EventId` | EventStore | 内部 `GenerateUuidV7()` |
| `Seq` | EventStore | `CachedLastSeq + 1` |
| `PrevEventHash` | EventStore | `CachedLastHash` |
| `EventHash` | EventStore | `SHA256(PrevHash ‖ canonical_json(self))` |

调用方填业务字段（Actor / EventType / PayloadJson / Visibility / ...），EventStore 填审计字段。**non-const ref 是契约的物理表达**：你不能传 `const` 进去，因为函数确实要改它。

返回值：成功返回 seq，**失败返回 -1**——失败语义同时往真相日志写一条 `system.parse_failed`（不会让你"静默丢失"）。

### 4.2 `AppendEventsAtomically` —— 多事件单事务

```cpp
int64 AppendEventsAtomically(TArray<FAILiveEvent>& InOutEvents);
```

一组事件**全部成功或全部失败**（SQLite 单事务包裹）。返回组首事件的 seq；任一失败 → 整组 rollback + 返回 -1。

注释里有重要的并发设计细节：

> *"Static validation runs **before** the WriteMutex is taken; rejected events trigger a system.parse_failed write through the regular bypass path (which takes the lock itself), so there is no nested-lock / nested-BEGIN risk."*

**记住这个分两阶段的写入纪律**：
1. **先静态校验**（不持锁，纯函数：`ValidateVisibility` / `ValidatePayloadJson`）
2. **再持锁 BEGIN IMMEDIATE 写入**

如果阶段 1 拒绝了，单独写 `system.parse_failed`（自己拿锁），**绝不嵌套**。

### 4.3 `BeginTick(InTickNo)` —— 开拍

```cpp
int64 BeginTick(int32 InTickNo);
int64 GetCurrentTickNo() const { return CachedCurrentTickNo; }
```

每个 tick 开始时调一次，做两件事：

1. 写一条 `orchestrator.tick_anchor` 事件
2. 更新 `CachedCurrentTickNo` —— **后续每个 `AppendEvent` 都会自动把这个值写进 `events.tick_no`**

失败时回滚 `CachedCurrentTickNo`；返回 anchor 事件的 seq。

> 这就是为什么 `FAILiveEvent.TickNo` 标 `BlueprintReadOnly` ——**调用方不要自己填，BeginTick 会接管**。

### 4.4 `TriggerDeleteExecuted` —— 自检题 #3

```cpp
int64 TriggerDeleteExecuted(
    const FString& InAgentId,
    const FString& InReasonSummary,
    const FString& InReasonPayloadJson,
    const TArray<FString>& InTombstoneVisibility,
    bool bAffectsPersonaContinuity,
    FString& OutLifecycleEventId);
```

注释明白写了三步：

```
Step 1 → MetaDb：INSERT 一行 agent_lifecycle_events
                 (lifecycle_event_type='delete_executed') → 获取 event_id
Step 2 → MetaDb：调 AILiveAgentRegistry::SyncRegistryFromLifecycle(MetaDb, eventId)
                 → 把 agent_registry.status 设为 'deleted'，回填 deleted_at
Step 3 → Db    ：Append 一条 system.delete_executed 事件到 game.db
                 visibility=["public"]，payload 含 lifecycle_event_id + agent_id + reason_summary
```

**自检题 #3 答案**：

- 涉及的两条连接：`MetaDb`（步骤 1+2）和 `Db`（步骤 3）。
- **它们不能共享 SQLite 事务**——是两个不同的连接到两个不同的 `.db` 文件。
- 步骤 (1)+(2) 之间崩溃：lifecycle 行已写、registry 同步未完——下次 Resume 应能识别（**MVP 未实现这个 reconcile**）
- (1)+(2) 完成、(3) 之前崩溃：lifecycle 行已写、game 内 system.delete_executed 缺失——同样需要 Resume 补偿（**MVP 未实现**）

返回 `system.delete_executed` 事件的 seq；任一步失败返回 -1。

> **改动建议**：以后给 Resume 加补偿时，逻辑应该是"扫 _meta.db 找最近的 lifecycle_event，看 game.db 里有没有对应 system.delete_executed；缺了就补写"。

---

## 5. 读 + 可见性组（T4）

### 5.1 视角隔离的硬规则

```cpp
// === T4 — 读 API（视角隔离 JOIN 强制）。
// viewer 永远是具体 actor ID 字符串（如 "NPC03" / "orchestrator" / "system"）；
// 传 "self" 字面量一律返回不可见——self 是 prompt 模板的相对语义，禁止入读路径。
```

**自检题 #4 答案**：传 `"self"` 进 `Quote` / `QuoteByRound` / `QuoteRecentRounds` 的 viewer，会返回**空集**。原因：`"self"` 是 prompt 装配阶段的占位符（"我自己"），不是任何 actor 的真实 ID；让它进读路径就等于绕过可见性，所以代码故意让它失效。

每个读 API **强制传 viewer**，内部 SQL 都带可见性 EXISTS JOIN。这是**硬约束 5、6**（见 memory_principles.md）。

### 5.2 `Quote*` 系列（按 seq / 按 round / 按窗口）

```cpp
bool Quote(int64 InSeq, const FString& InViewer, FAILiveEvent& OutEvent) const;
TArray<FAILiveEvent> QuoteByRound(int32 InRoundNo, const FString& InActor, const FString& InViewer) const;
TArray<FAILiveEvent> QuoteRecentRounds(int32 InCurrentRound, int32 InK, const FString& InViewer) const;
```

- `Quote` 单点取，不可见或不存在均返回 false
- `QuoteByRound` 取**指定 actor 在该轮**对 viewer 可见的事件
- `QuoteRecentRounds` 取最近 K 轮窗口（用于 prompt 装配的 "NEAR WINDOW" 段）

### 5.3 `ListMy*` 系列（agent 自查）

```cpp
ListMyStatements(agent)        // speech.public ∪ private_msg（自我发言全量追溯）
ListMyNotes(agent, N)          // 最近 N 条 speech.note
ListMyReflections(agent, N)    // 最近 N 条 reflection.9q
ListMyPendingIntended(agent, N) // 最近 N 条「写了 intended 但未派生 public」
ListMyCommitments(agent, roundStart, roundEnd)  // 投影表直读
```

注意 `ListMyStatements` 注释：

> *"private_msg 能被命中要求写入侧把 actor 自身放进 visibility（T5/T7 写入纪律）"*

也就是说"我能查到我自己发的私聊"这件事**不是免费的**——写入 private_msg 时必须把 actor 自己放进 visibility 数组，否则 viewer=自己也读不出来。**这是写入纪律**，阶段 5 会看到具体落点。

### 5.4 `ListMyPendingIntended` 的特殊性

注释强调：

> *"走 events 表 + parent 链直算，**不**依赖 agent_view_state 投影表（即使该表被 DROP 仍工作）"*

为什么？**因为 RebuildProjections 失败时投影表可能丢失，但 prompt 装配仍要能拉到 pending_intended**。这是一条防御性 API。

### 5.5 `ListVotes(round)` 的反直觉

注释明确说**绕过 vote_history 投影表**：

> *"取 round 内全部 event_type='vote' 的公开原文（直接查 events 表，**不**读 vote_history 投影表——投影表只用作索引/计数；提取原文必须走 events，否则 EventId/PayloadJson/EventHash 等关键字段缺失，调用方误以为拿到了可 quote 的事件）"*

记住：**投影表是派生索引，不是真相源**。任何需要 `EventId` / `PayloadJson` / `EventHash` 的场景**必须查 events 表**。

### 5.6 `SearchHistory` —— FTS5 + 自动降级

```cpp
TArray<FAILiveEvent> SearchHistory(keyword, actor, roundStart, roundEnd, viewer, limit);
```

实现细节：

- 优先用 FTS5 trigram 索引（`events_fts` 表）
- 关键词长度 < 3 或 trigram 不可用时**自动降级到 `payload_text LIKE '%kw%'`**
- 两条路径都套相同的 visibility EXISTS

### 5.7 `ListAllianceStateJson` / `DebugReadPendingIntendedJson` / `QuoteByEventTypeAndTick`

```cpp
FString ListAllianceStateJson() const;
FString DebugReadPendingIntendedJson(const FString& InAgentId) const;
TArray<FAILiveEvent> QuoteByEventTypeAndTick(EAILiveEventType InEventType, int64 InTickNo) const;
```

- 前两个返回 JSON 字符串，BlueprintCallable，给 UI / 调试用
- `QuoteByEventTypeAndTick` 是 **orchestrator 内部 API**——故意**不**加 BlueprintCallable，避免 BP 误用绕过 viewer 隔离

---

## 6. 投影组（T8）—— 自检题 #2

```cpp
// === T8 — Projector 重建 ============================================
// principles §7.4 + §7.2 末「projector 是纯函数，不调任何 LLM」。
// 任一时刻调用都得到与 events 当前快照一致的派生表内容（幂等）。
// 单事务包裹 DELETE → INSERT；事务在 WriteMutex 下持有，与 AppendEvent 互斥。
// 失败 → 整体 ROLLBACK，旧投影保留。

bool RebuildProjections();
```

**自检题 #2 答案**：

- **是幂等的**——注释直接说"任一时刻调用都得到与 events 当前快照一致的派生表内容"
- 实现：单事务 `DELETE * FROM projection_table` → `INSERT ...`，跑 4 次（commitments / vote_history / alliance_state / agent_view_state）
- **什么时候调**：
  1. 阶段 5 主循环 Stage D：每拍 `tick_resolved` 写完后异步调一次
  2. `ResumeFromGameId` 末尾调一次
  3. 调试控制台命令手动触发
- 失败 → 整体 rollback，**旧投影保留**——这是为什么投影表"不可作为真相源"，但仍可供查询（永远是最近一次成功重建的快照）

### 私有 reducer（4 个）

```cpp
private:
    bool ProjectCommitments_LockHeld();
    bool ProjectVoteHistory_LockHeld();
    bool ProjectAllianceState_LockHeld();
    bool ProjectAgentViewState_LockHeld();   // 单快照策略：每 agent 一行
```

要求调用方**已持锁 + BEGIN IMMEDIATE 在飞**。失败返回 false 让 caller rollback——**reducer 不直接抛错**。这是阶段 4c 的内容。

---

## 7. T7 Bid + Floor（orchestrator 内部）

```cpp
TArray<FAILiveBid> ListBidsForTick(int64 InTickNo) const;

FAILiveTickResolution ResolveFloor(
    int64 InTickNo,
    const TArray<FString>& InEligibleAgentIds,
    const TMap<FString, float>& InAgentBidOffsets,
    float InColdThreshold = 3.0f) const;
```

两个都 **没有 `UFUNCTION(BlueprintCallable)`**——故意只在 C++ 层暴露，避免 BP 误用破坏 floor 协议。

`ResolveFloor` 的关键不变量（注释）：

- 取最大 FinalScore 的 actor 为 winner
- **最大 < ColdThreshold（默认 3.0）→ WinnerActor=""**（**冷场**：本拍无人发言）
- 同分 → **lexicographic actor_id 取小**（确定性，无随机）
- `DerivedPublicSeq` 留 0 → caller 写完 `speech.public` 后回填

`RuntimeAdj` 的具体公式藏在私有 `ComputeRuntimeAdjustment`：

```cpp
float ComputeRuntimeAdjustment(const FString& InActor, int64 InCurrentTickNo) const;
```

注释给的是任务卡常量基线：

| 项 | 条件 | 调整 |
|---|---|---|
| 反霸麦 | tick-1..tick-3 winner 连续 ≥3 拍是本 actor | **−1.5** |
| 被 @ 加权 | 上拍 winner intended payload.addressed_to_hint 含本 actor | **+2.0** |
| 沉默加权 | 本 actor 最近 5 拍无 speech.public | **+0.5** |

三项独立累加。**这就是阶段 1 自检 #3 里 RuntimeAdj 三股力的具体常量**。

---

## 8. 完整性与调试组

### 8.1 哈希链验证

```cpp
bool VerifyHashChain(int64& OutFirstBadSeq) const;

bool RecomputeHashChainOnMainConnection(
    int64& OutLastSeq, int64& OutBadCount, int64& OutFirstBadSeq);
```

两者差异：

- `VerifyHashChain` 用通用读路径
- `RecomputeHashChainOnMainConnection` 注释解释为什么必须走主连接：

> *"secondary connections within the same process can't acquire the WAL writer lock"*

**这是个**踩坑点**：UE 同进程开第二条 SQLite 连接，在 WAL 模式下**无法**和主连接共享 -shm 内存映射，会回 `SQLITE_IOERR`。所以**所有需要写、或要拿写锁的操作都必须走 `Db` 主连接**。

### 8.2 调试 SQL（仅开发/测试）

```cpp
bool ExecuteDebugSqlOnMainConnection(const FString& Sql, FString& OutError);
bool SetGameDbQueryOnly(bool bQueryOnly);
bool QueryMetaSchemaRegistry(TMap<FString, FString>& OutKVs);
bool UpsertMetaSchemaKV(const FString& Key, const FString& Value);
```

后两个用于元数据 KV 存储（`schema_meta` 表），用例：

- `parser_version` 动态读取（默认从 schema_meta 读，读不到落到常量 fallback）
- 调试控制台命令运行时改 schema_meta 看新事件是不是生效

### 8.3 静态校验工具

```cpp
static bool ValidateVisibility(const TArray<FString>& InVisibility, FString& OutError);
static bool ValidatePayloadJson(const FString& InPayloadJson, FString& OutError);

static bool IsAddressedToSubsetOfVisibility(
    const TArray<FString>& InAddressedTo,
    const TArray<FString>& InVisibility,
    FString& OutError);
```

- 前两个是写入纪律的硬校验
- `IsAddressedToSubsetOfVisibility` 是**软校验**：违反不拒绝写入（"暗中点名"是合法的），但 caller 应 log Warning + 写一条 `system.parse_failed` 审计行，再继续原写入

### 8.4 哈希工具（公开）

```cpp
static FString CanonicalJsonOf(const FAILiveEvent& InEvent);
FString ComputeEventHash(const FString& PrevHash, const FString& CanonicalPayload) const;

static constexpr int32 kCurrentSchemaVersion = 1;
static const TCHAR* const kGenesisHash;
```

- `CanonicalJsonOf` 是 `static` 的，**因为对同一事件必须永远输出相同字节序列**——任何依赖外部状态都会破链
- `kGenesisHash` 是哈希链的初始值（第 1 条事件的 PrevEventHash）

---

## 9. 私有 helper 一览

只列名字 + 一句话，阶段 4a 再深入：

```cpp
ApplyPragmas / EnsureSchema / RunMigrations / EnsureMetaRegistry  // schema 初始化
DetectFtsTokenizer                                                // trigram or unicode61
LoadHashChainTail                                                 // 启动时填 CachedLast*
InsertEventBypassValidation_LockHeld / InsertEventBypassValidation
                                                                  // 内部插入路径
AppendSystemParseFailure                                          // 拒绝时写真相日志
ResolveLiveParserVersion                                          // schema_meta 动态读
ComputeRuntimeAdjustment                                          // T7 三股力计算
ProjectCommitments_LockHeld / ProjectVoteHistory_LockHeld
ProjectAllianceState_LockHeld / ProjectAgentViewState_LockHeld    // T8 reducer
GenerateUuidV7                                                    // EventId 生成
```

---

## 10. 心智模型

```
┌──────────────────── 调用方 (Director / Prompt / 调试) ────────────────────┐
│                                                                          │
│  写：BeginGame ─► BeginTick ─► AppendEvent(s) ─► RebuildProjections      │
│      ↑（生命周期）  ↑（每拍开头）  ↑（每条事件）   ↑（每拍尾或异步）      │
│                                                                          │
│  读：Quote / QuoteByRound / QuoteRecentRounds / ListMy* (含 viewer)      │
│      └─► viewer EXISTS JOIN visibility 数组 ─► 行集                      │
│                                                                          │
│  完整性：VerifyHashChain / RecomputeHashChainOnMainConnection            │
│  跨库：TriggerDeleteExecuted（MetaDb 2 步 + Db 1 步）                     │
│  恢复：ResumeFromGameId（扫 inflight → 补 timeout → 重建投影）            │
│                                                                          │
└──────────────────────────────────────────────────────────────────────────┘
                                  │
                                  ▼
            ┌─────────── EventStore 内部 ───────────┐
            │ WriteMutex + Cached{LastSeq,LastHash,│
            │             CurrentTickNo}           │
            │                                      │
            │ Db (game.db)        MetaDb (_meta)   │
            │  └ events (真相源)   └ agent_registry │
            │  └ FTS5 索引         └ lifecycle      │
            │  └ 4 张投影表        └ schema_meta    │
            └──────────────────────────────────────┘
```

---

## 11. 自检答案汇总

1. **`AppendEvent` 为何 non-const ref？** EventId / Seq / PrevEventHash / EventHash 由 EventStore 回填——不能传 const，因为函数确实要改它。
2. **`RebuildProjections` 幂等吗？** 是；DELETE → INSERT 包在单事务里，失败 rollback 旧投影保留。**调用时机**：每拍 `tick_resolved` 后异步、`Resume` 末尾、调试触发。
3. **`TriggerDeleteExecuted` 涉及哪两个连接？为什么不能合事务？** `MetaDb`（步骤 1+2）+ `Db`（步骤 3）。两条独立 SQLite 连接到两个不同 .db 文件，SQLite 事务**不能跨连接**。MVP 没做崩溃中间态的补偿。
4. **`Quote(seq, "self", out)` 会发生什么？** 返回 false（视为不可见）。`"self"` 是 prompt 装配的相对语义，禁止入读路径——硬约束 5、6。

---

## 12. 顺手指出的 4 个"易错点"

读 .cpp 之前先记住，后面会反复遇到：

1. **WAL secondary connection 陷阱**：同进程开第二条 SQLite 连接拿不到 -shm 共享映射，所有写或拿写锁的操作必须走主 `Db` 连接
2. **投影表不是真相源**：要拿 `EventId/PayloadJson/EventHash` 必须查 `events` 表（`ListVotes` 注释专门强调过）
3. **写入纪律**：private_msg 要让自己以后查得到，必须把自己 ID 放进 visibility 数组——这是写入侧的责任不是读侧
4. **canonical JSON 的字段顺序**：`CanonicalJsonOf` 是 static + 纯函数，**任何引入外部状态/不稳定排序都会破链**

---

## 下一步

进入 **阶段 3 — 主导演 API**（30 min）。读两个 header：

```
Public/Acts/Act02RuleReceiveDirector.h    ★ 核心
Public/Acts/Act01RuleIntroDirector.h      ← 对照看共同套路
```

读完回来，我把阶段 3 写进 `Learn/03-acts-api.md`。
