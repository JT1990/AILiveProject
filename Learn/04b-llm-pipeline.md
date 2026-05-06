# 阶段 4b — LLM 双管线

读：

```
Public/LLM/OpenAIChatClient.h        (40 行 — Request/Result 契约)
Private/LLM/OpenAIChatClient.cpp     (~270 行 — 同步 HTTP + 重试)
Public/LLM/AILiveParserClient.h
Public/LLM/AILiveParserVersion.h     (12 行 — 4 个版本/模型查询)
Private/LLM/AILiveParserClient.cpp   (~430 行 — 三阶段验证 + LLM 中转)
Public/Memory/AILivePromptAssembler.h
Private/Memory/AILivePromptAssembler.cpp  (~510 行 — 9 段装配 + 降级)
Public/LLM/AILiveAgentRoster.h
Private/LLM/AILiveAgentRoster.cpp    (~115 行 — endpoint 解析 + 默认花名册)
Public/Util/ProjectEnvLoader.h
Content/Prompts/Parser/v1.txt        (Parser system prompt — 真相源)
```

读完这一阶段你应当能回答：

1. Reasoner 和 Parser **是不是同一个 HTTP 客户端**？为什么 Parser 要再调一次 LLM 而不是纯 regex？
2. `OpenAIChat::RequestBlocking` 失败后会不会自动重试？什么情况会重试？
3. PromptAssembler 的 7 段哪些是 **must-keep**？降级时怎么砍？
4. Parser 用 DeepSeek 还是其他供应商？为什么和 Reasoner 不一定一致？

---

## 1. 整条管线的形状

```
Director.RunTick()
  ↓ for each NPC（worker 线程）
        ↓
        ① BuildReasonerSystemPrompt + AssembleUserPrompt（PromptAssembler）
        ↓
        ② Reasoner LLM = OpenAIChat::RequestBlocking
              SystemPrompt: 4 通道契约 + 角色配置
              UserPrompt:   PromptAssembler 7 段
              ↓ raw_text（含 <SCRATCHPAD>...</SCRATCHPAD> 等 4 个 tagged section）
        ↓
        ③ Parser LLM = AILiveParser::ParseFourChannels
              Stage 1: 预校验 raw_text 4 个 tag + intended/bid 是 JSON
              Stage 2: 把 raw_text 当 user prompt 喂给 Parser LLM（用 v1.txt 当 system prompt）
              Stage 3: 校验 Parser LLM 返回的 JSON schema（4 keys + intended.text + bid.urgency）
              ↓ FParseResult { Scratchpad, IntendedJson, BidJson, NoteText }
        ↓
        ④ 失败 → 重试（worker 内 3 次循环，每次新 Reasoner 调用）→ 仍败 → bAbstain=true
```

**两个 LLM 调用职责分离**：

- **Reasoner** = 角色扮演 / 决策。可能用 4 通道 tagged 输出（不是严格 JSON），也可能输出半结构化文本。**不强 JSON**——给推理留呼吸空间。
- **Parser** = 把 tagged 文本翻译成严格 4-key JSON。**强 `response_format=json_object`**；如果 endpoint 拒绝，整体直接判失败（不像 Reasoner 那样降级）。

为什么 Parser 不能用纯 regex？因为 `<INTENDED>` body 内的 JSON 可能含 `</INTENDED>` 字面量、嵌套 `}`、转义引号——靠正则切分会被边角 case 反复打脸。**用 LLM 做"格式翻译"**，是把"我读懂结构"这种模糊判断外包给模型，规则化层只校验 schema。

---

## 2. `OpenAIChat::RequestBlocking`（cpp:114-265）

40 行 header 看不出任何坑——所有暗礁都在 cpp 的 ~150 行里。**这个函数你以后会高频改，把每个奇怪写法的原因记牢。**

### 2.1 三处"看似多此一举"的细节都是踩过坑

**(a) `temperature` 用字符串字面量包装**（cpp:23-28）

