# AILiveProject 代码阅读导览（结构化清单）

## Context

你需要从零自读整个 `AILiveProject` 仓库，但所有 C++ 是过去若干轮和 Claude Code 协作写出的，缺一份"按什么顺序读才不绕路"的地图。

目标：

- 读完后能独立修改/扩展任意子系统（事件溯源、双 LLM 流水线、Bid/Floor、投影、Resume/Delete、Acts 状态机、TTS/A2F 玻璃层）
- 形态：你按节奏读这份清单，每完成一个阶段（或卡在任何一个文件/函数），回来贴行号让我现场讲解
- 与之配合的"我会讲什么"段已写在每个阶段末尾

阅读时间预估总计 ~6h，可分多段。建议按 0 → 6 顺序，**不要跳过阶段 1（词汇表）**——后面所有讨论都基于那里的术语。

---

## 项目骨架（先记住这张图）

```
单 C++ 模块 AILiveProject（49 文件）
├─ Memory/     ← 中央枢纽：UAILiveEventStoreSubsystem（事件溯源 + 哈希链 + 4 个投影 reducer）
├─ Acts/       ← 两个有状态导演 Actor：Act01（线性）、Act02（双 LLM 循环）
├─ LLM/        ← OpenAIChatClient（Reasoner）+ AILiveParserClient（Parser）+ AgentRoster（多供应商）
├─ Util/       ← Sha256 / JsonEscape / ProjectEnvLoader（.env 解析）
└─ MinimaxACELibrary  ← 既有玻璃层：LLM 输出 → MiniMax TTS → NVIDIA A2F-3D 面部动画
```

设计模式：**Subsystem 是状态枢纽；命名空间纯函数（PromptAssembler / Parser / AgentRegistry）做派生；Director 是有状态编排者，串起所有调用。**

---

## 阶段 0 — 鸟瞰（10 min）

读这 3 个文件确定"模块边界"：

| 文件                                          | 看什么                                                                                                  |
| --------------------------------------------- | ------------------------------------------------------------------------------------------------------- |
| `Source/AILiveProject/AILiveProject.Build.cs` | Public/PrivateDependencyModuleNames：HTTP / Json / SQLiteCore / OpenSSL / ACERuntime / NavigationSystem |
| `AILiveProject.uproject`                      | 启用的插件：MetaHuman 套件、ACE Reference、Mover、PoseSearch、AnimationWarping                          |
| `Source/AILiveProject/Public/AILiveAgent.h`   | 1 行 UInterface 标记类——感受"打 tag 才被纳入 AI 体系"的契约                                             |

**自检**：你能说出"为什么 SQLiteCore 和 OpenSSL 都是 Required"吗？（答案在阶段 1）

---

## 阶段 1 — 词汇表（30 min，**必读**）

读这 4 个纯类型 header，建立后面一切讨论的术语：

| 文件                               | 重点结构 / 枚举                                                                                                                        |
| ---------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------- |
| `Public/Memory/AILiveEventTypes.h` | `FAILiveEvent`（27 字段）、`EAILiveEventType`（27 种事件）、`EAILivePhase`、`Visibility[]/AddressedTo[]`、`FAILiveCommitment` 三种状态 |
| `Public/Memory/AILiveBidTypes.h`   | `FAILiveBid`（Urgency/BidOffset/RuntimeAdj/FinalScore）、`FAILiveTickResolution`（WinnerActor/DerivedPublicSeq）                       |
| `Public/Memory/AILiveAgentTypes.h` | `FNPCAgentConfig` 三层：Core（id/faction/role）+ Identity（人设）+ Battle（策略）                                                      |
| `Public/LLM/AILiveAgentRoster.h`   | `ELLMProvider`（DeepSeek/GLM/Qwen3）+ `GetDefaultRoster()`                                                                             |

**自检**：

1. `Visibility=['public']` 和 `Visibility=['system']` 的事件，谁能在 prompt 里看到？
2. `EventType::SpeechIntended` 和 `SpeechPublic` 区别？为什么要分？
3. `FAILiveBid.RuntimeAdj` 是用来干嘛的？（关键词：antispotlighting decay）

