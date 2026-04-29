# NPC1 走到长椅并坐下（SmartObject 入位 + 姿态切换）

日期：2026-04-29

## 目标

按 K 键 → NPC1 申领 `BP_SmartBench_Example`（GASP 5.7 范本长椅）的第一个空闲 slot → 内层 `ST_SmartObject_Bench` 接管 → FindSlotEntranceLocation 算 entrance（投到 NavMesh）→ StateTreeMoveToTask 走过去 → entry montage 把 NPC 对齐到 slot 并切换站→坐姿态 → 停在 sit loop。

LLM Mind 模块接入前的最小执行基线：验证 SmartObject claim → entrance MoveTo → 入位姿态切换 整条链路在本工程跑通。

## 90% 资产已就绪

工程是 GASP 5.7 范本 fork，整套 SmartObject 链路 **已存在**：

| 资产 | 角色 |
|---|---|
| `Content/Blueprints/SmartObjects/Bench/BP_SmartBench`（关卡 label `BP_SmartBench_Example` 位于 `(9376, 1630, 240)`） | Bench actor，父类 `BP_SmartObject_Base` 带 `USmartObjectComponent` |
| `Content/Blueprints/SmartObjects/Bench/SO_BenchDefinition` | 2 slots（local Y=±45）+ `SmartObjectSlotEntranceAnnotation`（默认 Offset=0,0,0，靠 `bProjectNavigationLocation` 投到 NavMesh）；`UserTagFilter = ANY_EXACT(SmartObject.ObjectType.NPC, SmartObject.ObjectType.Player)`；`DefaultBehaviorDefinitions` 含 inline `GameplayInteractionSmartObjectBehaviorDefinition_0` 指向 `ST_SmartObject_Bench` |
| `Content/Blueprints/SmartObjects/Bench/ST_SmartObject_Bench`（schema=`GameplayInteractionStateTreeSchema`） | `Find Slot Location → Move To Slot And Play Entry → Play Loop → Play Exit → ReleaseSlot` |
| `Content/Blueprints/SmartObjects/AC_SmartObjectAnimation` | 已挂在 `SandboxCharacter_Mover` 上：`SmartObjectAnimationPayload`、`Owner Montage Finished` dispatcher、`Warp Target Name="SmartObject"`、`Cache Necessary Data` 自动取 SkeletalMesh + MoverComponent |
| `Content/Blueprints/SmartObjects/TasksAndConditions/STT_PlayAnimFromBestCost` | StateTree Task BP，通过 Chooser ProxyTable `CHPT_SmartObject_Bench` 选 entry/exit montage 喂给 `AC_SmartObjectAnimation` |
| `Content/Blueprints/SmartObjects/TasksAndConditions/STT_UseSmartObject` | GASP 范本里被 `ST_NPC_SandboxCharacter_SmartObject` 调，内部用 `K2Node_LatentGameplayTaskCall` 包 `UseSmartObjectWithGameplayInteraction`。本里程碑参考它确定接入点 |
| `SandboxCharacter_Mover` 14 组件 | `CharacterMover` + `NavMover` + `MotionWarping` + `AC_SmartObjectAnimation` + `AIPerceptionStimuliSource` + ... |
| 启用插件 | `SmartObjects` / `GameplayInteractions` / `GameplayBehaviorSmartObjects` / `Mover` / `MotionWarping` / `Chooser` / `PoseSearch` |

**结论**：本里程碑只补"K 键触发：找 SO actor → 申领 slot → 调 `UseSmartObjectWithGameplayInteraction` → ReadyForActivation"的入口胶水，并补 user tag。

## 技术决策

**蓝图为主 + 一个 C++ 小工具**。Level BP 已是 M / I / N 测试触发链路（按 `2026-04-29_npc_perception_hearing.md` 踩坑#1 用嵌套 elseif），K 键沿用同模式接到 N 键 Branch 的 `.else`。

