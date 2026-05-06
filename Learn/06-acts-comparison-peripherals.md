# 阶段 6 — Acts 对照与周边

读：

```
Public/Acts/Act01RuleIntroDirector.h
Private/Acts/Act01RuleIntroDirector.cpp     (~736 行 — 线性版导演)

Public/StoryScenarioDirector.h              (170 行 — 独立玩具脚本)
Private/StoryScenarioDirector.cpp           (~551 行 — 双 NPC 对话 + 不接 EventStore)

Public/MinimaxACELibrary.h                  (109 行 — 3 个公开入口)
Private/MinimaxACELibrary.cpp               (~474 行 — TTS + A2F 玻璃层)

Public/AILiveProjectScatterMover.h          (31 行)
Private/AILiveProjectScatterMover.cpp       (~211 行 — EQS + 贪心匹配 + ProcessEvent)

Public/SightMemoryComponent.h               (62 行 — UActorComponent 包装感知)
Private/SightMemoryComponent.cpp            (~195 行)

Public/AILiveProjectPerceptionLogger.h      (122 行 — BP 工具函数库)
Private/AILiveProjectPerceptionLogger.cpp   (~318 行)
```

读完这一阶段你应当能回答：

1. Act01 和 Act02 共享哪些 idiom？为什么不抽公共基类？
2. StoryScenarioDirector 是干嘛的？它和 Act 体系的关系？
3. `MinimaxACELibrary` 三个公开入口（`TriggerMinimaxSpeech` / `WithNoise` / `FromPawnWithNoise` / `FromPawnNative`）有什么区别？要扩展时该用哪个？
4. ScatterMover 的"贪心最近匹配"是什么？为什么不用简单的索引对应？
5. `SightMemoryComponent` 和事件溯源系统的关系？要不要把 SightStimulus 写进 events 表？

---

## 1. Director Pattern：Act01 与 Act02 的共同 idiom

读完两个 cpp 你会发现它们**结构完全平行**。下面是共同模板：

```cpp
class A<X>Director : public AActor {
    // 1. UPROPERTY 配置（编辑器调）
    bool      bEnableKeyTrigger = true;
    FKey      StartKey;
    TSubclassOf<AActor> NPCMoverClass;
    TSubclassOf<AActor> NavTargetClass / NavLookTargetClass;
    UEnvQuery* ScatterQueryAsset;
    
    // 2. 完成 / 失败 multicast delegate
    FOn<X>CompletedDelegate OnCompleted;
    FOn<X>FailedDelegate    OnFailed;

    // 3. State machine（4–9 个 state）
    enum class E<X>State { Idle, ..., Idle };
    E<X>State SceneState = Idle;
    
    // 4. 标准 actor 生命周期
    BeginPlay()  → 加载默认资产 + Cache + Resolve + RegisterDebugConsoleCommands()
    Tick(Dt)     → 检测 StartKey + state 分发
    EndPlay()    → UnregisterDebugConsoleCommands()
    
    // 5. 公开 API（蓝图可调）
    BeginAct<X>()  → 校验 + EventStore.BeginGame + StartXxx()
    Cancel<X>()    → 重置状态到 Idle
    
    // 6. 私有辅助
    CacheInitialNPCTransforms / ResolveTargetActors / DebugMessage / FailAct<X>
    
    // 7. 控制台命令
    act01.start / act01.cancel / act02.start / act02.cancel / act02.next
};
```

### 1.1 共同辅助函数清单

下面 **8 个函数 Act01/Act02 几乎逐字相同**：

| 函数 | Act01 cpp 行 | Act02 cpp 行 |
|---|---|---|
| `CacheInitialNPCTransforms` | 207-225 | 264-281 |
| `ResolveActorByClassAndLabel` | 286-307 | 283-303 |
| `ResolveTargetActors` | 309-346 | 305-328 |
| `RegisterDebugConsoleCommands` | 348-365 | 416-438 |
| `UnregisterDebugConsoleCommands` | 372-385 | 445-463 |
| `ResetToInitialPositions` | 387-411 | 392-414 |
| `AreAllNPCsIdle` | 528-559 | 529-544 |
| `DebugMessage` | 709-716 | 1403-1410 |

**几乎重复**——尤其 `CacheInitialNPCTransforms` / `ResolveActorByClassAndLabel` / `ResolveTargetActors` 三套是**逐字 copy-paste**，只换了字段名。

### 1.2 自检题 #1 答案

> 为什么不抽公共基类？

**几个候选答案**（按不靠谱程度排序）：

