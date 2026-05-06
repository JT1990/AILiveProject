# 阶段 3 — 主导演 API：Act02 + Act01 对照

只读两个 header：

```
Public/Acts/Act02RuleReceiveDirector.h    ★ 核心，9 态 + 双 LLM
Public/Acts/Act01RuleIntroDirector.h      ← 4 态线性，做对照
```

读完这一阶段你应当能回答：

1. `EAct02State::GatedAwaitNext` 在做什么？为什么把"种子轮和反应轮之间"做成显式状态？
2. `OnAct02Completed` / `OnAct01Completed` 怎么连起来？为什么 Act02 里有个 `HandleAct01Completed`？
3. Act01 和 Act02 都有 `CacheInitialNPCTransforms` / `ResetToInitialPositions`——为什么不抽到共同基类？

---

## 1. 它们是什么

```cpp
UCLASS(BlueprintType, Blueprintable)
class AILIVEPROJECT_API AAct02RuleReceiveDirector : public AActor

UCLASS(BlueprintType, Blueprintable)
class AILIVEPROJECT_API AAct01RuleIntroDirector : public AActor
```

两者都是**有状态的 AActor**——你把它**放进关卡**，给 UPROPERTY 配好引用，按 `StartKey` 启动。

为什么是 AActor 不是 Subsystem？

- 需要 `Tick`（驱动状态机推进、看门狗、按键检测）
- 需要 UPROPERTY 编辑器内可视化绑定（`TSoftObjectPtr<AActor>` 拖关卡里的目标点）
- 需要参与 Begin/EndPlay 生命周期
- 跨关卡的"状态"已经由 `UAILiveEventStoreSubsystem` 管，导演自己只管单关卡演出

每个导演**自己注册控制台命令** `act01.start/cancel` / `act02.start/next/cancel`——所以你在 PIE 里既能按键也能用控制台。

---

## 2. Act02 — 核心导演

### 2.1 状态机 9 态

```cpp
enum class EAct02State : uint8
{
    Idle,
    PrescatterToTV,    // 走到 TV 前散开
    SeedDispatch,      // 派 LLM 任务（种子轮）
    SeedAwait,         // 等所有 worker future 就绪
    SeedSpeak,         // 赢家播音 + A2F
    GatedAwaitNext,    // ⭐ 暂停点
    ReactionDispatch,  // 派 LLM 任务（反应轮）
    ReactionAwait,
    ReactionSpeak,     // 循环 ReactionRoundCount 次
};
```

这是个**非闭合**的状态机——`ReactionSpeak` 完后要么回 `ReactionDispatch`（还有轮次），要么 `Idle`（结束）。

#### 自检题 #1：`GatedAwaitNext` 是什么

注释写明 `NextPhaseKey`（默认按 [3]）会把它推进到 `ReactionDispatch`。它是**故意做出来的可观察暂停**：

| 时机 | 用途 |
|---|---|
| **Seed 轮发完音后** | 给观察者（你/调试者）一个看清楚 seed 输出的机会 |
| **再按 [3] 或自动推进** | 进入 reaction 轮 |

为什么不直接 `SeedSpeak → ReactionDispatch`？因为 seed 轮本质上是**一次性的开场白**（10 个 NPC 听到游戏规则后的初始反应），它和后续的 reaction 轮属性不同——隔一个状态便于：

1. 调试时挂断点观察 seed 数据
2. 未来加"用户介入步骤"（比如玩家在这里输入额外提示）
3. Replay / 录像系统在这里打 chapter 标记

这是**为可观察性付出的状态机税**。值。

### 2.2 18 个 UPROPERTY 配置（按桶分）