唯一的 C++ 是 `UAILiveProjectPerceptionLogger::ClaimFirstSlotInActor` —— `FindSmartObjectsInActor` + `MarkSmartObjectSlotAsClaimed` 的小封装，并主动给 `Filter.UserTags` 加 `SmartObject.ObjectType.NPC`。原因：

1. **SO 的 UserTagFilter 必须满足**。`SO_BenchDefinition.UserTagFilter = ANY_EXACT(SmartObject.ObjectType.NPC, .Player)`，但 `FindSmartObjectsInActor` 的默认 `FSmartObjectRequestFilter.UserTags` 为空（`UserActor` 参数仅用于条件求值上下文，不自动注入 user tag）→ ANY_EXACT 评估为 false → 所有 slot 被过滤。helper 内主动 `Filter.UserTags.AddTag(NPCTag)` 才有结果。
2. BP 端 `Array_Get + BreakStruct` 取 `OutResults[0].SlotHandle` 在 MCP 下走不通（wildcard pin 类型推断失败，DevLog `2026-04-28_npc_movement_basic.md` 已记），C++ 一行解决，未来 Mind 模块可直接复用。

**用 `UseSmartObjectWithGameplayInteraction`（无外层 MoveTo），不用 `MoveToAndUse...`**。`AITask_UseGameplayInteraction.cpp:118-126` 显示 `MoveToAndUseSmartObjectWithGameplayInteraction` 的外层 `AITask_MoveTo` 用 `SmartObjectSubsystem::GetSlotLocation(ClaimedHandle)` 作目标 —— 即 slot 本体位置（slot1 = bench 中心 + local(0,+45,0) = world(9376, 1675, 240) **在 bench 碰撞 mesh 内部**，bench bounds Y=1532–1727）。NavMesh 不能寻路到那里，Mover 反复微调导致"踱步"。改用 `UseSmartObjectWithGameplayInteraction`（不带外层 MoveTo），内层 `ST_SmartObject_Bench` 的 `StateTreeTask_FindSlotEntranceLocation` 用 `bProjectNavigationLocation=true` 把 entrance 投到 NavMesh 上的合法位置，再由 `StateTreeMoveToTask` 寻路过去。

**手动 `ReadyForActivation`**。GASP 范本里 `STT_UseSmartObject` 用 `K2Node_LatentGameplayTaskCall` 一站式包了 factory + ReadyForActivation + delegate 绑定。Monolith MCP 无法通过 `add_node` 给 `K2Node_LatentGameplayTaskCall` 绑定 proxy factory（生成空节点，title=`异步任务：缺失函数`；`copy_nodes` 拷贝过来的实例编译时被孤儿剪枝清掉）。退而求其次用普通 `K2Node_CallFunction` 调 `UseSmartObjectWithGameplayInteraction`（factory 只 NewObject，不 ReadyForActivation），再串一个 `K2Node_CallFunction(ReadyForActivation)` 把返回的 `UAITask_UseGameplayInteraction*` 通过 self pin 喂进去。

不动 `StateTreeAIComponent.StartLogic`（仍保持 `2026-04-28_npc_perception_sight.md` 里 disconnect 状态避免 NavMover 抢占）：`UAITask_UseGameplayInteraction` 由 BP 端按需 spawn，不依赖 StateTreeAI 启动状态。

## 资产改动

### 1. `Source/AILiveProject/AILiveProject.Build.cs`

新增模块依赖：

```csharp
"SmartObjectsModule",   // USmartObjectBlueprintFunctionLibrary, FSmartObjectClaimHandle
"GameplayTags",         // FGameplayTagQuery::FGameplayTagQuery() ctor 需链接（FSmartObjectRequestFilter 含其成员）
```

### 2. `Source/AILiveProject/Public/AILiveProjectPerceptionLogger.h` 新增 UFUNCTION

```cpp
#include "SmartObjectRuntime.h"  // FSmartObjectClaimHandle USTRUCT，UHT 需 visible

UFUNCTION(BlueprintCallable, Category = "AILive|SmartObject",
    meta = (WorldContext = "WorldContextObject",
        DisplayName = "Claim First Slot In Actor"))
static FSmartObjectClaimHandle ClaimFirstSlotInActor(
    AActor* SmartObjectActor,
    AActor* UserActor,
    UObject* WorldContextObject);
```

