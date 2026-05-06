# 阶段 4a — 存储与哈希链

读：

```
Private/Memory/AILiveEventStoreSubsystem.cpp  (~3500 行，按函数挑读)
Private/Memory/AILiveSchemaMigration.cpp      (DDL 真相)
Private/Memory/AILiveSchemaCheck.cpp          (USTRUCT vs schema.yaml 对账)
Public/Util/AILiveSha256.h                    (SHA-256 工具)
Public/Util/AILiveJsonEscape.h                (JSON 安全转义)
```

读完这一阶段你应当能回答：

1. 为什么 `tick_no` **不参与哈希链**？真到 cpp 里看一眼是怎么排除的。
2. **canonical JSON** 怎么定义？字段顺序怎么决定？
3. `AppendEventsAtomically` 为什么要分**两阶段**（lock-free 校验 + locked 写入）？
4. `ResumeFromGameId` 重复跑同一个 .db 会重复写 timeout 吗？为什么？

---

## 1. 文件物理位置

```
<ProjectDir>/Saved/Games/
   ├── <game_id>.db          ← 主连接 Db   (单局)
   └── _meta.db              ← 主连接 MetaDb (跨局)
```

两个文件都是 SQLite，但用**两条独立连接**。`Saved/Games/` 不存在时由 `EnsureSavedGamesDir()` 现建。

> 想搞清楚某局到底落了什么，直接拿 SQLite Browser 打开这两个 .db 看就行——**它们是真相源**，UE 内的 events/commitments 表和这俩文件 1:1 对应。

---

## 2. `BeginGame` 的 9 步开局（cpp:398-469）

```cpp
bool BeginGame(const FString& InGameId)
{
    // 1. 早退检查
    if (InGameId.IsEmpty()) return false;
    if (IsGameOpen()) EndGame();          // 已有局先关
    
    // 2. 建目录
    EnsureSavedGamesDir();
    
    // 3. 开 Db
    Db.Open("<game_id>.db", ReadWriteCreate);
    ApplyPragmas(Db);                      // WAL / synchronous=NORMAL 等
    EnsureSchema(Db, /*meta=*/false);      // events / events_fts / indexes / triggers
    
    // 4. 开 MetaDb
    MetaDb.Open("_meta.db", ReadWriteCreate);
    ApplyPragmas(MetaDb);
    EnsureSchema(MetaDb, /*meta=*/true);   // agent_registry / lifecycle / schema_meta
    EnsureMetaRegistry(MetaDb);            // 跨局表的初始行
    
    // 5. 提交身份
    CurrentGameId = InGameId;
    
    // 6. 加载哈希链尾
    LoadHashChainTail();                   // 填 CachedLastSeq + CachedLastHash
    
    return true;
}
```

任一步失败 → 全部回滚（Close 两个 db、重置 CurrentGameId）。

### 2.1 `LoadHashChainTail`（cpp:489-521）

```cpp
CachedLastSeq  = 0;
CachedLastHash = FString(kGenesisHash);  // 默认创世哈希
CachedCurrentTickNo = 0;

// 取末尾 1 行
SELECT seq, event_hash FROM events WHERE game_id=? ORDER BY seq DESC LIMIT 1;

if (有行) {
    CachedLastSeq  = row.seq;
    CachedLastHash = row.event_hash;
}
// 没行（新建库） → 保持创世默认
```

**这就是为什么 `BeginGame` 同时是"新建"和"打开"——同一段代码处理两种情形**：

- 新库：events 表为空 → CachedLastSeq=0，下一条事件 seq=1，PrevHash=kGenesisHash
- 老库：取末尾一行 → 续链

---

## 3. Schema：DDL 在哪里（SchemaMigration.cpp）

`AILiveSchemaMigration.cpp` 是 DDL 字符串的**真相源**，没有 .h——所有表定义直接以 `static const TCHAR*` 声明在匿名命名空间里。摘几条关键的：

### 3.1 `events` 表（cpp:24-46）