不确定就回来问我，这几个概念是后面所有阶段的"砖"。

---

## 阶段 2 — 中央枢纽 API（45 min）

只看 `.h`，不进 `.cpp`。目标：建立"对外契约"心智模型。

**唯一文件**：`Public/Memory/AILiveEventStoreSubsystem.h`

按 5 组 API 分块阅读：

1. **生命周期**：`BeginGame(GameId)` / `ResumeFromGameId(GameId)` / `EndGame()`
2. **写入**：`AppendEvent(FAILiveEvent&)` / `AppendEventsAtomically(TArray&)` / `BeginTick(NewTick)`
3. **读 + 可见性**：`Quote*` / `ListMy*`（注意 viewer 参数——可见性门控在这里）
4. **投影**：`RebuildProjections()` 一键重算 4 个派生表（commitments / vote_history / alliance_state / agent_view_state）
5. **完整性 / 跨库**：`VerifyHashChain(OutFirstBadSeq)` / `TriggerDeleteExecuted(...)`

**自检**：

1. 为什么 `AppendEvent` 拿的是 **non-const ref**？（提示：seq/hash 是 EventStore 帮你填的）
2. `RebuildProjections` 是幂等的吗？什么时候会调？
3. `TriggerDeleteExecuted` 涉及两个不同的 SQLite 连接——哪两个？

---

## 阶段 3 — 主导演 API（30 min）

只看 `.h`。目标：理解状态机和编排者要的"外部输入"。

| 文件                                     | 焦点                                                                                                                                                                                  |
| ---------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `Public/Acts/Act02RuleReceiveDirector.h` | `EAct02State` 9 态、UPROPERTY 配置项（Roster / GameRuleRelativePath / LLMTimeoutSeconds / A2FProviderName / SpeechWatchdogSeconds / ReactionRoundCount）、`OnAct02Completed` delegate |
| `Public/Acts/Act01RuleIntroDirector.h`   | 同样的模式但状态更少（OpeningDoors → NPCsMovingToTV → PlayingVideo），用作对照看出"导演的共同套路"                                                                                    |

**自检**：Act02 的 `EAct02State::GatedAwaitNext` 在做什么？为什么要把"种子轮和反应轮之间"做成一个显式状态？

---

## 阶段 4 — 三个深度专题（约 3h，可分次进行）

每个专题独立。建议顺序 4a → 4b → 4c。

### 4a — 存储与哈希链（1h）

| 文件                                                            | 关键函数                                                                                                                                        |
| --------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------- |
| `Private/Memory/AILiveEventStoreSubsystem.cpp`                  | `BeginGame`、`AppendEvent`、`AppendEventsAtomically`、`ComputeEventHash`（≈ L1712）、`VerifyHashChain`（≈ L793）、`ResumeFromGameId`（≈ L3195） |
| `Public/Memory/AILiveSchemaCheck.h` + `AILiveSchemaMigration.h` | schema DDL 来自哪里                                                                                                                             |
| `Public/Util/AILiveJsonEscape.h` + `AILiveSha256.h`             | 哈希前 canonical JSON 序列化关键——任何字段顺序 / 转义不一致都会破链                                                                             |

**关键执行链路**：

```
AppendEvent(e)
  ↓ 加锁 WriteMutex
  ↓ 自动填 seq / event_id / tick_no(来自 CachedCurrentTickNo) / wall_clock
  ↓ canonical_json(e) → SHA256(prev_hash || canonical_json) → e.event_hash
  ↓ INSERT INTO events ...
  ↓ 更新 CachedLastSeq / CachedLastHash
```

**自检**：

1. 为什么 `tick_no` 不参与哈希链？（提示：T6 commit 信息）
2. Resume 协议的"未配对 inflight"是什么？怎么发现？怎么补偿？
3. 如果你想新增一种事件类型（比如 `EventType::AgentApologized`），需要改哪些地方？

### 4b — LLM 双管线（1h）