1. **"现状 OK 不重构"**——MVP 阶段重构不增加功能。✓
2. **"两个状态机生命周期不同"**——Act01 完成即 Idle；Act02 完成即 Idle 但中间有循环 + 异步 LLM。这层差异让基类抽象不平凡：基类要不要管 LLM 状态？要不要管 Speech？基类管多了变成 god class，管少了又没什么可抽。
3. **"代码量小，DRY 收益不明显"**——重复代码合计 ~150 行。抽基类后单测 + 文档维护成本可能 > 当前重复维护成本。
4. **风险**：Act01 想小改（比如改 NavTarget 解析逻辑）时，**不影响 Act02**——这是当前设计的隐藏价值。基类抽象会增加耦合。

**建议**：等 Act03 / Act04 出现时再抽——三处重复才有抽象价值（"rule of three"）。两处重复保留 copy 比硬抽象好。

---

## 2. Act01 独有的部分

Act01 的**线性 4 状态**：`Idle → OpeningDoors → NPCsMovingToTV → PlayingVideo → Idle`。

```
按 [1] → BeginAct01() → 验 NavTarget+NavLookTarget+MediaPlate 都齐 → BeginGame
       → ResetToInitialPositions（**注意**：和 Act02 不同，这里是要重置！）
       → StartOpeningDoors → state = OpeningDoors
            ↓ TickOpenDoors 1.0s 内插值开门
       → StartNPCMovement → 写 setup#1 事件 + ScatterMover 派发
            ↓ TickNPCMovement 等 NPCs idle + 0.5s settle
       → StartVideo → 写 setup#2 事件 + 启动 MediaPlate
            ↓ TickVideoFallback 兜底 + HandleVideoEnded 回调
       → CompleteAct01 → OnAct01Completed.Broadcast → state = Idle
                              ↑
                          Act02 在 BeginPlay 自动 bind 这条 → HandleAct01Completed → BeginAct02
```

### 2.1 开门动画（cpp:431-448）

```cpp
TickOpenDoors(Dt):
    DoorAnimationElapsed += Dt
    Alpha = clamp(elapsed / 1.0s, 0, 1)
    for each cached door:
        Lerped = Lerp(ClosedRot, ClosedRot + DoorOpenRelativeRotation, Alpha)
        door->SetRelativeRotation(Lerped)
    if Alpha >= 1.0:
        StartNPCMovement()
```

**关键技巧（cpp:227-265）**：`CacheCellDoorComponents` 在 BeginPlay 时按 actor label name 做精确匹配（`CellDoorActorNames` 数组的 10 个名字写死在构造器 cpp:38-50），找到 actor 后取它的 `SM_Door` 组件并记录 `ClosedRelativeRotation`。

**为什么硬编码 actor 名？**——关卡里的 cell door 是 `SM_blocking_prison_Cube_270` 这类自动生成名字，没办法用 class+label 通用查找。每加一个 cell 都得改 cpp 数组——**这个数据应该挪到 DataAsset**，但 MVP 没做。

### 2.2 MediaPlate 视频播放（cpp:561-675）

```cpp
StartVideo:
    Player = MediaPlateCached->MediaPlateComponent->GetMediaPlayer()
    Player->OnEndReached.AddUniqueDynamic(this, &HandleVideoEnded)   ← bVideoDelegateBound = true
    PlateComponent->Open()                                            ← 异步 ready
    写 setup#2 事件 "rule intro video started"

TickVideoFallback:
    if Player->IsReady() && !Player->IsPlaying():
        PlateComponent->Play()                                        ← 真正开始播
    if !Player->IsReady() && elapsed > 15s:
        FailAct01("MediaPlayer did not become ready")
    if elapsed > VideoFallbackTimeoutSeconds:
        写 setup#3 事件 + CompleteAct01                                ← 兜底退出

HandleVideoEnded:
    写 setup#3 事件 + CompleteAct01                                    ← 正常退出
```

**两条退出路径**：

1. **正常路径**：`Player->OnEndReached` 委托回调 → `HandleVideoEnded`
2. **兜底路径**：`VideoFallbackTimeoutSeconds`（默认未读 .h，假定 ~30s）超时 → `TickVideoFallback` 自己强制 CompleteAct01

委托一定要在 `EndPlay` / `CancelAct01` 时 unbind（`UnbindVideoDelegate` cpp:677-691），否则 actor destroy 后 MediaPlayer 仍持有这个 delegate 会崩。

### 2.3 Setup 阶段写 3 个 orchestrator.resolved 事件（cpp:718-736）

```cpp
AppendOrchestratorRoundResolved(Text):
    FAILiveEvent Ev {
        RoundNo    = 0,
        Phase      = Setup,
        Actor      = "orchestrator",
        EventType  = OrchestratorResolved,
        Visibility = ["public"],
        PayloadJson = {"text": EscapeJsonString(Text), "phase": "setup"},
    };
    return Store->AppendEvent(Ev) > 0;
```

**Act01 在 setup 阶段写 3 条事件**：

