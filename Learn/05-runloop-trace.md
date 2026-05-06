# 阶段 5 — 主循环执行追踪

读：

```
Public/Acts/Act02RuleReceiveDirector.h     (255 行 — 状态机 / UPROPERTY / 内部结构体)
Private/Acts/Act02RuleReceiveDirector.cpp  (1466 行 — 全文必读)
```

读完这一阶段你应当能回答（自检题 #1 也是这个）：

1. 从用户按 [2] 键开始，到 NPC1 嘴巴动起来播音频，链路上至少 8 个函数 + 它们的归属类，并标出哪几步在 worker 线程。
2. `RunTick → RunAgentTickInWorker → TickLLMAwait → GatherTickAndResolveFloor` 各自跑在哪个线程？UObject 在 worker 线程能 touch 吗？
3. `GatherTickAndResolveFloor` 的 5 个 Stage 都做什么？哪一步可能 `FailAct02`，哪一步只 log warning？
4. `tick_resolved` 和 `tick_audit` 为什么要拆成两条事件而不是一条？
5. seed 冷场（无 winner）的话，会卡在 GatedAwaitNext 等手按 [3] 吗？

---

## 1. 状态机的整体形状

```
              Idle ─[2]─→ PrescatterToTV
                            │
                            │ all NPCs idle + 0.5s settle
                            ▼
                      SeedDispatch ────► RunTick (worker 线程发车) ────► SeedAwait
                                                                          │
                                              Future 全 ready / 200s 超时 │
                                                                          ▼
                                                       GatherTickAndResolveFloor
                                                              │
                                          有 winner ──────────┤────────── 冷场
                                                              ▼                  
                                                          SeedSpeak              
                                                       (TTS + 30s watchdog)      
                                                              │                  
                                                              ▼                  
                                                       GatedAwaitNext ─[3]┐      
                                                                          │      
                                                                   StartReactionPhase
                                                                          │
                                                                          ▼
                                                                  ReactionDispatch
                                                                          │
                                                                          ▼
                                                                  ReactionAwait
                                                                          │
                                              ┌───────────────────────────┘
                                              ▼
                                      GatherTickAndResolveFloor
                                              │
                                              ▼
                                       ReactionSpeak (TTS)
                                              │
                                              ▼
                                       AdvanceReactionRound ──── 还有轮 ──→ ReactionDispatch
                                              │
                                              ▼ 已完成 ReactionRoundCount
                                          CompleteAct02 → Idle
```

**两个手动按键入口**：
- `[2]` — Idle 状态进入 PrescatterToTV → 自动跑完 seed
- `[3]` — GatedAwaitNext 状态进入反应轮（seed 冷场会自动跳过这一步）

**两个 Watchdog**：
- LLM 200s GT watchdog（TickLLMAwait）— 强制收菜，未 ready 视作 abstain
- 语音 30s watchdog（TickSpeakWatchdog）— 强制 finished + failed=true

---

## 2. `BeginPlay`（cpp:84-140）的三件事

```cpp
1. 加载默认资产（如果 UPROPERTY 没指定）
   NPCMoverClass     ← /Game/Blueprints/SandboxCharacter_Mover_C
   NavTargetClass    ← /Game/Blueprints/Markers/BP_NavTarget_C
   NavLookTargetClass← /Game/Blueprints/Markers/BP_NavLookTarget_C
   ScatterQueryAsset ← /Game/AI/EQS/EQS_ScatterAroundTarget
   Roster            ← AILiveAgentRoster::GetDefaultRoster()  (10 NPC)

2. 初始化运行时
   CacheInitialNPCTransforms()
       ↓ GetAllActorsOfClass(NPCMoverClass) 拿全部 NPC pawns
   ResolveTargetActors(true)
       ↓ TSoftObjectPtr.LoadSynchronous() → 失败则 by class+label 兜底
   BindRoster()
       ↓ 把 Roster[i] 按 NPCActorLabel 配对到 world 里的 actor

3. 自动衔接 Act01
   for each Act01 director in world:
       OnAct01Completed.AddDynamic(this, &HandleAct01Completed)
```

**`HandleAct01Completed` = 自动调 `BeginAct02()`**——这就是你按 [1] 跑完 Act01 视频后自动进 Act02 的原因（cpp:142-146）。

---

## 3. `Tick`（cpp:154-196）的调度模型

```cpp
StateElapsed += Dt;

// 输入：两个按键各自 guard 一个状态
if (state == Idle             && [2] just pressed) BeginAct02();
if (state == GatedAwaitNext   && [3] just pressed) StartReactionPhase();

// 状态分发：每帧只跑当前状态的 Tick 函数
switch (state) {
    PrescatterToTV:                  TickPrescatter(Dt);
    SeedAwait | ReactionAwait:       TickLLMAwait(Dt);
    SeedSpeak | ReactionSpeak:       TickSpeakWatchdog(Dt);
    default:                          ;  // Idle / Dispatch / GatedAwaitNext 不需要每帧 work
}
```

**Dispatch 状态不在 switch 里**——因为 `RunTick()` 在状态切到 Dispatch 时一次性发车，立即把状态推到 `Await`。**`SeedDispatch` / `ReactionDispatch` 在状态机里只存在一帧**（`StartSeedPhase` / `StartReactionPhase` / `AdvanceReactionRound` 内部直接 `state = Dispatch; RunTick(); state = Await;`）。

`StateElapsed` 累计当前状态停留时间，给 watchdog 判超时用。任何 state 切换都要 `StateElapsed = 0.f` 重置。

---

## 4. `BeginAct02`（cpp:198-249）的前置校验

```cpp
1. state == Idle 防重入
2. ResolveTargetActors(false) 重试一次 NavTarget / NavLookTarget
3. InitialNPCTransforms 空 → CacheInitialNPCTransforms() 重补
4. BindRoster() 失败 → FailAct02
5. EventStore 必须可用且开局
   - !Store              → FailAct02
   - !Store.IsGameOpen() → BeginGame(<timestamp game_id>)，失败则 FailAct02
6. StartPrescatter()
```