```cpp
const FString TempStr = FString::SanitizeFloat(Req.Temperature, /*InMinFractionalDigits=*/1);
Root->SetField(TEXT("temperature"), MakeShared<FJsonValueNumberString>(TempStr));
```

注释原话：

> *"直接 SetNumberField(double) 会让 UE TJsonWriter 用 "%.17g" 输出，把 float→double 的 IEEE 精度损失暴露成 `0.69999998807907104`，GLM 等严格校验的 endpoint 拒绝。"*

不能用 `SetNumberField(0.7)`——必须先把 float SanitizeFloat 成 `"0.7"`，再用 `FJsonValueNumberString` 让 writer 当**预序列化的数字字面量**直接拼进去。

**(b) UTF-8 显式编码 body**（cpp:147-150）

```cpp
const FTCHARToUTF8 Utf8(*Body);
TArray<uint8> Bytes;
Bytes.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
Http->SetContent(Bytes);
```

注释原话：

> *"UE 的 SetContentAsString 在某些平台/版本上按 FString 原始字节（UTF-16 LE）拷贝，GLM 等严格 UTF-8 校验的 endpoint 会回 400 / parse error。"*

**别用 `SetContentAsString`**。你以后扩展时的反射性正确选项是 `SetContent(UTF-8 bytes)`。

**(c) 显式覆盖 UE 默认 User-Agent / Accept / Accept-Encoding**（cpp:154-156）

```cpp
Http->SetHeader(TEXT("User-Agent"), TEXT("AILiveProject/1.0"));
Http->SetHeader(TEXT("Accept"), TEXT("application/json"));
Http->SetHeader(TEXT("Accept-Encoding"), TEXT("identity"));
```

注释原话：

> *"某些 LLM 网关（GLM v4 PaaS）会拒绝没有 Accept 或 UE 默认 UA 的请求，错码 1210 'API 调用参数有误'。"*

`Accept-Encoding: identity` 表示"不要给我 gzip/deflate"——避免 UE HTTP 客户端在某些版本上漏了解压逻辑。

### 2.2 调试 dump（cpp:162-170）

每次请求都把 body 写到：

```
<ProjectDir>/Saved/Logs/OpenAIChat/last_<model>.json
```

> 调 LLM 出诡异 400 的时候，**第一反应是 dump 这个文件**——不要去 reproduce 整条调用链。

### 2.3 同步阻塞：`ProcessRequestUntilComplete`（cpp:172）

**这是为什么 RequestBlocking 必须跑在 worker 线程**。`ProcessRequestUntilComplete` 在 GT 上会卡死消息循环——10 个 NPC 串行 × 60s = PIE 直接锁住。Director 用 `Async(ThreadPool, RunAgentTickInWorker)` 把每个 NPC 调用扔到 worker，main thread 只 poll 它们的 TFuture。

### 2.4 HTTP 400 + json_object 自动重试（cpp:191-207）

```cpp
if (Result.HttpStatus == 400 && Req.bResponseFormatJson)
{
    UE_LOG(Warning, "OpenAIChat HTTP 400; retrying without response_format");
    FRequest RetryReq = Req;
    RetryReq.bResponseFormatJson = false;
    FResult RetryResult = RequestBlocking(RetryReq);     // 递归一次
    RetryResult.bRetriedWithoutResponseFormat = true;     // 通知 caller
    return RetryResult;
}
```

**只在 400 且原请求要求 JSON 才降级**。`bRetriedWithoutResponseFormat=true` 是回执标记——**Parser 看到这个标记会直接判失败**（cpp:395-406）：

```cpp
if (LLMResult.bRetriedWithoutResponseFormat) {
    R.ErrorReason = TEXT("parser endpoint rejected response_format=json_object");
    return R;  // Parser 必须 JSON，降级即失败
}
```

Reasoner 反过来无所谓——它本来就允许返回半结构化 tagged 文本。

### 2.5 响应解析：兼容 reasoning_content（cpp:238-256）

