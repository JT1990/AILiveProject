# T1 — 新建 `Memory/` 类型骨架 + 重整 `FNPCAgentConfig` + schema 三方对齐

日期：2026-05-04

## 目标

T0（启用 SQLiteCore + OpenSSL）落地后，把对抗博弈记忆系统（`Tasks/00_overview.md` + `Docs/memory_implementation_ue57.md`）的"形"先建起来：

- `Source/AILiveProject/Public|Private/Memory/` 全部新增类型——三层 agent struct（Core / Identity / Battle）、events / commitments / bid USTRUCT、UENUM、序列化辅助、schema 字段映射 console command；
- `FNPCAgentConfig` 由 6 字段扁平改成 `Core / Identity / Battle + Runtime` 三层组合，配合一拍内改完 `Roster.cpp` + `Act02RuleReceiveDirector.cpp` 共 7 处现有字段访问点；
- 落地"装配双轨同步"unit test（§11 L2）+ 协作者新增的"schema 三方对齐"unit test。

完成 T1 **不**等于 EventStore 就绪——只意味着类型层就位、ACT02 行为不退化、双轨 + schema 三向校验通过。

## 文档真相源 / 决策点

| 决策点 | 取值 | 出处 |
| --- | --- | --- |
| `agent.model_provider` 字面值大小写 | **小写**（`"deepseek"` / `"glm"` / `"qwen3"`） | 用户裁决：`00_overview.md §83` 第 1 项明列冲突，用户选定小写。`ProviderToString` 是该裁决在 C++ 的实现，schema.yaml line 49 注释中的大小写举例（`"DeepSeek"`）由后续文档同步统一改写 |
| `Identity.VoicePresentation` 默认值 | **全 10 项 NPC 统一 `Synthetic`** | 用户裁决（AskUserQuestion）。任务卡风险点要求"重新决定"，不沿用 `GenderHint` 直觉映射；后续在 T6 / DataAsset 重整时按 NPC 一一裁定。与 PRD line 104 "AI 不是人类、只是声线与形象呈现" 对齐 |
| `GenderHint`（`"male"` / `"female"`）落点 | **`VoicePresentationHint`（runtime FString）** | 任务卡风险点：runtime 字段不入 schema.yaml；TTS pipeline 沿用此字面值 |

## 改动清单

### 新增

| 文件 | 内容摘要 |
| --- | --- |
| `Source/AILiveProject/Public/Memory/AILiveAgentTypes.h` | `EAILiveAgentStatus`(3) + `EAILiveVoicePresentation`(5) + `FAILiveAgentCore`(7 字段) + `FAILiveAgentIdentity`(5 字段) + `FAILiveAgentBattleConfig`(7 字段) |
| `Source/AILiveProject/Public/Memory/AILiveEventTypes.h` | `EAILivePhase`(6) + `EAILiveEventType`(24 项含 `SpeechIntended` / `Bid` / `OrchestratorTickAnchor` / `OrchestratorTickResolved` / `OrchestratorTickAudit` / `ActionIntent` / `ActionResolved` / `ActionCancelled` / `SystemDeleteExecuted`) + `EAILiveSpeechActType`(8 含 None) + `FAILiveEvent`(17 字段) + `EAILiveCommitmentType`(5) + `EAILiveCommitmentStatus`(3) + `FAILiveCommitment`(8 字段) + `namespace AILiveEvent` 序列化辅助声明 + `DECLARE_LOG_CATEGORY_EXTERN(LogAILiveMemory, ...)` |
| `Source/AILiveProject/Private/Memory/AILiveEventTypes.cpp` | 5 套 `*ToString` + 5 套 `*FromString` + `ArrayToJsonString` / `JsonStringToArray` 序列化辅助；switch + `TEXT()` 字面量；未知字符串返各自默认 + `UE_LOG(Warning)`；`DEFINE_LOG_CATEGORY(LogAILiveMemory)` |
| `Source/AILiveProject/Public/Memory/AILiveBidTypes.h` | `FAILiveBid`(8 字段：Actor / Seq / IntendedSeq / Urgency / BidOffset / FinalScore / ProposedTarget / Rationale) + `FAILiveTickResolution`(5 字段：TickNo / WinnerActor / WinnerIntendedSeq / DerivedPublicSeq / AllBids) |
| `Source/AILiveProject/Private/Memory/AILiveSchemaCheck.cpp` | 三向校验逻辑 + 9 项显式映射表 + 两条 `FAutoConsoleCommand`（`AILive.CheckSchemaMapping` / `AILive.CheckRosterDualTrack`） |

### 修

