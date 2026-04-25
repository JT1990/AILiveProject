# AI 心智决策系统任务清单审查

审查日期：2026-04-25  
审查重心：落地可执行  
交付物范围：审查 `Docs/PRD.md`、`Tasks/README.md`、全部 `Tasks/task_*.md`，并对照当前 `Source/`、`Config/`、`Content/` 文件列表、`.gitignore`、`.mcp.json`、`CLAUDE.md` 与 `DevLog/` 记录。

> 说明：本次会话没有暴露 Monolith MCP 工具，`tool_search` 未发现可调用的 `monolith_status` / Blueprint 查询工具。因此本报告没有直接读取 Blueprint 图的实时状态；涉及资产图细节的结论以 DevLog、Content 文件列表和项目约束为依据，并在建议中保留“实施前用 Monolith 预检确认”的要求。

## 执行摘要

当前任务清单的方向是成立的：用骗子酒馆验证“私有状态、声明与真实分离、回合制裁判、欺骗/质疑”，再用少数决验证“自由谈判、联盟、背叛、跨轮记忆”。这条路径和 PRD 的“多智能体社会博弈系统”目标高度一致。

但以“马上指导开发”为标准，清单还不能直接执行。主要阻塞点有 6 类：

1. `.env` key 命名在 T00/T01/T05/T07 之间不一致，会让 DeepSeek Provider 读不到 key。
2. `TriggerMinimaxSpeech` 的调用对象与参数在多张任务卡里写错，可能出现编译失败或“有声音但脸不动”。
3. `Validate / Apply` 原则写在 README 和 T11 中，但 T12/T21/T22 仍残留 `ValidateAndApply` 或 Action 直接改 GM state 的设计。
4. T18 Perception 的 M0 范围自相矛盾：文字说“只 log”，伪代码却直接 `RequestDecision`。
5. T16 要用 per-peer `by_tag` 检索，但 `/memory/by_tag` 到 T24 才实现，时序倒挂。
6. SmartObject/EQS/关卡卡片的前置依赖写得不一致，T10/T20 在卡面上只依赖 T01，却直接消费 T19.7 的资产。

建议先修订这些 P0/P1 问题，再进入 T01。否则很容易出现“每张卡看似能做，跨卡集成时返工”的情况。

## P0 Findings

### P0-1：`.env` key 命名不一致会直接阻断 DeepSeek 调用

**证据**

- T00 写了 `DEEPSEEK_API_BASE` / `DEEPSEEK_API_KEY`，但又写“计划添加 `deepseek=sk-...`”。
- T01 明确说后续用 `XXX_API_KEY` / `XXX_API_BASE`，但验收信号要求 `Get Env Value From Project Env("deepseek")`。
- T05 DoD 要通过 `GetEnvValueFromProjectEnv("deepseek")` 读 key，伪代码又使用 `ApiKeyEnvName = "DEEPSEEK_API_KEY"`。
- T07 的 DataAsset 字段使用 `DEEPSEEK_API_BASE` / `DEEPSEEK_API_KEY`。

**影响**

T05 可能在 Blueprint 测试里通过不了，T06/T07 的 Mind 闭环会因为 key 为空而失败。这个问题不属于实现细节，而是跨卡契约不一致。

**建议**

- 统一约定：新 key 一律使用大写 env name，例如 `DEEPSEEK_API_KEY`、`DEEPSEEK_API_BASE`、`GLM_API_KEY`、`GLM_API_BASE`。
- T01 验收改成 `GetEnvValueFromProjectEnv("DEEPSEEK_API_KEY")` 和 `GetEnvValueFromProjectEnv("DEEPSEEK_API_BASE")`。
- T02 `UMindAgentConfig.ApiKeyEnvName` 注释从“例 deepseek”改成“例 DEEPSEEK_API_KEY”。
- T05 DoD 改为通过 `ApiKeyEnvName` 字段读取，不再写死 `"deepseek"`。
- `minimax=` 作为历史遗留只给 MiniMax TTS 使用，不扩散到新 provider。

