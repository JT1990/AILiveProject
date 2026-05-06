# 阶段 4c — 投影 / 登记 / Resume / Delete

读：

```
Private/Memory/AILiveEventStoreSubsystem.cpp
   :2405-2536  ProjectCommitments_LockHeld
   :2543-2585  ProjectVoteHistory_LockHeld
   :2592-2732  ProjectAllianceState_LockHeld
   :2741-3074  ProjectAgentViewState_LockHeld
   :3082-3125  RebuildProjections（主入口）
   :3195-3355  ResumeFromGameId（4a 已读，本阶段对照投影联动）
   :3366-3501  TriggerDeleteExecuted（三步跨库）
   :765-1234   读 API 的 visibility EXISTS 子句（节选）
Public/Memory/AILiveAgentRegistry.h        (~25 行 — 单一文档化契约)
Private/Memory/AILiveAgentRegistry.cpp     (~165 行 — agent_registry 唯一授权 writer)
Public/Memory/AILiveListenerFilter.h       (~13 行 — 注意是 moderator stub，不是可见性 SQL)
Private/Memory/AILiveListenerFilter.cpp    (~22 行 — MVP 直通 passthrough)
```

> **导览修正**：阶段 0-3 的导览里说 `AILiveListenerFilter` 是「viewer 可见性门控的 SQL where 子句生成器」——**那是错的**。可见性门控 SQL **直接内嵌在 EventStore 读 API**（`EXISTS (SELECT 1 FROM event_visibility ...)` 的子句重复了 9 次）；`ListenerFilter` 是 §5.3「intended → public 衍生前的 moderator stub」，目前 MVP 直通。本文件第 9 节会把这两个分清。

读完这一阶段你应当能回答：

1. `RebuildProjections` 怎么保证 4 个 reducer **要么全成功要么全回滚**？为什么 `ProjectAgentViewState` 必须最后跑？
2. `ProjectCommitments` 是怎么做"二维分派"的（event_type × speech_act_type）？
3. `agent_registry` 表谁能 UPDATE？为什么不让业务代码直接改？
4. `TriggerDeleteExecuted` 在 Step 2 和 Step 3 之间崩溃，会留下什么"悬空状态"？

---

## 1. 派生表的整体形状

```
events 表（append-only，已上锁的 single source of truth）
   │
   │ RebuildProjections() 一次性重算下面 4 张派生表
   ▼
   ┌────────────────────┐
   │ commitments        │  ← Project_Commitments  （event_type × speech_act_type 分派）
   │ vote_history       │  ← Project_VoteHistory  （INSERT…SELECT + json_extract）
   │ alliance_state     │  ← Project_AllianceState（按 alliance_id 内存聚合）
   │ agent_view_state   │  ← Project_AgentViewState（依赖前 2 张 → 必须最后跑）
   └────────────────────┘
```

**派生 = 全量重建**。每次 RebuildProjections：

1. `DELETE FROM <table> WHERE game_id = ?1`
2. 全量从 `events` 重建

不做增量——理由：

- **简单 > 性能**。1000 events × 几张表，<100 ms 重算，比维护 invalidation 模型靠谱。
- **rebuild = 修复**。任何投影 bug 修一行 cpp，重启 PIE 就一致。
- **新增一种 event 不影响现有 reducer**——只在相关 reducer 里加 `case`，老数据自然兼容。

> 性能假设之后会失效（10 万 events 时单 reducer 重建可能 500 ms+）。**当前 MVP 不优化**，但记住：以后想引入"基于 last_seq 的增量"时，4 个 reducer 都得改成"读 last_built_seq → 处理增量 events"，工作量 ≥ 当前 reducer 总代码量。

---

## 2. `RebuildProjections` 主入口（cpp:3082-3125）

```cpp
bool RebuildProjections()
{
    if (!IsGameOpen() || !Db.IsValid()) return false;
    
    FScopeLock Lock(&WriteMutex);
    
    if (!Db.Execute("BEGIN IMMEDIATE;")) return false;
    
    bool bOk = ProjectCommitments_LockHeld()        // 1
            && ProjectVoteHistory_LockHeld()         // 2
            && ProjectAllianceState_LockHeld()       // 3
            && ProjectAgentViewState_LockHeld();     // 4 ← 依赖 1+2
    
    if (bOk && !Db.Execute("COMMIT;")) bOk = false;
    if (!bOk) {
        Db.Execute("ROLLBACK;");
        return false;
    }
    return true;
}
```

