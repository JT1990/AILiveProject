# T2.5 — Parser LLM client + 固定 system prompt + parser_version registry

> 总览：[00_overview.md](00_overview.md)
> 依赖：blockedBy = [T2](T2_schema_eventstore_init.md)
> 改动量：**中**（1–4h）—— 主要时间在 Parser system prompt 的迭代调优
> §11 验收：新增「Parser 输出 4 段 schema 校验」+「故意缺 `<BID>` 段 → reject」+「prompt 文件加载路径与 schema_meta 一致」

## 预检

纯 C++ + Parser prompt 文本文件，**不需要** Monolith MCP。

## 目标

用户裁决「本轮含真实 Parser LLM」的最小落地。把参赛 Reasoner 的 raw text 解析为四通道结构化 JSON 的 PARSER LLM 路径打通——独立 LLM 调用 + 固定 system prompt + 输出 schema 强校验 + 100% 解析率约束（reject sample 至 3 次，详见 principles §5.4）。

## 涉及文件

### 新增

- `Source/AILiveProject/Public/LLM/AILiveParserClient.h` + `.cpp` —— `namespace AILiveParser { struct FParseRequest { FString RawText; FString AgentId; }; struct FParseResult { bool bOk; FString Scratchpad; FString IntendedJson; FString BidJson; FString NoteText; FString ErrorReason; }; FParseResult ParseFourChannels(const FParseRequest&); }`
  - 内部复用 `OpenAIChat::RequestBlocking` 但 wrapper 强制：(a) system prompt 固定从 **`Content/Prompts/Parser/v1.txt`** 读取（启动时 cache 到内存常量）—— 路径用 `FPaths::ProjectContentDir() / TEXT("Prompts/Parser/v1.txt")` 解析；(b) `response_format = json_object`；(c) `temperature = 0`；(d) Provider 选择独立的中等规模模型——MVP 复用现有 `ELLMProvider` 中**与本局 Reasoner 不同**的某一项作为 Parser，避免新引入 endpoint；记录 Parser 模型在 `_meta.db.schema_meta('parser_model','<provider>:<model>')`
  - 输出 schema：JSON 必须含 `scratchpad` (string) / `intended` (object: `{text, intended_action?, addressed_to_hint?}`) / `bid` (object: `{urgency, ...}`) / `note_to_self` (string) 四键，缺一即失败
- **`Content/Prompts/Parser/v1.txt`**（**唯一**真实路径，禁止其他副本）—— Parser system prompt 文本（按 principles §A.1 "OUTPUT FORMAT — STRICT" 段反向编码：把四通道格式描述给 Parser，让它从 Reasoner raw text 中切分出 4 段并输出严格 JSON）。Git 入库；后续 v2/v3 同目录递增文件名
- `Source/AILiveProject/Public/LLM/AILiveParserVersion.h` —— `namespace AILiveParser { AILIVEPROJECT_API FString GetCurrentParserVersion(); AILIVEPROJECT_API FString GetCurrentParserPromptPath(); }`，T3 的 AppendEvent 写入 `events.parser_version` 时调前者，启动时 ParserClient 加载 prompt 调后者——保证读路径与 `schema_meta.parser_prompt_registry_path` 来源一致

### 修

- `Source/AILiveProject/Private/Memory/AILiveSchemaMigration.cpp` —— `EnsureSchema()` 末尾插入 `INSERT INTO schema_meta(key,value) VALUES('parser_prompt_registry_path','Content/Prompts/Parser/'), ('parser_version','1'), ('parser_model','<concrete provider:model>')`（注：`schema_meta` 在 `_meta.db`，跨局共享）

## 依赖

blockedBy = [T2](T2_schema_eventstore_init.md)（需要 schema_meta 表）

## 验收方式

1. **正常解析**：构造一段标准 Reasoner raw text（含 `<SCRATCHPAD>...</SCRATCHPAD><INTENDED>{...}</INTENDED><BID>{...}</BID><NOTE_TO_SELF>...</NOTE_TO_SELF>`）→ `ParseFourChannels` 返回 `bOk=true` + 4 段结构化结果
2. **缺段拒绝**：故意缺 `<BID>` 段 → `bOk=false` + ErrorReason 含 "missing bid section"
3. **JSON 子对象校验**：`<INTENDED>` 段不是合法 JSON → `bOk=false` + ErrorReason 含 "intended payload not valid JSON"
4. **schema_meta 持久化**：`SELECT value FROM _meta.db.schema_meta WHERE key='parser_version'` 返回 `'1'`；`'parser_prompt_registry_path'` 返回 `'Content/Prompts/Parser/'`；`'parser_model'` 返回 Parser 实际选用的 provider:model 字符串
5. **GetCurrentParserVersion()** 返回 `'1'`；**GetCurrentParserPromptPath()** 返回 `'Content/Prompts/Parser/v1.txt'`（绝对路径展开后等于 `FPaths::ProjectContentDir() / "Prompts/Parser/v1.txt"`）
6. **路径一致性**：ParserClient 实际加载的文件路径（运行时记录到 UE_LOG）= schema_meta 注册的 `parser_prompt_registry_path` + `'v' + parser_version + '.txt'` 拼接结果

## 风险点

- principles §5.4 强调 100% 解析率是必需而非可选 —— 本步只把通道打通，**真实失败率统计**留到 T7 整合后再做（第一个完整 ACT02 跑下来看 SystemParseFailed 频次）
- Parser 用什么 provider：MVP 不引入新厂商，从现有 DeepSeek / GLM / Qwen3 三家中选一家作为 Parser；建议选 GLM 或 Qwen3 中等规模模型（速度优先，避免 Reasoner 用同一模型的 self-play 风险）
- Parser system prompt 是 schema 真相源的一部分（parser_version 升级即此 prompt 改动）—— 必须文件化、Git 入库，而不是嵌在代码字面量
- **此处的"parse_failed"**（Parser 解析失败 → Director 写 SystemParseFailed）与 **T3 的 `AppendSystemParseFailure`**（EventStore 静态校验失败兜底）是两个不同场景，都用同一 `EAILiveEventType::SystemParseFailed` enum 但触发路径独立，互不干扰

## 里程碑 DevLog

`DevLog/2026-MM-DD_parser_llm_integration.md`，记录 Parser 选型（哪家小模型）+ system prompt 设计 + 解析失败统计基线

## 完成定义

Parser client 可独立调用并通过 4 项验收 + parser_version 写入 schema_meta + 里程碑 DevLog 入库。**不**意味着已经接入 RunTick（T7）。
