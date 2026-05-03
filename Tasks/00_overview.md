# 对抗博弈记忆系统 — UE 5.7 落地任务总览

## Context

把 `Docs/memory_principles.md`（协议契约）+ `Docs/memory_implementation_ue57.md`（UE 5.7 落地）+ `Docs/schema.yaml`（字段真相源）从文档落到 `Source/AILiveProject/` 工程代码。

**为什么现在做**：ACT01「规则介绍」、ACT02「10 NPC 多 LLM 并发 + TTS 串行」已跑通，但当前 `Act02RuleReceiveDirector` 走 `WriteLLMLog` / `WriteWinnerLog` 把对话历史 dump 到 `Saved/Logs/Act02/<session>/*.md`——这条路无法支撑后续协议层契约（四通道认知输出、bid + floor control、视角隔离的 prompt 拼装、防赖账 commitments、防自爆 Listener-as-filter、跨拍 pending_intended 注入、跨局 Delete 协议）。

**目标产出**：`Saved/Games/<game_id>.db` 单局 SQLite 真相源 + `_meta.db` 跨局生命周期 + `UAILiveEventStoreSubsystem` + `AILivePromptAssembler` + bid 协议 + Director 改造为 tick 驱动。各步对齐 `memory_implementation_ue57.md` §11 L0/L1/L2/L3 验收。

**MVP 范围对齐 PRD §"MVP 首局锁定"**：05 僵尸游戏 + 10 NPC + DEEPSEEK/GLM/QWEN。**本轮范围裁决**：

- **包含**：真实 Reasoner / Parser 双 LLM 分离（用户裁决，本轮必须含）
- **不做**：Listener-as-filter 真 LLM 调用（MVP 直通）、Score Leakage Judge、BEL_EXT 9 项 evaluator 接入、跨局 Experience Pool

> 与 implementation §12 的偏离：Parser 双 LLM 从"不在范围"提升到"本轮必做"；其余 §12 列项保持。

---

## 既有工程 inventory（已核实，影响拆解）

| 项                                             | 当前状态                                                                                                                     | 任务影响                                                                                                                         |
| ---------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------- |
| `AILiveProject.uproject` Plugins               | 未启用 `SQLiteCore`                                                                                                          | T0 必须显式启用                                                                                                                  |
| `AILiveProject.Build.cs`                       | Public deps 无 `SQLiteCore` / `OpenSSL`                                                                                      | T0 加两条                                                                                                                        |
| `Source/AILiveProject/Public/Memory/`          | **不存在**                                                                                                                   | T1 新建目录                                                                                                                      |
| `FNPCAgentConfig`（`LLM/AILiveAgentRoster.h`） | 6 个扁平字段（NPCIndex / NPCActorLabel / **DisplayName** / Provider / **Voice** / **GenderHint**）                           | T1 整体重整为三层组合，破现有调用点                                                                                              |
| `AAct02RuleReceiveDirector`                    | `WriteLLMLog` / `WriteWinnerLog` / `GetSessionDir` / `MakeSubDir` 落 .md                                                     | T5 删除 4 个方法 + 改 AppendEvent                                                                                                |
| `AAct01RuleIntroDirector`                      | **未实现 LLM 写盘路径**（grep 确认零 `WriteLLMLog` 等引用），只 orchestrate Door + 视频流                                    | T5 是**首次**为 ACT01 引入 EventStore 写入（setup phase 事件），不是删旧路径                                                     |
| `Saved/Games/`                                 | 不存在                                                                                                                       | `BeginGame` 自建                                                                                                                 |
| `DevLog/`（**根目录**，非 Docs/DevLog/）       | 实存在 16 篇里程碑文档（`2026-04-24` ~ `2026-05-02`）；AGENTS.md line 81 强制"每个里程碑新增 `DevLog/YYYY-MM-DD_<topic>.md`" | 每个大节点（T1/T3/T5/T7/T9 完成）必须产出一篇 DevLog；写在各任务卡的"完成定义"                                                   |
| `AGENTS.md` line 25 + CLAUDE.md                | 强制 Monolith MCP 预检：`mcp__monolith__monolith_status` 在线 → 读真实状态 → 差量                                            | 涉及 PIE / 资产 / Director 的任务（T0/T5/T7）必须把 MCP 预检写为前置步骤；纯 C++ 任务（T1/T2/T2.5/T3/T4/T6/T9）用 Read/Grep 即可 |
| `OpenAIChat::FRequest/FResult`                 | 已就位                                                                                                                       | 不动；Parser 客户端 T2.5 复用此 client 但走独立 wrapper（固定 system prompt）                                                    |
| `Util/ProjectEnvLoader`                        | 已就位                                                                                                                       | 复用读取 .env                                                                                                                    |