```cpp
(*Message)->TryGetStringField("content", Result.RawContent);
(*Message)->TryGetStringField("reasoning_content", Result.ReasoningContent);

Result.ParsedJson = ExtractFirstJsonObject(Result.RawContent);
if (Result.RawContent.IsEmpty() && !Result.ReasoningContent.IsEmpty()) {
    Result.ErrorMessage = TEXT("Response content is empty; reasoning_content is present");
}
```

**DeepSeek-R1 系列 / GLM Thinking** 会把推理过程放在 `reasoning_content`，最终答案放在 `content`。如果 model 走神到只填了 `reasoning_content`、留 `content` 空，这里给个明确的 errorMessage 让你知道是怎么回事。

### 2.6 `ExtractFirstJsonObject`（cpp:71-112）—— 手撸的 JSON 边界扫描

```cpp
// 找首个 '{'
// 从该位置往后维护 Depth + bInString + bEscape
// Depth 回到 0 即为闭合的对象
```

**为什么不用 FJsonSerializer 直接 deserialize 整个 RawContent？**

因为 LLM 经常会输出 ```` ```json\n{...}\n``` ```` 这种 markdown 包装。直接 deserialize 会失败，但只要 RawContent 里**藏着**一个合法 JSON 对象，这里能挖出来。这是 `bResponseFormatJson` 不可用时的兜底。

---

## 3. `AILiveParserClient`（cpp）

Parser 是个**有状态**的命名空间——它有模块作用域 cache。

### 3.1 三个进程级 cache（cpp:32-39）

```cpp
FCriticalSection GParserPromptLock;
bool             GParserPromptLoaded = false;     // 是否尝试过加载
bool             GParserPromptLoadOk = false;     // 加载是否成功
FString          GParserSystemPrompt;             // v1.txt 内容

FCriticalSection GParserModelLock;
bool             GParserModelResolved = false;
FString          GParserModelCached;              // "deepseek:DeepSeek-V3.1" 等
```

**v1.txt 在首次 ParseFourChannels 调用时同步加载**（cpp:46-73），加载失败永久标记 `GParserPromptLoadOk=false`，**不重试**——保证模块行为可预测，启动期就发现 prompt 缺失。

### 3.2 Parser provider 是写死的（cpp:25）

```cpp
constexpr ELLMProvider kParserProvider = ELLMProvider::DeepSeek;
```

注释（cpp:18-23）讲了一段历史：

> *"T7 烟测决定，2026-05-04：原 Qwen3 选型在 10 NPC 并发时 dashscope endpoint 限流，HTTP 0 / reasoner_timeout 失败率 70%+；切 DeepSeek 后 winner 路径可见，L1 #1/#2 才能机械验。"*

**Reasoner 的 provider 由 Roster 决定（每个 NPC 不同）；Parser 永远走 DeepSeek**。这是因为 Reasoner 才是"角色扮演的多样性"来源，Parser 是单一的格式翻译器，**用最稳的供应商即可**。

### 3.3 三阶段（cpp:334-427）

```cpp
FParseResult ParseFourChannels(const FParseRequest& Req)
{
    // === Stage 1: raw text 预检（lock-free，纯 regex/JSON 校验）
    FParseResult Pre = PrevalidateRawTaggedSections(Req.RawText);
    if (!Pre.bOk) return Pre;        // FailedStage="raw_prevalidate"
    
    // === Stage 2: LLM 调用
    if (!EnsureSystemPromptLoaded()) return /* prompt file not found */;
    Ep = AILiveAgentRoster::ResolveProviderEndpoint(kParserProvider);
    if (Ep.ApiKey/Endpoint/Model 任一空) return /* parser model not configured */;
    
    LLMReq.SystemPrompt = GParserSystemPrompt;     // v1.txt
    LLMReq.UserPrompt   = Req.RawText;             // Reasoner 的 raw 输出
    LLMReq.Temperature  = 0.f;                     // 翻译任务用 0
    LLMReq.bResponseFormatJson = true;
    
    LLMResult = OpenAIChat::RequestBlocking(LLMReq);
    if (!LLMResult.bSuccess) return /* parser LLM call failed */;
    if (LLMResult.bRetriedWithoutResponseFormat) return /* endpoint rejected JSON mode */;
    
    // === Stage 3: 输出 JSON 校验
    return ValidateParserOutputJson(LLMResult.ParsedJson | RawContent);
}
```