```sql
CREATE TABLE IF NOT EXISTS events (
  event_id        TEXT PRIMARY KEY,
  game_id         TEXT NOT NULL,
  seq             INTEGER NOT NULL,
  tick_no         INTEGER NOT NULL DEFAULT 0,
  round_no        INTEGER NOT NULL,
  phase           TEXT NOT NULL,
  actor           TEXT NOT NULL,
  event_type      TEXT NOT NULL,
  speech_act_type TEXT,
  visibility      TEXT NOT NULL,         -- JSON 数组字符串
  addressed_to    TEXT,                   -- JSON 数组字符串
  payload         TEXT NOT NULL,
  payload_text    TEXT GENERATED ALWAYS AS (json_extract(payload,'$.text')) STORED,
  parent_event_id TEXT,
  parser_version  TEXT NOT NULL DEFAULT '1',
  raw_llm_output  TEXT,
  prev_event_hash TEXT NOT NULL,
  event_hash      TEXT NOT NULL,
  wall_clock      TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')),
  UNIQUE (game_id, seq)
);
```

**注意 4 处**：

1. `payload_text` 是 **GENERATED STORED 列**——直接从 `payload` JSON 抽 `$.text`。这就是 `SearchHistory` 降级到 `LIKE` 时打哪个字段。
2. `wall_clock` 用 SQLite `strftime` 自动填，不在 InsertEventBypassValidation 里手填。
3. `UNIQUE(game_id, seq)` 是**写入完整性的物理保险**——并发越权写时数据库会先报错，比代码里查 LastSeq 更可靠。
4. `prev_event_hash` 和 `event_hash` 是 `NOT NULL`——所以创世事件的 prev_hash 必须等于 `kGenesisHash`，不能为 NULL。

### 3.2 索引

```sql
idx_events_game_round   (game_id, round_no, seq)
idx_events_actor        (game_id, actor, seq)
idx_events_type         (game_id, event_type, round_no)
idx_events_game_tick    (game_id, tick_no, seq)
```

四个索引覆盖：QuoteByRound / QuoteRecentRounds（前两个）/ ListVotes / QuoteByEventTypeAndTick。

### 3.3 不可变性触发器

```sql
CREATE TRIGGER events_no_update BEFORE UPDATE ON events
BEGIN SELECT RAISE(ABORT, 'events table is append-only; UPDATE forbidden'); END;

CREATE TRIGGER events_no_delete BEFORE DELETE ON events
BEGIN SELECT RAISE(ABORT, ...); END;
```

**SQLite 触发器层**禁止 UPDATE / DELETE 任何 events 行。代码里手滑想 `UPDATE events SET ...` 都会被数据库拒——这是事件溯源的物理护栏。

---

## 4. Schema 对账（SchemaCheck.cpp）

`AILiveSchemaCheck.cpp` 是个**对齐检查器**：它在编辑器启动时遍历 `kFieldMappings` 表（硬编码 9 条），对每条 `schema.yaml` 路径，检查对应 USTRUCT 是否真的有那个字段、类型对不对：

```cpp
// 摘录
{ TEXT("agent.agent_id"),             &GetCoreStruct,     TEXT("AgentId"),           StrProperty },
{ TEXT("identity.voice_presentation"),&GetIdentityStruct, TEXT("VoicePresentation"), EnumVoicePresentation },
...
```

**它不是运行时严格依赖**——更像编译态的 schema linter。如果你以后在 `FAILiveAgentCore` 加 / 删 / 改字段，这个表也要跟着改，否则启动期会 log Warning。

---

## 5. Canonical JSON：哈希的输入（cpp:1658-1710）—— 自检题 #2

```cpp
FString CanonicalJsonOf(const FAILiveEvent& InEvent)
{
    W->WriteObjectStart();
    W->WriteValue("actor",            InEvent.Actor);
    W->WriteArrayStart("addressed_to");
        for (T : AddressedTo) W->WriteValue(T);
    W->WriteArrayEnd();
    W->WriteValue("event_id",         InEvent.EventId);
    W->WriteValue("event_type",       EventTypeToString(...));
    W->WriteValue("game_id",          InEvent.GameId);
    W->WriteValue("parent_event_id",  InEvent.ParentEventId);
    W->WriteValue("parser_version",   InEvent.ParserVersion);
    W->WriteIdentifierPrefix("payload");
        // 反序列化 PayloadJson → 递归 canonical
        WriteCanonicalObject(PayloadObj, *W);
    W->WriteValue("phase",            PhaseToString(...));
    W->WriteValue("raw_llm_output",   InEvent.RawLLMOutput);
    W->WriteValue("round_no",         (int64)InEvent.RoundNo);
    W->WriteValue("seq",              InEvent.Seq);
    W->WriteValue("speech_act_type",  SpeechActToString(...));
    W->WriteArrayStart("visibility");
        for (V : Visibility) W->WriteValue(V);
    W->WriteArrayEnd();
    W->WriteObjectEnd();
}
```

