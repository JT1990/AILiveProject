# T19 — 通用动作扩展（MoveTo / SitDown / LookAt / Wait / Observe / Think / Decision / Remember）

## 目标

实现 8 个通用动作，让少数决（M4）有充足的动作空间表达"走到投票箱前"、"私下示意盟友"、"先想一想再说"等行为。**Recall 不在通用 Action 列表**——recall 由 Provider 内部 tool_call 循环驱动（见 T05），不走 Action 流程。

## 前置

T09（`UMindContextManager` + `UMindMemoryClient` + Provider tool_call 已就位）+ T11（GM 基类 Validate/Apply 拆分）+ T18 / T19.5 / T19.7（Perception / EQS / SO 已在 M0 完成）

## DoD

- [ ] 实现 8 个 Action 子类（在 `Public/Mind/Actions/` 下）：

| Action     | Params                                      | 行为                                                                                                           |
| ---------- | ------------------------------------------- | -------------------------------------------------------------------------------------------------------------- |
| `move_to`  | `{target_actor_id?, named_location?}`       | 调 EQS_FindFacingPoint(target) 选落点 → AIController.MoveToLocation                                            |
| `sit_down` | `{}`                                        | 调 EQS_FindAvailableSeat → ApproachAndUse(Sit slot, T19.7)                                                     |
| `look_at`  | `{target_actor_id}`                         | 平滑转向；先用 `MindComponent.CanSenseActor(target, Sight)` 检查能否看见，看不见则 Done(false, "out of sight") |
| `wait`     | `{seconds: float}`                          | 计时器，到点 Done(true)                                                                                        |
| `observe`  | `{}`                                        | 主动让 PerceptionComponent 强刷一次（`ForceRebuildPerceptionPriorityList`），将最新感知合并进下次 view        |
| `think`    | `{thought: string}`                         | 写 event tag={"type":"thought","channel":"private"}，不发声不动作；同时 `Context.PushUser("[私念] ...")`       |
| `decision` | `{intent: string, target?, justification?}` | 写 event tag={"type":"intent"}；存到 NPC 上的 `CurrentIntent` 字段，下次 RecordPhaseChange 自动带上            |
| `remember` | `{content: string, tags?: dict}`            | 显式 `MemoryClient.WriteEvent(content, tags)`                                                                  |

- [ ] `UMindComponent` 加 `CurrentIntent` 字段，由 `GM.RecordPhaseChange` 在 `BuildViewFor` 时拼到 messages[1] L0b 段
- [ ] 通用动作在 `MindComponent::Initialize` 中默认全部注册
- [ ] `recall` 不在本卡范围——由 `UMindLLMProvider_DeepSeek` 在启动时 `RegisterTool("recall_long_term_memory", schema, MemoryClient.RecallSync)` 注册为 tool；LLM 主动调，Provider 内部循环（max_iterations=2），不进入 Action.Execute 流程

## 关键文件

- 实现 `Public/Mind/Actions/MindAction_MoveTo.cpp` 等 8 个文件

## 关键 API / 伪代码

```cpp
// MindAction_MoveTo.cpp（EQS + AIController 组合）
void UMindAction_MoveTo::Execute(UMindComponent* Owner, const FString& ParamsJson, FOnActionDone Done) {
    AActor* Target = ResolveTargetByAgentId(Owner, ParamsJson);
    if (!Target) { Done.ExecuteIfBound(false, "target not found"); return; }

    UMindEQSHelpers::RunFindFacingPoint(Owner, /*Querier*/Owner->GetOwner(), Target,
        FOnEQSDone::CreateLambda([Owner, Target, Done](bool ok, FVector Loc, AActor* /*slot*/) {
            if (!ok) { Done.ExecuteIfBound(false, "no valid path point"); return; }
            auto* Pawn = Cast<APawn>(Owner->GetOwner());
            auto* AICtrl = Cast<AAIController>(Pawn->GetController());
            FAIMoveRequest Req(Loc); Req.SetUsePathfinding(true); Req.SetAcceptanceRadius(60);
            AICtrl->MoveTo(Req, /*OutPath*/nullptr);
            // 注册 OnMoveCompleted 委托缓存 Done lambda
            AICtrl->ReceiveMoveCompleted.AddDynamic(/*...*/);
        }));
}
```

```cpp
// MindAction_SitDown.cpp（用 SO）
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
    Mem->WriteEvent(FMindEventReq{
        .agent_id = Owner->GetAgentId(),
        .action = TEXT("think"),
        .result_summary = Thought,
        .tags = {{"type","thought"}, {"channel","private"}}
    });
    Owner->GetContext()->PushUser(EMindChannel::Private,
        FString::Printf(TEXT("[私念] %s"), *Thought));

    Done.ExecuteIfBound(true, "thought silently");
}
```

```cpp
// MindAction_Decision.cpp
void UMindAction_Decision::Execute(UMindComponent* Owner, const FString& ParamsJson, FOnActionDone Done) {
    TSharedPtr<FJsonObject> P; ... 解析 intent
    Owner->CurrentIntent = Intent;   // 后续 RecordPhaseChange BuildViewFor 自动带

    auto* Mem = ...;
    Mem->WriteEvent(FMindEventReq{
        .agent_id = Owner->GetAgentId(),
        .action = TEXT("decision"),
        .result_summary = Intent,
        .tags = {{"type","intent"}}
    });
    Done.ExecuteIfBound(true, FString::Printf(TEXT("decided: %s"), *Intent));
}
```

## 验收信号

在 `L_prison` sandbox 测试：

- 让 NPC_1 用一个测试 BP 节点强制 RequestDecision("test_walk_to_player")，prompt 鼓励它选 move_to
- 看 NPC_1 走到玩家面前 + look_at + speak 一句话
- 测试 `think` 动作：触发后无外显但 Memory Service `/memory/event` 能查到 thought event + Context.Layer2 出现 `[私念]`
- 测试 `decision` 动作：触发后再次 RecordPhaseChange 时，messages[1] L0b 含 `Current Intent: ...`
- **recall 验证**（属于 T05 + Provider 工作）：设计明显需要"很久以前事件"的 prompt scenario，观察 LLM 在同一次 RequestCompletion 内通过 tool_call 调 `recall_long_term_memory`，结果作为 `role=tool` message append 后再生成最终 envelope；max_tool_iterations=2 上限生效

## 不在范围

- Vote / Propose Alliance / AcceptAlliance / AskQuestion 这些游戏专属动作（T22）
- Speak 加 channel 参数（T22）
- AI Controller 完整接管 NPC（保持只在需要时临时使用）
- Recall 通用 Action（已废弃；recall 走 Provider 内部 tool_call 循环）

## 风险

- **NPC 类型分支**（已由 T00 解决）：
  - Pawn 子类 → 用 `AAIController::MoveTo`，本卡按伪代码实施
  - Actor 子类 → 必须先把 BP 父类升 Pawn（更大改动）；本卡前置 T00 确保已升级
- NavMesh build 的覆盖范围决定 MoveTo 可达性
- LookAt 平滑实现：用 `RInterpTo` 每 tick 转向——MindComponent 默认不 tick，LookAt 需用 `FTSTicker` 临时挂；完成后取消
- **EQS 异步**：MoveTo / SitDown 现在涉及 2 段异步（EQS → MoveTo → SO Use），生命周期管理用 `TWeakObjectPtr` 守 Owner，确保中途销毁不崩