每个 stage 都用 `FailedStage` 字段标识失败位置——上层用它做日志归类（cpp:341-343）。

### 3.4 Stage 1 预检（cpp:163-226）

```cpp
const FTagSpec Tags[] = {
    { "<SCRATCHPAD>",   "</SCRATCHPAD>",   "missing scratchpad section" },
    { "<INTENDED>",     "</INTENDED>",     "missing intended section" },
    { "<BID>",          "</BID>",          "missing bid section" },
    { "<NOTE_TO_SELF>", "</NOTE_TO_SELF>", "missing note_to_self section" },
};

for (T : Tags) {
    if (!ExtractTagBody(Raw, T.Open, T.Close, Body)) return /* missing */;
    if (T == "<INTENDED>") IntendedBody = Body;
    if (T == "<BID>")      BidBody      = Body;
}

if (!TryParseJsonObject(IntendedBody, ...)) return /* intended payload not valid JSON */;
if (!TryParseJsonObject(BidBody, ...))      return /* bid payload not valid JSON */;
return /* ok */;
```

**预检的目的是防止把垃圾喂给 Parser LLM**——4 个 tag 必须齐、其中 INTENDED/BID 必须本身就是 valid JSON。这一步**不调 LLM**——便宜、确定、可单元测试。

### 3.5 Stage 3 输出校验（cpp:228-332）

校验 Parser LLM 返回的 4-key JSON：

| 键 | 类型 | 必需子字段 |
|---|---|---|
| `scratchpad` | string | — |
| `intended` | object | `text` (string) |
| `bid` | object | `urgency` (number) |
| `note_to_self` | string | — |

**`intended.text` 必填**——这是一句"我想说的话"的 ground truth。
**`bid.urgency` 必填且必须是 number**——principles §5.2bis 要求，floor 解算靠它。
其它字段（`intended_action` / `addressed_to_hint` / `bid.proposed_target` / `bid.relates_to_seq` / `bid.rationale`）**Parser 透传不强校**。

注意：`SerializeJsonObjectCondensed`（cpp:108-119）把 IntendedPtr / BidPtr 序列化回字符串——存到 `FParseResult.IntendedJson` / `BidJson`。**caller 拿到的是序列化字符串而不是 FJsonObject**，因为这两个 JSON 直接进 events 表的 payload 列。

---

## 4. `AILivePromptAssembler`（cpp）

PromptAssembler 是命名空间纯函数——**无状态、无 LLM 调用、不读文件**。所有数据来自 EventStore 读 API。

### 4.1 SystemPrompt 是个 hardcode 模板（cpp:273-298）

```cpp
return FString::Printf(
    "你是 AI %s，%s。\n"
    "游戏规则：%s\n\n"
    "[INVARIANT REMINDERS]\n"
    "- 你是 AI，没有人类背景；只有外观符号（名字 / 昵称 / 性别 / 声线）。\n"
    "- 必须用严格 JSON 回答；不输出任何 JSON 之外的文字。\n\n"
    "[CURRENT TICK]\n"
    "现在第 %d 轮 day_discuss。请阅读 user prompt 中的事件流...\n\n"
    "[OUTPUT SCHEMA — 严格 JSON]\n"
    "{\"want_to_speak\": true|false, \"willingness\": ..., \"content\": \"...\"}\n",
    *Cfg.Identity.FullName, *Cfg.VoicePresentationHint,
    *Opt.GameRule, Opt.CurrentRound);
```

**注意 SystemPrompt 当前还在 ACT02 legacy schema**（want_to_speak / willingness / content）——**而不是** 4 通道 SCRATCHPAD/INTENDED/BID/NOTE。注释（cpp:277-278）写得很清楚：

