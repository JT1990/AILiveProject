# T19.7 — SmartObjects 基础（M0 项目地基）

## 目标
NPC 与物体交互的标准化是项目地基。本卡建好 3 个 SOD 资产 + 3 个 actor BP + ApproachAndUse helper；业务消费（Vote / SitDown 走 SO 流程）在后续 T19/T22 完成。

## 前置
T00（NPC 类型确认 → Pawn 才能 MoveTo）+ T01（SmartObjectsModule + GameplayInteractionsModule）+ T03（GameMaster 骨架——helper 调用需要 actor 关系，骨架就够）

## DoD
- [ ] `AILiveProject.Build.cs` 增加依赖：`SmartObjectsModule`, `GameplayInteractionsModule`
- [ ] `AILiveProject.uproject` 确认 `SmartObjects` + `GameplayInteractions` plugin enabled（CLAUDE.md 显示已启用，无需改）
- [ ] 创建 3 个 SmartObject Definition（`Content/MyAssets/SmartObjects/`）：

| Definition | 用途 | Slot 设计 |
|---|---|---|
| `SOD_VoteBox` | 投票箱 | 1 slot "Approach"（NavMesh 可达点 30cm 前）→ 1 slot "Cast"（按下"投票"行为；带 tag `yes` or `no` 由 actor 实例区分） |
| `SOD_Chair` | 椅子 | 1 slot "Sit"（坐下点 + 朝向） |
| `SOD_PokerSeat` | 骗子酒馆桌位 | 1 slot "Stand"（站位 + 朝桌心） |

- [ ] 创建 3 个 Actor BP（**用 Monolith MCP**）：
  - `BP_VoteBox_SmartObject`：StaticMesh（box）+ `USmartObjectComponent` 引用 `SOD_VoteBox` + `Tags=["VoteBox","yes"]` 或 `"no"`
  - `BP_Chair_SmartObject`：StaticMesh（chair）+ `USmartObjectComponent` 引用 `SOD_Chair`
  - `BP_PokerSeat_SmartObject`：空 actor + scene component（不可见标记位）+ `USmartObjectComponent` 引用 `SOD_PokerSeat`
- [ ] C++ 接入层：`UMindSmartObjectHelpers`（BlueprintFunctionLibrary）
  - `ClaimAndUseSlot(Querier, SlotActor, OnDone)` — 标准 SO 申请 + 使用流程
- [ ] **资产就位（不放进任何关卡）**：本卡只创建 actor BP 和 SOD 资产，关卡的摆放在 T10 / T20 时通过 spawn_actor 直接用即可

## 后续消耗本卡资产的卡（不在本卡范围）
- T10 骗子酒馆关卡用 `BP_PokerSeat_SmartObject`（M2）
- T20 少数决关卡用 `BP_VoteBox_SmartObject` × 2 + `BP_Chair_SmartObject` × 8（M4）
- T19 SitDown action 调 ApproachAndUse(Sit)（M4）
- T22 Vote action 调 ApproachAndUse(Cast)（M4）

## 关键文件
- 新建 `Content/MyAssets/SmartObjects/SOD_*.uasset`（3 个 Definition）
- 新建 `Content/Blueprints/Interactables/BP_*_SmartObject.uasset`（3 个 actor BP）
- 新建 `Public/Mind/MindSmartObjectHelpers.h` + `.cpp`
- 修改 `AILiveProject.Build.cs`

## 关键 API / 伪代码

```cpp
// MindSmartObjectHelpers.h
DECLARE_DELEGATE_TwoParams(FOnSOUseDone, bool /*ok*/, FString /*reason*/);

UCLASS()
class UMindSmartObjectHelpers : public UBlueprintFunctionLibrary {
    GENERATED_BODY()
public:
    // 通用流程：Find handle → Claim → Move to slot → Use → Release
    UFUNCTION(BlueprintCallable, meta=(WorldContext="WorldCtx"))
    static void ApproachAndUse(
        UObject* WorldCtx, AActor* Querier, AActor* SlotActor,
        FName SlotTag, FOnSOUseDone Done);
};
```

