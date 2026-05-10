# T6+T7 · UE 协议基础设施 + Dispatcher 主体 + 测试键退役

> 主仓库：UE C++（`Source/AILiveProject/`）+ UE 蓝图（`Content/`）+ UE 仓 `Docs/`
> 前置卡：T1 ✅（协议骨架 + 12 schema + 20 examples，`protocol_version = 0.1.1`）、T2 ✅（数据层 + EventStore + 投影 + 召回 + `_meta.db`）、T03-5 ✅（协议层主链 + LLM Provider + HTTP Server，可启 `python -m brain.server`）
> 下游消费者：T9（02 病毒游戏 brain 机制 + zone 字典）、T10（UE touch 检测 + 一局完整 PIE 联调）

---

## 1. 目标

把 BrainService 0.1.1 协议在 UE 端落地为**薄 glue 层**：HTTP polling 客户端 + USTRUCT 镜像 + RosterSubsystem + WorldStateCollector + ActionDispatcher + ActionCompletionWrapper + ActionResultReporter + SpeechResultReporter + IngressValidator + SpeakDispatcher + 退役 4 个测试按键（M/K/N/L），不重写既有 GASP / Mover / 感知 / TTS / SmartObject 链路。

**本卡定位**：阶段 3 全部硬验收 = brain↔UE 真实闭环跑通。T10 仅做 LLM 真实多 agent 联调，不再补做基础链路验证。

---

## 2. 必读上下文锚点

按顺序读完再动手：

1. **`Docs/Roadmap/00_total_plan.md`** —— 总路线图（重点：「边界」「阶段 3」「关键依赖图」「需要保留 / 不动的 UE 资产清单」「可退役 / 待评估的 UE 资产」「风险与开放决策」）。
2. **`Docs/Roadmap/handbook.md`** §0 工作流总览 + §4 已收敛项目级决策。
3. **`Docs/memory_principles.md`** 关键章节：
   - §〇 硬约束 1–6（特别是硬约束 5 viewer 封闭命名空间、硬约束 6 视角隔离行粒度）
   - §一 两层架构（数据层 vs 协议层职责）
   - §4.1.2 事件类型枚举全表（含 T1 新增的 `system.ingress_rejected` / `speech.playback_resolved`，T2 新增的 `world.*` 拆条命名）
   - §4.1.5 默认事件可见性模板
   - §5.4.2 动作生命周期（`action.cancelled` 与 `action.resolved` 互斥终态；动作通道独立于发言权）
4. **`Docs/Roadmap/T01_protocol_skeleton.md`** §3.2 / §3.3 / §3.4 —— endpoint 表（10 个）+ 12 份 schema 名册 + 20 个 example fixture + 命名空间约束。
5. **`Docs/Roadmap/T02_brain_data_layer.md`** §3.2 / §3.4 —— `world.perception.sight` / `world.perception.hearing` / `world.actor_state` / `world.client_sample` 拆条命名 + 落库语义。
6. **`Docs/Roadmap/T03-5_brain_protocol_layer.md`** §3.4 / §3.5 —— orchestrator 派发的 `action.intent` / `speech.public` 实际形态 + brain HTTP server 当前路由行为；本卡 dispatcher 的对接方。
7. **BrainService 真相源**（已生成）：
   - `BrainService/protocol/protocol.md`（人读契约全文）
   - `BrainService/protocol/schemas/*.schema.json`（全部 12 份）
   - `BrainService/protocol/examples/*.json`（全部 20 份）
   - `BrainService/brain/server/routes/{actions,speech,roster,world_state,ingress_reject,events,health,sessions}.py` —— 实际 server 接收 / 派发行为
8. **UE 既有 C++ glue（保护红线，签名锁定）**：
   - `Source/AILiveProject/Public/MinimaxACELibrary.h`（TTS+A2F 入口表 + `GetMinimaxApiKeyFromProjectEnv` env fallback）
   - `Source/AILiveProject/Public/AILiveProjectPerceptionLogger.h`（`FPerceivedAgentInfo` / `FHeardSoundInfo` 字段表 + `GatherSightPerception` / `GatherHearingPerception` / `ClaimFirstSlotInActor` / `GetChildActorOf` 签名）
   - `Source/AILiveProject/Public/SightMemoryComponent.h`（`OnSightEnter`/`OnSightExit` delegate + `GetLastSeenLocation` / `IsCurrentlyVisible`）
   - `Source/AILiveProject/Public/AILiveAgent.h`（marker interface `IAILiveAgent`）
   - `Source/AILiveProject/Public/AILiveProjectScatterMover.h`（`ScatterNPCsAroundTarget` —— 散点；MVP 不进 ontology v1，留位）
   - `Source/AILiveProject/AILiveProject.Build.cs`（HTTP / Json / JsonUtilities / ACERuntime 已就位）
   - `Source/AILiveProject.Target.cs` / `Source/AILiveProjectEditor.Target.cs`（`DefaultBuildSettings = V6`，**不得动**）
9. **DevLog 关键踩坑**：
   - `2026-04-29_npc_smartobject_sit.md`（`UAITask_UseGameplayInteraction` BindEventToOnSucceeded / OnFailed 模式；LatentTask 在 C++ 不能直接走，必须 BP wrapper）
   - 与 `visual_override_system` / `a2f_metahuman_contract` skill 涉及的 BP 现状（A2F 链路保持不动）
10. **`Docs/PRD.md`**：「项目核心」「AI 心智决策系统」「让博弈对 AI 自己而言"重要"」三节（理解 LLM 主导决策、UE 内置 AI 处理执行的设计意图；本卡严格遵守"UE 仅身体侧"边界）。
11. **`CLAUDE.md`**（项目根） + 用户级 `~/.claude/CLAUDE.md` —— 关键规则（红线 / 红线 / DefaultBuildSettings = V6 / bTickPhysicsAsync 不准翻 True）+ Monolith MCP 操作守则。
12. **auto memory 关键 feedback**（按 `MEMORY.md` 索引）：
    - `feedback_unreal_tooling.md`（Unreal 资产改动优先 Monolith MCP）
    - `feedback_precheck_before_BP_changes.md`（改 BP 前先用 MCP 查现状）
    - `feedback_monolith_patterns.md`（MCP 改 BP 的参数名 / 函数名 / 批量套路）
    - `feedback_mcp_limitations.md`（继承组件 / class pin / LatentTask 走 BP wrapper）
    - `feedback_chinese_comments_msvc.md`（中文注释 .h/.cpp/.cs **必须** UTF-8 BOM）

---

## 3. 范围内（DO）

### 3.0 前置动作：修 BrainService `/health` bug + 同步 protocol pointer

本卡执行的**第一步**，先把以下三件事一并提交（属本卡范围内动作；不算 protocol 升版，仅是 server 实现 bugfix + UE 仓追赶 BrainService HEAD）：

1. **修 BrainService `/health` 实现 bug**：
   - `BrainService/brain/server/routes/health.py:14` 当前返回 `{"ok": True, "protocol_version": ..., "server_time": ...}`，与 `BrainService/protocol/schemas/health.schema.json` 要求的 `{"status": "ok", "protocol_version": ..., "server_time": ...}` + `additionalProperties: false` 不一致。
   - 改成：`return {"status": "ok", "protocol_version": PROTOCOL_VERSION, "server_time": ...}`
   - 在 `BrainService/tests/test_server_health.py` 加断言验证 `response.json()["status"] == "ok"` 与 `"ok" not in response.json()`，防止再漂回去。
   - **不升 protocol_version**（仍 `0.1.1`）—— 这是 server 实现追赶自家 schema 的 bugfix，不动协议契约。
   - 在 BrainService 仓提一个独立 commit（建议 message：`fix(server): /health response key 'ok' → 'status' to match schema`）。

2. **vendored 同步**：
   - 把 `BrainService/protocol/examples/*.json` 全部 20 个文件复制到 `Source/AILiveProject/Tests/Fixtures/protocol_examples/`。
   - 用 PowerShell 校验 sha256 一致：

     ```powershell
     Get-ChildItem D:\Project\Unreal\AILiveProject\BrainService\protocol\examples\*.json | ForEach-Object {
         $src = $_.FullName
         $dst = "D:\Project\Unreal\AILiveProject\Source\AILiveProject\Tests\Fixtures\protocol_examples\$($_.Name)"
         $h1 = (Get-FileHash $src -Algorithm SHA256).Hash
         $h2 = (Get-FileHash $dst -Algorithm SHA256).Hash
         if ($h1 -ne $h2) { Write-Error "Mismatch: $($_.Name)" }
     }
     ```

3. **更新 `Docs/protocol_pointer.md`**：
   - 把 `commit hash` 字段从 `c271383c8c3c2864a3988ba31e270e39be4552db` 改为本卡 §3.0 步骤 1 提交的新 commit hash（即 `/health` bugfix 后的 BrainService HEAD）。
   - `protocol_version` 字段保持 `0.1.1`（不升版本）。
   - 把 §"UE C++ round-trip 测试位置（T6 实现后填写）" 占位填上 `Source/AILiveProject/Tests/AILiveProtocolRoundTripTest.cpp` + 运行命令。

**为什么本卡管这件事**：T1 完成时 BrainService HEAD 是 `c271383`；T2 / T03-5 推进过程里 BrainService HEAD 又前移了若干 commit（实测 HEAD = `c604af0`），但 UE 仓 `Docs/protocol_pointer.md` 没跟进。本卡是 UE 端首次落地协议 client，必须在动手前先把 pointer 与 BrainService HEAD 对齐——否则 USTRUCT 镜像写哪一版协议都不知道。同步动作放在 §3.0 而不是收尾环节，确保后续 §3.2–§3.17 全部基于正确的 schema 状态推进。

### 3.1 Build.cs 增量与构建节奏

`Source/AILiveProject/AILiveProject.Build.cs` 加 `DeveloperSettings` 到 `PublicDependencyModuleNames`：

```csharp
PublicDependencyModuleNames.AddRange(new string[]
{
    // ... 既有 14 个依赖保持原顺序
    "DeveloperSettings",  // T6 新增：UAILiveProjectSettings : UDeveloperSettings 需要
});
```

**构建节奏（CLAUDE.md 关键规则）**：
- 改 `.Build.cs` 后**必须** UBT 全量重建一次：

  ```powershell
  & "D:/Software/UE_5.7/Engine/Build/BatchFiles/Build.bat" `
    AILiveProjectEditor Win64 Development `
    -Project="D:/Project/Unreal/AILiveProject/AILiveProject.uproject"
  ```
- 之后 `.cpp` / `.h` 改动走 Live Coding；扩 `MinimaxACELibrary` 加新 delegate 时**第一次**也走 UBT 全量（防 Live Coding 反射 bug）。
- `Source/*.Target.cs` 的 `DefaultBuildSettings = V6` **不动**（CLAUDE.md 红线）。

### 3.2 USTRUCT 协议镜像（`AILiveProtocolTypes.h/cpp`）

单文件镜像 12 份 schema，全部使用 `FAIL_*` 前缀（避免与 UE 内置类型 / 既有 `FPerceivedAgentInfo` 等冲突）。