> *"**不**引入 T7 四通道认知措辞——ACT02 仍走 legacy JSON `{ want_to_speak, willingness, content }`（ParseAnswer 依赖）。"*

**这意味着 PromptAssembler 当前**和** T7 双 LLM 管线**没有完全对齐**——SystemPrompt 还是老格式，但 Reasoner 在 RunAgentTickInWorker 里走的是 `BuildReasonerSystemPrompt()`（在 Director 里另写）。**PromptAssembler 只用 UserPrompt 部分**——这是阶段 5 主循环要弄清的。

### 4.2 UserPrompt 7 段装配（cpp:300-356）

```cpp
TArray<FSectionEntry> Sections;
Sections.Add({ "OwnHistory",        ...,  bMustKeep=true  });   // 必保留 3
Sections.Add({ "PendingIntended",   ...,  bMustKeep=true  });   // 必保留 4
Sections.Add({ "Notes",             ...,  bMustKeep=false });   // 必保留 5（可降）
Sections.Add({ "Commitments",       ...,  bMustKeep=false });   // 必保留 6（可降）
Sections.Add({ "NearWindow",        ...,  bMustKeep=true  });   // 必保留 7
Sections.Add({ "PrivateChats",      ...,  bMustKeep=false });   // 必保留 8（可降）
Sections.Add({ "ChallengePrefetch", ...,  bMustKeep=false });   // §4.4（条件输出）
```

**注意**：`bMustKeep` 字段定义了"绝不能丢的段"，但 cpp:319-332 的降级逻辑**实际上**只看段名硬编码丢哪几段：

```cpp
if (TotalLen > kPromptCtxLimit) DropSectionByName("PrivateChats");
if (TotalLen > kPromptCtxLimit) DropSectionByName("Notes");
if (TotalLen > kPromptCtxLimit) DropSectionByName("Commitments");

if (FinalLen > kPromptCtxLimit) {
    UE_LOG(Error, "...降级后仍超限...");
    ensureAlwaysMsgf(...);     // 留 callstack 但不 abort
}
```

`bMustKeep` 字段**目前只在文档级生效**——降级逻辑硬编码了"先丢 PrivateChats、再丢 Notes、最后丢 Commitments"的优先级。OwnHistory / PendingIntended / NearWindow 永远不被丢——这就是 §4.3 的 must-keep。

**`kPromptCtxLimit = 64000`**（cpp:25），约 21K tokens（中文 ~3 char/token），DeepSeek/GLM/Qwen3 32K context 都能装下。

### 4.3 各段的查询 API（cpp:86-204）

| 段 | EventStore 调用 | 过滤条件 |
|---|---|---|
| OwnHistory | `ListMyStatements(Viewer)` | actor=Viewer 的 speech.public ∪ private_msg |
| PendingIntended | `ListMyPendingIntended(Viewer, K)` | 最近 K 拍 actor=Viewer 的 speech.intended，**且没对应 public** |
| Notes | `ListMyNotes(Viewer, K)` | actor=Viewer 的 speech.note，最近 K 条 |
| Commitments | `ListMyCommitments(Viewer, -1, -1)` | 投影表 `commitments` 查询（T8 reducer 写的） |
| NearWindow | `QuoteRecentRounds(CurrentRound, K, Viewer)` + 客户端过滤 actor != Viewer + visibility 含 'public' | 最近 K 拍他人公开发言 |
| PrivateChats | 同上 + 客户端过滤 actor != Viewer + visibility 不含 'public' 含 Viewer | 私聊给 Viewer |
| ChallengePrefetch | `ExtractRoundRefs(ChallengeText)` 抽"第 N 轮" → `QuoteRecentRounds(round, 1, Viewer)` | §4.4 |

**`ChallengePrefetch` 的关键陷阱**（cpp:206-208）：

```cpp
// **不能**用 QuoteByRound(round, "", Viewer)：SQL 强制 e.actor = ?3，空串匹配 0 条。
```

记住：要"某轮所有事件"，必须走 `QuoteRecentRounds(round, 1)`——`QuoteByRound` 是 actor-targeted 的。