### 3. `Source/AILiveProject/Private/AILiveProjectPerceptionLogger.cpp` 实现

```cpp
#include "GameplayTagContainer.h"
#include "SmartObjectBlueprintFunctionLibrary.h"
#include "SmartObjectComponent.h"
#include "SmartObjectRequestTypes.h"
#include "SmartObjectTypes.h"

FSmartObjectClaimHandle UAILiveProjectPerceptionLogger::ClaimFirstSlotInActor(
    AActor* SmartObjectActor, AActor* UserActor, UObject* WorldContextObject)
{
    if (!SmartObjectActor) return FSmartObjectClaimHandle();

    TArray<FSmartObjectRequestResult> Results;
    FSmartObjectRequestFilter Filter;
    // Definition 的 UserTagFilter = ANY_EXACT(NPC, Player)，
    // Filter.UserTags 默认空 → 全过滤掉。主动加 NPC tag。
    Filter.UserTags.AddTag(FGameplayTag::RequestGameplayTag(
        FName(TEXT("SmartObject.ObjectType.NPC")), /*bErrorIfNotFound=*/false));

    if (!USmartObjectBlueprintFunctionLibrary::FindSmartObjectsInActor(
            Filter, SmartObjectActor, Results, UserActor) || Results.Num() == 0)
    {
        UE_LOG(LogAILivePerception, Warning,
            TEXT("[Sit] no available slot on %s"), *SmartObjectActor->GetName());
        return FSmartObjectClaimHandle();
    }
    return USmartObjectBlueprintFunctionLibrary::MarkSmartObjectSlotAsClaimed(
        WorldContextObject, Results[0].SlotHandle, UserActor,
        ESmartObjectClaimPriority::Normal);
}
```

实际代码包含 `[Sit] Filter prepared / SOComp / FindSmartObjectsInActor returned / claimed slot` 4 条 Display 诊断日志，便于 PIE 验证。无效返回默认构造句柄，BP 端 `IsValidSmartObjectClaimHandle` 检测后走失败分支。

### 4. `L_prison` Level BP 新增 1 变量 + K 键节点链

新增变量（category `NPCMove`）：
- `BenchClass: class:Actor` default `/Script/Engine.BlueprintGeneratedClass'/Game/Blueprints/SmartObjects/Bench/BP_SmartBench.BP_SmartBench_C'`

K 键节点链接到 `K2Node_IfThenElse_3.else`（N 键 Branch 的 else 出口）：

```
N_Branch.else
  → K_WasInputKey(WasInputKeyJustPressed Key="K", self←GetPlayerController)
    → K_Branch(IfThenElse Condition=ReturnValue)
        .then →
          GetActorOfClass(NPC1Class) → Cast<Pawn>
            .then →
              Pawn.GetController(pure, self=AsPawn)
              → Cast<AIController>(Object=Controller)
                .then →
                  GetActorOfClass(BenchClass) → BenchActor
                  → ClaimFirstSlotInActor(SmartObjectActor=BenchActor, UserActor=AsPawn) → ClaimHandle
                    → IsValidSmartObjectClaimHandle(Handle=ClaimHandle)
                      → ClaimBranch(IfThenElse Condition=ReturnValue)
                          .then →
                            UseSmartObjectWithGameplayInteraction(
                              Controller=AsAI控制器, ClaimHandle=ClaimHandle, bLockAILogic=true)
                            → ReadyForActivation(self=ReturnValue (UAITask*))
                            → PrintString("[Sit BP] UseSO+ReadyForActivation called (no MoveTo)")
```

CastFailed 分支不接（C++ helper 已有 UE_LOG 诊断）。AITask 的 `OnSucceeded`/`OnFailed`/`OnMoveToFailed` delegate 不绑定（本里程碑只验证目视坐下）。

