# NPC 移动 + 看向目标

日期：2026-04-28

## 目标

按 M 键 → 关卡内全部 `SandboxCharacter_Mover_C` 子类实例（含 8 个 MetaHuman NPC）各自走到 `BP_NavTarget` 实例位置 → 走路过程身体朝速度方向（自然行走）；到达后身体转向 `BP_NavLookTarget` 实例。LLM 决策层接入前的最小执行基线。

> **M 键是测试触发器**，不是产品入口。LLM 决策层接入后，此 Level BP 段直接删除，由 Mind 模块按 NPC 实例分别 dispatch `MoveAndLookAt`；功能本身（父类 `SandboxCharacter_Mover` 的函数 + 状态变量）零改动复用。

## 调试链路替换说明

上一里程碑（`2026-04-28_npc_movement_basic.md`）的 M 键链路是 8 NPC 各自走到一个硬编码 vector 散点，本里程碑**替换**为统一目标 + `GetAllActorsOfClass + ForEachLoop` 一段循环驱动全部 NPC。原 `NPC1Class..NPC8Class` Level BP 变量保留但已不引用，留作历史参考，可后续清理。

## 关键技术决策（必读）

GASP Mover 2.0 下 `SetActorRotation` 无效——Mover 每帧 sim 时按 `FCharacterDefaultInputs.OrientationIntent` 重写 Capsule rotation（UE 5.7 源码 `Engine/Plugins/Experimental/Mover/Source/Mover/Private/DefaultMovementSet/Modes/WalkingMode.cpp:54` 明确：`OrientationIntent` 非零时把它作为 IntendedOrientation_WorldSpace 写入 ProposedMove）。

正确做法是改 `OrientationIntent`。本工程父类 `SandboxCharacter_Mover` 的 BP 函数 `Get_OrientationIntent` 就是 Mover 输入产出器（注释 "To find where Mover takes inputs, open the 'Produce Input' function in the INTERFACES tab"），每帧返回 `FVector` 给 Mover。在该函数里加一条"到达后用我们算的方向"的覆盖即可。

## 资产改动

### 1. 新增 BP 标记类（全 MCP 创建）

- `Content/Blueprints/Markers/BP_NavTarget.uasset`
- `Content/Blueprints/Markers/BP_NavLookTarget.uasset`

两个都是 `AActor` 子类，含 `BillboardComponent`（编辑器可见图标）+ `StaticMeshComponent`（PIE 可见，Mesh 默认空）。

### 2. 关卡放置（手工，选项 A）

L_prison 关卡 Outliner 里有且仅有 `BP_NavTarget` × 1、`BP_NavLookTarget` × 1。

### 3. `SandboxCharacter_Mover` 新增 4 个实例变量

| 变量 | 类型 | 用途 |
|---|---|---|
| `CurrentLookTarget` | `object:Actor` (transient) | 存 LookTarget 引用，PollAndAlignLook 用来读位置 |
| `LookAtTimerHandle` | `struct:TimerHandle` (transient) | K2_SetTimer 的 handle（暂未实际使用，按名清 timer 即可） |
| `bAligningLook` | `bool` (transient) | 到达后置 true，触发 Get_OrientationIntent 切到 override 分支 |
| `LookDirection` | `struct:Vector` (transient) | PollAndAlignLook 算好的归一化方向，Get_OrientationIntent 直接读 |

全部 `category = AI`。

### 4. `SandboxCharacter_Mover` 新增 BP Function：`MoveAndLookAt`

签名：`MoveAndLookAt(MoveTarget: Actor, LookTarget: Actor) → bSucceeded: bool`，category 为 `AI`。