### 4.4 `ExtractRoundRefs`（cpp:50-66）—— 中文正则

```cpp
const FRegexPattern Pat(TEXT("第\\s*(\\d+)\\s*轮"));
FRegexMatcher M(Pat, InChallengeText);
while (M.FindNext()) {
    Out.AddUnique(FCString::Atoi(*M.GetCaptureGroup(1)));
}
```

**只识别中文模式 "第 N 轮"**——英文 "round 5" 抓不到。如果国际化时要扩，记得别破坏现有匹配。

### 4.5 `FormatEventLine`（cpp:69-79）—— principles §7 模板

```cpp
return FString::Printf(
    "Tick %03lld round_no=%02d %s seq=%lld: \"%s\"",
    Ev.TickNo, Ev.RoundNo, PhaseToString(Ev.Phase), Ev.Seq, *Text);
```

**这个模板被 prompt 里所有事件行复用**——任何修改会影响全部 prompt 输出。`tick_no` 在这里被消费——这就是 4a 阶段提过的"`tick_no` 进 events 表 + 进内存结构 + 不进哈希链"三套同步而独立的**第二处用途**。

---

## 5. 控制台命令（cpp:402-512）

Parser/PromptAssembler 提供两个 `FAutoConsoleCommand`，用于 §11 L1 验收：

```
AILive.Test.AssemblePendingIntended <agent_id>
    → 写一条 speech.intended → AssembleUserPrompt → 检查输出含
      [YOUR RECENT INTENDED-BUT-NOT-SAID] 段 + 原文

AILive.Test.AssembleChallenge <agent_id> <round_no>
    → 在指定 round 写一条 NPC03 的 public speech → 用"第 N 轮"作 ChallengeText
    → AssembleUserPrompt → 检查输出含 [PREFETCHED EVIDENCE FROM REFERENCED ROUND N]
```

**调试时直接在 Output Log 输 `AILive.Test.AssemblePendingIntended NPC01`**——它会把整段 user prompt log 出来，比 dump 文件更快。

---

## 6. `AILiveAgentRoster::ResolveProviderEndpoint`（cpp:67-113）

```cpp
case ELLMProvider::DeepSeek:
    Out.ApiKey   = ProjectEnvLoader::Get("DEEPSEEK_API_KEY");
    Out.Endpoint = ProjectEnvLoader::Get("DEEPSEEK_API_BASE");
    if (!Out.Endpoint.IsEmpty() && !Out.Endpoint.Contains("/chat/completions")) {
        Out.Endpoint += Out.Endpoint.EndsWith("/") ? "chat/completions" : "/chat/completions";
    }
    Out.Model = ProjectEnvLoader::Get("DEEPSEEK_MODEL_NAME");
    break;

case ELLMProvider::GLM:    // 同 DeepSeek 的自动补全
case ELLMProvider::Qwen3:  // **不**做 endpoint 自动补全（dashscope 路径不一样）
```

**为什么 DeepSeek/GLM 自动补 `/chat/completions` 但 Qwen3 不补？**

`.env` 里：

```
QWEN3_API_BASE=https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions
```

阿里 dashscope 的 OpenAI 兼容路径是固定的——你**手填整段路径**就行。DeepSeek/GLM 的 base URL 形式更宽松（`api.deepseek.com` / `open.bigmodel.cn/api/paas/v4` 都用过），自动补全是为了减少 `.env` 配置歧义。

### 6.1 默认花名册（cpp:18-65）—— 10 NPC × 3 供应商

```
NPC 1, 2, 5, 6   → DeepSeek（4 个，最稳）
NPC 3, 7, 9      → GLM（3 个）
NPC 4, 8, 10     → Qwen3（3 个）
```

性别 / 声线写死映射；`PersonaVersion=1`；`Status=Active`；其他 Battle.* / Identity.Nickname / Appearance 全空——T2 setup phase 由 Director 决定。

---

## 7. `ProjectEnvLoader`（h:5-9）