1. cell doors opened（StartNPCMovement 时）
2. rule intro video started（StartVideo 时）
3. rule intro video ended（HandleVideoEnded 或 fallback 时）

**这是事件溯源的"开局历史"**——Act02 的 PromptAssembler 不主动用，但日后 Resume 协议或者需要"游戏从规则视频结束才开始"语义时这些事件就是真相源。

`AppendOrchestratorRoundResolved` 失败时（前 2 条）`FailAct01`；第 3 条**只 warning 不 fail**——视频已经看完了，没必要因为审计写不进去就让玩家看不到 Act02。

---

## 3. `StoryScenarioDirector`（独立玩具脚本）

### 3.1 它是什么

**两个 NPC 走到一起说话再分开**的硬编码脚本。**完全独立于 Act 体系**——不接 EventStore，不进游戏主流程，**主要用作 TTS+A2F 集成的最小可运行 demo**。

状态机：

```
Idle → MovingToMeeting → Dialogue → Returning → Idle
```

主要靠 `FTimerHandle` 而不是 Tick 来驱动（cpp:114 `MovementPollInterval = 0.2f`）——和 Act 的 Tick 模型不同。

### 3.2 与 Act 体系的对照表

| 维度 | StoryScenarioDirector | Act01/02 |
|---|---|---|
| 驱动模型 | `FTimerHandle` 0.2s 轮询 | `Tick(Dt)` 每帧 |
| EventStore | **不写任何事件** | BeginGame + 写多种事件 |
| TTS | 用 `MinimaxACELibrary` | Act02 用同一接口 |
| 状态广播 | `OnArrived` / `OnLeft` / `OnFailed` | `OnAct0XCompleted` |
| 输入 | DialogueLines TArray + 两个 NPC class | Roster + GameRule |
| LLM | **不调** | Act02 双 LLM |

**作用**：留给开发期"我想看 NPC 嘴动起来"快速验证用。生产应该会被删掉或改成 unit test 性质的辅助。

### 3.3 关键依赖

`SightMemoryComponent` —— Story scenario 用 NPC1 的 Sight Memory 来做"NPC2 走过来时 NPC1 是否看见"的视觉验证（cpp 154 字段 `NPC1SightMemory`）。这是 SightMemoryComponent **当前唯一的 C++ 调用点**。

---

## 4. `MinimaxACELibrary` 玻璃层

**4 个公开入口 + 1 个工具函数**。这是 Act02 调到的关键依赖。

### 4.1 入口对照表

| 入口 | TTS | A2F | 听觉 | OnFinished 回调 | 用途 |
|---|---|---|---|---|---|
| `TriggerMinimaxSpeech` | ✅ | ✅ | ❌ | ❌ | BP 简单调用，无感知 |
| `TriggerMinimaxSpeechWithNoise` | ✅ | ✅ | ✅ AISense_Hearing | ❌ | 用于 NPC 听到他人说话 |
| `TriggerMinimaxSpeechFromPawnWithNoise` | ✅ | ✅ | ✅ | ❌ | NPC pawn 自动找 VisualOverride |
| `TriggerMinimaxSpeechFromPawnNative` | ✅ | ✅ | ✅ | ✅ GT 回调 | **Act02 用这个** |
| `GetVisualOverrideAudioTarget` | — | — | — | — | 取 NPC 的可见 ChildActor |

### 4.2 共同流程（4 个入口都一样的 8 步）

```cpp
1. 校验 SpeakerPawn / AudioTarget 有效
2. 校验 Text + ApiKey 非空
3. AudioTarget = GetVisualOverrideAudioTarget(Pawn)（pawn 入口才走）
4. Consumer = GetOrAddCurveSource(AudioTarget)
       ├─ FindComponentByClass<UACEAudioCurveSourceComponent>
       └─ 没有就 NewObject + SetupAttachment + RegisterComponent + AddInstanceComponent
5. WeakConsumer = TWeakObjectPtr<UACEAudioCurveSourceComponent>(Consumer)
6. Async(ThreadPool, [Req, WeakConsumer, ...]) {
       Result = MinimaxSpeech::RequestBlocking(Req)        ← TTS HTTP（worker 线程）
       
       if Result.bSuccess && WeakConsumer.IsValid():
           # WithNoise 才有：AsyncTask GameThread → AISense_Hearing::ReportNoiseEvent
           AsyncTask(GameThread, []{ UAISense_Hearing::ReportNoiseEvent(...) });
           
           # 关键：在 dispatch 之前发听觉事件——dispatch 阻塞数秒，dispatch 后发会推迟到几乎播完
           
           bOk = FACERuntimeModule::Get().AnimateFromAudioSamples(
               Live, Result.Samples, 1, Result.SampleRate,
               bEndOfSamples=true, ..., A2FProviderName);
       
       FireFinishedWorker(bOk)        ← Native 入口才有：AsyncTask GameThread 触发 OnFinished
   }
```