### P0-2：Speak Action 应该驱动可见 MetaHuman child actor，而不是 NPC 壳 Pawn

**证据**

- 当前 C++ `UMinimaxACELibrary::TriggerMinimaxSpeech` 只会在传入的 `AActor* Character` 上找/挂 `UACEAudioCurveSourceComponent`。
- DevLog 明确指出生产路径应通过 `NPC.VisualOverride.ChildActor` 间接拿到 `BP_MH_Character_N` 实例来触发 A2F/TTS。
- T06/T13/T22 伪代码都用 `Owner->GetOwner()` 作为 `Speaker`，这通常是 `BP_NPC_MH_Character_N` 壳 Pawn，不是可见的 MetaHuman child actor。
- 项目规则写明：A2F 驱动角色必须在可见 skeletal mesh actor 上有 `UACEAudioCurveSourceComponent`，Face AnimBP 里有 `ApplyACEAnimation` 节点。

**影响**

最坏情况是 T06/T07 看起来“音频播了”，但 Face AnimBP 没有读到正确 actor 上的 curve，口型静默失败。这会直接破坏 M1/M2/M4 的核心演示。

**建议**

- 在 T06 前增加一个小的 C++/BP helper：`ResolveSpeechActor(AActor* MindOwner)`。
- 解析顺序：
  1. 如果 owner 是 VisualOverride 壳，优先返回 `AC_VisualOverrideManager` 当前 child actor。
  2. 如果找不到 child actor，再 fallback 到 owner。
  3. 如果目标 actor 没有 Face/ACE 契约，输出 `LogMindAction` warning。
- 所有 `UMindAction_Speak`、`PlayCards`、`Challenge`、`Vote` 的 TTS 调用都走该 helper。
- M1 验收增加一项：`LogMinimaxACE` 显示实际 dispatch 的 actor name 是 `BP_MH_Character_*` 或对应 child actor，而不是只看到 NPC shell。

### P0-3：`TriggerMinimaxSpeech` 伪代码参数类型和默认 Endpoint 用法错误

**证据**

- 真实签名是 `TriggerMinimaxSpeech(UObject*, AActor*, const FString& Text, const FString& ApiKey, const FString& VoiceId, const FString& Endpoint, FName A2FProviderName)`。
- T06/T13/T22 多处传 `FText::FromString(...)` 给 `Text`。
- 多处传 `/*Endpoint*/{}` 或 `{}`，这不是“使用默认参数”。在 C++ 调用中只要传了这个位置参数，就会把 endpoint 设置为空字符串。

**影响**

这会造成编译错误，或造成 `MinimaxSpeech::RequestBlocking` 请求空 URL。即使编译后从 Blueprint 默认值可用，C++ Action 路径也会坏。

**建议**

- 所有 C++ Action 调用都传 `FString Line`，不要传 `FText`。
- 不要用 `{}` 表示默认 endpoint。建议增加：
  - `UMinimaxACELibrary::GetDefaultMinimaxEndpoint()`，或
  - 在 `UMindAction_Speak` 内部统一填 `https://api.minimaxi.com/v1/t2a_v2`，或
  - 扩展 `UMindAgentConfig` 加 `MinimaxEndpoint` 默认值。
- `A2FProviderName` 在 Config 中建议存 `FName`，不是 `FString`，避免每个 Action 自己转换。
- 给 T06 增加编译级验收：`MindAction_Speak.cpp` 不允许出现 `FText::FromString` 作为 `TriggerMinimaxSpeech` 实参。

### P0-4：Validate / Apply 契约仍被后续卡破坏

**证据**

- README 和 T11 明确要求 `Validate -> Action.Execute -> Apply -> Memory.Write`。
- T12 关键 API 里仍写 `ValidateAndApply`，并在伪代码里混合验证、修改 state、切 phase。
- T21 DoD 写 `ValidateAndApply`。
- T22 DoD 写 `UMindAction_AskQuestion` 解析后“写入 `GM.State.CurrentQuestion`”，`ProposeAlliance` / `AcceptAlliance` 也写 GM 状态。