```cpp
namespace ProjectEnvLoader {
    AILIVEPROJECT_API FString Get(const FString& Key);
    AILIVEPROJECT_API void Reload();
}
```

读 `<ProjectDir>/.env`（key=value 格式）。**只在测试用**——CLAUDE.md 项目级守则明确："仅测试用；生产应走 `UDeveloperSettings` 或 secret store"。

`Reload()` 是给 PIE 改完 `.env` 不重启编辑器的逃生口。

---

## 8. 自检题答案

### 8.1 自检题 #1

> Reasoner 和 Parser 是不是同一个 HTTP 客户端？为什么 Parser 要再调一次 LLM？

**同一个 HTTP 客户端**（`OpenAIChat::RequestBlocking`），但**两次调用**：

- **Reasoner**：每个 NPC 用自己 Roster 配置的 provider（DeepSeek/GLM/Qwen3）。允许 tagged 半结构化输出。
- **Parser**：永远走 DeepSeek（`kParserProvider` 写死），强 JSON。

**为什么不 regex？** 因为 INTENDED/BID body 内的 JSON 可能含 `</tag>` 字面量、嵌套 `}`、转义引号——Parser LLM 在 `temperature=0` 下做"格式翻译"是机器可证的：raw_text 里是什么 → JSON 里逐字搬过去，不发明、不修补。`PrevalidateRawTaggedSections` + `ValidateParserOutputJson` 双层兜底确保 Parser LLM 不能瞎搞。

### 8.2 自检题 #2

> RequestBlocking 失败后会不会自动重试？

**只在一种情况下递归重试**：`HTTP 400 + bResponseFormatJson=true`。重试时关掉 json_object，标记 `bRetriedWithoutResponseFormat=true`。

其他失败（HTTP 其他码 / 网络错 / 解析错 / response 空）都**不在** `RequestBlocking` 内重试——重试责任在调用层（`RunAgentTickInWorker` 自己 3 次 retry loop）。

### 8.3 自检题 #3

> PromptAssembler 7 段哪些 must-keep？降级时怎么砍？

**Must-keep 3 段**：OwnHistory / PendingIntended / NearWindow。
**可降级 4 段**（按砍的顺序）：PrivateChats → Notes → Commitments → ChallengePrefetch（条件输出）。

降级逻辑**硬编码在 cpp:319-332** 三个 if 串行检查；**Sections 数组里的 `bMustKeep` 字段当前不参与 if 判断**——它只是文档级标记。如果以后改降级策略（比如新增段也想可降级），需要同时改 if 里的硬编码。

### 8.4 自检题 #4

> Parser 用 DeepSeek 还是其他？为什么和 Reasoner 不一定一致？

Parser 永远走 **DeepSeek**（`constexpr ELLMProvider kParserProvider = ELLMProvider::DeepSeek`，cpp:25）。

Reasoner 由 NPC 的 Roster 配置决定，每个 NPC 不同。

**为什么不一致？** Reasoner 要"角色扮演的多样性"——10 个 NPC 用 3 家不同模型让博弈结果不被单一模型偏好支配。Parser 是单一格式翻译任务，**用最稳的供应商即可**——T7 烟测发现 Qwen3 在 10 NPC 并发时限流（70%+ 失败率），切 DeepSeek 后稳定。

---

## 9. 关键不变量 / 设计约束速查表

