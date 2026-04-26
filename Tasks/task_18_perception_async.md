# T18 — `UAIPerceptionComponent` 集成（M0 项目地基）

## 目标

和 NavMesh 同级的项目地基。本卡完成：

- NPC 父类挂 PerceptionComponent + StimuliSource，互相能感知
- 玩家 DefaultPawn 挂 StimuliSource
- MindComponent 提供 `GetCurrentlyVisibleAgentIds` / `GetCurrentlyAudibleAgentIds` / `CanSenseActor` API
- GM 基类提供 `BuildSceneAwarenessSection(Agent)` helper（返回 FString，由子类 `BuildViewFor` 拼进 `FMindMessage` body）
- OnPerceptionUpdated 委托：节流后 push 一条 `[感知]` user message 到 `Context.Layer2`；是否调 `RequestDecision` 由 `bPerceptionCanTriggerDecision` flag + 阶段策略决定（M0 默认 false）
- 私聊 hearing 物理判定的工具方法就位（消费它在 T22）

## 前置

T00（**强依赖**：NPC 必须是 Pawn 子类）+ T01（AIModule + GameplayTasks）+ T02（MindComponent 骨架）

## DoD（M0 范围）

### 1. 组件就位

- [ ] `BP_NPC_MH_Character`（父类）加：
  - `UAIPerceptionComponent`
  - `UAIPerceptionStimuliSourceComponent`（让 NPC 之间互相能感知）
  - `AISenseConfig_Sight`（视距 1500cm / 锁角 90° / 失忆 5s）
  - `AISenseConfig_Hearing`（听距 800cm / 失忆 5s）
- [ ] StimuliSource 注册感知到 Sight + Hearing
- [ ] 玩家 DefaultPawn 加 `UAIPerceptionStimuliSourceComponent`（让 NPC 能感知玩家）—— **用 Monolith MCP**

### 2. 感知回调（push user message + 阶段策略控制是否唤醒）

- [ ] `UMindComponent::OnPerceptionUpdated(AActor* Source, FAIStimulus Stim)`：
  - 节流：相同 Source 5s 内重复刺激不触发
  - 节流通过后：`Context->PushUser(EMindChannel::System, "[感知] X 进入视野/听到 X 的动静")`（无论是否唤醒，事件都进入 Layer2）
  - 是否调 `RequestDecision` 由 `bPerceptionCanTriggerDecision` flag 控制（M0 默认 false → 仅 push + log；T07 完成后由 NPC.Initialize 显式打开；某些阶段如 Negotiate 由 GM 显式 disable）
- [ ] `UMindComponent` 加 `bool bPerceptionCanTriggerDecision = false;` + setter `SetPerceptionCanTriggerDecision(bool)`

### 3. AgentView 数据源 API

- [ ] `UMindComponent` 提供：
  - `GetCurrentlyVisibleAgentIds() const` —— 返回 agent_id 列表
  - `GetCurrentlyAudibleAgentIds() const`
  - `CanSenseActor(AActor*, FName SenseTag) const`
- [ ] `AMindGameMaster` 基类提供 `BuildSceneAwarenessSection(Agent) const` —— GM 子类（T12 / T21）`BuildViewFor` 时调用并把字符串拼进返回的 `FMindMessage` body

### 4. 私聊 hearing 物理判定的工具就位

- [ ] `CanSenseActor(other, "Hearing")` 是物理性私聊的核心 query
- [ ] 真正的"按 hearing 推送 listener"消费在 T22 `RecordSpeechEvent`，本卡只确保 query API 工作

## 不在 M0 范围（T07 / T22 / T12 等卡内消费）

- OnPerceptionUpdated 实际唤醒决策（由 T07 打开 flag）
- AgentView 拼 awareness section（T12 / T21 实现 BuildViewFor 时调）
- 私聊偷听物理过滤（T22）
- 压力测试 frame time spike（移到 T07 完成后跑一次）

## 关键文件