| 文件 | 改动 |
| --- | --- |
| `Source/AILiveProject/Public/LLM/AILiveAgentRoster.h` | `FNPCAgentConfig` 重整为 `Core / Identity / Battle` + 4 runtime 字段（`NPCIndex` / `NPCActorLabel` / `VoicePresentationHint` / `Provider`）；保留 `ELLMProvider` 枚举与 `ProviderToString` / `ResolveProviderEndpoint` 不动 |
| `Source/AILiveProject/Private/LLM/AILiveAgentRoster.cpp` | `GetDefaultRoster()` 全量改写：lambda 装配三层 + 4 runtime；显式 `Cfg.Core.ModelProvider = ProviderToString(Cfg.Provider)`（**双轨同步关键行**） |
| `Source/AILiveProject/Private/Acts/Act02RuleReceiveDirector.cpp` | 7 处字段路径替换：`Cfg.DisplayName` → `Cfg.Identity.FullName`（528 / 965 / 972 / 990 / 998 行）；`N.Config.Voice` → `N.Config.Identity.Voice`（861 行）；`Cfg.GenderHint` → `Cfg.VoicePresentationHint`（965 / 990 行）。Prompt 文本字面量未动（"性别（%s）" 留待 T6 PromptAssembler 统一改写） |

## `GetDefaultRoster()` 装配字段默认值（锁定，避免 T2/T5 漂移）

| USTRUCT 字段 | T1 装配值 |
| --- | --- |
| `Core.AgentId` | `FString::Printf("NPC%02d", Idx)` → `"NPC01"` .. `"NPC10"`，对齐 schema.yaml viewer 命名空间 `NPC<NN>` |
| `Core.PersonaVersion` | `1` |
| `Core.ModelProvider` | `AILiveAgentRoster::ProviderToString(Provider)`，双轨同步 |
| `Core.ModelName / CreatedAt / DeletedAt` | 空 FString，T2/T5 写库时填 |
| `Core.Status` | `EAILiveAgentStatus::Active` |
| `Identity.FullName` | `"NPC-01"` .. `"NPC-10"`（短横线分隔，沿用旧 DisplayName 格式） |
| `Identity.Nickname / Appearance` | 空 FString，DataAsset 后续填 |
| `Identity.VoicePresentation` | `EAILiveVoicePresentation::Synthetic`（10/10 全 NPC） |
| `Identity.Voice` | `"male-qn-qingse"` / `"female-shaonv"`（MiniMax voice ID，沿用） |
| `Battle.*` | 全部默认值（Faction / Role / PrivateGoal 空；BidOffset=0；SeqStart=0；bAlive=true；AllianceMembers 空数组）；T2 setup phase 由 Director 决 |
| Runtime: `NPCIndex` | `1..10` |
| Runtime: `NPCActorLabel` | `"BP_NPC_MH_Character_<N>"` |
| Runtime: `Provider` | 沿用 4 DeepSeek / 3 GLM / 3 Qwen3 分配 |
| Runtime: `VoicePresentationHint` | 沿用旧 `GenderHint` 字面值 `"male"` / `"female"` |

## Schema 三方对齐（`AILiveSchemaCheck.cpp`）

任务卡 line 41 + 协作者建议 #1 + #2：仅遍历映射表查反射会留漏洞——schema 新增字段忘加映射 / schema 删字段后旧映射残留 / `enum class : uint8` 反射类型误判（应为 `FEnumProperty` 而非 `FByteProperty`）都会让验收给假阳性。三向校验：

1. **运行时扫 `Docs/schema.yaml`** —— `FFileHelper::LoadFileToString` + 段名 tracking；只在顶层段为 `agent` / `identity` 时收集；字段行匹配规则：恰好 2 个前导空格 + 标识符 + `:` + 行内含 `__storage: asset` 子串（避开 `meta_db.agent_registry` / `game_db.agent_calibration` 与散文注释）。输出 expected `TSet<FString>`。
2. **显式映射表**（9 项 entry）—— `{ yaml_path, struct_getter, ustruct_field, expected_kind }`，覆盖 schema.yaml 当前所有 `__storage: asset` 字段。
3. **三向校验**：
   - `(a) expected ⊆ mapping`：schema 新增字段忘加映射 → Error
   - `(b) mapping ⊆ expected`（**stale mapping**）：schema 删/改字段后旧映射残留 → Error
   - `(c) mapping → reflection`：`FindFProperty` 命中 + 类型匹配；`identity.voice_presentation` 走 `IsA<FEnumProperty>` + `Cast<FEnumProperty>(P)->GetEnum() == StaticEnum<EAILiveVoicePresentation>()`（**不是** `FByteProperty`，那是底层存储）

末尾 `UE_LOG(Display, "[SchemaCheck] OK count=N FAILED count=M")`。

### 双轨同步 unit test

`AILive.CheckRosterDualTrack`：遍历 `GetDefaultRoster()`，逐项校验 `Cfg.Core.ModelProvider == ProviderToString(Cfg.Provider)`；不用 `check()`（会触发 Editor 断言中断、与"列出漂移项"自相矛盾），改累计 `FailCount` + `UE_LOG(Error)`，末尾输出 OK / FAILED 计数。

## 验收实证