```cpp
//─── Input ────────────────────────────────
bool bEnableKeyTrigger;
FKey StartKey;        // 默认 [2]
FKey NextPhaseKey;    // 默认 [3]

//─── Actors ───────────────────────────────
TSubclassOf<AActor>     NPCMoverClass;
TSoftObjectPtr<AActor>  NavTargetActor;        NavLookTargetActor;
TSubclassOf<AActor>     NavTargetClass;        NavLookTargetClass;
FName                   NavTargetActorLabel;   NavLookTargetActorLabel;

//─── Movement ─────────────────────────────
TObjectPtr<UEnvQuery>   ScatterQueryAsset;
float MovementTimeoutSeconds       = 30.f;
float ScatterDispatchDelaySeconds  = 2.f;
float ArrivalSettleSeconds         = 0.5f;

//─── Roster ───────────────────────────────
TArray<FNPCAgentConfig> Roster;

//─── Prompt ───────────────────────────────
FString GameRuleRelativePath;        // "Docs/playscript/zombie-game-rule.md"
FString GameRuleFallbackSummary;     // 文件读失败时用

//─── LLM ──────────────────────────────────
float LLMTimeoutSeconds = 65.f;       // ⚠ 见下文

//─── TTS ──────────────────────────────────
FName A2FProviderName;                // "LocalA2F-James"
float SpeechWatchdogSeconds = 30.f;

//─── Reaction ─────────────────────────────
int32 ReactionRoundCount = 3;

//─── Debug ────────────────────────────────
bool bDebugPrintScreen = true;
```

**记住几件事**：

1. **目标点有 3 种解析方式**：直接拖 `TSoftObjectPtr` / 用 `Class+Label` 模糊匹配 / 留空走默认。代码会按"显式拖入 → Class+Label 解析 → 默认 fallback"顺序试。这是**编辑器友好性**写法。
2. **`Roster` 留空时**：`BindRoster()` 会 fallback 到 `AILiveAgentRoster::GetDefaultRoster()`（10 人花名册）。
3. ⚠️ **`LLMTimeoutSeconds = 65.f` 与阶段 5 的 worker 实际行为有错位**——worker 内部硬编码 3 retry × 45s 的攻击模式，外层 65s 看起来更像"GT 主线程的安全带"而不是真正的 per-attempt 超时。读 .cpp 时记着核对。

### 2.3 两个 Delegate

```cpp
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FAct02CompletedDelegate);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FAct02FailedDelegate, FString, Reason);

UPROPERTY(BlueprintAssignable, ...)
FAct02CompletedDelegate OnAct02Completed;
UPROPERTY(BlueprintAssignable, ...)
FAct02FailedDelegate    OnAct02Failed;
```

- **DYNAMIC_MULTICAST**：可以在蓝图里 Bind / Unbind，可以多个监听者
- 完成事件**无参数**——你想知道结果就读 `GetSceneState()`
- 失败事件带 `Reason` 字符串

### 2.4 BP 可调的 4 个入口 + 1 个监听器

```cpp
UFUNCTION(BlueprintCallable) bool        BeginAct02();
UFUNCTION(BlueprintCallable) void        StartReactionPhase();
UFUNCTION(BlueprintCallable) void        CancelAct02();
UFUNCTION(BlueprintPure)     EAct02State GetSceneState() const;

UFUNCTION() void HandleAct01Completed();   // 自动绑 Act01 完成事件
```

#### 自检题 #2：Act01 → Act02 的自动衔接

`HandleAct01Completed` 注释写明：

> *"监听 ACT01 完成事件，自动触发 BeginAct02（开发调试：按 1 即可一路跑完 act01 → act02 → 反应轮）"*

机制：

```
Act02::BeginPlay()
   └─► 找到关卡里所有 AAct01RuleIntroDirector
   └─► 给它们的 OnAct01Completed 绑 HandleAct01Completed
                                            │
按 [1] 启动 Act01 ─► Act01 完成 ─► OnAct01Completed.Broadcast()
                                            │
                                            ▼
                        HandleAct01Completed() 触发 BeginAct02()
```

这就是为什么按 [1] 能"一键跑完整套"。**它不是硬编码的**——你可以把 Act02 actor 删掉，Act01 仍然独立工作。

⚠️ **隐含约束**：场景里**最多放一个** Act02 instance。若放多个，它们都会绑同一个 Act01 完成事件——会同时启动，状态会乱。

### 2.5 内部数据结构（窥探阶段 5）

这两个嵌套 struct 是阶段 5 的"线程边界对象"，先认识：

```cpp
struct FAgentTickResult
{
    int32        NPCIndex;
    FString      ActorId;        // "NPC%02d"
    FString      RequestId;
    ELLMProvider Provider;
    FString      Model;
    bool         bAbstain;       // 3 次重试都失败 → true
    FString      FailureReason;  // 错误归因
    FString      FailedStage;    // 失败阶段（"PrevalidateRawTaggedSections" 等）
    FString      ReasonerRawText;
    AILiveParser::FParseResult Parsed;
    OpenAIChat::FResult        ReasonerResult;
};

struct FInflightTick
{
    int32   NPCIndex;
    FString ActorId;
    FString RequestId;
    ELLMProvider Provider;
    FString Model;
    TFuture<FAgentTickResult> Future;   // worker 的 promise
};
```