主路径：
```
FunctionEntry
  → Set bAligningLook = false                         # 复位（保证走路阶段不被 override 劫持）
  → DynamicCast<Actor>(MoveTarget)                    # None-check
    → DynamicCast<Actor>(LookTarget)                  # None-check
      → DynamicCast<AIController>(GetController)
        → Set CurrentLookTarget = LookTarget          # 存住 LookTarget 给 PollAndAlignLook 用
        → AIController.MoveToLocation(
            Dest = MoveTarget.K2_GetActorLocation,
            AcceptanceRadius = 80,
            bStopOnOverlap = true,
            bUsePathfinding = true,
            bProjectDestinationToNavigation = true,   # 关键：目标 Z=321 自动投到 NavMesh Z≈240
            bCanStrafe = false,                       # 关键：走路阶段身体朝速度方向
            bAllowPartialPath = false
          ) → MoveResult
        → KismetSystemLibrary.K2_SetTimer(
            Object = self,
            FunctionName = "PollAndAlignLook",
            Time = 0.1,
            bLooping = true,
            InitialStartDelay = 0.5                   # 等 NavMover 把 status 切成 Moving 再开始 poll
          )
        → bSucceeded = NotEqual_ByteByte(MoveResult, 0)
        → Return
```

失败路径（每条都 PrintString 后 Return false）：
- `cast_move.CastFailed` 或 `cast_look.CastFailed` → `[NPC] MoveAndLookAt: invalid Move/Look target actor`
- `cast_ai.CastFailed` → `[NPC] MoveAndLookAt: AIController missing (check AutoPossessAI)`

### 5. `SandboxCharacter_Mover` 新增 BP Function：`PollAndAlignLook`

签名：无入参无返回，category `AI`。被 K2_SetTimer 周期性调用。

主路径：
```
FunctionEntry
  → DynamicCast<Actor>(CurrentLookTarget)             # None-check
    → DynamicCast<AIController>(GetController)        # None-check
      → AIController.GetMoveStatus → status: byte
      → Branch (NotEqual_ByteByte(status, 0))         # 0 = EPathFollowingStatus::Idle
          ├─ true (still moving)  → Return            # 啥也不做，timer 继续轮询
          └─ false (arrived idle) →
              Set LookDirection = Normalize(
                  CurrentLookTarget.K2_GetActorLocation - Self.K2_GetActorLocation
              )
              → Set bAligningLook = true              # 触发 Get_OrientationIntent override
              → KismetSystemLibrary.K2_ClearTimer(self, "PollAndAlignLook")
              → Return
```

失败路径（早退 + 清 timer 防止死循环）：
- `cast_look.CastFailed` → ClearTimer + Return（CurrentLookTarget 失效，停止轮询）
- `cast_ai.CastFailed` → ClearTimer + Return（AIController 没了，停止轮询）

### 6. `SandboxCharacter_Mover` 修改既有函数：`Get_OrientationIntent`

在 GASP 原有的 "Walking + idle + (OrientToMovement|Strafe)" 分支末端的 `K2Node_FunctionResult_16` 之前插入一个 `K2Node_Select`：

```
K2Node_Select:
  Index    = VariableGet bAligningLook
  Option 0 = BreakStruct(MoverDefaultInputs_PreSim).OrientationIntent  # 原 sticky 行为
  Option 1 = VariableGet LookDirection                                 # 我们的方向
  ReturnValue → K2Node_FunctionResult_16.ReturnValue
```

- `bAligningLook = false`（走路或未启动）→ 走 Option 0，沿用 GASP 原 sticky 朝向行为
- `bAligningLook = true`（已到达）→ 走 Option 1，返回 `LookDirection`，Mover 每帧按这个方向以 RotationRate 平滑旋转 Capsule

只动 `FunctionResult_16` 这一条出口（Walking + idle + OrientToMovement/Strafe 模式），其它 movement mode（Falling/Sliding/Traversing/Aim）全部不动。

### 7. `L_prison` Level BP 重写

新加 3 个 `class:Actor` 变量：
- `NavTargetClass`，default = `/Script/Engine.BlueprintGeneratedClass'/Game/Blueprints/Markers/BP_NavTarget.BP_NavTarget_C'`
- `NavLookTargetClass`，default 同样指向 `BP_NavLookTarget_C`
- `MoverNPCClass`，default 指向 `/Game/Blueprints/SandboxCharacter_Mover.SandboxCharacter_Mover_C`（用作 `GetAllActorsOfClass` 的过滤类）