### 2.1 单一 BEGIN IMMEDIATE 包 4 个 reducer

**短路求值 + 单事务**：

- **短路**：第一个 false 就跳过后续——不浪费 SQL。
- **单事务**：要么 4 张表一致更新 + COMMIT，要么 ROLLBACK 全丢。**不存在"commitments 重建了但 agent_view_state 没"的中间态**。

为什么这是关键：`ProjectAgentViewState` **直接 SELECT 自 commitments 和 vote_history 表**（cpp:2898-2905），如果它们是上一轮的旧数据，agent_view_state 就会脏读。单事务保证 4 张表都基于同一个 events 快照。

### 2.2 `&&` 顺序固定不能换

```
ProjectCommitments → ProjectVoteHistory → ProjectAllianceState → ProjectAgentViewState
                                                                    └─ 依赖前两张表
```

注释（cpp:2738-2739）：

> *"必须在 ProjectCommitments / ProjectVoteHistory 之后调用——本步直读那两张表。"*

任意一步失败都 ROLLBACK——不会留半投影。

### 2.3 由谁触发？

```
Director 在每个 tick 末尾的 Stage D 异步调用 → 阶段 5 看
ResumeFromGameId 末尾兜底调用 → 4a 已知
控制台手动调用：AILive.Memory.RebuildProjections（cpp:4426-4441）
```

---

## 3. `ProjectCommitments_LockHeld`（cpp:2405-2536）

最复杂的一个 reducer。**一次 SELECT + 内存分派 + 逐行 INSERT**。

### 3.1 候选事件 SELECT

```sql
SELECT seq, round_no, actor, event_type, speech_act_type, payload
FROM events WHERE game_id = ?1
  AND event_type IN ('vote','alliance_propose','alliance_accept',
                     'speech.public','speech.intended')
ORDER BY seq;
```

注意：`speech.intended` 也被纳入候选——因为 promise/claim/deny 的承诺**在意图阶段就建立**了，不必等到说出口。这是 §6 commitment 语义里的一个微妙点。

### 3.2 二维分派：event_type × speech_act_type（cpp:2470-2503）

```cpp
if (EventType == "vote") {
    CommitmentType = "vote_for";
    Target = JsonGetString(P, "target");
}
else if (EventType == "alliance_propose" || EventType == "alliance_accept") {
    CommitmentType = "alliance";
    Target = JsonGetString(P, "alliance_id");
}
else  // speech.public / speech.intended → 由 speech_act_type 决定
{
    if (SpeechAct == "commit")     CommitmentType = "promise";
    else if (SpeechAct == "claim") CommitmentType = "claim_role";
    else if (SpeechAct == "deny") {
        CommitmentType = "deny";
        Target = ExtractDenyTarget(P);   // payload.deny_target / 否则首个 mentioned actor
    }
    else continue;   // 其它 speech_act_type 不入 commitments
}
```

**5 种 commitment_type**：vote_for / alliance / promise / claim_role / deny。其它（如 `chat`, `narrate`, `react`）直接 skip——这就是为什么 commitments 表行数 << events 表行数。

### 3.3 INSERT OR IGNORE（cpp:2442-2445）

```sql
INSERT OR IGNORE INTO commitments
  (game_id, agent_id, round_no, seq, commitment_type, target, text, status)
VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, 'active');
```

`OR IGNORE`：如果同 (game_id, seq) 已经有行（理论上不会，因为前面 DELETE 了），跳过不报错。**`status` 永远写死 `'active'`**——retract / contradicted 状态目前不在这个 reducer 维护（DevLog 留了"未来 status reducer 处理"的位置）。

### 3.4 NULL 绑定的写法（cpp:2515）

```cpp
if (Target.IsEmpty()) {
    Ins.SetBindingValueByIndex(6);   // bind NULL（不传值版本）
} else {
    Ins.SetBindingValueByIndex(6, Target);
}
```

**`SetBindingValueByIndex(idx)` 不传第二参数 = bind SQL NULL**，不是 bind 空字符串。空字符串 `''` 和 `NULL` 在 `WHERE target IS NOT NULL` 里语义不同——这种细节在投影对账时会咬人。

---

## 4. `ProjectVoteHistory_LockHeld`（cpp:2543-2585）

最简单的一个 reducer。**单 SQL 完事**。

