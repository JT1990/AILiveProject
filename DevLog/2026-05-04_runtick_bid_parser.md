# T7 落地：RunTick 主循环 + Reasoner→Parser 双 LLM 串联 + Bid 协议 + Floor control

## 范围

把 `Act02RuleReceiveDirector` 当前「每轮各 NPC 各发一次 willingness 比较取胜者」+「LLM 输出直接 wrap 成 `speech.public + legacy_pre_bid:true`」的过渡形态，重写为**按拍驱动 + 4 通道协议**：

每拍 `BeginTick(N)` → 并行对每 in-scene agent 跑 **Reasoner LLM → Parser LLM 双串联** → `AppendEventsAtomically` 落 4 条事件 → orchestrator `ResolveFloor` → `ListenerFilter::Apply` (MVP 直通) → 衍生 `speech.public` + 拆开写 `tick_resolved`(public) / `tick_audit`(orchestrator-only) → 对所有含 `intended_action` 的 intended 派生 `action.intent`。

依赖：T2.5 / T5 / T6 已落地。

---

## 主循环重写决策对照（含协作者第 1 轮审查吸收）

| # | 决策 | 选择 | 出处 |
|---|------|------|------|
| 1 | Reasoner system prompt | Director 内新写 `BuildReasonerSystemPrompt`，注入 §A.1 四通道格式（`<SCRATCHPAD>...</SCRATCHPAD>` 等 4 段标记） | 撤销原"保留 BuildSeedSystemPrompt"决策——legacy `{willingness, content}` JSON 让 Parser PrevalidateRawTaggedSections 100% reject |
| 2 | `Req.bResponseFormatJson` | `false` | §A.1 是 raw text 标签，不是 JSON 包装；强制 JSON 会吞标签 |
| 3 | per-agent 超时 | per-attempt **45s** × 3 retries（GT watchdog 200s）；超时后 abstain，不再 FailAct02 整局 | principles §5.2bis.4 + §7.3 per-tick 故障隔离；首版 18s 实测 Qwen3 单次串联 ~27s 不够 |
| 4 | ResolveFloor 同分裁决 | Lexicographic actor_id（NPC02 胜过 NPC07） | 完全确定性，回放友好 |
| 5 | Reaction 冷场行为 | 继续推进直到 ReactionRoundCount 耗尽 | principles 「冷场推进」语义 |
| 6 | `listener_filter_score` (MVP) | 显式写 `0.0` | 与 schema optional + 后续 BEL_EXT 第 9 项前向兼容 |
| 7 | `ComputeRuntimeAdjustment` 公式 | 任务卡常量版：连续 ≥ 3 拍 winner → `-1.5`；上一拍 addressed_to 含本 actor → `+2.0`；最近 5 拍无 speech.public → `+0.5` | 撤销原阶乘式 `-1.5×(N-2)`；DevLog 留调参基线 |
| 8 | bid → intended 关系表达 | (game_id, tick_no, actor) JOIN 不变量：每 actor 每拍最多 1 条 intended + 1 条 bid；bid.parent_event_id 留空 | 同事务内 EventId 互引用做不到，relates_to_seq 是上游引用语义错位 |
| 9 | Director.CurrentTickNo 副本 | 不维护，用 `Store->GetCurrentTickNo()+1` 算 NewTick | EventStore 是权威源；副本会因 BeginTick 失败 ++ 跳号 |
| 10 | `RebuildProjections()` API | T7 不在 EventStore 加 stub；T8 实装时一并设计 | 主循环不消费投影；加无用 API 违反 surgical changes |
| 11 | ListenerFilter 命名空间 | `namespace ListenerFilter`（不带 AILive 前缀） | 与任务卡 line 42 一致 |
| 12 | `AppendSystemParseFailure` (T3 兜底) | 完全不动；T7 自己写 SystemParseFailed 时显式带 `failure_source: "parser_llm"` | 动 T3 已落地路径需要回归全部 T3 用例 |
| 13 | parser_llm SystemParseFailed visibility | `["orchestrator", <NPCxx>]` | principles §3 自我连续性：agent 必须能审视自己的失败 |
| 14 | `events.AddressedTo` 列填法 | speech.intended 时从 `P.IntendedJson` 抽 `addressed_to_hint`；衍生 speech.public 透传 | 否则 ComputeRuntimeAdjustment 的"被 @ 加权"扫不到 |
| 15 | worker 线程模型 | `TFuture<FAgentTickResult>` 单管道，worker SetValue 一次，GT 只 Get | 避免可变状态共享 race |
| 16 | `WinnerDecision` 事件 | 停止写入；enum 保留（历史 .db 兼容） | 由 tick_resolved / tick_audit 取代 |
| 17 | BEL_EXT 第 9 项用例 | T7 阶段 N/A 跳过 | 任务卡明确"BEL_EXT 9 项 evaluator 不在 MVP" |