---

## 任务总览表

| 编号                                     | 标题                                                                                     | 依赖         | 改动量 | §11 验收用例                                                                                                                                                                                                                                                               |
| ---------------------------------------- | ---------------------------------------------------------------------------------------- | ------------ | ------ | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| **[T0](T0_sqlitecore_openssl_setup.md)** | 启用 SQLiteCore + 加 OpenSSL 依赖 + UBT 全量 + 最小编译验证                              | —            | 小     | impl §1 实施前验证 4 步                                                                                                                                                                                                                                                    |
| [T1](T1_memory_types_skeleton.md)        | 新建 `Memory/` 类型骨架 + 重整 `FNPCAgentConfig` + schema 三方对齐                       | T0           | 中     | L2「装配双轨同步」+ 新增「schema 三方对齐」                                                                                                                                                                                                                                |
| [T2](T2_schema_eventstore_init.md)       | Schema migration + EventStore 子系统骨架 + agent_calibration 全字段落地                  | T1           | 中     | L0「`.db` 存在 + 14 表 + schema_version 校验」+ 新增「`agent_calibration` 列存在性 SQL 检查」                                                                                                                                                                              |
| **[T2.5](T2_5_parser_llm_client.md)**    | Parser LLM client + 固定 system prompt + parser_version registry                         | T2           | 中     | 新增「Parser 输出 4 段 schema 校验」+「故意缺 `<BID>` 段 → reject」+「prompt 文件加载路径与 schema_meta 一致」                                                                                                                                                             |
| [T3](T3_eventstore_write_path.md)        | AppendEvent / 哈希链 / parse_failed 兜底 / tick_no                                       | T2.5         | 大     | L0「100 事件 + tick_no」、L2 全部写入相关项                                                                                                                                                                                                                                |
| [T4](T4_eventstore_read_api.md)          | 读 API + 视角隔离 JOIN（含最小化 ListMyPendingIntended）                                 | T3           | 中     | L1「视角隔离 / 系统事件不泄露 / FTS5 模糊」+ 新增「ListMyPendingIntended 不依赖投影表」                                                                                                                                                                                    |
| [T5](T5_director_writeback.md)           | Director 写入接入 + in-flight 协议 + ACT01 首次入库                                      | T3           | 中     | L1「ACT02 全链路 + events 无丢失」+ 新增「ACT01 首次入库」                                                                                                                                                                                                                 |
| [T6](T6_prompt_assembler.md)             | `AILivePromptAssembler` —— 自我发言段 + pending_intended 段 + challenge prefetch         | T4           | 中     | L1「pending_intended 注入」                                                                                                                                                                                                                                                |
| [T7](T7_runtick_bid_parser.md)           | Bid 协议 + Floor control + Director `RunTick` 重写（**含 Reasoner→Parser 双 LLM 串联**） | T2.5, T5, T6 | 大     | L1「一拍单一 winner / speech.public 衍生关系 / 冷场推进 / bid 隔离」、L2「反霸麦 / 被 @ 加权 / bid 超时 / tick_resolved/audit 拆分 / action.intent 对所有 agent 派生 / 同拍 tick_no 切片 / payload redaction fuzz」+ 新增「Parser 解析失败 → SystemParseFailed + abstain」 |
| [T8](T8_projector_rebuild.md)            | Projector 完整重建（commitments / pending_intended / alliance / vote）                   | T7           | 中     | L2「摘要展开 source_seq 定位」+ 新增「pending_intended 投影表与 T6 内联计算结果一致」                                                                                                                                                                                      |
| [T9](T9_resume_delete_audit.md)          | VerifyHashChain + Resume 协议 + Delete 跨库桥接 + agent_registry 同步                    | T8           | 中     | L2「超时恢复 / Resume 一致性 / 哈希链 / DB Browser 实时只读 / Delete 协议跨库桥接 / agent_registry 同步」、L3「性能 / 并发」                                                                                                                                               |

