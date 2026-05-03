# T4 — 读 API + 视角隔离 JOIN

> 总览：[00_overview.md](00_overview.md)
> 依赖：blockedBy = [T3](T3_eventstore_write_path.md)
> 改动量：**中**（1–4h）
> §11 验收：L1「视角隔离 / 系统事件不泄露 / FTS5 模糊」+ 新增「ListMyPendingIntended 不依赖投影表」

## 预检

纯 C++ + DB，**不需要** Monolith MCP；视角隔离用例靠 console command + DB Browser。

## 目标

把 events + event_visibility + event_addressed_to + 投影表 的查询路径暴露为 BlueprintCallable / 内部方法，所有读路径都通过视角隔离 JOIN（不靠上层"自觉"过滤 payload 字段）。**额外**：`ListMyPendingIntended` 走"events 表 + parent 链状态"的最小化路径（找出所有 actor 自己的 speech.intended 中没有任何 speech.public 的 parent_event_id 指向它的），**不依赖 T8 的完整投影表**——保证 T6 PromptAssembler 不会被依赖锁死。

## 涉及文件

### 修

- `Source/AILiveProject/Public/Memory/AILiveEventStoreSubsystem.h` —— 暴露所有读 API（声明在 T2 已写）
- `Source/AILiveProject/Private/Memory/AILiveEventStoreSubsystem.cpp` —— 实现：
  - `Quote(seq, viewer)`、`QuoteByRound(round, actor, viewer)`、`QuoteRecentRounds(currentRound, K, viewer)`
  - `ListMyStatements(agentId)`、`ListMyNotes(agentId, n)`、`ListMyReflections(agentId, n)`、`ListMyPendingIntended(agentId, n)`
  - `ListMyCommitments(agentId, roundStart, roundEnd)`（投影表直读）
  - `SearchHistory(keyword, actor, roundStart, roundEnd, viewer, limit)` —— FTS5 + visibility JOIN
  - `ListVotes(roundNo)`、`ListAllianceStateJson()`
  - `QuoteByEventTypeAndTick(eventType, tickNo)` —— 内部用，T7 派生 action.intent 时按 tick_no 切片

## 依赖

blockedBy = [T3](T3_eventstore_write_path.md)

## 验收方式

对齐 §11 **L1 烟测** 视角相关项：

- L1「第 N 轮原文质问」：`QuoteByRound(N, "NPC03", "NPC03")` 返回准确原文（**注**：viewer 参数永远是具体 actor ID 字符串，**禁止** "self"——`ValidateVisibility` 在写入侧已拒绝，读取侧 API 同样不接受 "self" 字面值，传入即返回不可见）
- L1「视角隔离」：NPC04 调 `Quote(seq, "NPC04")` 取 visibility=`["NPC07"]` 的事件 → 返回不可见标记
- L1「系统事件不泄露」：NPC03 调 `Quote(seq, "NPC03")` 取 SystemLLMInflight 事件 → 不可见
- L1「FTS5 模糊」：`SearchHistory("结盟", ...)` 命中含"结盟"或形近字的发言（FTS5 trigram tokenizer；T2 自检若降级到 unicode61，本步加 UE_LOG Warning 提示能力受限）
- **新增「ListMyPendingIntended 不依赖投影表」**：跑一段 ACT02 → 直接 `DROP TABLE agent_view_state`（T8 的投影表）→ 调 `ListMyPendingIntended("NPC04", 5)` 仍返回正确结果（验证 T4 路径不走投影）

## 风险点

- `Quote(seq, viewer)` 不可见时返回值约定要明确——建议返回 `bool` 函数值 + `OutEvent` 仅在可见时填值；调用方必须查返回值
- FTS5 模糊在中文场景的字符切分行为：trigram 对汉字的覆盖比 unicode61 好但仍非完美；上层调用方不应假设"语义匹配"
- JOIN 性能：`event_visibility` 索引已在 T2 建好（`idx_event_visibility_viewer`），不应有 N+1 问题

## 完成定义

所有读 API 通过视角隔离用例 + FTS5 用例。**不**意味着 PromptAssembler 已就绪（T6）。
