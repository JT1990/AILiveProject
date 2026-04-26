# 2026-04-27 Mind 上下文架构重设计：messages[] 主路径

## 目的

T07.5 通过后准备进 M2（task_08 Memory 端点 + task_09 Recall 客户端）时，对原设计做了一次架构层面的否决与重写：

- 原设计：每次决策走单发 `(System, User)` prompt + Neo4j vector recall top_5 拼记忆
- 否决理由（基于实战经验）：
  1. vector recall 召回率不可靠
  2. 一局游戏几十到几百回合，top_5 撑不住战术上下文
  3. NPC 看不到自己上轮的"内心独白"会跨轮行为漂移

新主路径：**per-agent 持续 `messages[]` 对话流**（参考 Letta MemGPT 三层记忆 + Claude Code auto-compact + Factory.ai anchored summary），vector recall 降级为 LLM 主动调的 tool。

完整架构方案在 `~/.claude/plans/recall-abundant-pascal.md`（v2.1）。**所有任务卡 + CLAUDE.md 已同步落地**——本次工作没改一行代码，全是文档级架构重写。

## 三层记忆架构（落到 task_02 + CLAUDE.md）

```
Layer 0a (messages[0] system, verbatim cache-stable)
    ① AI 实例声明 + ② IdentitySummary + ③ ContinuityStakes
    + ④ Persona + ⑤ Goals + ⑥ Injection 防御段 + ⑦ 输出协议
    禁止入: ActionRegistry / 阶段元 / peer_summary / 时间戳

Layer 0b (messages[1] user, 每唤醒原地重写)
    当前阶段元 + ActionRegistry schema (Keys.Sort() 后渲染)
    + cross-scene peer_summary

Layer 1 (messages[2] user, 增量摘要)
    AnchoredSummary, USER role (跨厂商兼容; system role 中途插入未在 GLM/Anthropic 文档显式承诺)

Layer 2 (messages[3..N], hot verbatim)
    user: GM 阶段广播 / Perception / 公开发言 / 偷听 / 驳回 / 结果回执
    assistant: 自己上次 ActionJSON (verbatim 含 reasoning + IM)
    tool: Recall tool_call 结果 (Provider 内部循环 append)

Layer 3 (Neo4j, 跨场跨 PIE)
    /memory/event         ← HandleActionDone 内 fire-and-forget 事件归档
    /memory/peer_summary  ← scene_end 时每个 NPC 自摘 per-peer
    /memory/scene_log     ← JSONL 全 buffer 归档 (debug + replay)
    /memory/recall        ← Provider tool_call 唯一消费方
```

## 已确认的关键决策

| ID | 决策 | 理由 |
|---|---|---|
| D1 | scene 边界 = 一局游戏 | 贴合 PRD"跨局指控"语义；M5 多轮淘汰每局有独立 peer_summary 留档 |
| D2 | 跨 PIE buffer 不落盘 | MVP 简化；靠 scene_end 写入 Neo4j 的 peer_summary + event log 沉淀 |
| D3 | T08/T09/T16/T22 直接重写正文（不留代际） | 项目初期无版本代际；不留 DEPRECATED 废墟也不留 prime 后缀 |
| D4 | 新增 T07.6 锚定 messages[0] 字段顺序 | P0 红线：cache 命中靠字符级稳定 |

## 改动汇总（19 张任务卡 + 3 份顶层文档）

### 任务卡