### 4.3 关键设计要点

**(1) `GetOrAddCurveSource` 必须 GameThread**（cpp:24-47）

```cpp
check(IsInGameThread());
```

`NewObject` / `SetupAttachment` / `RegisterComponent` 都是 UObject API，必须 GT。这函数调用点都在 `Trigger*` 入口的同步部分（在 `Async(...)` 之前）——保证 worker lambda 拿到的 `WeakConsumer` 是已经创建好的。

**(2) WithNoise 的听觉事件必须在 audio dispatch 之前发**（cpp:238-262）

注释原话：

> *"AnimateFromAudioSamples 是阻塞 streaming dispatch（多个 chunk 串行 send 到 ACE thread），实测可能持续数秒。如果把 ReportNoiseEvent 放到 dispatch 之后，hearing 命中会推迟到 audio 几乎播完时，超出测试 timer 窗口。"*

**(3) `UAISense_Hearing::ReportNoiseEvent` 必须 GameThread**

> *"AISense API 不是线程安全的，GetActorLocation 也必须在 GameThread 内取。"*

所以 `AsyncTask(ENamedThreads::GameThread, ...)` 包裹——worker 线程派一个 GT task 上来发感知事件。

**(4) `FromPawnNative` 的 OnFinished 委托**

```cpp
auto FireFinished = [OnFinished](bool bSuccess) {
    if (!OnFinished.IsBound()) return;
    AsyncTask(ENamedThreads::GameThread, [OnFinished, bSuccess]() {
        OnFinished.ExecuteIfBound(bSuccess);
    });
};
```

**所有失败路径都调 `FireFinished(false)`**——保证 caller 一定收到一次 finished 通知，不会泄漏。Act02 的 `HandleSpeechFinished` 就是这个回调。

`OnFinished` 在 worker 线程被引用，但 `ExecuteIfBound` 一定在 GT——`AsyncTask(GameThread, ...)` 是关键。

### 4.4 `GetVisualOverrideAudioTarget`（cpp:279-310）

```cpp
TArray<UChildActorComponent*> ChildActorComponents;
SpeakerPawn->GetComponents<UChildActorComponent>(ChildActorComponents);
for (Component : ChildActorComponents) {
    if (Component->ComponentHasTag("VisualOverride") || Component->GetFName() == "VisualOverride") {
        return Component->GetChildActor();
    }
}
return nullptr;
```

**两种识别**：tag 或 name。BP_NPC_MH_Character_X 用 tag，老 BP 用 name——双兼容。

**返回的是 ChildActor 不是 ChildActorComponent**——因为 ACEAudioCurveSourceComponent 要挂到可见 actor 上（脸 mesh actor），不能挂到 ChildActorComponent 容器。

### 4.5 `PrewarmA2F`（cpp:50-54）

```cpp
FACERuntimeModule::Get().AllocateA2F3DResources(A2FProviderName);
```

**Provider 第一次用会触发 TRT 编译**（NVIDIA TensorRT 模型加载），延迟数秒到几十秒。Prewarm 让你在 BeginPlay 时做一次预热——Act02 BeginPlay 应该调（当前没调，DevLog 留了 TODO）。

### 4.6 `GetMinimaxApiKeyFromProjectEnv`（cpp:72-107）

读 `<ProjectDir>/.env` 找 `minimax=...` 行。**和 `ProjectEnvLoader::Get("minimax")` 是两套独立解析**——前者是这个 lib 的早期实现，后者是阶段 4b 看的统一封装。注释（h:88-89）也承认：

> *"Keep this Blueprint function name stable; BP_MH_Character_1 references it."*

——保留是因为 BP 蓝图依赖了这个函数名。**新代码用 `ProjectEnvLoader::Get`**。

---

## 5. `AILiveProjectScatterMover`（EQS + 贪心匹配）

**Static BP 工具函数**——`UBlueprintFunctionLibrary` 子类，只有一个公开 API。

### 5.1 整体流程（cpp:24-211）

```cpp
ScatterNPCsAroundTarget(QueryAsset, CenterActor, NPCs, LookTarget, _):
    1. 校验 + 过滤掉玩家控制的 Pawn (cpp:48-63)
    2. WeakNPCs 数组（防 actor 销毁）
    3. FEnvQueryRequest(QueryAsset, Querier=CenterActor)
    4. Request.Execute(AllMatching, lambda)
       ↓ (EQS 异步在 EQS 线程跑)
       Result.GetAllAsLocations(Locations)   // 通常 ~37 个候选点
       
       5. 贪心最近匹配（O(N² × min(N,P))）：
          while (有未分配 NPC 且有未占用点):
              找全局最短距离的 (NPC, point) 对
              分配它
       
       6. for each NPC: 
          ProcessEvent(BP 函数 "MoveAndLookAtLocation", {Loc, LookTarget})
```