### 5.1 字段**字母序**（这就是 canonicalization 规则）

```
actor < addressed_to < event_id < event_type < game_id <
parent_event_id < parser_version < payload < phase <
raw_llm_output < round_no < seq < speech_act_type < visibility
```

任意 `Map<K,V>` 的序列化都要按 **K 的字典序**展开。`payload` 内的嵌套对象用 `WriteCanonicalObject` 递归走同一规则。这就是为什么不能用普通 `FJsonSerializer::Serialize`——后者**不保证**字段顺序，破链分分钟的事。

### 5.2 故意排除的 4 个字段（自检题 #1 答案）

```
× tick_no          ← BeginTick 设的逻辑时序，可能与 seq 顺序错位
× prev_event_hash  ← 自指：哈希链算的就是它，进了 canonical 就死循环
× event_hash       ← 同上
× wall_clock       ← 写入时间，机器钟差异会破链；不参与审计
```

`tick_no` 的注释写在 `InsertEventBypassValidation_LockHeld`（cpp:1789-1792）：

> *"T6: 回填 TickNo 给调用方……新增 `FAILiveEvent.TickNo` 字段后必须同步回填，否则 PromptAssembler 拿到的事件结构体 TickNo 仍是 0。**`CanonicalJsonOf` 跳过 `tick_no` 字段，所以回填不影响哈希计算**。"*

也就是说：

- `tick_no` **写进 events 表的物理列**（第 17 列，索引 `idx_events_game_tick` 用它）
- `tick_no` **回填进 `FAILiveEvent` 内存结构体**（PromptAssembler 拿来用）
- `tick_no` **从 canonical JSON 排除**（不进哈希）

**三套 tick_no 同步而独立**——值得记住。

### 5.3 `ComputeEventHash`（cpp:1712-1716）

```cpp
FString ComputeEventHash(const FString& PrevHash, const FString& CanonicalPayload) const
{
    const FString Combined = PrevHash + CanonicalPayload;
    return AILiveUtil::Sha256Fingerprint(Combined);  // OpenSSL SHA-256 → 64 hex
}
```

字符串拼接 → SHA-256 → 64 字符小写 hex。**`AILiveSha256.h` 头注释专门强调"SHA-1 helpers are forbidden project-wide"**——别误用。

---

## 6. Insert 路径：3 表 + 17 列（cpp:1779-1898）

`InsertEventBypassValidation_LockHeld` 是写入的最底层，要求 caller **已持锁 + BEGIN IMMEDIATE 在飞**。

它做 4 件事：

### 6.1 回填 EventStore-controlled 字段（cpp:1784-1797）

```cpp
InOutEvent.GameId        = CurrentGameId;
InOutEvent.Seq           = InOutLocalLastSeq + 1;
InOutEvent.EventId       = GenerateUuidV7();
InOutEvent.ParserVersion = ResolveLiveParserVersion();
InOutEvent.PrevEventHash = InOutLocalLastHash;
InOutEvent.TickNo        = CachedCurrentTickNo;

const FString CanonicalPayload = CanonicalJsonOf(InOutEvent);
const FString NewHash = ComputeEventHash(InOutLocalLastHash, CanonicalPayload);
InOutEvent.EventHash = NewHash;
```

**注意 `LocalLastSeq` / `LocalLastHash`**：用 caller 传进来的 by-ref 局部副本，不直接读 `Cached*`——这样事务 rollback 时全局缓存不被污染。

### 6.2 写 events 表（17 列 INSERT）

```cpp
INSERT INTO events (event_id, game_id, seq, round_no, phase, actor,
                    event_type, speech_act_type, visibility, addressed_to,
                    payload, parent_event_id, parser_version, raw_llm_output,
                    prev_event_hash, event_hash, tick_no)
VALUES (?1, ..., ?17);
```

`visibility` 和 `addressed_to` 数组 → `ArrayToJsonString` 序列化成 JSON 数组字符串再存。

### 6.3 写 `event_visibility` 规范化表

```cpp
for (Viewer : InOutEvent.Visibility)
    INSERT INTO event_visibility(event_id, viewer) VALUES (?1, ?2);
```

为什么需要这张表？**EXISTS JOIN 用**——读 API 的 viewer 隔离 SQL 是这样写的：

```sql
WHERE EXISTS (
  SELECT 1 FROM event_visibility ev
  WHERE ev.event_id = events.event_id
    AND ev.viewer IN (?, 'public')   -- viewer 自己 + 'public'
)
```