| Schema 文件 | USTRUCT 名称 | 关键字段 |
| --- | --- | --- |
| `common.event_meta` | `FAIL_EventMeta` | seq / game_id / round_no / phase / actor / event_type / visibility (TArray<FString>) / addressed_to (TArray<FString>) / payload (FString JSON pass-through，因 schema 描述说 "structure varies by event_type"，无固定形态) / **bHasParentEventId + parent_event_id (int64) ← 不用 TOptional，UE 反射不支持**；prev_hash 同模式（**bHasPrevHash + prev_hash**） / event_hash / wall_clock_ts |
| `error_response` | `FAIL_ErrorResponse` + `FAIL_ErrorDetails` | error_code / message / retryable / details (optional) |
| `health` | `FAIL_HealthResponse` | protocol_version (FString，反序列化后断言 == "0.1.1") / status / server_time |
| `session_create.request` | `FAIL_SessionCreateRequest` | client_label (optional) |
| `session_create.response` | `FAIL_SessionCreateResponse` | game_id / protocol_version / server_time |
| `roster_register.request` | `FAIL_RosterRegisterRequest` + `FAIL_NPCEntry` + `FAIL_Position3D` | roster: TArray<FAIL_NPCEntry>；NPCEntry: actor_id / display_name / initial_position (optional FAIL_Position3D) |
| `roster_register.response` | `FAIL_RosterRegisterResponse` | game_id / accepted_count |
| `world_state_push.request` | `FAIL_WorldStatePushRequest` + `FAIL_NpcObservation` + `FAIL_SightEntry` + `FAIL_HeardSound` + `FAIL_Position3D` | client_sample_id / sample_wall_clock_ts / observations: TArray<FAIL_NpcObservation> |
| `action_pull.response` | `FAIL_ActionPullResponse` + `FAIL_ActionIntentEvent` + `FAIL_OntologyV1Intent` + `FAIL_MoveToParams` + `FAIL_SitParams` + `FAIL_WaitParams` + `FAIL_Position3D` | events / next_cursor；OntologyV1Intent 用 discriminant `name` ∈ {move_to / sit / wait}（详见 §3.2.1） |
| `action_result.request` | `FAIL_ActionResultRequest` | actor_id / outcome (E_AIL_ActionOutcome) / duration_ms / final_position / intent_seq / error_reason (optional) |
| `speech_pull.response` | `FAIL_SpeechPullResponse` + `FAIL_SpeechPublicEvent` | events / next_cursor；SpeechPublicEvent: actor_id / addressed_to / seq / text |
| `speech_result.request` | `FAIL_SpeechResultRequest` | actor_id / status (E_AIL_SpeechStatus) / duration_ms / speech_seq / error_reason (optional) |
| `ingress_reject.request` | `FAIL_IngressRejectRequest` | actor_id / original_seq / raw_message_snippet / reject_reason (E_AIL_RejectReason) |
| `event_query.response` | `FAIL_EventQueryResponse` | events: **TArray<FAIL_EventMeta>**（schema `event_query.events.items` 是 `$ref common.event_meta` 对象数组，不是字符串）/ next_cursor |

#### 3.2.1 oneOf discriminant：`FAIL_OntologyV1Intent`

`action_pull.schema.json` 的 `ontology_v1_intent` 是 oneOf（move_to / sit / wait 三选一），UE 端用扁平结构 + discriminant：

```cpp
USTRUCT(BlueprintType)
struct FAIL_OntologyV1Intent
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly) FString ActionOntologyVersion;  // const "1"
    UPROPERTY(BlueprintReadOnly) FString Name;                   // "move_to" / "sit" / "wait"

    // 三个 params 子结构：仅 Name 命中的那个有效，另外两个为默认值
    UPROPERTY(BlueprintReadOnly) FAIL_MoveToParams MoveToParams;
    UPROPERTY(BlueprintReadOnly) FAIL_SitParams SitParams;
    UPROPERTY(BlueprintReadOnly) FAIL_WaitParams WaitParams;
};
```

序列化 / 反序列化走 §3.3 的 helper（手工 switch on `Name`），不依赖 `FJsonObjectConverter` 的默认 oneOf 处理（它不支持）。

#### 3.2.2 enum 字段映射

| schema enum | UENUM | 序列化 |
| --- | --- | --- |
| `current_action ∈ {idle, moving, sitting, speaking}` | `E_AIL_NpcAction : uint8 { Idle, Moving, Sitting, Speaking }` | 自定义 ToString/FromString |
| `outcome ∈ {succeeded, failed}` | `E_AIL_ActionOutcome : uint8 { Succeeded, Failed }` | 同上 |
| `status ∈ {succeeded, failed}` | `E_AIL_SpeechStatus : uint8 { Succeeded, Failed }` | 同上 |
| `reject_reason ∈ {ACTOR_NOT_IN_ROSTER, AUDIENCE_OUTSIDE_VISIBILITY, INTENT_NOT_IN_ONTOLOGY, SENTENCE_TOO_LONG}` | `E_AIL_RejectReason : uint8 { ActorNotInRoster, AudienceOutsideVisibility, IntentNotInOntology, SentenceTooLong }` | 同上 |

序列化原则：JSON wire 永远用 schema 定义的小写 / SCREAMING_SNAKE 字符串；UE 端 enum 名只是内部表达。

#### 3.2.3 actor_id / viewer 不用 enum

UE 端 actor_id 用 `FString`（值约束在 `NPC01..NPC10`，由 IngressValidator 与 RosterSubsystem 在边界处校验）；不用 UENUM——便于 T9 后续扩展（lifecycle 复用规则等）。

#### 3.2.4 nullable 字段处理（避开 TOptional UPROPERTY 限制）

UE USTRUCT 的 UPROPERTY 反射**不支持** `TOptional<T>`。所有 schema 中的 nullable / optional 字段统一走 "bool flag + value" 模式：

```cpp
USTRUCT(BlueprintType)
struct FAIL_EventMeta
{
    GENERATED_BODY()

    UPROPERTY() FString GameId;
    UPROPERTY() int64 Seq = 0;
    // ... 其余必备字段

    /** 当 prev_hash 在 wire JSON 为 null 时（仅首事件 seq=0），bHasPrevHash=false。 */
    UPROPERTY() bool bHasPrevHash = false;
    UPROPERTY() FString PrevHash;

    /** 当 parent_event_id 在 wire JSON 为 null 时，bHasParentEventId=false。 */
    UPROPERTY() bool bHasParentEventId = false;
    UPROPERTY() int64 ParentEventId = 0;
};
```

JSON 序列化助手（§3.3）在 ToJsonString 时根据 `bHasXxx` 决定写 null 还是值；FromJsonString 时检测 wire 是否为 null 并设置 flag。BlueprintReadOnly 字段保持平铺，便于 BP 调试 view 直接看。

同样模式适用：`FAIL_RosterRegisterRequest.NPCEntry.bHasInitialPosition` / `FAIL_OntologyV1Intent.MoveToParams.bHasCoords` / `FAIL_ActionResultRequest.bHasErrorReason` / `FAIL_SpeechResultRequest.bHasErrorReason` / `FAIL_ErrorResponse.bHasDetails` 等所有 schema 中标 optional / nullable 的字段。

### 3.3 JSON 序列化辅助（`AILiveProtocolJson.h/cpp`）

集中提供：

```cpp
namespace AILiveProtocol
{
    // 主路径：USTRUCT ↔ JSON 字符串
    template <typename T>
    bool ToJsonString(const T& InStruct, FString& OutJson);

    template <typename T>
    bool FromJsonString(const FString& InJson, T& OutStruct);

    // 显式偏特化（oneOf / enum 走自定义路径）
    template <> bool ToJsonString<FAIL_ActionPullResponse>(const FAIL_ActionPullResponse&, FString&);
    template <> bool FromJsonString<FAIL_ActionPullResponse>(const FString&, FAIL_ActionPullResponse&);
    // ... 其余 oneOf / enum 含字段同样偏特化

    // helpers
    FString NewIdempotencyKey();           // FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens).ToLower()
    FString IsoUtcNow();                    // FDateTime::UtcNow().ToIso8601() + "Z"
    bool IsValidGameIdUuidV4(const FString& In);   // 正则校验
}
```

- 大多数 USTRUCT 走 `FJsonObjectConverter::UStructToJsonObjectString` / `JsonObjectStringToUStruct` 默认路径。
- `FAIL_OntologyV1Intent` / 含 enum 字段的结构走偏特化：手工调 `FJsonObjectConverter` 拆字段 + switch on discriminant。
- canonical JSON：UE 端 wire **不**强制按 ASCII 升序（brain 端持久层强制；UE 仅作为 client 提交），保留 `FJsonObjectConverter` 默认输出。
- `Idempotency-Key` 生成：每次 POST call 一个新 UUID v4；同 call 重试用同一 key（HTTP 客户端层负责）。

### 3.4 round-trip 测试（`Source/AILiveProject/Tests/AILiveProtocolRoundTripTest.cpp`）

- vendored copy `BrainService/protocol/examples/*.json`（全部 20 个）到 `Source/AILiveProject/Tests/Fixtures/protocol_examples/`（不走 git submodule，而是按 `Docs/protocol_pointer.md` commit hash 同步时手工 / 脚本 vendored）。
- vendored 时机：本卡执行的最后一步前确认与 BrainService HEAD 同步；后续协议升版时由更新 `Docs/protocol_pointer.md` 的同一 commit 一并 vendored。
- 测试形态：

  ```cpp
  IMPLEMENT_SIMPLE_AUTOMATION_TEST(
      FAILiveProtocolRoundTripTest_ActionPullSuccess,
      "AILive.Protocol.RoundTrip.ActionPullSuccess",
      EAutomationTestFlags::ApplicationContextMask | EAutomationTestFlags::EngineFilter)

  bool FAILiveProtocolRoundTripTest_ActionPullSuccess::RunTest(const FString& Parameters)
  {
      FString JsonIn = LoadFixture("action_pull.success.json");
      FAIL_ActionPullResponse Parsed;
      TestTrue("FromJsonString", AILiveProtocol::FromJsonString(JsonIn, Parsed));
      TestEqual("events.Num", Parsed.Events.Num(), 2);
      TestEqual("events[0].actor_id", Parsed.Events[0].ActorId, TEXT("NPC03"));
      // ...
      FString JsonOut;
      TestTrue("ToJsonString", AILiveProtocol::ToJsonString(Parsed, JsonOut));
      // 字段集 + 值语义等价（不强制字符级一致，因为字段顺序不强制）
      TestTrue("semantic equal", AILiveProtocol::JsonSemanticEqual(JsonIn, JsonOut));
      return true;
  }
  ```

- 覆盖范围：全部 20 个 example fixture（10 happy + 10 failure）；失败样例验证 `FAIL_ErrorResponse` 路径。
- helper `AILiveProtocol::JsonSemanticEqual(FStringView A, FStringView B)`：递归比较 dict（key set 相等 + 每个 key 值相等）/ array（顺序敏感）/ 标量。

### 3.5 配置 `UAILiveProjectSettings`（`AILiveProjectSettings.h/cpp`）

```cpp
UCLASS(Config=Game, DefaultConfig, meta=(DisplayName="AI Live Project"))
class AILIVEPROJECT_API UAILiveProjectSettings : public UDeveloperSettings
{
    GENERATED_BODY()

public:
    /** Brain HTTP server 基础 URL（含 port），不带尾部 slash。 */
    UPROPERTY(Config, EditAnywhere, Category="Brain Service")
    FString BrainBaseUrl = TEXT("http://127.0.0.1:8000");

    /** Brain Authorization Bearer token；与 brain 端 BRAIN_API_TOKEN env 一致。 */
    UPROPERTY(Config, EditAnywhere, Category="Brain Service", meta=(PasswordField=true))
    FString BrainApiToken = TEXT("dev_token");

    UPROPERTY(Config, EditAnywhere, Category="Brain Service|Polling", meta=(ClampMin=50))
    int32 PollingIntervalActionMs = 200;

    UPROPERTY(Config, EditAnywhere, Category="Brain Service|Polling", meta=(ClampMin=50))
    int32 PollingIntervalSpeechMs = 200;

    UPROPERTY(Config, EditAnywhere, Category="Brain Service|Polling", meta=(ClampMin=100))
    int32 PollingIntervalWorldStateMs = 500;

    UPROPERTY(Config, EditAnywhere, Category="Brain Service|HTTP", meta=(ClampMin=500))
    int32 HttpTimeoutMs = 5000;

    UPROPERTY(Config, EditAnywhere, Category="Brain Service|HTTP", meta=(ClampMin=0))
    int32 MaxRetries = 5;

    UPROPERTY(Config, EditAnywhere, Category="Brain Service|HTTP", meta=(ClampMin=0.0))
    float RetryBackoffBaseSeconds = 1.0f;

    /**
     * Minimax TTS API key. Leave empty to fallback to UMinimaxACELibrary::GetMinimaxApiKeyFromProjectEnv()
     * (reads <ProjectDir>/.env line `minimax=...`). Production should set this here.
     */
    UPROPERTY(Config, EditAnywhere, Category="Brain Service|TTS", meta=(PasswordField=true))
    FString MinimaxApiKey;

    // 注意：game_id 故意不在此处。硬约束（k）：MVP 不支持 game session 跨 PIE 进程恢复；
    // game_id 唯一入口是 POST /v1/games，由 GameInstanceSubsystem 在内存中持有。
};
```

