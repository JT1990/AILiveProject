# T18 — `UAIPerceptionComponent` 集成（M0 项目地基）

## 目标
和 NavMesh 同级的项目地基。本卡完成：
- NPC 父类挂 PerceptionComponent + StimuliSource，互相能感知
- 玩家 DefaultPawn 挂 StimuliSource
- MindComponent 提供 `GetCurrentlyVisibleAgentIds` / `GetCurrentlyAudibleAgentIds` / `CanSenseActor` API
- GM 基类提供 `BuildSceneAwarenessSection(Agent)` helper
- OnPerceptionUpdated 委托挂上但**只 log**（T07 真接 Mind 后才补"调 RequestDecision"那一行）
- 私聊 hearing 物理判定的工具方法就位（消费它在 T22）

## 前置
T00（**强依赖**：NPC 必须是 Pawn 子类；如是 Actor 需要先升级 Pawn——这是 T18 进 M0 的真实成本）+ T01（AIModule + GameplayTasks）+ T02（MindComponent 骨架）

## DoD（M0 范围）

### 1. 组件就位
- [ ] `BP_NPC_MH_Character`（父类）加：
  - `UAIPerceptionComponent`
  - `UAIPerceptionStimuliSourceComponent`（让 NPC 之间互相能感知）
  - `AISenseConfig_Sight`（视距 1500cm / 锁角 90° / 失忆 5s）
  - `AISenseConfig_Hearing`（听距 800cm / 失忆 5s）
- [ ] StimuliSource 注册感知到 Sight + Hearing
- [ ] 玩家 DefaultPawn 加 `UAIPerceptionStimuliSourceComponent`（让 NPC 能感知玩家）—— **用 Monolith MCP**

### 2. 感知回调（M0 阶段先 stub，T07 接 Mind 后补真实调用）
- [ ] `UMindComponent::OnPerceptionUpdated(AActor* Source, FAIStimulus Stim)`：
  - 节流：相同 Source 5s 内重复刺激不触发
  - **行为受 `bPerceptionCanTriggerDecision` flag 控制**（M0 默认 false → 仅 log；T07 完成后由 NPC.Initialize 显式打开）
- [ ] `UMindComponent` 加 `bool bPerceptionCanTriggerDecision = false;` + setter `SetPerceptionCanTriggerDecision(bool)`

### 3. AgentView 数据源 API
- [ ] `UMindComponent` 提供：
  - `GetCurrentlyVisibleAgentIds() const` —— 返回 agent_id 列表
  - `GetCurrentlyAudibleAgentIds() const`
  - `CanSenseActor(AActor*, FName SenseTag) const`
- [ ] `AMindGameMaster` 基类提供 `BuildSceneAwarenessSection(Agent) const` —— GM 子类（T12/T21）拼 AgentView 时调用

### 4. 私聊 hearing 物理判定的工具就位
- [ ] `CanSenseActor(other, "Hearing")` 是物理性私聊的核心 query
- [ ] 真正的"按 hearing 写记忆"消费在 T22 的 `RecordSpeechEvent`，本卡只确保 query API 工作

## 不在 M0 范围（T07 / T22 等卡内消费）
- OnPerceptionUpdated 真正调 RequestDecision（T07 实施细节）
- AgentView 拼 awareness section 进 prompt（T12 / T21）
- 私聊记忆按 hearing 距离过滤（T22）
- 压力测试 frame time spike（移到 T07 完成后跑一次）

## 关键文件
- 修改 `BP_NPC_MH_Character`（父类，加 PerceptionComponent + StimuliSource + 委托绑定）—— **用 Monolith MCP**
- 修改 `Public/Mind/MindComponent.h` + `.cpp`（OnPerceptionUpdated + GetCurrently*Agents）
- 修改 `Public/Mind/GameMaster/MindGameMaster.h`（基类 BuildViewFor 提供工具方法 `BuildSceneAwarenessSection(Agent)` 让子类拼）
- 修改 `MindGameMaster_*` 子类 BuildViewFor（拼 awareness section）
- 修改 DefaultPawn（加 StimuliSource，BP 子类化或在 `Level_AILive` 里给现有 DefaultPawn 加组件）

## 关键 API / 伪代码

```cpp
// MindComponent.h
UFUNCTION(BlueprintCallable)
void OnPerceptionUpdated(AActor* Source, FAIStimulus Stimulus);

UFUNCTION(BlueprintCallable, BlueprintPure)
TArray<FString> GetCurrentlyVisibleAgentIds() const;

UFUNCTION(BlueprintCallable, BlueprintPure)
TArray<FString> GetCurrentlyAudibleAgentIds() const;

UFUNCTION(BlueprintCallable, BlueprintPure)
bool CanSenseActor(AActor* Other, FName SenseTag /*Sight or Hearing*/) const;
```

