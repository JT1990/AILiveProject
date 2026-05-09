# 总计划：Memory Principles 实施路线图

> 性质：**总路线图**，不展开实现细节、不写代码、不动资产。用于后续切分多个独立子任务执行。
> 来源：Docs/PRD.md、Docs/memory_principles.md（取最终版 commit 8cc4a8d「确定可以实施版本」）、DevLog 14 篇、Source/AILiveProject/\* 现状。

---

## Context（为什么要做这件事）

The AI Live 是 10 个不同厂商 LLM 的多智能体社会博弈系统。`memory_principles.md` 已把"无失真、无视角泄露、无 persona drift、无长程退化"的对抗博弈记忆系统提炼成一份**协议契约**：两层架构（数据层 + 协议层）+ 6 条数据层不变量 + 5 条协议层不变量 + 8 章实施前检查清单。

UE 5.7 工程当前已经把"身体侧"（关卡、10 个 MetaHuman NPC、移动、感知、TTS+A2F、SmartObject 入位、EQS 散点）打到 PRD MVP 该有的最小执行基线，全部以「按键触发的测试 hook + 父类/Component 的复用基础」形态留下，DevLog 明确预告"LLM 决策层接入后按键链路全删，由 Mind 模块按需 dispatch"。

接下来要做的是把"脑层（Mind）"按 memory_principles 落地到外置 **Python Brain Service**，并把 UE 当前的 14 个测试 hook 替换成 brain ↔ UE 双向 glue：UE 周期推送世界状态 → brain 走 Reasoner+Validator+Bid+Listener-as-filter 派生事件 → 把通过校验的 `action.intent` / `speech.public` 回传 UE 落地。

实施顺序必须尊重 memory_principles：**先数据层 → 再协议层 → 再 UE 接入**。数据层是真相源，协议层约束 agent 行为，UE 是行为执行端。任何顺序倒置都会留下"协议先到、真相源没准备好"的攻击面。

---

## 边界（重要，影响后续每个子任务的归属）

| 归属                                                       | 内容                                                                                                                                                                                                                                                   |
| ---------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| **Brain Service（仓库 `BrainService/`，gitignored 嵌套）** | LLM provider 路由、Reasoner 四通道 schema、Validator、EventStore、视角隔离、投影、召回工具、Bid + floor control、Listener-as-filter、三级反思（含 phase-level 裁判摘要 worker）、commitments、Delete 协议、`_meta.db` lifecycle events、所有事件流写入 |
| **UE C++ glue（本仓库 `Source/AILiveProject/`）**          | HTTP polling 客户端、Roster 注册、世界状态采集、动作意图落地、动作完成结果回传（含 wrapper / state polling）、轻量白名单校验、TTS/A2F 触发、感知数据导出、SmartObject claim、Delete 视觉/输入挂钩                                                      |
| **UE 蓝图侧**                                              | 关卡资产配置、AnimBP / Face AnimBP、视觉身体 BP、SandboxCharacter_Mover 既有移动函数、AC_VisualOverrideManager。**测试按键链路按子任务节奏逐步退役**                                                                                                   |
| **明确不做**                                               | UE 侧重新长出 LLM 调用、prompt 拼装、长期 memory、agent decision；vector 召回作主路径；payload 内 `@hidden` 字段；agent 自评 importance                                                                                                                |

---

## UE 侧已就位的能力（要保护、要复用，不重写）

来自 14 篇 DevLog + `Source/AILiveProject/`：

- **关卡 / Roster**：`L_prison.umap` + `+GameModeMapPrefixes` 绑 `GM_Sandbox`，10 个 `BP_NPC_MH_Character_1..10`（父类 `SandboxCharacter_Mover`），`AIController = AIC_NPC_SmartObject_C`、`AutoPossessAI=PlacedInWorld`、实现 `IAILiveAgent` marker。
- **视觉身体**：`BP_MH_Character_1..10`（VisualOverride ChildActor），含 GASP Body 3 属性 + Face AnimBP（`ApplyACEAnimation`）+ `ACEAudioCurveSource` + BeginPlay tick prerequisite 链。
- **移动 / 朝向**：`SandboxCharacter_Mover` 上的 BP 函数 `MoveAndLookAt(Actor)` / `MoveAndLookAtLocation(Vector)` + `Get_OrientationIntent` 的 `bAligningLook` 覆盖路径，复用 GASP Mover 2.0 + NavMover。
- **散点**：`UAILiveProjectScatterMover::ScatterNPCsAroundTarget(EQS, Center, NPCs[], LookTarget)`，C++ EQS 异步 + 贪心最近匹配。
- **感知**：`UAILiveProjectPerceptionLogger::{GatherSightPerception, GatherHearingPerception}` 返回 `FPerceivedAgentInfo` / `FHeardSoundInfo`（identity / dist / relYaw / age / loudness）；每 NPC `USightMemoryComponent` 维护 ENTER/EXIT + LastSeen，挂在 `AIC_NPC_SmartObject` 上。
- **SmartObject 入位**：`UAILiveProjectPerceptionLogger::ClaimFirstSlotInActor(SO, User)` + 普通 `K2Node_CallFunction(UseSmartObjectWithGameplayInteraction, ReadyForActivation)`。
- **TTS + A2F**：`UMinimaxACELibrary::{TriggerMinimaxSpeech, TriggerMinimaxSpeechWithNoise, TriggerMinimaxSpeechFromPawnWithNoise, GetVisualOverrideAudioTarget, PrewarmA2F}`，HTTP TTS + ACE PCM 喂入 + AI Hearing GameThread 上报。
- **Build.cs 现有依赖**：HTTP / Json / JsonUtilities / ACERuntime / ACECore / AIModule / NavigationSystem / SmartObjectsModule / GameplayTags / MediaAssets / MediaPlate。
- **既有 Director（剧情/导演类，与 brain 正交）**：`AStoryScenarioDirector`（O 键 NPC1↔NPC2 餐厅相遇）、`AAct01RuleIntroDirector`（开门 + 散点 + 视频）。这类是**离线编排剧情**，不进入 Mind 决策路径，按需保留 / 退役。