写入 `Config/DefaultGame.ini` 新增段：

```ini
[/Script/AILiveProject.AILiveProjectSettings]
BrainBaseUrl=http://127.0.0.1:8000
BrainApiToken=dev_token
PollingIntervalActionMs=200
PollingIntervalSpeechMs=200
PollingIntervalWorldStateMs=500
HttpTimeoutMs=5000
MaxRetries=5
RetryBackoffBaseSeconds=1.0
```

**绝不**写到 `DefaultEngine.ini`——避免与既有 `+GameModeMapPrefixes` 等键冲突。

### 3.6 HTTP 客户端 `FAILiveProjectBrainHttpClient`（`AILiveProjectBrainHttpClient.h/cpp`）

普通 C++ 类（非 UObject），由 `UAILiveProjectBrainSessionSubsystem`（§3.8）以 `TPimplPtr` 持有。每个 endpoint 一个 async API：

```cpp
class FAILiveProjectBrainHttpClient
{
public:
    FAILiveProjectBrainHttpClient(FString BaseUrl, FString ApiToken,
                                   int32 TimeoutMs, int32 MaxRetries, float BackoffBaseSec);

    TFuture<TOptional<FAIL_HealthResponse>>          Health();
    TFuture<TOptional<FAIL_SessionCreateResponse>>   CreateSession(const FAIL_SessionCreateRequest& Req);
    TFuture<TOptional<FAIL_RosterRegisterResponse>>  RegisterRoster(const FString& GameId, const FAIL_RosterRegisterRequest& Req);
    TFuture<bool>                                     PushWorldState(const FString& GameId, const FAIL_WorldStatePushRequest& Req);
    TFuture<TOptional<FAIL_ActionPullResponse>>      PullActions(const FString& GameId, int64 SinceSeq);
    TFuture<bool>                                     PostActionResult(const FString& GameId, const FAIL_ActionResultRequest& Req);
    TFuture<TOptional<FAIL_SpeechPullResponse>>      PullSpeech(const FString& GameId, int64 SinceSeq);
    TFuture<bool>                                     PostSpeechResult(const FString& GameId, const FAIL_SpeechResultRequest& Req);
    TFuture<bool>                                     PostIngressReject(const FString& GameId, const FAIL_IngressRejectRequest& Req);
    TFuture<TOptional<FAIL_EventQueryResponse>>      QueryEvents(const FString& GameId, int64 SinceSeq);

    void CancelAllInFlight();   // PIE 关闭时调
};
```

实现要点：

- 用 `FHttpModule::Get().CreateRequest()` + `OnProcessRequestComplete().BindLambda` 实现单次 call；GameThread 回调。
- 每个 POST 自动注入：
  - `Authorization: Bearer <BrainApiToken>`（除 `Health()` 外）
  - `Content-Type: application/json`
  - `Idempotency-Key: <new uuid v4>`（除 `CreateSession` 外）—— 同一 call 重试用同一 key
- 错误重试：5xx + 网络错指数退避（`MaxRetries` 次，base = `RetryBackoffBaseSec`）；4xx 直接返失败（不重试）。
- pull endpoint URL：`<BaseUrl>/v1/games/{game_id}/actions/pull?since_seq=N`
- 失败成因（4xx body 是 `error_response`）：解析 `FAIL_ErrorResponse` 并日志 `UE_LOG(LogAILiveBrain, Warning, TEXT("Brain HTTP %d %s: %s"), Status, *ErrorCode, *Message)`。
- 单元测试 `FAILiveProjectBrainHttpClientTest.cpp`：用 `FHttpModule::Get().CreateRequest()` 在测试线程内 mock—— **但** UE 5.7 没有官方 mock 路径，MVP 阶段单元测试用一个**真实但本地的**测试 brain server（PIE 启动前可启 `python -m brain.server`）；不强求 unit test 100% 隔离。

### 3.7 RosterSubsystem `UAILiveProjectRosterSubsystem`（`AILiveProjectRosterSubsystem.h/cpp`）

```cpp
UCLASS()
class AILIVEPROJECT_API UAILiveProjectRosterSubsystem : public UGameInstanceSubsystem
{
    GENERATED_BODY()
public:
    void RegisterPawn(const FString& ActorId, APawn* Pawn);
    APawn* FindPawnByActorId(const FString& ActorId) const;
    bool TryGetActorIdForPawn(APawn* Pawn, FString& OutActorId) const;

    /**
     * 用 BP class FName 反查 actor_id。WorldStateCollector 必走这条路径——
     * UAILiveProjectPerceptionLogger::GatherSightPerception / GatherHearingPerception
     * 返回的 FPerceivedAgentInfo.Identity / FHeardSoundInfo.SourceIdentity 是
     * StripBlueprintSuffix 后的 BP class FName（如 "BP_NPC_MH_Character_3"），
     * 不是 Pawn 指针，所以 PawnToActorId 反向 map 在 collector 路径上**不**适用。
     */
    bool TryGetActorIdForBPClassFName(FName BPClassFName, FString& OutActorId) const;

    void GetAllRegistered(TArray<TPair<FString, TWeakObjectPtr<APawn>>>& Out) const;
    int32 NumRegistered() const;
    void ClearAll();   // PIE 结束时调

private:
    TMap<FString, TWeakObjectPtr<APawn>> ActorIdToPawn;
    TMap<TWeakObjectPtr<APawn>, FString> PawnToActorId;       // 反向 map（含 IngressValidator / Dispatcher 路径用）
    TMap<FName, FString> BPClassFNameToActorId;                // collector 用：BP class FName → actor_id
};
```

actor_id 推断规则（由 `BrainSession` §3.8 在 BeginPlay 时执行）：

1. `UWorld::GetWorld()` → 找所有实现 `IAILiveAgent` 的 Pawn
2. 对每个 Pawn：从 BP class name `BP_NPC_MH_Character_<N>_C` 提取 N（正则 `_(\d+)_C$` 或 `_(\d+)$`）
3. `ActorId = FString::Printf(TEXT("NPC%02d"), N)`（如 `_1_C` → `NPC01`，`_10_C` → `NPC10`）
4. 失败则 fallback：按 Pawn `GetName()` 末尾数字推断；仍失败则 `UE_LOG(Error)` 跳过该 Pawn
5. `RegisterPawn` 同时填三个 map：`ActorIdToPawn[ActorId] = Pawn` / `PawnToActorId[Pawn] = ActorId` / `BPClassFNameToActorId[StripBlueprintSuffix(Pawn->GetClass()->GetFName())] = ActorId`。`StripBlueprintSuffix` 复用 `UAILiveProjectPerceptionLogger` 内部 helper（如未导出则在 RosterSubsystem 内本地实现，规则与 PerceptionLogger 一致：`_C` 后缀剥除）。

### 3.8 BrainSession `UAILiveProjectBrainSessionSubsystem`（`AILiveProjectBrainSessionSubsystem.h/cpp`）

```cpp
UCLASS()
class AILIVEPROJECT_API UAILiveProjectBrainSessionSubsystem : public UGameInstanceSubsystem
{
    GENERATED_BODY()
public:
    virtual void Initialize(FSubsystemCollectionBase& Collection) override;
    virtual void Deinitialize() override;

    bool IsReady() const { return bReady; }
    const FString& GetGameId() const { return GameId; }
    FAILiveProjectBrainHttpClient& GetClient() { return *Client; }

    /**
     * 启动序列入口。**唯一触发点**是 GM_Sandbox BP 的 BeginPlay 节点：
     *   GetGameInstance() → GetSubsystem<UAILiveProjectBrainSessionSubsystem>() → StartHandshake()
     * 不要在 Initialize / PostInitialize 里自动启动——Initialize 时 Pawn 还未 spawn，
     * roster 枚举会拿到 0 个 Pawn 然后被 brain 端 minItems:1 schema 拒。
     */
    UFUNCTION(BlueprintCallable, Category = "AI Live|Brain")
    void StartHandshake();

private:
    void Phase1_HealthCheck();
    void Phase2_CreateSession();
    void Phase3_RegisterRoster();
    void Phase4_StartPolling();   // 启动 WorldStateCollector / ActionDispatcher / SpeakDispatcher 的 timer

    TPimplPtr<FAILiveProjectBrainHttpClient> Client;
    FString GameId;
    FString ProtocolVersion;
    bool bReady = false;
};
```

启动序列（每步必须等上一步成功才进下一步，全部走 GameThread 的 future continuation）：

1. **Health Check**：`Client->Health()` → 验证 response `protocol_version == "0.1.1"`，不一致 → `UE_LOG(Fatal, ...)` 拒绝运行（按硬约束 j）
2. **CreateSession**：`Client->CreateSession({})` → 拿 `game_id`、存入 `GameId` 字段（运行时内存，不持久化）
3. **RegisterRoster**：等 RosterSubsystem 完成 Pawn 枚举（在 GameMode `BeginPlay` 后驱动）→ 拼 `FAIL_RosterRegisterRequest` → `Client->RegisterRoster(GameId, Req)` → 200 OK 后置 `bReady = true`
4. **Start Polling**：启 3 个独立 timer：
   - `PollingIntervalWorldStateMs` → WorldStateCollector::TickPush
   - `PollingIntervalActionMs` → ActionDispatcher::TickPull
   - `PollingIntervalSpeechMs` → SpeakDispatcher::TickPull

`Deinitialize`：取消所有 in-flight HTTP（`Client->CancelAllInFlight()`）+ unregister 所有 timer + 清 `GameId` / `bReady`。

### 3.9 WorldStateCollector `UAILiveProjectWorldStateCollector`（`AILiveProjectWorldStateCollector.h/cpp`）

```cpp
UCLASS()
class AILIVEPROJECT_API UAILiveProjectWorldStateCollector : public UWorldSubsystem
{
    GENERATED_BODY()
public:
    void StartPolling(int32 IntervalMs);
    void StopPolling();

private:
    void TickPush();   // by FTimerManager

    int32 NextClientSampleId = 0;
    FTimerHandle TimerHandle;
};
```

单次 `TickPush`：

1. 取 `UAILiveProjectBrainSessionSubsystem`；若 `!IsReady()` → return
2. 取 `UAILiveProjectRosterSubsystem` → `GetAllRegistered()` 拿 ≤10 个 Pawn
3. 对每个 Pawn：
   - 拿 `AAIController* AIC = Pawn->GetController<AAIController>()`；nullptr → 跳过
   - 调 `UAILiveProjectPerceptionLogger::GatherSightPerception(AIC, this)` → `TArray<FPerceivedAgentInfo>`
     - 对每条 `Info`：用 `Roster->TryGetActorIdForBPClassFName(Info.Identity, OutActorId)` 反查 actor_id（Identity 是 BP class FName，**不是** Pawn 指针——见 §3.7）；过滤 `bCurrentlySensed == true` 且 `OutActorId` 命中 → 转 `FAIL_SightEntry { distance_cm = info.DistanceCm, rel_yaw_degrees = info.RelativeYawDeg, target_actor_id = OutActorId }`
   - 调 `GatherHearingPerception(AIC, this)` → `TArray<FHeardSoundInfo>`
     - 同样用 `TryGetActorIdForBPClassFName(Info.SourceIdentity, OutActorId)` 反查 → 命中则转 `FAIL_HeardSound`（age_seconds / loudness / rel_yaw_degrees / source_actor_id = OutActorId）
   - 取 `Pawn->GetActorLocation()` → `FAIL_Position3D` (UE units cm)
   - 取 `Pawn->GetActorRotation().Yaw` → `facing_degrees`
   - 推断 `current_action`（§3.9.1）
4. 拼 `FAIL_WorldStatePushRequest`：
   - `client_sample_id = NextClientSampleId++`
   - `sample_wall_clock_ts = AILiveProtocol::IsoUtcNow()`
   - `observations = TArray<FAIL_NpcObservation>`
5. `Client->PushWorldState(GameId, Req)` → 失败仅 `UE_LOG(Warning)`，不阻塞下次

