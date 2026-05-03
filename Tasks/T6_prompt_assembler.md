# T6 — `AILivePromptAssembler`

> 总览：[00_overview.md](00_overview.md)
> 依赖：blockedBy = [T4](T4_eventstore_read_api.md)（读 API；**注意**：依赖图上不再 blockedBy T8——pending_intended 走最小化路径，T8 的投影表是后续优化）
> 改动量：**中**（1–4h）
> §11 验收：L1「pending_intended 注入」

## 预检

`Act02RuleReceiveDirector` 当前的 prompt 构造路径用 Read/Grep 查 .cpp（C++ 状态）；本步**不需要** Monolith MCP。

## 目标

把 `memory_principles.md` §4.3 prompt 拼装优先级落到代码——自我发言段（永不压缩）+ pending_intended 段（最近 N 拍未抢中 floor 的 intended）+ 自我承诺投影 + 公开发言近场窗口 + 私聊段 + 当前拍提示。质问场景预先 prefetch 引用。**关键依赖修订**：pending_intended 段直接走 T4 的 `ListMyPendingIntended`（events 表 + parent 链状态最小化路径），**不**等 T8 的完整投影表 —— 解开协作者指出的依赖错位。

## 涉及文件

### 新增

- `Source/AILiveProject/Public/Memory/AILivePromptAssembler.h` —— `namespace AILivePromptAssembler` + `FAssembleOptions` + `AssembleSystemPrompt` / `AssembleUserPrompt`
- `Source/AILiveProject/Private/Memory/AILivePromptAssembler.cpp` —— 上述实现 + 内部辅助 `AppendOwnHistorySection` / `AppendPendingIntendedSection` / `AppendCommitmentsSection` / `AppendNearWindowSection` / `AppendChallengePrefetchSection` + `ExtractText(payloadJson)` / `ExtractRoundRefs(challengeText)`

### 修

- `Source/AILiveProject/Private/Acts/Act02RuleReceiveDirector.cpp` —— 把现有 prompt 构造改为 `AILivePromptAssembler::AssembleSystemPrompt(Store, Cfg, Opt)` + `AssembleUserPrompt(...)`

## 依赖

blockedBy = [T4](T4_eventstore_read_api.md)（读 API；**注意**：依赖图上不再 blockedBy T8——pending_intended 走最小化路径，T8 的投影表是后续优化）

## 验收方式

对齐 §11 **L1 烟测**：

- L1「pending_intended 注入」：手工构造场景 → 调 `Store->AppendEvent` 写一条 NPC04 的 speech.intended（不写对应 speech.public）→ 调 `AssembleUserPrompt(NPC04, ...)` 返回的字符串中包含 `[YOUR RECENT INTENDED-BUT-NOT-SAID]` 段 + 该条 intended 原文。**不需要**先跑 RunTick 或重建投影
- 手工：发起一个含"第 3 轮"引用的 challenge prompt → AssembleUserPrompt 自动 prefetch 第 3 轮所有可见事件插入 prompt
- 长度检查：`[YOUR OWN COMPLETE STATEMENT HISTORY]` 段对自我发言永不压缩；超 prompt 上限时按 §4.3 硬规则降级（先丢与自己无关私聊 → 移除非自己产出私有推理 → 把第 cur-K 拍移到摘要段；**永远不丢**自己发言/公开发言/自己未抢中 intended）

## 风险点

- "自我发言永不压缩"是底层不变量——实现里要写明断言（pseudocode：`check(SelfStatementsTokens < kPromptCtxLimit)`）；超限时不能默默丢，应抛出并 UE_LOG Error 让上层处理
- PromptAssembler 不应调任何 LLM —— 所有降级逻辑都是确定性规则
- `tick_no` 在 prompt 段落中怎么呈现：implementation §7 模板里 `Tick 042 round_no=03` 的 tick 编号来自 events.tick_no，PromptAssembler 必须查这一列

## 完成定义

ACT02 跑一遍后 prompt 中能看到自我发言段 + pending_intended 段。**不**意味着 RunTick 主循环已重写（T7）。
