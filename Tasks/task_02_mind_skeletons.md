# T02 — Mind 层 C++ 类骨架

## 目标
创建 Mind 决策层全部 C++ 类的**空骨架**（头文件齐全，cpp 仅含构造函数 / 默认值返回 / `PURE_VIRTUAL` 占位）。本卡保证三件事：编译通过 + 编辑器面板字段全部可绑 + 后置卡（T05 / T05.5 / T06 / T07 / T09 / T11 / T18 / T19）只改 .cpp 不改 .h。本卡不实现任何业务逻辑。

## 前置
T01（模块依赖）

## DoD

### 1. 文件清单（16 .h/.cpp 配对 + 4 .gitkeep = 36 个新文件）

**Mind 层核心**（位于 `Source/AILiveProject/Public/Mind/` 与 `Private/Mind/`）：

- `MindLog.h` + `.cpp` — `DECLARE_LOG_CATEGORY_EXTERN` × 3：`LogMind` / `LogMindAction` / `LogMindGM`
- `MindMessage.h` + `.cpp` — USTRUCT `FMindMessage` (role / content / ts / scene_id / channel / speaker_id / tags / est_tokens / Seq) + `EMindRole` / `EMindChannel` enums + `FToolSchema` struct
- `MindModelCapabilities.h` + `.cpp` — USTRUCT `FMindModelCapabilities` (ModelIdPattern / ContextWindow / MaxOutput / SupportsTools / SupportsJsonMode / CacheUsageField) + 静态 lookup table
- `MindActionEnvelope.h` + `.cpp` — USTRUCT，字段全 `BlueprintReadWrite`
- `MindMockResponseTable.h` + `.cpp` — `UPrimaryDataAsset` + `FMockResponseRow` USTRUCT (含 `MatchTrigger` 字段，T05.5 数据容器)
- `MindAgentConfig.h` + `.cpp` — `UPrimaryDataAsset`，含 `AgentIdStable`；**完整 `#include "Mind/MindMockResponseTable.h"`**
- `MindAction.h` + `.cpp` — `UCLASS(Abstract, Blueprintable)`，前向声明 `UMindComponent`
- `MindLLMProvider.h` + `.cpp` — `UCLASS(Abstract, EditInlineNew)`；接口含 messages[]+tool_call
- `MindLLMProvider_DeepSeek.h` + `.cpp` — non-Abstract 空骨架（让 ProviderClass 下拉可选）
- `MindContextManager.h` + `.cpp` — `UObject` per-agent buffer 管理骨架
- `MindSummarizer.h` + `.cpp` — `UObject` Compact / scene_end 摘要骨架
- `MindLLMBudget.h` + `.cpp` — `UGameInstanceSubsystem` LLM 并发限流两池骨架
- `MindMemoryClient.h` + `.cpp` — `UGameInstanceSubsystem`，含 `FMindMemoryItem` USTRUCT + `FMindEventReq` + `WriteEvent / RecallSync / WritePeerSummary / FetchPeerSummaries / WriteSceneLog` 签名
- `MindSpeechHelpers.h` + `.cpp` — `UBlueprintFunctionLibrary`，`ResolveSpeechActor(AActor*)`
- `MindComponent.h` + `.cpp` — `UActorComponent`，前向声明 `class AMindGameMaster;` + `TWeakObjectPtr` 弱指针避免 include 循环

**子目录 placeholder**（`.gitkeep` 占位）：

- `Public/Mind/Actions/.gitkeep`
- `Public/Mind/GameMaster/.gitkeep`
- `Public/Mind/GameMaster/Actions/.gitkeep`
- `Public/Mind/GameMaster/State/.gitkeep`

### 2. 反射 / 编辑器约定