#### 3.9.1 `current_action` 推断（MVP 启发式）

无 GAS 状态机，按以下信号合成：

| 信号 | current_action |
| --- | --- |
| ActionDispatcher 当前为该 Pawn 持有 `move_to` in-progress | `moving` |
| ActionDispatcher 当前为该 Pawn 持有 `sit` in-progress | `sitting` |
| SpeakDispatcher 当前为该 Pawn 持有未完成 TTS | `speaking` |
| 全无 | `idle` |

ActionDispatcher 与 SpeakDispatcher 都暴露 `GetCurrentActionForPawn(APawn*) const` 查询接口供 collector 调。

#### 3.9.2 拆条由 brain 完成

UE 端**只发 transport**（一次 push 含 N 个 observation）；brain 端按 T2 §3.4 拆成 `world.perception.sight` / `world.perception.hearing` / `world.actor_state` / `world.client_sample` 多条事件行。

#### 3.9.3 ENTER/EXIT 字段不在本卡范围

`00_total_plan.md:147` 列了"最近一次 ENTER/EXIT 事件（来自 USightMemoryComponent）"作为采集项，但 `world_state_push.schema.json` 在 protocol_version 0.1.1 中**不含**该字段。本卡不引入 ENTER/EXIT 采集——上行通道只有 sighted_actors 的"当前快照"，brain 端可按拍 diff 推断 ENTER/EXIT。

如未来 T9/T10 确认需要事件级 ENTER/EXIT 语义（而非 brain diff 推断），由那时升 protocol minor 加可选字段，再回填本卡 collector 实现。本卡不预留字段、不写桩位。`SightMemoryComponent.OnSightEnter/OnSightExit` delegate 在 BP 节点链中保持挂着（既有），不动；只是 collector 不消费它们。

### 3.10 ActionDispatcher `UAILiveProjectActionDispatcher`（`AILiveProjectActionDispatcher.h/cpp`）

```cpp
USTRUCT()
struct FAIL_ActiveAction
{
    GENERATED_BODY()
    int64 IntentSeq = 0;
    FString Name;             // "move_to" / "sit" / "wait"
    double StartedAtSeconds = 0.0;
    TWeakObjectPtr<UObject> Watcher;   // MoveWatcher / SitWatcher / WaitWatcher
};

UCLASS()
class AILIVEPROJECT_API UAILiveProjectActionDispatcher : public UWorldSubsystem
{
    GENERATED_BODY()
public:
    void StartPolling(int32 IntervalMs);
    void StopPolling();

    // 供 WorldStateCollector 查 current_action
    bool HasActionForPawn(APawn* Pawn) const;
    FName GetActionNameForPawn(APawn* Pawn) const;   // "move_to" / "sit" / "wait" / NAME_None

    // 由 ActionCompletionWrapper 在 watcher 完成时回调
    // **必须**在入口比对 IntentSeq 与 ActiveByActorId[ActorId].IntentSeq；
    // 不匹配（说明该 watcher 是被 cancel 后残留触发）→ 直接丢弃，不上报。
    void OnActionTerminated(const FString& ActorId, int64 IntentSeq,
                            E_AIL_ActionOutcome Outcome, int32 DurationMs,
                            const FVector& FinalPos, const FString& ErrorReason);

private:
    void TickPull();   // by FTimerManager
    void DispatchOne(const FAIL_ActionIntentEvent& Ev);

    /**
     * 物理停止旧 action：停 move（StopMovementImmediately）/ 释放 SmartObject claim
     * / 取消 wait timer 等；**先调 OldWatcher->CancelSilently() 把旧 watcher 设为
     * disarmed**，避免 StopMovementImmediately 触发 GetMoveStatus()==Idle 导致旧
     * watcher 立刻 fire OnActionTerminated（会与 brain 同事务派生的 action.cancelled
     * 构成双终态 bug，违反 memory_principles §5.4.2 互斥终态硬约束）。
     * 也**不**调 ActionResultReporter 上报任何 result。
     */
    void StopActiveAction(APawn* Pawn);

    int64 LastActionSeq = -1;   // -1 表示首次（与 brain server actions/pull 默认 since_seq 对齐）
    TMap<FString, FAIL_ActiveAction> ActiveByActorId;   // per actor 最多一个 in-progress action
    FTimerHandle TimerHandle;
};
```

`TickPull`：

1. `Client->PullActions(GameId, LastActionSeq)` → 收 `FAIL_ActionPullResponse`
2. 对每个 `FAIL_ActionIntentEvent`：调 `DispatchOne(Ev)`
3. `LastActionSeq = response.next_cursor`

`DispatchOne(Ev)`：

1. **IngressValidator** 校验 actor_id ∈ roster + intent ∈ ontology v1（含 target_zone 拒）→ 失败 POST `/ingress_reject`（reject_reason / raw_message_snippet 填好），不执行
2. 找 Pawn = `Roster->FindPawnByActorId(Ev.ActorId)`
3. **覆盖式 cancel**：若 `ActiveByActorId` 含该 actor_id：
   - 取出 `OldWatcher = ActiveByActorId[ActorId].Watcher`，调 `OldWatcher->CancelSilently()`（让 watcher 立即 disarm，不再 fire OnActionTerminated）
   - 调 `StopActiveAction(Pawn)` 物理停止（停 move、释放 sit claim、取消 wait timer）
   - **绝不**上报任何 result for old intent（cancelled 由 brain 同事务派生）
   - 从 `ActiveByActorId` 移除旧条目
4. 路由到原语：
   - `move_to` 含 `coords` → 调 `Pawn` 上的 `SandboxCharacter_Mover.MoveAndLookAtLocation(coords)`（通过 BP 函数节点反射调用，因为 SandboxCharacter_Mover 是 BP 父类）
   - `move_to` 含 `target_npc` → 找目标 Pawn → `MoveAndLookAt(TargetPawn)`
   - `move_to` 含 `target_zone` → IngressValidator 已拒（§3.14），到不了这一分支
   - `sit` → 先把 `target_smartobject` 解析为当前关卡里的 `SmartObjectActor`：按 `ActorNameOrLabel` / `GetName()` 精确匹配，且目标 actor 必须带 `USmartObjectComponent`；找不到时这是**执行失败**而非 ingress 拒绝，直接 `ActionResultReporter.ReportFailed(..., "target_smartobject not found")`
   - `sit` 找到 `SmartObjectActor` 后，通过 AI controller BP wrapper（详见 §3.11.2）调用 `UseSmartObjectAndNotify(SmartObjectActor, IntentSeq, NotifyTarget)` 启动 `UseSmartObjectWithGameplayInteraction` 链路
   - `wait` → no-op，标记 `ActiveByActorId[ActorId]` = `{Name: "wait", ...}`
5. 创建对应 watcher（`UAILiveActionMoveWatcher` / `UAILiveActionSitWatcher` / `UAILiveActionWaitWatcher`），存入 `ActiveByActorId[ActorId] = { IntentSeq, Name, StartedAt, Watcher }`
6. watcher 完成时触发 `OnActionTerminated(ActorId, IntentSeq, ...)`：
   - **入口先比对**：`if (ActiveByActorId.Find(ActorId)?.IntentSeq != IntentSeq) return;` —— watcher 已被 cancel 或属于过期 intent → 直接丢弃，**不**上报
   - 命中则调 `ActionResultReporter.Report*`（§3.12）→ 清 `ActiveByActorId[ActorId]`

**严格不做**：LLM 决策、目标挑选、prompt 拼装、写入事件流——只查表 + 调既有函数 + 上报。

### 3.11 ActionCompletionWrapper（`AILiveProjectActionCompletionWrapper.h/cpp`）

#### 3.11.1 Move watcher

```cpp
UCLASS()
class UAILiveActionMoveWatcher : public UObject, public FTickableGameObject
{
    GENERATED_BODY()
public:
    void Init(AAIController* InAIC, const FString& InActorId, int64 InIntentSeq, UAILiveProjectActionDispatcher* InOwner);

    /** 由 ActionDispatcher 在覆盖式 cancel 时调用。设 bArmed=false，保证 Tick / delegate 不再 fire OnActionTerminated。 */
    void CancelSilently();

    virtual void Tick(float DeltaTime) override;
    virtual TStatId GetStatId() const override;
    virtual bool IsTickable() const override { return bArmed; }

private:
    TWeakObjectPtr<AAIController> AIC;
    TWeakObjectPtr<UAILiveProjectActionDispatcher> Owner;
    FString ActorId;
    int64 IntentSeq;
    double StartedAt;
    bool bArmed = true;
};
```

Tick 每帧：`if (!bArmed) return;` 短路；否则 `AIC->GetMoveStatus()` == `EPathFollowingStatus::Idle` → 完成（成功；最终位置 = AIC->GetPawn()->GetActorLocation()，duration_ms = (now - StartedAt) * 1000）。同时 BindLambda 到 `AIC->ReceiveMoveCompleted`（如可用，UE 5.7 上 `FAIRequestID` 路径），收到 failure 信号即上报 failed。delegate lambda 内同样 `if (!bArmed) return;`。

**关键**：`CancelSilently()` 是与 ActionDispatcher 互斥终态硬约束的本地兜底——配合 §3.10 OnActionTerminated 入口的 IntentSeq 比对，构成双重保险。

#### 3.11.2 Sit watcher（C++ ↔ BP wrapper）

DevLog `2026-04-29` 已记：`UseSmartObjectWithGameplayInteraction` 是 Latent Task，C++ 侧**不能**直接调；必须 BP wrapper。

**方案**：把 BP wrapper 函数加到 `Content/Blueprints/AI/AIC_NPC_SmartObject.uasset`（所有 NPC 共用的 AI controller，已经持有 SmartObject claim 业务），命名 `UseSmartObjectAndNotify(SmartObjectActor, IntentSeq, NotifyTarget)`。

> **澄清**：实测 `Content/Blueprints/NPCs/` 仅有 `BP_NPC_MH_Character_1..10.uasset` 10 个独立 BP，**不存在共用父类** `BP_NPC_MH_Character`（10 个的父类是 `Content/Blueprints/SandboxCharacter_Mover.uasset`，CLAUDE.md 红线不能改）。AI controller 是更合理的 wrapper 持有点：sit 是 AI 行为，逻辑挂在 controller 而非 Pawn，且只需改一个资产。

BP 节点链：
1. `ClaimFirstSlotInActor(SmartObjectActor, ControlledPawn, Self)` → ClaimHandle（`UAILiveProjectPerceptionLogger::ClaimFirstSlotInActor` 既有 C++ 函数）
2. `IsValidSmartObjectClaimHandle(ClaimHandle)`；无效则调 `NotifySitFailed(IntentSeq, ReasonText)`
3. `UseSmartObjectWithGameplayInteraction(ControlledPawn, ClaimHandle)` → 返回 `UAITask_UseGameplayInteraction* Task`
4. 节点 `BindEventToOnSucceeded(Task)` → 调 NotifyTarget 上的 BlueprintCallable C++ 函数 `NotifySitSucceeded(IntentSeq)`
5. 节点 `BindEventToOnFailed(Task)` → 调 `NotifySitFailed(IntentSeq, ReasonText)`
6. `ReadyForActivation(Task)`（delegate 绑定完成后再激活）

**禁止**使用 `MoveToAndUseSmartObjectWithGameplayInteraction`：DevLog `2026-04-29_npc_smartobject_sit.md` 已确认它会先寻路到 SmartObject slot 本体位置，bench 场景会导致不可达 / 踱步；本工程正确路径是 `UseSmartObjectWithGameplayInteraction`，让内层 `ST_SmartObject_Bench` 自己找 entrance 并移动。

C++ 端 `UAILiveActionSitWatcher` 持 `IntentSeq` 与 dispatcher 引用，作为 `NotifyTarget` 暴露 `BlueprintCallable` 函数 `NotifySitSucceeded` / `NotifySitFailed`，并暴露 `CancelSilently()` 与 Move watcher 同语义（设 bArmed=false；后续 BP 触发的 NotifySit* 在 `if (!bArmed) return;` 短路）。