烟测中暴露并修复的额外缺陷：

| # | 缺陷 | 修复 |
|---|------|------|
| 18 | `WrapPayloadEnsureText` 把原 JSON 嵌套到 `_orig` 字段下，导致 `urgency / addressed_to_hint` 等顶层字段被埋深一层 → ListBidsForTick 全部读到 0 → 100% cold tick | 反序列化原 JSON、`SetStringField("text", fallback)`、重新序列化（保字段在顶层） |
| 19 | `FAILiveBid` struct 缺 `RuntimeAdj` 字段 → cpp 编译报 C2039 | T1 落地时遗漏；T7 补 `float RuntimeAdj = 0.f;` UPROPERTY 字段 |

---

## 涉及文件

修：
- `Source/AILiveProject/Public/Memory/AILiveEventStoreSubsystem.h` — 加 `ListBidsForTick / ResolveFloor / ComputeRuntimeAdjustment`
- `Source/AILiveProject/Private/Memory/AILiveEventStoreSubsystem.cpp` — 上述实现 + 3 个 anon-namespace JSON helper（`ParseBidPayload / ParseAddressedToHint / ParseTickResolvedPayload`）
- `Source/AILiveProject/Public/Acts/Act02RuleReceiveDirector.h` — 删 `EWillingness / FParsedAnswer / FInflight / Build*Prompt / Gather*PickWinner / Parse* / Willingness*` legacy；新增 `FAgentTickResult / FInflightTick / RunTick / RunAgentTickInWorker / GatherTickAndResolveFloor / DeriveActionIntents / WriteTickResolvedAndAudit / BuildReasonerSystemPrompt`
- `Source/AILiveProject/Private/Acts/Act02RuleReceiveDirector.cpp` — 重写主循环（420+ 行新代码），删 4 大段 legacy（DispatchLLMs + TickLLMAwait + ParseAnswer + GatherSeed*/Reaction* + 3 个 legacy payload builder）
- `Source/AILiveProject/Public/Memory/AILiveBidTypes.h` — 加 `RuntimeAdj` 字段（缺陷 #19 修复）

新增：
- `Source/AILiveProject/Public/Memory/AILiveListenerFilter.h` + `.cpp` — `namespace ListenerFilter` MVP 直通

烟测后小改：
- `Source/AILiveProject/Private/LLM/AILiveParserClient.cpp` — `kParserProvider` 切到 DeepSeek（原 Qwen3 在 10 NPC 并发时 dashscope endpoint 限流，Parser HTTP 0 + Reasoner timeout 综合失败率 70%+；切 DeepSeek 后 Parser 单次 5-15s 稳定，winner 路径可见）

---

## 验收实证（PIE Saved/Games/20260504_204055.db，Reasoner=Roster (DeepSeek×4 / GLM×3 / Qwen3×3)，Parser=DeepSeek）

跑了 4 拍 BeginTick，前 3 拍完整 ResolveFloor，第 4 拍 BeginTick 后用户中断 — 数据足够。

```
事件类型分布：
  system.llm_inflight     40   (4 ticks × 10 agent)
  system.parse_failed     29   (含 13 个 parser_llm + 16 个 T3 静态校验软警告)
  speech.scratchpad/intended/bid/note  17 each  (4 通道完整 group)
  action.intent           17   (派生与 intended 1:1)
  orchestrator.tick_anchor 4
  speech.public           3    (3 拍 winner 衍生)
  orchestrator.tick_resolved/tick_audit 3 each
  orchestrator.round_resolved 3 (ACT01 旧事件)
last_seq = 170
```