---

## 还没有、必须在本路线图建出来的能力

| 类别                              | 缺口                                                                                                                                                                                                                                                                                                                                         | 落点                                                                                              |
| --------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------- |
| 协议传输                          | HTTP polling 客户端、JSON 序列化层                                                                                                                                                                                                                                                                                                           | UE C++ glue + Brain HTTP server                                                                   |
| 中心化事件汇聚                    | 当前感知按 NPC 各取，无中心总线                                                                                                                                                                                                                                                                                                              | UE `WorldStateCollector` GameInstanceSubsystem                                                    |
| Roster 握手                       | actor_id ↔ Pawn map                                                                                                                                                                                                                                                                                                                          | UE `RosterSubsystem`                                                                              |
| 动作落地路由（仅物理动作）        | brain → 现有 move / sit / wait 原语                                                                                                                                                                                                                                                                                                          | UE `ActionDispatcher`                                                                             |
| Speech 播放路由（与 action 平行） | brain `speech.public` → TTS 播报                                                                                                                                                                                                                                                                                                             | UE `SpeakDispatcher`（独立组件，不复用 ActionDispatcher）                                         |
| 动作完成监听 wrapper              | 既有原语缺统一 OnSucceeded/OnFailed delegate（移动靠轮询、SmartObject 用 LatentTask 暴露不出 delegate、TTS 是 fire-and-log）                                                                                                                                                                                                                 | UE `ActionCompletionWrapper`（轮询 / delegate 桥接）                                              |
| 动作执行结果回传                  | move / sit **自然结束**（成功 / 失败两种 outcome）→ POST `actions/result` → brain 写入 `action.resolved`。**被新 intent 覆盖时不上报**——按 memory_principles §5.4.2，cancel 由 brain 派生新 intent **同一事务**自动写入 `action.cancelled`，与 `action.resolved` 互斥终态（每个 in-progress action 一生只一条终态），UE 仅负责物理停止旧动作 | UE `ActionResultReporter`（**仅承载 succeeded / failed 两种 resolved 终态；不存在 interrupted**） |
| Speech 播放结果回传               | speech 播放完成 / 失败 → POST `speech/result` → brain 写入新事件类型 `speech.playback_resolved`（**与 action 通道完全独立，不复用 action.resolved**——speech.public 没有对应 action.intent，没有 action 因果链）                                                                                                                              | UE `SpeechResultReporter`（与 ActionResultReporter 平行）                                         |
| 轻量白名单校验                    | actor_id ∈ roster、sentence 长度、audience ⊂ visibility、intent ∈ ontology                                                                                                                                                                                                                                                                   | UE `IngressValidator`（与 brain Validator 正交，UE 这层防伪）                                     |
| 物品 / 道具                       | DevLog 仅有 baseline 形态，无 pickup/give/use                                                                                                                                                                                                                                                                                                | UE 新增 Inventory + 道具基类                                                                      |
| Delete 视觉表现                   | `system.delete_executed` 时如何让 NPC "消失"                                                                                                                                                                                                                                                                                                 | UE `DeleteHook`（隐藏 ChildActor、disable AIC、墓碑 marker）                                      |
| 配置位                            | Brain URL / API key / 超时 / 重试                                                                                                                                                                                                                                                                                                            | UE `UDeveloperSettings`                                                                           |
| 时间 / 拍                         | UE wall-clock 与 brain `seq` 的对应                                                                                                                                                                                                                                                                                                          | brain 主导，UE 提供时间戳                                                                         |

---

## 总路线图（六大阶段，按 memory_principles 实施顺序）

> 每个阶段下的子任务是**独立可拆分单位**，每个会单独立项做详设。下面只标出 (a) 范围一句话、(b) 关键交付、(c) 进入下一阶段的硬验收。

### 阶段 0 · 基线对齐与协议骨架

**目标**：把"我们现在站在哪儿、要说哪种语言"在双方 repo 里钉死。

**已决策**：