**`game_id` 的格式（cpp:232）**：

```cpp
const FString GameId = FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S"));
```

例：`20260505_143022`。**没有冲突保护**——同一秒内多次 BeginGame 会撞名（实际不可能，但理论上需要防御）。

**关键设计**（cpp:243-247 注释）：

> *"不 ResetToInitialPositions：ACT02 复用 ScatterMover 把 NPC 散到 TV 前。若 ACT01 已跑过，NPC 已在 TV 前，scatter 仅做小幅调整；若 ACT02 单独触发，scatter 通过 NavMesh 寻路。否则瞬移回 BeginPlay 位置会让用户视觉上误以为是 ACT01 重新触发。"*

也就是说 **Act02 不重置 NPC 位置**——尊重当前世界状态，只用 EQS 把它们重新排到 TV 前。

---

## 5. `StartPrescatter` / `TickPrescatter`（cpp:465-544）

### 5.1 一次性发车（cpp:465-493）

```cpp
StartPrescatter():
    state = PrescatterToTV
    StateElapsed = 0
    bArrivalSettling = false
    
    UAILiveProjectScatterMover::ScatterNPCsAroundTarget(
        ScatterQueryAsset, NavTargetCached, NPCArray, NavLookTargetCached, this);
    // ScatterMover 内部对每个 NPC 用 EQS 找 spot → AIController.MoveTo
```

### 5.2 等待到位（cpp:495-527）

```cpp
TickPrescatter(Dt):
    if StateElapsed > 30s        → FailAct02("prescatter movement timeout")
    if StateElapsed < 2s         → return    // ScatterDispatchDelaySeconds，等 EQS 解算 + MoveTo 启动
    
    if AreAllNPCsIdle():
        if !bArrivalSettling:
            bArrivalSettling = true
            MovementSettleElapsed = 0
        else:
            MovementSettleElapsed += Dt
            if MovementSettleElapsed >= 0.5s:    // ArrivalSettleSeconds
                StartSeedPhase()
    else:
        bArrivalSettling = false   // 走偏 → 重置 settle 计时
```

**两层等待**：

1. **Idle 检测**：`AIController.GetMoveStatus() == EPathFollowingStatus::Idle`（cpp:541）
2. **Settle 计时**：连续 0.5s 都 Idle 才进 Seed——避免 Move 还在做最后微调时误判

### 5.3 `StartSeedPhase`（cpp:546-555）

```cpp
CurrentRound = 0;       // 0 = seed 标记
SceneState = SeedDispatch;
RunTick();
SceneState = SeedAwait;
StateElapsed = 0;
```

**这是 RunTick 的第一次调用**——后续 `StartReactionPhase` / `AdvanceReactionRound` 都走类似 3 行模板。

---

## 6. `RunTick`（cpp:793-888）

**最核心的函数之一**。一次 RunTick = **一拍 LLM 决策的整批发车**。

### 6.1 入口清场（cpp:795-797）

```cpp
CurrentTickInflight.Reset();
ActorToIntendedSeq.Reset();
ActorToRequestId.Reset();
```

`CurrentTickInflight` 是 `TArray<FInflightTick>`——保存每个 NPC 的 RequestId + Future。Reset 是为了让本拍干净。

### 6.2 BeginTick：tick 号自增（cpp:806-813）

```cpp
const int64 NewTick = Store->GetCurrentTickNo() + 1;
const int64 AnchorSeq = Store->BeginTick(NewTick);
if (AnchorSeq <= 0) FailAct02(...);
```

**Director 不持有 tick_no**（注释 cpp:806）：

> *"决策 #9：Director 不维护 CurrentTickNo 副本；以 EventStore 为权威源。"*

`BeginTick` 内部保护 OldTick 回滚（4a 第 8 节）；**这里只读 `GetCurrentTickNo()` 自加再传给 `BeginTick`**——单一 source of truth。

### 6.3 加载 GameRule（cpp:815-816）

```cpp
FString GameRule;
LoadGameRule(GameRule);   // 读 Docs/playscript/zombie-game-rule.md，失败则 fallback
```

**每拍重读**——文件可能在 PIE 期间被改（DevLog 调试用）。性能影响可忽略（一次 ~5ms 同步 IO）。

### 6.4 per-NPC 发车循环（cpp:822-883）

```cpp
constexpr float kPerAttemptTimeoutSec = 45.f;   // 决策 #3

for (FAct02NPCRuntime& N : NPCs) {
    if (!N.Config.Battle.bAlive) continue;     // 死了的不发车
    
    // ① 装 prompt
    SystemPrompt = BuildReasonerSystemPrompt(N.Config, GameRule, CurrentRound);
    UserPrompt   = AILivePromptAssembler::AssembleUserPrompt(Store, N.Config, Opt);
    
    // ② 解析 endpoint
    Ep = AILiveAgentRoster::ResolveProviderEndpoint(N.Config.Provider);
    
    // ③ 发起请求前先写 system.llm_inflight（principles §5.4 配对协议）
    SysHash   = Sha256Fingerprint(SystemPrompt);
    UserHash  = Sha256Fingerprint(UserPrompt);
    StartedAt = FDateTime::UtcNow().ToIso8601();
    
    Pre = FAILiveEvent {
        Actor = "orchestrator",
        EventType = SystemLLMInflight,
        Visibility = ["system"],
        PayloadJson = BuildLLMInflightPayloadJson(NPCIdx, RequestId, SysHash, UserHash, StartedAt),
    };
    if (Store->AppendEvent(Pre) <= 0)
        FailAct02("in-flight write failed");
    
    // ④ Async ThreadPool 发车 → 单个 TPromise/TFuture
    F.Future = Async(EAsyncExecution::ThreadPool,
        [CfgCopy, ReqId, SystemPrompt, UserPrompt]() -> FAgentTickResult {
            return RunAgentTickInWorker(CfgCopy, ReqId, SystemPrompt, UserPrompt, 45.f);
        });
    
    CurrentTickInflight.Emplace(MoveTemp(F));
}
```