---

## 与 implementation §8 12 步的偏离

| §8 步                                                                                        | 落到本拆解                                                                                                           | 理由                                                                                                                                                                                                                                   |
| -------------------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| step 1（Build.cs + 空骨架）                                                                  | T0 + T1 拆开                                                                                                         | T0 必须先独立验证 SQLiteCore 真的能 link，再投入类型工作；空骨架放 T1 与三层 struct 一起，避免空文件中间态                                                                                                                             |
| step 2 + step 3（类型 + Roster 重整）                                                        | 合并到 T1                                                                                                            | `FNPCAgentConfig` 字段重整会破 `Roster.cpp` + `Act02Director.cpp` 全部 `DisplayName / Voice / GenderHint` 引用，必须一拍内全改完才能编译                                                                                               |
| step 11（VerifyHashChain + RebuildProjections）                                              | 拆到 T8（Rebuild）+ T9（Verify + Resume）                                                                            | Rebuild 是投影层职责，Verify + Resume 是审计层；Resume 依赖 Rebuild，但 Verify 不依赖。拆开后两层可独立验证。                                                                                                                          |
| step 12（pending_intended projector）                                                        | **T4 最小化路径 + T8 完整投影**                                                                                      | 协作者指出原方案中 T6 验收 pending_intended 注入但 projector 在 T8——依赖错位。修订：T4 提供最小化 `ListMyPendingIntended`（events 表 + parent 链状态实时计算），让 T6 PromptAssembler 不依赖 T8；T8 仍建立完整投影表加速大数据量场景。 |
| **新增点 tick_no / tick_anchor**                                                             | T3（缓存 + BeginTick）+ T7（按 tick_no 切片消费）                                                                    | tick_no 是写入侧字段（必须 AppendEvent 路径自动填），ListBidsForTick / 同拍 action.intent 派生在裁决侧消费。                                                                                                                           |
| **新增点 agent_registry 跨库桥接**                                                           | T9                                                                                                                   | 与 Delete 协议同生命周期，没必要单独成步。                                                                                                                                                                                             |
| **新增点 parse_failed 兜底**（schema.yaml line 138/152 硬约束）                              | T3（与 AppendEvent 同步落地）                                                                                        | implementation §5.2 明确 `AppendSystemParseFailure + InsertEventBypassValidation` 是 AppendEvent 失败路径的内置兜底，分两步会留下"AppendEvent 已上线但失败时直接吞错"的窗口期                                                          |
| **新增点 Reasoner / Parser 双 LLM**（用户裁决本轮含真实 Parser，与 implementation §12 偏离） | **新增 T2.5（Parser client + system prompt + parser_version registry）+ T7（RunAgentTick 内 Reasoner→Parser 串联）** | T2.5 把 Parser 通道独立打通便于单元测试；T7 整合到 RunTick，含 reject sample 至 3 次 + agent abstain 兜底（principles §5.4）。`events.parser_version` 字段从 schema_meta registry 读取，避免硬编码。                                   |
| **新增点 schema 三方对齐验收**（协作者建议）                                                 | T1（USTRUCT 层） + T2（DDL 层）                                                                                      | schema.yaml 是字段真相源，T1 落 USTRUCT、T2 落 DDL，本拆解必须在两层都加显式 unit test 防止漂移。                                                                                                                                      |

---

## 端到端验证（按 T0 → T9 顺序提交后应当通过）