ActionDispatcher dispatch sit 时通过 `UFunction` 反射调 AI controller 上的 BP 函数 `UseSmartObjectAndNotify`（用 `FindFunctionByName` + `ProcessEvent`）；避免在 C++ 强制 binding 到 BP 节点。

**StopActiveAction 的 sit 路径处理**：调 `Pawn->GetController<AAIController>()` → 取 controller → 调 `StopMovementImmediately()` + 用 `USmartObjectSubsystem::ReleaseClaim(ClaimHandle)` 释放 slot（claim handle 需要 ActiveByActorId 缓存，从 watcher 取）。

#### 3.11.3 Wait watcher

简单 timer + `CancelSilently()`：派 wait 后立即 `SetTimerForNextTick` 触发 `Owner->OnActionTerminated(actor_id, intent_seq, Succeeded, duration_ms=0, final_pos=current, error_reason="")`（MVP 视为瞬间完成；后续可加 timeout 参数让 wait 持续 N 秒）。`CancelSilently()` 调 `ClearTimer` + `bArmed = false`。

### 3.12 ActionResultReporter `UAILiveProjectActionResultReporter`（`AILiveProjectActionResultReporter.h/cpp`）

```cpp
UCLASS()
class AILIVEPROJECT_API UAILiveProjectActionResultReporter : public UWorldSubsystem
{
    GENERATED_BODY()
public:
    void ReportSucceeded(const FString& ActorId, int64 IntentSeq, int32 DurationMs, const FVector& FinalPos);
    void ReportFailed(const FString& ActorId, int64 IntentSeq, int32 DurationMs, const FVector& FinalPos, const FString& ErrorReason);
};
```

实现：拼 `FAIL_ActionResultRequest` → `Client->PostActionResult(GameId, Req)` → 失败仅日志（HTTP 客户端层已重试）。**严格不**承载 cancel；不存在 `outcome=interrupted`。

### 3.13 SpeechResultReporter `UAILiveProjectSpeechResultReporter`（`AILiveProjectSpeechResultReporter.h/cpp`）

与 ActionResultReporter 平行 + 完全独立组件：

```cpp
UCLASS()
class AILIVEPROJECT_API UAILiveProjectSpeechResultReporter : public UWorldSubsystem
{
    GENERATED_BODY()
public:
    void ReportSpeechSucceeded(const FString& ActorId, int64 SpeechSeq, int32 DurationMs);
    void ReportSpeechFailed(const FString& ActorId, int64 SpeechSeq, int32 DurationMs, const FString& ErrorReason);
};
```

实现：拼 `FAIL_SpeechResultRequest` → `Client->PostSpeechResult(GameId, Req)` → brain 写 `speech.playback_resolved`（**不**写 `action.resolved`，不复用 action 通道）。

### 3.14 IngressValidator（`AILiveProjectIngressValidator.h/cpp`）

普通 static function library：

```cpp
class AILIVEPROJECT_API FAILiveProjectIngressValidator
{
public:
    static bool ValidateActionIntent(
        const FAIL_ActionIntentEvent& Event,
        const UAILiveProjectRosterSubsystem& Roster,
        FAIL_IngressRejectRequest& OutReject);

    static bool ValidateSpeechPublic(
        const FAIL_SpeechPublicEvent& Event,
        const UAILiveProjectRosterSubsystem& Roster,
        FAIL_IngressRejectRequest& OutReject);
};
```

校验项（任何一条不过 → 拒，并填 `OutReject` + 截 `raw_message_snippet` ≤ 512 chars）：

| 校验 | 失败 reject_reason |
| --- | --- |
| `actor_id` 在 Roster 注册集合 | `ACTOR_NOT_IN_ROSTER` |
| `speech.text.Len() <= 2000` | `SENTENCE_TOO_LONG` |
| `addressed_to` ⊆ `roster.actor_id`（**不**含 `"public"` —— `speech_pull.schema.json` 的 `addressed_to.items` 是 `$ref common.actor_id` 封闭枚举 NPC01..NPC10；**空数组表 broadcast**，不允许字面值 `"public"` 进数组） | `AUDIENCE_OUTSIDE_VISIBILITY` |
| `intent.name` ∈ `{move_to, sit, wait}` | `INTENT_NOT_IN_ONTOLOGY` |
| `move_to` 含 `target_zone`（且无 `coords` override） | `INTENT_NOT_IN_ONTOLOGY`（snippet 写明 `"target_zone='<zone>' unbound in MVP UE; T9 will introduce zone dictionary"`） |

**严格不**伪造 `system.validation_failed`——validation_failed 是 brain Reasoner schema 失败的特定语义（要求 `raw_llm_output`），不属 UE 范畴。UE 端拒只走 `system.ingress_rejected`。

### 3.15 SpeakDispatcher `UAILiveProjectSpeakDispatcher`（`AILiveProjectSpeakDispatcher.h/cpp`）

与 ActionDispatcher 平行 + 完全独立组件 + 独立 polling timer + 独立 `LastSpeechSeq` 高水位线：

```cpp
UCLASS()
class AILIVEPROJECT_API UAILiveProjectSpeakDispatcher : public UWorldSubsystem
{
    GENERATED_BODY()
public:
    void StartPolling(int32 IntervalMs);
    void StopPolling();

    // 供 WorldStateCollector 查 current_action
    bool HasSpeechForPawn(APawn* Pawn) const;

private:
    void TickPull();
    void DispatchOne(const FAIL_SpeechPublicEvent& Ev);

    int64 LastSpeechSeq = -1;  // -1 表示首次（与 brain server speech/pull 默认 since_seq 对齐）
    TMap<FString, int64> ActiveSpeechByActorId;   // actor_id → speech_seq（in-progress）
    FTimerHandle TimerHandle;
};
```

`DispatchOne(Ev)`：

1. **IngressValidator** 校验（actor_id ∈ roster；text 长度；addressed_to ⊆ visibility）→ 失败 POST `/ingress_reject`，不播报
2. 找 Pawn = `Roster->FindPawnByActorId(Ev.ActorId)`
3. 取 `MinimaxApiKey`：
   - 优先 `UAILiveProjectSettings::MinimaxApiKey`（非空时用）
   - 否则 fallback `UMinimaxACELibrary::GetMinimaxApiKeyFromProjectEnv()`
4. 调 `UMinimaxACELibrary::TriggerMinimaxSpeechFromPawnWithNoiseEx(this, Pawn, Ev.Text, ApiKey, /*VoiceId*/, /*Endpoint*/, /*A2FProvider*/, OnComplete)`（§3.16 新增的扩签名 API）
5. `OnComplete` delegate（在 GameThread 触发）：
   - succeeded → `SpeechResultReporter.ReportSpeechSucceeded(actor_id, speech_seq, duration_ms)`
   - failed → `SpeechResultReporter.ReportSpeechFailed(actor_id, speech_seq, duration_ms, error_reason)`
   - 清 `ActiveSpeechByActorId.Remove(actor_id)`

**严格不**做 LLM 决策；只查表 + 调既有 TTS 函数 + 上报。

### 3.16 MinimaxACELibrary 扩签名（`MinimaxACELibrary.h/cpp`）

沿用 DevLog 推荐的"新增独立函数 + 老函数保留"模式（避免破坏 BP_MH_Character_1 T 键调用现有 `TriggerMinimaxSpeechFromPawnWithNoise` 的链路）。

**`MinimaxACELibrary.h` 新增**：

```cpp
DECLARE_DYNAMIC_DELEGATE_ThreeParams(FOnSpeechCompleted, bool, bSucceeded, float, DurationSeconds, FString, ErrorReason);

UFUNCTION(BlueprintCallable, Category = "Minimax|ACE",
    meta = (WorldContext = "WorldContextObject",
            AdvancedDisplay = "VoiceId,Endpoint,A2FProviderName",
            DisplayName = "Trigger Minimax Speech From Pawn With Noise (with completion)"))
static void TriggerMinimaxSpeechFromPawnWithNoiseEx(
    UObject* WorldContextObject,
    AActor* SpeakerPawn,
    const FString& Text,
    const FString& ApiKey,
    FOnSpeechCompleted OnComplete,
    const FString& VoiceId = TEXT("male-qn-qingse"),
    const FString& Endpoint = TEXT("https://api.minimaxi.com/v1/t2a_v2"),
    FName A2FProviderName = FName(TEXT("LocalA2F-James")));
```

**老函数 `TriggerMinimaxSpeechFromPawnWithNoise` 保留不动**——既有 BP_MH_Character_1 T 键链路通过它走 fire-and-forget；C++ 内部 `TriggerMinimaxSpeechFromPawnWithNoise` 改为内部调 `TriggerMinimaxSpeechFromPawnWithNoiseEx` + 传一个 noop delegate（实现细节复用，不改外部签名）。

**完成时机**（关键修订，避开双倍延迟）：

`MinimaxACELibrary.cpp:238` 注释明确：`AnimateFromAudioSamples 是阻塞 streaming dispatch（多个 chunk 串行 send 到 ACE thread），实测可能持续数秒`。也就是说音频实际开播 ≈ AnimateFromAudioSamples 调用**起点**（chunk 0 已 dispatch），不是返回点。如果等 AnimateFromAudioSamples 返回后再启 timer 等 duration → 实际延迟 = streaming dispatch 时长 + audio 时长（双倍）。

正确时机：
- HTTP TTS 返回后回到 GameThread（既有路径已经有这一步——见 `MinimaxACELibrary.cpp:243` 的 `AsyncTask(ENamedThreads::GameThread, ...)`）
- 在 GameThread 上、调 `ReportNoiseEvent` 之后、调 `AnimateFromAudioSamples` **之前**：`FTimerManager::SetTimer(EstimatedDurationSec, OnCompleteWrapper)`，`EstimatedDurationSec = Result.Samples.Num() / float(Result.SampleRate)`（同一线程同一 tick 排 timer 与 dispatch，确保 timer 起点 ≈ audio 实际开播刹那）
- timer 触发时调 `OnComplete(true, EstimatedDurationSec, "")`
- 失败路径（HTTP TTS 失败、ACE provider 不可用、PCM 解析失败、Consumer 销毁等）→ 立即 `OnComplete(false, 0, ErrorReason)`，不排 timer
- 实施时把上述路径放进 `TriggerMinimaxSpeechFromPawnWithNoiseEx` 内部；`TriggerMinimaxSpeechFromPawnWithNoise` 老函数转调新函数 + 传 noop delegate（保持 BP_MH_Character_1 T 键链路无副作用）

### 3.17 测试键退役（蓝图改动，**一次性退**）

**节奏：一次性退**——等所有 brain 通道（move_to / sit / wait / speak）端到端验证全部通过后，**单次** Level BP 改动批量删 M / K / N / L 输入事件链。

退役范围：

| 按键 | 当前用途 | 退役条件 | 本卡是否退役 |
| --- | --- | --- | --- |
| **M** | 测试 move_to | brain 派 `move_to` 端到端通过 | ✅ |
| **K** | 测试 sit | brain 派 `sit` 端到端通过 | ✅ |
| **N** | 测试 wait | brain 派 `wait` 端到端通过 | ✅ |
| **L** | 测试 speak | brain 派 `speech.public` 端到端通过 | ✅ |
| **I** | 触发 `AAct01RuleIntroDirector` | 与 Mind 正交，离线导演 | ❌（保留） |
| **O** | 触发 `AStoryScenarioDirector` | 与 Mind 正交，离线导演 | ❌（保留） |
| `BP_MH_Character_1` 的 T 键 + 6 个 A2F 调试变量 | A2F 调试 | 调试特例 | ❌（保留，第 5 阶段一并清理） |

操作步骤（用 Monolith MCP）：

1. `mcp__monolith__monolith_status` 确认 UE Editor 在线
2. `mcp__monolith__blueprint_query` 读 Level BP（`L_prison.umap` 的 Level Blueprint）当前节点拓扑，导出 M/K/N/L input event 节点 + 它们直连的下游函数节点
3. 用 MCP 批量 delete 4 个 input event 起始节点 + 直连函数链（**注意**保留 I/O 键链路 + 任何 unrelated 节点）
4. `compile_blueprint` 重编译，验证无错误
5. PIE 启动，依次按 M/K/N/L 键 → 确认按下后**无任何反应**（已退役）；按 I/O 键 → 验证 director 触发仍正常
6. 重跑 §6 端到端联调全套场景，验证 brain 派发 dispatch 路径仍正常