`EventGraph` 删掉 32 个旧硬编码 vector 散点节点，保留 `Tick + GetPlayerController + WasInputKeyJustPressed("M") + Branch`。`NPC1Class..NPC8Class` 变量保留但不再引用（历史包袱，下次清理）。

新链路（Branch.then 之后）：
```
GetActorOfClass(NavTargetClass)        → MoveActor: Actor
GetActorOfClass(NavLookTargetClass)    → LookActor: Actor
GetAllActorsOfClass(MoverNPCClass)     → OutActors: Array<Actor>
ForEachLoop(OutActors)
  LoopBody → DynamicCast<SandboxCharacter_Mover_C>(ArrayElement)
              ├─ CastFailed → (skip)
              └─ then → MoveAndLookAt(self, MoveActor, LookActor)
  Completed → end
```

Nav 目标的 None-check 不在 Level BP 里加，统一由 `MoveAndLookAt` 内部处理。Cast 失败的 ArrayElement（理论上不会发生，因为 GetAllActorsOfClass 已按类筛过）也直接 skip。

> 副作用：玩家 Pawn 如果也是 `SandboxCharacter_Mover_C` 的直接实例（非 NPC 子类），会被 `GetAllActorsOfClass` 抓到一起执行 `MoveAndLookAt`。当前 PRD 未要求排除玩家；如需要，加一个 `Cast<BP_NPC_MH_Character_*>` 或 `if AIController != null` 过滤。

## MCP 实施踩坑

1. `create_blueprint` 的参数是 `save_path`，不是 `asset_path`。
2. `resolve_node` 对 `K2_SetFocus` / `MoveToLocation` / 自定义函数等会返回 `pin_count: 0`。`add_node` 实际放入图后 `get_node_details` 才能拿到全部 pin。流程：用 add_node + get_node_details 校验 pin 名，再 connect_pins。
3. `IsValid` 不论 `target_class` 怎么传，都解析到 `SubobjectDataBlueprintFunctionLibrary.IsValid`（auto-memory 已记）。绕法：用 `DynamicCast<Actor>` 当 None-check（cast on null 走 CastFailed 分支，等价 IsValid）。
4. **新增：`add_function` 创建函数后，多 `K2Node_FunctionResult` 节点在 compile 后仍可能没有输出 pin（`bSucceeded` / `ReturnValue`），即便 `set_function_params` 已声明返回值**。绕法：用 `K2Node_Select` 数据合流到 GASP 已有的、带正确 `ReturnValue` 的 `FunctionResult` 节点，避开新建 result 节点。
5. **新增：`K2Node_Select` 的 wildcard 类型在 connect 时按"先连接的那个 pin 的类型"锁定**。如果先把 `Self` 节点（Pawn-ref）连到 Option 0，然后想把 `Actor` 连到 Option 1 会报"Actor 对象引用和Self 对象引用不兼容"。解决：要么把所有 Option 都连同一类型源，要么用变量中转（本里程碑用 `LookDirection` Vector 变量绕过这个坑）。
6. **新增：MCP `connect_pins` 不会触发 wildcard pin 的类型推断**。`K2Node_Array_Get` / `K2Node_Select` 这类 wildcard 节点必须先有一端是确定类型且能连上，再连其它端。`Array_Get` 在 MCP 下基本不可用（DevLog `2026-04-28_npc_movement_basic.md` 已记），本里程碑用 `GetActorOfClass` + 专用 BP 类回避。**例外**：`K2Node_MacroInstance("ForEachLoop")` 的 `Array`（input wildcard）和 `Array Element`（output wildcard）pin 在 Monolith 0.14.7 下能正常连——`GetAllActorsOfClass.OutActors → ForEachLoop.Array → ForEachLoop.Array Element → DynamicCast.Object` 整条 wildcard 链 `connect_pins_bulk` 一次成功。看起来 MCP 对 ForEachLoop 这种特殊 macro 的 wildcard 推断有专门处理，但 `Array_Get` 仍不行。
7. **新增：`class:Actor` 变量的 `default_value` 写法**：`/Script/Engine.BlueprintGeneratedClass'/Game/.../BP_xxx.BP_xxx_C'`（外层路径 + 内层引号 + `_C` 后缀，外层不加引号）。
8. `batch_execute` + `add_nodes_bulk` + `connect_pins_bulk` + `set_pin_defaults_bulk` 一次 round-trip 完成大改，比逐节点操作高效得多。

