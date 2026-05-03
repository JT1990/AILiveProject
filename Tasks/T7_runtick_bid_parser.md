# T7 — Bid 协议 + Floor control + Director `RunTick` 主循环重写（**含 Reasoner→Parser 双 LLM 串联**）

> 总览：[00_overview.md](00_overview.md)
> 依赖：blockedBy = [T2.5](T2_5_parser_llm_client.md), [T5](T5_director_writeback.md), [T6](T6_prompt_assembler.md)
> 改动量：**大**（> 4h）—— 主循环重写涉及面广 + Parser 整合
> §11 验收：L1「一拍单一 winner / speech.public 衍生关系 / 冷场推进 / bid 隔离」、L2「反霸麦 / 被 @ 加权 / bid 超时 / tick_resolved/audit 拆分 / action.intent 对所有 agent 派生 / 同拍 tick_no 切片 / payload redaction fuzz」+ 新增「Parser 解析失败 → SystemParseFailed + abstain」+ 「Parser 100% 解析率烟测」

## 预检

(a) `Act02RuleReceiveDirector` 当前主循环 + `EAct02State` 状态机用 Read/Grep 查 .cpp（C++ 状态）；(b) `mcp__monolith__monolith_status` 在线供后续 PIE 联调，按 CLAUDE.md 规则不重写既有 BP 链路。

## 目标

把 Director 主循环从"每轮各 NPC 各发一次"重写为"按拍驱动"——每拍 `BeginTick(N)` → 并行对每 in-scene agent 跑 **Reasoner LLM → Parser LLM 双串联**（Reasoner 出 raw text → Parser 出 4 段结构化 → 4 条事件 `AppendEventsAtomically`，3 次 reject sample 失败后写 `SystemParseFailed` + agent 该拍 abstain）→ orchestrator `ResolveFloor` → Listener-as-filter（MVP 直通）→ 衍生 speech.public + 拆开写 tick_resolved (public) / tick_audit (orchestrator-only) → 对所有合法 intended_action 派生 action.intent。

## 涉及文件

### 修

- `Source/AILiveProject/Public/Memory/AILiveEventStoreSubsystem.h` —— 加 `ListBidsForTick(tickNo)` + `ResolveFloor(tickNo, eligibleAgents, coldThreshold)`
- `Source/AILiveProject/Private/Memory/AILiveEventStoreSubsystem.cpp` —— 上述实现 + `ComputeRuntimeAdjustment`（反霸麦衰减 + 被 @ 加权 + 长时间没说话小幅加权）
- `Source/AILiveProject/Private/Acts/Act02RuleReceiveDirector.cpp` —— 主循环改为 `RunTick()`：
  1. `Store->BeginTick(++CurrentTickNo)`
  2. 并行调每个 in-scene agent 的 LLM → `RunAgentTick(Cfg)` 内部跑 **Reasoner→Parser 双 LLM 串联**：
     - 2a. 调 `OpenAIChat::RequestBlocking` 跑 Reasoner（参赛 LLM，按 Cfg.Provider 选 endpoint）→ 拿 raw text
     - 2b. 调 `AILiveParser::ParseFourChannels(raw, agentId)`（T2.5 落地）→ 4 段结构化结果
     - 2c. 失败 → reject sample 重试 Reasoner 最多 **3 次**（principles §5.4）；3 次仍失败 → 用 `AppendEvent` 写一条 `EAILiveEventType::SystemParseFailed`，**`actor=<具体 agent ID>`**（如 `"NPC03"`，标识哪个 agent 解析失败），**`payload.failure_source="parser_llm"`** 子字段（区分于 EventStore 静态校验失败的 `failure_source="eventstore_validation"`），payload 含原 raw text 截断 256 字符 + Parser ErrorReason。该 agent 该拍**视为 abstain**（不写 4 通道事件，bid 视为 0 不参与裁决）
     - **两种 SystemParseFailed 的区分约定（与 T3 的 `AppendSystemParseFailure` 协调）**：
       - `actor=<NPC ID>` + `payload.failure_source="parser_llm"` ← Parser LLM 输出非法 JSON / 缺段（**本步路径**）
       - `actor="system"` + `payload.failure_source="eventstore_validation"` ← visibility/payload 静态校验失败（T3 兜底路径）
       - 两路径共享同一 enum 但 actor 字段 + payload.failure_source 子字段精确二分；任何下游统计 SQL 必须按 `payload.failure_source` 过滤，**不**仅按 actor 字段
     - 2d. 成功 → 用 `AppendEventsAtomically` 一次提交 4 条事件（scratchpad + intended + bid + note）
  3. `Store->ResolveFloor(ThisTickNo, EligibleIds, 3.0f)` 拿 winner（abstain 的 agent 因为没写 bid 自动不参与）
  4. 若非冷场：取 winner intended → ListenerFilter::Apply → 写 speech.public（parent_event_id 指向 winner intended）
  5. `QuoteByEventTypeAndTick(SpeechIntended, ThisTickNo)` → 对每个含 intended_action 字段的 intended 派生一条 action.intent（visibility 展开为具体 actor ID，**禁用 "self"**）
  6. 写 OrchestratorTickResolved (visibility=public, payload 仅含 winner / cold info, **不含 all_bids**)
  7. 写 OrchestratorTickAudit (visibility=orchestrator, payload 含 all_bids + filter_decision + parent_tick_resolved_seq)
  8. 异步 `RebuildProjections()`（pending_intended 投影刷新放 T8；T6 PromptAssembler 不依赖此投影所以不影响下一拍）

