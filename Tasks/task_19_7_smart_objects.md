# T19.7 — SmartObjects 基础（M0 项目地基，复用 GASP）

## 目标
复用项目内已有的 GASP SmartObject 框架（`BP_SmartObject_Base` / `SO_BenchDefinition` / `STT_FindSmartObject` / `STT_ClaimSlot` / `STT_UseSmartObject` / `AIC_NPC_SmartObject` 等），建好 NPC 与场景物体交互的标准化路径。**不自造低层 Claim API**——直接继承现有基类 + 用 GASP 已验证过的 SO Subsystem helper。

## 前置
T00（NPC 必须是 Pawn 子类 → 才能用 AIController + SO claim）+ T01（SmartObjectsModule + GameplayInteractionsModule）+ T03（GameMaster 骨架）

## DoD

### 1. 复用预检（**最先做**）
- [ ] 用 Monolith MCP 检查 `Content/Blueprints/SmartObjects/` 现有资产：
  - `BP_SmartObject_Base` 公开 API（继承时该 override 什么）
  - `SO_BenchDefinition` 看 Definition asset 字段约定（slot 命名 / behavior tags / activity tags）
  - `STT_FindSmartObject` / `STT_ClaimSlot` / `STT_UseSmartObject` 的输入/输出 pin 与触发条件
  - `AIC_NPC_SmartObject` 怎么挂 GASP NPC（说明 AIController 与 SO 的耦合方式）
- [ ] 把预检结果写到 `DevLog/2026-04-XX_smartobject_baseline.md`

### 2. 建 3 个 actor BP（继承 GASP 基类，**不写 C++**）
- [ ] `BP_VoteBox_SmartObject`：继承 `BP_SmartObject_Base` + StaticMesh（box，颜色按 yes/no 区分）+ Tags=`["VoteBox","yes"]` 或 `"no"`
- [ ] `BP_Chair_SmartObject`：继承 `BP_SmartObject_Base` + 椅子 mesh
- [ ] `BP_PokerSeat_SmartObject`：继承 `BP_SmartObject_Base` + 空场景组件（不可见站位标记）
- [ ] **用 Monolith MCP 创建**

### 3. 建 3 个 SOD 资产（参考 `SO_BenchDefinition` 复制改）
- [ ] `SOD_VoteBox`：1 slot `Cast`（接受 `Vote` activity tag）
- [ ] `SOD_Chair`：1 slot `Sit`
- [ ] `SOD_PokerSeat`：1 slot `Stand`
- [ ] 各 BP_*_SmartObject 在 SmartObjectComponent 引用对应 SOD

### 4. C++ helper（**薄包装**，不重写 Claim/Release）
- [ ] `UMindSmartObjectHelpers::ApproachAndUse(WorldCtx, Querier, SlotActor, ActivityTag, OnDone)`
  - 内部调 GASP `USmartObjectSubsystem::FindSlot` + `MarkSlotAsClaimed`
  - 接到 `AAIController::MoveToLocation(SlotXform.GetLocation())`
  - `OnMoveCompleted` 后 `MarkSlotAsFree`，回调 `OnDone(true)`
  - 失败路径（无 slot / Move fail）→ `OnDone(false, reason)`
- [ ] **如果 GASP 已有等效 helper**（StateTree task 链或现有 BPFL），优先 expose 给 Mind 层而不是新写

## 与 GASP NPC AI Controller 的关系
- [ ] 确认 `BP_NPC_MH_Character_*` 的 AIControllerClass 是 `AIC_NPC_SmartObject` 或同等支持 SO 的子类
- [ ] 如果不是，T00 / T19.7 期间需要给 NPC 父类替换 AIControllerClass（这是 BP 层面改动，用 MCP 完成）

## 关键文件
- 新建 BP（用 MCP）：
  - `Content/Blueprints/Interactables/BP_VoteBox_SmartObject.uasset`
  - `Content/Blueprints/Interactables/BP_Chair_SmartObject.uasset`
  - `Content/Blueprints/Interactables/BP_PokerSeat_SmartObject.uasset`