```sql
INSERT OR IGNORE INTO vote_history (game_id, round_no, seq, voter, target)
SELECT game_id, round_no, seq, actor,
       COALESCE(json_extract(payload, '$.target'), '')
FROM events
WHERE game_id = ?1 AND event_type = 'vote';
```

### 4.1 `json_extract(payload, '$.target')` 内置函数

**SQLite 自己解析 payload JSON**——不用 C++ 做 JSON 反序列化。比 `ProjectCommitments` 里的循环 + ParseJsonObject 快十倍。

为什么 `ProjectCommitments` 不用 `json_extract`？因为它要做二维分派 + 多个 case 分支，纯 SQL 写起来反而臃肿。VoteHistory 只有一种事件类型一个字段，单 SQL 最清楚。

### 4.2 `COALESCE(json_extract(...), '')`

`json_extract` 缺字段返回 NULL。`COALESCE` 把 NULL → 空串，避免 `vote_history.target IS NULL` 的歧义。**但** `vote_history.target = ''` 表示"vote 但没指定目标"——空票，业务上是合法状态（弃权）。

---

## 5. `ProjectAllianceState_LockHeld`（cpp:2592-2732）

**两阶段**：先内存 TMap 聚合，再批 INSERT。

### 5.1 三种事件聚合到一行（cpp:2611-2620）

```cpp
struct FAllianceAcc {
    int64 ProposedAtSeq = 0;
    int64 AcceptedAtSeq = -1;     // -1 = 未发生
    int64 BetrayedAtSeq = -1;
    FString MembersJson = "[]";
    FString Terms;
    bool bHasPropose = false;
};
TMap<FString, FAllianceAcc> ByAlliance;   // key = alliance_id
```

聚合规则（cpp:2649-2676）：

- `alliance_propose`：**取首次**（`if (!Acc.bHasPropose)`）；填 ProposedAtSeq + Terms + MembersJson
- `alliance_accept`：**覆盖 last** AcceptedAtSeq
- `alliance_betray`：**覆盖 last** BetrayedAtSeq

为什么 propose 取首次？**联盟一旦提出就成立**——后续重复 propose 是噪音；accept/betray 取最后一次因为"最后状态"才是当前真值。

### 5.2 members 用 raw JSON 字符串保存（cpp:2658-2665）

```cpp
if (P->TryGetArrayField("members", Arr) && Arr) {
    FString MembersOut;
    const TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&MembersOut);
    FJsonSerializer::Serialize(*Arr, W);     // 重新序列化成字符串
    Acc.MembersJson = MembersOut;
}
```

**不解析成 TArray<FString> 再序列化**——直接用 UE JSON writer 序列化原数组。这样 members 列保持和原 payload 完全一致的 JSON 形态。

### 5.3 漏 propose 的兜底（cpp:2693-2696）

```cpp
const int64 ProposedSeq = A.bHasPropose
    ? A.ProposedAtSeq
    : (A.AcceptedAtSeq >= 0 ? A.AcceptedAtSeq : A.BetrayedAtSeq);
```

如果某条 alliance 只看到 accept / betray 没看到 propose（理论上不应该，但日志缺失或时序错乱时会发生），用首个出现的 seq 兜底。**不报错也不 skip**——因为下游业务不区分"完整生命周期"和"残缺记录"。

---

## 6. `ProjectAgentViewState_LockHeld`（cpp:2741-3074）

**最长**的 reducer（~330 行）。每个 agent 一行 snapshot。

### 6.1 输出 schema：8 列

```sql
INSERT INTO agent_view_state
  (game_id, agent_id, as_of_seq, alive_players, known_roles,
   my_commitments, vote_history, pending_intended)
```

5 段 JSON：

| 列 | 含义 | 数据源 |
|---|---|---|
| `alive_players` | JSON 数组 `["NPC01","NPC02",...]` | Roster - DeletedActors |
| `known_roles` | JSON 对象 `{"NPC01":"Mayor",...}` | events.role_assigned，按 viewer 过滤 |
| `my_commitments` | JSON 对象（key=seq）`{"42":{...}}` | commitments WHERE agent_id=Self |
| `vote_history` | JSON 数组 `[{"round_no":1,...}]` | vote_history WHERE voter=Self |
| `pending_intended` | JSON 数组 `[{"seq":...,"text_snippet":...}]` | speech.intended 没 paired public，最近 10 拍 |

