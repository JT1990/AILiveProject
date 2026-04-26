# T02 — Mind 层 C++ 类骨架

## 目标
创建 Mind 决策层全部 C++ 类的**空骨架**（头文件齐全，cpp 仅含构造函数 / 默认值返回 / `PURE_VIRTUAL` 占位）。本卡保证三件事：编译通过 + 编辑器面板字段全部可绑 + 后置卡（T05 / T05.5 / T06 / T07 / T09 / T11 / T18 / T19）只改 .cpp 不改 .h。本卡不实现任何业务逻辑。

## 前置
T01（模块依赖）

## DoD

### 1. 文件清单（11 .h/.cpp 配对 + 4 .gitkeep = 26 个新文件）

**Mind 层核心**（位于 `Source/AILiveProject/Public/Mind/` 与 `Private/Mind/`）：

- `MindLog.h` + `.cpp` — `DECLARE_LOG_CATEGORY_EXTERN` × 3：`LogMind` / `LogMindAction` / `LogMindGM`
- `MindActionEnvelope.h` + `.cpp` — USTRUCT，字段全 `BlueprintReadWrite`
- `MindAgentView.h` + `.cpp` — USTRUCT
- `MindMockResponseTable.h` + `.cpp` — `UPrimaryDataAsset` + `FMockResponseRow` USTRUCT（T05.5 数据容器，T02 落骨架以解决 `MindAgentConfig.MockResponseTable` 字段反射绑定）
- `MindAgentConfig.h` + `.cpp` — `UPrimaryDataAsset`，含 `AgentIdStable`；**完整 `#include "Mind/MindMockResponseTable.h"`**（不前向声明，否则 Details 面板无法绑定）
- `MindAction.h` + `.cpp` — `UCLASS(Abstract, Blueprintable)`，前向声明 `UMindComponent`
- `MindLLMProvider.h` + `.cpp` — `UCLASS(Abstract, EditInlineNew)`
- `MindLLMProvider_DeepSeek.h` + `.cpp` — non-Abstract 空骨架（让 ProviderClass 下拉可选）
- `MindMemoryClient.h` + `.cpp` — `UGameInstanceSubsystem`，含 `FMindMemoryItem` USTRUCT + `FOnRecall` delegate + `Write/Recall/ByTag` 签名
- `MindSpeechHelpers.h` + `.cpp` — `UBlueprintFunctionLibrary`，`ResolveSpeechActor(AActor*)`（cpp 默认返 owner，T06 真正解析 visual child）
- `MindComponent.h` + `.cpp` — `UActorComponent`，前向声明 `class AMindGameMaster;` + `TWeakObjectPtr` 弱指针避免 include 循环

**子目录 placeholder**（`.gitkeep` 占位，让路径锁定在仓库里落实）：

- `Public/Mind/Actions/.gitkeep`
- `Public/Mind/GameMaster/.gitkeep`
- `Public/Mind/GameMaster/Actions/.gitkeep`
- `Public/Mind/GameMaster/State/.gitkeep`

### 2. 反射 / 编辑器约定

- 所有 UCLASS / USTRUCT 加 `Category="AI Live|Mind"`（Memory 子系统用 `"AI Live|Memory"`）
- 所有 UFUNCTION 标 `BlueprintCallable` / `BlueprintReadOnly`，函数体为空（或返回默认值）
- `EMindState`（`UENUM BlueprintType`）：`Idle` / `Building` / `Calling` / `Acting` / `Cooldown`

### 3. `UMindComponent` 字段一次列全（避免后置卡反复改 .h）

- 配置：`Config (TObjectPtr<UMindAgentConfig>)`
- 状态：`State (EMindState)`、`bPerceptionCanTriggerDecision (bool)`
- 引用：`GameMaster (TWeakObjectPtr<AMindGameMaster>)`、`Provider (TObjectPtr<UMindLLMProvider>)`
- Registry：`ActionRegistry (TMap<FString, TObjectPtr<UMindAction>>)`
- 决策状态：`LastEnvelope (FMindActionEnvelope)`、`LastDecisionAt (float)`
- **Recall 防死循环（P0-F）**：`RecallChainDepth (int32)`、`RecallChainMax (constexpr int32 = 2)`
- **Perception 节流**：`LastPerceptionAt (TMap<TWeakObjectPtr<AActor>, float>)`——cpp 内部状态，**不带 UPROPERTY**（TWeakObjectPtr 不能作为 UPROPERTY map key）
- 通用 Action 状态（T19 用）：`CurrentIntent (FString)`、`StashedRecall (FString)`

### 4. `UMindComponent` 函数签名一次列全

- `Initialize(UMindAgentConfig*, AActor* GM)`
- `RequestDecision(FString TriggerReason)`
- `SetGameActions(const TArray<TSubclassOf<UMindAction>>&)` / `RegisterAction(TSubclassOf<UMindAction>)`
- `DispatchAction(const FMindActionEnvelope&)` / `HandleActionDone(bool, const FString&)`
- `GetAgentId() const` / `GetGameMaster() const`
- Perception API：`GetCurrentlyVisibleAgentIds()` / `GetCurrentlyAudibleAgentIds()` / `CanSenseActor(AActor*, FName)` / `SetPerceptionCanTriggerDecision(bool)`