| 文件                                             | 焦点                                                                                                                                                                              |
| ------------------------------------------------ | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `Public/LLM/OpenAIChatClient.h` + `.cpp`         | `RequestBlocking(Req)`：同步 HTTP，OpenAI 兼容协议；多供应商共用                                                                                                                  |
| `Public/LLM/AILiveParserClient.h` + `.cpp`       | `ParseFourChannels(Req)`：固定 system prompt（来自 `Content/Prompts/Parser/v1.txt`）；输出 `FParseResult{Scratchpad, Intended, Bid, Note}`                                        |
| `Public/Memory/AILivePromptAssembler.h` + `.cpp` | 9 段提示装配（INVARIANT REMINDERS → CURRENT TICK → OUTPUT SCHEMA → YOUR OWN HISTORY → INTENDED-NOT-SAID → NOTES → COMMITMENTS → NEAR WINDOW → PRIVATE MSGS → CHALLENGE PREFETCH） |
| `Private/LLM/AILiveAgentRoster.cpp`              | `ResolveProviderEndpoint(ELLMProvider)`：从 `.env` 读 KEY/BASE/MODEL                                                                                                              |
| `Public/Util/ProjectEnvLoader.h`                 | `.env` 单例缓存；启动期一次性加载                                                                                                                                                 |
| `Content/Prompts/Parser/v1.txt`                  | Parser 系统提示词（不是 .cpp 但被运行时加载）                                                                                                                                     |

**关键执行链路**：

```
RunTick 内每个 NPC 一个 worker
  ↓ AssembleSystemPrompt + AssembleUserPrompt
  ↓ OpenAIChat::RequestBlocking → raw text（4 通道格式）
  ↓ AILiveParser::ParseFourChannels → FParseResult
  ↓ 失败重试最多 3 次，每次 45s 超时
  ↓ 仍失败 → bAbstain=true，上层写 system.parse_failed
```

**自检**：

1. Parser 是另一次 LLM 调用，还是纯 regex/JSON 解析？（看 `ParseFourChannels` 实现）
2. 为什么用 `RequestBlocking` 跑在 worker 线程而不是 UE 的 `FHttpModule` 异步？（提示：3 重试 × 45s 在 GT 上会卡 PIE）
3. `AssembleUserPrompt` 的 `NearWindowK=5` 是什么的窗口大小？

### 4c — 投影 / 登记 / Resume / Delete（1h）

| 文件                                                                                  | 关键函数                                                                                                                                                               |
| ------------------------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `Private/Memory/AILiveEventStoreSubsystem.cpp`                                        | `ProjectCommitments_LockHeld` (≈L2405) / `ProjectVoteHistory_LockHeld` (≈L2543) / `ProjectAllianceState_LockHeld` (≈L2592) / `ProjectAgentViewState_LockHeld` (≈L2741) |
| `Public/Memory/AILiveAgentRegistry.h` + `.cpp`                                        | `SyncRegistryFromLifecycle(MetaDb, LifecycleEventId)`——\_meta.db.agent_lifecycle_events → agent_registry.status                                                        |
| `Private/Memory/AILiveEventStoreSubsystem.cpp` 中的 `TriggerDeleteExecuted`（≈L3366） | 三步原子性：1) INSERT \_meta.db lifecycle row → 2) SyncRegistryFromLifecycle → 3) Append `system.delete_executed` 到 game.db                                           |
| `Public/Memory/AILiveListenerFilter.h`                                                | viewer 可见性门控的 SQL where 子句生成器                                                                                                                               |

**关键问题**：

- 4 个 reducer 都在**同一个 transaction + WriteMutex** 下执行，确保 agent_view_state 永远基于一致快照
- `_meta.db` 和 `game.db` 是两个 SQLite 连接，所以 `TriggerDeleteExecuted` 的 3 步**不是真正的原子**——在 (2) 和 (3) 之间崩溃会留悬空 lifecycle 行；MVP 不处理，但 Resume 应能识别（目前未实现）

**自检**：

1. `agent_view_state` 是每 tick 重算的，还是增量维护的？性能含义？
2. 如果 `commitments` 表里某行 `status='Active'`，但你直接看事件流找不到对应的 `commitment.promise` 事件，可能是什么原因？