| # | 不变量 / 约束 | 在哪强制 |
|---|---|---|
| 1 | RequestBlocking 必须跑在 worker 线程（`ProcessRequestUntilComplete` 阻塞） | RunAgentTickInWorker（阶段 5 看） |
| 2 | temperature 用 `FJsonValueNumberString` 字符串包装，不能 `SetNumberField(double)` | OpenAIChatClient.cpp:23-28 |
| 3 | body 用 UTF-8 显式编码，不用 `SetContentAsString` | OpenAIChatClient.cpp:147-150 |
| 4 | HTTP 头必须显式设 User-Agent / Accept / Accept-Encoding | OpenAIChatClient.cpp:154-156 |
| 5 | Parser 必须强 JSON；endpoint 拒绝 = Parser 失败 | AILiveParserClient.cpp:395-406 |
| 6 | Reasoner 自动 400 + json_object 降级；Parser 不降级 | OpenAIChatClient.cpp:191 + Parser 检查 bRetriedWithoutResponseFormat |
| 7 | PromptAssembler 必须 must-keep OwnHistory / PendingIntended / NearWindow | PromptAssembler.cpp:319-332 +  ensureAlwaysMsgf |
| 8 | Parser provider 写死 DeepSeek；Reasoner 由 Roster 决 | AILiveParserClient.cpp:25 |
| 9 | v1.txt 缺失 → Parser 永久 fail（不重试） | AILiveParserClient.cpp:46-73 |
| 10 | "第 N 轮" challenge prefetch 必须走 `QuoteRecentRounds(round, 1)` 不能 `QuoteByRound(round, "")` | PromptAssembler.cpp:206-208 |

---

## 10. 扩展任务推演

| 改动 | 至少要动 |
|---|---|
| 让 Reasoner 也用 4 通道认知输出（替换 ACT02 legacy schema） | (1) `AssembleSystemPrompt` cpp:273 改模板成 SCRATCHPAD/INTENDED/BID/NOTE 4 个 tag；(2) Director 的 `BuildReasonerSystemPrompt`（阶段 5 看）也要同步——**两处别忘**；(3) `parser_version` 不变（输入侧改而不是输出侧改），但 schema_meta 的 reasoner_prompt_path 要 bump |
| 加一种 Parser provider 备份（主 DeepSeek，限流时切 GLM） | 把 `kParserProvider` 从 `constexpr` 改成函数 `ResolveParserProvider()`；记一个进程级"上次失败时间"——失败 3 次后 60s 内切备份；写 `system.parse_failed` 时把当时用的 provider 写进 payload |
| 让 PromptAssembler 支持英文 challenge prefetch | `ExtractRoundRefs` cpp:50-66 加英文 pattern `(?i)round\s+(\d+)`；保留中文模式互不干扰；测试用 `AILive.Test.AssembleChallenge NPC01 5` 时把 ChallengeText 改英文确认 |
| 给 OpenAIChat 加请求级日志（不是 dump，而是结构化 JSON） | (1) RequestBlocking 末尾新增 log 函数写到 `Saved/Logs/OpenAIChat/timeline.jsonl`；(2) 用 `wall_clock_ms / model / latency_ms / status / tokens` 字段；(3) **不要**写 RawContent——可能含玩家 NPC 私聊；(4) 文件用 append-only（`SaveStringToFile` + `EFileWrite::FILEWRITE_Append`） |

---

## 11. 阅读追溯：从 PromptAssembler 反向看 EventStore 读 API

读完 PromptAssembler.cpp 应能回答：

1. `ListMyStatements(Viewer)` 返回的事件类型是什么？是按 seq 升序还是降序？  
   → 回到 EventStore.h 看签名

2. `QuoteRecentRounds(round, K, Viewer)` 的 K 是"近 K 拍"还是"近 K 个事件"？  
   → 阶段 4a 已答：是"K 个 round"，covering index `idx_events_game_round`

3. `ListMyPendingIntended(Viewer, K)` 的 SQL 怎么知道某个 intended 没对应 public？  
   → 提示：JOIN events 表自身（actor=Viewer + EventType=SpeechIntended）LEFT JOIN（actor=Viewer + EventType=SpeechPublic + 同 round 后续）where 右侧 NULL

如果你打开 EventStore.cpp 看了答案不一致，回来告诉我哪里偏了——可能是注释过时。

---

## 下一步

进 **阶段 4c — 投影 / 登记 / Resume / Delete**：4 个 reducer + AgentRegistry + ListenerFilter 的可见性 SQL + TriggerDeleteExecuted 三步链路。告诉我可以开始，我把它写到 `Learn/04c-projection-resume-delete.md`。