### 6.2 Roster 解析的两层兜底（cpp:2761-2796）

```
1. SELECT agent_id FROM agent_calibration WHERE game_id = ?1   ← 优先
2. SELECT DISTINCT actor FROM events WHERE actor LIKE 'NPC%'   ← 兜底
3. 都空 → 直接 return true（不写任何行）
```

**为什么不报错？** 因为开局期 (`BeginGame` 后还没写 calibration) 没有 NPC，roster 自然空——调用 `RebuildProjections` 不应该 fail。

### 6.3 `as_of_seq = CachedLastSeq`（cpp:2800）

每次重建都用当前最末 seq 当快照锚点。下次重建会**全量 DELETE + 重写**——所以 `agent_view_state` 表里**只有最新一行**（per agent_id）。

> 想看历史 snapshot？目前没接口。要加的话需要新设计：每次 BeginTick 写 snapshot 而不是覆盖。

### 6.4 `known_roles` 的可见性手算（cpp:2937-2956）

**这一段是这个 reducer 最 tricky 的部分**——不能用 SQL EXISTS，因为是 per-agent 循环。

```cpp
for (RoleAssignment R : RoleEvents) {
    const bool bVisible = R.Viewers.Contains(AgentId)
        || R.Viewers.Contains("public")
        || R.Viewers.Contains("audience");
    if (!bVisible) continue;
    if (SeenActors.Contains(R.Actor)) continue;  // 同 actor 取首次 role
    SeenActors.Add(R.Actor);
    W->WriteValue(R.Actor, R.Role);
}
```

**手算可见性 = 把 event_visibility 表的语义在 C++ 里重做一遍**。理论上可以改成 SQL 子查询，但当前每个 NPC 看 RoleEvents 数组（10 NPC × 10 role 事件 = 100 比较），完全可以 cache 复用——当前为简单选 C++。

注意 `'audience'` 也算可见——这是 §6 角色公布机制里"非玩家观众"也能看的设计。

### 6.5 `pending_intended` 的 NOT EXISTS 反向连接（cpp:2906-2918）

```sql
SELECT seq, tick_no, json_extract(payload, '$.text'),
       json_extract(payload, '$.intended_action')
FROM events e
WHERE e.game_id = ?1 AND e.actor = ?2
  AND e.event_type = 'speech.intended'
  AND e.tick_no >= ?3                              -- 最近 10 拍（PendingTickFloor = LatestTickNo - 9）
  AND NOT EXISTS (SELECT 1 FROM events c
                  WHERE c.game_id = e.game_id
                    AND c.event_type = 'speech.public'
                    AND c.parent_event_id = e.event_id)   -- ← 关键
ORDER BY e.seq;
```

**"未抢中 floor"的判定 = 没有 speech.public 把它当 parent_event_id 引用**。speech.public 写入时会把对应的 speech.intended 的 event_id 填到 parent_event_id 列。**这是 events 表 parent_event_id 列的主用途**。

阶段 4b 的 PromptAssembler `ListMyPendingIntended` 调用走类似 SQL（cpp:1060-1067）；这里的版本是 reducer 内做窗口聚合，二者**SQL 略有不同但语义一致**。

---

## 7. `AILiveAgentRegistry`（registry.cpp）

**这是 `_meta.db.agent_registry` 表的唯一授权 writer**。

### 7.1 单一接口（h:23-24）

```cpp
namespace AILiveAgentRegistry {
    bool SyncRegistryFromLifecycle(FSQLiteDatabase& MetaDb, const FString& LifecycleEventId);
}
```

设计契约（h:21-22）：

> *"Business code MUST NOT issue `UPDATE agent_registry` directly — this is the only authorized writer (CI grep guard documented in DevLog)."*

业务代码任何时候想改 agent_registry，**只能**先写一条 `agent_lifecycle_events` 行，再调这个函数同步过去。**单一 writer = 没有"业务到处写状态机不一致"的风险**。

### 7.2 五种 lifecycle_type 分派（cpp:96-129）

```cpp
if (LifecycleType == "delete_executed") {
    UPDATE: status='deleted', deleted_at=<wall_clock>
}
else if (LifecycleType == "created" || "revived") {
    UPDATE: status='active', deleted_at=NULL
}
else if (LifecycleType == "archived") {
    UPDATE: status='archived'
}
else {
    // delete_proposed / delete_vetoed / unknown → 只 bump heartbeat
    UPDATE: last_updated_at=<wall_clock>
}
```