- 新建 SOD：
  - `Content/MyAssets/SmartObjects/SOD_VoteBox.uasset`
  - `Content/MyAssets/SmartObjects/SOD_Chair.uasset`
  - `Content/MyAssets/SmartObjects/SOD_PokerSeat.uasset`
- 新建 C++：
  - `Public/Mind/MindSmartObjectHelpers.h` + `.cpp`（薄包装）

## 关键 API / 伪代码

```cpp
DECLARE_DELEGATE_TwoParams(FOnSOUseDone, bool /*ok*/, FString /*reason*/);

UCLASS()
class UMindSmartObjectHelpers : public UBlueprintFunctionLibrary {
    GENERATED_BODY()
public:
    UFUNCTION(BlueprintCallable, meta=(WorldContext="WorldCtx"))
    static void ApproachAndUse(
        UObject* WorldCtx, AActor* Querier, AActor* SlotActor,
        FName ActivityTag, FOnSOUseDone Done);
};

// 实现伪代码：基于 GASP SO Subsystem
void UMindSmartObjectHelpers::ApproachAndUse(...) {
    auto* SOSubsys = USmartObjectSubsystem::GetCurrent(WorldCtx->GetWorld());
    auto* SOComp = SlotActor->FindComponentByClass<USmartObjectComponent>();
    if (!SOComp) { Done.ExecuteIfBound(false, "no SO comp"); return; }

    // 用 GASP 已用过的 Filter 模式（参考 STT_FindSmartObject）
    FSmartObjectRequestFilter Filter;
    Filter.ActivityRequirements.AddTag(FGameplayTag::RequestGameplayTag(ActivityTag));
    FSmartObjectClaimHandle Handle = SOSubsys->MarkSlotAsClaimed(SOComp->GetRegisteredHandle(), Filter);
    if (!Handle.IsValid()) { Done.ExecuteIfBound(false, "no slot available"); return; }

    FTransform Xform = SOSubsys->GetSlotTransform(Handle).Get(FTransform::Identity);
    auto* AICtrl = Cast<AAIController>(Cast<APawn>(Querier)->GetController());
    AICtrl->MoveToLocation(Xform.GetLocation());

    AICtrl->ReceiveMoveCompleted.AddDynamic(/*lambda*/ [SOSubsys, Handle, Done]() {
        SOSubsys->MarkSlotAsFree(Handle);
        Done.ExecuteIfBound(true, FString());
    });
}
```

⚠️ UE 5.7 SmartObjects API 的具体方法名 / 签名**实施时用 Monolith MCP `resolve_node` 验证一次**——GASP 资产已经在用某个版本的 API，对齐它就行。

## 后续消耗本卡资产的卡（不在本卡范围）
- T10 骗子酒馆 spawn `BP_PokerSeat_SmartObject`
- T20 少数决 spawn `BP_VoteBox_SmartObject` × 2 + `BP_Chair_SmartObject` × 8
- T19 SitDown action 调 ApproachAndUse(Sit)
- T22 Vote action 调 ApproachAndUse(Cast)

## 验收信号
- 在 `Level_AILive` 测试场景手动放 1 个 `BP_VoteBox_SmartObject` + 1 个 `BP_Chair_SmartObject`
- 用 Monolith MCP 给 NPC_2 调 `ApproachAndUse(self, chair, "Sit")` → NPC_2 走过去 + Done(true)
- `ApproachAndUse(self, votebox_yes, "Cast")` → 同样工作
- 没有可用 slot 时 `Done(false, "no slot available")`

## 不在范围
- 复杂多步交互（开门 / 操作机器）
- SO 行为驱动的 montage（M3 Montage 接口）
- 多 NPC 抢同一 slot 的并发竞争（GASP SO Subsystem 自身有 ClaimHandle 兜底）

## 风险
- GASP 现有 API 可能与 MVP 需求不完全对齐——预检阶段如果发现差异需要回头讨论范围
- `BP_SmartObject_Base` 可能内部持有大量 GASP 特定逻辑（如动画、状态）——继承时要观察是否带来不必要副作用
- ClaimHandle 生命周期：必须在 `OnDone` 之前 / 之中 ReleaseSlot；NPC 中途销毁会泄漏，先依赖 GASP 自身的清理路径