**影响**

异步 Action 失败、PIE 中断、TTS/MoveTo 失败、并发 challenge 晚返回时，会把 GM 状态提前污染。该问题会直接破坏规则可信度。

**建议**

- 全部任务卡只保留 `Validate` / `Apply`，删除 `ValidateAndApply` 术语。
- Action 只做三件事：解析参数、执行外显动作、返回 summary。
- `AskQuestion` 的 `question_text`、`Vote.choice`、`ProposeAlliance.target`、`AcceptAlliance.from` 都应存在 `FMindActionEnvelope.ParamsJson` 中，由 `GM.Apply` 写入真实 state。
- `GM.OnAgentActionFinished` 只负责阶段推进，不在 Action 内推进。
- 在 T11 验收加“故意让 Action.Execute 返回 false，确认 GM state 没有变化”的测试。

### P0-5：T18 Perception M0 范围自相矛盾

**证据**

- T18 目标和 DoD 说 M0 阶段 `OnPerceptionUpdated` 只 log，T07 后才补 `RequestDecision`。
- 同一文件伪代码最后直接调用 `RequestDecision(Reason)`。

**影响**

如果按伪代码实现，M0 或 T18 时会在 LLM/Action/Memory 尚未稳定时引入感知驱动决策，导致 cooldown drop、意外 HTTP 调用和难以定位的测试噪声。

**建议**

- T18 M0 实现严格只 log + 更新感知缓存。
- 在 `UMindComponent` 加开关：`bPerceptionCanTriggerDecision = false`。
- T07 或 M1 通过后再显式打开该开关，且默认仍节流 5s。
- T18 验收不应出现 DeepSeek request；只允许出现 `saw_*` / `heard_*` 日志。

### P0-6：T16 依赖 `/memory/by_tag`，但该端点到 T24 才实现

**证据**

- T16 要在 `BuildSystemPrompt` 中对每个 peer 做 `ByTag(speaker=peer_id, top_3)`。
- README 也把“跨游戏长期关系”落到 T16/T26。
- `/memory/by_tag` 的服务端和 UE client 实现在 T24。

**影响**

M3 无法实现 T16 的“跨游戏关系 prompt 模板”，只能用普通 vector recall 模糊检索。若按 T16 写，会在 T24 前返工 Memory Service 和 UE client。

**建议**

二选一：

- 推荐方案：把 `/memory/by_tag` 从 T24 提前到 T08/T09，作为 Memory Service 的基础能力。T24 只消费它，不再新增端点。
- 保守方案：拆 T16 为两层：
  - T16A：LiarsBar 事件级记忆 + persona，只用 `/memory/recall`。
  - T24/T26：引入 per-peer `by_tag` 后，再开启跨游戏关系 prompt。

### P0-7：T10/T20 关卡卡片前置依赖漏写 SmartObject 资产

**证据**

- T10 DoD 使用 `BP_PokerSeat_SmartObject`，但前置只写 T01。
- T20 DoD 使用 `BP_Chair_SmartObject` 和 `BP_VoteBox_SmartObject`，但前置只写 T01。
- README 依赖图知道 T19.7 在 M0 完成，但单卡前置不一致。

**影响**

执行者按单卡施工时会先建关卡，发现需要不存在的 SO actor BP，然后回头改 T19.7，造成顺序返工。

**建议**

- T10 前置改为 `T01 + T19.7`。
- T20 前置改为 `T01 + T19.7`。
- T10/T20 的“前置依赖”段合并进标准 `## 前置`，避免同一文件两个前置段落。

## P1 Findings

### P1-1：UMG HUD 不能直接绑定普通 `DECLARE_MULTICAST_DELEGATE`