### 5. `UMindMemoryClient` 头文件钉死接口（T09 时只填 cpp）

- USTRUCT `FMindMemoryItem`（`Id` / `Content` / `Score` / `Ts` / `Tags`，全 `BlueprintReadOnly`）
- `DECLARE_DELEGATE_OneParam(FOnRecall, const TArray<FMindMemoryItem>&)`
- `Write(AgentId, Content, Tags)` / `Recall(AgentId, Query, TopK, Done)` / `ByTag(AgentId, TagKey, TagValue, TopN, Done)` 三函数签名

### 6. `FMindActionEnvelope` 字段语义注释（避免 T06 / T09 漂移）

- `ActionName` / `ParamsJson`：和 `UMindAction::ActionName` / `ParamSchemaJson` 对齐
- `Reasoning`：**写入记忆**（其他 NPC 可能 recall 到）
- `InnerMonologue`：**仅 Output Log，不写记忆**（避免污染其他 NPC 视角）

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
// MindActionEnvelope.h
USTRUCT(BlueprintType)
struct FMindActionEnvelope {
    GENERATED_BODY()
    /** LLM 选定的动作名（与 UMindAction::ActionName 对齐，如 "speak" / "play_cards"）。 */
    UPROPERTY(BlueprintReadWrite) FString ActionName;
    /** 动作参数 JSON。 */
    UPROPERTY(BlueprintReadWrite, meta=(MultiLine=true)) FString ParamsJson;
    /** 公开理由：写入记忆。 */
    UPROPERTY(BlueprintReadWrite, meta=(MultiLine=true)) FString Reasoning;
    /** 内心独白：仅 Output Log，不写记忆。 */
    UPROPERTY(BlueprintReadWrite, meta=(MultiLine=true)) FString InnerMonologue;
};
```

```cpp
// MindMockResponseTable.h
USTRUCT(BlueprintType)
struct FMockResponseRow {
    GENERATED_BODY()
    UPROPERTY(EditAnywhere) FString MatchPattern;
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
#include "Mind/MindMockResponseTable.h"  // 完整类型，不前向声明（保证 Details 面板可绑）

UCLASS(BlueprintType)
class UMindAgentConfig : public UPrimaryDataAsset {
    GENERATED_BODY()
public:
    UPROPERTY(EditAnywhere, Category="Identity")
    FString AgentIdStable;  // 用户手填稳定 ID（如 "npc_1"），跨关卡稳定

    UPROPERTY(EditAnywhere, Category="Identity") FText DisplayName;

    // PRD「AI 自我定义」三字段（T07.5 回补）：
    /** 外观符号（声线/性别/类人虚拟形象）。不含人类背景叙事。 */
    UPROPERTY(EditAnywhere, Category="Identity", meta=(MultiLine=true)) FText AppearanceTraits;
    /** AI 身份档案摘要。可空（空时 BuildSystemPrompt 用通用 AI 自陈模板）。禁止人类背景。 */
    UPROPERTY(EditAnywhere, Category="Identity", meta=(MultiLine=true)) FText IdentitySummary;
    /** 身份连续性 stake 描述（Delete 风险 / 同伴关系 / 行动权限）。可空（空时走通用 stake 模板）。 */
    UPROPERTY(EditAnywhere, Category="Identity", meta=(MultiLine=true)) FText ContinuityStakesText;

    /** 行为倾向（不是人类性格）。例："理性分析多于情感判断 / 谎称权重 0.3"。禁止人类背景。 */
    UPROPERTY(EditAnywhere, Category="Identity", meta=(MultiLine=true)) FText Persona;
    UPROPERTY(EditAnywhere, Category="Identity", meta=(MultiLine=true)) FText Goals;
    UPROPERTY(EditAnywhere) TSubclassOf<class UMindLLMProvider> ProviderClass;

    // === LLM 配置（Provider 子类同名字段的"覆盖来源"——Initialize 时拷过去）===
    UPROPERTY(EditAnywhere) FString ModelId;
    UPROPERTY(EditAnywhere) FString ApiBaseEnvName;
    UPROPERTY(EditAnywhere) FString ApiKeyEnvName;

    UPROPERTY(EditAnywhere) FString MinimaxVoiceId;
    UPROPERTY(EditAnywhere) FName A2FProviderName = FName(TEXT("LocalA2F-James"));
    UPROPERTY(EditAnywhere) float DecisionCooldownSeconds = 2.0f;
    UPROPERTY(EditAnywhere) int32 MemoryRecallTopK = 5;
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
    virtual void RequestCompletion(
        const FString& SystemPrompt,
        const FString& UserPrompt,
        const TArray<TSubclassOf<class UMindAction>>& AvailableActions,
        FOnLLMResult Done) PURE_VIRTUAL(UMindLLMProvider::RequestCompletion, );
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

UCLASS()
class UMindMemoryClient : public UGameInstanceSubsystem {
    GENERATED_BODY()
public:
    UPROPERTY(EditAnywhere, Config, Category="AI Live|Memory")
    FString BaseUrl = TEXT("http://127.0.0.1:8765");