**inflight 配对协议是关键**：先写 inflight 事件，**再**发请求。理由：

- 任何崩溃 / EndPlay 之后 Resume，都能扫到 `system.llm_inflight` 行
- payload 里有 `request_id` + `started_at` + 双 prompt hash——足以法医还原
- 4a 阶段的 ResumeFromGameId 就靠这个事件来发现"未配对完成"的 inflight

**worker 线程的 lambda 捕获是 by-value 的 `CfgCopy`**——绝对不能 by-ref，因为 `N` 是 local ref，函数返回前 worker 可能还在跑。

### 6.5 `system.llm_inflight` 失败 = 整拍 fail

```cpp
if (PreSeq <= 0) {
    FailAct02(FString::Printf(...));
    CurrentTickInflight.Reset();
    return;
}
```

**inflight 写不进去就放弃整拍**——因为没有 inflight 事件，后续 Resume 找不到这个请求，会留下 phantom 请求。**这是不可降级的硬约束**。

---

## 7. `RunAgentTickInWorker`（cpp:890-951）—— **worker 线程**

### 7.1 关键约束：`static` + 不 touch UObject

```cpp
static FAgentTickResult AAct02RuleReceiveDirector::RunAgentTickInWorker(
    const FNPCAgentConfig& Cfg,
    const FString& RequestId,
    const FString& SystemPrompt,
    const FString& UserPrompt,
    float PerAttemptTimeoutSec)
```

**`static` 是故意的**——保证函数内部**不能** `this->XXX`。函数签名 + 参数全部都是 POD / FString / FNPCAgentConfig（值类型）。**worker 线程不能访问任何 UObject**，否则 GC 期间崩溃。

> 你以后想给 worker 加输入参数，**绝不要**传 `AActor*` / `UObject*` / `TWeakObjectPtr`。需要某个 actor 的属性，先在 GT 抽出来 by value 传进去。

### 7.2 三轮 reject sample 重试（cpp:907-947）

```cpp
for (int32 Attempt = 1; Attempt <= 3; ++Attempt)
{
    OpenAIChat::FRequest Req;
    Req.SystemPrompt = SystemPrompt;
    Req.UserPrompt   = UserPrompt;
    Req.bResponseFormatJson = false;     // 决策 #2：§A.1 是 tagged 文本不是 JSON
    Req.TimeoutSec   = 45.f;
    Req.Temperature  = 0.7f;
    if (Cfg.Provider == ELLMProvider::GLM)
        Req.ThinkingType = "disabled";    // GLM 必须显式禁用 thinking
    
    R = OpenAIChat::RequestBlocking(Req);    // 阻塞这个 worker 线程
    if (!R.bSuccess) {
        Out.FailureReason = (R.HttpStatus == 0) ? "reasoner_timeout" : "reasoner_http_failed";
        continue;       // 继续下次 Attempt
    }
    
    // Reasoner 成功 → Parser
    P = AILiveParser::ParseFourChannels({R.RawContent, ActorId});
    if (P.bOk) {
        Out.Parsed = P;
        return Out;     // 早返回
    }
    Out.FailureReason = P.ErrorReason;
    Out.FailedStage   = P.FailedStage;
}

Out.bAbstain = true;   // 3 次都失败 → abstain
return Out;
```

**关键设计**：

- **每次 Attempt 重新调 Reasoner**，不是只重试 Parser。Reasoner 的随机性可能让下次输出格式更对。
- **GLM 强制 `ThinkingType = "disabled"`**——开了 thinking 模式 Reasoner 把推理写到 `reasoning_content` 而 `content` 几乎空，4 通道 tagged 文本无处栖身。
- **不写 `system.llm_inflight` 完成事件**——Parser 成功直接 return，让 GT 端的 `GatherTickAndResolveFloor` 写 4 通道事件（4 通道事件本身就含 `request_id`，配对就靠这个字段）。

### 7.3 `bAbstain=true` 的语义

3 次都失败 = abstain（弃权）。**不是"agent 死了"**——它本拍就不出声，下拍照样发车。GT 端会写 `system.parse_failed` 审计事件。

---

## 8. `TickLLMAwait`（cpp:953-982）—— 200s GT watchdog

```cpp
constexpr float kGTWatchdogSec = 200.f;

bool bAllReady = true;
for (FInflightTick& F : CurrentTickInflight) {
    if (!F.Future.IsReady()) {
        bAllReady = false;
        break;
    }
}
if (!bAllReady) {
    if (StateElapsed > kGTWatchdogSec) {
        UE_LOG(Warning, "GT watchdog 200s reached; forcing Gather (未 ready 视为 abstain)");
        GatherTickAndResolveFloor();
    }
    return;        // 等下一帧
}
GatherTickAndResolveFloor();
```

### 8.1 双重保险

- **Worker 内**：每次 Attempt 45s × 3 = 135s 上限
- **GT 这里**：200s 总超时

理论上 worker 135s 内必然返回（Reasoner timeout + Parser 内部还有 LLM 调用 + retry overhead），200s 是 **safety margin**。**超过 200s 还有 future 没 ready** ≈ ThreadPool 卡死或某个 worker 死循环——这种异常情况就 force 推进，让游戏不卡死。

### 8.2 200s 推进时未 ready 的 future 怎么办？

进 `GatherTickAndResolveFloor` 后第一个 if（cpp:1001-1016）：

