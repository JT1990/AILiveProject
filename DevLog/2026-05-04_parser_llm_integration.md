# T2.5 — Parser LLM client + 固定 system prompt + parser_version registry

日期：2026-05-04
依赖：T0（SQLiteCore + OpenSSL）+ T1（Memory/ 类型骨架）+ T2（Schema migration + EventStore 子系统骨架，DevLog 同日）。

## 目标

把"用户裁决本轮含真实 Parser LLM"的最小落地拉通：把 Reasoner raw text → 四通道结构化 JSON 的解析路径走成可独立单元测试 + 已注册 `parser_version` 的状态。本步**不**接入 RunTick（T7 范围），只为 T3（AppendEvent 写 `events.parser_version`）+ T7（reject sample 至 3 次 + abstain 兜底）准备契约面。

## 关键决策

| 决策点 | 取值 | 理由 |
| --- | --- | --- |
| Parser provider | **Qwen3** | 用户裁决覆盖最初 GLM 选择；避开 DeepSeek×4 Reasoner 的 self-play 风险，Qwen3×3 Reasoner 重叠风险可接受 |
| Parser model | **qwen3.6-plus**（`.env` 当前值） | 写 `provider:<.env model>` 真实字符串，单一真相源；运行时 `ResolveProviderEndpoint(Qwen3).Model` 取，**不**在 cpp 字面量硬编码 |
| Registry 写入位置 | **`UAILiveEventStoreSubsystem::EnsureMetaRegistry`**，BeginGame 在 EnsureSchema 后调用 | 协作者审查指出：放进 0→1 migration 无法补当前 schema_version=1 已存在的 `_meta.db`。改为幂等 `INSERT OR REPLACE`，每次 BeginGame 同步 |
| 缺段拒绝走哪一层 | **`ParseFourChannels` 入口预检 (`PrevalidateRawTaggedSections`)** | 协作者审查指出：若只校验 Parser LLM 输出，LLM 可能"修复"缺段，导致 reject 假阳性。在 LLM 调用前用本地字符串匹配兜住 4 个 tagged section 存在性 + INTENDED 体合法 JSON object |
| `GetCurrentParserPromptPath` 返回 | **相对字面量** `Content/Prompts/Parser/v1.txt` | 任务卡硬性要求；加载时由 client 内部 `FPaths::ProjectContentDir()` 展开 |
| `bRetriedWithoutResponseFormat=true` 处置 | **视为失败** | Parser 强约束 `response_format=json_object`，endpoint 拒绝即不能信任；`ErrorReason="parser endpoint rejected response_format=json_object"; FailedStage="llm_call"` |
| 缺 `QWEN3_MODEL_NAME` 处置 | **`GetCurrentParserModel()` 返回空 → `EnsureMetaRegistry` 整体 fail → BeginGame 回退** | 不写 `<missing>` 占位污染 schema_meta |
| Parser system prompt 文件位置 | **`Content/Prompts/Parser/v1.txt`** | 唯一真实路径，git 入库；后续 v2/v3 同目录递增 |
| v1.txt 失败 schema | **永远输出 4 键，不允许 `_parse_error` 旁路 schema** | 协作者审查指出：与"exactly 4 keys"自相矛盾。缺段语义改由入口预检兜住，Parser LLM 永远在 4 段齐全的输入上工作 |

## 改动清单

### 新增

| 文件 | 内容摘要 |
| --- | --- |
| `Content/Prompts/Parser/v1.txt` | Parser system prompt（按 principles §A.1 STRICT 反向编码）：4 段输入识别 + 4 键 JSON schema + 强约束子句（不修复 / 不解释 / 不输出额外字段） |
| `Source/AILiveProject/Public/LLM/AILiveParserClient.h` | `namespace AILiveParser` 导出 `FParseRequest` / `FParseResult` / `ParseFourChannels` / `PrevalidateRawTaggedSections` / `ValidateParserOutputJson` |
| `Source/AILiveProject/Public/LLM/AILiveParserVersion.h` | 4 个 `Get*` 函数声明（`GetCurrentParserVersion` / `GetCurrentParserPromptRegistryDir` / `GetCurrentParserPromptPath` / `GetCurrentParserModel`） |
| `Source/AILiveProject/Private/LLM/AILiveParserClient.cpp` | 4 阶段 ParseFourChannels（预检 → 加载 prompt → LLM 调用 → 输出校验）+ 4 个 `Get*` 实现 + 模块作用域 prompt/model lazy cache |
| `Source/AILiveProject/Private/LLM/AILiveParserSelfCheck.cpp` | console 命令 `AILive.Test.ParserSelfCheck`，跑 5 条用例（A real LLM smoke / B 缺 BID / C 坏 INTENDED / D API 值 / E 路径一致性） |