`FAgentTickResult` 是**线程边界**——worker `SetValue` 一次，GT 只 `Get` 一次。**不要在 worker 里 touch UObject**（注释强调）。

### 2.6 内部 helper 函数（阶段 5 会逐个看）

按调用顺序列出：

```cpp
// 解析 / 缓存（BeginPlay 用）
CacheInitialNPCTransforms / ResolveActorByClassAndLabel / ResolveTargetActors / BindRoster
LoadGameRule

// 控制台命令注册
RegisterDebugConsoleCommands / UnregisterDebugConsoleCommands / BeginAct02Console

// PrescatterToTV 阶段
StartPrescatter / TickPrescatter / AreAllNPCsIdle / ResetToInitialPositions

// SeedDispatch / SeedAwait / ReactionDispatch / ReactionAwait
StartSeedPhase / RunTick / RunAgentTickInWorker [static] / GatherTickAndResolveFloor
TickLLMAwait / DeriveActionIntents / WriteTickResolvedAndAudit
BuildReasonerSystemPrompt / BuildLLMInflightPayloadJson [static]

// SeedSpeak / ReactionSpeak
StartSpeak / TickSpeakWatchdog / HandleSpeechFinished

// 收尾
AdvanceReactionRound / CompleteAct02 / FailAct02

// 调试
DebugMessage
```

### 2.7 私有运行时状态

```cpp
TObjectPtr<AActor> NavTargetCached;       // 解析后的 nav 目标
TObjectPtr<AActor> NavLookTargetCached;
TMap<TWeakObjectPtr<AActor>, FTransform> InitialNPCTransforms;  // ResetToInitialPositions 用

TArray<FAct02NPCRuntime>  NPCs;                 // FNPCAgentConfig + Pawn 弱指针
TArray<FInflightTick>     CurrentTickInflight;  // 当拍 worker 的 future 集合
TMap<FString, int64>      ActorToIntendedSeq;   // 阶段 A 填，B/C 用
TMap<FString, FString>    ActorToRequestId;

int32   LastSpeakerIndex   = INDEX_NONE;
FString LastSentence;
int32   CurrentRound       = 0;          // 0 = seed；1..N = reaction
int32   CurrentSpeaker     = INDEX_NONE;

float StateElapsed = 0.f;                // 通用 phase timer
float MovementSettleElapsed = 0.f;
bool  bArrivalSettling = false;
float SpeakElapsed = 0.f;

bool bSpeechFinished = false;            // 由 HandleSpeechFinished 翻起
bool bSpeechFailed   = false;

EAct02State SceneState = EAct02State::Idle;
```

**这 3 个 map 是阶段 5 的"接力棒"**：

```
GatherTickAndResolveFloor:
   Stage A → 写 speech.intended，把 actor → seq 填进 ActorToIntendedSeq
           → 写 system.llm_inflight 时把 actor → request_id 填进 ActorToRequestId
   Stage B → ResolveFloor 用 ActorToIntendedSeq 反查 winner 的 IntendedSeq
   Stage C → 写 speech.public 时引用 winner 的 IntendedSeq
```

### 2.8 `CurrentRound = 0` 是 seed，1..N 是 reaction

这是个**语义约定**，但注释里没标得很显眼。值得记住——阶段 5 的 `AdvanceReactionRound` 会读这个。

---

## 3. Act01 — 线性导演（对照）

### 3.1 状态机 4 态

```cpp
enum class EAct01State : uint8
{
    Idle,
    OpeningDoors,
    NPCsMovingToTV,
    PlayingVideo,
};
```

完全线性、不循环、无 LLM、无 TTS。本质就是个**编排好的开场动画**。

### 3.2 配置桶