**`delete_proposed` 不立刻置 deleted**——这是社会博弈的"提名 → 投票 → 执行"流程的核心：提名只是动议，被投票才能变 deleted。

### 7.3 `INSERT OR IGNORE` 先建行再 UPDATE（cpp:74-94）

```cpp
INSERT OR IGNORE INTO agent_registry (agent_id, last_seen_game_id)
VALUES (?1, NULLIF(?2, ''));
```

**幂等开**：如果 agent 还没在 registry 里（比如 created 事件先到，agent 第一次出现），就先建行。`NULLIF(?2, '')` 把空 game_id 转成 NULL。**不报错也不 conflict**——保证后续 UPDATE 一定有行可以打中。

### 7.4 函数自己管事务（cpp:61-156）

```cpp
MetaDb.Execute("BEGIN IMMEDIATE;");
INSERT OR IGNORE...;
UPDATE agent_registry SET ...;
MetaDb.Execute("COMMIT;");
// 任何 failure → ROLLBACK
```

**caller 不需要外层包事务**——这是为什么 `TriggerDeleteExecuted` 的 Step 1 + Step 2 不能写在同一个 transaction 里（详见下一节）。

---

## 8. `TriggerDeleteExecuted`（cpp:3366-3501）

**三步跨 DB 协调**——4a 自检题 #4 提过，这里给执行层全貌。

### 8.1 三步链路

```
Step 1: INSERT _meta.db.agent_lifecycle_events
        ┌─────────────────────────────┐
        │ MetaDb BEGIN                │
        │   INSERT INTO ...           │
        │ MetaDb COMMIT               │
        └─────────────────────────────┘
                ↓
Step 2: UPDATE _meta.db.agent_registry via SyncRegistryFromLifecycle
        ┌─────────────────────────────┐
        │ MetaDb BEGIN  (函数自己开)  │
        │   INSERT OR IGNORE          │
        │   UPDATE agent_registry     │
        │ MetaDb COMMIT               │
        └─────────────────────────────┘
                ↓
Step 3: AppendEvent system.delete_executed → game.db
        ┌─────────────────────────────┐
        │ Db BEGIN  (AppendEvent 内部) │
        │   INSERT INTO events        │
        │ Db COMMIT                   │
        └─────────────────────────────┘
```

### 8.2 为什么不能合一个事务？（注释 cpp:3361-3363）

> *"Two SQLite connections → no shared transaction; ordering guarantees consistency for the L2 acceptance scenario."*

**Db 和 MetaDb 是两条独立 SQLite 连接**——一条事务不能跨连接。所以三步都是各自的 BEGIN/COMMIT，**不可原子**。

### 8.3 顺序选择的意图

为什么是 _meta lifecycle → _meta registry → game events 这个顺序？

- **Step 1 先**：lifecycle_events 是审计事实；registry 是它的派生 / 索引。先写事实再投影。
- **Step 2 后**：用 SyncRegistryFromLifecycle 读 lifecycle 行 → 投影到 registry。
- **Step 3 最后**：把 system.delete_executed 落到本局 events——可见的"对外宣告"。

**自检题 #4 答案**：Step 2 和 Step 3 之间崩溃 → 

- _meta.db.agent_lifecycle_events ✅ 有了
- _meta.db.agent_registry.status ✅ 已置 'deleted'
- game.db.events 系统事件 ❌ 没写

**结果**：metadata 层认为 agent 已死；game 层不知道。下次 BeginGame / Resume 不会自动修——理论上 Resume 应能识别 lifecycle 行没有对应 game event，但当前 ResumeFromGameId **不处理这种情况**。注释 cpp:3490-3492 也承认这是缺口：

> *"lifecycle row %s already committed (compensation deferred — see DevLog)"*

**业务影响**：罕见崩溃后下一局玩家会看到 agent_registry 显示 deleted 但 game 里没人看到死亡公告——目前接受这个风险。

### 8.4 visibility 校验（cpp:3385-3395）

```cpp
const TArray<FString> AgentIdAsViewer = { InAgentId };
if (!ValidateVisibility(AgentIdAsViewer, TmpErr)) {
    return -1;
}
```

把 agent_id 当成 visibility 数组校验——**确保 agent_id 是合法 viewer 字符串**（不为空、不含特殊字符等）。注意校验的是 agent_id 本身的字符串合法性，不是 InTombstoneVisibility。