T11 定义的是普通 C++ multicast delegate，T14/T23 又要求 UMG BP 订阅 `OnPhaseChanged` / `OnGameEvent`。Blueprint 需要 `DECLARE_DYNAMIC_MULTICAST_DELEGATE` 并通过 `UPROPERTY(BlueprintAssignable)` 暴露，或者 HUD 用 C++ Widget/Subsystem 主动 polling。

建议在 T11 就把 GM 事件接口定型：

- `FOnMindPhaseChangedDynamic`
- `FOnMindGameEventDynamic`
- `UPROPERTY(BlueprintAssignable, Category="AI Live|GM")`

否则 T14 才发现 HUD 绑定不上，会回头改 GM 基类。

### P1-2：T05.5 Mock Provider 应前移，且伪代码 timer 写法不可用

Mock 的目标是提速开发，但当前前置放在 T05 后，等真 DeepSeek 打通后才做。实际更合理的是：

- T02 Provider 基类后即可实现 Mock。
- T06/T07 先用 Mock 跑通 speak 闭环。
- T05 真 DeepSeek 可以并行或作为 M1B 验收。

另外 `GEngine->GetTimerManager()` 不是可用路径。Mock 延迟应通过 `World->GetTimerManager()`、`FTSTicker`，或让 `RequestCompletion` 接收/保存 world context。

还要补一个配置字段：`UMindAgentConfig` 当前只有 `ProviderClass`，没有 `MockResponseTable`。若 Mock 需要 DataAsset 表，Config 或 Provider 实例必须有地方持有该表。

### P1-3：T02/T03 文件数量过大，容易制造“骨架即债务”

T02 一次新增 14 个文件，T03 一次新增 20+ 个文件。虽然 T02 写了增量编译策略，但 T03 仍把两款游戏、7 个游戏动作、8 个通用动作全部提前铺开。

建议：

- T02 保留核心 contract：`AgentConfig`、`AgentView`、`ActionEnvelope`、`LLMProvider`、`MindAction`、`MindComponent`。
- T03 只建 GM 基类和两个 state USTRUCT。
- 具体 Action 类在 T06/T13/T19/T22 各自实现时创建，避免一堆空类在 schema、路径和命名变更时一起返工。
- 如果坚持 T03 建骨架，必须在 T03 中锁定路径命名，后续卡不能一会儿写 `Public/Mind/Actions/`，一会儿写 `Public/Mind/GameMaster/Actions/`。

### P1-4：Memory Service vector 检索方案需要收敛到一个 MVP 路径

T08 同时提到 GDS cosine、Neo4j native vector index、Python cosine fallback。当前数据量很小，M2/M4 不需要一开始就压榨 Neo4j 向量索引能力。

建议 MVP 选择一个确定路径：

- T08 默认用 Python 端 cosine fallback：按 `agent_id` 拉取该 agent 的 Memory，numpy/Python 排序 top_k。
- 如果 T00 确认 Neo4j 5.x native vector index 可用，再作为优化路径。
- T08 验收写入 embedding dim，并把 dim 写进 `Tasks/T00_PREFLIGHT_RESULT.md`。

这样能显著降低 Memory Service 首次实现复杂度。

### P1-5：SmartObject 低层 C++ helper 风险高，且项目已有 GASP SmartObject 资产可借鉴

Content 中已有 `Content/Blueprints/SmartObjects/`、`AIC_NPC_SmartObject`、StateTree 和 SmartObject 相关 Task/Condition 资产。T19.7 直接设计低层 `USmartObjectSubsystem` Claim/Release helper，但 UE 5.7 SmartObjects API 版本差异较大，任务卡自己也标记了风险。

建议：

- T19.7 开头新增“复用/差量预检”：用 Monolith 检查现有 SmartObject Base、Bench Definition、StateTree 是否能复用。
- MVP 的 Vote/Sit 可以先走“EQS 找 actor + AIController MoveTo + 到达即 Done”，把 SmartObject Claim 作为 P1 增强，而不是 M0 阻塞项。
- 如果坚持 SmartObject Claim，T19.7 必须先用 Monolith/engine source 验证具体 UE 5.7 API 签名，再写 C++。

