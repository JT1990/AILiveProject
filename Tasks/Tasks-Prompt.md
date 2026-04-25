# 单任务执行 Prompt（新对话窗口用）

## 用法

开一个新的 Claude Code 对话窗口，把下方"启动咒语"整段复制进**第一条消息**，并把 `<TASK_ID>` 替换成你要做的任务编号（如 `T00` / `T01` / `T05` / `T13.5` / `T19.7`）。

每个新对话只做**一张**任务卡。完成后开新对话做下一张。

---

## 启动咒语（复制这段）

```
我要在新对话窗口独立执行 AI 心智决策系统的单张任务卡：<TASK_ID>。

## 上下文加载顺序

请按以下顺序加载，然后等我说"开始"再动手：

1. **必读**：`Tasks/README.md`（全局任务清单 / 里程碑 / 设计原则 / 依赖图 / 关键决策表）
2. **必读**：`Tasks/task_<TASK_ID>_*.md`（本次任务卡，全文读）
3. **必读**：`CLAUDE.md`（项目约束、Monolith MCP 踩坑、A2F 角色契约、Visual-override 系统）
4. **必读**：项目根 `.env`（DEEPSEEK_API_KEY / EMBEDDING_API_BASE / NEO4J_URI 等环境变量；只看变量名和值，不要打印完整 key 到对话）
5. **按需读**：任务卡"前置"段提到的其他卡——只读其"目标"和"关键 API"段，不读全文
6. **按需读**：任务卡"关键文件"段提到的现有代码 / 资产

## 加载完后回报

请按以下结构回报，等我确认：

- **目标**：任务卡的 1 句话目标
- **DoD 关键项**：≤3 条最重要的验收清单
- **前置完成度**：每个前置卡用 git log / 读现有文件验证（不靠假设），明确哪些已完成、哪些缺失
- **风险或阻塞**：识别到的具体风险（包括"前置应有的代码 / 资产没找到"这种）
- **实施计划**：≤5 步，每步对应 DoD 的某一项
- **不在范围**：明确这次不做什么（避免越界）

## 实施纪律

等我说"开始" / "go" / "可以" 后才动手。开始后：

- 严格按 DoD 走，不要发散
- 不要改任何不在 DoD 列表的东西（"顺手改进"是反模式）
- BP / UMG / 资产读写一律用 Monolith MCP（先 `monolith_status` 确认在线；离线则**停下问我**，不要 fallback 到手点编辑器）
- 改 BP 前预检：`get_components` / `get_graph_summary` / `get_execution_flow` / `get_variables`，对照差量改，避免重复组件 / 断裂图（CLAUDE.md "改 BP 前做预检"强约束）
- C++ 修改先 read 文件再 edit；增量编译（每加 1-2 个 .h/.cpp 配对就 build 一次）
- 遇到任务卡里没明示的决策点 → **停下问我**，不要自己拍板

## 项目级强约束（**违反就塌**，CLAUDE.md 也有）

- **Validate / Apply 拆分**：GM 不能在 Action 异步执行前改状态。流程 `Dispatch → Validate(只读) → Action.Execute → OnActionDone → if ok: Apply → Memory.Write → State=Idle → OnAgentActionFinished`。所有 Action **不直接改 GM state**
- **agent_id 用 `UMindAgentConfig.AgentIdStable`**，绝不用 `GetName()`（PIE 有 `_C_0` 后缀会让记忆全断）
- **Speak 必须经 `UMindSpeechHelpers::ResolveSpeechActor()`** 拿可见 MetaHuman child actor，不是 NPC 壳 Pawn（否则口型静默失败）
- **TriggerMinimaxSpeech 参数**：`Text` 是 `FString`（不是 `FText::FromString`）；`Endpoint` 不传 `{}`（会覆盖默认值为空，要么不传要么显式 `TEXT("https://api.minimaxi.com/v1/t2a_v2")`）；`A2FProviderName` 是 `FName`
- **`.env` key 命名**：用 `XXX_API_KEY` / `XXX_API_BASE` 风格；通过 `GetEnvValueFromProjectEnv()` 读取，不硬编码
- **NPC 必须是 Pawn 子类**（T00 已应验证）
- **Prompt injection 防御**：拼记忆进 prompt 时 wrap `[NPC_X 在 ts=... 说: "..."]`
- **MindComponent 不加 tick**（事件驱动）

## 完成后

- 跑任务卡的"验收信号"段所列每条
- 给一份完成总结：
  - 修改 / 新增的文件清单
  - 验收信号哪些 ✅ 哪些 ❌（失败的写明原因）
  - 遗留风险或后续卡的依赖（如本卡接口供 T<XX> 用）
- **大里程碑末尾卡**（T07 / T14 / T17 / T23 / T26）额外写 DevLog 到 `DevLog/2026-XX-XX_<topic>.md`
- **不要主动 commit**；等我说 "commit" 才提交

## 开发期加速（M1 起）

- 默认用 `UMindLLMProvider_Mock`（T05.5 已就位），改 prompt 不需等真 LLM 2-4s 响应
- 验收前的最后一步才把 Provider 切回 DeepSeek / GLM
- Memory Service 必须先起：`cd Tools/MemoryService && uvicorn app:app --port 8765`

## 现在请开始加载上下文。
```

---

## 启动咒语的设计要点（给你看，不在咒语内）

这份咒语解决新对话窗口的 4 个常见失败模式：

1. **新对话上来就乱写代码**——咒语强制"加载 → 回报 → 等确认"三段式
2. **Claude 不知道哪些是项目级强约束**——咒语显式列 P0 强约束（即使 CLAUDE.md 已写，重复一遍降低遗漏）
3. **越界改无关代码**——咒语强调"不在 DoD 的不改"
4. **遇到决策点自己拍板**——咒语显式说"停下问我"

## 推荐的对话流（你和新对话 Claude 之间）

```
[你]    （贴启动咒语，把 <TASK_ID> 换成 T00）
[Claude] 加载完毕。回报：目标=... / DoD 关键=... / 前置缺失=... / 计划=...
[你]    开始 / 或者：先回答几个问题
[Claude] 实施... 遇到决策点：问你
[你]    回答
[Claude] 继续... 验收完毕，给完成总结
[你]    （在原对话窗口或新窗口）commit
```

## 任务卡选择建议

按 `Tasks/README.md` 的依赖图顺序做。新手建议第一张做 **T00**——它是 checklist 类型，没有代码改动，可以让你（和 Claude）熟悉这套流程而不冒架构风险。

如果某张卡对你来说太大（比如 T19.7 涉及 GASP SO 复用预检），可以让 Claude 先**只做预检部分**，写个中间报告，再开新对话做后续实现——这是把"一张卡"拆成两个 session 的合法用法。

## 不在本咒语范围

- 自动化测试 / CI（项目无）
- 跨多张卡的串联（每个对话只做 1 张；想串联用 `/loop` 自己排）
- 调用 Anthropic API / Claude SDK（与本项目无关）