- 所有 UCLASS / USTRUCT 加 `Category="AI Live|Mind"`（Memory 子系统用 `"AI Live|Memory"`）
- 所有 UFUNCTION 标 `BlueprintCallable` / `BlueprintReadOnly`，函数体为空（或返回默认值）
- `EMindState`（`UENUM BlueprintType`）：`Idle` / `Building` / `Calling` / `Acting` / `Cooldown`
- `EMindRole`：`System` / `User` / `Assistant` / `Tool`
- `EMindChannel`：`System` / `Public` / `Private` / `Perception`

### 3. `UMindComponent` 字段一次列全（避免后置卡反复改 .h）

- 配置：`Config (TObjectPtr<UMindAgentConfig>)`
- 状态：`State (EMindState)`、`bPerceptionCanTriggerDecision (bool)`、`bDecisionDisabled (bool)`（hard fail 时 set）、`CurrentSceneId (FString)`
- 引用：`GameMaster (TWeakObjectPtr<AMindGameMaster>)`、`Provider (TObjectPtr<UMindLLMProvider>)`、`Context (TObjectPtr<UMindContextManager>)`、`Summarizer (TObjectPtr<UMindSummarizer>)`、`MemoryClient (TWeakObjectPtr<UMindMemoryClient>)`
- Registry：`ActionRegistry (TMap<FString, TObjectPtr<UMindAction>>)`
- 决策状态：`LastEnvelope (FMindActionEnvelope)`、`LastDecisionAt (float)`
- **决策上限（P0）**：`ValidateRetryDepth (int32)`、`ValidateRetryMax (constexpr int32 = 5)` —— GM 驳回兜底重试上限（recall 上限由 Provider 内部 `max_tool_iterations=2` 控制，不在本类）
- **Perception 节流**：`LastPerceptionAt (TMap<TWeakObjectPtr<AActor>, float>)`——cpp 内部状态，**不带 UPROPERTY**（TWeakObjectPtr 不能作为 UPROPERTY map key）
- 通用 Action 状态：`CurrentIntent (FString)`（T19 Decision Action 写）

### 4. `UMindComponent` 函数签名一次列全

- `Initialize(UMindAgentConfig*, AActor* GM)` —— AgentIdStable 空 hard fail
- `RequestDecision(FString TriggerReason)`
- `SetGameActions(const TArray<TSubclassOf<UMindAction>>&)` / `RegisterAction(TSubclassOf<UMindAction>)`
- `DispatchAction(const FMindActionEnvelope&)` / `HandleActionDone(bool, const FString&)`
- `GetAgentId() const` / `GetGameMaster() const` / `GetContext() const`
- Perception API：`GetCurrentlyVisibleAgentIds()` / `GetCurrentlyAudibleAgentIds()` / `CanSenseActor(AActor*, FName)` / `SetPerceptionCanTriggerDecision(bool)`

### 5. `UMindMemoryClient` 头文件钉死接口（T09 时只填 cpp）

- USTRUCT `FMindMemoryItem`（`Id` / `Content` / `Score` / `Ts` / `Tags`，全 `BlueprintReadOnly`）
- USTRUCT `FMindEventReq`（`scene_id` / `round` / `phase` / `agent_id` / `action` / `params_digest` / `public_text` / `target_agent_id` / `result_summary` / `tags` / `ts`）
- USTRUCT `FMindPeerSummaryReq` / `FMindSceneLogReq`
- `WriteEvent(FMindEventReq)` —— fire-and-forget，不返回
- `RecallSync(AgentId, Query, TopK) -> TArray<FMindMemoryItem>` —— 5s timeout (Provider tool_call 用)
- `WritePeerSummary(AgentId, PeerId, SceneId, SummaryText)` / `FetchPeerSummaries(AgentId, TArray<PeerIds>) -> Map<PeerId, Summary>`
- `WriteSceneLog(SceneId, AgentId, TArray<FMindMessage>)`

### 6. `FMindActionEnvelope` 字段语义注释