### 5.2 贪心最近匹配 vs 索引对应（cpp:107-157）

**简单实现**：按数组索引一对一配（NPC[0]→Locations[0], NPC[1]→Locations[1], ...）。问题：有的 NPC 离自己被分配的点很远，跑半天才到，有的近得多。

**当前实现**（贪心）：每轮在所有未分配 NPC × 未占用点里挑全局最短距离的对。

```
While (Remaining > 0):
    BestDist = +INF
    For (i: 未分配 NPC):
        For (j: 未占用 point):
            D = DistSquared(NPC[i].pos, Locations[j])
            if D < BestDist: BestDist = D, BestNPC = i, BestPoint = j
    分配 (BestNPC → BestPoint)
    --Remaining
```

**复杂度** O(N² × min(N, P))——10 NPC × 37 点，单次 ~3700 比较 ≈ 几微秒，完全可接受。**比 Hungarian 算法简单多了**，效果接近最优。

注释（cpp:107-109）原话：

> *"贪心最近匹配：每轮在所有未分配 NPC × 未占用点里挑全局最短距离的一对，分配掉。比 stride 采样合理：NPC 直接走最近的散点而不是被强制配到远处。"*

### 5.3 `ProcessEvent` 调 BP 函数（cpp:175-195）

```cpp
UFunction* Fn = NPC->FindFunction(FName("MoveAndLookAtLocation"));
FMoveAndLookAtLocationParams Params;
Params.MoveLocation = Target;
Params.LookTarget   = LookTargetActor;
NPC->ProcessEvent(Fn, &Params);
```

**`FMoveAndLookAtLocationParams` 必须**和 BP 函数签名 byte-for-byte 一致**：

```cpp
struct FMoveAndLookAtLocationParams {
    FVector MoveLocation;        // BP 输入
    AActor* LookTarget;          // BP 输入
    bool    bSucceeded;          // BP 输出
};
```

**BP 函数 `MoveAndLookAtLocation` 在哪**？在 `BP_NPC_MH_Character_1` 父类（或 `SandboxCharacter_Mover`）的 BP 图里——**不在 C++**。这是一种 C++ → BP 调用模式，避免要给每个 NPC pawn 写 C++ MoveTo 逻辑。

> 缺点：编译器不能校验签名。BP 改了参数名 / 类型，C++ 端的 Params struct 也要同步改——容易漂移。**当前测试覆盖**：每次改完 BP 的 MoveAndLookAtLocation 必须 PIE 烟测 scatter，否则会有 silent 失败（NPC 不动也不报错）。

### 5.4 NPC 多于点数的兜底（cpp:165-170）

```cpp
LocIdx = AssignedIdx[i];
if (LocIdx < 0):  // 这个 NPC 没分到点（NPC 数 > 点数）
    LocIdx = NumPoints - 1;  // 复用最后一个点
```

**会 log warning 但不 fail**。10 NPC vs 37 点的当前布局不会触发这条路径——但万一未来 NPC 数变多或 EQS 配置改紧，至少不会 crash。

---

## 6. `SightMemoryComponent`（视觉感知缓存）

### 6.1 它是什么

包装 UE AIPerception 系统的便捷组件——**把"视觉感知到的 actor + 位置"按 actor 缓存起来**，让 BP 端能用 `GetLastSeenLocation(Target)` 这种 API 而不是直接听 `OnPerceptionUpdated` 委托。

```cpp
TMap<TWeakObjectPtr<AActor>, FVector> LastSeenLocations;     // 每个见过的 actor 最近坐标
TSet<TWeakObjectPtr<AActor>> CurrentlyVisibleActors;          // 当前可见集合
```

### 6.2 与事件溯源系统的关系

**完全独立**——SightMemory 不写 events，events 也不读 SightMemory。**两套独立的"AI 心智模型"**。

- **SightMemory**：UE 引擎层感知（AIPerception sight stimulus），运行时数据，不持久化
- **events**：业务层事件溯源（speech / vote / commitment），SQLite 持久化，进 prompt

### 6.3 自检题 #5 答案

> 要不要把 SightStimulus 写进 events 表？

**当前不写，未来也大概率不写**。理由：

1. **不可压缩的数据膨胀**：sight 事件 N 个 NPC × 每帧 60 次 × 30 分钟一局 = 数千万 stimulus；存这些 events 表会爆
2. **没有玩家可读语义**：sight stimulus 是引擎数据（位置 / 角度 / 强度），不是社会博弈事件；放进 prompt 是噪音
3. **如果 NPC 真要"用看到 NPC02 走过去"做决策**：应该是在 prompt 装配时**临时**问 SightMemory（"你最近看到了谁？"），结果作为 prompt 段；不持久化进 events