1. **PIE 启动 ACT01 + ACT02 + 跑 3 轮反应** → `Saved/Games/<ts>.db` 文件存在；**`Saved/Logs/Act02/` 不再生成新文件**；DB Browser 看到 events / event_visibility / commitments / agent_view_state.pending_intended 均有数据
2. **手工质问场景**：构造一个引用"第 2 轮"的 prompt → AssembleUserPrompt 自动 prefetch 第 2 轮所有该 NPC 可见事件
3. **Kill UE 中途 → 重启 ResumeFromGameId** → 游戏状态恢复；in-flight 的 LLM 调用全部转 SystemAgentTimeout
4. **DB Browser Read-only 实时审查**：UE 运行中打开同一 .db → 刷新能看到新事件
5. **Delete 触发**：手动调 `TriggerDeleteExecuted("NPC03", ...)` → `_meta.db.agent_lifecycle_events` + 该局 .db `system.delete_executed` 同步写入；其他 NPC 在下一拍 prompt 中通过近场窗口看到该 system 事件
6. **CI grep**：工程内无 `Sha1` / `FSHA1` 引用；业务层无 `InsertEventBypassValidation` 调用；无 `SetCDOProperty` 直接改继承组件（与 CLAUDE.md MCP 规则一致）

---

## 文档冲突 / 待澄清

施工前如发现下列冲突，**先停下来跟用户确认**，不要自行决定：

1. **schema.yaml `agent.model_provider`**（小写字符串如 `"deepseek"`）vs **`ELLMProvider` enum**（首字母大写 `DeepSeek`）—— `ProviderToString` 应当输出哪种 case？impl §4.1 注释里"deepseek/glm/qwen3"暗示小写；T1 的"装配双轨同步"用例需要确定。
2. **跨 Act 的"局"边界**：当前 `BeginAct02()` 触发 BeginGame，但 ACT01 + ACT02 是否同一局？implementation §5.3 说"`EndAct02()` 不调 EndGame"暗示同一局；T5 修订版**默认采用此解读**——ACT01 入口先检查 `IsGameOpen()`，若否则 BeginGame；ACT02 沿用同一 game_id。如该解读不对请告知。
3. **Listener-as-filter MVP 直通的 `listener_filter_score` 字段值**：T7 写的 `speech.public.payload.listener_filter_score` 在 MVP 直通时填 `0.0`？还是缺字段？schema.yaml 标注为 optional，但 BEL_EXT 第 9 项的距离计算需要它存在 —— MVP 范围 BEL_EXT evaluator 不实现，但字段约定应当确定下来避免后续迁移。
4. **Parser LLM 选用哪家 provider**（用户裁决本轮含真实 Parser，但未指定具体模型）：T2.5 默认选 GLM 或 Qwen3 中等规模模型作为 Parser（避免与 Reasoner 用同一模型造成 self-play 风格匹配）；如果你已有指定供应商或要求"必须用现有 3 家之外的小模型"，请告知。

---

## 不在本拆解范围（对齐 implementation §12，**Parser 与 PARSER prompt 注册表 已剔除**）

- ~~Reasoner / Parser 双 LLM 架构分离~~ → **本轮含**（T2.5 + T7）
- Listener-as-filter 真 LLM 调用（MVP 直通）
- Score Leakage Judge 接入
- BEL_EXT 9 项 evaluator 接入（仅占位 bel_violations 表）
- GOAL / BEL dashboard
- 跨厂商 bidding 校准的预热流程
- 联盟形成机制完整实现（events 表 + projector 已支持，moderator 巡检不实现）
- 跨局 Experience Pool
- Delete 协议的决策机制（仅 hook 已就绪）
- ~~PARSER prompt 注册表~~ → **本轮含**（T2.5 落 `Content/Prompts/Parser/v1.txt` + `schema_meta.parser_prompt_registry_path` + `parser_version`；下阶段才考虑做 `_meta.db.parser_prompts` 表存全文 + SHA-256 双备份）
- 节目化层 / 观众接口
- 离线哈希链重写工具