- `ActionName` / `ParamsJson`：和 `UMindAction::ActionName` / `ParamSchemaJson` 对齐
- `Reasoning` / `InnerMonologue`：**verbatim 写入 Layer 2 hot 的 assistant role message**（NPC 下次决策时 LLM 能看到自己上次的内心独白，连贯性强）

### 7. 编译

- UBT 完整编译通过：`Build.bat AILiveProjectEditor Win64 Development -Project=...`

## 关键文件
全部新建（位于 `Source/AILiveProject/Public/Mind/` 和 `Source/AILiveProject/Private/Mind/`）

## 关键 API / 伪代码

```cpp
// MindLog.h
DECLARE_LOG_CATEGORY_EXTERN(LogMind, Log, All);
DECLARE_LOG_CATEGORY_EXTERN(LogMindAction, Log, All);
DECLARE_LOG_CATEGORY_EXTERN(LogMindGM, Log, All);
```

```cpp
// MindMessage.h
UENUM(BlueprintType)
enum class EMindRole : uint8 { System, User, Assistant, Tool };

UENUM(BlueprintType)
enum class EMindChannel : uint8 { System, Public, Private, Perception };

USTRUCT(BlueprintType)
struct FMindMessage {
    GENERATED_BODY()
    UPROPERTY(BlueprintReadWrite) EMindRole Role = EMindRole::User;
    UPROPERTY(BlueprintReadWrite, meta=(MultiLine=true)) FString Content;
    UPROPERTY(BlueprintReadWrite) double Ts = 0.0;
    UPROPERTY(BlueprintReadWrite) FString SceneId;
    UPROPERTY(BlueprintReadWrite) EMindChannel Channel = EMindChannel::System;
    UPROPERTY(BlueprintReadWrite) FString SpeakerId;
    UPROPERTY(BlueprintReadWrite) TMap<FString,FString> Tags;
    UPROPERTY(BlueprintReadWrite) int32 EstTokens = 0;
    UPROPERTY(BlueprintReadWrite) int32 Seq = 0;        // 单调递增, Compact compare-and-append 用
    UPROPERTY(BlueprintReadWrite) FString ToolCallId;   // role=Tool 用
};

USTRUCT(BlueprintType)
struct FToolSchema {
    GENERATED_BODY()
    UPROPERTY() FName Name;
    UPROPERTY(meta=(MultiLine=true)) FString Schema;    // OpenAI tool 格式 JSON
};
```

```cpp
// MindModelCapabilities.h
USTRUCT(BlueprintType)
struct FMindModelCapabilities {
    GENERATED_BODY()
    UPROPERTY(EditAnywhere) FString ModelIdPattern;     // 如 "deepseek-chat" 或正则
    UPROPERTY(EditAnywhere) int32 ContextWindow = 128000;
    UPROPERTY(EditAnywhere) int32 MaxOutput = 8192;
    UPROPERTY(EditAnywhere) bool bSupportsTools = true;
    UPROPERTY(EditAnywhere) bool bSupportsJsonMode = true;
    UPROPERTY(EditAnywhere) FString CacheUsageField = TEXT("prompt_cache_hit_tokens");
};

UCLASS()
class UMindModelCapabilitiesTable : public UObject {
    GENERATED_BODY()
public:
    static FMindModelCapabilities Lookup(const FString& Provider, const FString& ModelId);
private:
    static TArray<FMindModelCapabilities> BuiltinTable;  // DeepSeek / GLM / Claude 等
};
```

```cpp
// MindActionEnvelope.h
USTRUCT(BlueprintType)
struct FMindActionEnvelope {
    GENERATED_BODY()
    /** LLM 选定的动作名（与 UMindAction::ActionName 对齐，如 "speak" / "play_cards"）。 */
    UPROPERTY(BlueprintReadWrite) FString ActionName;
    /** 动作参数 JSON。 */
    UPROPERTY(BlueprintReadWrite, meta=(MultiLine=true)) FString ParamsJson;
    /** 公开理由：verbatim 进 Layer 2 assistant message。 */
    UPROPERTY(BlueprintReadWrite, meta=(MultiLine=true)) FString Reasoning;
    /** 内心独白：verbatim 进 Layer 2 assistant message (LLM 下次决策时回看自我连续性)。 */
    UPROPERTY(BlueprintReadWrite, meta=(MultiLine=true)) FString InnerMonologue;
};
```