| 卡 | 改动 |
|---|---|
| **task_07_6** | 新建：messages[0] System 完整字段顺序 ①-⑦ verbatim 锚定（P0 红线） |
| **task_08** | 重写正文：Memory Service 5 端点（event / peer_summary / scene_log / recall / by_tag debug）；老的 write/recall/by_tag 单端点设计完全替换 |
| **task_09** | 重写正文：UMindContextManager + Provider messages[]+tool_call + Summarizer + LLMBudgetSubsystem；老的"决策前自动 Recall + 拼 user 段"完全替换 |
| **task_16** | 重写正文：scene_end Summarizer 写 peer_summary + 新场 Layer 0b 注入；老的 ByTag 跨局 prompt 替换 |
| **task_22** | 重写正文：私聊 hearing 推送（GM.RecordSpeechEvent → listener.Context.PushUser，不调 MemoryClient.Write）+ 4 个少数决 Action |
| **task_02** | 完整重构：加 FMindMessage / FMindModelCapabilities / FToolSchema / ContextManager / Summarizer / LLMBudget 6 个新类骨架；UMindComponent 加 Context / Summarizer / MemoryClient / bDecisionDisabled / CurrentSceneId / ValidateRetryDepth 字段；删 RecallChainDepth / StashedRecall / MemoryRecallTopK |
| **task_05** | 重写：Provider 接口签名换 messages[]+tool_call 循环；TestPing 包临时 messages；新增 GetContextWindowSize 查 Capabilities table |
| **task_05.5** | 重写：Mock 加 trigger marker + MatchTrigger 字段；匹配优先级 MatchTrigger > MatchPattern > Fallback |
| **task_06** | 重写：BuildSystemPrompt 改为 Context.RebuildLayer0a + UpdateLayer0bFrame；OnLLMResponse 末尾 PushAssistant verbatim |
| **task_07** | DoD 调整：Initialize 时 AgentIdStable 空 hard fail（不 fallback DisplayName）；BeginPlay 顺序明确含 ContextManager / Summarizer 构造 |
| **task_07.5** | 调整：Identity 字段对齐 Layer 0a ①-⑤，⑥⑦ 是固定文本不来自 Config |
| **task_11** | 调整：DispatchAction 加 invalid 时 Context.PushUser 驳回 + ValidateRetryDepth ≤ 5 自递归；HandleActionDone 加 MemoryClient.WriteEvent + Context.PushUser |
| **task_12** | 调整：BuildViewFor 产物从 FMindAgentView 改为 FMindMessage；AwakeAgent 之前调 RecordPhaseChange push L0b（含 Keys.Sort() 后的 ActionRegistry schema） |
| **task_13** | 调整：Speak Action 触发 TTS 同时调 GM.RecordSpeechEvent，不调 MemoryClient.Write |
| **task_14** | 验收信号扩：`/memory/event` 中能查到所有回合 event；buffer 内可引用上回合 commitment；prompt cache 命中率；Compact 触发次数 |
| **task_14.5** | metrics 表重写：context_tokens_max / compact_count / summary_count / cache_hit_tokens / recall_retry_count / fallback_raw_count / actual_to_estimated_token_ratio_p95 / inflight_http_p95 / scene_summary_p95_seconds |
| **task_17** | 加长会话稳定性指标（5 局累计 Compact / peer_summary 命中率 / cache 命中率） |
| **task_18** | OnPerceptionUpdated 改：节流后 push `[感知]` user message 到 Context.Layer2；是否唤醒由 bPerceptionCanTriggerDecision flag + 阶段策略决定 |
| **task_18.5** | 降级"95% 复用 DeepSeek"为"HTTP helper 复用，serializer + Capabilities + tool_call serializer 单独实现"；提前测中途 system role 兼容性 + cache_usage 字段差异 |
| **task_19** | 删 inline Recall Action（recall 走 Provider 内部 tool_call）；通用动作只剩 8 个真动作 |
| **task_21** | LLM budget 分两池：MaxConcurrentReasoner=4 (决策) + MaxConcurrentSummarizer=2 (Compact + scene_end)；token 类型枚举 Decision / RecallRetry / Compact / SceneSummary |
| **task_23** | 验收 A 级 #7 改"event log 查询"+ B 级 #13 加"tool recall 场景" |
| **task_24** | Vote 阶段不再额外 ByTag 检索，靠 buffer 内 tags=intent/tally 自然回看；明确 tags 在 PushUser/PushAssistant 时的注入点 |
| **task_25** | DebugBadge 加 buffer token 数 + bIsCompacting flag 显示；F10 可选打开历史详情面板调 /memory/by_tag debug 端点 |
| **task_26** | 加长会话稳定性强制项（Compact ≥ 1 / peer_summary 命中 ≥ 90% / Recall tool ≥ 1 / cache ≥ 70%）；跨场验证改为"messages[1] L0b 中能看到上场 peer_summary 文字" |

### 顶层文档

- `Tasks/README.md`：里程碑表 / 任务列表 / 依赖图直接呈现新结构；P1-F 段从"Recall 死循环"改为"决策上限分两层（ValidateRetryDepth ≤ 5 + Provider max_tool_iterations=2）"
- `CLAUDE.md` "AI 心智决策系统"段重写：架构图含 ContextManager + 三层 Memory；P0 强约束扩到 9 条（新增 messages[] 主路径 / 决策上限分两层 / Compact 线程模型 / Speech 写入路径）
- `Tasks/Tasks-Prompt.md`：删除"缺 RecallChainDepth ≤ 2 漂移事故"旧字段例

### 文件操作（4 删 + 4 改名）

