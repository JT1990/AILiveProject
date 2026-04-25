# T02 — Mind 层 C++ 类骨架

## 目标
创建 Mind 决策层全部 C++ 类的**空骨架**（头文件齐全，cpp 仅含构造函数和 PURE_VIRTUAL 占位）。本卡只保证编译通过，不实现任何业务逻辑。

## 前置
T01（模块依赖）

## DoD
- [ ] 下列 14 个新文件就位（头 + cpp 配对）：
  - `Public/Mind/MindAgentConfig.h` (`UPrimaryDataAsset`)
  - `Public/Mind/MindAgentView.h` (USTRUCT)
  - `Public/Mind/MindActionEnvelope.h` (USTRUCT)
  - `Public/Mind/MindLLMProvider.h` (UCLASS Abstract)
  - `Public/Mind/MindLLMProvider_DeepSeek.h` (UCLASS)
  - `Public/Mind/MindMemoryClient.h` (UGameInstanceSubsystem)
  - `Public/Mind/MindComponent.h` (UActorComponent)
  - `Public/Mind/MindAction.h` (UCLASS Abstract)
- [ ] 所有类都有 `Category="AI Live|Mind"` 的属性元数据
- [ ] 所有 UFUNCTION 标 BlueprintCallable / BlueprintReadOnly 但函数体为空（或 return 默认值）
- [ ] `EMindState` 枚举：Idle / Building / Calling / Acting / Cooldown
- [ ] UBT 完整编译通过

## 关键文件
全部新建（位于 `Source/AILiveProject/Public/Mind/` 和 `Source/AILiveProject/Private/Mind/`）

## 关键 API / 伪代码

```cpp
// MindAgentConfig.h
UCLASS(BlueprintType)
class AILIVEPROJECT_API UMindAgentConfig : public UPrimaryDataAsset {
    GENERATED_BODY()
public:
    // === 稳定 agent 标识符（跨关卡持久） ===
    UPROPERTY(EditAnywhere, Category="Identity")
    FString AgentIdStable;  // 用户手填，如 "npc_1"，不能为空，跨关卡稳定

    UPROPERTY(EditAnywhere) FText DisplayName;
    UPROPERTY(EditAnywhere, meta=(MultiLine=true)) FText Persona;
    UPROPERTY(EditAnywhere, meta=(MultiLine=true)) FText Goals;
    UPROPERTY(EditAnywhere) TSubclassOf<class UMindLLMProvider> ProviderClass;
    UPROPERTY(EditAnywhere) FString ModelId;
    UPROPERTY(EditAnywhere) FString ApiBaseEnvName;      // 例 "DEEPSEEK_API_BASE"
    UPROPERTY(EditAnywhere) FString ApiKeyEnvName;       // 例 "DEEPSEEK_API_KEY"
    UPROPERTY(EditAnywhere) FString MinimaxVoiceId;
    UPROPERTY(EditAnywhere) FName A2FProviderName = FName(TEXT("LocalA2F-James"));  // FName，与 TriggerMinimaxSpeech 签名一致
    UPROPERTY(EditAnywhere) float DecisionCooldownSeconds = 2.0f;
    UPROPERTY(EditAnywhere) int32 MemoryRecallTopK = 5;
    UPROPERTY(EditAnywhere) TObjectPtr<class UMindMockResponseTable> MockResponseTable;  // 仅 Mock provider 用
};
```

```cpp
// MindActionEnvelope.h
USTRUCT(BlueprintType)
struct FMindActionEnvelope {
    GENERATED_BODY()
    UPROPERTY() FString ActionName;
    UPROPERTY() FString ParamsJson;
    UPROPERTY() FString Reasoning;
    UPROPERTY() FString InnerMonologue;
};
```

```cpp
// MindAgentView.h
USTRUCT(BlueprintType)
struct FMindAgentView {
    GENERATED_BODY()
    UPROPERTY() FString OwnAgentId;
    UPROPERTY() FString GameStage;
    UPROPERTY() FString PublicStateText;     // GM 序列化的公开信息
    UPROPERTY() FString PrivateStateText;    // GM 序列化的本 agent 私有信息
    UPROPERTY() TArray<FString> RecentEvents;
    FString ToPromptText() const { return {}; }  // T06 实现
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
// MindComponent.h
UCLASS(ClassGroup=(Mind), meta=(BlueprintSpawnableComponent))
class UMindComponent : public UActorComponent {
    GENERATED_BODY()
public:
    UPROPERTY(EditAnywhere, BlueprintReadOnly) TObjectPtr<UMindAgentConfig> Config;
    UPROPERTY(BlueprintReadOnly) EMindState State = EMindState::Idle;
    UFUNCTION(BlueprintCallable) void Initialize(UMindAgentConfig* InConfig, AActor* GM) {}
    UFUNCTION(BlueprintCallable) void RequestDecision(FString TriggerReason) {}
    UFUNCTION(BlueprintCallable) void SetGameActions(const TArray<TSubclassOf<UMindAction>>& Actions) {}
};
```

## 验收信号
- UBT 编译通过
- 在编辑器创建 `UMindAgentConfig` DataAsset 实例无错误，能编辑所有字段
- 在 BP 中能 Add `MindComponent` 组件（即使没有逻辑）

## 不在范围
- 任何函数体的实际实现
- DeepSeek HTTP 调用（T05）
- ActionRegistry 注册机制（T06）

## 路径命名锁定（必须在 T02 钉死，后续卡不得修改）

```
Public/Mind/                        # Mind 决策层
Public/Mind/Actions/                # 通用动作（Speak/MoveTo/Wait/...）
Public/Mind/GameMaster/             # GM 基类
Public/Mind/GameMaster/Actions/     # 游戏专属动作（PlayCards/Vote/...）
Public/Mind/GameMaster/State/       # FLiarsBarTableState 等 USTRUCT
Private/Mind/...                    # 镜像 cpp
```

后续 T03 / T13 / T19 / T22 等卡都按此目录组织 .h/.cpp，不得引入新顶级命名空间。

## 实施策略
14 个文件一次写完容易 link error 难定位。**按 .h+.cpp 配对一个一个加，每加一个就 increment 编译**。建议顺序：
1. `MindActionEnvelope.h`（USTRUCT，无依赖）→ build
2. `MindAgentView.h`（USTRUCT）→ build
3. `MindAgentConfig.h/.cpp`（含 AgentIdStable 字段）→ build
4. `MindAction.h/.cpp`（基类，前向声明 MindComponent）→ build
5. `MindLLMProvider.h/.cpp`（基类）→ build
6. `MindLLMProvider_DeepSeek.h/.cpp`（空骨架）→ build
7. `MindMemoryClient.h/.cpp`（GameInstanceSubsystem）→ build
8. `MindComponent.h/.cpp`（最复杂，依赖前面所有）→ build

每步出错先解决再继续。

## 风险
- `EditInlineNew` + Abstract 的组合在 UE 5.7 有时编辑器面板不显示子类，T05 实现 DeepSeek 时验证
- Header include 循环：MindComponent.h 不要 include MindAction.h，用前向声明