**如果要桥接**：写一种新事件 `SystemSightHeartbeat`（每 3 秒采样一次 + visibility="system"），ProjectAgentViewState 里多一段 `recent_sightings`。但目前 MVP 不接。

---

## 7. `AILiveProjectPerceptionLogger`

### 7.1 它是什么

**纯 BP 工具函数库**——给蓝图调的 5 个 static 函数：

| 函数 | 作用 |
|---|---|
| `GatherSightPerception(Perceiver)` | 返回 `TArray<FPerceivedAgentInfo>`：每个被感知 actor 的 distance / direction / yaw |
| `LogPerceptionToOutput` | 同上 + 输出到 Output Log（BP 调试） |
| `GatherHearingPerception` | 返回 `TArray<FHeardSoundInfo>` |
| `LogHearingPerceptionToOutput` | hearing 版的 log |
| `GetChildActorOf(Component)` | UChildActorComponent::GetChildActor() 的 BP wrapper |
| `ClaimFirstSlotInActor(SO, User)` | SmartObject claim 一站式（绕开 BP wildcard 限制） |

### 7.2 `FPerceivedAgentInfo` / `FHeardSoundInfo` 是 USTRUCT

```cpp
struct FPerceivedAgentInfo {
    FName Identity;                 // 被感知 actor 的标识
    float DistanceCm;               // 距离 (cm)
    FRotator DirectionFromPerceiver; // 方向
    float RelativeYawDeg;            // 相对 perceiver forward 的 yaw [-180, 180]
    bool  bCurrentlySensed;          // 当前帧是否在感知
    float StimulusAge;               // 上次感知到现在的秒数
    FVector LastStimulusLocation;    // 上次感知位置
};
```

**这是给 BP 端做 "我能看到什么/听到什么" 决策的数据结构**——和 SightMemoryComponent 的 TMap 是两种暴露方式（一个 functional 一个 stateful），数据来源相同（AIPerception）。

### 7.3 与 LLM/事件溯源系统的关系

**完全独立**。Perception Logger 没有任何 EventStore 调用。**它是给 BP 做调试 + UI 显示用的**——比如某个 debug widget 显示 NPC1 当前能看到的人列表。

### 7.4 `ClaimFirstSlotInActor`（SmartObject）

```cpp
ClaimFirstSlotInActor(SmartObjectActor, UserActor, _):
    1. FindSmartObjectsInActor(SmartObjectActor) → handles[]
    2. for each handle:
        if MarkSmartObjectSlotAsClaimed(handle) succeeds:
            return handle
    return invalid handle
```

注释（h:108-112）原话：

> *"绕开 BP Array_Get wildcard pin 在 MCP 下不可用的问题。"*

——这是 **MCP 操作蓝图的 workaround**：MCP 工具控制蓝图时，`Array_Get` 的 wildcard pin 不能正确解析，所以把"找首个有效 slot 并 claim"封到 C++ 里，BP 只调一次 `ClaimFirstSlotInActor` 就拿到 ClaimHandle。

---

## 8. 自检题答案

### 8.1 自检题 #1 ✓ 已在第 1.2 节答

### 8.2 自检题 #2

> StoryScenarioDirector 是干嘛的？关系？

**独立玩具脚本**——两个 NPC 走过来对话再走回去。**完全不接 EventStore / 不接 LLM / 不接 PromptAssembler**——只用 MinimaxACELibrary 验证 TTS+A2F 链路。

**和 Act 体系无任何依赖**——可以独立删除。生产里要么留作开发期 demo，要么删掉。

### 8.3 自检题 #3

> 4 个 MinimaxACELibrary 公开入口的区别？

| 入口 | 声响 | OnFinished 回调 |
|---|---|---|
| `TriggerMinimaxSpeech` | ❌ 不发听觉 | ❌ |
| `TriggerMinimaxSpeechWithNoise` | ✅ 发 AISense_Hearing | ❌ |
| `TriggerMinimaxSpeechFromPawnWithNoise` | ✅ + 自动找 VisualOverride | ❌ |
| `TriggerMinimaxSpeechFromPawnNative` | ✅ + 自动找 VisualOverride | ✅ GameThread 回调 |

**Act02 用 `TriggerMinimaxSpeechFromPawnNative`**——因为 watchdog 需要"语音播完"的回调。

新功能扩展时：
- BP 调用 + 不需要回调 → `TriggerMinimaxSpeechFromPawnWithNoise`
- C++ + 需要回调 → `TriggerMinimaxSpeechFromPawnNative`
- 不要新增第 5 个入口——现有 4 个已经覆盖矩阵全部

### 8.4 自检题 #4

> 贪心最近匹配 vs 索引对应？

