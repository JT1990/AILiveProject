# ACT02 规则接收（10 个 NPC + 多 LLM 并发 + TTS 串行）

日期：2026-05-02

## 目标

ACT02 是 Zombie Game 规则播完后的「开口阶段（种子）」。按 `数字键 2` 或 console `act02.start` 触发：10 NPC 散点到 TV 前 → 10 路 LLM 并发投递（DeepSeek×4 / GLM×3 / Qwen3×3 模拟 10 个 AI）→ 每个 LLM 返回 `{willingness, content}` → 选意愿最高者 TTS 播放 → 其他 9 个 NPC 的「未说出口」缓存进各自 buffer → 等待用户按 `数字键 3` / `act02.next` 进入环节2（反应阶段循环）。

LLM 决策接入前做线性推演编排，落地：

1. LLM 输出结构（5 级语义意愿 + 内容）固化为 JSON schema。
2. 多家 LLM 并发 + TTS 串行调度的最小可行调度。
3. 每次请求/响应单独写盘成 .md，便于复盘。

## 触发方式

| 操作           | 行为                                       |
| -------------- | ------------------------------------------ |
| `数字键 2`     | 触发 ACT02（仅 SceneState=Idle）           |
| `act02.start`  | 同上，console 入口                         |
| `数字键 3`     | 环节1 通过后进入环节2（仅 GatedAwaitNext） |
| `act02.next`   | 同上，console 入口                         |
| `act02.cancel` | 任意状态终止状态机                         |

## 状态机

```
Idle
 ↓ press 2 / act02.start
PrescatterToTV (复用 ScatterMover EQS 散到 TV，等所有 NPC idle)
 ↓
SeedDispatch  → SeedAwait    ← 环节1 种子：10 路并发 LLM
 ↓ all ready, 选最高意愿
SeedSpeak                    ← winner TTS（OnFinished 回调 + watchdog 30s）
 ↓ TTS 完成
GatedAwaitNext               ← 等待 数字键3 / act02.next
 ↓
ReactionDispatch → ReactionAwait  ← 环节2 第 i 轮：9 路并发（排除上一轮 speaker）
 ↓ all ready, 选最高意愿（无人接话则结束）
ReactionSpeak                ← winner TTS
 ↓ TTS 完成
 → AdvanceReactionRound（共 ReactionRoundCount=3 轮）
 ↓
Idle (Broadcast OnAct02Completed)
```

任何阶段失败 → `FailAct02(reason)` → Idle，PrintString 红色 + LogAct02 错误。

## 资产改动

### C++ 新增

- `Source/AILiveProject/Public/Acts/Act02RuleReceiveDirector.h` + `Private/Acts/Act02RuleReceiveDirector.cpp` — 状态机 / 触发 / LLM 调度 / TTS 串行 / 日志写盘
- `Source/AILiveProject/Public/LLM/OpenAIChatClient.h` + `Private/LLM/OpenAIChatClient.cpp` — OpenAI 兼容 `/v1/chat/completions` 阻塞 HTTP 客户端
- `Source/AILiveProject/Public/LLM/AILiveAgentRoster.h` + `Private/LLM/AILiveAgentRoster.cpp` — 10 NPC 配置（USTRUCT + 默认值）+ provider→endpoint 映射
- `Source/AILiveProject/Public/Util/ProjectEnvLoader.h` + `Private/Util/ProjectEnvLoader.cpp` — 通用 `.env` 解析

### C++ 修改

- `Source/AILiveProject/Public/MinimaxACELibrary.h` + `.cpp` 追加 `TriggerMinimaxSpeechFromPawnNative(..., FOnMinimaxSpeechFinishedNative OnFinished)` —— 仅 C++ 的 native delegate 变体，TTS 完成（`AnimateFromAudioSamples` 阻塞返回）后跳回 GameThread broadcast `OnFinished(bSuccess)`。原有 3 个 UFUNCTION 签名不动。

### 资产

- `Content/Blueprints/Acts/BP_Act02Director`（新）—— C++ 类的 placeable wrapper
- `Content/MyAssets/Levels/L_prison.umap` —— 放 `BP_Act02Director_C_0`，folder `/Acts/Act02`

### 不动