```cpp
// MindMockResponseTable.h
USTRUCT(BlueprintType)
struct FMockResponseRow {
    GENERATED_BODY()
    UPROPERTY(EditAnywhere) FString MatchTrigger;     // 精确匹配 ContextManager.LastTriggerTag
    UPROPERTY(EditAnywhere) FString MatchPattern;     // substring of system + last user
    UPROPERTY(EditAnywhere, meta=(MultiLine=true)) FString ResponseJson;
    UPROPERTY(EditAnywhere) float Weight = 1.0f;
};

UCLASS(BlueprintType)
class UMindMockResponseTable : public UPrimaryDataAsset {
    GENERATED_BODY()
public:
    UPROPERTY(EditAnywhere) TArray<FMockResponseRow> Rows;
    UPROPERTY(EditAnywhere, meta=(MultiLine=true)) FString FallbackResponseJson;
};
```

```cpp
// MindAgentConfig.h
#include "Mind/MindMockResponseTable.h"

UCLASS(BlueprintType)
class UMindAgentConfig : public UPrimaryDataAsset {
    GENERATED_BODY()
public:
    UPROPERTY(EditAnywhere, Category="Identity")
    FString AgentIdStable;  // 用户手填稳定 ID（如 "npc_1"），跨关卡稳定；Initialize 时空 → hard fail

    UPROPERTY(EditAnywhere, Category="Identity") FText DisplayName;

    // PRD「AI 自我定义」字段（对应 messages[0] Layer 0a ①-⑤）：
    UPROPERTY(EditAnywhere, Category="Identity", meta=(MultiLine=true)) FText AppearanceTraits;
    UPROPERTY(EditAnywhere, Category="Identity", meta=(MultiLine=true)) FText IdentitySummary;
    UPROPERTY(EditAnywhere, Category="Identity", meta=(MultiLine=true)) FText ContinuityStakesText;
    UPROPERTY(EditAnywhere, Category="Identity", meta=(MultiLine=true)) FText Persona;
    UPROPERTY(EditAnywhere, Category="Identity", meta=(MultiLine=true)) FText Goals;

    UPROPERTY(EditAnywhere) TSubclassOf<class UMindLLMProvider> ProviderClass;
    UPROPERTY(EditAnywhere) TSubclassOf<class UMindLLMProvider> CheapSummarizerProviderClass;   // 留空走主 Provider
    UPROPERTY(EditAnywhere) int32 ContextWindowOverride = 0;   // 0 = 用 Provider 默认

    UPROPERTY(EditAnywhere) FString ModelId;
    UPROPERTY(EditAnywhere) FString ApiBaseEnvName;
    UPROPERTY(EditAnywhere) FString ApiKeyEnvName;

    UPROPERTY(EditAnywhere) FString MinimaxVoiceId;
    UPROPERTY(EditAnywhere) FName A2FProviderName = FName(TEXT("LocalA2F-James"));
    UPROPERTY(EditAnywhere) float DecisionCooldownSeconds = 2.0f;
    UPROPERTY(EditAnywhere) TObjectPtr<UMindMockResponseTable> MockResponseTable;
};
```