JSON 字段做不出这种高效 JOIN，所以**冗余存储**：events.visibility 是 JSON（写哈希用），event_visibility 是规范化表（读 JOIN 用）。

### 6.4 写 `event_addressed_to`（如果非空）

同理，规范化表给读路径用。

### 6.5 出口：更新 caller 的本地副本

```cpp
InOutLocalLastSeq  = InOutEvent.Seq;
InOutLocalLastHash = NewHash;
return InOutEvent.Seq;
```

**caller 决定是否提交到全局** `Cached*`。

---

## 7. `AppendEventsAtomically` 的两阶段（cpp:2012-2138）—— 自检题 #3

```cpp
int64 AppendEventsAtomically(TArray<FAILiveEvent>& InOutEvents)
{
    // 早退...
    
    // === Stage 1: static validation (LOCK-FREE on purpose).
    for (Ev : InOutEvents)
    {
        if (!ValidateVisibility(...))     { AppendSystemParseFailure(...); return -1; }
        if (!ValidatePayloadJson(...))    { AppendSystemParseFailure(...); return -1; }
        if (!IsAddressedToSubsetOfVisibility(...))   // 软校验
        {
            UE_LOG(Warning, ...);
            AppendSystemParseFailure(...);            // 写审计但不返回
            // 继续写原事件
        }
    }
    
    // === Stage 2: lock + transaction.
    FScopeLock Lock(&WriteMutex);
    Db.Execute("BEGIN IMMEDIATE;");
    
    int64 LocalLastSeq = CachedLastSeq;     // 局部副本
    FString LocalLastHash = CachedLastHash;
    
    for (Ev : InOutEvents) {
        if (InsertEventBypassValidation_LockHeld(Ev, LocalLastSeq, LocalLastHash) < 0)
            { bOk = false; break; }
    }
    
    if (bOk && !Db.Execute("COMMIT;")) bOk = false;
    
    if (!bOk) {
        Db.Execute("ROLLBACK;");
        // 清空已分配的身份字段，避免 caller 误信
        for (Ev : InOutEvents) {
            Ev.Seq = 0; Ev.EventId.Reset();
            Ev.PrevEventHash.Reset(); Ev.EventHash.Reset();
        }
        return -1;
    }
    
    // 提交全局缓存
    CachedLastSeq  = LocalLastSeq;
    CachedLastHash = LocalLastHash;
    return FirstAssignedSeq;
}
```

### 7.1 为什么要分两阶段？

`AppendSystemParseFailure` 自己拿锁 + 自己 BEGIN/COMMIT。**SQLite 不允许嵌套 BEGIN IMMEDIATE 在同一连接**——如果 Stage 1 在已开事务里 reject，再调 AppendSystemParseFailure 会立刻死锁/出错。

**所以 Stage 1 必须在锁外做完**：

```
✗ 错误设计：BEGIN → 校验 → reject → 嵌套 BEGIN 写 parse_failed → 死锁
✓ 实际设计：校验 → reject → 写 parse_failed（自己拿锁/事务） → return -1
            校验 → ok → BEGIN → 批量插入 → COMMIT
```

注释（cpp:2025-2028）原话：

> *"static validation runs **LOCK-FREE on purpose**. On rejection write a system.parse_failed via the public bypass — which takes the lock itself. Doing this **outside** WriteMutex avoids a nested BEGIN IMMEDIATE within an already-open transaction."*

### 7.2 软校验的特殊处理

`IsAddressedToSubsetOfVisibility` 不通过 → log Warning + 写一条 parse_failed 审计行，**但不返回**——继续写入原事件。这是协议允许的"暗中点名"语义。

### 7.3 失败回滚的"清空 caller 状态"

```cpp
for (Ev : InOutEvents) {
    Ev.Seq = 0;
    Ev.EventId.Reset();
    Ev.PrevEventHash.Reset();
    Ev.EventHash.Reset();
}
```

caller 拿到的是 by-ref 数组，函数已经把 EventId / Seq 等填进去过——回滚时**必须清空**，否则 caller 可能拿着"看起来成功"的字段做下一步操作。

### 7.4 全局缓存只在 COMMIT 后提升

```cpp
LocalLastSeq / LocalLastHash    ← Stage 2 内的副本
        ↓ COMMIT 成功
CachedLastSeq / CachedLastHash  ← 全局
```