---

## 4. 范围外（DON'T）

- ❌ **不**实现 brain HTTP server 路由代码（属 T03-5，已落）
- ❌ **不**修改既有 perception / move / sit / TTS 原语签名（CLAUDE.md 红线：`MinimaxACELibrary` / `PerceptionLogger` / `ScatterMover` / `SightMemoryComponent` / `AILiveAgent`）
- ❌ **不**动 `AStoryScenarioDirector` / `AAct01RuleIntroDirector`（与 Mind 正交，按 PRD 决定退役时机）
- ❌ **不**写 02 病毒游戏专属事件 / `world.touch` / 感染状态视觉化（属 T9 / T10）
- ❌ **不**实现 SmartObject zone 字典（target_zone 路径在本卡仅由 IngressValidator 拒）—— 等 T9 病毒游戏定义具体 zone
- ❌ **不**实现观众调试视图 / `audience` 可见的衍生事件
- ❌ **不**实现 brain 进程崩溃后的 UE 端容错（仅"重试 + 日志"，**不**"恢复 game_id"，**不**"自动重新握手"）
- ❌ **不**引入 SSE / WebSocket 模块依赖（CLAUDE.md / T1 禁；`FHttpModule` polling 即可）
- ❌ **不**引入新 LLM provider 调用 / prompt 拼装 / 长期 memory（CLAUDE.md 边界：脑层外置）
- ❌ **不**持久化 `last_*_seq` 高水位线 / `Idempotency-Key` / `client_sample_id`（硬约束 k：MVP 不跨 PIE 进程恢复）
- ❌ **不**升 `protocol_version`（保持 0.1.1）—— 本卡未发现 protocol 漏洞，target_zone 拒走 INTENT_NOT_IN_ONTOLOGY 复用
- ❌ **不**改 `Source/*.Target.cs` `DefaultBuildSettings = V6`（CLAUDE.md 红线）
- ❌ **不**改 `Config/DefaultEngine.ini` 的 `bTickPhysicsAsync`（CLAUDE.md 红线）
- ❌ **不**预留物品 / 跟随 / 逃离等非 MVP intent；后续确有需求时再按协议升版新增
- ❌ **不**采集 ENTER/EXIT 事件（00_total_plan.md:147 提及但 `world_state_push.schema.json` 0.1.1 不含该字段；本卡明确从范围裁掉，留 T9/T10 评估是否升 protocol minor 加可选字段——见 §3.9.3）

**注意**：本卡**会**在 §3.0 同步 `Docs/protocol_pointer.md` commit hash（追赶 BrainService HEAD + `/health` bugfix 后的新 commit），同时**不**升 `protocol_version`（保持 0.1.1）。同步动作不算"改协议"——是 UE 仓 client 跟上 BrainService server 实现的 bugfix。

---

## 5. 交付清单

### 5.1 新建 C++ 文件

| 路径 | 一句话职责 |
| --- | --- |
| `Source/AILiveProject/Public/AILiveProtocolTypes.h` | 12 份 schema 的 USTRUCT 镜像（`FAIL_*`）+ 4 个 UENUM |
| `Source/AILiveProject/Private/AILiveProtocolTypes.cpp` | UENUM ToString/FromString 实现 |
| `Source/AILiveProject/Public/AILiveProtocolJson.h` | `ToJsonString` / `FromJsonString` 模板 + 工具 helper |
| `Source/AILiveProject/Private/AILiveProtocolJson.cpp` | 偏特化（含 oneOf / enum 字段）+ helper 实现 |
| `Source/AILiveProject/Public/AILiveProjectSettings.h` | `UAILiveProjectSettings : UDeveloperSettings`（Brain URL / token / polling intervals / Minimax key） |
| `Source/AILiveProject/Private/AILiveProjectSettings.cpp` | 默认值实现 |
| `Source/AILiveProject/Public/AILiveProjectBrainHttpClient.h` | `FAILiveProjectBrainHttpClient` HTTP 客户端类（10 endpoint 的 async API） |
| `Source/AILiveProject/Private/AILiveProjectBrainHttpClient.cpp` | `FHttpModule` 实现 + Authorization / Idempotency-Key / 指数退避 |
| `Source/AILiveProject/Public/AILiveProjectRosterSubsystem.h` | `UAILiveProjectRosterSubsystem : UGameInstanceSubsystem`（actor_id ↔ Pawn 双向 map） |
| `Source/AILiveProject/Private/AILiveProjectRosterSubsystem.cpp` | actor_id 推断（BP class name 正则）+ map 维护 |
| `Source/AILiveProject/Public/AILiveProjectBrainSessionSubsystem.h` | `UAILiveProjectBrainSessionSubsystem : UGameInstanceSubsystem`（health → session → roster → polling 启动序列） |
| `Source/AILiveProject/Private/AILiveProjectBrainSessionSubsystem.cpp` | 启动序列 future continuation 链 |
| `Source/AILiveProject/Public/AILiveProjectWorldStateCollector.h` | `UAILiveProjectWorldStateCollector : UWorldSubsystem`（周期 push） |
| `Source/AILiveProject/Private/AILiveProjectWorldStateCollector.cpp` | TickPush + perception 转 USTRUCT + current_action 推断 |
| `Source/AILiveProject/Public/AILiveProjectActionDispatcher.h` | `UAILiveProjectActionDispatcher : UWorldSubsystem`（拉 + 路由 + cancel） |
| `Source/AILiveProject/Private/AILiveProjectActionDispatcher.cpp` | TickPull + DispatchOne + StopActiveAction |
| `Source/AILiveProject/Public/AILiveProjectActionCompletionWrapper.h` | 3 个 watcher（Move / Sit / Wait） |
| `Source/AILiveProject/Private/AILiveProjectActionCompletionWrapper.cpp` | watcher 实现 + BP 反射调用 |
| `Source/AILiveProject/Public/AILiveProjectActionResultReporter.h` | `UAILiveProjectActionResultReporter : UWorldSubsystem`（POST /actions/result） |
| `Source/AILiveProject/Private/AILiveProjectActionResultReporter.cpp` | Report{Succeeded,Failed} 实现 |
| `Source/AILiveProject/Public/AILiveProjectSpeechResultReporter.h` | `UAILiveProjectSpeechResultReporter : UWorldSubsystem`（POST /speech/result） |
| `Source/AILiveProject/Private/AILiveProjectSpeechResultReporter.cpp` | ReportSpeech{Succeeded,Failed} 实现 |
| `Source/AILiveProject/Public/AILiveProjectIngressValidator.h` | static class 校验 ActionIntent / SpeechPublic |
| `Source/AILiveProject/Private/AILiveProjectIngressValidator.cpp` | 校验项实现（含 target_zone 拒） |
| `Source/AILiveProject/Public/AILiveProjectSpeakDispatcher.h` | `UAILiveProjectSpeakDispatcher : UWorldSubsystem`（拉 + 路由 TTS） |
| `Source/AILiveProject/Private/AILiveProjectSpeakDispatcher.cpp` | TickPull + DispatchOne + 调 TriggerMinimaxSpeechFromPawnWithNoiseEx |
| `Source/AILiveProject/Tests/AILiveProtocolRoundTripTest.cpp` | 20 个 example fixture round-trip 测试 |
| `Source/AILiveProject/Tests/AILiveProjectIngressValidatorTest.cpp` | IngressValidator 5 类拒绝路径单元测试 |
| `Source/AILiveProject/Tests/AILiveProjectActionDispatcherTest.cpp` | DispatchOne 路由 + cancel 路径单元测试（mock HTTP 客户端） |
| `Source/AILiveProject/Tests/Fixtures/protocol_examples/*.json` | vendored copy 自 `BrainService/protocol/examples/` 全部 20 份 |

### 5.2 修改 C++ 文件

| 路径 | 改动 |
| --- | --- |
| `Source/AILiveProject/AILiveProject.Build.cs` | `PublicDependencyModuleNames` 加 `"DeveloperSettings"` |
| `Source/AILiveProject/Public/MinimaxACELibrary.h` | 新增 `FOnSpeechCompleted` delegate + `TriggerMinimaxSpeechFromPawnWithNoiseEx` UFUNCTION |
| `Source/AILiveProject/Private/MinimaxACELibrary.cpp` | 新函数实现（timer 排在 AnimateFromAudioSamples 之前，避免双倍延迟）+ 老函数 `TriggerMinimaxSpeechFromPawnWithNoise` 内部转调新函数（保持外部签名） |

### 5.2.1 BrainService 仓改动（§3.0 前置，独立 commit）

| 路径 | 改动 |
| --- | --- |
| `BrainService/brain/server/routes/health.py` | `return {"ok": True, ...}` → `return {"status": "ok", ...}`（对齐 schema 的 `additionalProperties: false`） |
| `BrainService/tests/test_server_health.py` | 加 `assert resp.json()["status"] == "ok"` 与 `assert "ok" not in resp.json()` 防回归 |

**绝不动**：
- `Source/AILiveProject/Public/AILiveProjectPerceptionLogger.h` / `.cpp`
- `Source/AILiveProject/Public/SightMemoryComponent.h` / `.cpp`
- `Source/AILiveProject/Public/AILiveAgent.h`
- `Source/AILiveProject/Public/AILiveProjectScatterMover.h` / `.cpp`
- `Source/AILiveProject/Public/StoryScenarioDirector.h` / `.cpp`
- `Source/AILiveProject/Public/Acts/Act01RuleIntroDirector.h` / `.cpp`
- `Source/AILiveProject.Target.cs` / `Source/AILiveProjectEditor.Target.cs`

### 5.3 蓝图改动（全部用 Monolith MCP 操作）

| 资产 | 改动 | 说明 |
| --- | --- | --- |
| `Content/Blueprints/AI/AIC_NPC_SmartObject.uasset`（NPC AI controller，10 个 NPC 共用） | 新增 BlueprintCallable 函数 `UseSmartObjectAndNotify(SmartObjectActor, IntentSeq, NotifyTarget)` | sit 路径 BP wrapper（C++ 不能直调 LatentTask；目标改 controller，不用 NPC BP——10 个独立 BP，无共用父类，详见 §3.11.2） |
| `Content/Blueprints/GM_Sandbox.uasset`（GameMode） | BeginPlay 节点链调 `GetGameInstance() → GetSubsystem<UAILiveProjectBrainSessionSubsystem>() → StartHandshake()` | 唯一握手触发点（详见 §3.8） |
| `Content/MyAssets/Levels/L_prison`（Level BP） | 删除 M / K / N / L input event 链 + 直连函数节点 | **一次性退**（dispatcher 全链 OK 后） |

**保留不动**：
- `Content/Blueprints/SandboxCharacter_Mover`（GASP 父类，move 原语）
- `Content/Blueprints/AC_VisualOverrideManager`
- `Content/Blueprints/AI/AIC_NPC_SmartObject` 的既有感知、SightMemory、Hearing、SmartObject 逻辑保持不动；本卡只允许追加 `UseSmartObjectAndNotify` wrapper 函数
- `BP_MH_Character_1` 的 T 键链 + 6 个 A2F 调试变量
- Level BP 的 I / O 键链（director 触发，与 Mind 正交）

### 5.4 配置

| 路径 | 改动 |
| --- | --- |
| `Config/DefaultGame.ini` | 新增 `[/Script/AILiveProject.AILiveProjectSettings]` 段（默认值见 §3.5） |

**绝不动**：`Config/DefaultEngine.ini`（CLAUDE.md 红线 + GameModeMapPrefixes 等关键键已存在）。

### 5.5 文档

| 路径 | 改动 |
| --- | --- |
| `Docs/protocol_pointer.md` | **§3.0 同步**：commit hash 从 `c271383...` 升到本卡 §3.0 提交的 `/health` bugfix 后的 BrainService HEAD；`protocol_version` 保持 `0.1.1`（不升）；占位的"UE C++ round-trip 测试位置"填为 `Source/AILiveProject/Tests/AILiveProtocolRoundTripTest.cpp` |
| `Docs/Roadmap/handbook.md` §1 | 由人类（不是 ClaudeCode）在卡完成后将 T6+T7 状态从 ⬜ 改为 ✅ + §5 进度跟踪表追加一行 |