---

## 阶段 5 — 主循环执行追踪（**核心**，2h）

读 `Private/Acts/Act02RuleReceiveDirector.cpp` **全文**，按以下顺序追踪一次完整的 tick：

```
按 [2] 键
  ↓ Tick() 检测 KeyPressed → BeginAct02()                 (≈L? Begin)
  ↓ StartPrescatter()  → EQS 散开 NPC
  ↓ 状态切到 SeedDispatch → RunTick()                     (L793)
       │
       ├─ EventStore.BeginTick(++tick) → 写 orchestrator.tick_anchor (L808)
       ├─ for each NPC in Roster (L822):
       │    ├─ BuildReasonerSystemPrompt()
       │    ├─ AILivePromptAssembler::AssembleUserPrompt()
       │    ├─ 写 system.llm_inflight 事件（in-flight 配对）
       │    └─ Async(ThreadPool, RunAgentTickInWorker) → TFuture (L883)
       ↓
  ↓ 状态切到 SeedAwait → 每帧 Tick() 走 TickLLMAwait()    (L953)
       │
       ├─ 200s GT 看门狗，超过强制收菜
       └─ 全部 Future 就绪 → GatherTickAndResolveFloor()   (L984)
            │
            ├─ Stage A (L999): 写 speech.intended/scratchpad/note + bid
            ├─ Stage B (L1115): EventStore::ResolveFloor()
            ├─ Stage C (L1143): 写 speech.public(winner) + tick_resolved (L1240)
            ├─ Stage D (L1158): Async RebuildProjections (4 reducers)
            └─ Stage E (L1172): StartSpeak(WinnerIdx, Text)
       ↓
  ↓ StartSpeak → MinimaxACELibrary::TriggerMinimaxSpeechFromPawnNative   (L1282)
  ↓ 状态切到 SeedSpeak → TickSpeakWatchdog (L1332)
  ↓ 语音播完 / 30s 超时 → 状态 GatedAwaitNext
       │
       ↓ 用户按 [3] 或自动推进 → StartReactionPhase()
            ↓ ReactionDispatch → ReactionAwait → ReactionSpeak → 循环 ReactionRoundCount 次
            ↓ AdvanceReactionRound → CompleteAct02() → OnAct02Completed.Broadcast()
```

**重点函数清单**（带行号一定要去对照）：

- `BeginPlay` —— BeginRoster bindings + 默认 NPCMoverClass 加载
- `RunTick` (L793)
- `RunAgentTickInWorker` (L890) ← **运行在 worker 线程，不要 touch UObject**
- `TickLLMAwait` (L953)
- `GatherTickAndResolveFloor` (L984) ← Stage A-E 五段
- `WriteTickResolvedAndAudit` (L1240)
- `StartSpeak` (L1282)
- `HandleSpeechFinished` / `TickSpeakWatchdog` (L1304-1332)
- `AdvanceReactionRound` (L1389 附近)

**自检**（最关键的一个）：能口头说出"按 2 键 → NPC1 嘴部动起来"这条链路上至少 8 个函数名 + 它们的归属类吗？卡住就回来对答案。

---

## 阶段 6 — Acts 对照与周边（45 min）

| 文件                                                                       | 焦点 / 用途                                                                                                                                                                     |
| -------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `Private/Acts/Act01RuleIntroDirector.cpp`                                  | 阶段 5 之后再回来读，会发现"线性版"和 Act02 共用大量 idiom：CacheInitialNPCTransforms / ResetToInitialPositions / RegisterDebugConsoleCommands                                  |
| `Private/Acts/StoryScenarioDirector.cpp`                                   | 独立小工具，两个 NPC 脚本化对话 + TTS。和 Act 体系不耦合                                                                                                                        |
| `Public/MinimaxACELibrary.h` + `.cpp`                                      | TTS（MiniMax t2a v2 HTTP）+ A2F（NVIDIA ACE local）。注意：必须在 ThreadPool 跑 RequestBlocking，回 GT 后挂 ACEAudioCurveSourceComponent。**这条链 Act02 不要重写——它已经能跑** |
| `Public/AILiveProjectScatterMover.h` + `.cpp`                              | EQS 散开。两个 Acts 都用                                                                                                                                                        |
| `Public/SightMemoryComponent.h` + `Public/AILiveProjectPerceptionLogger.h` | NPC 视觉感知 + 事件日志。和事件溯源系统**不是同一套**，是辅助调试                                                                                                               |
| `Public/Util/ProjectEnvLoader.h`                                           | `.env` 解析；只在测试用                                                                                                                                                         |