```cpp
// MindLLMProvider.h
UCLASS(Abstract, EditInlineNew)
class UMindLLMProvider : public UObject {
    GENERATED_BODY()
public:
    DECLARE_DELEGATE_TwoParams(FOnLLMResult, bool, FString);
    DECLARE_DELEGATE_RetVal_OneParam(FString, FOnToolCall, const FString& /*args_json*/);

    virtual void RequestCompletion(
        const TArray<FMindMessage>& Messages,
        const TArray<FToolSchema>& Tools,
        FOnLLMResult Done) PURE_VIRTUAL(UMindLLMProvider::RequestCompletion, );

    virtual void RegisterTool(FName Name, FString Schema, FOnToolCall Handler) {}
    virtual int32 GetMaxToolIterations() const { return 2; }
    virtual int32 GetContextWindowSize() const { return 128000; }
};
```

```cpp
// MindAction.h
UCLASS(Abstract, Blueprintable)
class UMindAction : public UObject {
    GENERATED_BODY()
public:
    DECLARE_DELEGATE_TwoParams(FOnActionDone, bool, FString);
    UPROPERTY(EditDefaultsOnly) FString ActionName;
    UPROPERTY(EditDefaultsOnly, meta=(MultiLine=true)) FString ParamSchemaJson;
    UPROPERTY(EditDefaultsOnly, meta=(MultiLine=true)) FString Description;
    virtual void Execute(class UMindComponent* Owner, const FString& ParamsJson, FOnActionDone Done) PURE_VIRTUAL(UMindAction::Execute, );
};
```

```cpp
// MindContextManager.h（per-agent buffer 骨架）
UCLASS()
class UMindContextManager : public UObject {
    GENERATED_BODY()
public:
    UPROPERTY() bool bIsCompacting = false;
    UPROPERTY() FString LastTriggerTag;        // Mock matcher 用
    int32 NextSeqValue = 0;

    void Init(const FString& AgentId, const FString& SceneId);
    void RebuildLayer0a(const class UMindAgentConfig* Config);
    void UpdateLayer0bFrame(const struct FMindMessage& Frame);
    void PushUser(EMindChannel Channel, const FString& Content);
    void PushAssistant(const FString& EnvelopeVerbatim, int32 Seq);
    TArray<FMindMessage> MaterializeForCall();
    void SetTrigger(const FString& Trigger) { LastTriggerTag = Trigger; }
    void ResetForNewScene(bool bKeepLayer0a);
    int32 NextSeq() { return ++NextSeqValue; }
    int32 EstimateTotalTokens() const;

    TOptional<TPair<FString, double>> PendingTrigger;
};
```

```cpp
// MindSummarizer.h（Compact + scene_end 摘要骨架）
UCLASS()
class UMindSummarizer : public UObject {
    GENERATED_BODY()
public:
    void Init(class UMindLLMProvider* MainProvider, TSubclassOf<class UMindLLMProvider> CheapClass);
    void CompactAsync(class UMindContextManager* Context, FSimpleDelegate OnDone);
    void GenerateAndPersistSceneSummaryAsync(const FString& AgentId, const struct FMindContextSnapshot& Snapshot);
private:
    UPROPERTY() TObjectPtr<UMindLLMProvider> CheapProvider;
    TWeakObjectPtr<UMindLLMProvider> MainProviderRef;
};
```

```cpp
// MindLLMBudget.h（两池 LLM 并发限流）
UENUM()
enum class EMindLLMBudgetKind : uint8 { Decision, RecallRetry, Compact, SceneSummary };

UCLASS()
class UMindLLMBudgetSubsystem : public UGameInstanceSubsystem {
    GENERATED_BODY()
public:
    UPROPERTY(EditAnywhere) int32 MaxConcurrentReasoner = 4;
    UPROPERTY(EditAnywhere) int32 MaxConcurrentSummarizer = 2;

    bool TryAcquire(EMindLLMBudgetKind Kind);
    void Release(EMindLLMBudgetKind Kind);
    void SubmitTask(EMindLLMBudgetKind Kind, TFunction<void()> Task);   // Compact/SceneSummary 排队
};
```