**触发键 K（不是 S）**：原计划 S 键，但 GASP Mover 2.0 的 EnhancedInput WASD 默认占用 S 作后退键，按 S 既触发 sit 又向后走有视觉混淆。换 K，无既有占用，输入隔离干净。

## 关键 Pin 名（中文/英文混杂）

- `Cast To AIController` 输出 pin 是 **中文 `AsAI控制器`**（`2026-04-29_npc_perception_sight.md` 踩坑#1 已记），不是 `AsAIController`
- `Cast To Pawn` 输出 pin 是 **英文 `AsPawn`**
- `WasInputKeyJustPressed.Key` 是 `struct:Key` pin，`set_pin_default value="K"` 即可（不是 K2Node_InputKey 那种节点属性）
- `ClaimFirstSlotInActor` 节点的 `WorldContextObject` 因 `meta=(WorldContext=...)` 在 BP 上**自动隐藏**，BP 编译时注入 self
- `UseSmartObjectWithGameplayInteraction` 与 `ReadyForActivation` 都标 `BlueprintInternalUseOnly=true`，但 MCP `CallFunction + target_class` 直接 fetch 到，运行时正常

## MCP 实施踩坑

### 1. SO `Filter.UserTags` 必须主动满足 Definition 的 UserTagFilter

最初 helper 用 `const FSmartObjectRequestFilter Filter;`（默认空 UserTags），`FindSmartObjectsInActor` 一致返回 `bAny=false` / `Results.Num=0`，PIE 屏显 `no available slot`。

排查：读 `SmartObjectSubsystem.cpp:2667-2671` 的过滤逻辑，确认 `Definition.GetUserTagFilter()` 用 `Query.Matches(Filter.UserTags)` 校验，传入 `UserActor` 参数仅用于 world condition 求值上下文，**不会**自动从 actor 上拉 GameplayTagContainer 注入到 `Filter.UserTags`。

修：helper 内显式 `Filter.UserTags.AddTag(FGameplayTag::RequestGameplayTag(FName("SmartObject.ObjectType.NPC"), false))`。`bErrorIfNotFound=false` 防止 tag 未注册时刷 error。SO 项目级标签 `SmartObject.ObjectType.{NPC,Player,Bench,QueSmartObject}` 都已在 `DefaultGameplayTags.ini` 注册。

### 2. `MoveToAndUseSmartObjectWithGameplayInteraction` 的外层 MoveTo 走 slot 本体位置（不可寻路）

排查"NPC 走到附近但坐不下，原地踱步"症状时，读 `AITask_UseGameplayInteraction.cpp:118`：

```cpp
const TOptional<FVector> GoalLocation = SmartObjectSubsystem->GetSlotLocation(ClaimedHandle);
FAIMoveRequest MoveReq(GoalLocation.GetValue());
MoveReq.SetUsePathfinding(true);
MoveReq.SetAllowPartialPath(false);
```

**Goal = slot 本体位置**（slot1 world = `(9376, 1675, 240)`，bench bounds Y=1532–1727）→ **在 bench 碰撞 mesh 内部**。NavMesh 没有这点的可寻路区域，但因为 NPC 离 bench 很近时算出"近似"路径，`bAllowPartialPath=false` 又拒绝部分路径，Mover 一直在尝试 → 踱步。

修：改用 `UseSmartObjectWithGameplayInteraction`（无外层 MoveTo），让内层 `ST_SmartObject_Bench` 的 `StateTreeTask_FindSlotEntranceLocation` + `StateTreeMoveToTask` 处理，前者有 `bProjectNavigationLocation=true` 把 entrance 投到 NavMesh 上的合法位置（即使 annotation Offset 为 0）。

### 3. `K2Node_LatentGameplayTaskCall` 在 MCP 下不可用，必须手动 `ReadyForActivation`

GASP 范本 `STT_UseSmartObject` 用 `K2Node_LatentGameplayTaskCall` 节点：自动 NewObject + ReadyForActivation + 暴露 OnFinished/OnSucceeded/OnFailed/OnMoveToFailed delegate pin。