- `Source/*.Target.cs` `DefaultBuildSettings = V6`
- `DefaultEngine.ini` `bTickPhysicsAsync`
- ACT01 director 任何代码 / CDO
- 既有 NPC BP / GASP / Mover / VisualOverride / Animation 链路
- M / I / N / K / O / `1` 演示触发链路

## 默认 Roster（10 NPC）

| NPCIndex | Provider | Voice          | Gender | Pawn label             |
| -------- | -------- | -------------- | ------ | ---------------------- |
| 1        | DeepSeek | male-qn-qingse | male   | BP_NPC_MH_Character_1  |
| 2        | DeepSeek | female-shaonv  | female | BP_NPC_MH_Character_2  |
| 3        | GLM      | male-qn-qingse | male   | BP_NPC_MH_Character_3  |
| 4        | Qwen3    | male-qn-qingse | male   | BP_NPC_MH_Character_4  |
| 5        | DeepSeek | male-qn-qingse | male   | BP_NPC_MH_Character_5  |
| 6        | DeepSeek | female-shaonv  | female | BP_NPC_MH_Character_6  |
| 7        | GLM      | female-shaonv  | female | BP_NPC_MH_Character_7  |
| 8        | Qwen3    | female-shaonv  | female | BP_NPC_MH_Character_8  |
| 9        | GLM      | male-qn-qingse | male   | BP_NPC_MH_Character_9  |
| 10       | Qwen3    | male-qn-qingse | male   | BP_NPC_MH_Character_10 |

Provider→endpoint 通过 `.env` 解析（`DEEPSEEK_API_BASE` / `GLM_API_BASE` / `QWEN3_API_BASE`），客户端自动补 `/chat/completions`（Qwen3 已在 base url 含完整路径，跳过补全）。

## 意愿强度语义梯度

| 语义               | uint8 权 | 含义                        |
| ------------------ | -------- | --------------------------- |
| `extremely_strong` | 5        | 强烈想说，必须说            |
| `strong`           | 4        | 想说                        |
| `moderate`         | 3        | 一般                        |
| `weak`             | 2        | 不太想说但可以              |
| `none`             | 0        | 不想说（环节2 中等价 skip） |

环节2 中 `want_to_speak=false` 等价 `none`。同分按 NPC 序号升序 tie-break（确定性）。winner 取 `BestWill > 0`，全为 0 时种子阶段 FailAct02、反应阶段 CompleteAct02（"无人接话")。

## LLM JSON 输出 schema

**环节1（种子）**

```json
{ "willingness": "strong", "content": "我想知道还能不能买更多解药。" }
```

**环节2（反应）**

```json
{
  "want_to_speak": true,
  "willingness": "moderate",
  "content": "如果都不接触会变僵尸，那必须早点找搭档。"
}
```

容错解析（`OpenAIChat::ExtractFirstJsonObject`）：手写一遍最小括号深度跟踪解析器，从 `RawContent` 抽出第一个完整 `{...}` 段，处理转义和字符串内大括号；解析失败 → `willingness=none, content=""`，`parse_error=true` 写入 frontmatter。

## 关键技术决策 / 踩坑

### 1. ACT02 不可先 ResetToInitialPositions（用户视觉错觉）

**踩坑**：第一版 BeginAct02 = `ResetToInitialPositions()`（瞬移回 BeginPlay 时位置 = 牢房）→ `StartPrescatter()`（散到 TV）。用户按 1 跑 ACT01 → NPC 在 TV 前 → 按 2 → NPC 闪回牢房 → 重新散到 TV，**视觉上误判为「ACT01 又跑了一次」**。

**修复**：BeginAct02 直接 `StartPrescatter()`，省掉 reset。

- 若 ACT01 已跑过，NPC 已在 TV 前，scatter 仅做小幅调整。
- 若 ACT02 单独触发（NPC 在牢房），scatter 通过 NavMesh 寻路（门 `bCanEverAffectNavigation=False` 由 ACT01 milestone 设过）。
- `ResetToInitialPositions` 函数保留供未来调试，当前路径不调用。

### 2. CacheInitialNPCTransforms 必须过滤玩家 Pawn

