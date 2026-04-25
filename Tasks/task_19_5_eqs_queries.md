# T19.5 — EQS 查询集合（M0 项目地基）

## 目标

执行层选位逻辑是项目地基。本卡建好 4 个 EQS query 资产 + Helpers C++ 库；具体业务消费（MoveTo Action 用 FindFacingPoint 等）在后续 T19/T22 时完成。

## 前置

T00（NPC 类型确认）+ T01（AIModule + NavigationSystem）+ T19.7（**强依赖**：EQS Generator 用 ActorsOfClass(BP\_\*\_SmartObject)，必须 SO actor 先存在）

## DoD

- [ ] `Plugins` / 项目启用 EQS（已在 AIModule 内，无需额外操作；确认 `Project Settings > AI System > bEnvQueryEnabled = true`）
- [ ] 创建 4 个核心 EQS 查询（`Content/MyAssets/EQS/`）：

| 查询                      | 用途                                                     | 关键 Generator + Test                                                                                                     |
| ------------------------- | -------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------- |
| `EQS_FindAvailableSeat`   | 找空椅（骗子酒馆 / 少数决入座）                          | Generator: ActorsOfClass(`BP_Chair_SmartObject`) → Test: Path Exists / Distance                                           |
| `EQS_FindNearestVoteBox`  | 按 yes/no 选投票箱                                       | Generator: ActorsOfClass(`BP_VoteBox_SmartObject`) → Test: Tag(`yes`/`no`) / Distance / PathExists                        |
| `EQS_FindFacingPoint`     | 走到一个能"面向 target"的位置（说话用）                  | Generator: PointsAroundContext(target, radius=180-220) → Test: Visibility(target) / NavMesh / Distance                    |
| `EQS_FindPrivateChatSpot` | 私聊位置——靠近 target 但远离其他在场 NPC（少数决联盟用） | Generator: PointsAroundContext(target, radius=300) → Test: Distance(其他 alive)（更远更好）/ NavMesh / Visibility(target) |

- [ ] C++ 包装：`UMindEQSHelpers`（BlueprintFunctionLibrary）
  - `RunFindSeat(WorldCtx, OnFound)` — 异步运行 query，回调 `(bool ok, FVector loc, AActor* slot)`
  - `RunFindVoteBox(WorldCtx, FString Choice, OnFound)`
  - `RunFindFacingPoint(WorldCtx, AActor* Target, OnFound)`
  - `RunFindPrivateChatSpot(WorldCtx, AActor* Target, TArray<AActor*> Others, OnFound)`
- [ ] 所有 query **异步运行**（`RunMode_SingleResult` + 回调），**不阻塞游戏线程**
- [ ] 失败回退：query 无结果时 `OnFound(false, ZeroVec, nullptr)`，调用方决策时回退到当前位置 / 跳过动作

## 关键文件

- 新建 `Content/MyAssets/EQS/EQS_FindAvailableSeat.uasset` 等 4 个 query
- 新建 `Content/MyAssets/EQS/Context_*.uasset`（如 `Context_TargetActor`，把 `EnvQueryContext_Item` 子类化用于传 target）
- 新建 `Public/Mind/MindEQSHelpers.h` + `.cpp`
- **用 Monolith MCP 创建 EQS 资产**（CLAUDE.md 强约束）

## 关键 API / 伪代码

```cpp
// MindEQSHelpers.h
DECLARE_DELEGATE_ThreeParams(FOnEQSDone, bool /*ok*/, FVector /*loc*/, AActor* /*slot_actor*/);

UCLASS()
class UMindEQSHelpers : public UBlueprintFunctionLibrary {
    GENERATED_BODY()
public:
    static void RunQuery(UObject* WorldCtx, UEnvQuery* Query, AActor* Querier,
                         const TMap<FName, FActorPropertyValue>& Params, FOnEQSDone Done);

    UFUNCTION(BlueprintCallable, meta=(WorldContext="WorldCtx"))
    static void RunFindSeat(UObject* WorldCtx, AActor* Querier, FOnEQSDone Done);

    UFUNCTION(BlueprintCallable)
    static void RunFindVoteBox(UObject* WorldCtx, AActor* Querier, FString Choice, FOnEQSDone Done);
    // ...
};
```