但 Monolith MCP `add_node` 创建该节点只能给空 fallback（title `异步任务：缺失函数`，没绑 proxy factory function），必要的元数据填不进去。`copy_nodes` 拷贝过来的同名节点在第一次 `compile_blueprint` 后被孤儿剪枝清掉（节点引用的内部 GUID 在新蓝图里不存在）。

退路：用普通 `K2Node_CallFunction` 调 `UseSmartObjectWithGameplayInteraction`，拿到 `UAITask_UseGameplayInteraction*` ReturnValue，再串一个 `K2Node_CallFunction(ReadyForActivation, target_class=GameplayTask)`，self pin 接收 task 引用。`ReadyForActivation` 也标 `BlueprintInternalUseOnly=true` 但同样 MCP 直接 fetch 通。

代价：不暴露 OnSucceeded/OnFailed delegate（要订阅得手动 `BindEventToOn...`）。本里程碑只验证目视坐下，无所谓。

### 4. Live Coding 不能链接新增的 module 依赖

`2026-04-28_npc_perception_sight.md` 踩坑#6 写过 5.7 + Monolith 0.14.7 下 Live Coding 能吃下新增 C++ 类。**本里程碑实测：新增 module 依赖（`SmartObjectsModule`、`GameplayTags`），Live Coding 报 `LNK2019` 失败**。

修：CLAUDE.md 既定规则——改 Build.cs 走 UBT 全量。Stop UE → `Build.bat AILiveProjectEditor Win64 Development -Project=...` → 重启 UE → 等 monolith 上线。本里程碑实测耗时约 35 秒（UBT 7 秒 + UE 启动 28 秒）。

### 5. `LNK2019: FGameplayTagQuery::FGameplayTagQuery(void)` 必须直接链 GameplayTags

只加 `SmartObjectsModule` 不够。`FSmartObjectRequestFilter` 默认构造内联触发其 `FGameplayTagQuery` 成员构造，而后者实现在 GameplayTags.dll 里。UBT 不传递 PublicDependencyModuleNames 第二层依赖，调用方必须自己显式链 `GameplayTags`。

### 6. Live Coding patch 在编辑器重启时丢失，UBT 全量才入盘

Live Coding patch 只改内存里的函数指针，不写磁盘 .dll。多次 Live Coding 累积的修改在编辑器进程退出时全部丢失，下次启动加载磁盘 .dll = 上次 UBT 的版本。

实测踩坑：tag 修复 + 诊断日志走 Live Coding，Editor 重启后 PIE 表现回退到"no available slot"且诊断日志不打印（disk .dll 是 tag 修复前的版本）。需要再跑一次 UBT 全量把当前源码烤进 .dll。

规则：每次"觉得稳了"的 C++ 改动，最后跑一次 UBT 全量定稿，避免后续 editor 重启回滚。

### 7. PIE 关闭后 BP 节点改动需立即 `compile_blueprint + save_asset`

PIE 在跑时 `save_asset` 会失败（`Failed to save asset`），但 `compile_blueprint` 走的是内存编译能 work（同 `2026-04-29_npc_perception_hearing.md` 踩坑#7）。流程：先 stop PIE → `compile_blueprint` → `save_asset`。若 editor 重启时 BP 未 save，graph 节点改动全部丢失，但 `add_variable` 加的变量保留（变量是 schema-level 修改，走另一持久化路径）。

### 8. `IsValidSmartObjectClaimHandle` 不是 BlueprintPure，必须串入 exec 链

`USmartObjectBlueprintFunctionLibrary::IsValidSmartObjectClaimHandle` 标 `BlueprintCallable` 不带 BlueprintPure，节点带 `execute/then` exec pin。必须 `Claim.then → IsValid.execute → IsValid.then → Branch.execute`，否则编译报 `已被修剪，因为其执行引脚未连接`。