### 8.5 system event payload 的拼接（cpp:3477-3484）

```cpp
Sys.PayloadJson = FString::Printf(
    "{\"text\":%s,\"lifecycle_event_id\":\"%s\",\"agent_id\":\"%s\",",
    *AILiveUtil::EscapeJsonString(...), *LifecycleEventId, *InAgentId);
Sys.PayloadJson += FString::Printf(
    "\"reason_summary\":%s}",
    *AILiveUtil::EscapeJsonString(InReasonSummary));
```

**手拼 JSON 字符串**——用 `EscapeJsonString` 工具转义。注意：`agent_id` 没用 `EscapeJsonString` 因为已经过 ValidateVisibility 保证不含特殊字符。**这种"半手工"的 JSON 拼接是潜在 bug 来源**——比如 InAgentId 含双引号会破 JSON。当前业务保证 agent_id 形如 `NPCxx` 安全。

---

## 9. `ListenerFilter` 的真相（cpp:9-21）

```cpp
namespace ListenerFilter {
    FString Apply(const FString& InIntendedPayloadJson, float& OutScore) {
        OutScore = 0.f;
        TSharedPtr<FJsonObject> Obj;
        const TSharedRef<TJsonReader<TCHAR>> Reader =
            TJsonReaderFactory<TCHAR>::Create(InIntendedPayloadJson);
        if (FJsonSerializer::Deserialize(Reader, Obj) && Obj.IsValid()) {
            FString T;
            if (Obj->TryGetStringField("text", T)) return T;
        }
        return FString();
    }
}
```

### 9.1 它是什么 / 不是什么

**不是**：可见性门控的 SQL where 子句生成器（导览写错了）。
**是**：principles §5.3「intended → public 衍生前的 moderator stub」。

签名：把 `intended.text` 抽出来透传，OutScore=0。**完全 passthrough**——MVP 版本不做任何过滤。

### 9.2 为什么存在这个空函数？

H 注释（h:8-12）：

> *"MVP 直通：返回 InIntendedPayloadJson.text 原文，OutScore=0.0。下阶段引入真 LLM 时本函数保持签名不变，RunTick 调用点稳定。"*

**它是一个稳定的接缝**：以后 §5.3 上线后，把这个函数体换成真正的 LLM moderator（检查 hate speech / persona leak / 越狱），**不影响 RunTick 的调用点**（阶段 5 看 cpp:1130）。

```cpp
// Act02RuleReceiveDirector.cpp:1130
FilteredText = ListenerFilter::Apply(WinnerIntended.PayloadJson, FilterScore);
```

### 9.3 真正的可见性 SQL 在哪里

**直接内嵌在 EventStore 读 API 的 SQL 里**——9 个查询函数共享同一段 EXISTS 子句模板：

```sql
WHERE e.game_id = ?1 AND e.actor = ?2
  AND EXISTS (SELECT 1 FROM event_visibility v
              WHERE v.event_id = e.event_id AND v.viewer IN (%s))
ORDER BY e.seq;
```

`%s` 会被替换成 `?N, 'public'`——也就是 viewer 自己 + 'public' 通配。

**例外**：`ListVotes` 用 `v.viewer = 'public'`（cpp:1232-1233）——**不**让 viewer 自己出现，只看公开投票。这是为防止"私下投票同时让自己看见但别人看不见"的破坏性写法漏过。

**没有独立的 "ListenerFilter" 命名空间做这个**——9 处 EXISTS 子句是**模板字符串重复**，不是函数复用。这是个可以重构的目标（提取一个 `BuildVisibilityClause(viewer)` 工具），但当前代码量小、可读性 OK，没动。

---

## 10. 自检题答案

### 10.1 自检题 #1

> RebuildProjections 怎么保证 4 个 reducer 要么全成功要么全回滚？为什么 ProjectAgentViewState 必须最后跑？

**单 BEGIN IMMEDIATE 包 4 个 reducer + && 短路**（cpp:3094-3104）：

```cpp
Db.Execute("BEGIN IMMEDIATE;");
bool bOk = ProjectCommitments_LockHeld() && ProjectVoteHistory_LockHeld()
        && ProjectAllianceState_LockHeld() && ProjectAgentViewState_LockHeld();
if (bOk) Db.Execute("COMMIT;");
else     Db.Execute("ROLLBACK;");
```