事务期间 / rollback 后 Cached* 不动。**这是确保 Cached* 永远反映已落盘真相**的关键。

---

## 8. `BeginTick` 的回滚保护（cpp:2146-2201）

```cpp
int64 BeginTick(int32 InTickNo)
{
    FScopeLock Lock(&WriteMutex);
    
    const int64 OldTick = CachedCurrentTickNo;   // 备份
    
    Db.Execute("BEGIN IMMEDIATE;");
    
    CachedCurrentTickNo = (int64)InTickNo;       // 先升 tick
    
    // 构建 anchor 事件（Actor=orchestrator, EventType=tick_anchor, vis=['public']）
    InsertEventBypassValidation_LockHeld(Anchor, LocalLastSeq, LocalLastHash);
    
    if (!Db.Execute("COMMIT;")) bOk = false;
    
    if (!bOk) {
        Db.Execute("ROLLBACK;");
        CachedCurrentTickNo = OldTick;           // 还原 tick！
        return -1;
    }
    
    CachedLastSeq  = LocalLastSeq;
    CachedLastHash = LocalLastHash;
    return AnchorSeq;
}
```

**关键不变量**：tick 在 anchor 事件被持久化**之前**就升起来了——因为 `InsertEventBypassValidation_LockHeld` 自己读 `CachedCurrentTickNo` 填进 events.tick_no。如果 anchor 写入失败而不还原 OldTick，下一条事件的 tick_no 就会"莫名跳号"。

注释（cpp:2141-2144）：

> *"Saves OldTick so a failure path restores the cache (otherwise a failed BeginTick(N) would leak N to the next AppendEvent)."*

---

## 9. `VerifyHashChain`（cpp:793-822）

```cpp
bool VerifyHashChain(int64& OutFirstBadSeq) const
{
    OutFirstBadSeq = -1;
    if (!IsGameOpen()) return false;
    
    int64 LastSeq, BadCount, FirstBadSeq;
    // const_cast 因为 RecomputeHashChainOnMainConnection 不是 const
    Self->RecomputeHashChainOnMainConnection(LastSeq, BadCount, FirstBadSeq);
    
    if (BadCount > 0) {
        OutFirstBadSeq = FirstBadSeq;
        UE_LOG(Error, "chain BAD bad_count=%lld first_bad_seq=%lld");
        return false;
    }
    return true;
}
```

实际 walk 在 `RecomputeHashChainOnMainConnection`：按 seq 升序逐行读 `events`，重算 `SHA256(prev || canonical_json(row))` 与 `event_hash` 比对。

**`const_cast<UAILiveEventStoreSubsystem*>(this)` 出现是因为**：`Self` 要调 `RecomputeHashChainOnMainConnection`（非 const），但 `VerifyHashChain` 自己签名是 const——调用方拿到的是只读视图。这是 UE 里"逻辑 const"模式的一个变体。

---

## 10. `ResumeFromGameId`（cpp:3195-3355）—— 自检题 #4

四步：

```cpp
bool ResumeFromGameId(const FString& InGameId)
{
    BeginGame(InGameId);                       // 0. 打开（不存在则建空库）
    
    // 1. 收集所有 system.llm_inflight 行
    SELECT seq, tick_no, round_no, phase, payload
      FROM events
      WHERE event_type='system.llm_inflight';
    // 解析每行的 payload.{request_id, npc_index, started_at}
    
    // 2. 收集所有"已配对完成"的 request_id
    SELECT payload FROM events
      WHERE event_type != 'system.llm_inflight'
        AND payload LIKE '%request_id%';
    // 提取每行 payload.request_id → 加入 CompletedRequestIds set
    
    // 3. 逐条未配对的 inflight：写一条 system.agent_timeout
    for (Row : Inflights) {
        if (CompletedRequestIds.Contains(Row.RequestId)) continue;
        
        // 计算 age
        AgeSeconds = Now - Row.StartedAt;
        
        FAILiveEvent Out;
        Out.EventType  = SystemAgentTimeout;
        Out.Visibility = ['system'];
        Out.PayloadJson = "{
            text:'agent timeout from resume',
            npc_index:..., request_id:..., age_seconds:...,
            resumed_at:..., source_inflight_seq:...
        }";
        
        // ⭐ 关键：把 tick_no 设为原 inflight 的 tick，便于法医
        CachedCurrentTickNo = Row.TickNo;
        AppendEvent(Out);
    }
    
    // 4. 重建投影
    RebuildProjections();
    
    return true;
}
```