**自检**：Act01 和 Act02 各自有 `CacheInitialNPCTransforms`——为什么不抽到共同基类？（这是个**设计决策提问**——读完后回来跟我讨论）

---

## 关键文件路径速查

```
Source/AILiveProject/
├─ AILiveProject.Build.cs
├─ Public/
│  ├─ AILiveAgent.h                          ← 标记接口
│  ├─ MinimaxACELibrary.h                    ← TTS+A2F 玻璃层
│  ├─ Acts/
│  │  ├─ Act01RuleIntroDirector.h
│  │  ├─ Act02RuleReceiveDirector.h          ★ 核心
│  │  └─ StoryScenarioDirector.h
│  ├─ LLM/
│  │  ├─ OpenAIChatClient.h                  ★ Reasoner
│  │  ├─ AILiveParserClient.h                ★ Parser
│  │  └─ AILiveAgentRoster.h
│  ├─ Memory/
│  │  ├─ AILiveEventTypes.h                  ★★ 词汇表
│  │  ├─ AILiveBidTypes.h                    ★ 词汇表
│  │  ├─ AILiveAgentTypes.h                  ★ 词汇表
│  │  ├─ AILiveEventStoreSubsystem.h         ★★★ 中央枢纽
│  │  ├─ AILivePromptAssembler.h             ★ 9 段装配
│  │  ├─ AILiveAgentRegistry.h               ← Resume / Delete
│  │  ├─ AILiveListenerFilter.h              ← 可见性门控
│  │  ├─ AILiveSchemaCheck.h
│  │  └─ AILiveSchemaMigration.h
│  └─ Util/
│     ├─ ProjectEnvLoader.h
│     ├─ AILiveSha256.h
│     └─ AILiveJsonEscape.h
└─ Private/
   ├─ Acts/Act02RuleReceiveDirector.cpp      ★★★ 1466 行，主循环
   └─ Memory/AILiveEventStoreSubsystem.cpp   ★★★ ~3500 行，存储 + 投影 + Resume + Delete
```

---

## 验证（怎么算"读完"）

按下面 5 题自我考核。任何一题答不上来，回来贴给我。

1. **数据流题**：从"用户按 [2]"到"NPC1 嘴动起来播音频"这条链上，画出至少 8 个函数 + 它们的归属类，并标出哪几步发生在 worker 线程。
2. **不变量题**：哈希链的 canonical JSON 为什么把 `tick_no` 排除在外？如果加进去会怎样？
3. **可见性题**：一个 `Visibility=['NPC1','NPC3']` 的 `speech.intended` 事件，NPC2 的 prompt 里能看到吗？为什么？
4. **故障恢复题**：游戏运行到第 5 个 tick 时崩溃，重启后调用 `ResumeFromGameId(GameId)`——它会做什么？哪种损坏它**不能**修？
5. **修改任务题**：要新增一种 `EventType::AgentApologized`，至少要改哪 4 个地方？（提示：枚举、序列化、reducer、prompt）

---

## 我能帮你做什么（与清单配套）

随时回来贴：

- **文件:行号** → 我现场讲那段代码做什么、为什么这么写
- **一段你看不懂的 .cpp** → 我逐行翻译并指出隐藏假设
- **某个术语对不上** → 我从 PRD/DevLog 给你定位定义
- **想做的修改/扩展** → 我告诉你最少要动哪些文件、是否会破坏哈希链 / 投影一致性
- **自检题答案** → 你写出来，我对答 + 补漏

不必按顺序问；任何阶段卡住都直接来。