```cpp
//─── Input ───── 同 Act02：bEnableKeyTrigger / StartKey（默认 [1]）

//─── Actors ──── 多了 MediaPlateActor（TV 视频）

//─── Doors ──── Act01 独有
TArray<FName>  CellDoorActorNames;
FName          CellDoorMeshComponentName;
FRotator       DoorOpenRelativeRotation;
float          DoorAnimationSeconds = 1.0f;

//─── Movement ─ 比 Act02 多一项
float ArrivalAcceptanceRadius = 250.0f;
//   其余同 Act02：MovementTimeoutSeconds / ScatterDispatchDelaySeconds /
//                  ArrivalSettleSeconds / ScatterQueryAsset

//─── Video ──── Act01 独有
float VideoFallbackTimeoutSeconds = 600.0f;  // 视频结束委托没触发时的兜底

//─── Debug ──── bDebugPrintScreen
```

**Act01 没有**：Roster / Prompt / LLM / TTS / Reaction 配置——它根本不和 LLM 系统打交道。

### 3.3 BP 入口

```cpp
UFUNCTION(BlueprintCallable) bool BeginAct01();
UFUNCTION(BlueprintCallable) void CancelAct01();
UFUNCTION(BlueprintPure)     bool IsAct01Running() const;
UFUNCTION(BlueprintPure)     EAct01State GetSceneState() const;
```

⚠️ **注意 Act01 有 `IsAct01Running()` 但 Act02 没有 `IsAct02Running()`**——这是个 API 不一致点。Act02 只暴露 `GetSceneState()`，调用方得自己判 `state != Idle`。改 API 时建议**给 Act02 也加一个**，对齐两个导演的接口。

### 3.4 写一条事件到 EventStore

```cpp
// Act01.h:158-160
bool AppendOrchestratorRoundResolved(const FString& Text);
```

注释：

> *"写一条 orchestrator.round_resolved 事件（phase=setup, visibility=["public"]）。返回 false 表示 EventStore 不可用或 AppendEvent 失败，由调用方决定是否升级为 FailAct01。"*

也就是说 **Act01 也写事件**——往 events 表写一条 `phase=setup` 的开场记录。但 Act01 不调 LLM，所以它的事件流极简，只为下游"知道开局发生过"打个 marker。

---

## 4. 两个导演的"共同 idiom"

把两个 header 并排看，能看到一组**完全同名同型**的成员：

| Helper | Act01 | Act02 |
|---|---|---|
| `CacheInitialNPCTransforms` | ✓ | ✓ |
| `ResolveActorByClassAndLabel` | ✓ | ✓ |
| `ResolveTargetActors(bLog)` | ✓ | ✓ |
| `ResetToInitialPositions` | ✓ | ✓ |
| `RegisterDebugConsoleCommands` | ✓ | ✓ |
| `UnregisterDebugConsoleCommands` | ✓ | ✓ |
| `AreAllNPCsIdle` | ✓ | ✓ |
| `DebugMessage` | ✓ | ✓ |
| `OnXxxCompleted / OnXxxFailed` | ✓ | ✓ |
| `bEnableKeyTrigger / StartKey` | ✓ | ✓ |
| `NPCMoverClass / NavTargetActor / NavLookTargetActor / NavTargetClass / NavLookTargetClass / NavTargetActorLabel / NavLookTargetActorLabel` | ✓ | ✓ |
| `ScatterQueryAsset / MovementTimeoutSeconds / ScatterDispatchDelaySeconds / ArrivalSettleSeconds` | ✓ | ✓ |
| `bDebugPrintScreen` | ✓ | ✓ |
| `IConsoleCommand* StartCommand / CancelCommand` | ✓ | ✓ |
| `EAxxState SceneState = Idle` | ✓ | ✓ |

#### 自检题 #3：为什么不抽到共同基类？

**没在代码里找到答案，但有几个合理的解释**（值得跟我讨论）：

1. **状态机太不同**——Act01 4 态线性 vs Act02 9 态带循环。共同基类只能抽出"BeginPlay 缓存 + Tick 派发"骨架，省的代码量不大
2. **配置桶差异**——Act01 没有 Roster/Prompt/LLM/TTS，Act02 没有 Door/Video。如果做基类，要么基类只放公共配置（仍然得各加各的），要么基类用 OptionalProperty（更难用）
3. **演出节奏不同**——Act01 完全自动，Act02 有 GatedAwaitNext 显式暂停。BeginXxx/Tick 的节奏不一样
4. **早期开发偷懒**——更可能的真实原因。两个 director 不是同时写的（Act02 是后来加的）