## PIE 验证结果

PIE 实测（2026-04-28）：

- [x] 按 M：关卡内全部 8 个 NPC 同时启动并走到 BP_NavTarget。`GetAllActorsOfClass + ForEachLoop` 一段循环驱动全员，无个体掉队。
- [x] 走路过程身体朝速度方向（NavMover.MoveInput 通过 `Get_MoveInput` 写入 `FCharacterDefaultInputs.MoveInput`，Mover 在 OrientToMovement 模式下用速度方向作为 OrientationIntent）。
- [x] 到达 BP_NavTarget 80uu 内：身体 yaw 主动转向 BP_NavLookTarget。`Get_OrientationIntent` 的 Select 切到 `LookDirection`，Mover 每帧按 RotationRate 平滑旋转。
- [x] OutputLog 无 `LogAIController` / `LogPathFollowing` 异常。
- [x] 失败路径：`MoveAndLookAt` 三条 PrintString 在故意断关卡引用时正确触发。

诊断 PrintString（`[Poll] fired` / `[Poll] arrived idle, aligning yaw`）已在验证通过后从 `PollAndAlignLook` 中移除，避免 8 NPC 并行轮询时日志刷屏；`MoveAndLookAt` 失败诊断 PrintString 保留。

## 已知限制 / 后续

- 8 NPC 同点拥挤（都走到 BP_NavTarget 同一位置）：当前依赖 CharacterMover 自带物理碰撞避让，视觉上会有轻微挤压但不卡死。如要平滑分散，下一里程碑加 `ai_query::set_crowd_manager_config` 启用 DetourCrowdManager。
- 玩家 Pawn 如果是 `SandboxCharacter_Mover_C` 直接实例，会被 `GetAllActorsOfClass` 抓到一起执行。当前 PRD 不要求排除；如需要，在 ForEachLoop 内换成 Cast 到 `BP_NPC_MH_Character_*` 父类（如有）或加 `if AIController != null` 过滤。
- `NPC1Class..NPC8Class` 这 8 个 Level BP 变量已不引用，留作历史包袱，下一里程碑随手清理。
- `BP_NavTarget` / `BP_NavLookTarget` 的 `Visual` 静态网格组件未指定 Mesh，PIE 默认不可见（不影响寻路）。如需可视化标识可在两个 BP 的 Visual 组件 Details 里配 `EngineSky/SM_Sphere` 或类似 Engine 自带网格。
- 旋转过程是 Mover 的 RotationRate 决定的（GASP 默认值），目前肉眼观感 OK；如要更快/更慢转向，可调 CharacterMover 的 RotationRate 设置或 `Update_ControlRotationRate` 函数。
- 当前 `Get_OrientationIntent` 的覆盖只作用在 Walking + idle + OrientToMovement/Strafe 这一条出口。若 NPC 在其它 movement mode（Falling/Sliding/Traversing/Aim）下需要 look-at，要分别处理。本里程碑场景内 NPC 全程 Walking，不涉及。
- M 键触发是测试 hook，未来 LLM 决策层接入时该段 Level BP 直接删，由 Mind 模块按 NPC dispatch；功能本身（父类的 `MoveAndLookAt` 函数 + 状态变量 + `Get_OrientationIntent` 覆盖）零改动复用。