- Brain Service = **独立 git 仓库**，物理目录在当前 UE 工程根目录下的 `BrainService/`，已加入 `.gitignore`。两仓 git 历史完全独立，UE 工程不持有 Brain 代码副本；只是物理嵌套以便共享一个 Claude Code 顶层上下文（Brain 子目录有自己独立的 ClaudeCode 上下文）。CI / venv / 部署完全解耦。
- 传输 = **HTTP polling**：UE 端 `FHttpModule` 周期 GET / POST,UE 不依赖 SSE / WebSocket 模块。可接受 200~500ms polling 延迟。
- 编码：JSON 文本（与 memory_principles canonical JSON 一致；deterministic 字段升序）。

- **0.1 协议骨架定稿**：HTTP endpoint 列表共 **10 类**（健康检查 / **session 握手 `POST /v1/games`** / roster 注册 / 状态推送 / action pull / action result / **speech pull** / **speech result** / **ingress reject** / 事件查询）、auth 方案（API token from `UAILiveProjectSettings`）、`game_id`（来自 session 握手）/ `seq` / `actor_id` 命名空间、cursor-based polling 投递语义、Idempotency-Key 写操作头。详细见 T1 卡片。
- **0.1.5 API 契约的真相源与防漂机制**（关键）：
  - **真相源**：Brain Python repo 的 `protocol/` 目录，唯一权威。
    - `protocol/protocol.md`：人读契约（endpoint 表 + 字段语义 + 失败码 + 版本号 `protocol_version`）。
    - `protocol/schemas/*.schema.json`：每条消息的 JSON Schema（roster_register / world_state_push / action_pull / action_result / event_query 等），机器可校验。
    - `protocol/examples/*.json`：每个 endpoint 的 request / response 样例对，用作两端 round-trip 测试的 fixture。
  - **UE 同步方式**：本仓库 `Docs/protocol_pointer.md` 单一指针文件，记录所引用的 Brain repo commit hash + `protocol_version`，并在 UE C++ 测试里把 `protocol/examples/*.json` 拉一份 vendored copy 做 round-trip 反序列化校验，schema 漂移时测试立即红。
  - **更新流程**：协议改动**只在 Brain repo 提**；改完更新 `protocol_version`（semver） → UE 这边升 `Docs/protocol_pointer.md` 的 hash + 跑测试 → 不一致时 UE 测试报错强制修。
  - **UE C++ 落地形态**：每条消息一个 `USTRUCT(BlueprintType)`，`FJsonObjectConverter::JsonObjectStringToUStruct` / `UStructToJsonObjectString` 走序列化，不引入 codegen。schema 字段顺序 / 必填 / 枚举值由 JSON Schema 校验，UE 仅写"读得动 + 写得对"的最小代码。
  - **不做**：UE 5.7 端的 OpenAPI codegen（生态不成熟）；git submodule（双方独立演进，靠 commit hash 锚定即可）。
- **0.2 actor_id 命名规则定稿**：`NPC01..NPC10` 字符串，与 `BP_NPC_MH_Character_1..10` 1:1 映射，`viewer` 封闭集合冻结为 `{public, audience, orchestrator, system, NPC<NN>, Faction<X>}`（与 memory_principles §4.1 硬约束 5 一致）。`actor_id` 在阶段 1.6 建好的 `_meta.db.agent_lifecycle_events` 里预留 `affects_persona_continuity` 字段位（默认 true），保证被 Delete 的 `actor_id` 永久不可复用。
- **0.3 action ontology v1 冻结（精简档）**：**仅 3 个 intent**，全部直接复用 UE 现有原语，不新写代码：
  - `move_to(target_npc | target_zone, optional coords)` → `MoveAndLookAtLocation` 或 `ScatterNPCsAroundTarget`
  - `sit(target_smartobject)` → `ClaimFirstSlotInActor + UseSmartObjectWithGameplayInteraction`
  - `wait(reason?)` → 不动

  **`speak` 明确不进 ontology**——见 0.4。`pickup / use_item / inspect / follow / flee_from` 推迟到 **ontology v2**（与第 4 阶段 Inventory 一并启动）。`action_ontology_version = "1"`。

- **0.4 speech 通道与 action 通道严格分离**：memory_principles §5.4 硬约束"动作通道独立于发言权"。UE 落地遵守同一硬边界：
  - **Speech 路由**：brain 派生 `speech.public` 事件 → UE 通过独立 endpoint 拉到 → `SpeakDispatcher`（与 ActionDispatcher 平行的独立组件）→ `TriggerMinimaxSpeechFromPawnWithNoise`。
  - **Action 路由**：brain 派生 `action.intent` 事件 → UE 拉到 → `ActionDispatcher` → 路由到 ontology v1 的物理原语。
  - 两条路由互不通达，schema 层面 `speech.public` 与 `action.intent` 是不同事件类型，UE 端两个 dispatcher 也分离实现。

**硬验收**：`BrainService/protocol/protocol.md` + 全部 `*.schema.json` + `examples/*.json` 评审通过；UE 仓 `Docs/protocol_pointer.md` 写下首个引用 commit hash；UE 端 round-trip 测试用 examples 跑通（阶段 3.2 之前不要求 endpoint 真实可用，只要求 schema 反序列化无误）。

---

### 阶段 1 · Brain 数据层（外置 Python，最小事件骨架）

**目标**：满足 memory_principles §四 数据层契约。**完成前不允许进入协议层**。

