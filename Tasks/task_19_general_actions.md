# T19 — 通用动作扩展（MoveTo / LookAt / Wait / Observe / Think / Decision / Remember / Recall）

## 目标
实现剩余 8 个通用动作，让少数决（M4）有充足的动作空间表达"走到投票箱前"、"私下示意盟友"、"先想一想再说"等行为。

## 前置
T09（`UMindMemoryClient` 含 Write/Recall/ByTag）+ T11（GM 基类，含 Validate/Apply 拆分）—— T18 / T19.5 / T19.7 已在 M0 完成，本卡只消费它们

## DoD
- [ ] 实现 9 个 Action 子类（在 `Public/Mind/Actions/` 下，骨架 T03 已建；`sit_down` 为新增）：

| Action | Params | 行为 |
|---|---|---|
| `move_to` | `{target_actor_id?, named_location?}` | 调 EQS_FindFacingPoint(target) 选落点 → AIController.MoveToLocation。target 不可见时（CanSenseActor 返回 false）允许，因为 MoveTo 本身可以走盲区 |
| `sit_down` | `{}` | **新增**：调 EQS_FindAvailableSeat → ApproachAndUse(Sit slot, T19.7) |
| `look_at` | `{target_actor_id}` | 平滑转向；**先用 `MindComponent.CanSenseActor(target, Sight)` 检查能否看见**，看不见则 Done(false, "out of sight") |
| `wait` | `{seconds: float}` | 计时器，到点 Done(true) |
| `observe` | `{}` | 主动让 PerceptionComponent 强刷一次（`ForceRebuildPerceptionPriorityList`），将最新感知合并进下次 AgentView |
| `think` | `{thought: string}` | 写记忆 tag={"type":"thought","channel":"private"}，不发声不动作 |
| `decision` | `{intent: string, target?, justification?}` | 写记忆 tag={"type":"intent"}；同时存到 NPC 上的 `CurrentIntent` 字段，下轮 prompt 自动带上 |
| `remember` | `{content: string, tags?: dict}` | 显式 Write |
| `recall` | `{query: string, top_k?: int=5}` | Recall + 把结果回灌为下一轮 prompt 的 context |

- [ ] `UMindComponent` 加 `CurrentIntent` 字段，`BuildSystemPrompt` 里把它拼到 system 段
- [ ] 通用动作在 `MindComponent::Initialize` 中默认全部注册
- [ ] `recall` 动作的特殊处理：执行后立即触发一次 RequestDecision（带 query 结果作为 prompt 增量），不算正常 Done — 实现上：Done(true) 之后由 MindComponent 检查 `LastEnvelope.ActionName=="recall"` 立刻再次 RequestDecision
- [ ] **`recall` 改为同一次决策内 inline 完成**（推荐方案，避免与 cooldown 冲突）：
  - LLM 输出 `recall` 时，MindComponent 不调 `OnActionDone` 收尾，而是 ByTag/Recall 后**把检索结果拼到 prompt 后立即重新调 LLM**（同一个 `RequestDecision` 上下文）
  - `RecallChainDepth` 计数仍保留，超过 `RecallChainMax(=2)` 时移除 recall 并强制 LLM 选其他动作
  - 这样不进 `OnActionDone` → 不更新 `LastDecisionAt` → 不与 cooldown 冲突

## 关键文件
- 实现 `Public/Mind/Actions/MindAction_MoveTo.cpp` 等 8 个文件

## 关键 API / 伪代码

```cpp
// MindAction_MoveTo.cpp（改造为 EQS + SmartObject 组合）
void UMindAction_MoveTo::Execute(UMindComponent* Owner, const FString& ParamsJson, FOnActionDone Done) {
    // 1. 解析 target
    AActor* Target = ResolveTargetByAgentId(Owner, ParamsJson);
    if (!Target) { Done.ExecuteIfBound(false, "target not found"); return; }

    // 2. EQS 选落点（T19.5）
    UMindEQSHelpers::RunFindFacingPoint(Owner, /*Querier*/Owner->GetOwner(), Target,
        FOnEQSDone::CreateLambda([Owner, Target, Done](bool ok, FVector Loc, AActor* /*slot*/) {
            if (!ok) { Done.ExecuteIfBound(false, "no valid path point"); return; }
            // 3. MoveTo
            auto* Pawn = Cast<APawn>(Owner->GetOwner());
            auto* AICtrl = Cast<AAIController>(Pawn->GetController());
            FAIMoveRequest Req(Loc); Req.SetUsePathfinding(true); Req.SetAcceptanceRadius(60);
            AICtrl->MoveTo(Req, /*OutPath*/nullptr);
            // 简化：注册 OnMoveCompleted 委托缓存 Done lambda
            AICtrl->ReceiveMoveCompleted.AddDynamic(/*...*/);
        }));
}
```