### 10.1 完成集合的"宽门"扫描

注意 step 2 的 SQL：

```sql
WHERE event_type != 'system.llm_inflight' AND payload LIKE '%request_id%'
```

这是个**宽门**——只要 payload 里出现"request_id"字符串就算"完成"。**包括 `system.agent_timeout` 自己**——这就是 Resume 幂等的关键。

### 10.2 自检题 #4 答案

> 重复跑同一个 .db 会重复写 timeout 吗？

**不会**。原因：

1. 第一次 Resume：扫到 N 条 inflight、0 条 completed → 写 N 条 agent_timeout
2. 写 timeout 时 payload 里包含 `request_id`
3. 第二次 Resume：扫 inflight 还是 N 条，但 completed 集合包含了上一轮写的 N 条 timeout（因为它们 payload 里有 request_id 字符串）
4. 第二次的 `unpaired` 集合 = 空 → 不写新 timeout

注释（cpp:3261-3263）原话：

> *"We include system.agent_timeout so that re-running Resume on the same .db is idempotent."*

### 10.3 `tick_no` 的"法医回填"

```cpp
CachedCurrentTickNo = Row.TickNo;       // 临时设为原 inflight 的 tick
AppendEvent(Out);                        // InsertEventBypass 会用这个 tick 填表
```

**为什么这么写**：让 `WHERE tick_no=N AND event_type='system.agent_timeout'` 能把 timeout 路由回原 tick——便于事后取证（"第 5 拍是不是有谁卡死了？"）。

注释明确说："Director's next BeginTick() overwrites this on the next live tick"——下次正常 BeginTick 会覆盖回正常值，**不会污染之后的事件**。

---

## 11. 关键不变量速查表

读完这一阶段，记住下面 6 条；下次改 .cpp 不破链就靠它们：

| # | 不变量 | 在哪强制 |
|---|---|---|
| 1 | events 表只能 INSERT，UPDATE/DELETE 被触发器 ABORT | SchemaMigration.cpp DDL |
| 2 | Canonical JSON 字段按字母序、嵌套对象递归 | CanonicalJsonOf cpp:1658 |
| 3 | tick_no / prev_hash / event_hash / wall_clock **不进哈希链** | CanonicalJsonOf 的字段缺位 |
| 4 | Cached* 只在 COMMIT 后提升；事务期用 LocalLast* | AppendEventsAtomically cpp:2089-2136 |
| 5 | 静态校验在锁外、写入在锁内——绝不嵌套 BEGIN | AppendEventsAtomically 注释 cpp:2025 |
| 6 | Resume 通过 "completed 集合包含已写 timeout" 实现幂等 | ResumeFromGameId 注释 cpp:3261 |

---

## 12. 扩展任务推演

按这一阶段的理解，下面 3 个改动你应该能马上判断"动哪些行"：

| 改动 | 至少要动 |
|---|---|
| 给 `FAILiveEvent` 加一个新字段 `Author`（是哪个 LLM 写的） | (1) header struct 加字段 + 序列化函数；(2) DDL 加列 + 加迁移；(3) **决定**：进哈希吗？进 → 加 `WriteValue("author", ...)` 到 CanonicalJsonOf 的字母序正确位置；(4) Insert SQL bind +1 列 |
| 让 `Quote` 也支持 `viewer="orchestrator"` 看到所有事件 | 当前已支持——orchestrator 不是 "self"，可见性 EXISTS JOIN 会让它命中 viewer='orchestrator' OR 'public'。**但**没有任何事件的 visibility 数组里写 'orchestrator'；要让它真的"看到所有"，得改读路径 SQL 加特例（**反对这么做**——破坏了视角对称性） |
| 让 Resume 区分"新鲜 inflight（< 60s）"和"陈旧" | 改 step 3 循环：if (AgeSeconds < 60) 走"重发"路径（新加 `system.llm_inflight_resumed` 事件 + 不写 timeout）；否则走原 timeout 路径。注意 completed 扫描要把 `system.llm_inflight_resumed` 也算进去保证幂等 |

---

## 下一步

进 **阶段 4b — LLM 双管线**：读 `OpenAIChatClient` + `AILiveParserClient` + `AILivePromptAssembler` + `AgentRoster::ResolveProviderEndpoint` + `Content/Prompts/Parser/v1.txt`。告诉我可以开始，我把它写到 `Learn/04b-llm-pipeline.md`。