- 删除 4 张老 DEPRECATED 卡：`task_08_memory_write_recall.md` / `task_09_memory_client_integration.md` / `task_16_liarsbar_crosssession.md` / `task_22_minorityrule_actions.md`
- 改名 4 张新卡为正式名：去掉 `_prime` 后缀，统一文件名风格

## 业界参考（落到 plan + CLAUDE.md）

- **Claude Code**：~83% 触发 auto-compact；buffer 留给摘要本身
- **Letta (MemGPT)** 三层：Core (RAM 永驻) / Recall (disk 可搜) / Archival (cold tool call)
- **Factory.ai**：增量 anchored summary——只摘要"新被丢弃的 span"并 merge 进持久 summary
- **DeepSeek KV cache 文档**：cache 命中要求 prefix unit 完全 verbatim 匹配（决定 Layer 0a 必须字符级稳定）

## 关键发现（影响落地）

### 1. LLM context window 已是 200k+ 时代

最初按 DeepSeek 32k 算阈值。修订时确认 2026-04 主流：DeepSeek V3.2 ≥ 128k / GLM 4.6 = 200k / Claude Sonnet 4.6 = 200k(/1M) / GPT-5 = 400k。**以 128k 为最坏基线 / 200k 为主流**计算阈值。

实际意义：一场 LiarsBar 100 回合 ~80-120k token，**200k 模型基本不触发 Compact**，128k 模型触发 1-2 次。M2 验收信号按模型区分。

### 2. prompt cache 命中是核心价值，Layer 0a/0b 拆分是必需

第一版 plan 把"身份三段 + Persona + Goals + Injection 防御 + ActionRegistry schema"全塞 messages[0]。审查发现：**ActionRegistry 每阶段动态变化** + **TMap 迭代顺序非确定**，每次决策 messages[0] 都变 → cache 命中率为 0 → "80% cache 折扣"核心价值失守。

修法：messages[0] 严格只放永久不变内容（身份三段 + Persona + Goals + Injection + 输出协议）；ActionRegistry / 阶段元 / peer_summary 进 messages[1] L0b（每次原地重写）；ActionRegistry 拼 prompt 时 `Keys.Sort()` 保证字节稳定。

### 3. UE 线程模型决定 Compact 用 bool flag 不用 mutex

第一版方案写 `Context.AcquireCompactLock() / ReleaseCompactLock()` 包裹 Materialize。审查指出：UObject 只能 game thread 访问；HTTP `FHttpModule` 回调本来就在 game thread。**用 mutex 反而会 crash**。

修法：`bool bIsCompacting` flag + `PendingTrigger`（覆盖式只留最新一个）+ HTTP 回调用 `TWeakObjectPtr` 守 + `BeginDestroy` 时 cancel pending HTTP + `FMindMessage.Seq` 单调递增 compare-and-append。

### 4. Recall 应走 Provider tool_call 而非 Action（避免 cooldown 互斥）

第一版方案把 Recall 设计为 `UMindAction_Recall`，LLM 输出 `recall` envelope → DispatchAction → Action.Execute 调 MemoryClient.Recall → 结果 push 回 Context → 再次 RequestDecision。

审查指出：HandleActionDone 内更新 `LastDecisionAt` → 下次 RequestDecision 立刻被 cooldown 检查 drop → **recall 等于 disabled**。

修法：删除 UMindAction_Recall。Recall 改为 OpenAI 标准 tool_call，Provider 内部循环（while LLM 返回 tool_calls → 执行 handler → append role=tool message → 再调 chat completion；超 max_iterations=2 强制最终生成）。

副作用：`RecallChainDepth` 字段废弃删除；改为 `ValidateRetryDepth ≤ 5`（GM 驳回兜底重试）+ Provider 内部 `max_tool_iterations=2`（recall 上限）拆开两个独立计数器。

### 5. Token 估算 50% 安全网在中文+JSON+emoji 混合下未必够

第一版公式 `HardWatermark 0.83 + OutputReserve 0.10 + SafetyMargin 0.10 = 1.03` 数学上已超过窗口。

修法：先扣 reserve 算 `UsableInput`，`SoftWatermark = UsableInput × 0.70` / `HardWatermark = UsableInput × 0.90`；50% 安全网仅在总量层算一次（`SafetyReserve = raw_estimated × 0.5`），不与单条 EstimateTokens 上调系数双算；连续 3 次实际/估算 > 1.3 自动升 70%。

### 6. AgentIdStable hard fail 必须落到 Initialize