任何一步 return false → 后续 reducer 不跑 → ROLLBACK 全部 DELETE。

**ProjectAgentViewState 必须最后**：它直接 SELECT `commitments` 和 `vote_history` 表（cpp:2898-2905 准备语句），如果跑在前面就读不到本轮重建的数据，会基于上一轮的脏数据生成 snapshot。

### 10.2 自检题 #2

> ProjectCommitments 怎么做"二维分派"？

**SQL 一次拉 5 种候选 event_type，C++ 逐行二次分派**：

```
event_type='vote'                        → commitment_type='vote_for'
event_type IN ('alliance_propose','alliance_accept') → 'alliance'
event_type='speech.public' or 'speech.intended':
    speech_act_type='commit'             → 'promise'
    speech_act_type='claim'              → 'claim_role'
    speech_act_type='deny'               → 'deny'
    其他                                  → skip（不入 commitments）
```

5 种 commitment_type 全部 status='active' 写入；其他 speech_act_type 直接 continue。

### 10.3 自检题 #3

> agent_registry 表谁能 UPDATE？为什么不让业务代码直接改？

**只有 `AILiveAgentRegistry::SyncRegistryFromLifecycle` 能 UPDATE**——这是唯一授权的 writer（h:21-22 + DevLog 文档化的 CI grep guard）。

**为什么单 writer**：

1. **状态机不一致风险**：业务代码各处写 `UPDATE agent_registry SET status=...` 容易遗漏 last_updated_at / deleted_at 等关联字段。
2. **审计性**：每次 status 变更必须先有 lifecycle_events 行（事件溯源对 metadata 的延伸）。直接 UPDATE 等于绕过审计。
3. **不可变性**：lifecycle_events 是 append-only；registry 是 lifecycle 的派生投影。**和 events ↔ commitments 的关系完全平行**——你不会绕过 events 直接写 commitments，同理不绕过 lifecycle 写 registry。

### 10.4 自检题 #4

> TriggerDeleteExecuted 在 Step 2 和 Step 3 之间崩溃，会留下什么悬空状态？

**结果**：

- _meta.db.agent_lifecycle_events ✅ 已 INSERT（Step 1 commit）
- _meta.db.agent_registry.status ✅ 已置 'deleted'（Step 2 commit）
- game.db.events.system.delete_executed ❌ 没写（Step 3 失败）

**业务症状**：

- 跨局视图看：agent 已死亡（registry 是真相源）
- 本局事件流：没有死亡公告 → NPC 们不会知道、PromptAssembler 也不会把 'deleted' 状态注入 prompt
- agent_view_state.alive_players 取自 events.system.delete_executed → **此 agent 仍在 alive 集合**

**目前不修**——cpp:3490-3492 的 log 注释承认这个 compensation 缺口。理论上的修法是 Resume 时扫 _meta.db.agent_lifecycle_events 找 type='delete_executed'，对每条检查 game.db 是否有对应 system.delete_executed，缺失则补写。

---

## 11. 关键不变量速查表

| # | 不变量 / 约束 | 在哪强制 |
|---|---|---|
| 1 | RebuildProjections 4 reducer 单事务包 + 顺序固定 | EventStoreSubsystem.cpp:3082-3125 |
| 2 | ProjectAgentViewState 必须最后跑（依赖 commitments + vote_history） | EventStoreSubsystem.cpp:2738-2739 |
| 3 | agent_registry 唯一 writer = SyncRegistryFromLifecycle | AILiveAgentRegistry.h:21-22 + CI grep guard |
| 4 | TriggerDeleteExecuted 三步不可原子（两条 SQLite 连接） | EventStoreSubsystem.cpp:3361-3363 |
| 5 | 派生 = 全量重建（不增量），DELETE → INSERT | 4 个 reducer 第一步都是 DELETE |
| 6 | ListenerFilter 是 moderator stub，不是可见性 SQL | AILiveListenerFilter.h:8-12 |
| 7 | 可见性 SQL 直接内嵌 9 处读 API；ListVotes 例外只看 'public' | EventStoreSubsystem.cpp:765-1234 |
| 8 | known_roles 在 reducer 内手算可见性（per-agent 循环），不走 SQL | EventStoreSubsystem.cpp:2937-2956 |
| 9 | pending_intended 用 NOT EXISTS + parent_event_id 反向连接 | EventStoreSubsystem.cpp:2906-2918 |
| 10 | INSERT OR IGNORE + bind NULL（不传第二参数）正确表示 SQL NULL | reducer 通用模式 |