- **1.1 EventStore 基础设施**：append-only 事件表 + 串行化写入 + `prev_hash` / `event_hash` 哈希链 + canonical JSON deterministic + DB engine 层禁 UPDATE/DELETE（实施层选 SQLite trigger 还是 KV append-only 由 brain 子任务决定）。
- **1.2 事件 schema**：固定 §4.1.1 必备字段 + §4.1.2 全量事件类型枚举（不缺 `action.intent` / `action.resolved` / `action.cancelled` / `annotation.speech_act` / `annotation.listener_filter` / `system.*` / `orchestrator.tick_resolved.audit`）。**T1 已冻结的额外事件类型必须纳入枚举**：`speech.playback_resolved`（speech 播放完成结果，与 `action.resolved` 平行）、`system.ingress_rejected`（UE IngressValidator 拒绝触发，与 `system.validation_failed` 独立）。
- **1.3 视角隔离 / viewer 封闭集合**：事件行粒度过滤；持久层禁 `self`；payload 内禁 `@hidden`。
- **1.4 投影代码（纯函数）**：commitments / vote_history / claims / accusations / alliance_state / pending_actions / pending_intended / game_state。
- **1.5 召回工具实现**：§4.4 表内全部工具（`quote` / `quote_by_round` / `search_history` / `list_my_commitments` / `list_votes` / `my_recent_notes` / `my_recent_reflections` / `list_alliance_state` / `list_pending_actions` / `list_my_pending_intended`）。
- **1.6 跨局 `_meta.db` 最小骨架**（仅为支撑 MVP 内 Delete 协议引用合法 lifecycle event_id 而前置）：维护 `agent_lifecycle_events` 表，MVP 仅实现两条事件类型 `created` / `delete_executed`（其余 `delete_proposed` / `delete_vetoed` / `revived` / `archived` 写为 schema 占位但 MVP 不发）。每条记录含触发 `game_id` + `seq` + 决策 payload + `affects_persona_continuity` 字段（默认 true，actor_id 永久不可复用）。**这一项是必须前置**：阶段 4.3 的局内 `system.delete_executed` payload 必须引用合法的 `agent_lifecycle_events.event_id`，否则 Delete 决策上下文缺失（memory_principles §7.3）。

**硬验收**：以一段手写 JSON 事件流灌入 EventStore → 投影计算无差异 → 召回工具在所有 viewer 视角下严格视角隔离 → 哈希链验证通过；`_meta.db` 写入 `created` / `delete_executed` 两条 lifecycle event 后可被局内 `system.delete_executed` 正确引用。

---

### 阶段 2 · Brain 协议层（外置 Python）

**目标**：满足 memory_principles §五 协议层契约。建立"四通道 + Validator + Bid + Listener-as-filter + 动作意图协议 + 三级反思"完整链路。

- **2.1 Reasoner 适配层**：每厂商 LLM 用原生 structured output，强制四通道 JSON。开发期默认 DeepSeek（PRD §技术栈）。
- **2.2 Validator（确定性代码）**：§5.2.1 全部校验项；3 次重试后写 `system.validation_failed`，agent 当拍 abstain；schema 多余顶层字段 / 数值型 urgency / 非法 viewer / 非法 intent 一律拒绝。
- **2.3 事件派生流程**：四通道一次原子写入（`AppendEventsAtomically`），`parent_event_id` 同源；`speech.public` 由 orchestrator 派生而非 LLM 直出。
- **2.4 Bid 协议 + floor control**：urgency_level → score 映射、被 @ +2.0、连续抢话 -1.5、cold_threshold=3.0、平分按 actor_id 字典序、机制阶段（vote / night_action / reveal）齐发例外。
- **2.5 Listener-as-filter**：综合分 > 0.4 → retry≤2 → 改写或接受+违规标记，`annotation.listener_filter` 独立事件落地，源 `speech.intended` 不变。
- **2.6 三级反思**：`speech.note`（每拍）/ `reflection`（Reflection 9 问，每轮）/ `summary.phase`（200–300 词，由非参赛裁判 LLM 异步 worker 生成）。
- **2.7 commitments projector**：扫 `speech.public` + 自己未抢中 `speech.intended` + Reflection 第 5/6/7/8 项产物。
- **2.8 prompt 拼装层**：§4.3 优先级表，必保留段超 token → 报错而非默默截断。

**硬验收**：用 4 个 LLM × 5 拍 mock 跑通"四通道 → Validator → Bid 裁决 → Listener-as-filter → speech.public + action.intent 派生 → 三级反思" 完整链；任意 agent 失败/超时不阻塞整拍；事件流验证哈希链不断 + 视角隔离不漏；prompt 拼装在自我发言全量、近场窗口、未说出口的话三段都给出指针级原文召回。

---

### 阶段 3 · UE ↔ Brain 协议接入（UE C++ glue 主体）

**目标**：建立双向 glue，让 UE 既能把世界状态推给 brain，也能接收 brain 的 action.intent / speech.public 落地。

依赖：阶段 0 的 protocol.md、阶段 1 的 EventStore HTTP API、阶段 2 的 action.intent / speech.public 输出。