```cpp
// MindAction_SitDown.cpp（新增，用 SO）
void UMindAction_SitDown::Execute(UMindComponent* Owner, const FString& ParamsJson, FOnActionDone Done) {
    UMindEQSHelpers::RunFindAvailableSeat(Owner, Owner->GetOwner(),
        FOnEQSDone::CreateLambda([Owner, Done](bool ok, FVector Loc, AActor* SeatActor) {
            if (!ok || !SeatActor) { Done.ExecuteIfBound(false, "no available seat"); return; }
            UMindSmartObjectHelpers::ApproachAndUse(Owner, Owner->GetOwner(), SeatActor, "Sit", Done);
        }));
}
```

```cpp
// MindAction_LookAt.cpp（用 Perception 检查可见性）
void UMindAction_LookAt::Execute(UMindComponent* Owner, const FString& ParamsJson, FOnActionDone Done) {
    AActor* Target = ResolveTargetByAgentId(Owner, ParamsJson);
    if (!Target) { Done.ExecuteIfBound(false, "no target"); return; }
    if (!Owner->CanSenseActor(Target, "Sight")) {
        Done.ExecuteIfBound(false, "out of sight"); return;
    }
    // 启用临时 tick 平滑转向 RInterpTo，到达后 Done
    StartSmoothTurn(Owner, Target, Done);
}
```

```cpp
// MindAction_Think.cpp
void UMindAction_Think::Execute(UMindComponent* Owner, const FString& ParamsJson, FOnActionDone Done) {
    TSharedPtr<FJsonObject> P; auto R = TJsonReaderFactory<>::Create(ParamsJson);
    FString Thought;
    if (FJsonSerializer::Deserialize(R, P)) P->TryGetStringField("thought", Thought);
    auto* Mem = Owner->GetWorld()->GetGameInstance()->GetSubsystem<UMindMemoryClient>();
    Mem->Write(Owner->GetAgentId(), Thought, {{"type","thought"}, {"channel","private"}});
    Done.ExecuteIfBound(true, "thought silently");
}
```

```cpp
// MindAction_Decision.cpp
void UMindAction_Decision::Execute(UMindComponent* Owner, const FString& ParamsJson, FOnActionDone Done) {
    TSharedPtr<FJsonObject> P; ... 解析 intent
    Owner->CurrentIntent = Intent;  // 后续 prompt 自动带
    auto* Mem = ...;
    Mem->Write(Owner->GetAgentId(), FString::Printf(TEXT("Intent: %s"), *Intent), {{"type","intent"}});
    Done.ExecuteIfBound(true, FString::Printf(TEXT("decided: %s"), *Intent));
}
```

```cpp
// MindAction_Recall.cpp — 特殊：执行后立即再触发一次决策
void UMindAction_Recall::Execute(UMindComponent* Owner, const FString& ParamsJson, FOnActionDone Done) {
    FString Query; int32 TopK = 5;
    // 解析...
    auto* Mem = ...;
    Mem->Recall(Owner->GetAgentId(), Query, TopK,
        FOnRecall::CreateLambda([Owner, Done](const TArray<FMindMemoryItem>& Items) {
            Owner->StashedRecall = FormatMemoriesAsText(Items);
            Done.ExecuteIfBound(true, FString::Printf(TEXT("recalled %d items"), Items.Num()));
            // 回到 OnActionDone，MindComponent 看 LastEnvelope.ActionName=="recall" → RequestDecision("post_recall")
        }));
}
```

## 验收信号

在 `Level_AILive` sandbox 测试（M4 关卡 T20 还没建）：

- 让 NPC_1 用一个测试 BP 节点强制 RequestDecision("test_walk_to_player")，prompt 鼓励它选 move_to
- 看 NPC_1 走到玩家面前 + look_at + speak 一句话
- 测试 `think` 动作：触发后无外显但 Memory Service 能 recall 出该 thought
- 测试 `decision` 动作：触发后再次 RequestDecision，新 prompt 的 system 里带 `Current Intent: ...`
- 测试 `recall` 动作：触发后 LLM 第二次决策的 prompt 里有刚 recall 出的内容

## 不在范围
- Vote / Propose Alliance / AcceptAlliance / AskQuestion 这些游戏专属动作（T22）
- Speak 加 channel 参数（T22）
- AI Controller 完整接管 NPC（保持只在需要时临时使用）

## 风险
- **NPC 类型分支**（已由 T00 解决）：
  - Pawn 子类 → 用 `AAIController::MoveTo`，本卡按伪代码实施
  - Actor 子类 → 必须先把 BP 父类升 Pawn（更大改动）；本卡前置 T00 确保已升级
- NavMesh build 的覆盖范围决定 MoveTo 可达性
- LookAt 平滑实现：用 `RInterpTo` 每 tick 转向——MindComponent 默认不 tick，LookAt 需用 `FTSTicker` 临时挂；完成后取消
- Recall 死循环防御不应过严（深度 2 太宽？太严？）—— M5 验证后调
- **EQS 异步**：MoveTo / SitDown 现在涉及 2 段异步（EQS → MoveTo → SO Use），生命周期管理用 `TWeakObjectPtr` 守 Owner，确保中途销毁不崩