```cpp
// MindComponent.cpp
TArray<FString> UMindComponent::GetCurrentlyVisibleAgentIds() const {
    auto* Pawn = Cast<APawn>(GetOwner());
    auto* AICtrl = Pawn ? Cast<AAIController>(Pawn->GetController()) : nullptr;
    auto* PC = AICtrl ? AICtrl->GetPerceptionComponent() : GetOwner()->FindComponentByClass<UAIPerceptionComponent>();
    if (!PC) return {};

    TArray<AActor*> Out;
    PC->GetCurrentlyPerceivedActors(UAISense_Sight::StaticClass(), Out);
    TArray<FString> Ids;
    for (AActor* A : Out) {
        auto* MC = A->FindComponentByClass<UMindComponent>();
        if (MC) Ids.Add(MC->GetAgentId());
    }
    return Ids;
}

void UMindComponent::OnPerceptionUpdated(AActor* Source, FAIStimulus Stim) {
    if (!Source) return;
    const float Now = GetWorld()->GetTimeSeconds();
    auto* Last = LastPerceptionAt.Find(Source);
    if (Last && (Now - *Last) < 5.0f) return;  // 节流
    LastPerceptionAt.Add(Source, Now);

    FString Reason;
    if (Stim.Type == UAISense::GetSenseID<UAISense_Sight>())
        Reason = FString::Printf(TEXT("saw_%s"), *Source->GetName());
    else if (Stim.Type == UAISense::GetSenseID<UAISense_Hearing>())
        Reason = FString::Printf(TEXT("heard_%s"), *Source->GetName());

    UE_LOG(LogMind, Verbose, TEXT("Perception: %s"), *Reason);

    // M0 阶段：仅 log，不触发决策。T07 / M1 完成后通过 SetPerceptionCanTriggerDecision(true) 打开
    if (bPerceptionCanTriggerDecision) {
        RequestDecision(Reason);  // 内部已节流（cooldown）+ async
    }
}
```

```cpp
// MindGameMaster.h（基类提供工具）
protected:
    FString BuildSceneAwarenessSection(AActor* Agent) const;
```

```cpp
// MindGameMaster.cpp
FString AMindGameMaster::BuildSceneAwarenessSection(AActor* Agent) const {
    auto* MC = Agent->FindComponentByClass<UMindComponent>();
    if (!MC) return {};
    TArray<FString> Visible = MC->GetCurrentlyVisibleAgentIds();
    TArray<FString> Audible = MC->GetCurrentlyAudibleAgentIds();
    TArray<FString> AllParticipants;
    for (AActor* P : Participants) {
        if (P == Agent) continue;
        auto* PMC = P->FindComponentByClass<UMindComponent>();
        if (PMC) AllParticipants.Add(PMC->GetAgentId());
    }
    TArray<FString> CannotSee;
    for (auto& Id : AllParticipants) if (!Visible.Contains(Id)) CannotSee.Add(Id);

    return FString::Printf(TEXT(
        "## 你当前的感知（信息不对称）\n"
        "- 视野中: [%s]\n"
        "- 最近听到说话: [%s]\n"
        "- 视野外（不知道他们在干什么）: [%s]\n"),
        *FString::Join(Visible, TEXT(",")),
        *FString::Join(Audible, TEXT(",")),
        *FString::Join(CannotSee, TEXT(",")));
}
```

```cpp
// 子类 BuildViewFor 调用
FMindAgentView AMindGameMaster_LiarsBar::BuildViewFor(AActor* Agent) {
    FMindAgentView V;
    // ... 现有内容 ...
    V.PublicStateText += BuildSceneAwarenessSection(Agent);  // 追加
    return V;
}
```

## 验收信号（M0 阶段无 LLM 也能验）

### 功能
- 在 `Level_AILive` 放 NPC_2 + NPC_3，朝向不同：
  - NPC_2 面朝 NPC_3
  - 编辑器手工 BP 节点 `NPC_2.MindComponent.GetCurrentlyVisibleAgentIds()` → 返回 `["npc_3"]`
  - `NPC_3.GetCurrentlyVisibleAgentIds()` → 返回 `[]`（NPC_3 背对 NPC_2 看不见）
- 玩家飞过 NPC_2 旁边 → Output Log 看到 `LogMind: saw_DefaultPawn_C_0`（仅日志，不触发决策因为 RequestDecision 还没接）
- 离开 5s 后再回来 → 节流通过新 trace 出现

### API
- `CanSenseActor(npc_3, "Sight")` 在 NPC_2 视野内时返回 true，背对时返回 false
- `BuildSceneAwarenessSection(npc_2)` 返回包含 "视野中: [npc_3]" 的字符串

### 不验（推迟到对应里程碑）
- LLM 决策链路：T05 / T07 完成后再跑
- frame time spike 性能测试：T07 完成后回头跑
- 私聊 hearing 物理过滤：T22

## 不在范围
- 噪音模拟（玩家说话产生 hearing 刺激给 NPC）— 后续 polish
- "Damage" / "Touch" sense — 不需要
- 视野遮挡复杂度（用引擎默认 Sight Trace 即可）

## 风险
- 8 NPC 互相 stimuli source + perception 在 GM Negotiate 阶段一起 active 时 trace 数量是 8×8=64/tick，引擎内置批处理可控
- `GetCurrentlyPerceivedActors` 在某些 UE 版本要求先注册 PerceptionListener — 检查接入时是否需要 init
- 感知回调频繁触发可能导致 cooldown drop 占 95%——这是设计预期
- DefaultPawn 加 StimuliSource 在 PIE 下要 spawn 后立即 RegisterForSense，否则前几秒不可感知
- **如果 BP_NPC_MH_Character 是 Actor 子类（T00 验证）**，AIController 不存在，此卡需要重设计——感知组件可挂在 Actor 上但 stimuli source 处理路径要确认