**贪心**：每轮全局找最短 (NPC, point) 距离对配。复杂度 O(N² × min(N, P))。优势——NPC 走最短路。

**索引**：i→i 简单粗暴，O(N)。劣势——靠近 spawn 区的 NPC 可能被强配到远点，平均移动距离 ↑↑。

10 NPC 场景下两者性能差异不可观察（< 1ms），但视觉效果差异显著——所以选贪心。

### 8.5 自检题 #5 ✓ 已在第 6.3 节答

---

## 9. 关键不变量速查表

| # | 不变量 / 约束 | 在哪强制 |
|---|---|---|
| 1 | Act01/02 共 8 个辅助函数逐字相同（暂不抽基类） | Act01 cpp + Act02 cpp |
| 2 | Act01 setup 阶段写 3 条 OrchestratorResolved 事件 | Act01 cpp:458, 604, 622 |
| 3 | Act01 第 3 条事件失败仅 warning（视频已结束，不阻塞游戏） | Act01 cpp:622-626 |
| 4 | StoryScenarioDirector 完全独立于 Act / EventStore | StoryScenarioDirector.cpp 不 include AILiveEventStoreSubsystem |
| 5 | MinimaxACELibrary 4 入口共用 Async ThreadPool 模式；GT 部分必须先做 | MinimaxACELibrary.cpp:24-26 |
| 6 | WithNoise 的 ReportNoiseEvent 必须在 AnimateFromAudioSamples 之前 | MinimaxACELibrary.cpp:238-262 |
| 7 | AISense_Hearing API 必须 GameThread 调用 | MinimaxACELibrary.cpp:243 + AsyncTask(GameThread) |
| 8 | OnFinished 委托所有失败路径都调一次（不泄漏） | MinimaxACELibrary.cpp:352-362 + 421, 424, 432, 469 |
| 9 | GetOrAddCurveSource 必须 GameThread（NewObject + RegisterComponent） | MinimaxACELibrary.cpp:26 check |
| 10 | ScatterMover 用贪心最近匹配；过滤玩家 pawn | ScatterMover.cpp:107-157 + cpp:48-63 |
| 11 | ScatterMover 通过 `ProcessEvent` 调 BP 函数 `MoveAndLookAtLocation`，签名必须对齐 | ScatterMover.cpp:175-195 |
| 12 | SightMemoryComponent 完全独立于 events 表 | SightMemoryComponent.cpp 不 include AILiveEventStoreSubsystem |
| 13 | Act02 BeginPlay 自动 bind 场上所有 Act01 director 的 OnAct01Completed | Act02 cpp:121-135 |
| 14 | `GetMinimaxApiKeyFromProjectEnv` 旧入口保留（BP 引用稳定）；新代码用 ProjectEnvLoader::Get | MinimaxACELibrary.h:88-89 |

---

## 10. 扩展任务推演

| 改动 | 至少要动 |
|---|---|
| 新增 Act03（投票 + 淘汰阶段） | (1) 复制 Act02 整个文件改名；(2) 加 EAct03State 状态；(3) Act03 BeginPlay 自动 bind Act02.OnAct02Completed；(4) **不抽基类**——前面解释过，rule of three 还没满足 |
| 把 Act01 的 cell door actor names 从硬编码挪到 DataAsset | (1) 新建 UDataAsset 子类 `UAct01CellDoorConfig`，含 `TArray<FName> CellDoorActorNames` + `FRotator OpenRelativeRotation` + `FName MeshComponentName`；(2) Act01.h 加 `TObjectPtr<UAct01CellDoorConfig>` UPROPERTY；(3) BeginPlay 改读 DataAsset 字段；(4) 关卡里建 DataAsset 实例配置 |
| 给 ScatterMover 加"NPC 已经在合适位置就不派遣"短路 | (1) 计算每个 NPC 当前位置到 CenterActor 的距离；(2) 如果 < threshold 就跳过 ProcessEvent；(3) 注意：不能用 EQS Locations 距离判断，因为那只是候选点，要的是 NPC 现位置 vs CenterActor。 |
| 在 SpeechPublic 写入后**也**同步触发 AISense_Hearing（绕过 Minimax） | (1) Act02 GatherTickAndResolveFloor Stage B 写 speech.public 之后增加：`UAISense_Hearing::ReportNoiseEvent(WinnerPawn, ...)`；(2) **caveat**：MinimaxACELibrary::TriggerMinimaxSpeechWithNoise 已经发了一次 hearing event——这样会重复发；要么二选一，要么在 speech.public 写入时**只**给 visibility=public（让 NPC 通过 prompt 知道），听觉留给 Minimax 那一次 |
| 让 PerceptionLogger 把 Gather* 的结果也写一条 events | (1) 在 Logger.cpp 内增加 `GetEventStore` 取 EventStore；(2) 写 `EAILiveEventType::SystemSightHeartbeat`（**注意**：先在 EventTypes.h 加这种类型 + DDL 加值）；(3) visibility=["system"]——避免污染 NPC prompt；(4) 决定采样频率（不是每帧；按 BP 决策频率，比如每 3s）；(5) **强烈建议先评估写入压力**——这个改动可能导致 events 表暴涨 |