### 修

| 文件 | 改动 |
| --- | --- |
| `Source/AILiveProject/Public/Memory/AILiveEventStoreSubsystem.h` | 新增 `EnsureMetaRegistry(FSQLiteDatabase&)` 私有方法声明 |
| `Source/AILiveProject/Private/Memory/AILiveEventStoreSubsystem.cpp` | + `#include "LLM/AILiveParserVersion.h"`；BeginGame 在 `EnsureSchema(MetaDb,true)` 之后调用 `EnsureMetaRegistry(MetaDb)`，失败 → 关闭两个 db 回退；新增 `EnsureMetaRegistry` 实现：3 条 `INSERT OR REPLACE INTO schema_meta` 幂等 upsert，prepared statement (`Stmt.Create` / `SetBindingValueByIndex` / `Stmt.Execute`，bool 用法对齐 UE 5.7 `SQLitePreparedStatement.h:205`) |

### 不动（任务卡硬约束）

- `AILiveSchemaMigration.cpp`：原计划在 0→1 migration 末尾追加 INSERT 的方案**作废**（协作者审查指出无法补现有 schema_version=1 的库）
- `.uproject` plugin 列表 / `.Target.cs` / `DefaultEngine.ini` / `AILiveProject.Build.cs`

### 任务卡范围对账

任务卡 §"涉及文件 → 修" 只列 `AILiveSchemaMigration.cpp`。本步把同等改动迁到 `AILiveEventStoreSubsystem.cpp` —— 这是为兼容已存在的 `_meta.db`（避免删 db 大锤）的必要例外。`AILiveParserSelfCheck.cpp` 是任务卡未列的验收脚手架，与既有 `AILiveSchemaCheck.cpp` 同 pattern。

## 协作者审查吸收

**第 1 轮 7 条 + 第 2 轮 3 条 + 第 3 轮 3 条全部接受**（详见对应 plan 文件）。重点：

- 第 1 轮：Provider 改 Qwen3 / registry upsert 改幂等 + 移到 EventStoreSubsystem / 缺段坏 JSON 走 raw text 预检 / prompt path 返回相对字面量 / v1.txt 不允许 `_parse_error` 备用 schema
- 第 2 轮：SQLitePreparedStatement::Execute 是 bool 用法 / `bRetriedWithoutResponseFormat=true` 视为失败 / 缺 env 时不写 `<missing>` 占位
- 第 3 轮（commit `bd30973` + Case E 主连接修订）：
  - **输出 schema 类型校验**：`scratchpad` / `note_to_self` 必须 string（TryGetStringField 返回值不再忽略）；`intended.text` 必须 string；`bid.urgency` 必须 number。绕过 bid 协议必填字段的旧路径已关闭，`{"scratchpad":{},"intended":{"text":123},"bid":{},"note_to_self":[]}` 这类坏 schema 会在 Stage 3 失败而非被吞掉。
  - **Stage 1 加 `<BID>` JSON 校验**：避免 `<BID>not json</BID>` 进 Parser LLM 后被 prompt 允许的空 `{}` 输出蒙混过关。错误短语 `"bid payload not valid JSON"`。
  - **Case E 走子系统主连接**：暴露 `QueryMetaSchemaRegistry` 接口；最初 ReadOnly→ReadWrite 一行 patch 实测仍失败（详见下方 §"Case E 实测路径"）。

## 验收实证

### 编译

```
Build.bat AILiveProjectEditor Win64 Development
[1/7] Compile [x64] AILiveParserSelfCheck.cpp
[2/7] Compile [x64] AILiveEventStoreSubsystem.cpp
[3/7] Compile [x64] AILiveParserClient.cpp
[4/7] Compile [x64] Module.AILiveProject.cpp
[5/7] Link [x64] UnrealEditor-AILiveProject.lib
[6/7] Link [x64] UnrealEditor-AILiveProject.dll
[7/7] WriteMetadata AILiveProjectEditor.target

Result: Succeeded
Total execution time: 15.03 seconds
```