### P1-6：T19 通用动作需要补 Memory 前置和 cooldown 旁路

T19 的 `think`、`remember`、`recall` 都依赖 `UMindMemoryClient`，但前置只写 T11。应补 `T09`。

`recall` 动作执行后“立刻再次 RequestDecision”，但 `OnActionDone` 会更新 `LastDecisionAt`，下一次 `RequestDecision("post_recall")` 很可能被 cooldown drop。

建议：

- T19 前置改为 `T09 + T11 + T18 + T19.5 + T19.7`。
- 给 `RequestDecision` 增加内部 flag：`bBypassCooldownForPostRecall`，只允许 recall 链路使用。
- 或者不二次调用 `RequestDecision`，改为在同一次 LLM 调用前完成 recall，减少递归决策复杂度。

### P1-7：T21 Negotiate 阶段需要全局 LLM 预算，而不只是 cooldown

T21 说 GM 每 10s 唤醒 1 个 NPC，但同阶段又允许所有 alive agent 多次 RequestDecision。T18 Perception 打开后，玩家/NPC 接近也可能触发额外决策。仅靠 `DecisionCooldownSeconds=2` 无法保证 QPS。

建议：

- 在 `AMindGameMaster` 或 `UMindDecisionScheduler` 中加全局预算：
  - 每阶段最大并发 LLM 数。
  - 每 10s 允许 N 次 GM 主动唤醒。
  - Perception trigger 在 GameMaster 阶段内可禁用或降级为只写 awareness。
- T14.5 的 rate limit 结果写入一个配置字段，如 `NegotiateWakeIntervalSeconds`，不要只写 DevLog。

### P1-8：T15 从 C++ 调 BP 函数的方式需要接口化

T15 写 `Owner->GetOwner()->PlayMindMontage(...)`。除非 owner 的 C++ 类型声明了该函数，否则普通 `AActor*` 不能直接调用 BP 函数。

建议：

- 新建 `UMindPerformableInterface` / `IMindPerformableInterface`，暴露 `PlayMindMontage`、`ResolveSpeechActor` 等行为。
- BP_NPC_MH_Character 父类实现该接口。
- C++ Action 通过 `IMindPerformableInterface::Execute_PlayMindMontage(...)` 调用。

### P1-9：M2/M4 的 Initialize 责任不清，可能重复初始化

T07 在 NPC BeginPlay 调 `MindComponent.Initialize(Config, nullptr)`。T11 `StartGame` 又对 Participants 调 `Initialize(Config, this)`。T23 还建议每个 NPC BeginPlay 通过 `GetAllActorsOfClass` 找 GM 再 Initialize。

建议统一：

- Sandbox/M1：NPC 自己 Initialize，GM 为空。
- 游戏关卡：只有 GM `StartGame` 负责给 Participants Initialize，并注入 GM。
- NPC BeginPlay 不找 GM；最多只缓存 Config。
- `Initialize` 应允许二次调用，但必须 log：旧 GM -> 新 GM、是否重建 Provider、是否重置 ActionRegistry。

### P1-10：T22 的“私聊”是信息过滤，不是音频过滤，验收要写清楚

T22 明确 MVP 不做 TTS attenuation，但又写“自然形成私聊在物理上就是不被听到”。实际玩家和场上角色表演层可能都能听到声音，只有 Memory 写入按 hearing 过滤。

建议术语改为：

- “信息层私聊”：Memory/Prompt 只写给能听到的 agent。
- “表演层声音”：MVP 仍可能全场播放，不作为信息真相。

验收也应改为检查 Memory Service 中谁收到了私聊记忆，而不是只靠人耳听。

### P1-11：M3 的 T15 动画 polish 可以后移，T16 记忆/persona 应先做

从 PRD 对齐度看，T16 的跨局记忆、persona 差异、事件级记忆，比 T15 的出牌/拿枪 Montage 更直接服务“社会人格”和“长期策略”。

建议 M3 顺序调整为：

