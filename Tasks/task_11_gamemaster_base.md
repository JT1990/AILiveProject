# T11 — `AMindGameMaster` 抽象基类 + Phase 调度

## 目标
实现 GameMaster 基类的可复用部分：阶段切换 / agent 唤醒 / 动作注入 / **Validate（只检查）+ Apply（OnActionDone 后才改状态）双阶段**。子类（LiarsBar / MinorityRule）只需要实现 `BuildViewFor` / `Validate` / `Apply` / `Phase_*` 函数。

## 前置
T03（GameMaster 骨架）+ T09（MindComponent 已能 RequestDecision）

## 核心流程

```
Dispatch(env)
  ├─ GM.Validate(agent, env, err)           # 只读检查，不改状态
  │  └─ err 非空 → OnActionDone(false, err)
  ├─ Action.Execute(owner, params, OnDone)  # fire-and-forget TTS/动画
  └─ OnActionDone(bOk, summary)
       ├─ if bOk: GM.Apply(agent, env)      # 现在才改状态
       ├─ Memory.Write
       ├─ State = Idle
       └─ GM.OnAgentActionFinished(agent, env, bOk)  # GM 决定是否切阶段
```

GM 不能在 Action 异步执行前先改状态——TTS 失败 / PIE 中断 / 跨阶段都会导致状态错乱。

## DoD
- [ ] **`Validate` 只读纯函数**：`virtual bool Validate(AActor* Agent, const FMindActionEnvelope& Env, FString& OutError) const`，不修改任何 state
- [ ] **`Apply` 修改函数**：`virtual void Apply(AActor* Agent, const FMindActionEnvelope& Env)`，由 MindComponent 在 OnActionDone 成功后调
- [ ] `OnAgentActionFinished(AActor* Agent, const FMindActionEnvelope& Env, bool bExecuteOk)` 虚函数：子类决定该阶段是否结束 / 切下个阶段
- [ ] `TransitionToPhase`：日志 + 广播 OnPhaseChanged + 调子类 hook `OnEnterPhase`
- [ ] `AwakeAgent(Agent, Reason)`：找 MindComponent，调 RequestDecision
- [ ] `RegisterActionsForAgent`：找 MindComponent，调 SetGameActions
- [ ] `UMindComponent::DispatchAction` 改造按新流程：

```
DispatchAction(Env):
  if !GM.Validate(owner, Env, err):
    log warning + OnActionDone(false, err) + return
  Action.Execute(owner, params, [this](bOk, summary) {
    if bOk and GM:
        GM.Apply(owner, LastEnvelope)
    OnActionDone(bOk, summary)
    if GM:
        GM.OnAgentActionFinished(owner, LastEnvelope, bOk)
  })
```

- [ ] `StartGame` 模板方法：抓 Participants 上的 MindComponent → Initialize（注入自身 GM 引用）→ 调 OnGameStart 子类 hook

## 关键文件
- 重写 `Public/Mind/GameMaster/MindGameMaster.h` + `.cpp`
- 修改 `Public/Mind/MindComponent.h` + `.cpp`（DispatchAction 流程）

## 关键 API / 伪代码

```cpp
// MindGameMaster.h
UCLASS(Abstract)
class AILIVEPROJECT_API AMindGameMaster : public AActor {
    GENERATED_BODY()
public:
    UPROPERTY(EditAnywhere) TArray<TObjectPtr<AActor>> Participants;
    UPROPERTY(BlueprintReadOnly) FName CurrentPhase = NAME_None;

    UFUNCTION(BlueprintCallable) virtual void StartGame();
    UFUNCTION(BlueprintCallable) virtual void EndGame();

    // Per-agent view（只读 GM 状态生成 prompt 上下文）
    virtual FMindAgentView BuildViewFor(AActor* Agent) PURE_VIRTUAL(,return {};);

    // === Validate / Apply 拆分 ===
    // Validate：只读，不改 state；返回 false 时 OutError 必填
    virtual bool Validate(AActor* Agent, const FMindActionEnvelope& Env, FString& OutError) const PURE_VIRTUAL(,return false;);

    // Apply：在 Action.Execute 成功 Done 之后才调；改 state（如：从 hand 移除卡 / 记录 vote）
    virtual void Apply(AActor* Agent, const FMindActionEnvelope& Env) PURE_VIRTUAL(,);

    // OnAgentActionFinished：Apply 之后调，子类决定 Phase 切换
    virtual void OnAgentActionFinished(AActor* Agent, const FMindActionEnvelope& Env, bool bExecuteOk) {}

    DECLARE_MULTICAST_DELEGATE_TwoParams(FOnPhaseChanged, FName, FName);
    FOnPhaseChanged OnPhaseChanged;

protected:
    void TransitionToPhase(FName NewPhase);
    void AwakeAgent(AActor* Agent, FString Reason);
    void RegisterActionsForAgent(AActor* Agent, const TArray<TSubclassOf<UMindAction>>& Actions);

    virtual void OnEnterPhase(FName Phase) {}
    virtual void OnGameStart() {}
    virtual void OnGameEnd() {}
};
```