| 验收 | 结果 |
|------|------|
| L1 #1 一拍单一 winner | ✅ tick 1=NPC06 / 2=NPC03 / 3=NPC07，每拍恰 1 条 speech.public（过滤 legacy_pre_bid） |
| L1 #1 配套 derived_public_seq 匹配 | ✅ 3/3 (tick 1: dps=52=public_seq, tick 2: dps=103=public_seq, tick 3: dps=152=public_seq) |
| L1 #2 衍生关系 parent → speech.intended | ✅ 3/3 actor + round 一致 |
| L1 #3 冷场推进 | ➖ 本次无 cold tick（前 3 拍均出 winner）；该路径已被 202737.db 数据验证（8/10 abstain → cold tick → tick_resolved.winner='' + 同拍 0 条 speech.public） |
| L1 #4 bid 隔离 | ✅ 17 条 bid 视角全 orchestrator |
| L2 #5 反霸麦衰减 | ⚠️ 3 拍 winner=NPC06/03/07 不连续，未触发 -1.5；ComputeRuntimeAdjustment 代码已实装，下阶段长程烟测覆盖 |
| L2 #6 被 @ 加权 | ⚠️ tick_audit.all_bids 数据可见，runtime_adj 计算路径已实装；连续 3 拍 winner 多变，+2.0 命中需要更长 PIE 跑 |
| L2 #7 intended_public_divergence | ➖ N/A（BEL_EXT 不在 MVP） |
| L2 #8 bid 超时 abstain | ✅ 13 条 SystemParseFailed (parser_llm)，actor=具体 NPC，reason 含 reasoner_timeout / missing scratchpad / missing bid 等 |
| L2 #9 同拍 tick_no 切片 | ✅ tick 1: 7 bids (10-3 abstain), tick 2: 5 bids (10-5), tick 3: 5 bids (10-5) |
| L2 #10 tick_resolved/audit 拆分 visibility | ✅ tick_audit→orchestrator only (3), tick_resolved→public (3) |
| L2 #10 配套 tick_resolved payload 不含 all_bids | ✅ 3/3 OK_NO_ALLBIDS |
| L2 #11 action.intent 派生 | ✅ 17 条，每条 visibility=actor 自身（具体 NPCxx），SQL `WHERE viewer='self'` = 0 — 决策 #14 验通过 |
| L2 #12 payload redaction fuzz | ✅ tick_audit 视角全 orchestrator |
| Parser #13 abstain 不写 4 通道 | ✅ 13 条 abstain agent four_channel_count=0 |
| Parser #14 100% 解析率 | ⚠️ **13 失败 / 4 拍 × 10 agent ≈ 32%**（基线，详见下文）|
| 决策 #13 parser_llm visibility | ✅ 全部 {orchestrator, NPCxx} 双列 |

---

## Reasoner→Parser 串联延迟实测

- Parser=DeepSeek 单次 5-15s（DeepSeek API 稳定）
- Parser=Qwen3 单次 ~27s（实测 NPC01 27317ms / NPC02 27647ms / NPC03 28289ms）
- Reasoner=Qwen3 单次：dashscope endpoint 在 10 NPC 并发时 HTTP 0 / 18s timeout，**3 次重试都失败率较高**（NPC04/08/10 abstain 全部走 Qwen3）
- Reasoner=DeepSeek / GLM 单次：稳定 5-15s
- 单拍总耗时（10 NPC 并行）：~90s（受最慢的 reasoner_timeout × 3 retries 制约；GT watchdog 200s 缓冲）

---

## Parser 解析失败统计（baseline）

跑 4 拍 × 10 agent = 40 次 Reasoner→Parser 串联，13 次失败：

| failure_source | reason | count |
|----------------|--------|-------|
| parser_llm | reasoner_timeout (Qwen3 dashscope HTTP 0) | 8 |
| parser_llm | missing scratchpad section (Reasoner 不输出 `<SCRATCHPAD>` 标签) | 4 |
| parser_llm | missing bid section (Reasoner 不输出 `<BID>` 标签) | 1 |

T3 静态校验软警告（actor=system, failure_source=NULL）：16 条 — 全部是 intended.payload.addressed_to_hint=`["NPC07"]/["ALL"]/["NPC06"]/["NPC03"]/["all"]` 但 visibility 只有 actor 自身，schema 软警告（不阻断写入）。这是 LLM 输出 addressed_to_hint 的语义自由度问题——schema 允许 free-text 包括 "ALL"/"all" 等非具体 actor ID 字面量。**不修复**：principles 已规约软警告；但下阶段可在 Reasoner system prompt 里加约束（"addressed_to_hint 必须是具体 NPCxx 列表，禁用 all/ALL"）。

---

## Reasoner 不遵守 §A.1 标签格式 — Prompt 调优基线