### 新增

- `Source/AILiveProject/Public/Memory/AILiveListenerFilter.h` + `.cpp` —— `namespace ListenerFilter { FString Apply(const FString& IntendedPayload); }` MVP 直通实现 + comment 标注下阶段引入真 LLM

## 依赖

blockedBy = [T2.5](T2_5_parser_llm_client.md), [T5](T5_director_writeback.md), [T6](T6_prompt_assembler.md)

## 验收方式

对齐 §11 **L1 + L2** 多项：

- L1「一拍单一 winner」：一拍 10 个 bid 中只有一个 actor 衍生出 speech.public；对应 tick_resolved 的 winner_actor 与 derived_public_seq 配对正确
- L1「speech.public 衍生关系」：T7 重写后产生的每条 speech.public 的 parent_event_id 指向一条 speech.intended，且二者 actor / round 一致。**SQL 必须过滤 T5 过渡形态**：`SELECT ... FROM events WHERE event_type='speech.public' AND json_extract(payload,'$.legacy_pre_bid') IS NULL` —— 满足条件的 speech.public 全部有 parent_event_id 指向 speech.intended
- L1「冷场推进」：全部 agent bid < 3.0 时 tick_resolved.winner_actor=""，无 speech.public 衍生
- L1「bid 隔离」：NPC04 调 `Quote(bid_seq, "NPC04")` 取另一个 NPC 的 bid → 不可见
- L2「反霸麦衰减」：单 agent 连续 4 拍最高 bid → 第 5 拍 RuntimeAdj 使其 FinalScore 落后于次高；非该 agent 抢中
- L2「被 @ 加权」：t 拍 addressed_to=["NPC07"] → t+1 拍 NPC07 bid 享 +2.0
- L2「intended_public_divergence」：构造一对（intended="我是 Seer" / public="我没什么想说"）→ BEL_EXT 第 9 项触发 flagged_only（注：BEL_EXT 9 项 evaluator 整体不在 MVP 范围，但本项触发标记应记录到 bel_violations 表的占位）
- L2「bid 阶段超时」：某 agent bid 调用超时 → 该 agent 该拍 bid 视为 0；不影响其他 agent
- L2「同拍 tick_no 切片」：单拍 10 agent × 4 事件 + 3 条 SystemLLMInflight 穿插 → `WHERE tick_no=N AND event_type='bid'` 精确返回 10 条
- L2「tick_resolved / tick_audit 拆分」：任一 NPC 调 `Quote(tick_audit_seq, "NPC03")` 不可见；`Quote(tick_resolved_seq, "NPC03")` 可见且 payload 不含 all_bids
- L2「action.intent 对所有 agent 派生」：一拍 3 agent 写 intended_action 但只 1 人抢中 → action.intent 表写入 3 条；3 条 visibility 各为各自 actor
- L2「payload redaction fuzz」：随机 100 条 tick_resolved/tick_audit/bid 事件 → 遍历每个 NPC 视角 Quote → all_bids 字段 0 次出现
- **新增「Parser 解析失败 → SystemParseFailed + abstain」**：故意 mock 让 NPC03 的 Reasoner 输出缺 `<BID>` 段 → 3 次 reject sample 后查 `SELECT COUNT(*) FROM events WHERE tick_no=N AND actor='NPC03' AND event_type='system.parse_failed' AND json_extract(payload,'$.failure_source')='parser_llm'` 应当 = 1；同 tick_no 下查 `SELECT COUNT(*) FROM events WHERE tick_no=N AND actor='NPC03' AND event_type IN ('speech.scratchpad','speech.intended','bid','speech.note')` 应当 = 0（无 4 通道事件）；其他 agent 正常进入 ResolveFloor，winner 仍能产生（abstain agent 不阻塞流水）
- **新增「Parser 100% 解析率烟测」**：跑一整局 ACT02 → `SELECT COUNT(*) FROM events WHERE event_type='system.parse_failed' AND json_extract(payload,'$.failure_source')='parser_llm'` 应当 = 0（Parser 期望 100% 解析率）；若 > 0 记录 SystemParseFailed 频次到 DevLog 作为 Parser 调优基线。**注**：本 SQL 只统计 Parser 来源；EventStore 静态校验失败由 T3 单独覆盖