1. T16A：事件级记忆 + persona。
2. T17A：不用动画先跑 3-5 局观察策略。
3. T15：动画 polish。
4. T17B：带动画录屏复检。

这样能更早暴露“LLM 是否真的产生策略差异”的产品风险。

### P1-12：T18.5 多厂商不应阻塞 M4 A 级最小闭环

多厂商是 PRD 终态目标，但 M4 的首要目标是 8 NPC 少数决单轮跑通。把 GLM 接入放在 M4 起点，会把供应商账号、模型名、格式差异、rate limit 差异同时引入。

建议：

- M4A 先用 DeepSeek 或 Mock/DeepSeek 混合跑通 8 NPC 单轮。
- T18.5 作为 M4B 或 M5 前增强，再做 DeepSeek + GLM 混搭。
- T18.5 实施当天必须重新核实 GLM endpoint/model/key 格式，不要把模型名当长期稳定事实。

## P2 Findings

### P2-1：T00 写“7 项 checklist”，实际有 8 项

README 里程碑表写 T00 “7 项 checklist 全过”，T00 文件实际有 1-8 项，其中第 8 项是弱前置。建议改成“1-7 必过，8 弱前置记录即可”。

### P2-2：`git status` 在大 UE 项目可能很慢

本次审查中 `git status --short` 在工作区里运行时间过长，需要手动终止。T00 的 `.env` 安全检查建议使用更窄的命令，例如：

- `git ls-files .env`
- `git check-ignore -v .env`
- `git status --short --untracked-files=no -- .env`

### P2-3：T26 只要求更新 `CLAUDE.md`，但本仓库也有 `AGENTS.md`

当前仓库同时存在 `CLAUDE.md` 和 `AGENTS.md`。如果 Mind/GameMaster 子系统成为长期协作约定，T26 应同步更新两个文件，或明确 `AGENTS.md` 是主源、`CLAUDE.md` 是镜像。

### P2-4：PRD 的供应商型号池容易过期，应标记为“候选快照”

PRD 已说明模型、价格、可用地区会变化。任务清单里的 T18.5 也应保持同样口径：不要把具体 model id 写成不可变承诺，实施时以 provider dashboard/API docs 当日结果为准。

### P2-5：M2/M4 验收缺少“非法 JSON 比例”

LLM ActionJSON 是整个系统的协议入口。T14/T23/T26 应收集：

- LLM 返回总数。
- JSON parse fallback 次数。
- Validate reject 次数。
- 连续 reject 后强制 Wait 次数。

否则后续很难判断 prompt 是否稳定。

## 里程碑级建议

### M-1：T00 应成为真正的“闸门”

保留 T00 作为强制前置。建议补充：

- `Monolith MCP 可用性`：如果 `monolith_status` 不在线，不进入任何 BP/资产任务。
- `NPC speech actor baseline`：记录每个 NPC 的 VisualOverride child actor 是否存在、Face AnimBP 是否含 ApplyACEAnimation。
- `AIController/AutoPossessAI baseline`：确认 MoveTo 是否能直接用 `AAIController`，否则 T18/T19/T19.7 都要调整。
- `Memory service secrets`：不要在结果文件里写明完整 password/key，只记录 masked 值。

### M0：拆成“代码/服务地基”和“执行层资产地基”

推荐 M0 顺序：

1. T01 env + Build.cs + log categories。
2. T02 Mind 最小 contract。
3. T04 Memory `/health`。
4. T05.5 Mock Provider。
5. T18/T19.7/T19.5 执行层资产与 helper，但 T19.7 先做现有资产复用预检。
6. M0 验收：Build 过、Memory health、Perception/EQS/SO 手工调用过。

这样 M1 可以先用 Mock 完成闭环，不必等真 DeepSeek 稳定。

### M1：先证明“可控闭环”，再证明“真 LLM”

M1 建议拆成：

- M1A：Mock -> Speak -> MiniMax/A2F，验证 actor 解析、口型同步、cooldown。
- M1B：DeepSeek -> JSON -> Speak，验证外部 LLM、解析、fallback。