**我的建议**：现状可接受。如果以后要加 Act03（比如投票轮），先把它**也按这个 idiom 写**一遍，等到 3 个并存时再抽 `AAILiveActDirectorBase` 也不迟。**3 个相似实例**才是抽象的合适触发点（"rule of three"）。

---

## 5. 心智模型

```
玩家按 [1]              玩家按 [2]              玩家按 [3]
     │                     │                       │
     ▼                     ▼                       ▼
  ┌──────┐              ┌──────────────────────────────────┐
  │Act01 │              │Act02RuleReceiveDirector          │
  │      │              │                                  │
  │ 4 态 │ Completed    │  Idle                            │
  │      │ ──────────►  │   └─PrescatterToTV               │
  │      │              │      └─SeedDispatch              │
  │      │              │         └─SeedAwait              │
  └──────┘              │            └─SeedSpeak           │
     │                  │               └─GatedAwaitNext ◄─┘ ← 玩家按 [3]
     │                  │                  └─ReactionDispatch
     ▼                  │                     └─ReactionAwait
  写 1 条               │                        └─ReactionSpeak ─┐
  orchestrator.         │                                          │
  round_resolved        │   循环 ReactionRoundCount 次 ◄───────────┘
                        │   ↓
                        │   CompleteAct02 → OnAct02Completed
                        └──────────────────────────────────┘
                              │
                              ▼
                        每一拍跑：BeginTick → 派 worker → 收菜
                                   → Stage A-E
                                   → speech.public + tick_resolved
                                   → MinimaxACELibrary 播音 + A2F
```

---

## 6. 自检答案

1. **`GatedAwaitNext` 在做什么？** Seed 轮发完音后**显式暂停**，等玩家按 [3] 或自动推进进入 reaction。设计目的是**可观察性**——给观察者看清 seed 输出 + 给未来"用户介入"留扩展位 + Replay 系统的章节锚点。

2. **`OnAct01Completed` → `BeginAct02()` 怎么连起来？** Act02::BeginPlay 时扫整个关卡找所有 Act01 director，把它们的 `OnAct01Completed` 绑到 `HandleAct01Completed`。**不需要任何蓝图配置**，但要求 Act02 instance 在 Act01 完成时已经存在于场景。

3. **为何不抽共同基类？** 状态机和配置桶差异都大，省下来的代码不多；现在两个相似实例属于"rule of two"——还没达到抽象的合适触发点。等出现 Act03 同样套路时再抽。

---

## 7. 你应该顺手判断的"修改任务"

按这一阶段的理解，下面 3 个改动你应该能马上想出位置：

| 改动 | 至少影响 |
|---|---|
| 给 Act02 加一个 `IsAct02Running()` BP 函数 | `.h` 加 `BlueprintCallable, BlueprintPure`；`.cpp` 一行 `return SceneState != EAct02State::Idle` |
| 把反应轮数改成可在运行时调而非编辑期固定 | `ReactionRoundCount` 已经是 `BlueprintReadWrite` 了——改值即可。但要小心**正在跑的本局不能减少**（除非补 cancel 逻辑） |
| 加一个新状态 `VotePhase`（投票轮）夹在反应轮之后 | 改 `EAct02State` 枚举 + `Tick` 中的 switch + 新增 `StartVote/TickVote` + 在 `AdvanceReactionRound` 完成最后一轮时跳到 `VotePhase` 而非 `CompleteAct02` |

---

## 下一步

阶段 4 是**三个深度专题**，可分次进行。建议顺序：

| 专题 | 关键文件 | 预估 |
|---|---|---|
| **4a** 存储与哈希链 | `AILiveEventStoreSubsystem.cpp` 的 BeginGame / AppendEvent / ComputeEventHash / VerifyHashChain / ResumeFromGameId | 1h |
| **4b** LLM 双管线 | `OpenAIChatClient` + `AILiveParserClient` + `AILivePromptAssembler` + `AgentRoster::ResolveProviderEndpoint` + `Content/Prompts/Parser/v1.txt` | 1h |
| **4c** 投影 / 登记 / Resume / Delete | EventStoreSubsystem.cpp 的 4 个 reducer + AILiveAgentRegistry + TriggerDeleteExecuted + AILiveListenerFilter | 1h |

读完任意一个专题告诉我，我把它写到对应的 `Learn/04a-*.md` / `04b-*.md` / `04c-*.md`。