---

## 12. 扩展任务推演

| 改动 | 至少要动 |
|---|---|
| 新增第 5 张派生表（如 `friendship_state` 跟踪两两关系） | (1) SchemaMigration.cpp 加 DDL；(2) EventStoreSubsystem.h 加 `ProjectFriendship_LockHeld` 声明；(3) cpp 实现 reducer（DELETE → SELECT events → 内存聚合 → INSERT）；(4) `RebuildProjections` 的 `&&` 链加这一步；(5) 决定它在 ProjectAgentViewState 之前还是之后——如果 agent_view_state 要消费 friendship，就放它前面 |
| 让 ProjectCommitments 支持 retract（事件 `commitment_retract` 把对应 commitment 改 status='retracted'） | (1) 候选 event_type IN 加上 `'commitment_retract'`；(2) 新增分派分支 `if (EventType=="commitment_retract") UPDATE commitments SET status='retracted' WHERE seq=payload.target_seq` ——**不再纯 INSERT**，要先 INSERT promises 再 UPDATE retract；(3) 注意：reducer 全量重建模式下，UPDATE 顺序由 ORDER BY seq 保证（先 INSERT promise 后 UPDATE retract） |
| 修 TriggerDeleteExecuted 的悬空 lifecycle 缺口 | (1) ResumeFromGameId step 1.5 加：从 _meta.db 拉所有 lifecycle_event_type='delete_executed' 的 event_id；(2) 对每条查 game.db 是否有 `system.delete_executed` 满足 `payload.lifecycle_event_id = ?`；(3) 缺失则用同样 payload 补 AppendEvent；(4) 关于 wall_clock：补的事件时间 ≠ lifecycle row 时间——payload 里加 `compensated=true` 标记 |
| 让 ListenerFilter 真的过滤（不再 passthrough） | (1) 改 ListenerFilter.cpp 调 OpenAIChat::RequestBlocking——**不要**直接调，因为 RunTick 已经在 worker 线程，再调 LLM 是双层调度；改成传入 caller 已经准备好的 LLM client 函数对象；(2) OutScore 由 LLM 输出的 risk score 填；(3) RunTick 拿到 score > threshold 时改写 visibility 或丢弃事件——决策不在 ListenerFilter 内做；(4) **caveat**: 不要在这一步做"修改 text"——审计语义要求 intended ↔ public 文本一致性，moderator 只能 yes/no，不能改写 |

---

## 13. 阅读追溯：把 4a + 4b + 4c 串起来

完成本阶段后，下面这个完整的 tick 链路你应该能口述：

```
Director.RunTick()
  ↓ EventStore.BeginTick(N)                  [4a: 写 tick_anchor，回滚保护 OldTick]
  ↓ for each NPC:
      Async(ThreadPool, RunAgentTickInWorker)
         ↓ AssembleSystemPrompt + AssembleUserPrompt   [4b: 7 段 + must-keep]
         ↓ Reasoner LLM                                [4b: OpenAIChat 同步 HTTP]
         ↓ Parser LLM                                  [4b: 4 通道 → 严格 JSON]
         ↓ FParseResult
  ↓ TickLLMAwait poll TFutures                [阶段 5 看]
  ↓ GatherTickAndResolveFloor (5 stages)      [阶段 5 看]
       Stage A: AppendEventsAtomically([speech.intended, speech.note, bid, scratchpad])
                                              [4a: lock-free 校验 + locked 写入]
       Stage B: ResolveFloor()
       Stage C: AppendEvent(speech.public)+(tick_resolved)
                                              [4a: parent_event_id 链回 intended]
                ListenerFilter::Apply(intended) → text   [4c: passthrough]
       Stage D: Async RebuildProjections     [4c: 单事务 4 reducer]
       Stage E: TriggerMinimaxSpeech(...)    [阶段 6 看]
```

如果哪一步描述不清、或不知道某个数据从哪里来——回头查对应阶段。

---

## 下一步

进 **阶段 5 — 主循环执行追踪**：把 `Act02RuleReceiveDirector.cpp` 1466 行从 BeginPlay 走到 CompleteAct02，包括 5 个 Stage、worker 线程的对象生命周期、UObject 跨线程访问的注意点。告诉我可以开始，我把它写到 `Learn/05-runloop-trace.md`。