| # | 用例 | 实证 |
| --- | --- | --- |
| 1 | UBT 全量 `Build.bat AILiveProjectEditor Win64 Development` | `Result: Succeeded`（exit 0，5.92s）。8 步全过：5 个 cpp 编译 + `Module.AILiveProject.gen.cpp`（UHT 生成的 USTRUCT 注册码）+ Link `.lib` + Link `.dll` + WriteMetadata |
| 2 | PIE 烟测 ACT02 3 轮（ReactionRoundCount=3） | session `2026-05-04_043140/` 完整：seed + reaction_round_01..03。`npc_07_glm.md` 第 21 行 system prompt: `你是 AI NPC-07，female` —— `Cfg.Identity.FullName`("NPC-07") + `Cfg.VoicePresentationHint`("female") 字段路径替换后输出与重命名前完全一致。HTTP 200 / latency 2254ms / parse_error=false / parsed_willingness=extremely_strong / parsed_want_to_speak=true。`_winner.md` 显示 winner=NPC06 (deepseek, extremely_strong)，9/10 NPC 有 unspoken（NPC03 单轮空属 LLM 偶发未生成，与 T0 前波动一致） |
| 3 | `AILive.CheckRosterDualTrack` console command | MCP `search_logs` 返回 2 条：`[DualTrack] starting` + `[DualTrack] OK count=10 FAILED count=0` —— 10 个 NPC 的 `Cfg.Core.ModelProvider == ProviderToString(Cfg.Provider)` 全部成立 |
| 4 | `AILive.CheckSchemaMapping` console command | MCP `search_logs` 返回 3 条：`[SchemaCheck] starting` + `[SchemaCheck] schema.yaml asset fields collected: 9` + `[SchemaCheck] OK count=9 FAILED count=0` —— 三向校验 `(a) expected ⊆ mapping` + `(b) mapping ⊆ expected (stale)` + `(c) mapping → reflection` 全过；9 个 schema asset 字段（agent.{agent_id, persona_version, model_provider, model_name} + identity.{full_name, nickname, voice_presentation, voice, appearance}）全命中 USTRUCT 反射类型，`identity.voice_presentation` 走 `FEnumProperty` + `GetEnum() == StaticEnum<EAILiveVoicePresentation>()` 通过 |
| 5 | `Saved/Logs/Act02/<session>/*.md` 仍正常生成 | 烟测期间产出 `2026-05-04_042531` + `2026-05-04_043140` 两个 session 目录；T1 不删除该路径（T5 EventStore 上线后才删），本步仅确认未误删 |

## 既有功能不退化

CLAUDE.md 三条硬规则全部未触碰：未改 `bTickPhysicsAsync`、未改 `DefaultBuildSettings = V6`、未动既有 GASP / Mover / VisualOverride / NPC BP 链路。`MinimaxACELibrary` / `Util/ProjectEnvLoader` / `OpenAIChat` 等 LLM/TTS 客户端 glue 完全不动。

## 完成定义达成

- [x] `Source/AILiveProject/Public|Private/Memory/` 全部新增类型 .h/.cpp
- [x] `FNPCAgentConfig` 重整为三层组合 + 4 runtime 字段
- [x] `AILiveAgentRoster::GetDefaultRoster()` 装配显式双轨同步
- [x] `Act02RuleReceiveDirector.cpp` 7 处字段路径替换
- [x] UBT 全量构建通过
- [x] PIE 烟测 ACT02 行为不退化
- [x] `AILive.CheckRosterDualTrack` 0 漂移
- [x] `AILive.CheckSchemaMapping` 9 字段全命中

## 给 T2 / T5 的提示

1. `FNPCAgentConfig.Core.ModelName` 在 T1 装配阶段留空（避免装配期触发 `ResolveProviderEndpoint` 读 `.env`）。T2 写 `agent_calibration` 表时由调用方按需填，或 T5 在 `BeginGame` 后由 Director 一次性解析填回。
2. `Battle.*` 全部默认值；T2 在 setup phase 决 Faction / Role / PrivateGoal / BidOffset，写 `agent_calibration` 表时同步从 `FNPCAgentConfig.Battle` 读出（USTRUCT 是 runtime 镜像，DB 是真相源）。
3. `Identity.VoicePresentation` 当前全填 `Synthetic`；T6 `PromptAssembler` 真正消费此字段时再决定是否按 NPC 一一裁定（DataAsset 路径，不在 `GetDefaultRoster()` 硬编码内）。
4. `Memory/AILiveEventTypes.h` 已含全部 `EAILiveEventType`（24 项）+ `EAILivePhase` / `EAILiveSpeechActType` / `EAILiveCommitmentType` / `EAILiveCommitmentStatus`。T2 写 DDL 时 enum 字符串映射直接走 `AILiveEvent::EventTypeToString` 等函数。
5. `LogAILiveMemory` 类别已 `DEFINE_LOG_CATEGORY` 在 `AILiveEventTypes.cpp`。后续 EventStore / PromptAssembler / Projector 子系统统一走该类别，不要再自定义。
6. schema.yaml 增删 `__storage: asset` 字段时，**同步更新** `AILiveSchemaCheck.cpp` 的 `kFieldMappings` 数组；`AILive.CheckSchemaMapping` 会在 `(a)` 与 `(b)` 两向校验中暴露漂移。