`UGameplayStatics::GetAllActorsOfClass(NPCMoverClass=SandboxCharacter_Mover_C)` 会**包含玩家 Pawn**（玩家也用同一个父类）。第一版只过滤 `Cast<APawn>(A)`，玩家也是 Pawn，于是被缓存进 `InitialNPCTransforms`。如果未来调用 `ResetToInitialPositions`，玩家会被传送回 spawn，体验奇怪。

修法：增加 `!Pawn->IsPlayerControlled()` 过滤，与 `AILiveProjectScatterMover` 内部一致。

### 3. GLM 严格校验 temperature 字面量精度（IEEE float 暴露 → 1210）

**症状**：GLM 3 个 NPC 全部返回 HTTP 400 + 错码 `1210 "API 调用参数有误"`，DeepSeek/Qwen3 正常。

**根因**：UE `TJsonWriter` 用 `%.17g` 格式化 double，把 `float Temperature = 0.7f` 提升 double 后输出 IEEE 全精度 `0.69999998807907104`。GLM v4 PaaS gateway 有字符串级参数校验：

- `"temperature": 0.7` → 200 OK
- `"temperature": 0.69999998807907104` → 400 / 1210

curl 直接复刻 UE body 字节 → 同样 400，证明是 body 内容（不是 header）。

**文档依据**：

