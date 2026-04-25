# T03 — GameMaster 层 C++ 类骨架

## 目标
创建 GameMaster 层全部 C++ 类的空骨架，包括基类和两个游戏的子类 stub（不实现阶段机），以及游戏专属 Action 的空骨架。

## 前置
T02（Mind 骨架，因为 GM 引用 `UMindAction` / `UMindComponent`）

## DoD
- [ ] 下列文件就位：
  - `Public/Mind/GameMaster/MindGameMaster.h` (`AActor` Abstract)
  - `Public/Mind/GameMaster/MindGameMaster_LiarsBar.h`
  - `Public/Mind/GameMaster/MindGameMaster_MinorityRule.h`
  - `Public/Mind/GameMaster/State/LiarsBarState.h` (USTRUCT)
  - `Public/Mind/GameMaster/State/MinorityRuleState.h` (USTRUCT)
  - `Public/Mind/Actions/` 目录下 7 个空 Action 子类（PlayCards / Challenge / PassTurn / AskQuestion / Vote / ProposeAlliance / AcceptAlliance）+ 8 个通用 Action 子类（Speak / MoveTo / LookAt / Wait / Observe / Think / Decision / Remember / Recall）
- [ ] 所有类编译通过
- [ ] 子类 `MindGameMaster_LiarsBar` / `_MinorityRule` 能在编辑器里被 Place Actor 摆到关卡

## 关键文件
全部新建（`Source/AILiveProject/Public/Mind/GameMaster/` + 相应 `.cpp`）

## 关键 API / 伪代码

```cpp
// MindGameMaster.h
UCLASS(Abstract)
class AILIVEPROJECT_API AMindGameMaster : public AActor {
    GENERATED_BODY()
public:
    UPROPERTY(EditAnywhere) TArray<TObjectPtr<AActor>> Participants;  // NPC actor 引用
    UPROPERTY(BlueprintReadOnly) FName CurrentPhase = NAME_None;

    UFUNCTION(BlueprintCallable) virtual void StartGame() {}
    UFUNCTION(BlueprintCallable) virtual void EndGame() {}
    virtual FMindAgentView BuildViewFor(AActor* Agent) { return {}; }
    virtual bool Validate(AActor* Agent, const FMindActionEnvelope& Env, FString& OutError) const { return false; }
    virtual void Apply(AActor* Agent, const FMindActionEnvelope& Env) {}
    virtual void OnAgentActionFinished(AActor* Agent, const FMindActionEnvelope& Env, bool bOk) {}

protected:
    void TransitionToPhase(FName NewPhase);
    void AwakeAgent(AActor* Agent, FString Reason);
    void RegisterActionsForAgent(AActor* Agent, const TArray<TSubclassOf<UMindAction>>& Actions);

    DECLARE_MULTICAST_DELEGATE_TwoParams(FOnPhaseChanged, FName /*Old*/, FName /*New*/);
    FOnPhaseChanged OnPhaseChanged;
};
```

```cpp
// LiarsBarState.h
USTRUCT(BlueprintType)
struct FLiarsBarPlayerState {
    GENERATED_BODY()
    UPROPERTY() FString AgentId;
    UPROPERTY() TArray<int32> Hand;            // 牌索引
    UPROPERTY() int32 RouletteChamber = 6;     // 剩余空格(1/6 = 致命概率)
    UPROPERTY() bool bAlive = true;
};

USTRUCT(BlueprintType)
struct FLiarsBarTableState {
    GENERATED_BODY()
    UPROPERTY() TArray<FLiarsBarPlayerState> Players;
    UPROPERTY() FString CurrentTurnAgentId;
    UPROPERTY() FString CurrentClaimRank;
    UPROPERTY() int32 CurrentClaimCount = 0;
    UPROPERTY() TArray<int32> DiscardPile;
    UPROPERTY() int32 RoundNumber = 0;
};
```

```cpp
// MindGameMaster_LiarsBar.h
UCLASS()
class AILIVEPROJECT_API AMindGameMaster_LiarsBar : public AMindGameMaster {
    GENERATED_BODY()
public:
    UPROPERTY(EditAnywhere) FLiarsBarTableState State;
    UPROPERTY(EditAnywhere) TObjectPtr<class UDataAsset> GameConfig;
    virtual void StartGame() override {}
    virtual FMindAgentView BuildViewFor(AActor* Agent) override { return {}; }
    virtual bool Validate(AActor* Agent, const FMindActionEnvelope& Env, FString& OutError) const override { return false; }
    virtual void Apply(AActor* Agent, const FMindActionEnvelope& Env) override {}
    virtual void OnAgentActionFinished(AActor* Agent, const FMindActionEnvelope& Env, bool bOk) override {}
};
```

```cpp
// 游戏专属 Action 骨架示例 - MindAction_PlayCards.h
UCLASS()
class UMindAction_PlayCards : public UMindAction {
    GENERATED_BODY()
public:
    UMindAction_PlayCards() {
        ActionName = TEXT("play_cards");
        Description = TEXT("Play 1-3 cards face-down with a public claim of rank+count");
        ParamSchemaJson = TEXT(R"({"claim_rank":"string","claim_count":"int","actual_indices":"int[]"})");
    }
    virtual void Execute(UMindComponent* Owner, const FString& ParamsJson, FOnActionDone Done) override {}
};
```

通用动作的骨架同样模式（`ActionName` / `Description` / `ParamSchemaJson` 在构造函数里设置）。

## 验收信号
- UBT 编译通过
- `Place Actors` 面板能搜索到 "Mind Game Master Liars Bar" 和 "Mind Game Master Minority Rule"，可拖入关卡
- `AMindGameMaster_LiarsBar.State` 可在 Details 面板展开编辑

## 不在范围
- 任何阶段机逻辑（T11/T12/T21）
- Action 实际执行（T13/T19/T22）

## 风险
- USTRUCT 嵌套数组在 BP 暴露的兼容性，必要时给 `BlueprintReadOnly`
- 文件数 20+，注意 .cpp 也要齐全（哪怕只有 GENERATED_BODY 实现），避免 link error