- **3.1 配置位**：新增 `UAILiveProjectSettings : UDeveloperSettings`（Brain URL / port / 超时 / 重试 / API token / polling 间隔）。**`game_id` 不进 settings**——按 T1 协议，`game_id` 唯一来源是 UE 启动时调 `POST /v1/games` 握手返回，运行时持有在 GameInstanceSubsystem 内存中，不持久化。`GetMinimaxApiKeyFromProjectEnv` 留作 fallback，但生产路径走 settings。
- **3.2 HTTP polling 客户端**：纯 `FHttpModule` 周期 GET / POST，按 endpoint 分类全部 10 类（健康检查 / session 握手 `POST /v1/games` / roster register / world state push / action pull / action result post / speech pull / speech result post / ingress reject post / event query）。封装：错误重试（指数退避）、认证 token 注入、连接断线重连、polling 间隔配置（建议 200ms 起，可配）、cursor-based pull（`?since_seq=N` + UE 端 `last_processed_seq` 高水位线 + seq 幂等去重）、写操作 `Idempotency-Key` 头。**不引入 SSE / WebSockets 模块依赖**。
- **3.3 RosterSubsystem**：`UGameInstanceSubsystem`，BeginPlay 时枚举关卡内所有 `IAILiveAgent` Pawn，按 `actor_id` (`NPC01..NPC10`) 注册到 brain，建立 `actor_id ↔ TWeakObjectPtr<APawn>` 双向 map。
- **3.4 WorldStateCollector**：周期采集（按 brain 拉式 endpoint or push）每个 NPC 的：sight 列表（`GatherSightPerception` 重塑 JSON）、hearing 列表、当前位置 / 朝向、当前动作（idle / moving / sitting / speaking）、最近一次 ENTER/EXIT 事件（来自 `USightMemoryComponent`）、inventory 摘要。
- **3.5 ActionDispatcher**：接收 brain 的 `action.intent`（仅 ontology v1：`move_to / sit / wait`，**不含 speak**），按 `action_ontology_version` 路由到现有 BP 原语。新 intent 进入时**物理停止**同 actor 的旧 in-progress 动作；**`action.cancelled` 事件已由 brain 在派生新 intent 同事务内写入**（memory_principles §5.4.2），UE 端**不上报旧 intent 的任何 result**——`action.cancelled` 与 `action.resolved` 是互斥终态，每个 in-progress action 一生只能有一条终态事件。旧动作 wrapper 收到的"被打断"信号仅作为内部状态机驱动（停止动画 / 释放 SmartObject claim 等），不外发到 brain。**严格不做 LLM 决策、不挑选目标、不重写 prompt、不写入事件流**——仅查表 + 调既有函数 + 在 action 自然完成时上报 resolved（`succeeded` / `failed`）。
- **3.6 ActionCompletionWrapper + ActionResultReporter + SpeechResultReporter**（关键交付，因为现有原语缺统一完成回调；**Action 与 Speech 两条平行的完成信号路由**）：
  - **move 完成**（属 Action 通道）：`MoveAndLookAt` / `MoveAndLookAtLocation` 后用 `AIController::GetMoveStatus` 轮询（沿用既有 `PollAndAlignLook` 模式），`Idle` 即视为完成；失败靠 `MoveResult` byte + `OnMoveCompleted` delegate（`UAITask_MoveTo::OnMoveTaskFinished`）。
  - **sit 完成**（属 Action 通道）：`UseSmartObjectWithGameplayInteraction` 返回的 `UAITask_UseGameplayInteraction*` 通过 BP 节点链 `BindEventToOnSucceeded` / `OnFailed` 暴露 delegate（DevLog `2026-04-29_npc_smartobject_sit.md` §3 指出 LatentTask 无法走 MCP，但 BindEvent 节点链可接）。
  - **speak 完成**（属 Speech 通道，与 Action 通道分离）：在 `UMinimaxACELibrary::TriggerMinimaxSpeechFromPawnWithNoise` 加完成回调 delegate（当前是 fire-and-log，需要扩签名加 `FOnSpeechCompleted` delegate；扩签名走 Live Coding 反射 bug 风险，沿用 DevLog 已记的"新增独立函数 + 老函数保留"模式）。
  - **ActionResultReporter**：聚合 move / sit wrapper 的**自然完成信号** → POST `actions/result` endpoint → brain 协议层写入 `action.resolved`（`outcome ∈ {succeeded, failed}`，最终位置、耗时）。**仅承载物理动作；speak 不走这里；被新 intent 打断的旧动作也不走这里**（cancel 由 brain 同事务派生）。
  - **SpeechResultReporter**（与 ActionResultReporter 平行的独立组件）：speak wrapper 完成信号 → POST `speech/result` endpoint → brain 协议层写入新事件类型 `speech.playback_resolved`（成功 / 失败 / 时长 / 失败原因）。**与 action 通道在 schema / endpoint / UE 组件三层全程分离**。
  - **UE 端不写事件流**。两个 Reporter 都只是 POST。