- [OpenAI 兼容 API](https://docs.bigmodel.cn/cn/guide/develop/openai)：`temperature 区间为 (0,1)` 开区间。
- [API 错误码 1210](https://docs.bigmodel.cn/cn/faq/api-code)：触发条件包含「Parameter format: Values must match their specified data types」+「Value constraints」。
- [HTTP API](https://docs.bigmodel.cn/cn/guide/develop/http)：官方示例统一用 `"temperature": 1.0` 干净十进制字面量。

**修法**：用 `FJsonValueNumberString` 显式控制数字字面量字符串：

```cpp
const FString TempStr = FString::SanitizeFloat(Req.Temperature, /*InMinFractionalDigits=*/1);
Root->SetField(TEXT("temperature"), MakeShared<FJsonValueNumberString>(TempStr));
```

`SanitizeFloat(0.7f, 1)` → `"0.7"`，绕过 TJsonWriter 的 `%.17g` 精度暴露。

### 4. GLM-5.1 是 reasoning 模型，必须显式 `thinking: {type: disabled}`

**症状**：UTF-8 + temperature 字面量都修了之后，GLM 偶发 `content` 空、`reasoning_content` 满（hit max_tokens 上限）。`finish_reason=length`。

**根因**：[GLM-5.1 模型卡](https://docs.bigmodel.cn/cn/guide/models/text/glm-5.1) 默认开启 thinking 模式，把全部 token 消耗在思考链 `reasoning_content` 字段，最终 `content` 为空。max_tokens=800 在 reasoning 模型里很容易触顶。

**修法**：`OpenAIChat::FRequest` 加 `ThinkingType` 字段，Director 在 GLM 路径下设 `Req.ThinkingType = "disabled"`。BuildBody 序列化为 `"thinking": {"type": "disabled"}` 字段。这是 GLM API 对 reasoning 模型的关闭开关，关闭后内容直接落到 `content`。

DeepSeek / Qwen3 不需要该字段（`ThinkingType` 默认空字符串，BuildBody 跳过该字段）。

### 5. UE `SetContentAsString` UTF-8 不可靠 → 显式 byte encode

**踩坑**：第一版 GLM 返回 `JSON parse error: Invalid UTF-8 middle byte 0xe3`。UE 5.7 的 `IHttpRequest::SetContentAsString` 在某些路径上按 FString 原始字节（Windows 平台 = UTF-16 LE）拷贝，没有自动 UTF-8 转码。MiniMax 服务端容错才没事，GLM 严格 UTF-8 校验直接拒。

**修法**：

```cpp
const FTCHARToUTF8 Utf8(*Body);
TArray<uint8> Bytes;
Bytes.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
Http->SetContent(Bytes);
Http->SetHeader(TEXT("Content-Type"), TEXT("application/json; charset=utf-8"));
```

显式 `FTCHARToUTF8` 转 UTF-8 字节后用 `SetContent(TArray<uint8>)`。

### 6. HTTP 400 + response_format 自动 retry 兜底

LLM 端点偶尔不接 `response_format: {"type":"json_object"}`（OpenAI 兼容子集差异）。Client 收到 HTTP 400 时**自动 retry 一次**去掉 `bResponseFormatJson`，结果带 `bRetriedWithoutResponseFormat=true` 标志写入 .md frontmatter。即使 retry 失败也保留两次的错误信息。

### 7. TTS 完成检测 = 加 native delegate 变体

`AnimateFromAudioSamples` 是 ACE 的阻塞流式调用（持续与音频时长相当）。在 worker 线程 return 时音频已基本播完，时机精度足够。新加 `UMinimaxACELibrary::TriggerMinimaxSpeechFromPawnNative(..., FOnMinimaxSpeechFinishedNative)`：成功路径 `AsyncTask(GameThread, [Cb](){ Cb(true); })`；HTTP 失败 / pawn 失效 / 编码失败路径 `Cb(false)`。Director 用 `FOnMinimaxSpeechFinishedNative::CreateLambda(weakSelf → HandleSpeechFinished)`，避开 Dynamic 委托和 BP 包装。

watchdog 30s 兜底防 ACE/A2F 卡死。

### 8. UPlayerController + WasInputKeyJustPressed 不冲突

ACT01 用 `EKeys::One`，ACT02 用 `EKeys::Two`/`EKeys::Three`。两个 director 都在 Tick 里轮询 `WasInputKeyJustPressed(StartKey)`，但 PC 的按键状态是 per-key 的，互不干扰。`EKeys::Three` 仅在 `SceneState==GatedAwaitNext` 时响应，非该状态按 3 无副作用。

### 9. 调试 dump：UE 实际发送的 body 字节落盘

每次 LLM 请求把 UTF-8 编码后的 body bytes dump 到 `Saved/Logs/OpenAIChat/last_<model>.json`（按 model 名分桶，覆盖式）。定位 GLM 1210 时，正是从该 dump 看到 `"temperature":0.69999998807907104` 才锁定问题。生产模式可以 #if 0 关闭，开发期保留。

## LLM 输入/输出落盘

每次请求/响应单独写一个 .md：

```
Saved/Logs/Act02/<session-时间戳>/
├── seed/
│   ├── npc_01_deepseek.md  ... npc_10_qwen3.md
│   └── _winner.md
└── reaction_round_01/  reaction_round_02/  reaction_round_03/
    ├── npc_NN_<provider>.md (9 个，排除上一轮 speaker)
    └── _winner.md
```

每个 npc\_\*.md 含：

```markdown
---
session: 2026-05-02_203332
phase: seed | reaction
round: 0..N
npc_index: 1..10
provider: deepseek | glm | qwen3
model: <model name>
http_status: 200 | 400 | ...
latency_ms: 2334
prompt_tokens: 600
completion_tokens: 49
finish_reason: stop | length | none
retried_without_response_format: true | false
parsed_willingness: extremely_strong | strong | moderate | weak | none
parsed_want_to_speak: true | false
parse_error: true | false
---

## System <- 完整 system prompt

## User <- 完整 user prompt

## Error message <- 失败原因，成功留 (empty)

## Raw response <- choices[0].message.content

## Reasoning content <- reasoning 模型独有，失败诊断关键

## Parsed JSON <- 抽出的 {...} 段

## Full response payload <- 完整 HTTP body（成功和失败都写）
```

`_winner.md` 含赢家 + 9 个未说出口（buffer 状态）。

写盘走 `FFileHelper::SaveStringToFile + ForceUTF8WithoutBOM`。Session 目录 = `BeginAct02()` 那一刻 `FDateTime::Now()`，一次 ACT02 共用，多次运行不互相覆盖。

## 「未说出口」buffer 更新规则

- 种子结束：winner.UnspokenContent = ""（已说），其他 9 个保留各自 content。
- 每轮反应结束：winner.UnspokenContent = ""，其他 9 个 NPC 中：`want_to_speak=true` 且非空 → 替换 buffer；`want_to_speak=false` 或空 → 保留旧未说。
- BeginAct02 时全部 NPC.UnspokenContent 清空。

## PIE 验证（已通过 2026-05-02 环节1）

按 `2`：

```
LogAct02: [Act02] StartKey pressed
LogAct02: [Act02] dispatched 10 NPC(s) via EQS scatter around NavTarget
... NPC 散到 TV 前 idle
LogAct02: [Act02] seed dispatch: 10 LLM in flight
LogOpenAIChat: POST .../chat/completions model=deepseek-v4-flash body=NN chars / MM utf8 bytes
LogOpenAIChat: POST .../chat/completions model=glm-5.1 body=NN chars / MM utf8 bytes
LogOpenAIChat: POST .../chat/completions model=qwen3.6-plus body=NN chars / MM utf8 bytes
... × 10 路并发
LogOpenAIChat: OpenAIChat ok model=glm-5.1 latency=2334ms tokens=600/49 content_len=49 reasoning_len=0 finish=stop
... 10 条
LogAct02: [Act02] LLM glm npc=3 http=200 latency=2334ms willingness=extremely_strong parse_error=0
... 10 条
LogAct02: [Act02] seed winner: NPC03 willingness=extremely_strong content="初始僵尸喝解药无效..."
LogAct02: [Native] dispatching N samples @ 16000 Hz (duration ...)
LogAct02: [Act02] speech finished npc=3 failed=0 elapsed=...s
LogAct02: [Act02] seed phase complete; press 3 / act02.next to continue
```

实测会话（`2026-05-02_203332`）：

| 检查项                    | 期望              | 实测           |
| ------------------------- | ----------------- | -------------- |
| 10 LLM 全 ready           | http=200 ×10      | **10/10 ✓**    |
| GLM 3 个不再为空          | content_len > 0   | **3/3 ✓**      |
| .md 写盘                  | 10 npc + 1 winner | **11 个 ✓**    |
| frontmatter `parse_error` | false             | **全 false ✓** |
| TTS winner 嘴动+音频      | 有                | **有 ✓**       |
| 状态 → GatedAwaitNext     | 有                | **有 ✓**       |
| 回归 M/I/N/K/O/键1        | 不受影响          | **正常 ✓**     |

环节2（反应阶段循环）已实现，等待用户跑过 3 轮验证后续完善。

## 已知限制 / 后续

- `ReactionRoundCount` 默认 3，验证后调到 10 跑长链。
- `_winner.md` 的"未说出口"是当前 buffer 快照，不含历史 ring。后续接入 PRD「长期记忆」需把整个 session 灌到 Neo4j（`qwen3-embedding:8b` 已本地部署）。
- 调试 dump（`Saved/Logs/OpenAIChat/last_*.json`）目前一直开。生产可加 `bDumpRequestBody` UPROPERTY 关掉。
- ACT01 → ACT02 不自动衔接（手按 1 → 2）。后续 LLM Mind 接入时由 director 链或剧本编排框架统一调度，按键全部移除。
- TTS 串行：当前每轮只 winner 播 TTS，PRD"AI 不同时说话"原则吻合。多 winner 并发场景未来再议。
- 抽 `AActChapterBase`（StartKey 轮询 / 缓存初始 transform / ScatterMover 调度 / console 注册）—— ACT01 + ACT02 现在两份样板，第三幕落地时一起抽。
- `EWillingness` 同分 tie-break 当前按 NPC 序号升序（确定性）。后续可换"上次发言时间最久的优先"或"关系图谱权重"。

## 关键文件清单

新建：

- `Source/AILiveProject/Public/Acts/Act02RuleReceiveDirector.h` + `Private/Acts/Act02RuleReceiveDirector.cpp`
- `Source/AILiveProject/Public/LLM/OpenAIChatClient.h` + `Private/LLM/OpenAIChatClient.cpp`
- `Source/AILiveProject/Public/LLM/AILiveAgentRoster.h` + `Private/LLM/AILiveAgentRoster.cpp`
- `Source/AILiveProject/Public/Util/ProjectEnvLoader.h` + `Private/Util/ProjectEnvLoader.cpp`
- `Content/Blueprints/Acts/BP_Act02Director`

修改：

- `Source/AILiveProject/Public/MinimaxACELibrary.h` + `.cpp`：追加 `TriggerMinimaxSpeechFromPawnNative` + `FOnMinimaxSpeechFinishedNative` 类型；原 3 个 UFUNCTION 不动。
- `Content/MyAssets/Levels/L_prison.umap`：放 `BP_Act02Director_C_0`。
