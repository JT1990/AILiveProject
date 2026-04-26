# T22 — 私聊 hearing 推送 + 4 个少数决 Action

## 目标
实现 4 个少数决专属 Action（AskQuestion / Vote / ProposeAlliance / AcceptAlliance）+ Speak channel 扩展（公开广播 vs 私聊）。偷听机制：`GM.RecordSpeechEvent` 经 `Perception.CanSenseActor(Hearing)` 过滤后主动 push `[偷听]` / `[公开]` 消息到 `listener.Context`（Layer 2 hot），不调 `MemoryClient.Write`（event 写入由发言者 `HandleActionDone` 一次完成）。

## 前置
T21（GM 阶段机 + 全局 LLM budget）+ T19（通用动作骨架）+ T11（GM 基类含 Validate/Apply 拆分）+ T18（AI Perception 集成）+ T09（ContextManager 已就位）

## DoD

### A. 4 个少数决 Action（保留原 T22 核心，迁入）
- [ ] `UMindAction_AskQuestion`：解析 `{question_text}` → Speak 配音说出问题 → Done(true) → **GM.Apply 写入 `GM.State.CurrentQuestion` + 广播 OnGameEvent**（Action 不直接改 state）
- [ ] `UMindAction_Vote`：解析 `{target_agent_id}` → EQS 找投票箱 SO → ApproachAndUse → Done(true) → GM.Apply 写票
- [ ] `UMindAction_ProposeAlliance`：解析 `{target_agent_id, terms}` → Speak 私聊 target → Done(true) → GM.Apply 记 proposal
- [ ] `UMindAction_AcceptAlliance`：解析 `{proposal_id, accept_bool}` → Speak 公开 → Done(true) → GM.Apply 改 alliance state

### B. Speak channel 扩展（公开 vs 私聊）
- [ ] `UMindAction_Speak` 加 `channel` 参数：`public` / `private:<target_agent_id>`
- [ ] Action.Execute 仍只触发 TTS + Done(true)；channel 解析交给 GM.RecordSpeechEvent

### C. GM.RecordSpeechEvent 推送流程（**核心新机制**）
- [ ] `AMindGameMaster::RecordSpeechEvent(speaker, channel, content, listeners[])` 由 Speak Action 在 HandleActionDone 内调
- [ ] listeners 由 Perception hearing 物理过滤产生（用现有 `bCanSenseActor(Hearing)` API）
- [ ] 对每个 listener，按以下规则 push 到 `listener.MindComponent.Context.PushUser(...)`：
  - 自己说的：assistant role 已经覆盖（Context.PushAssistant），不重复
  - **公开发言**（channel=public）：所有听到的 listener 收到 user role `[公开] NPC_X 在 ts=T 说: "..."`
  - **私聊接收方**（channel=private 且 listener 是 target）：user role `[私聊] NPC_X 对你说: "..."`
  - **偷听者**（channel=private 且 listener 不是 target，但在 hearing 范围内）：user role `[偷听: NPC_X→NPC_Y 私聊: "..."]`
  - **范围外**：不 push
- [ ] **完全不调 MemoryClient.Write**（event 写入由发言者自己一次 fire-and-forget 在 HandleActionDone 写）
- [ ] push 入口校验 `listener.AgentId == listeners[i]`（防 R6 跨 NPC buffer 泄露）

## 关键文件
- 修改 `AMindGameMaster.h/.cpp`（RecordSpeechEvent）
- 修改 `UMindAction_Speak.h/.cpp`（channel 参数）
- 新增 `UMindAction_AskQuestion.h/.cpp` / `UMindAction_Vote.h/.cpp` / `UMindAction_ProposeAlliance.h/.cpp` / `UMindAction_AcceptAlliance.h/.cpp`
- 修改 `AMindGameMaster_MinorityRule.h/.cpp`（Apply 改 GM state + register Action 类）

## 验收信号
1. 4 NPC 场景，A 公开发言：所有 listener.Context 出现 `[公开] A 说: ...`；不在 hearing 范围的 NPC（如墙后）不出现
2. A 对 B 私聊：B.Context 出现 `[私聊]`；hearing 范围内的 C.Context 出现 `[偷听]`；hearing 范围外的 D.Context 不出现
3. Memory Service 上 event log 中只有发言者一条 event 记录（不是每个 listener 一条）
4. Action.Execute 不直接改 GM state（验证 Validate/Apply 拆分照旧）
5. EQS Vote 流程跑通：LLM 输出 `Vote(target=npc_3)` → EQS 选投票箱 → ApproachAndUse → GM.Apply 写票

## 不在范围
- 多轮投票淘汰循环（T24）
- 联盟可视化 HUD（T25）
- 跨厂商 hearing 表达（T18.5）

## 风险
- R6 跨 NPC buffer 泄露：push 入口校验 + 单元测试覆盖
- hearing 距离参数（Sense Range）需要 T18 已配置好；本卡不调
- 私聊频次过高时 Context 膨胀快：靠 Compact 兜底（不在本卡）