```cpp
if (!F.Future.IsReady()) {
    // 写 system.parse_failed 事件，标 reason="gt_watchdog_timeout" stage="future_not_ready"
    FAILiveEvent Pf;
    Pf.PayloadJson = BuildParseFailedPayload(NPCIdx, RequestId, "", "gt_watchdog_timeout", "future_not_ready");
    Store->AppendEvent(Pf);
    continue;
}
```

**不调 `Future.Get()` 阻塞**——直接当成 abstain 写 parse_failed 跳过。worker 还会继续跑完，但它的结果**被丢弃**——不被任何代码读取。

> 这里有个隐含 bug 风险：worker 跑完后会写 parse_failed**事件**吗？不会——worker 只写到 `Out.Parsed`，事件由 GT 写。GT 这边已经写了 timeout 事件，worker 即使后来成功也没人读 future。**TFuture 对象在 vector 里析构时不阻塞**——靠 ThreadPool 的引用计数自然回收。

---

## 9. `GatherTickAndResolveFloor`（cpp:984-1205）—— 5 个 Stage

> 这是整个项目最长的函数（~220 行），但结构清晰。**Stage A → B → C → D → D.5 → E**。

### 9.1 Stage A：写 4 通道 / abstain（cpp:998-1099）

```
for each FInflightTick F:
    if !F.Future.IsReady():
        写 system.parse_failed (reason="gt_watchdog_timeout")
        continue
    
    R = F.Future.Get()
    
    if R.bAbstain:
        写 system.parse_failed (reason=R.FailureReason, stage=R.FailedStage,
                                visibility=["orchestrator", R.ActorId],
                                raw_llm_output=Reasoner 原文左 2048 字符)
        continue
    
    # 4 通道原子提交
    Group = [
        FAILiveEvent { type=SpeechScratchpad, vis=[ActorId], payload={text:R.Parsed.Scratchpad} },
        FAILiveEvent { type=SpeechIntended,   vis=[ActorId], 
                       AddressedTo=ExtractAddressedToHint(R.Parsed.IntendedJson),
                       payload=WrapPayloadEnsureText(R.Parsed.IntendedJson) },
        FAILiveEvent { type=Bid,              vis=["orchestrator"], 
                       payload=WrapPayloadEnsureText(R.Parsed.BidJson) },
        FAILiveEvent { type=SpeechNote,       vis=[ActorId], payload={text:R.Parsed.NoteText} },
    ]
    FirstSeq = Store->AppendEventsAtomically(Group)
    
    ActorToIntendedSeq[ActorId] = Group[1].Seq    # intended seq 用于 floor resolve
    BidOffsets[ActorId] = N.Config.Battle.BidOffset
```

### 9.2 关键 Stage A 不变量

**(1) 4 通道事件用 `AppendEventsAtomically`，不是 4 次 `AppendEvent`**：

要么 4 条全成功要么全回滚——避免"intended 写了 bid 没写"的悖论。4a 阶段已经看过这函数的两阶段事务设计。

**(2) 4 通道顺序固定 `[Scratchpad, Intended, Bid, Note]`**：

cpp:1094 注释：

> *"Group[1].Seq 是 intended seq（4-channel 顺序：scratchpad, intended, bid, note）"*

下面 ResolveFloor 取 winner_intended_seq 时**写死**了 `Group[1]`。**新加事件别插到这 4 个之间**，会破坏 floor resolve 的索引。

**(3) `RawLLMOutput` 只挂 Scratchpad（cpp:1052）**：

> *"仅一条挂 raw（避免 4× 重复）"*

raw response payload 几 KB，每个 NPC 1 条够审计；4 条会让 events 表膨胀 4 倍。

**(4) Bid 的 visibility = `["orchestrator"]`**（cpp:1072）：

> *"§5.2bis 核心约束"*

Bid 内含 urgency + rationale——**不能让其他 NPC 看到**，否则会被读心。orchestrator 拿来做 floor 解算，玩家 NPC 永远看不到自己的或他人的 bid。

**(5) Intended 的 `AddressedTo` 来自 Parser 输出的 `addressed_to_hint`**（cpp:1062）：

也透传到 speech.public（Stage B）—— 决策 #14 的"暗中点名"语义：私聊给 NPC07 但 visibility=public，AddressedTo=["NPC07"]，**所有人都能看到，但格式上标了对话对象**。

**(6) Bid Offset 双写后被 Roster 覆盖（cpp:1101-1109）**：

```cpp
// 阶段 A 内每个 active agent 默认 BidOffset=0
// 阶段 A 后再扫 Roster 覆盖回 N.Config.Battle.BidOffset
for (FAct02NPCRuntime& N : NPCs) {
    if (BidOffsets.Contains(NPC%02d)) {
        BidOffsets[NPC%02d] = N.Config.Battle.BidOffset;
    }
}
```

> 当前 `Battle.BidOffset` 全是默认 0（Roster 没填）；以后 agent_calibration 表上线后从那里读。

### 9.3 Stage B：ResolveFloor + ListenerFilter + speech.public（cpp:1111-1150）

```cpp
TArray<FString> Eligible;
ActorToIntendedSeq.GenerateKeyArray(Eligible);

const FAILiveTickResolution Res = Store->ResolveFloor(
    ThisTick, Eligible, BidOffsets, /*ColdThreshold=*/3.0f);
```

**Eligible = 本拍写了 intended 的 actor 集合**（abstain 的不参与）。

`ResolveFloor` 是 EventStore 的方法（4a 没读到的，签名在 EventStoreSubsystem.h）：

- 取每个 eligible 在 ThisTick 写的 Bid 事件
- 算 final_score = urgency + bid_offset + runtime_adj
- 最高分 winner；有平手按 actor 字典序破手
- 最高分 < 3.0 = 冷场（WinnerActor 为空）

**Stage B 写 speech.public 的关键不变量**（cpp:1132-1149）：