- 修改 `BP_NPC_MH_Character`（父类，加 PerceptionComponent + StimuliSource + 委托绑定）—— **用 Monolith MCP**
- 修改 `Public/Mind/MindComponent.h` + `.cpp`（OnPerceptionUpdated + GetCurrently\*Agents）
- 修改 `Public/Mind/GameMaster/MindGameMaster.h`（基类 `BuildSceneAwarenessSection(Agent)` 工具方法）
- 修改 `MindGameMaster_*` 子类 `BuildViewFor`（拼 awareness 字符串进 `FMindMessage` body）
- 修改 DefaultPawn（加 StimuliSource）

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

UFUNCTION(BlueprintCallable)
void SetPerceptionCanTriggerDecision(bool bEnable);

UPROPERTY(EditAnywhere)
bool bPerceptionCanTriggerDecision = false;
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
    if (Last && (Now - *Last) < 5.0f) return;
    LastPerceptionAt.Add(Source, Now);

    FString SenseLabel;
    if (Stim.Type == UAISense::GetSenseID<UAISense_Sight>()) SenseLabel = TEXT("看到");
    else if (Stim.Type == UAISense::GetSenseID<UAISense_Hearing>()) SenseLabel = TEXT("听到");
    else SenseLabel = TEXT("感知");

    FString PerceptionLine = FString::Printf(TEXT("[感知] %s %s"), *SenseLabel, *Source->GetName());
    UE_LOG(LogMind, Verbose, TEXT("%s"), *PerceptionLine);

    // 事件总是进 Layer2（保证 buffer 完整性）
    if (Context) Context->PushUser(EMindChannel::System, PerceptionLine);

    // 是否唤醒决策：由 flag + 阶段策略决定
    if (bPerceptionCanTriggerDecision) {
        FString Reason = FString::Printf(TEXT("perception_%s_%s"), *SenseLabel, *Source->GetName());
        RequestDecision(Reason);   // 内部仍走 cooldown / bIsCompacting / PendingTrigger
    }
}
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
FMindMessage AMindGameMaster_LiarsBar::BuildViewFor(AActor* Agent) {
    FString Body = FString::Printf(TEXT("[当前帧]\n- 阶段: %s\n..."), *CurrentPhase.ToString());
    Body += BuildSceneAwarenessSection(Agent);   // 追加 awareness 段
    return FMindMessage{
        .Role = EMindRole::User,
        .Content = Body,
        .Channel = EMindChannel::System,
        .Tags = {{"phase", CurrentPhase.ToString()}, {"vis", "private+public+awareness"}}
    };
}
```

## 验收信号（M0 阶段无 LLM 也能验）

### 功能

- 在 `L_prison` 放 NPC_2 + NPC_3，朝向不同：
  - NPC_2 面朝 NPC_3
  - 编辑器手工 BP 节点 `NPC_2.MindComponent.GetCurrentlyVisibleAgentIds()` → 返回 `["npc_3"]`
  - `NPC_3.GetCurrentlyVisibleAgentIds()` → 返回 `[]`（NPC_3 背对 NPC_2 看不见）
- 玩家飞过 NPC_2 旁边 → Output Log 看到 `LogMind: [感知] 看到 DefaultPawn_C_0`；M0 阶段 `bPerceptionCanTriggerDecision=false` 不触发 RequestDecision
- 离开 5s 后再回来 → 节流通过新 trace 出现
- T07 后打开 flag 跑一次：感知触发会 push `[感知]` 到 Layer2 + 调 RequestDecision

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
- 感知触发在阶段如 Negotiate 应由 GM 显式 disable（避免与 GM 主动唤醒并发）
- DefaultPawn 加 StimuliSource 在 PIE 下要 spawn 后立即 RegisterForSense，否则前几秒不可感知
- **如果 BP_NPC_MH_Character 是 Actor 子类（T00 验证）**，AIController 不存在，此卡需要重设计——感知组件可挂在 Actor 上但 stimuli source 处理路径要确认