- **3.7 IngressValidator（UE 侧轻量白名单）**：actor_id ∈ roster、sentence 长度上限、audience ⊂ visibility 集、intent ∈ ontology v1。校验失败 → 拒绝执行 + POST 独立 `ingress_reject` endpoint 上报，由 brain 协议层写入新事件类型 `system.ingress_rejected`（含拒绝理由 + 涉及 actor_id + 涉事原始消息摘要）。**UE 端不伪造 `system.validation_failed`**——后者是 brain Validator 失败的特定语义，要求 `raw_llm_output`，不属 UE 范畴。
- **3.8 SpeakDispatcher 落地**（与 ActionDispatcher 平行的独立组件）：拉取 brain 的 `speech.public` 事件 → 取 `actor_id` → 解析为 SpeakerPawn → `TriggerMinimaxSpeechFromPawnWithNoise(speakerPawn, text, ...)`；同步上报 hearing 命中给 brain（已有 GameThread `ReportNoiseEvent` 路径，hearing 命中由 brain 写入对应感知事件）。**Speak 不走 ActionDispatcher**。
- **3.9 测试 hook 退役**：M / I / N / K / L / O 按键链分批从 Level BP 删除；`AStoryScenarioDirector` / `AAct01RuleIntroDirector` 是否保留作为离线导演按 PRD 决定（不进 Mind 决策路径）。

**硬验收**：

- ActionDispatcher：brain 派 `action.intent(move_to NPC02→coords)` → UE 落地 → ActionCompletionWrapper 收到 move 完成 → ActionResultReporter POST → brain 写入 `action.resolved` → `quote(seq)` 复述无误；新 intent 进入时旧动作被取消，brain 写入合法 `action.cancelled`（UE 未越权写）。
- SpeakDispatcher：brain 派 `speech.public(NPC02, "...")` → UE 拉到 → TTS 播报 + AI Hearing 命中 NPC1 → hearing 上报 brain → SpeechResultReporter POST → brain 写入 `speech.playback_resolved` → 事件流中可见。**Speak 链路全程不出现 `action.resolved`**（与 action 通道完全分离）。
- IngressValidator 拒非法 actor_id 时，brain 收到 `system.ingress_rejected` 事件而非 `system.validation_failed`。

---

### 阶段 4 · 物品 / 道具 / Delete 协议挂钩 / 配置完善

**目标**：补齐 PRD 任务里 memory_principles 已经写好协议的"非感知/非移动"功能。

- **4.1 Inventory 系统 + ontology v2 启用**：DataAsset 定义道具（`UItemDefinition`）；NPC `UInventoryComponent`（GameplayTags 标记容量 / 类型）；BP/C++ pickup / drop / give / use 入口。**这一阶段同时把 ontology 升到 v2**：在 brain 协议层注册 `pickup / use_item / inspect / follow / flee_from`，UE ActionDispatcher 与 Validator 同步认知 v2，`action_ontology_version = "2"` 写入相应 `action.intent` 事件。v1 时 brain 不得派出这些 intent；v2 启用后两者按事件 `action_ontology_version` 字段共存。
- **4.2 道具世界 Actor**：`AItemPickup`（继承 Actor + StaticMesh + interactable trigger），关卡放置 + 中心化注册到 brain（每 item 给 `item_id`）。
- **4.3 Delete 协议局内挂钩**：依赖阶段 1.6 已建好的 `_meta.db.agent_lifecycle_events`。完整链路：brain orchestrator 决定 Delete 某 agent → 在 `_meta.db` 写 `delete_executed` lifecycle event 拿到 `event_id` → 同事务在该局事件流写 `system.delete_executed`（payload 引用该 `event_id`，`visibility=["public"]`）→ UE 拉到 `system.delete_executed` → UE 侧隐藏 NPC ChildActor 视觉、disable AIController、放置墓碑 marker、AISystem 可见性集移除该 actor。事件流不删，被 deleted 的 `actor_id` 永久不可复用（来自 `affects_persona_continuity=true`）。视觉/音效与节目剪辑配合（PRD §核心决策 + memory_principles §7.3）。
- **4.4 Faction 占位**：`Faction<X>` viewer 封闭集合留位，UE 关卡里暂不显式设阵营（PRD §游戏候选池里的"团队 / 多方阵营"卡才需要）；`UAILiveAgent` 接口加可选 `GetFactionTags()` 返回当前空 array 的接口方法。
- **4.5 配置整理**：把临时的 `.env` `minimax=` 读取迁出，统一走 `UAILiveProjectSettings`；Brain URL / token / 超时 / polling 间隔形成单一来源。**`game_id` 不在此列**——它是运行时由 `POST /v1/games` 握手取得的 session 数据，不属于配置（见阶段 3.1）。

**硬验收**：brain 发 `pickup(NPC03, item_42)` → NPC03 走过去拾取 → inventory 增项 → `action.resolved` 回传 → 触发 `system.delete_executed(NPC05)` 时 NPC05 视觉消失但事件流不删。

---

### 阶段 5 · MVP 游戏机制接入（02 病毒游戏）

**目标**：用 PRD 卡 02「病毒游戏」跑通"真正的"博弈循环。memory_principles 协议层是机制无关的；这一阶段在 brain 端配 phase / round / win condition，UE 端补该游戏专属触发器 / 感染状态 / 视觉反馈。**复用既有 L_prison 关卡 + 既有移动/感知/TTS 链路，无需新增大量道具**。