CLAUDE.md 明文 "agent_id 必须用 AgentIdStable，绝不能用 GetName()"。但现有 `MindComponent::GetAgentId()` 实现是 fallback 到 DisplayName 加 Warning——这是隐性 bug：跨局/跨场 peer_summary key 会用错 ID 造成身份漂移。

修法：`Initialize` 时 `AgentIdStable.IsEmpty()` → Error log + `bDecisionDisabled = true` + State=Idle 拒绝决策（不 fallback DisplayName）。

### 7. 私聊偷听不应走 Memory Service

第一版 plan 跟原 T22 设计走 ByTag 写入旁观者记忆。审查指出：写入路径混乱 + 8 NPC 偷听一条公开发言要写 8 次 event log。

修法：`GM.RecordSpeechEvent(speaker, channel, content, listeners[])` 经 `Perception.CanSenseActor(Hearing)` 过滤后**直接 push** `[偷听] / [公开]` user message 到 listener.Context.Layer2；event 写入由发言者自己一次 `WriteEvent` 完成（不是每个 listener 一条）。

## 协作过程（值得记录）

- 启动 plan workflow，做了 5 轮迭代修订（v1 探索 → v2 综合方案 → v2.1 含审查反馈）
- 让两位协作者审查 plan，得到 26 条议题反馈，**批判性接受 23 条**：
  - 完全接受 P0 类 7 条（含修复 cache 失效 / PRD 三段保真 / Recall tool_call 改造等核心 blocker）
  - 部分接受 2 条：scene_end 60s 阈值改为"无硬上限监控记录"；DeepSeek context window 接受可配置原则但拒绝具体数字（无法核实"v4-flash 1M"引用）
  - 拒绝 1 条：审查 1 引用的 DeepSeek pricing 文档具体数字
- 用户多次按"扁平化"约定（CLAUDE.md 末尾"文档要扁平化——不保留 v1/v2/原版 vs 修订/修复历史 等迭代痕迹"）要求清理：
  - 第一轮：4 张老卡标 `[DEPRECATED-by-RECALL-PASCAL]` 头部 + 5 张新卡用 `_prime_` 后缀文件名 + README 加"RECALL-PASCAL 重构追踪"段
  - 用户指出这本身就是"打补丁/迭代痕迹"——项目初期无版本代际，不该保留 v1 / v2 概念
  - 第二轮：删 4 张老卡 + 4 张 prime 改回正式名 + 删除 README "重构追踪"段 + 单卡正文删 "取代 / 不再走 / 关键变化" 等迭代措辞
  - 第三轮：grep 全 Tasks 目录确认 0 处残留（除 UE PCG `_DEPRECATED` 真实 API 字段）

教训：**文档同步要一次性按"当前结论"写，不要按"diff / 补丁"形式累加**。即使是过渡期文档（plan / DevLog 本身），落到任务卡 / CLAUDE.md / README 时必须是结论态。

## 不在 MVP 范围（明确拒绝）

| 项 | 落地节点 |
|---|---|
| Anthropic Claude Provider | M6+（messages[] 顶级 system 字段不兼容 OpenAI 协议；映射工作量大） |
| 跨 PIE buffer 持久化 | D2 决策不做；靠 Neo4j event log + peer_summary 沉淀 |
| cl100k tokenizer 接入 | 后期可换；MVP 用字符粗估 + 50% 安全网 + 偏差监控自动调升 |
| 结构化 JSON anchored summary | M5 spot check 漂移 ≥ 1 次时再切换 |
| Memory Service 认证 | MVP 本地端口任意进程可访问 |
| 真正 Delete 执行 | M6+；MVP 仅 prompt stake |

## 后续工作

任务追踪 (TaskList #2-#7 + #9 共 7 个 task) 是写代码 task，按用户指示"不实现任何任务卡，专注于当前问题改正"暂搁置。任意一项启动按 plan 实施顺序：

1. FMindMessage + FMindModelCapabilities + Provider messages[]+tool_call 接口（解锁所有上层）
2. UMindContextManager 基础（push/materialize + token 估算 + bIsCompacting）
3. UMindMemoryClient + MemoryService 端点（event / peer_summary / scene_log / recall / by_tag）
4. MindComponent 接入 ContextManager（含 L0a/L0b 拆分 + AgentIdStable hard fail）
5. UMindSummarizer + UMindLLMBudgetSubsystem + Compact（含二次压缩）
6. GM RecordPhaseChange / RecordSpeechEvent / RecordSceneEnd 三入口
7. T08-T22 任务卡 DoD 在代码完成后回填实测细节
8. Provider 注册 recall tool + tool_call 循环 e2e 验证（M4+ 验收）