```cpp
Pub.AddressedTo   = WinnerIntended.AddressedTo;     // 透传 intended 的点名
Pub.ParentEventId = WinnerIntended.EventId;         // §5.2 衍生关系
Pub.PayloadJson   = BuildPublicPayloadFromIntended(
    FilteredText,                                    // ListenerFilter::Apply 后文本
    Res.WinnerIntendedSeq,                           // payload.derived_from_intended_seq
    /*listener_filter_score=*/0.0f,                  // 当前 stub 永远 0
    request_id);
```

**`ParentEventId = WinnerIntended.EventId`** 是 4c 阶段的 ProjectAgentViewState 里 `pending_intended` 反向连接的核心——`NOT EXISTS (... c.parent_event_id = e.event_id)` 就是用这条链。

speech.public 写失败只 log warning（cpp:1146-1148）——**不 FailAct02**。冷场（无 winner）则跳过本 stage。

### 9.4 Stage C：DeriveActionIntents（cpp:1152-1153 + 1207-1238）

```cpp
DeriveActionIntents(ThisTick);
```

这一步**对所有 agent**（不只 winner）扫 intended，提取 `intended_action.intent` 字段，写 action.intent 事件。

```cpp
for (Iv : Store->QuoteByEventTypeAndTick(SpeechIntended, ThisTick)):
    if !ExtractIntendedAction(Iv.PayloadJson, IntentName, ParamsJson): continue
    
    FAILiveEvent Ai {
        Actor         = Iv.Actor,
        EventType     = ActionIntent,
        Visibility    = [Iv.Actor],                  // 自己可见，禁 "self"
        ParentEventId = Iv.EventId,                   // 链接到 intended
        PayloadJson   = BuildActionIntentPayload(IntentName, ParamsJson, request_id, Iv.Seq),
    };
    Store->AppendEvent(Ai);
```

**为什么对所有 agent 而不只 winner？** 因为"我说出口的话"和"我打算做的事"是两件事——一个 NPC 可能这拍没抢中 floor 但已经决定下拍要"投票踢 NPC03"。`action.intent` 让这个意图持久化，下拍 PromptAssembler 装 `[YOUR RECENT INTENDED-BUT-NOT-SAID]` 段时能看到。

**Visibility = `[Iv.Actor]` 不是 `"self"`**（cpp:1228）：

> *"具体 NPCxx，禁 'self'"*

写死字符串 `"self"` 会让所有 NPC 通过 viewer 自指看到——可见性 SQL 里 `viewer = 'self'` 没意义。**always 用具体 ActorId**。

### 9.5 Stage D：tick_resolved + tick_audit（cpp:1155-1156 + 1240-1280）

**两条事件分开写**——这是个有意的设计选择。

```cpp
TR (tick_resolved):
    Actor      = "orchestrator"
    Visibility = ["public"]                   ← 所有 NPC 看到
    PayloadJson = { tick_no, winner_actor, winner_intended_seq, derived_public_seq, text }
    // 不含 all_bids — 防止 NPC 看到他人 bid

TA (tick_audit):
    Actor         = "orchestrator"  
    Visibility    = ["orchestrator"]          ← 只 orchestrator 自己看
    ParentEventId = TR.EventId                ← 链回 tick_resolved
    PayloadJson   = {
        tick_no,
        all_bids: [{actor, urgency, bid_offset, runtime_adj, final_score}, ...],
        filter_decision: {score, rewrote, rationale},
        parent_tick_resolved_seq,
    }
```

**为什么不合一条？** 因为可见性不同：

- `tick_resolved` 公开，让 NPC 知道 "本拍 NPC03 抢中 floor"——这是博弈所需信息
- `tick_audit` 含 all_bids（其他 NPC 的 urgency/score）——**绝对不能给 NPC 看**

如果合一条事件，要么把 all_bids 进 visibility=public 的事件 payload（违反 §5.2bis），要么把整条改 visibility=orchestrator（NPC 看不到 winner）。**拆成两条 + parent_event_id 关联是最干净的解法**。

`WriteTickResolvedAndAudit` 返回 `TRSeq` 给上层但实际**没人用这返回值**——是预留接口。

### 9.6 Stage D.5：异步 RebuildProjections（cpp:1158-1170）

```cpp
TWeakObjectPtr<UAILiveEventStoreSubsystem> WeakStore(Store);
AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, [WeakStore]() {
    if (UAILiveEventStoreSubsystem* S = WeakStore.Get()) {
        S->RebuildProjections();
    }
});
```

**3 个细节**：

**(1) `TWeakObjectPtr` 守 EventStore**——PIE 关闭后 lambda 进入时 Subsystem 已被销毁；`WeakStore.Get() == nullptr` 时直接退出。

**(2) `AsyncTask + AnyBackgroundThreadNormalTask` 不阻塞 GT**——RebuildProjections 4 reducer 可能 100+ms（4c 阶段算过），跑后台。

**(3) 失败只 Verbose 日志，不阻塞主循环**——投影脏一拍不影响游戏推进，下次 Rebuild 自动修。

> **隐含的并发陷阱**：如果 Stage E 之后立刻进下一拍（理论上不会，但 watchdog 强推时可能），下一拍的 RunTick 会再调 BeginTick + AppendEvent，这些都拿 WriteMutex；后台 Rebuild 也拿 WriteMutex。**SQLite 单写者模型 + WriteMutex 保证不会真冲突**——只是后台 Rebuild 可能阻塞下拍写入几十毫秒。当前可接受。

### 9.7 Stage E：进 SeedSpeak / ReactionSpeak / 推进（cpp:1172-1204）