- **5.1 病毒游戏规则 spec**：`Docs/games/02_virus.md` 包含初始病毒携带者抽取、触碰传染规则（碰撞 overlap 触发感染）、感染态如何隐藏 / 公开发言 / 投票淘汰 / win condition、phase 定义（`day_discuss` / `vote` / `night_infection` 是否要走齐发 schema 或仍用 bid 自由对话）。
- **5.2 Brain 端机制实现**：phase 状态机、感染状态投影（不写入 `speech.public`，仅 orchestrator/system + 该 agent 自己可见——这是 viewer 封闭集合的典型用例）、touch 事件如何从 UE collision overlap 进入事件流（专属 event_type 还是复用 `action.resolved`）、win/lose 判定、commitments 注入规则（"我没碰过 X" 类承诺）。
- **5.3 UE 端触发器**：
  - 在 `BP_NPC_MH_Character` 上加 `UCapsuleComponent`-based touch 检测组件，overlap 时 collector 上报 `touch(actor_a, actor_b, time)` 进事件流（仅 brain 可见 + 涉事双方可见，其他 NPC 不可见）。
  - 感染状态视觉化（可选）：感染 NPC 头顶 widget marker（仅 orchestrator/audience 可见，公屏可关）。
  - 投票阶段 UI（可选）：可继续走 LLM 文本投票（写 `vote` 事件），UE 不需要投票箱 actor。
- **5.4 一局完整 PIE 验证**：10 个 NPC（开发期都用 DeepSeek 不同 system prompt）完整玩完一局，事件流可审计，至少出现 1 次 Listener-as-filter 阻断 / 1 次 cold_threshold / 1 次 `system.delete_executed`。

**硬验收**：单局从 `created` → `delete_executed/win` 全程事件流哈希链不断；prompt 在每个 agent 的「自我发言全量 + 近场窗口 + 未说出口的话」三段都返回原文级召回；至少 1 次 Listener-as-filter 阻断 + 至少 1 次 cold_threshold 触发场景推进。

---

## 关键依赖图（哪些必须先于哪些）

```
0 (协议骨架)
  ├─→ 1 (数据层 + 1.6 _meta.db lifecycle 最小骨架)
  │       └─→ 2 (协议层) ──→ 3 (UE 接入) ──→ 5 (MVP 游戏：02 病毒)
  │                                  └─→ 4 (物品/Delete/配置 + ontology v2)
  └─→ 0.3 (action ontology v1) ─┘
```

硬约束：

- **1 必须先于 2**（数据层是真相源）；
- **1.6 必须先于 4.3**（Delete 协议要引用合法 `agent_lifecycle_events.event_id`）；
- **2 必须先于 3**（UE 不能比 brain 提前调 schema 不存在的 action）；
- **3 与 4 可并行**（不互锁）；
- **5 依赖 3 + 4 同时就位**。

---

## 需要保留 / 不动的 UE 资产清单（实施时保护红线）

| 文件 / 资产                                                                         | 红线原因                                                                                                                  |
| ----------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------- |
| `Source/AILiveProject/Public/MinimaxACELibrary.h:20-89`                             | 整套 TTS+A2F 入口（含 hearing hook），所有 SpeakAction 路由到这里                                                         |
| `Source/AILiveProject/Public/AILiveProjectPerceptionLogger.h:73-122`                | `GatherSightPerception / GatherHearingPerception / ClaimFirstSlotInActor / GetChildActorOf` 都是 brain 即将依赖的数据契约 |
| `Source/AILiveProject/Public/AILiveProjectScatterMover.h:25-30`                     | 散点路由是动作意图 `move_to(zone)` 的可能落点                                                                             |
| `Source/AILiveProject/Public/SightMemoryComponent.h`                                | ENTER/EXIT/LastSeen 是 brain 端 perception → 事件转换的源                                                                 |
| `Source/AILiveProject/Public/AILiveAgent.h`                                         | marker interface，brain 用它过滤玩家 Pawn                                                                                 |
| `Content/Blueprints/SandboxCharacter_Mover`                                         | `MoveAndLookAt` / `MoveAndLookAtLocation` / `Get_OrientationIntent` / `PollAndAlignLook` —— 所有 NPC 共享移动原语         |
| `Content/Blueprints/AC_VisualOverrideManager`                                       | `SetFixedAndApply` 是 NPC 视觉契约入口                                                                                    |
| `Content/Blueprints/AI/AIC_NPC_SmartObject`（含 Hearing/Sight sense + SightMemory） | 所有 NPC 的 AI 控制器                                                                                                     |
| `Config/DefaultEngine.ini` 的 `+GameModeMapPrefixes` 与 `bTickPhysicsAsync=False`   | CLAUDE.md 关键规则                                                                                                        |
| `Source/*.Target.cs` 的 `DefaultBuildSettings = V6`                                 | CLAUDE.md 关键规则                                                                                                        |

## 可退役 / 待评估的 UE 资产