## 风险点

- `RunAgentTick` 内并行调 LLM 但写入路径串行（WriteMutex 已在 T3 落地）—— 性能瓶颈在 LLM 延迟，不在 SQLite
- `QuoteByEventTypeAndTick` 必须按 `idx_events_game_tick` 索引列直读，**不**依赖 seq 范围分组（impl §7bis 反复强调；多 LLM 并发返回 + in-flight + action.intent 派生交叉时 seq 必然被穿插）
- **Parser 双 LLM 串联引入两次 LLM round-trip 的延迟叠加**——每 agent 每拍 = Reasoner latency + Parser latency；MVP 必须监控单拍总延迟，若超出可接受范围（建议 < 5s），考虑 Parser 用更小模型 / 本地推理；reject sample 触发时延迟更高，3 次后该 agent 该拍 abstain（principles §5.4 已规约）
- **Parser 选型与 self-play 风险**：T2.5 已决定 Parser 用 GLM 或 Qwen3 中等规模（与 Reasoner 不同模型）；若同一局 Reasoner 与 Parser 用同一 provider，PRD「同模型全员 self-play 风格匹配抱团」反模式可能扩散到解析层 —— T7 跑前 grep 当前 Roster 配置，确认 Parser 选用模型不在 Reasoner 池里
- `EAct02State` 既有状态机仍然有效（Idle / PrescatterToTV / SeedDispatch / SeedAwait / SeedSpeak / ...）—— 这些不进 events 表（impl §4.5 明确保留），RunTick 是 SeedSpeak / Reaction 阶段内部的细粒度循环
- `OrchestratorTickAudit.parent_event_id` 必须指向同拍 OrchestratorTickResolved 的 EventId —— Director 写完 TR 后立即拿到 TR.EventId 再写 TA
- 与 CLAUDE.md 关键规则「不要修改 `bTickPhysicsAsync` / `DefaultBuildSettings = V6`」无关；本步只改 Director 主循环 + EventStore 接口

## 里程碑 DevLog

`DevLog/2026-MM-DD_runtick_bid_parser.md`，记录主循环重写细节 + Reasoner→Parser 串联结果 + 反霸麦/被@加权调参 + Parser 解析失败统计

## 完成定义

tick 驱动主循环跑通 + Reasoner→Parser 双 LLM 串联 100% 工作 + bid 隔离 + 衍生 public 正确 + 派生 action.intent 完整 + 视角隔离的 audit 不泄露 + 里程碑 DevLog 入库。**不**意味着投影已重建（T8）。