```cpp
if !Res.WinnerActor.IsEmpty():
    WinnerIdx = NPCs.IndexOfByPredicate(...)         # 通过 ActorId 反查 NPCs[] 索引
    LastSpeakerIndex = WinnerIdx
    LastSentence = FilteredText
    StartSpeak(WinnerIdx, FilteredText)              # 进 TTS
    state = (CurrentRound == 0) ? SeedSpeak : ReactionSpeak
else:
    # 冷场（无 winner）
    if (CurrentRound == 0):
        # seed 冷场也自动推到反应阶段——不卡 GatedAwaitNext
        state = GatedAwaitNext
        StartReactionPhase()
    else:
        AdvanceReactionRound()
```

**冷场的两条路径**：

- **seed 冷场**：直接 `state = GatedAwaitNext` 然后立刻 `StartReactionPhase()`。注释（cpp:1195-1196）：

  > *"seed 冷场也自动推进到反应阶段，避免卡在 GatedAwaitNext 等手动按 3"*

- **reaction 冷场**：直接 `AdvanceReactionRound()`——可能进下一轮或 CompleteAct02。

**自检题 #5 答案**：seed 冷场不会卡 GatedAwaitNext——会自动跳到反应轮。

---

## 10. `StartSpeak` / `TickSpeakWatchdog` / `HandleSpeechFinished`（cpp:1282-1371）

### 10.1 `StartSpeak`（cpp:1282-1330）

```cpp
StartSpeak(NPCIndex, Text):
    CurrentSpeaker = INDEX_NONE; SpeakElapsed = 0; bSpeechFinished = false
    
    if NPCs[NPCIndex] 越界:
        bSpeechFinished = true; bSpeechFailed = true; return
    
    Pawn       = NPCs[NPCIndex].Pawn.Get()
    MinimaxKey = ProjectEnvLoader::Get("minimax")
    
    if !Pawn || MinimaxKey.IsEmpty() || Text.IsEmpty():
        # 任何前提缺失都 auto-finish（不报错，让游戏继续）
        bSpeechFinished = true
        bSpeechFailed   = MinimaxKey.IsEmpty()      # 没 key 算失败但仍推进
        return
    
    # WeakObjectPtr 守 self → 跨线程回调 GT 时安全
    TWeakObjectPtr<AAct02RuleReceiveDirector> WeakSelf(this);
    Cb = FOnMinimaxSpeechFinishedNative::CreateLambda([WeakSelf](bool bOk) {
        if (auto* Self = WeakSelf.Get()) Self->HandleSpeechFinished(bOk);
    });
    
    bDispatched = UMinimaxACELibrary::TriggerMinimaxSpeechFromPawnNative(
        this, Pawn, Text, MinimaxKey, N.Config.Identity.Voice,
        "https://api.minimaxi.com/v1/t2a_v2", A2FProviderName, Cb);
    
    if !bDispatched:
        bSpeechFinished = true; bSpeechFailed = true   # TTS 派发失败 = 立即 auto-finish
```

**两层保险**：

1. **前置缺失** → auto-finish 不调 TTS
2. **TTS 派发返回 false** → 也 auto-finish

无论哪个都不 FailAct02——**TTS 失败不阻塞游戏**。NPC 嘴不动但下拍照样跑。

### 10.2 `TickSpeakWatchdog`（cpp:1332-1365）

```cpp
SpeakElapsed += Dt
if bSpeechFinished:
    bWasSeed = (state == SeedSpeak)
    if bWasSeed:
        state = GatedAwaitNext     # StartReactionPhase 的 guard 要求
        StartReactionPhase()        # 自动进反应轮（决策：seed 不需手按 3）
    else:
        AdvanceReactionRound()
    return

if SpeakElapsed > SpeechWatchdogSeconds (30s):
    bSpeechFinished = true; bSpeechFailed = true   # 强制 finish
```

**`bSpeechFinished` 由 TTS 回调或 watchdog 触发**——两者都会让下一帧的 Tick 进入 advance 分支。

**为什么 `state = GatedAwaitNext` 然后立刻 `StartReactionPhase`？** 因为 `StartReactionPhase` 内部有 guard：

```cpp
if (SceneState != EAct02State::GatedAwaitNext) {
    UE_LOG(Warning, "ignored, state=...");
    return;
}
```

直接从 SeedSpeak 进 ReactionDispatch 会被 guard 拒。**先满足前置再调**。

### 10.3 `HandleSpeechFinished`（cpp:1367-1371）—— TTS 跨线程回调

```cpp
void AAct02RuleReceiveDirector::HandleSpeechFinished(bool bSuccess) {
    bSpeechFinished = true;
    bSpeechFailed   = !bSuccess;
}
```

**3 行函数但很关键**：

- 它会在 **MiniMax/A2F 内部线程** 上被调用（lambda 已经过 `TWeakObjectPtr.Get()` 守 self）
- **没加任何同步原语**——`bSpeechFinished` / `bSpeechFailed` 是 `bool` 成员变量，靠"GT 只读 + 回调线程只写"的简单约定保证可见性
- TickSpeakWatchdog 每帧读 `bSpeechFinished`——**理论上**有 race（写到 false→true 的过程中读到 false 后下一帧再读到 true，不影响正确性，只是延一帧）
- **不需要 `std::atomic`**——bool 写入是原子操作（Win64 编译器保证），最坏延一帧

> 严格说应该用 `std::atomic<bool>` 或 `FThreadSafeBool`，但当前 PIE 没观察到任何 race 症状，留作未来优化。

---

## 11. `AdvanceReactionRound`（cpp:1373-1387）—— 循环出口

```cpp
if (CurrentRound >= ReactionRoundCount):
    CompleteAct02();        # 所有反应轮跑完 → 完结
    return;

CurrentRound += 1;
SceneState = ReactionDispatch;
RunTick();                  # 下一拍发车
SceneState = ReactionAwait;
StateElapsed = 0.f;
```

**`ReactionRoundCount` 默认 3**（h:109）——seed 1 拍 + reaction 3 拍 = 4 拍后 CompleteAct02。

**实际跑完一局 ≈ 4 × (135s LLM + 30s TTS) ≈ 11 分钟**——开发期 PIE 跑完一次要这么久，所以 `act02.cancel` console 命令是必备调试工具。