```cpp
// MindMemoryClient.h
USTRUCT(BlueprintType)
struct FMindMemoryItem {
    GENERATED_BODY()
    UPROPERTY(BlueprintReadOnly) FString Id;
    UPROPERTY(BlueprintReadOnly) FString Content;
    UPROPERTY(BlueprintReadOnly) float Score = 0;
    UPROPERTY(BlueprintReadOnly) int64 Ts = 0;
    UPROPERTY(BlueprintReadOnly) TMap<FString, FString> Tags;
};

USTRUCT(BlueprintType)
struct FMindEventReq {
    GENERATED_BODY()
    UPROPERTY() FString SceneId;
    UPROPERTY() int32 Round = 0;
    UPROPERTY() FString Phase;
    UPROPERTY() FString AgentId;
    UPROPERTY() FString Action;
    UPROPERTY() FString ParamsDigest;
    UPROPERTY() FString PublicText;
    UPROPERTY() FString TargetAgentId;
    UPROPERTY() FString ResultSummary;
    UPROPERTY() TMap<FString,FString> Tags;
    UPROPERTY() int64 Ts = 0;
};

UCLASS()
class UMindMemoryClient : public UGameInstanceSubsystem {
    GENERATED_BODY()
public:
    UPROPERTY(EditAnywhere, Config, Category="AI Live|Memory")
    FString BaseUrl = TEXT("http://127.0.0.1:8765");

    void WriteEvent(const FMindEventReq& Req);                                // fire-and-forget
    TArray<FMindMemoryItem> RecallSync(const FString& AgentId, const FString& Query, int32 TopK);   // 5s timeout (Provider tool 用)
    void WritePeerSummary(const FString& AgentId, const FString& PeerId, const FString& SceneId, const FString& SummaryText);
    void FetchPeerSummaries(const FString& AgentId, const TArray<FString>& PeerIds, TFunction<void(const TMap<FString,FString>&)> Done);
    void WriteSceneLog(const FString& SceneId, const FString& AgentId, const TArray<FMindMessage>& Messages);
};
```

```cpp
// MindSpeechHelpers.h（P0-B+ 强约束统一入口）
UCLASS()
class UMindSpeechHelpers : public UBlueprintFunctionLibrary {
    GENERATED_BODY()
public:
    UFUNCTION(BlueprintCallable, BlueprintPure, Category="AI Live|Mind")
    static AActor* ResolveSpeechActor(AActor* MindOwner);  // T02 仅返 owner，T06 真正解析 visual child
};
```