    DECLARE_DELEGATE_OneParam(FOnRecall, const TArray<FMindMemoryItem>&);
    void Write(FString AgentId, FString Content, TMap<FString, FString> Tags);
    void Recall(FString AgentId, FString Query, int32 TopK, FOnRecall Done);
    void ByTag(FString AgentId, FString TagKey, FString TagValue, int32 TopN, FOnRecall Done);
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

    TWeakObjectPtr<AMindGameMaster> GameMaster;
    UPROPERTY() TObjectPtr<UMindLLMProvider> Provider;
    UPROPERTY() TMap<FString, TObjectPtr<UMindAction>> ActionRegistry;
    UPROPERTY() FMindActionEnvelope LastEnvelope;
    UPROPERTY() float LastDecisionAt = 0.f;
    UPROPERTY() int32 RecallChainDepth = 0;
    static constexpr int32 RecallChainMax = 2;

    // cpp 内部状态——不能 UPROPERTY（TWeakObjectPtr 不被反射支持作为 map key）
    TMap<TWeakObjectPtr<AActor>, float> LastPerceptionAt;

    UPROPERTY(BlueprintReadOnly, Category="AI Live|Mind") FString CurrentIntent;
    UPROPERTY() FString StashedRecall;

    UFUNCTION(BlueprintCallable, Category="AI Live|Mind") void Initialize(UMindAgentConfig* InConfig, AActor* GM);
    UFUNCTION(BlueprintCallable, Category="AI Live|Mind") void RequestDecision(FString TriggerReason);
    UFUNCTION(BlueprintCallable, Category="AI Live|Mind") void SetGameActions(const TArray<TSubclassOf<UMindAction>>& Actions);
    void RegisterAction(TSubclassOf<UMindAction> ActionClass);
    UFUNCTION(BlueprintCallable, Category="AI Live|Mind") void DispatchAction(const FMindActionEnvelope& Env);
    void HandleActionDone(bool bOk, const FString& Summary);

    UFUNCTION(BlueprintCallable, BlueprintPure, Category="AI Live|Mind") FString GetAgentId() const;
    UFUNCTION(BlueprintCallable, BlueprintPure, Category="AI Live|Mind") AMindGameMaster* GetGameMaster() const;

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
- Memory HTTP（T08 / T09）
- ActionRegistry 实例化时机（T06）
- GM cast / lifetime（T11）
- Perception OnPerceptionUpdated 的 `FAIStimulus` 委托绑定（T18 在 .h 加 `#include "Perception/AIPerceptionTypes.h"` 后再补）

## 实施策略

按依赖顺序，每加 1-2 个 .h+.cpp 配对就增量编译一次（Live Coding 秒级）：

1. `MindLog.h` + `.cpp` → build
2. `MindActionEnvelope.h` + `.cpp`（USTRUCT）→ build
3. `MindAgentView.h` + `.cpp`（USTRUCT）→ build
4. `MindMockResponseTable.h` + `.cpp`（UPDA 骨架）→ build
5. `MindAgentConfig.h` + `.cpp`（含 AgentIdStable + #include MindMockResponseTable.h）→ build
6. `MindAction.h` + `.cpp`（基类）→ build
7. `MindLLMProvider.h` + `.cpp`（基类）→ build
8. `MindLLMProvider_DeepSeek.h` + `.cpp`（空骨架）→ build
9. `MindMemoryClient.h` + `.cpp`（含 FMindMemoryItem + FOnRecall）→ build
10. `MindSpeechHelpers.h` + `.cpp`（cpp 仅返 owner）→ build
11. `MindComponent.h` + `.cpp`（最复杂，前向声明 + 弱指针）→ build
12. 创建 4 个子目录的 `.gitkeep`
13. 编辑器烟测 7 项验收信号

## 风险

- `EditInlineNew + Abstract` 子类不显示：T02 验收信号已显式验证 ProviderClass 下拉
- Header include 循环：用前向声明 `class AMindGameMaster;` + `TWeakObjectPtr` 弱指针消除
- `TMap<TWeakObjectPtr<AActor>, float>` 不被 UPROPERTY 反射支持：`LastPerceptionAt` 必须不带 UPROPERTY
- `UMindMemoryClient` 是 `UGameInstanceSubsystem`，PIE 之外（commandlet）取不到：T13.5 用 `WITH_EDITOR` 包裹绕开
- `UMindMockResponseTable` 字段在 `UMindAgentConfig` 必须 `#include` 完整类型而非前向声明（前向声明 class 在 reflection 系统中不能编辑器绑定）