M1 的关键不是“说出一句即兴话”，而是证明 MindComponent 状态机、Provider、Action、A2F actor 选择都正确。

### M2：在接 4 NPC 前加一个 deterministic GM simulation

T12/T13 完成后，建议先用手工 envelope 或 Mock responses 驱动 GM 跑 3 回合，不接真 LLM。这个可以作为 T13.5 或 T14 A0：

- 手工 play_cards 合法/非法各一次。
- challenge 先到先得一次。
- no challenge timeout/auto reveal 一次。
- roulette hit/miss 固定 seed 各一次。

这能把规则 bug 和 LLM 格式 bug 分开。

### M3：优先验证“社会人格”，动画 polish 后置

把 T16 的事件级记忆、persona 差异、跨局引用提前到 T15 前。动画不会证明 PRD，记忆与策略差异会。

### M4：先单厂商 8 NPC 单轮，再混厂商

少数决本身已经引入 8 NPC、自由谈判、私聊、投票、EQS、SmartObject、HUD。建议先用单 provider 稳定 M4A，再引入 GLM 做 M4B。

### M5：明确持久化范围

T24 写“重启游戏即清记忆可接受”，但 Memory Service 使用 Neo4j，本身是持久化的；PRD 又强调长期记忆。建议明确：

- MVP 验收要求：同一 Memory Service 进程内跨关卡、跨局持久。
- 可选验收：重启 PIE 后仍保留记忆。
- 不要求：重启 Neo4j/清库后的恢复。

## 任务卡级修订建议

| 任务 | 建议 |
| --- | --- |
| T00 | 增加 speech actor baseline、AIController baseline、Monolith 可用性；`.env` 检查用窄 git 命令。 |
| T01 | 统一 uppercase env names；在 `AILiveProject.cpp` 明确实现 `StartupModule` 才能启动时 `VerifyEnvSafety`；补 `AIModule`、`NavigationSystem`、`GameplayTasks`、`UMG`、`Slate`、`SlateCore`、`GameplayTags` 等真实依赖候选。 |
| T02 | `ApiEndpoint` 改成 `ApiBaseEnvName`；`A2FProviderName` 用 `FName`；为 Mock 表预留配置字段。 |
| T03 | 减少提前创建的空 Action 类，或锁定唯一目录；GM delegates 用 BlueprintAssignable 动态委托。 |
| T04 | `/health` 可返回 `status:"ok"` 但建议同时有 `dependencies.neo4j/embed` 字段，避免调用方误解 degraded 也完全可用。 |
| T05 | 修正 env key；`TestPing` 若 BlueprintCallable 需要动态 delegate 或明确“只 log”。 |
| T05.5 | 前移到 T02 后；修正 timer；补 Mock table 配置入口。 |
| T06 | 增加 `ResolveSpeechActor`；修正 `FString`/Endpoint/FName；明确 TTS 失败当前无法同步回 Action Done。 |
| T07 | 验收增加“实际 speech actor 是 child actor”与 Face AnimBP curve 生效；游戏关卡中不再由 NPC 自己找 GM。 |
| T08 | 收敛 vector 检索 MVP 路径；如要 T16 by_tag，提前实现 `/memory/by_tag`。 |
| T09 | HTTP 回调用 `TWeakObjectPtr` 是必须项；BaseUrl 建议来自 env/config，而不是硬编码。 |
| T10 | 前置补 T19.7；如果 `BP_PokerSeat_SmartObject` 不存在则阻塞，不临时改普通 actor。 |
| T11 | 把 dynamic delegates、Initialize ownership、Validate/Apply 测试写进 DoD。 |
| T12 | 删除 `ValidateAndApply` 伪代码；补 `pass_turn` 语义和 ChallengeWindow timeout。 |
| T13 | 修正 TTS 调用；Action 不知道 TTS 成功时不要把“语音成功”当作游戏成功。 |
| T14 | HUD 绑定依赖 T11 dynamic delegates；A 级验收增加 JSON fallback/reject 计数。 |
| T14.5 | 把测得的 wake interval 推荐写入可配置资产或 GM 默认值，不只写 DevLog。 |
| T15 | 用接口调用 BP montage；先确认 GASP montage slot，不要直接假设 UpperBody slot 存在。 |
| T16 | 拆分 by-query 版和 by-tag 版；若不提前 T24，则不要承诺 per-peer by_tag。 |
| T17 | 除主观观察外增加基础计数：谎称率、挑战率、JSON fallback 率、Validate reject 率。 |
| T18 | M0 只 log，不 RequestDecision；加开关控制感知触发决策。 |
| T18.5 | 不阻塞 M4A；实现 OpenAI-compatible helper 后再加 GLM 子类。 |
| T19.7 | 先审查现有 GASP SmartObject 资产；低层 Claim API 先验证签名。 |
| T19.5 | 前置保持 T19.7；Context/Params 类型要落实到 UE 5.7 可编译 API。 |
| T19 | 前置补 T09；recall 后二次决策要 bypass cooldown 或改为同轮 recall。 |
| T20 | 前置补 T19.7；标准 `## 前置` 不要与后面的“前置依赖”重复。 |
| T21 | 加全局 LLM budget；Tally 伪代码中不要引用临时数组引用返回局部对象。 |
| T22 | Action 不直接写 GM state；私聊改名为“信息层私聊”；Memory 验收检查接收者集合。 |
| T23 | 统一 Initialize ownership；A 级验收增加 429/JSON fallback/Validate reject 指标。 |
| T24 | 若 by_tag 不提前，则这里实现后再开启跨游戏 per-peer prompt；明确持久化范围。 |
| T25 | WidgetComponent 更新频率 2Hz 很好；F9 输入要确认 DefaultPawn/PlayerController 实际承载位置。 |
| T26 | 同步更新 `AGENTS.md`/`CLAUDE.md`；多厂商已在 T18.5，不要再写“LLM 厂商单一”的固定模板。 |