---

## 12. 自检题答案

### 12.1 自检题 #1

> 按 [2] → NPC1 嘴动起来，链路上至少 8 个函数 + 归属类 + worker 标记。

```
1.  AAct02RuleReceiveDirector::Tick                     [GT]  检测 [2] just pressed
2.  AAct02RuleReceiveDirector::BeginAct02                [GT]  校验 + EventStore.BeginGame
3.  AAct02RuleReceiveDirector::StartPrescatter           [GT]  发车 EQS scatter
4.  AAct02RuleReceiveDirector::TickPrescatter            [GT]  poll 直到 NPC 都 idle
5.  AAct02RuleReceiveDirector::StartSeedPhase            [GT]  state=SeedDispatch → RunTick
6.  AAct02RuleReceiveDirector::RunTick                   [GT]  EventStore.BeginTick + 10×Async dispatch
7.  AAct02RuleReceiveDirector::RunAgentTickInWorker      [worker]  Reasoner→Parser × 3 retry
8.    OpenAIChat::RequestBlocking                        [worker]  HTTP 同步阻塞
9.    AILiveParser::ParseFourChannels                    [worker]  Stage1+2+3 校验/调用 Parser LLM
10. AAct02RuleReceiveDirector::TickLLMAwait              [GT]  poll Future + 200s watchdog
11. AAct02RuleReceiveDirector::GatherTickAndResolveFloor [GT]  Stage A-E
12.   UAILiveEventStoreSubsystem::AppendEventsAtomically [GT]  写 4 通道
13.   UAILiveEventStoreSubsystem::ResolveFloor           [GT]  bid 解算
14.   AAct02RuleReceiveDirector::WriteTickResolvedAndAudit [GT]
15. AAct02RuleReceiveDirector::StartSpeak                [GT]  → MinimaxACELibrary
16.   UMinimaxACELibrary::TriggerMinimaxSpeechFromPawnNative [GT 派发 → ThreadPool 跑]
17.     MinimaxSpeech::RequestBlocking                   [worker]  T2A v2 HTTP
18.     FACERuntimeModule::AnimateFromAudioSamples       [worker→GT]  喂 PCM
19. AAct02RuleReceiveDirector::HandleSpeechFinished      [TTS 内部线程]  set bSpeechFinished
20. AAct02RuleReceiveDirector::TickSpeakWatchdog         [GT]  下一帧检测 bSpeechFinished → advance
```

**worker 线程的步骤**：7, 8, 9, 17, 18 的部分。其余全 GT。

### 12.2 自检题 #2

> 4 个函数各跑哪线程？UObject 在 worker 能 touch 吗？

```
RunTick                     ─ GT
RunAgentTickInWorker        ─ worker (ThreadPool)
TickLLMAwait                ─ GT (每帧 Tick 调用)
GatherTickAndResolveFloor   ─ GT
```

**worker 线程绝不能 touch UObject**——`RunAgentTickInWorker` 是 `static` + 参数全 by value（`FNPCAgentConfig` / `FString`）就是为了这个不变量。一旦 worker 内 `this->Foo` 或 `Pawn->GetTransform()`，PIE 关闭瞬间就会崩。

`Stage D.5` 的 RebuildProjections 也跑后台线程，但用 `TWeakObjectPtr` 守 + `Get()` 检查——这是后台访问 UObject 的合法模式。

### 12.3 自检题 #3

> 5 个 Stage 各做什么？哪步 FailAct02，哪步只 warning？

| Stage | 做什么 | 失败行为 |
|---|---|---|
| A | 写 4 通道 / parse_failed | `AppendEventsAtomically` 失败 → **FailAct02** |
| B | ResolveFloor + 写 speech.public | Quote 失败 → **FailAct02**；speech.public 写失败 → warning |
| C | DeriveActionIntents 写 action.intent | warning（每个 actor 失败独立） |
| D | tick_resolved + tick_audit | warning（不阻塞游戏） |
| D.5 | 异步 RebuildProjections | Verbose log（连 warning 都不到） |
| E | StartSpeak / 推进 | 不会失败——TTS 派发失败 = auto-finish |

**只有 Stage A 和 Stage B 的 Quote 会 FailAct02**——这两个一旦失败就破坏事件溯源完整性，必须停掉游戏让人查。

### 12.4 自检题 #4

> tick_resolved 和 tick_audit 为什么拆？

可见性不同：

- `tick_resolved` visibility=["public"]——所有 NPC 看到 winner
- `tick_audit` visibility=["orchestrator"]——含 all_bids，绝对不能让 NPC 看

合一条事件没法既公开 winner 又隐藏 bid 细节。**parent_event_id 把两条链起来**做审计追溯。

### 12.5 自检题 #5

> seed 冷场会卡 GatedAwaitNext 等手按 [3] 吗？

**不会**。Stage E 的 cold tick 分支：

```cpp
if (CurrentRound == 0) {
    state = GatedAwaitNext;
    StartReactionPhase();   // 立刻调
}
```

**`SeedSpeak` 结束的 watchdog 也是同样模式**——seed 永远自动推进。手按 [3] 是给 PIE 调试用的"暂停一下让我看 prompt 内容"机制，正常游戏流程不需要。

---

## 13. 关键不变量速查表

