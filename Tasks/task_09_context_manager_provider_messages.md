# T09 — UMindContextManager + Provider messages[]+tool_call + Summarizer + LLMBudgetSubsystem

## 目标
UE 端实现 per-agent `UMindContextManager` 持续 messages[] 对话流；Provider 接口为 messages[] + OpenAI tool_calls 标准（recall 走 tool_call 而非 Action）；Summarizer 处理 Compact 增量摘要 + scene_end per-peer 摘要；LLMBudgetSubsystem 两池限流。

## 前置
T08（Memory Service 端点就绪）+ T07（NPC_1 Mind 闭环跑通）+ T07.6（messages[0] 字段顺序锚定）

## DoD

### A. Provider 接口升级（解锁所有上层）
- [ ] `FMindMessage` USTRUCT：role / content / ts / scene_id / channel / speaker_id / tags / est_tokens / **Seq**（单调递增）
- [ ] `FMindModelCapabilities`：按 model_id 查 context_window / max_output / supports_tools / cache_usage_field
- [ ] `UMindLLMProvider::RequestCompletion(TArray<FMindMessage> Messages, TArray<FToolSchema> Tools, FOnLLMResult Done)` 单一入口；删 (System, User) 双串
- [ ] `UMindLLMProvider::RegisterTool(FName Name, FString Schema, FOnToolCall Handler)` + `GetMaxToolIterations()=2` + `GetContextWindowSize()`（查 Capabilities）
- [ ] `UMindLLMProvider_DeepSeek`：for-loop 平铺 messages[] → OpenAI 格式；tool_call 循环（while LLM 返回 tool_calls → 执行 handler → append role=tool 消息 → 再调；超 max_iterations 强制最终生成）
- [ ] `UMindLLMProvider_Mock`：扫 `LastTriggerTag` 优先；扩展 `FMockResponseRow.MatchTrigger`；匹配优先级 MatchTrigger > MatchPattern > Fallback

### B. ContextManager
- [ ] `UMindContextManager` per-agent buffer：Layer 0a/0b/1/2 切片
- [ ] `RebuildLayer0a()` 按 T07.6 字段顺序 verbatim
- [ ] `UpdateLayer0bFrame(view_data, action_classes)` 全量原地覆盖 messages[1]；ActionRegistry `Keys.Sort()` 后渲染
- [ ] `PushUser / PushAssistant / SnapshotPlainStruct`
- [ ] Token 估算（中文 1 字 = 1 token，英文 4 字符 = 1 token，emoji 3 token）；总量 SafetyReserve × 0.5（不双算）
- [ ] `bIsCompacting` flag + `PendingTrigger`（覆盖式，仅留最新）
- [ ] `LastTriggerTag` 字段（Mock 用）

### C. Summarizer + LLMBudget
- [ ] `UMindSummarizer`：Compact 增量摘要（MERGE_PROMPT_V1 DataAsset 承载）+ scene_end per-peer 摘要；持有 `cheap_provider`；HTTP 用 TWeakObjectPtr 守 + BeginDestroy cancel
- [ ] `UMindLLMBudgetSubsystem`：MaxConcurrentReasoner=4（决策）+ MaxConcurrentSummarizer=2（Compact / scene_end）；token 类型 Decision / RecallRetry / Compact / SceneSummary
- [ ] Compact 触发：MaterializeForCall 检查 SoftWatermark / HardWatermark；CompactAsync / CompactSync；Layer 1 自身超 L1AnchorBudget 时调 COMPRESS_PROMPT_V1 二次压缩；compare-and-append 防 race

### D. MindComponent 接入
- [ ] 持有 `TObjectPtr<UMindContextManager> Context`
- [ ] BuildSystemPrompt 改为 `Context->RebuildLayer0a` + `Context->UpdateLayer0bFrame`
- [ ] `OnLLMResponse` 末尾 `Context->PushAssistant(EnvelopeJsonVerbatim, Seq=NextSeq())`
- [ ] `HandleActionDone` 末尾：`MemoryClient->WriteEvent(...)` + `Context->PushUser("[结果] ...")`（成功）/ `PushUser("[失败] ...")` + WriteEvent(result="failed")（失败）
- [ ] `ValidateRetryDepth ≤ 5` 字段 + `ValidateRetryMax` 常量；recall 上限由 Provider 内部 `max_tool_iterations=2` 控制（不在本字段）
- [ ] **Initialize 时 `AgentIdStable` 空 → hard fail**（Error log + State=Idle 拒绝决策；不 fallback DisplayName）

### E. MemoryClient
- [ ] `UMindMemoryClient` 重设计现有骨架：`WriteEvent`（fire-and-forget）/ `RecallSync`（5s timeout，Provider tool_call 用）/ `WritePeerSummary` / `FetchPeerSummaries` / `WriteSceneLog`
- [ ] BaseUrl 优先从 `.env` 读 `MEMORY_SERVICE_URL`，找不到 fallback `http://127.0.0.1:8765`

## 关键文件
新增：MindMessage.h / MindModelCapabilities.h / MindContextManager.h+.cpp / MindSummarizer.h+.cpp / MindLLMBudget.h+.cpp / DA_MergePromptV1.uasset / DA_CompressPromptV1.uasset
修改：MindLLMProvider.h / MindLLMProvider_DeepSeek.cpp / MindLLMProvider_Mock.cpp / MindAgentConfig.h / MindComponent.h+.cpp / MindMemoryClient.h+.cpp
删除：MindAction_Recall.h+.cpp（如已存在）

## 验收信号
1. Mock + DeepSeek 都用新 messages[] 签名跑通 TestPing
2. **第二次起** RequestDecision，DeepSeek 响应 `usage.prompt_cache_hit_tokens` 占 input ≥ 70%（首次允许 0）
3. 手工注入 100 条混合消息触发 Compact 一次，AnchoredSummary 非空 ≤ L1AnchorBudget
4. 再注入 100 条触发第二次 Compact，AnchoredSummary 仍 ≤ L1AnchorBudget
5. Compact 异步进行中收到 RequestDecision → PendingTrigger 覆盖式保留最新；Compact 完成后 dispatch
6. 构造 Compact 期间 Layer2 被 push 新消息场景 → anchor 不覆盖 hot（compare-and-append 生效）
7. 清空某 NPC AgentIdStable 后 Initialize 直接 Error 拒绝决策
8. Token 估算偏差监控：连续 3 次 actual/estimated > 1.3 → SafetyReserve 自动升 0.7

## 不在范围
- recall tool 实际 e2e 验证（Task #9 即 plan 第 8 步做）
- T22 偷听消息 push（独立任务卡）
- T16 跨场 peer_summary 注入 L0b（独立任务卡）

## 风险
- HTTP 异步嵌套 + UObject GC：所有回调 TWeakObjectPtr 守 + BeginDestroy cancel
- Token 字符估算偏差大：50% 安全网 + 偏差监控自动调升
- Layer 1 user role（不 system）跨厂商兼容；T18.5 提前测 GLM