### PIE 流程

PIE Play → Output Log：

```
Cmd: AILive.Test.BeginGame parser_smoke_001
LogSQLiteDatabase: Opened database '.../parser_smoke_001.db'
LogSQLiteDatabase: Opened database '.../_meta.db'
LogAILiveMemory: EnsureSchema(meta): schema_version=1, no migration
LogProjectEnv: .env loaded with 18 entries
LogAILiveMemory: EnsureMetaRegistry OK: parser_version=1 parser_model=qwen3:qwen3.6-plus
LogAILiveMemory: BeginGame('parser_smoke_001') OK

Cmd: AILive.Test.ParserSelfCheck
LogAILiveMemory: [ParserSelfCheck] starting (sync D/E first; A/B/C dispatch async)
LogAILiveMemory: [ParserSelfCheck:D_ApiValues] ver='1' reg='Content/Prompts/Parser/' rel='Content/Prompts/Parser/v1.txt' model='qwen3:qwen3.6-plus' file_len=3092
LogAILiveMemory: [ParserSelfCheck:D_ApiValues] verdict=PASS ver_ok=1 reg_ok=1 rel_ok=1 model_ok=1 file_ok=1
LogAILiveMemory: Warning: AILiveParser[selfcheck_B] Stage1 reject: missing bid section
LogAILiveMemory: [ParserSelfCheck:B_MissingBid] verdict=PASS
LogAILiveMemory: Warning: AILiveParser[selfcheck_C] Stage1 reject: intended payload not valid JSON
LogAILiveMemory: [ParserSelfCheck:C_InvalidIntendedJson] verdict=PASS
LogOpenAIChat: POST https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions model=qwen3.6-plus body=3853 chars / 3857 utf8 bytes
LogOpenAIChat: OpenAIChat ok model=qwen3.6-plus latency=14177ms tokens=940/758 content_len=454 reasoning_len=1968 finish=stop
LogAILiveMemory: AILiveParser[selfcheck_A] OK in 14177ms (model=qwen3.6-plus tokens=940/758)
LogAILiveMemory: [ParserSelfCheck:A_RealLLMSmoke] bOk=1 stage=- reason=- scratch_len=56 intended_len=129 bid_len=101 note_len=22
LogAILiveMemory: [ParserSelfCheck:A_RealLLMSmoke] verdict=PASS intended_is_object=1
```

### 6 条验收用例对账

| # | 任务卡验收 | 落到哪个用例 | 结果 |
| --- | --- | --- | --- |
| 1 | 正常解析 → bOk=true + 4 段结构化 | A_RealLLMSmoke（real Qwen3 调用） | ✅ PASS — 14177ms 实调，`bOk=1 intended_is_object=1`，4 段长度均非零 |
| 2 | 缺 `<BID>` → reject + "missing bid section" | B_MissingBid（`ParseFourChannels` 整路径） | ✅ PASS — `FailedStage=raw_prevalidate / ErrorReason=missing bid section` |
| 3 | `<INTENDED>` 非合法 JSON → reject + "intended payload not valid JSON" | C_InvalidIntendedJson（`ParseFourChannels` 整路径） | ✅ PASS — `FailedStage=raw_prevalidate / ErrorReason=intended payload not valid JSON` |
| 4 | schema_meta SQL 三行精确 | 外部 sqlite3 CLI 直查 | ✅ PASS（详见下方 SQL 输出） |
| 5 | `GetCurrentParserVersion()=='1'` + `GetCurrentParserPromptPath()` 展开等于实际文件 | D_ApiValues | ✅ PASS — 5/5 ok flags（ver_ok / reg_ok / rel_ok / model_ok / file_ok）；prompt 文件 3092 字节可读 |
| 6 | runtime 拼 `registry + v + version + .txt` == `GetCurrentParserPromptPath()` | E_RegistryConsistency（走子系统主连接，第 3 轮修订后）| ✅ PASS — `kvs=4 reconstructed='Content/Prompts/Parser/v1.txt' api='Content/Prompts/Parser/v1.txt'` |

### 验收 4：sqlite3 外部 CLI