### 9. `add_node` / `add_nodes_bulk` / `connect_pins_bulk` / `batch_execute` 参数名各异

| 操作 | param 名 |
|---|---|
| `add_node` 单调用 | `node_type`（短名，如 `CallFunction`，不带 `K2Node_` 前缀）+ 函数节点 `function_name` + `target_class` |
| `add_nodes_bulk` 数组项 | `node_type` + `function_name` + `target_class` + `position` |
| `connect_pins_bulk` 数组项 | **`source_node` / `source_pin` / `target_node` / `target_pin`**（不是 `src_node`/`dst_node`） |
| `batch_execute` op 项 | **`op`**（不是 `action`） + 各操作的 flat 参数 |
| DynamicCast 节点 | `cast_class`（不是 `target_class`） |
| 节点 ID 不能由调用方指定 | 由 MCP 自动生成 `K2Node_*_N`，需用 `nodes_created` 返回的 `node_id` 跟踪 |

## PIE 验证（已通过 2026-04-29）

按 K，Output Log：

```
LogAILivePerception: Display: [Sit] Filter prepared: NPCTag.IsValid=1 UserTags=(GameplayTags=((TagName="SmartObject.ObjectType.NPC"))) bShouldEvaluateConditions=1 bShouldIncludeClaimedSlots=0
LogAILivePerception: Display: [Sit]   SOComp on BP_SmartBench_C_1: RegisteredHandle.IsValid=1 Definition=SO_BenchDefinition
LogAILivePerception: Display: [Sit] FindSmartObjectsInActor returned bAny=1 Results.Num=2
LogAILivePerception: Display: [Sit] ClaimFirstSlotInActor: claimed slot on BP_SmartBench_C_1, valid=1
```

NPC1 由内层 `ST_SmartObject_Bench` 接管：FindSlotEntranceLocation 算 entrance 投到 NavMesh → StateTreeMoveToTask 寻路过去 → STT_PlayAnimFromBestCost 通过 Chooser ProxyTable 选 entry montage → AC_SmartObjectAnimation 在 owner SkeletalMesh 上播放 + MotionWarping 把 NPC 对齐到 slot → 站→坐姿态切换完成 → 停在 sit loop 姿态。

回归基线：M / I / N / O 既有功能不受影响（`StateTreeAIComponent.StartLogic` 仍保持 disconnect，AITask 由 BP 按需 spawn，不影响其他链路）。

## 已知限制 / 后续

- 本里程碑只验证"坐下"。`UAITask_UseGameplayInteraction` 完成后 slot 自动释放（`ST_SmartObject_Bench.ReleaseSlot` 末尾），但站起 montage 的显式触发未做。后续接"离开 bench"时调 `RequestAbort` 或绑定 `OnSucceeded` delegate（需要 `K2Node_LatentGameplayTaskCall` 暴露 delegate pin —— MCP 限制下要走 BindEventToOnSucceeded 节点链）
- Slot 选择策略固定取 `Results[0]`。Mind 模块按距离 / 朝向 / 已坐 NPC 关系自定义
- 多 NPC 同时申请同 bench 2 slot 的 conflict / claim priority 未验证
- 未接入 `UAILiveAgent` interface 过滤，SO 与感知系统正交
- ContextualAnim 走 `AC_SmartObjectAnimation` payload + Chooser `CHPT_SmartObject_Bench` 选 montage，**不是 UE5 ContextualAnim Plugin**（CHPT_ asset class=`ProxyTable` 而非 `ContextualAnimSceneAsset`，工程实际未启 ContextualAnim plugin）
- K 键是测试 hook，LLM Mind 模块接入后 Level BP 段全删，由 Mind 周期调 `ClaimFirstSlotInActor` + `UseSmartObjectWithGameplayInteraction` + `ReadyForActivation`。C++ wrapper（绕开 BP 节点链）需要：a) 引入 plugin Private include path 拿 `UAITask_UseGameplayInteraction` 类定义，或 b) UFunction 反射 + ProcessEvent。本里程碑不预先做。