---

## 11. 阅读完成 — 整体串联

恭喜，你现在能从第一行 Build.cs 一直读到 1466 行 Act02 cpp 的最后一个 lambda 闭包。下面这张图你应能凭记忆画出：

```
按 [1] (Act01)
   ↓ BeginPlay 已 bind Act02.HandleAct01Completed
   ├─ OpeningDoors (1.0s lerp)
   ├─ NPCsMovingToTV (ScatterMover EQS + 贪心)
   ├─ PlayingVideo (MediaPlate + OnEndReached)
   └─ CompleteAct01 → OnAct01Completed.Broadcast
                              ↓
                     HandleAct01Completed → BeginAct02
按 [2] (Act02 / 直接进入)
   ├─ PrescatterToTV (ScatterMover)
   ├─ SeedDispatch / SeedAwait
   │      ↓ RunTick
   │      ├─ EventStore.BeginTick                     [4a]
   │      └─ for each NPC: Async ThreadPool
   │             ↓ RunAgentTickInWorker (worker)
   │             ├─ Reasoner LLM (DeepSeek/GLM/Qwen3) [4b]
   │             └─ Parser LLM (DeepSeek 写死)        [4b]
   ├─ TickLLMAwait (200s GT watchdog)
   ├─ GatherTickAndResolveFloor (5 stages)            [5]
   │      ├─ Stage A: AppendEventsAtomically 4 通道   [4a]
   │      ├─ Stage B: ResolveFloor + ListenerFilter passthrough [4c]
   │      │           + speech.public(parent_event_id ← intended)
   │      ├─ Stage C: DeriveActionIntents
   │      ├─ Stage D: tick_resolved + tick_audit (拆开)
   │      ├─ Stage D.5: AsyncTask RebuildProjections   [4c]
   │      └─ Stage E: StartSpeak (winner only)
   ├─ SeedSpeak / TTS (MinimaxACELibrary.FromPawnNative) [6]
   │      ├─ TTS HTTP (worker)
   │      ├─ AISense_Hearing (GT)
   │      ├─ AnimateFromAudioSamples (worker → ACE)
   │      └─ OnFinished GT 回调 → HandleSpeechFinished
   ├─ TickSpeakWatchdog (30s)
   ├─ GatedAwaitNext (seed 自动跳，reaction 等 [3])
   ├─ ReactionDispatch / Await / Speak (× ReactionRoundCount)
   └─ CompleteAct02 → OnAct02Completed.Broadcast
```

整个仓库 49 个文件、约 9000 行 C++ 代码，从事件溯源 + 双 LLM 流水线 + Bid/Floor 协议 + 投影 + Resume + Acts 状态机 + TTS/A2F 玻璃层全部理顺。

### 阅读完成自检（从 0 阶段到 6 阶段一共 6 题）

回看每个阶段末尾的自检题：

| 阶段 | 自检题数 | 你能口述答案？ |
|---|---|---|
| 4a 存储与哈希链 | 4 | ☐ |
| 4b LLM 双管线 | 4 | ☐ |
| 4c 投影 / 登记 / Resume / Delete | 4 | ☐ |
| 5 主循环执行追踪 | 5 | ☐ |
| 6 Acts 对照与周边 | 5 | ☐ |

**任何一题答不上**，回到对应阶段对照源码 + 我的笔记。

### 接下来你能做的事

至此你具备**独立修改/扩展任意子系统**的能力。下一步推荐：

1. **跑一次完整 PIE**（按 [1] → 自动 [2] → 等所有反应轮跑完，~11 分钟）
   - 用 SQLite Browser 打开 `Saved/Games/<game_id>.db` 看 events 表
   - 数 events 类型分布是否符合预期（每拍 4 通道 × 10 NPC + 1 tick_anchor + 1 tick_resolved + 1 tick_audit + 0-1 speech.public）

2. **挑一个扩展任务**实现（每个阶段末尾的"扩展任务推演"任选一个）
   - 推荐从 4b 的"修 ListenerFilter 真的过滤"开始——改动小、价值高、不破坏现有不变量

3. **写新的 PRD 段落**改动时
   - 先翻这份 Learn 笔记找哪一阶段的不变量受影响
   - 改完用对应阶段的"扩展任务推演"格式记录"动了哪些行"

随时回来贴文件:行号让我深入；或者贴一段你不确定的 .cpp 让我对照你的理解。