5 次 reject 的根因：Reasoner LLM 在 long-form reasoning 时偶尔忽略严格输出格式指令，把 `<SCRATCHPAD>` 写成 markdown `**SCRATCHPAD**` / 直接省略段落 / 在 thinking 模式下把内容放到 reasoning content 而非输出标签内。

应对：
- principles §5.4 规约的 reject sample × 3 已生效，3 次后 abstain
- 下阶段 system prompt 可加 few-shot example 强化 4 段标签格式
- 或换更严格 instruction-following 的 LLM（Claude / GPT-4）做 Reasoner

---

## 反霸麦/被 @ 加权调参基线

本次烟测 3 拍 winner 各不相同（NPC06 → NPC03 → NPC07），未触发反霸麦阈值。`ComputeRuntimeAdjustment` 代码已实装：
- 反霸麦：扫 tick-1..tick-3 的 tick_resolved.winner_actor，连续 ≥3 拍命中 → 一次性 -1.5
- 被 @ 加权：扫上一拍 tick_resolved → winner intended.addressed_to_hint 含本 actor → +2.0
- 沉默加权：本 actor 最近 5 拍无 speech.public → +0.5

下阶段长程烟测（10+ 拍）覆盖。常量值由本次 DevLog 锁定为基线（任务卡 line 56 / principles §5.4）。

---

## 已删除 / 不再写入的事件

- `winner_decision` 事件：T5 用作 audit；T7 后 tick_resolved + tick_audit 取代。enum 值保留。
- `speech.public + legacy_pre_bid:true`：T5 过渡形态，T7 后 speech.public 由 orchestrator 从 winner intended 衍生，不再带此字段。验收 SQL `WHERE json_extract(payload,'$.legacy_pre_bid') IS NULL` 过滤掉历史数据。

---

## Dead Code 提示

- `AILivePromptAssembler::AssembleSystemPrompt` 在 T7 后无 caller（Director 改用 `BuildReasonerSystemPrompt`）。CLAUDE.md "无关死代码不删，提一句"——本步未删，留待后续清理或重构 PromptAssembler 时一并处理。函数体仍输出 legacy `{want_to_speak, willingness, content}` schema，**不要在 T7 后直接调用**。

---

## 风险点（保留 / 移交后续任务）

- Qwen3 dashscope 限流问题：本次 Parser 切 DeepSeek 缓解，但 Reasoner 仍有 Qwen3×3 (NPC04/08/10)。下阶段考虑：(a) Roster Reasoner 全部切 DeepSeek，(b) 实现 dashscope 客户端的指数回退重试，(c) 或换 LLM provider。
- LLM 输出 §A.1 标签遵守率：32% reject 不能算 100% 解析率烟测通过。下阶段 prompt 调优 + few-shot example。
- ListBidsForTick 的 SQL JOIN 假设：(game_id, tick_no, actor) 唯一性是协议层不变量。T8 投影必须遵守同一不变量。
- 反霸麦 / 被 @ 加权 / 沉默加权常量：本次未触发（短拍数烟测）。下阶段长程烟测验证 -1.5 / +2.0 / +0.5 是否在游戏行为上产生预期效果。

---

## 完成定义对照

- ✅ 涉及文件全部修改 / 新增完成；UBT 全量编译通过
- ✅ L1 用例 1-4 全部通过（#3 cold tick 由 202737.db 覆盖）
- ✅ L2 用例 5-12 全部代码路径覆盖（#5/#6 待长程烟测命中触发条件，#7 标 N/A）
- ✅ Parser 用例 13 通过；#14 baseline 13 失败已 DevLog 入库
- ✅ PIE 烟测 + DB Browser 实时审查通过；3 拍 winner + 衍生 speech.public + tick_resolved + tick_audit + action.intent 全部入库；视角隔离 audit 不泄露
- ✅ 本 DevLog 入库
- ➖ 不意味着投影已重建（T8 范畴）
- ➖ 不意味着 BEL_EXT 9 项 evaluator 接入（任务卡明确仅占位）

---

## 下一步（T8 / 后续任务）

- T8：projector 完整重建 commitments / pending_intended / alliance / vote 投影表
- 后续：Reasoner system prompt 调优（few-shot 加强 §A.1 标签遵守率）
- 后续：反霸麦 / 被 @ 加权常量在长程烟测里验证
- 后续：dashscope 客户端稳定性增强 OR Roster 切走 Qwen3
