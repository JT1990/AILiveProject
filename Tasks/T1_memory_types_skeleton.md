# T1 — 新建 `Memory/` 类型骨架 + 重整 `FNPCAgentConfig` + schema 三方对齐

> 总览：[00_overview.md](00_overview.md)
> 依赖：blockedBy = [T0](T0_sqlitecore_openssl_setup.md)
> 改动量：**中**（1–4h）
> §11 验收：L2「装配双轨同步」+ 新增「schema 三方对齐」

## 预检

本步纯 C++（USTRUCT / Roster / Director 字段访问），用 Read/Grep 直接查 `FNPCAgentConfig` 定义 + `Cfg.DisplayName` / `Cfg.Voice` / `Cfg.GenderHint` 引用点做差量。**不**走 Monolith MCP（Monolith 专管 Blueprint/资产，不读 C++）。

## 目标

建立 `Source/AILiveProject/Public|Private/Memory/` 全部新增类型的"形"——三层 agent struct、events/bid USTRUCT/UENUM、序列化辅助函数。同步重整 `FNPCAgentConfig` 字段为三层组合，把 Roster 与 Act02Director 所有现有字段访问点改完，编译通过 + 跑通 ACT02 一次确认行为不退化。**额外**：写一个静态 unit test（即便项目无测试套件，写一个 console command 触发也行）遍历 schema.yaml 字段名 → 确认 USTRUCT 字段名一致。

## 涉及文件

### 新增

- `Source/AILiveProject/Public/Memory/AILiveAgentTypes.h` —— `EAILiveAgentStatus` / `EAILiveVoicePresentation` / `FAILiveAgentCore` / `FAILiveAgentIdentity` / `FAILiveAgentBattleConfig`
- `Source/AILiveProject/Public/Memory/AILiveEventTypes.h` —— `EAILivePhase` / `EAILiveEventType`（含 SpeechIntended / Bid / OrchestratorTickAnchor / OrchestratorTickResolved / OrchestratorTickAudit / ActionIntent / ActionResolved / ActionCancelled / SystemDeleteExecuted 等全枚举值）/ `EAILiveSpeechActType` / `FAILiveEvent` / `EAILiveCommitmentType` / `EAILiveCommitmentStatus` / `FAILiveCommitment` + `namespace AILiveEvent` 序列化辅助声明
- `Source/AILiveProject/Private/Memory/AILiveEventTypes.cpp` —— `PhaseToString` / `EventTypeToString` / `SpeechActToString` / `CommitmentTypeToString` / `CommitmentStatusToString` + 反向 `From*` 函数 + `ArrayToJsonString` / `JsonStringToArray`
- `Source/AILiveProject/Public/Memory/AILiveBidTypes.h` —— `FAILiveBid` / `FAILiveTickResolution`
- `Source/AILiveProject/Private/Memory/AILiveSchemaCheck.cpp` —— 显式字段映射表 + console command `AILive.CheckSchemaMapping`

### 修

- `Source/AILiveProject/Public/LLM/AILiveAgentRoster.h` —— `FNPCAgentConfig` 重整为 `Core / Identity / Battle` + `NPCIndex / NPCActorLabel / VoicePresentationHint / Provider`；保留 `ELLMProvider` 枚举与 `ProviderToString` / `ResolveProviderEndpoint`
- `Source/AILiveProject/Private/LLM/AILiveAgentRoster.cpp` —— `GetDefaultRoster()` 全量改写；**装配双轨同步**：对每个 entry 显式 `Cfg.Core.ModelProvider = ProviderToString(Cfg.Provider)`
- `Source/AILiveProject/Private/Acts/Act02RuleReceiveDirector.cpp` —— 所有 `Cfg.DisplayName` → `Cfg.Identity.FullName`、`Cfg.Voice` → `Cfg.Identity.Voice`、`Cfg.GenderHint` → `Cfg.VoicePresentationHint`（runtime 字段不入 schema.yaml）

## 依赖

blockedBy = [T0](T0_sqlitecore_openssl_setup.md)

## 验收方式

1. UBT 全量编译通过（结构体改名 → 全工程重编）
2. 跑 ACT02 一遍：10 NPC TTS / 反应链路与 T0 前一致，**无功能退化**
3. 写一个临时 unit test（CI 或手工）遍历 `AILiveAgentRoster::GetDefaultRoster()` 校验 `Cfg.Core.ModelProvider == ProviderToString(Cfg.Provider)` —— 对应 §11 L2「装配双轨同步」
4. **schema 三方对齐（显式映射表 + storage 标记校验，非裸集合相等）**：在 `Source/AILiveProject/Private/Memory/AILiveSchemaCheck.cpp` 维护一张静态 `TMap<FString /* yaml field snake_case */, FString /* USTRUCT field PascalCase */>` 显式映射表，覆盖 schema.yaml 中带 `__storage: asset` 标记的字段（即 USTRUCT 必须出现的）；console command `AILive.CheckSchemaMapping` 走这张表逐项查 UE 反射 `FProperty*` 是否存在 + 类型是否匹配。**不**做裸集合相等（snake/Pascal 不可一一映射；runtime-only 字段如 `NPCIndex` 不在 schema.yaml；storage-only 字段如 `events.parser_version` 不在 USTRUCT）。映射表手工维护，schema.yaml 增删字段时同步更新；缺漏 = 测试失败 + UE_LOG Error 列出未映射项

## 风险点

- `FNPCAgentConfig` 字段改名是**编译级破坏性变更**——所有引用点必须一拍内全改完，不能分两批 PR
- `Cfg.GenderHint`（字面 "gender"）和 `Identity.VoicePresentation`（5 项 enum，明确**非二元生理性别**，对齐 PRD AI 身份定义）语义不同——PRD 反复强调 AI 不是人类、不预置生理性别；新枚举的 5 项表达"声线呈现"（masculine / feminine / androgynous / synthetic / custom）。重整时务必把现有 `GenderHint` 字段值映射到 `VoicePresentationHint`（runtime hint 字符串）而**不**直接塞 `Identity.VoicePresentation`——后者要重新决定枚举值。
- 既有 `Saved/Logs/Act02/<session>/*.md` 不会因为本步消失（T5 才删），但字段名变化可能让 .md 内容字段名小改，团队成员若在用旧 .md 做对照需告知

## 里程碑 DevLog

`DevLog/2026-MM-DD_memory_types_skeleton.md`，记录三层 struct 设计、字段映射决策、ACT02 烟测验证结果

## 完成定义

编译通过 + ACT02 烟测通过 + 双轨同步 unit test 通过 + schema 三方对齐 unit test 通过 + 里程碑 DevLog 入库。**不**意味着 EventStore 已就绪。