```cpp
// MindGameMaster.cpp
void AMindGameMaster::StartGame() {
    for (AActor* P : Participants) {
        auto* MC = P->FindComponentByClass<UMindComponent>();
        if (MC) MC->Initialize(MC->Config, this);
    }
    OnGameStart();
}

void AMindGameMaster::TransitionToPhase(FName NewPhase) {
    FName Old = CurrentPhase;
    CurrentPhase = NewPhase;
    UE_LOG(LogMindGM, Log, TEXT("Phase %s -> %s"), *Old.ToString(), *NewPhase.ToString());
    OnPhaseChanged.Broadcast(Old, NewPhase);
    OnEnterPhase(NewPhase);
}
```

```cpp
// MindComponent.cpp 的 DispatchAction（按新流程）
void UMindComponent::DispatchAction(const FMindActionEnvelope& Env) {
    LastEnvelope = Env;

    // === 1. Validate（只读检查）===
    if (GameMaster.IsValid()) {
        FString Err;
        if (!GameMaster->Validate(GetOwner(), Env, Err)) {
            UE_LOG(LogMind, Warning, TEXT("Action rejected: %s"), *Err);
            OnActionDone(false, Err);  // 不调 Apply / OnAgentActionFinished
            return;
        }
    }

    // === 2. 找 Action 并 Execute（fire-and-forget TTS/动画）===
    auto* Act = ActionRegistry.FindRef(Env.ActionName);
    if (!Act) { OnActionDone(false, TEXT("unknown action")); return; }

    Act->Execute(this, Env.ParamsJson,
        FOnActionDone::CreateUObject(this, &UMindComponent::HandleActionDone));
}

void UMindComponent::HandleActionDone(bool bOk, const FString& Summary) {
    // === 3. Apply（成功才改 state）===
    if (bOk && GameMaster.IsValid()) {
        GameMaster->Apply(GetOwner(), LastEnvelope);
    }

    // === 4. 写记忆 + State Idle ===
    OnActionDone(bOk, Summary);  // 内部：Memory.Write + LastDecisionAt 更新 + State=Idle

    // === 5. 通知 GM（决定切阶段）===
    if (GameMaster.IsValid()) {
        GameMaster->OnAgentActionFinished(GetOwner(), LastEnvelope, bOk);
    }
}
```

## 验收信号
- 编译通过
- 在 `Level_LiarsBar.umap`（T10 已建）的 GM 上调 StartGame
- Output Log 看到 Phase 切换序列
- 给一个 NPC 调 AwakeAgent("test_awake")，能看到 RequestDecision 触发并尝试调 LLM
- 提交一个非法动作（手工构造 envelope）→ 看到 Validate 拒绝日志，state 没改
- 提交合法动作 → 看到 Validate pass → Execute 异步开始 → 完成后 Apply → OnAgentActionFinished

## 不在范围
- LiarsBar / MinorityRule 子类的具体阶段机（T12 / T21）
- Action 重试逻辑（不做——避免无限循环）

## 风险
- `Validate` 必须严格只读；如果子类不小心在 Validate 里 mutate 了，会导致 Apply 时状态已 partial 改 → **代码 review 时重点查 const 正确性**（`virtual bool Validate(...) const`）
- 异步 Action.Execute 期间如果 GM 被外部强制 EndGame，Apply 不会被调用（因为 GM.IsValid 失败）—— 接受，作为正常清理路径
- Apply 在游戏线程同步执行，子类 Apply 内部不应该有阻塞 IO（HTTP 等）