## 推荐调整后的关键路径

```text
T00
  -> T01
  -> T02
  -> T05.5
  -> T06(Mock Speak + ResolveSpeechActor)
  -> T07(M1A)
  -> T05(DeepSeek) -> T07(M1B)

并行：
T04 -> T08(/write /recall /by_tag 可选提前) -> T09
T03 -> T11
T18 / T19.7 / T19.5

M2：
T10 -> T12 -> T13 -> T13.5(手工/Mock GM simulation) -> T14 -> T14.5

M3：
T16A(事件级记忆 + persona) -> T17A(无动画观察) -> T15 -> T17B

M4：
T19 -> T20 -> T21 -> T22 -> T23(M4A 单厂商)
  -> T18.5(M4B 多厂商)

M5：
T24 -> T25 -> T26
```

## 待确认问题

1. Monolith MCP 是否能在实施期正常连接编辑器。如果不能，所有 BP/UMG/资产任务都需要 fallback 方案或暂停。
2. `BP_NPC_MH_Character_1..8` 当前 `AIControllerClass`、`AutoPossessAI`、Controller 实例是否满足 `AAIController::MoveTo`。
3. VisualOverride child actor 的运行时获取方式应落在哪个类：`AC_VisualOverrideManager`、NPC BP interface，还是 C++ helper。
4. `/memory/by_tag` 是否提前到 T08/T09。如果不提前，T16 的跨游戏关系模板必须降级。
5. M4 是否接受先单厂商跑通，再引入 GLM。这个决定会显著降低 M4 首轮集成风险。

## 结论

这份任务清单的产品路线是对的，尤其是“骗子酒馆 -> 少数决”的验证顺序很贴合 PRD。但当前版本混入了若干会直接阻断执行的接口不一致和时序倒挂。优先修掉 P0，再把 P1 中的 Mock 前移、HUD delegate、Memory/by_tag、speech actor helper、全局 LLM budget 固化，任务清单就能变成一条更稳的工程路径。