| # | 不变量 / 约束 | 在哪强制 |
|---|---|---|
| 1 | Director 不维护 tick_no 副本，权威在 EventStore | RunTick cpp:806-808 |
| 2 | inflight 配对协议：先写 system.llm_inflight 再发 LLM | RunTick cpp:849-870 |
| 3 | inflight 写失败 = 整拍 FailAct02（不可降级） | RunTick cpp:862-870 |
| 4 | RunAgentTickInWorker 必须 static + 参数全 by value，不 touch UObject | h:189-194 + cpp:890 |
| 5 | worker 内 lambda 捕获用 `CfgCopy` 不用 by-ref `N` | RunTick cpp:873-880 |
| 6 | LLM 重试在 worker 内 3×；GT 端不再用 LLMTimeoutSeconds 整体 fail | RunAgentTickInWorker cpp:907 + cpp:818-820 |
| 7 | 4 通道事件顺序 `[Scratchpad, Intended, Bid, Note]` 写死，floor 解算靠 Group[1] | GatherTickAndResolveFloor cpp:1043-1095 |
| 8 | 4 通道用 AppendEventsAtomically 全成功或全回滚 | cpp:1087-1093 |
| 9 | RawLLMOutput 只挂 Scratchpad（避免 4× 重复） | cpp:1052 |
| 10 | Bid visibility=["orchestrator"]——禁让 NPC 看见他人 bid | cpp:1072 + §5.2bis |
| 11 | speech.public.ParentEventId = WinnerIntended.EventId（pending_intended 反向连接靠这条） | cpp:1139 + 4c 第 6.5 节 |
| 12 | tick_resolved 和 tick_audit 拆开（可见性不同）+ parent_event_id 链起 | WriteTickResolvedAndAudit cpp:1240-1280 |
| 13 | RebuildProjections 用 TWeakObjectPtr 守 EventStore + AsyncTask 后台跑 | cpp:1158-1170 |
| 14 | seed 冷场和 SeedSpeak 完成都自动推进到反应轮，不等手按 [3] | cpp:1193-1199 + cpp:1342-1350 |
| 15 | TTS 派发失败 / key 缺失 → auto-finish，不 FailAct02 | StartSpeak cpp:1299-1306 |
| 16 | Speech 30s watchdog 强制 finished | TickSpeakWatchdog cpp:1357-1364 |
| 17 | 所有跨线程回调用 TWeakObjectPtr 守 self | StartSpeak cpp:1309-1317 |
| 18 | bSpeechFinished 是裸 bool，无锁，靠"GT 读 + 回调线程写"约定 | HandleSpeechFinished cpp:1367-1371 |

---

## 14. 扩展任务推演

| 改动 | 至少要动 |
|---|---|
| 新增"晚到的 Future 也写入"（200s GT watchdog 后 worker 才完成的结果不丢弃） | (1) 状态机加新 state `LateAwait`；(2) GatherTickAndResolveFloor 把超时 future 不立刻 abstain，而是迁到 `CurrentTickInflight_Late` 数组；(3) 加 `TickLateAwait` 在 `LateAwait` state 下 poll late futures，到了写迟到 audit 事件。**caveat**: events 表的 seq 是按 AppendEvent 时间排序的，迟到事件会 seq 远大于本拍其他事件——下游 ProjectAgentViewState 的 PendingTickFloor 算法可能要调整 |
| Director 端做 LLM 调用日志（不只 RawLLMOutput） | (1) RunAgentTickInWorker 末尾把 (Attempt, Provider, Model, Latency, FinishReason) 收到 Out 里；(2) 改 GatherTickAndResolveFloor 写 4 通道时，在 Scratchpad 的 RawLLMOutput 字段拼接全部 Attempt 信息；(3) 不是新增 events 行——多挂 metadata 而已，避免 events 表膨胀 |
| 把 reaction 拆成多个并发对话（NPC07 ↔ NPC03 私聊不影响 NPC02 主线） | (1) 新增 `MultiThread` state 在 GatedAwaitNext 之后；(2) RunTick 不再 BeginTick 一次后所有 NPC 一起发车——按 `addressed_to_hint` 分组，每组独立 BeginTick；(3) **重大改动**：4a 阶段说过哈希链是单链，多 tick 并发会破链。**这个改动需要先重设计 hash chain（改成 per-conversation 或保留单链但允许 tick_no 乱序）**——MVP 不接 |
| 把 LLM 失败时的 fallback agent（备 deepseek 顶上） | (1) RunAgentTickInWorker 的 `for (Attempt 1..3)` 内首两次用 N.Config.Provider，第 3 次切 DeepSeek；(2) FAgentTickResult 加字段 `bUsedFallbackProvider`；(3) Stage A 写 4 通道时把 fallback 标记进 Scratchpad 的 RawLLMOutput；(4) **caveat**: parser_version 不变，但 schema_meta 表的 reasoner_provider 列要支持 "primary:fallback" 表达 |

---

## 15. 阅读追溯：把整个项目串起来

读到这里，回头看 `Learn/00-overview.md` 里画的总图。能否凭记忆重画？应当能口述：

```
events 表 ← (4a) AppendEvent 三表写 + 哈希链 + 不可变性触发器
            ↑
4 reducer ← (4c) 全量重建 + 单事务 + 顺序固定
            ↑
PromptAssembler 7 段 ← (4b) must-keep + 降级
            ↑
Reasoner LLM (per-NPC provider) → Parser LLM (DeepSeek 写死) → 4 通道 ← (4b)
            ↑
GatherTickAndResolveFloor ← (5) Stage A→E
            ↑
RunTick → RunAgentTickInWorker 10× → TickLLMAwait ← (5)
            ↑
StartPrescatter → StartSeedPhase ← (5)
            ↑
[2] 键 → BeginAct02 ← (5) BeginPlay 期间已 bind Roster + 自动监听 Act01
```

任何一段画不出，对着这阶段的目录回去复习。

---

## 下一步

进 **阶段 6 — Acts 对照与周边**：Act01 线性版（理解共同 idiom）+ StoryScenarioDirector 独立工具 + MinimaxACELibrary TTS+A2F 玻璃层 + ScatterMover EQS 散开 + 感知组件辅助。这是**最后一个阶段**——读完整个项目就建立完整心智模型了。告诉我可以开始，我把它写到 `Learn/06-acts-comparison-peripherals.md`。