```cpp
// .cpp 伪代码
void UMindEQSHelpers::RunQuery(UObject* WorldCtx, UEnvQuery* Q, AActor* Querier, ..., FOnEQSDone Done) {
    auto* World = GEngine->GetWorldFromContextObject(WorldCtx, EGetWorldErrorMode::LogAndReturnNull);
    auto* QueryMgr = UEnvQueryManager::GetCurrent(World);
    FEnvQueryRequest Req(Q, Querier);
    // 配 Params（如 target actor，作为 Context）
    Req.Execute(EEnvQueryRunMode::SingleResult,
        FQueryFinishedSignature::CreateLambda([Done](TSharedPtr<FEnvQueryResult> Res) {
            if (!Res.IsValid() || !Res->IsSuccessful() || Res->Items.Num() == 0) {
                Done.ExecuteIfBound(false, FVector::ZeroVector, nullptr); return;
            }
            FVector Loc = Res->GetItemAsLocation(0);
            AActor* Slot = Res->GetItemAsActor(0);
            Done.ExecuteIfBound(true, Loc, Slot);
        }));
}
```

## 与现有 Action 的集成

`UMindAction_MoveTo` 改造（**原 T19 的 MoveTo 升级版**）：

```
LLM 输出 {target_actor_id: "npc_3"}
  ↓
MoveTo Action:
  1. 找到 target actor (按 agent_id)
  2. RunFindFacingPoint(target) → EQS 选一个"面向 target 且 NavMesh 可达"的点
  3. AIController.MoveToLocation(eqs_result)
  4. 到达回调 → Done(true)
```

`UMindAction_Vote`（少数决，T22）：

```
LLM 输出 {choice: "yes"}
  ↓
Vote Action:
  1. RunFindVoteBox(choice="yes") → 选一个 yes 投票箱（最近 + 可达）
  2. AIController.MoveTo + 到达后触发 SmartObject 交互（T19.7）
  3. SmartObject 完成回调 → GM.RecordVote → Done(true)
```

## 验收信号（M0 阶段，手工调可验）

- 在 `L_prison` 测试场景里放 4 个 `BP_Chair_SmartObject` + 2 个 `BP_VoteBox_SmartObject`（带 yes/no tag）（资产由 T19.7 创建）
- 用 Monolith MCP 给一个 NPC 调 `RunFindAvailableSeat` → 看到回调返回正确的椅子位置
- 调 `RunFindNearestVoteBox(choice="yes")` → 返回 yes 那个 box
- `RunFindFacingPoint(target=NPC_2)` → 返回 NPC_2 周围 NavMesh 可达点
- 任何一次 EQS 调用游戏线程 frame time spike < 5ms（query 是异步执行）

## 不在 M0 范围

- 业务 Action 调 EQS 的集成（T19 MoveTo / T22 Vote）

## 不在范围

- 复杂战术 EQS（如"找掩体"、"包围目标"）— 这些等扩展到空间博弈游戏卡
- EQS 性能优化（Sample 规模、tick rate） — MVP 用默认即可
- EQS 可视化调试 — 引擎自带 `Show EQS` debug 已够

## 风险

- EQS Generator `ActorsOfClass` 需要场景里真有这种类——T19.7 的 SmartObject 类必须先做出来
- `Visibility` test 用 line trace 走 `Visibility` channel，配 `bShouldTraceForGroundCheck=false` 减少误判
- `PointsAroundContext` 在小空间（如骗子酒馆 10×10m）半径要小，避免穿墙；用 NavMesh test 兜底
- 异步回调中 Querier actor 可能已销毁，用 `TWeakObjectPtr` 守