```cpp
// MindComponent.h（一次列全字段 + 函数签名）
class AMindGameMaster;  // 前向声明，避免 include 循环

UENUM(BlueprintType)
enum class EMindState : uint8 { Idle, Building, Calling, Acting, Cooldown };

UCLASS(ClassGroup=(Mind), meta=(BlueprintSpawnableComponent))
class UMindComponent : public UActorComponent {
    GENERATED_BODY()
public:
    UMindComponent();

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="AI Live|Mind") TObjectPtr<UMindAgentConfig> Config;
    UPROPERTY(BlueprintReadOnly, Category="AI Live|Mind") EMindState State = EMindState::Idle;
    UPROPERTY(BlueprintReadOnly, Category="AI Live|Mind") bool bPerceptionCanTriggerDecision = false;
    UPROPERTY(BlueprintReadOnly, Category="AI Live|Mind") bool bDecisionDisabled = false;   // hard fail 时设
    UPROPERTY(BlueprintReadOnly, Category="AI Live|Mind") FString CurrentSceneId;

    TWeakObjectPtr<AMindGameMaster> GameMaster;
    UPROPERTY() TObjectPtr<UMindLLMProvider> Provider;
    UPROPERTY() TObjectPtr<UMindContextManager> Context;
    UPROPERTY() TObjectPtr<UMindSummarizer> Summarizer;
    TWeakObjectPtr<UMindMemoryClient> MemoryClient;
    UPROPERTY() TMap<FString, TObjectPtr<UMindAction>> ActionRegistry;
    UPROPERTY() FMindActionEnvelope LastEnvelope;
    UPROPERTY() float LastDecisionAt = 0.f;

    UPROPERTY() int32 ValidateRetryDepth = 0;
    static constexpr int32 ValidateRetryMax = 5;

    // cpp 内部状态——不能 UPROPERTY（TWeakObjectPtr 不被反射支持作为 map key）
    TMap<TWeakObjectPtr<AActor>, float> LastPerceptionAt;

    UPROPERTY(BlueprintReadOnly, Category="AI Live|Mind") FString CurrentIntent;

    UFUNCTION(BlueprintCallable, Category="AI Live|Mind") void Initialize(UMindAgentConfig* InConfig, AActor* GM);
    UFUNCTION(BlueprintCallable, Category="AI Live|Mind") void RequestDecision(FString TriggerReason);
    UFUNCTION(BlueprintCallable, Category="AI Live|Mind") void SetGameActions(const TArray<TSubclassOf<UMindAction>>& Actions);
    void RegisterAction(TSubclassOf<UMindAction> ActionClass);
    UFUNCTION(BlueprintCallable, Category="AI Live|Mind") void DispatchAction(const FMindActionEnvelope& Env);
    void HandleActionDone(bool bOk, const FString& Summary);

    UFUNCTION(BlueprintCallable, BlueprintPure, Category="AI Live|Mind") FString GetAgentId() const;
    UFUNCTION(BlueprintCallable, BlueprintPure, Category="AI Live|Mind") AMindGameMaster* GetGameMaster() const;
    UFUNCTION(BlueprintCallable, BlueprintPure, Category="AI Live|Mind") UMindContextManager* GetContext() const { return Context; }

    UFUNCTION(BlueprintCallable, BlueprintPure, Category="AI Live|Mind") TArray<FString> GetCurrentlyVisibleAgentIds() const;
    UFUNCTION(BlueprintCallable, BlueprintPure, Category="AI Live|Mind") TArray<FString> GetCurrentlyAudibleAgentIds() const;
    UFUNCTION(BlueprintCallable, BlueprintPure, Category="AI Live|Mind") bool CanSenseActor(AActor* Other, FName SenseTag) const;
    UFUNCTION(BlueprintCallable, Category="AI Live|Mind") void SetPerceptionCanTriggerDecision(bool bEnabled);
};
```

## 验收信号

- UBT 编译通过
- 编辑器创建 `UMindAgentConfig` DataAsset 实例无错误，所有字段可编辑
- `ProviderClass` 下拉能看到 `UMindLLMProvider_DeepSeek`（验证 EditInlineNew + Abstract 子类显示，提前于 T05 暴露）
- `MockResponseTable` 字段下拉能选 `UMindMockResponseTable` DataAsset（不为灰态/未识别类型）
- 在 BP 中能 Add `MindComponent` 组件（即使没有逻辑）
- Output Log 启动时无 `LogMind` Error（说明 LogCategory 注册成功）
- `git status` 看到 `Public/Mind/{Actions,GameMaster,GameMaster/Actions,GameMaster/State}/.gitkeep` 已新增

## 路径锁定（必须在 T02 钉死，后续卡不得修改）

```
Public/Mind/                        # Mind 决策层
Public/Mind/Actions/                # 通用动作（Speak/MoveTo/Wait/...）
Public/Mind/GameMaster/             # GM 基类
Public/Mind/GameMaster/Actions/     # 游戏专属动作（PlayCards/Vote/...）
Public/Mind/GameMaster/State/       # FLiarsBarTableState 等 USTRUCT
Public/Mind/MindLog.h               # 日志类目
Public/Mind/MindMessage.h           # FMindMessage / EMindRole / EMindChannel / FToolSchema
Public/Mind/MindModelCapabilities.h # FMindModelCapabilities + lookup table
Public/Mind/MindContextManager.h    # per-agent buffer 管理
Public/Mind/MindSummarizer.h        # Compact + scene_end 摘要
Public/Mind/MindLLMBudget.h         # LLM 并发限流两池
Public/Mind/MindSpeechHelpers.h     # P0-B+ visual child 工具入口
Public/Mind/MindMockResponseTable.h # T05.5 数据容器（T02 落骨架）
Public/Mind/MindLLMProvider_Mock.h  # T05.5 创建（不在 T02）
Private/Mind/...                    # 镜像 cpp
```