---

## 6. 验收（机器化优先 + 真实端到端联调）

**本卡定位：阶段 3 全部硬验收 = brain↔UE 真实闭环跑通**。T10 仅做 LLM 真实多 agent 联调，不再补做基础链路验证。

### 6.1 静态 / 单元测试

- [ ] §3.0 BrainService `/health` bugfix 已提交独立 commit（`status: "ok"` 替换 `ok: True`）；`pytest tests/test_server_health.py` 含新加断言并通过
- [ ] `Docs/protocol_pointer.md` commit hash 已升级到 §3.0 bugfix 后的 BrainService HEAD；`protocol_version` 仍为 `0.1.1`
- [ ] vendored `Source/AILiveProject/Tests/Fixtures/protocol_examples/` 与 `BrainService/protocol/examples/` sha256 全部一致（运行 §3.0 给的 PowerShell 校验脚本无报错）
- [ ] `Build.bat AILiveProjectEditor Win64 Development` UBT 全量重建通过（首次加 `DeveloperSettings` 必须全量）
- [ ] BrainService `cd BrainService && pytest -q` 全绿（含 §3.0 新加的 health 断言）
- [ ] UE 端 round-trip 测试 `AILiveProtocolRoundTripTest.cpp` 全绿（覆盖全部 20 个 example fixture，含 happy + failure 路径；其中 `event_query.success.json` 必须能反序列化为 `TArray<FAIL_EventMeta>` 而非 `TArray<FString>`）
- [ ] UE 端 IngressValidator 单元测试 5 路径全绿（ACTOR_NOT_IN_ROSTER / SENTENCE_TOO_LONG / AUDIENCE_OUTSIDE_VISIBILITY 含**字面值 `"public"` 入数组应拒** / INTENT_NOT_IN_ONTOLOGY / target_zone 拒）
- [ ] UE 端 ActionDispatcher 单元测试 dispatch + cancel 路径全绿；**新增**：覆盖式 cancel 后旧 watcher 不能上报 result（mock watcher 在 CancelSilently 后 fire OnActionTerminated → dispatcher 入口 IntentSeq 比对丢弃 → 验证 ActionResultReporter 未被调）

### 6.2 端到端联调流程

每次执行前从干净状态启动 brain server（用 PowerShell）：

```powershell
$env:BRAIN_API_TOKEN = "dev_token"
$env:LLM_PROVIDER = "fake"
$env:BRAIN_HOST = "127.0.0.1"
$env:BRAIN_PORT = "8000"
$env:BRAIN_JUDGE_WORKER = "0"
cd D:\Project\Unreal\AILiveProject\BrainService
python -m brain.server
```

UE 启 PIE 后 Output Log 必须依次出现：

- `[AILiveBrain] Health OK protocol_version=0.1.1`
- `[AILiveBrain] Session created game_id=<uuid v4>`
- `[AILiveBrain] Roster registered count=10`
- 之后每 N ms 出现 `PullActions` / `PullSpeech` / `PushWorldState` 日志

### 6.3 端到端场景验收（curl + PIE 物理观察）