```sql
$ sqlite3 Saved/Games/_meta.db
> SELECT key, value FROM schema_meta
    WHERE key IN ('parser_version','parser_prompt_registry_path','parser_model','schema_version')
    ORDER BY key;
parser_model|qwen3:qwen3.6-plus
parser_prompt_registry_path|Content/Prompts/Parser/
parser_version|1
schema_version|1
```

3 行 parser_* 精确命中 + schema_version=1（T2 baseline 不变）。

### Case E 实测路径（已修复）

第 3 轮协作者审查指出 in-process E_RegistryConsistency 固定报 Error 误导验收。最初尝试 ReadOnly→ReadWrite 改一行（commit `bd30973`）实测仍回 SQLITE_IOERR——UE 5.7 SQLiteCore + WAL 同进程二次连接（**ReadOnly / ReadWrite 都不行**）拿不到 -shm 共享映射。

**最终方案**：暴露 `UAILiveEventStoreSubsystem::QueryMetaSchemaRegistry(TMap<FString,FString>&)`，走子系统**主连接**读 schema_meta。SelfCheck 通过 `GetEventStoreForConsole()`（用 `GEngine->GetWorldContexts()` 找 PIE/Game world → GameInstance → Subsystem）拿子系统调用。

实测输出（`parser_smoke_003`）：
```
LogAILiveMemory: [ParserSelfCheck:E_RegistryConsistency] kvs=4 reg='Content/Prompts/Parser/' ver='1' model='qwen3:qwen3.6-plus' reconstructed='Content/Prompts/Parser/v1.txt' api='Content/Prompts/Parser/v1.txt'
LogAILiveMemory: [ParserSelfCheck:E_RegistryConsistency] verdict=PASS
```

`kvs=4` = schema_meta 当前 4 行（parser_prompt_registry_path / parser_version / parser_model / schema_version）。reconstructed == api == `Content/Prompts/Parser/v1.txt`，路径一致性闭环。

**T2 SchemaSelfCheck 同症状**（继承自 T2 未改动）：仍打 ReadOnly 二次连接，依然会回 disk I/O error。T2.5 不顺手清理（任务卡硬约束）；T9 重做 SchemaSelfCheck 时建议改走子系统主连接同形 pattern。

## 风险点 / 后续

- **Parser 真实失败率统计**：本步只跑 1 次 real LLM smoke（`A_RealLLMSmoke` 14177ms）。Qwen3 实际解析率要等 T7 整合 ACT02 跑一局之后看 SystemParseFailed 频次（principles §5.4 要求 100%）。Reasoner self-play 风险（Reasoner 是 Qwen3 的 NPC×3）也要那时观测。
- **Parser endpoint 14 秒延迟**：单调用 14s 比 ACT02 串行 TTS 慢。如果整局拍数 ×10 NPC ×14s 太长，需要 (a) Parser 模型换更小的 / (b) 并发 Reasoner+Parser per-NPC 流水线。T7 整合时再调。
- **`bRetriedWithoutResponseFormat=true` 兜底**：本次 smoke 未触发（finish=stop 正常），未来 Qwen3 endpoint 风格如变化需要监控。
- **Stage 2 缺 endpoint 字段**：`.env` 改名 / `QWEN3_API_BASE` 临时缺失会让 `ParseFourChannels` 立即返回 `parser model not configured` —— 这与 BeginGame 的 EnsureMetaRegistry 一致（不写占位）。
- **`bRetriedWithoutResponseFormat` 路径**：当前若 OpenAIChat 已自动 retry without `response_format` 并成功，我们仍 fail。这避免 endpoint 不支持 json_object 时静默走非 JSON 输出污染数据。

## 完成定义对账

任务卡 §"完成定义"：
> Parser client 可独立调用并通过 4 项验收 + parser_version 写入 schema_meta + 里程碑 DevLog 入库。**不**意味着已经接入 RunTick（T7）。

- ✅ Parser client 可独立调用：`ParseFourChannels` 实测 14s 正常返回 4 段
- ✅ 4 项验收（实际 6 项）全部通过：A/B/C/D + schema_meta SQL + 路径一致性
- ✅ `parser_version=1` + `parser_prompt_registry_path=Content/Prompts/Parser/` + `parser_model=qwen3:qwen3.6-plus` 写入 `_meta.db.schema_meta`
- ✅ 本 DevLog 入库
- ✅ 不接入 RunTick（与定义一致）

T2.5 完成。下一步 T3：AppendEvent + 哈希链 + parse_failed 兜底 + tick_no。