T03 / T05 / T05.5 / T06 / T07 / T09 / T11 / T13 / T13.5 / T18 / T19 / T19.5 / T19.7 / T22 全部按此目录组织，不引入新顶级路径。

## 不在范围

- 任何函数体的实际实现（cpp 仅 GENERATED_BODY / 默认值返回 / PURE_VIRTUAL 占位）
- DeepSeek HTTP 调用（T05）
- Mock 匹配逻辑（T05.5）
- ContextManager / Summarizer / LLMBudget 的实现（T09）
- Memory HTTP（T08 / T09）
- ActionRegistry 实例化时机（T06）
- GM cast / lifetime（T11）
- Perception OnPerceptionUpdated 的 `FAIStimulus` 委托绑定（T18 在 .h 加 `#include "Perception/AIPerceptionTypes.h"` 后再补）

## 实施策略

按依赖顺序，每加 1-2 个 .h+.cpp 配对就增量编译一次（Live Coding 秒级）：

1. `MindLog.h` + `.cpp` → build
2. `MindMessage.h` + `.cpp`（USTRUCT + enums）→ build
3. `MindModelCapabilities.h` + `.cpp`（USTRUCT + lookup table 骨架）→ build
4. `MindActionEnvelope.h` + `.cpp`（USTRUCT）→ build
5. `MindMockResponseTable.h` + `.cpp`（含 MatchTrigger）→ build
6. `MindAgentConfig.h` + `.cpp`（含 AgentIdStable + #include MindMockResponseTable.h + Identity 5 字段 + ContextWindowOverride / CheapSummarizerProviderClass）→ build
7. `MindAction.h` + `.cpp`（基类）→ build
8. `MindLLMProvider.h` + `.cpp`（基类含 messages[]+tool_call 接口）→ build
9. `MindLLMProvider_DeepSeek.h` + `.cpp`（空骨架）→ build
10. `MindContextManager.h` + `.cpp`（骨架）→ build
11. `MindSummarizer.h` + `.cpp`（骨架）→ build
12. `MindLLMBudget.h` + `.cpp`（Subsystem 骨架）→ build
13. `MindMemoryClient.h` + `.cpp`（含 FMindMemoryItem + FMindEventReq + 5 个方法签名）→ build
14. `MindSpeechHelpers.h` + `.cpp`（cpp 仅返 owner）→ build
15. `MindComponent.h` + `.cpp`（最复杂，前向声明 + 弱指针 + 持有 Context/Summarizer/MemoryClient）→ build
16. 创建 4 个子目录的 `.gitkeep`
17. 编辑器烟测 7 项验收信号

## 风险

- `EditInlineNew + Abstract` 子类不显示：T02 验收信号已显式验证 ProviderClass 下拉
- Header include 循环：用前向声明 `class AMindGameMaster;` + `TWeakObjectPtr` 弱指针消除
- `TMap<TWeakObjectPtr<AActor>, float>` 不被 UPROPERTY 反射支持：`LastPerceptionAt` 必须不带 UPROPERTY
- `UMindMemoryClient` 是 `UGameInstanceSubsystem`，PIE 之外（commandlet）取不到：T13.5 用 `WITH_EDITOR` 包裹绕开
- `UMindMockResponseTable` 字段在 `UMindAgentConfig` 必须 `#include` 完整类型而非前向声明
- `UMindContextManager` 不是 Subsystem——per-agent 实例由 `MindComponent` 持有，避免共享状态污染