```cpp
// .cpp 伪代码
void UMindSmartObjectHelpers::ApproachAndUse(...) {
    auto* SOSubsys = USmartObjectSubsystem::Get(WorldCtx->GetWorld());
    auto* SOComp = SlotActor->FindComponentByClass<USmartObjectComponent>();
    if (!SOComp) { Done(false, "no SO comp"); return; }

    // 1. 用 Filter 找匹配的 slot（按 tag）
    FSmartObjectClaimHandle Handle = SOSubsys->MarkSlotAsClaimed(SOComp->GetRegisteredHandle(),
        FSmartObjectRequestFilter{ /*BehaviorTags 含 SlotTag*/ });
    if (!Handle.IsValid()) { Done(false, "no slot available"); return; }

    // 2. MoveTo slot location
    FTransform SlotXform = SOSubsys->GetSlotTransform(Handle).Get(FTransform::Identity);
    auto* AICtrl = Cast<AAIController>(Cast<APawn>(Querier)->GetController());
    AICtrl->MoveToLocation(SlotXform.GetLocation());
    AICtrl->ReceiveMoveCompleted.AddDynamic([Querier, Handle, Done](FAIRequestID, EPathFollowingResult::Type Res) {
        if (Res != EPathFollowingResult::Success) { Done(false, "move fail"); return; }
        // 3. Use slot（这里 MVP 简化：到达即视为完成；真实场景可触发动画 montage）
        SOSubsys->MarkSlotAsFree(Handle);
        Done.ExecuteIfBound(true, "");
    });
}
```

## 与 Action 的集成

`UMindAction_Vote`（M4 / T22）改造：
```
LLM 输出 {choice: "yes"}
  ↓
Vote Action:
  1. RunFindNearestVoteBox(choice) → SlotActor (T19.5)
  2. ApproachAndUse(querier=self, slot_actor, tag="Cast")
  3. 成功回调 → GM.RecordVote → Done(true)
  4. 失败回调（无可达投票箱 / Move 失败）→ Done(false) → GM 视为"弃权 / 漏投"
```

`UMindAction_SitDown`（新增通用动作，骗子酒馆 / 少数决入座）：
```
RunFindAvailableSeat → SlotActor
ApproachAndUse(slot=Sit) → Done
```

## 验收信号
- 在 `Level_LiarsBar`（T10 已建）4 把椅子改成 `BP_PokerSeat_SmartObject` 后 PIE
- 用 Monolith MCP 给 NPC_2 调 `ApproachAndUse(self, seat_2, "Stand")` → 看到 NPC 走到对应位置
- `Level_MinorityRule`（T20 改造后）：放 1 个 NPC，给它调 `Vote(yes)` flow → 看到 NPC 走到 yes 投票箱前 + 完成
- 没有可用 slot 时 Done(false, "no slot available")，状态不污染

## 不在范围
- 复杂多步交互（如"开门"、"操作机器"）— 这是后续游戏卡的事
- SO 行为驱动的动画 montage（M3 在 T15 已经有 Montage 桥）
- 多 NPC 抢同一 slot 的并发竞争 — UE SO Subsystem 自身有 ClaimHandle 机制兜底，简化处理

## 风险
- **UE 5.7 SmartObjects API 在版本间变动**：用前先 `mcp__monolith__resolve_node` 验证当前可用方法签名，避免按文档写但 API 已重构
- 椅子改 SmartObject 后视觉外观需要保留（StaticMesh 仍要在），不要做成空 actor
- yes/no 投票箱用 actor tag 区分而不是两份 Definition——简化设计；如果未来需要不同投票动画再拆 Definition
- ClaimHandle 的生命周期：必须在 Done 回调里 ReleaseSlot，否则一旦 NPC 销毁会泄漏 slot 占用