| # | 场景 | 操作 | 期望结果 |
| --- | --- | --- | --- |
| 1 | move_to(target_npc) | curl 在 brain 写一条 `action.intent` 让 NPC03 move_to NPC07 | UE NPC03 物理移动到 NPC07 附近 → `ActionResultReporter` POST `/actions/result` → brain 事件流出现 `action.resolved` (succeeded) |
| 2 | move_to(coords) | brain 写 `action.intent` move_to coords{x,y,z} | UE 物理移动到坐标 → `action.resolved` succeeded |
| 3 | sit(target_smartobject) | brain 写 `action.intent` sit target_smartobject="BP_SmartBench_Example"`（DevLog 已验证的关卡 SmartObject actor；如资产 label 变更，以 MCP 读到的当前 actor name/label 为准） | UE NPC 走向 SmartObject 椅子 + 坐下动画 + claim slot → `action.resolved` succeeded |
| 4 | wait | brain 写 `action.intent` wait reason="observing" | UE 不动 → 立即 `action.resolved` succeeded |
| 5 | speech.public | brain 写 `speech.public(NPC02, "你好")` | UE NPC02 TTS 播报 + MetaHuman 嘴型动 + AI Hearing 命中近场 NPC + `SpeechResultReporter` POST → brain 写 `speech.playback_resolved`（**不**写 `action.resolved`） |
| 6 | non-roster actor_id | brain 写 `action.intent(NPC11, ...)` | UE IngressValidator 拒 → POST `/ingress_reject` reject_reason=`ACTOR_NOT_IN_ROSTER` → brain 写 `system.ingress_rejected`（**不**写 `system.validation_failed`） |
| 7 | target_zone（MVP 未绑定） | brain 写 `action.intent(NPC03, move_to target_zone="dining_hall")` | UE IngressValidator 拒 → POST `/ingress_reject`（reject_reason=`INTENT_NOT_IN_ONTOLOGY`，snippet 含 "target_zone unbound"）→ brain 写 `system.ingress_rejected` |
| 8 | 覆盖式 cancel | brain 对同一 NPC 连续派两条 move_to（不同目标），同事务派出 `action.cancelled` + 新 `action.intent` | UE 第二条到达时 dispatcher 调 `OldWatcher->CancelSilently()` → 调 `StopActiveAction(Pawn)` 物理停止第一条移动 + 切换到新目标；UE 端 `ActionResultReporter` **未上报** first intent 的任何 result（GET `/v1/games/{game_id}/events?since_seq=0` 抽查事件流，**确认不存在** `source_event_id == first_intent_seq` 的 `action.resolved` 行）；brain 事件流出现 `action.cancelled`（`source_event_id` = first intent_seq）+ 新 `action.intent` 同 seq 区间；最终新 intent 完成时上报 `action.resolved` succeeded（`source_event_id` = new intent_seq） |
| 9 | TTS 失败（mock） | SpeakDispatcher 注入失败 fixture（如 ApiKey 错） | `SpeechResultReporter` POST status=failed + error_reason → brain 写 `speech.playback_resolved` failed |
| 10 | brain 离线重连 | PIE 期间 kill brain server → 等 30s → 重启 | UE 端 polling 失败 → 退避重试 → brain 重启后旧 game_id 已失效，UE 端会 401/404 → MVP 阶段记日志即可，**不**要求 UE 自动重新握手 |

### 6.4 事件流审计

每场端到端联调结束后：

- [ ] 跑 `verify_chain(game_id)` 必须返回 True（事件流哈希链不断）
- [ ] `GET /v1/games/{game_id}/events?since_seq=0` 抽查事件流，必须含：
  - 1 条 lifecycle `created`（per actor，T2 写入）
  - 若干 `world.perception.sight` / `world.perception.hearing` / `world.actor_state` / `world.client_sample`
  - 4 个 ontology v1 intent（move_to / sit / wait + speech 触发的 N 条）对应的 `action.intent` + `action.resolved`
  - 至少 1 条 `action.cancelled`（覆盖式 cancel 场景）
  - 至少 1 条 `speech.public` + `speech.playback_resolved`
  - 至少 2 条 `system.ingress_rejected`（non-roster + target_zone 各 1）

### 6.5 测试键退役验收（端到端联调全部通过后执行）

- [ ] M / K / N / L 测试键链已从 Level BP 删除（用 MCP `blueprint_query` 验证对应 input event 节点不存在）
- [ ] I / O 键 + T 键链路保留（按 I 键 → `AAct01RuleIntroDirector` 仍能触发开门 + 散点 + 视频；按 O 键 → `AStoryScenarioDirector` 仍能触发餐厅相遇）
- [ ] 退役后再跑一次完整 §6.3 端到端联调，所有 10 个场景仍通过

### 6.6 人工评审

- [ ] 由人类（不是 ClaudeCode）评审 §3 设计 + 退役节奏 + 端到端联调录屏 / 截图
- [ ] handbook.md §1 表格的 T6 / T7 状态由人类从 ⬜ 改为 ✅，§5 进度跟踪表追加一行登记

---

## 7. 上下游交接

**前置卡**：T1 ✅ T2 ✅ T03-5 ✅。本卡所有上行 payload 严格按 T1 schema；下行消费的 `action.intent` / `speech.public` 必须来自 T03-5 orchestrator 派出的事件。

**下游消费者**：

| 后续卡 | 依赖本卡的什么 |
| --- | --- |
| T9（02 病毒游戏 brain 端机制 + zone 字典） | brain 端新增 zone 字典 + 派 target_zone 时本卡 IngressValidator 改为放行 + `UAILiveProjectScatterMover::ScatterNPCsAroundTarget` 接入路径；本卡保留 `target_zone` 拒桩位以备 T9 升级 |
| T10（UE touch 检测 + 一局完整 PIE 联调） | 全部 dispatcher / wrapper / reporter / collector 链路；T10 在 BP_NPC_MH_Character 加 touch detection 组件 + 在 brain 写 touch 事件类型，复用本卡的 PushWorldState 通道 |
| 后续观众调试视图 | 本卡的 `speech.playback_resolved` / `system.ingress_rejected` 默认 visibility 是 `["orchestrator", "system"]`；如需 audience 可见，按 T1 §8 与 §4.1.5 拆事件类型派生衍生事件，不在 UE 端默认开放 |

---

## 8. 风险与已知坑

- **DeveloperSettings 模块依赖首次加 → 必须 UBT 全量重建**（CLAUDE.md "改 Build.cs 才需要全量"）。后续 `.cpp` / `.h` 走 Live Coding。
- **§3.0 bugfix 越权问题**：本卡 §3.0 在 BrainService 仓提一个 `/health` bugfix，与本卡 DON'T "不实现 brain HTTP server 路由代码"看似矛盾——但 health 现状是 schema 与 server 实现**不一致**（server 返回 `ok:true`，schema 要求 `status:"ok"`，且 `additionalProperties: false`），不修则 UE 端按 schema 反序列化必失败。这是 UE 端落地协议 client 的硬阻断，必须先在源头修；这不是新增 server 路由代码，是已有路由的实现 bug。bugfix 不动 schema、不动 protocol_version、不改其他路由——是最小修补。
- **MinimaxACELibrary 扩签名 + delegate → Live Coding 反射 bug 风险**。预防：扩签名后第一次走 UBT 全量；老函数 `TriggerMinimaxSpeechFromPawnWithNoise` 签名**不变**，仅内部转调新函数。
- **MCP 改 BP 的限制**（`feedback_mcp_limitations.md`）：
  - sit 路径必须用 `UseSmartObjectWithGameplayInteraction`，在 BP wrapper 里先绑定 `OnSucceeded` / `OnFailed`，再手动 `ReadyForActivation`；不要用 `MoveToAndUseSmartObjectWithGameplayInteraction`（DevLog `2026-04-29` 已记）
  - BP 函数 `UseSmartObjectAndNotify` 由 C++ 通过 `FindFunctionByName` + `ProcessEvent` 反射调用
- **NPC `current_action` 推断启发式**：当前 NPC 没有 GAS，按 §3.9.1 表合成（move/sit/speak in-progress 信号 → idle）；可能漏边界 case（如玩家手动调 BP 函数），MVP 接受
- **`wall_clock_ts` 时区**：必须 UTC + ISO8601 + `Z` 后缀（`FDateTime::UtcNow().ToIso8601()` 生成的格式末尾加 `Z`，因为 UE 5.7 `ToIso8601` 默认无 `Z`）；不要带本地时区
- **`Idempotency-Key` 生成与重试**：每次 POST call 一个新 UUID v4；同一 call 的 5xx 重试**用同一 key**（HTTP 客户端层在 retry 循环外只生成一次 key）；`FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens).ToLower()`
- **`client_sample_id` 单调**：`int32` 累加，从 0 开始；GameInstance 重启重置（与 game_id 一起重置）；不持久化
- **HTTP 完成回调线程**：`FHttpModule::CreateRequest()->OnProcessRequestComplete()` 默认在 GameThread 触发，可以直接调 UObject API。**不要**用 `IHttpRequest::SetURL` 之外的多线程接口
- **PIE 关闭清理**：所有 in-flight HTTP 请求必须 cancel（`FHttpRequestPtr->CancelRequest()`）；周期 timer 必须 unregister；`UWorldSubsystem` 自然 destroy；`UGameInstanceSubsystem::Deinitialize` 是终点
- **中文注释 .h/.cpp/.cs 必须 UTF-8 BOM**（`feedback_chinese_comments_msvc.md`）：MSVC 否则按 GBK 误判触发 C4010；本卡新文件全部用 UTF-8 BOM
- **AI Hearing 必须在 GameThread 上报**：SpeakDispatcher 调用的是 `TriggerMinimaxSpeechFromPawnWithNoiseEx`，内部沿用既有 `TriggerMinimaxSpeechWithNoise` 的 GameThread dispatch 路径（HTTP TTS 完成 → AsyncTask GameThread → ReportNoiseEvent），不重写
- **不要伪造 `system.validation_failed`**：UE 端拒只走 `/ingress_reject` → `system.ingress_rejected`；validation_failed 是 brain Reasoner schema 失败的特定语义（要求 `raw_llm_output`），不属 UE 范畴
- **target_zone 路径 MVP 不实现**：T9 才会定义具体 zone 字典（餐厅 / 走廊 / 单人间等）；本卡如收到 `target_zone` 直接 IngressValidator 拒 reject_reason=`INTENT_NOT_IN_ONTOLOGY`（snippet 写明 "target_zone unbound in MVP UE; T9 will introduce zone dictionary"），**不**新增 reject_reason enum 值，**不**升 protocol 版本
- **覆盖式 cancel 双重保险**：`action.cancelled` 与 `action.resolved` 互斥终态硬约束（memory_principles §5.4.2）极其敏感——一个旧 watcher 在 cancel 后 fire OnActionTerminated 就会写出双终态 bug。本卡用**双层保护**：(a) ActionDispatcher 在 dispatch 新 intent 时**先**调 `OldWatcher->CancelSilently()`（让 watcher 本地 disarm，Tick / delegate 都短路）；(b) ActionDispatcher.OnActionTerminated 入口比对 IntentSeq 与 `ActiveByActorId[actor_id].IntentSeq`，不匹配丢弃。两层都必须实现，缺一会让 race condition 漏网。
- **TOptional UPROPERTY 限制**：UE 反射不支持 `TOptional<T>` 作为 UPROPERTY；本卡所有 nullable 字段（parent_event_id / prev_hash / coords / error_reason 等）走 "bool flag + value" 模式（§3.2.4）。
- **PerceptionLogger 返回 BP class FName 而非 Pawn 指针**：`Source/AILiveProject/Private/AILiveProjectPerceptionLogger.cpp:105/206` 实测返回的 `Identity` 是 `StripBlueprintSuffix(A->GetClass()->GetFName())`——BP class 名（如 `BP_NPC_MH_Character_3`），不是 Pawn 指针。WorldStateCollector 必须用 `RosterSubsystem::TryGetActorIdForBPClassFName(FName) → FString actor_id` 反查（§3.7 已加），不能用 `PawnToActorId` 反向 map。
- **AIC_NPC_SmartObject 是 sit BP wrapper 的唯一可改资产**：实测 `Content/Blueprints/NPCs/` 仅有 10 个独立 NPC BP（无共用父类），父类是 `SandboxCharacter_Mover.uasset`（CLAUDE.md 红线）。AI controller 是所有 NPC 共用的可修改点，sit 业务挂在 controller 而非 Pawn 也更符合"AI 行为由 controller 持有"的 UE 惯例。
- **brain 进程崩溃 / UE 不崩**：MVP 进程内 idempotency dict 缓存清空；UE 端旧 game_id 已失效。MVP 阶段仅记日志，**不**实现 UE 端自动重新握手；用户手动重启 PIE
- **HTTP 客户端单元测试**：UE 5.7 没有官方 mock 路径；MVP 单元测试用本地 brain server（启 `python -m brain.server`）做半集成测试；不强求 100% mock 隔离
- **vendored fixture 同步**：本卡完成时手工把 `BrainService/protocol/examples/*.json` 复制到 `Source/AILiveProject/Tests/Fixtures/protocol_examples/`；后续 protocol 升版时由更新 `Docs/protocol_pointer.md` 的同一 commit 一并 vendored
- **roster 注册时机**：必须在 `WorldStateCollector` / `ActionDispatcher` / `SpeakDispatcher` 启动**之前** + GameMode `BeginPlay` 完成 Pawn spawn **之后**。BrainSession 在 `Initialize` 时不连 brain，等 GameMode hook 触发 `StartHandshake`
- **`IAILiveAgent` 实现 Pawn 枚举**：用 `TActorIterator<APawn>` + `Pawn->Implements<UAILiveAgent>()`；空场景或 PIE 立刻关 → roster 0 个 Pawn → brain 端 `register_roster` schema `minItems: 1` 拒 → 本卡需在 BrainSession 检查 `Roster->NumRegistered() == 0` → fatal 日志 + 拒绝启动
- **NavMover 与 GASP Mover 移动状态查询**：`AAIController::GetMoveStatus()` 在使用 NavMoverComponent + Mover 2.0 的 NPC 上是否准确？需要在实施时实测；如不准则改为查询 NavMover 的内部状态或用 `MoveAndLookAtLocation` 的 BP 完成 delegate（如有暴露）

---

## 9. 启动 prompt（粘贴到新 ClaudeCode 窗口的开场）

```text
你是 AILive 项目 Memory Principles 路线图的 T6+T7 子任务执行者：UE 协议基础设施 + Dispatcher 主体 + 测试键退役。

**必读上下文（按顺序读完再动手）**：
1. Docs/Roadmap/00_total_plan.md ——总路线图（重点："边界" / "阶段 3" / "需要保留 / 不动的 UE 资产清单" / "可退役 / 待评估的 UE 资产" / "风险与开放决策"）
2. Docs/Roadmap/handbook.md §0 + §4
3. Docs/memory_principles.md：硬约束 5 / 6、§4.1.2、§4.1.5、§5.4.2
4. Docs/Roadmap/T01_protocol_skeleton.md §3.2 / §3.3 / §3.4
5. Docs/Roadmap/T02_brain_data_layer.md §3.2 / §3.4
6. Docs/Roadmap/T03-5_brain_protocol_layer.md §3.4 / §3.5
7. Docs/Roadmap/T06-7_ue_glue_main.md ——本任务详设卡（已生成）
8. BrainService/protocol/protocol.md + schemas/*.json + examples/*.json + brain/server/routes/*.py
9. Source/AILiveProject/Public/{MinimaxACELibrary,AILiveProjectPerceptionLogger,SightMemoryComponent,AILiveAgent}.h
10. Source/AILiveProject/AILiveProject.Build.cs + Source/*.Target.cs
11. CLAUDE.md（项目根） + 用户级 ~/.claude/CLAUDE.md
12. auto memory：feedback_unreal_tooling.md / feedback_precheck_before_BP_changes.md / feedback_monolith_patterns.md / feedback_mcp_limitations.md / feedback_chinese_comments_msvc.md

**任务范围（DO）**：T06-7 §3 全部子节（USTRUCT 镜像 + JSON helper + round-trip 测试 + Settings + HTTP 客户端 + RosterSubsystem + BrainSession + WorldStateCollector + ActionDispatcher + ActionCompletionWrapper + ActionResultReporter + SpeechResultReporter + IngressValidator + SpeakDispatcher + MinimaxACELibrary 扩签名 + 测试键退役）

**范围外（DON'T）**：T06-7 §4 全部条目（不动 brain server / 不动既有原语签名 / 不动 director / 不动 protocol 版本 / 不持久化 last_seq / Idempotency-Key 等）

**约束**：
- 严格遵守 CLAUDE.md（项目级 + 用户级）：暴露假设、surgical changes、goal-driven。
- 所有 UE 资产改动用 Monolith MCP（不让用户手点编辑器）。
- 改 Build.cs 加 DeveloperSettings → 必须 UBT 全量重建；其余 .cpp / .h 走 Live Coding。
- 中文注释的 .h/.cpp/.cs 必须 UTF-8 BOM。
- 完成时按 T06-7 §6 验收清单逐条勾选；端到端联调全 10 个场景必须跑通。

**第一步**：先读完上述全部必读文件，然后用 1–3 句话给我复述：
(a) 你理解的本卡范围与上下游边界；
(b) 为什么 speech.public 与 action.intent 必须通道分离；
(c) action.cancelled 与 action.resolved 为什么是互斥终态、UE 为什么不上报 interrupted；
(d) target_zone MVP 阶段为什么由 IngressValidator 拒（且复用 INTENT_NOT_IN_ONTOLOGY）；
(e) 测试键退役为什么是一次性退（不渐进）；
(f) Idempotency-Key 在 UE 端怎么生成、重试时同 key 还是新 key；
(g) §3.0 为什么必须先在 BrainService 修 `/health` bugfix（schema 与 server 实现不一致的具体表现）+ 同步 `Docs/protocol_pointer.md`；
(h) 覆盖式 cancel 的"双层保护"是哪两层，为什么少一层都会漏（举一个 race condition 例子）；
(i) WorldStateCollector 为什么必须用 `RosterSubsystem::TryGetActorIdForBPClassFName` 而不是 `PawnToActorId` 反向 map（PerceptionLogger 实际返回什么）；
(j) sit BP wrapper 为什么挂在 `AIC_NPC_SmartObject`、不挂在 NPC BP 父类（实测目录结构）；
(k) `addressed_to` 校验为什么不允许字面值 `"public"` 进数组（schema 怎么定义的、空数组语义）；
(l) TTS 完成 timer 为什么必须排在 AnimateFromAudioSamples 之**前**（既有 cpp:238 注释怎么说的）；
(m) `FAIL_EventMeta` / `FAIL_EventQueryResponse.events` 为什么不能用 TArray<FString> + TOptional（UE 反射什么限制、schema 实际形态是什么）；
(n) ENTER/EXIT 字段为什么从本卡裁掉（schema 0.1.1 现状 + 升 protocol 的代价）。
等我确认后再动手。
```

---

## 10. 可选前置准备

- **执行顺序提醒**：本卡第一步是 §3.0（BrainService `/health` bugfix + sync `Docs/protocol_pointer.md` + vendored examples），完成并提交后再进 §3.1+。如果跳过 §3.0 直接做 §3.2 USTRUCT 镜像，会基于错误的 schema 状态推进，所有 round-trip 测试都会卡在 health 反序列化。
- UE Editor 在线状态：执行 §3.5 之后的子节前确认 `mcp__monolith__monolith_status` 返回 online；如未启动用 PowerShell 启 UnrealEditor.exe 并等 30s 加载（见 CLAUDE.md "UE 编辑器进程管理"）。§3.0–§3.6 不需要 UE Editor 在线（只改 BrainService Python + UE 仓 `Docs/` + UE C++ 头/源文件）。
- BrainService server 启动测试：§3.0 修完 health.py 后先在 PowerShell 起 `python -m brain.server` 一次，确认 `curl http://127.0.0.1:8000/health` 返 `{"status":"ok","protocol_version":"0.1.1","server_time":"..."}`（应该不再含 `ok` 字段）
- vendored fixture 同步：§3.0 给的 PowerShell 校验脚本可保存到 `BrainService/scripts/sync-protocol-examples.ps1`（独立 commit），后续协议升版自动用
- 中文注释 BOM 工具：用 PowerShell `Out-File -Encoding utf8BOM` 或编辑器自动 BOM 选项（VSCode 默认 UTF-8 无 BOM，需手动改）；本卡所有新增 .h/.cpp 必须 BOM
- AI controller 资产路径确认：实施 §3.11.2 之前用 Monolith MCP `mcp__monolith__blueprint_query` 实际打开 `Content/Blueprints/AI/AIC_NPC_SmartObject.uasset`，确认它是 NPC 共用 controller 且未实现 `UseSmartObjectAndNotify` 函数（避免重名冲突）