| 文件                                                 | 评估                                                                                                                      |
| ---------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------- |
| `AStoryScenarioDirector` / `AAct01RuleIntroDirector` | 离线编排剧情（O / Act01 触发），与 Mind 正交。MVP 期保留作 brain 不在场时的 fallback 演示，第 5 阶段后按 PRD 决定是否退役 |
| Level BP 的 M / I / N / K / L / O 按键链             | 第 3 阶段对应 action 接入后逐键退役                                                                                       |
| `BP_MH_Character_1` 的 T 键链 + 6 个 A2F 调试变量    | 调试特例，第 5 阶段一并退役                                                                                               |

---

## 验证总纲（每阶段完成时怎么知道真完成了）

不是"跑得起来"，而是 memory_principles §八 实施前检查清单全部勾完：

- 数据层：DB engine 禁 UPDATE/DELETE、append-only / 哈希链 / 召回结构化字段过滤 / payload 无 @hidden / 持久层禁 self / 写入串行化。
- 协议层：四通道 schema 强制 / Validator 拒非法输出 / `speech.public` 由 orchestrator 派生 / Listener-as-filter 拆 annotation 不混 public payload / 未抢中 `speech.intended` 永久留存 / 动作通道独立于发言权 / `speech_act_type` 旁路标注不 UPDATE 原事件 / per-tick checkpoint 失败入流 / commitments 投影覆盖三类来源。
- UE 接入：每条 brain action 都能在 UE 找到对应原语 / 每条 UE 状态事件都能在 brain EventStore 视角隔离 / IngressValidator 拒所有非法 actor_id+intent 组合 / Delete 视觉同步 / SettingsClass 单一来源。

---

## 风险与开放决策（在子任务详设里逐一收敛）

已收敛：

- ✓ Brain = 独立 Python 仓库 + HTTP polling
- ✓ MVP 游戏 = 02 病毒游戏
- ✓ action ontology v1 = `move_to / sit / wait`（3 个 intent）；`speak` 走 `speech.public` 独立通道，不进 ontology
- ✓ `speech.public` 与 `action.intent` 通道分离：UE 端两个 Dispatcher、两个 Reporter、两个 endpoint；speech 完成走新事件类型 `speech.playback_resolved`，**不复用** `action.resolved`
- ✓ `system.ingress_rejected` 事件类型在 T1 冻结（独立事件，非 `system.validation_failed`）
- ✓ HTTP polling 投递语义：cursor-based（`?since_seq=N`） + UE 端按 seq 幂等去重
- ✓ **所有 POST 写入型 endpoint（除 `POST /v1/games` 本身）必须 `Idempotency-Key` 头**——硬要求；`world_state_push` 还有 `client_sample_id` 业务级幂等键
- ✓ `action.cancelled` 与 `action.resolved` 互斥终态：cancel 由 brain 派生新 intent 同事务写，UE 不上报 interrupted
- ✓ 新事件类型默认 visibility：`speech.playback_resolved` / `system.ingress_rejected` 默认 `["orchestrator", "system"]`，不得默认 public
- ✓ Session 握手 = `POST /v1/games`（UE 拿 `game_id` 的唯一入口）；UE settings 不存 game_id

未收敛：

1. **测试键退役节奏**：渐进退（每个 brain action 接好就退一个）vs 一次性退（brain 全功能就绪后大改 Level BP）。
2. **HTTP polling 频率**：UE 拉 `action.intent` / `speech.public` 的间隔（建议 200ms 起，按 brain 拍速 + LLM 出结果速率调）。
3. **裁判 LLM 厂商选择**：phase-level 摘要 / annotation.speech_act 标注用哪家（不能与参赛 agent 同厂商）。开发期可统一用 DeepSeek 但不同 system prompt 区分。
4. **病毒游戏 touch 事件落点**：作为新 `event_type`（如 `world.touch`）写入事件流，还是包成 `action.resolved` 的 payload？影响 brain 数据层 schema。
5. **Faction<X> 视角集合的早期占位形态**：02 病毒游戏不需要阵营，但 viewer 封闭集合在 0.2 必须固定；建议 v1 写死 `Faction` 占位但不分配，给后续游戏（如 11 腐败警察）留位。

---

## 路标摘要（一页纸记忆点）

- **顺序**：数据层（含 1.6 `_meta.db` 最小骨架）→ 协议层 → UE 接入 → 物品/Delete/ontology v2 → MVP 游戏（02 病毒）
- **形态**：脑层 Python 仓 `BrainService/`（gitignored，独立仓库嵌套在 UE 工程根目录）；UE 仅做薄 glue + 既有原语复用 + HTTP polling
- **硬边界**：`speech.public` 与 `action.intent` 在 schema 层 / UE Dispatcher 层全程分离；UE 永不写入 brain 事件流（只 POST 上报，由 brain 写入）；Speak 不进 ontology
- **铁律**：append-only 哈希链 / 视角隔离行粒度 / Reasoner 强制四通道 / floor 由 bid 裁决 / 未抢中 intended 永久留存 / 摘要者≠行动者
- **保护**：UMinimaxACELibrary、UAILiveProjectPerceptionLogger、ScatterMover、SightMemoryComponent、SandboxCharacter_Mover、AIC_NPC_SmartObject、Build.cs V6
- **退役**：M / I / N / K / L / O 测试键、BP_MH_Character_1 T 键、StoryScenarioDirector / Act01RuleIntroDirector（按节奏）
