# NPC 移动基础能力

日期：2026-04-28

## 目标

让 10 个 MetaHuman NPC（`BP_NPC_MH_Character_1..10`）在 PIE 中按 M 键后移动到指定目标点。

## 技术选型

**蓝图实现** `MoveToLocation` 函数挂在父类 `SandboxCharacter_Mover`，子类自动继承。理由：`AIController::MoveToLocation` 的核心在引擎 C++（PathFollowing → NavMover → CharacterMover），蓝图调用无性能差；与既有 BP 父类架构一致；Live Coding 免重建，调参成本低。

## 实现概览

### SandboxCharacter_Mover 新增 Function

`MoveToLocation(TargetLocation: FVector, AcceptanceRadius: float=80, bAllowPartialPath: bool=false) → (bSucceeded: bool, MoveResult: byte)`

内部：`GetController → Cast<AIController> → AIController::MoveToLocation`
成功语义：`MoveResult != 0`（即 `RequestSuccessful` 或 `AlreadyAtGoal`，排除 `Failed`）
参数：`bProjectDestinationToNavigation=true`（目标 Z=321 自动投影到 NavMesh Z≈240）

### L_prison Level BP 触发图

`EventTick → WasInputKeyJustPressed(M) → Branch → GetActorOfClass(NPCi_Class) → Cast → MoveToLocation(target_i, 80, false)` × 10 串联

触发方式：PIE 中焦点在 viewport，按 M 键。

## 寻路验证（离线）

- NavMesh `is_built=true`
- 10 个目标点全部 `project_point_to_navigation` 成功，投影到地板级 NavMesh
- 抽样 find_path：`success=true, is_partial=false`，路径约 3000~3200 uu

## MCP 实现要点

### TSubclassOf class pin 类引用

`GetActorOfClass.ActorClass` 是 `TSubclassOf<AActor>` pin。`set_pin_default` 接受任何格式（全路径、`Class'...'`、短类名）但编译时全部报 `String NewDefaultValue '...' specified on class pin 'ActorClass'`，UE 无法从字符串解析 BP 类引用。

**当前方案**：Level BP 变量（`NPC1Class..NPC10Class: TSubclassOf<Actor>`，default_value 设全路径，存在变量 CDO 里）→ `VariableGet` 节点连接到 `ActorClass` pin。pin 有连接时编译器忽略 pin default，变量的 CDO 值在运行时正确解析为类引用。

### Array wildcard 连接限制

`GetAllActorsOfClass.OutActors`（`array:object:Actor`）→ `Array_Get.TargetArray`（`array:wildcard`）：`connect_pins` 报 "仅可以转换对象/接口的类型"，类型推断不触发。当前实现使用 `GetActorOfClass` 直接返回 `Actor*`，绕过数组。

### GetActorOfClass 优于 GetAllActorsOfClass + Array_Get

对每个 NPC 类唯一实例使用 `GetActorOfClass`（非数组，单 Actor 出口），无 wildcard 问题，代码也更简洁。

### DynamicCast / CallFunction target_class 需 `_C` 后缀

BP 生成类的短名必须带 `_C` 后缀（如 `SandboxCharacter_Mover_C`），不加则 MCP 报 "Class not found"。

### InputKey M 键节点无法通过 MCP 配置 FKey

`K2Node_InputKey` 的 `InputKey` (FKey) 是节点属性非 pin，`set_pin_default` 无效。绕过方案：Level BP `EventTick` + `WasInputKeyJustPressed(Key="M")`（FKey pin 接受字符串 "M"）。

## PIE 验证结果（2026-04-28）

- [x] WasInputKeyJustPressed FKey 字符串 "M" 正确传入，触发链路正常
- [x] AIController 自动 possess 成功（AutoPossessAI=PlacedInWorld 在 PlacedInWorld 时生效）
- [x] NavMover + CharacterMover 驱动移动：**GASP Mover 2.0 与标准 AIController.MoveToLocation 原生兼容**，无需额外配置
- [x] 10 NPC 同时走到各自目标位置，路径正确
- [ ] CrowdManager 互相避让未测（首版不做，路径可能有轻微重叠，待需要时加 set_crowd_manager_config）

触发键：**M 键**。原占用于 PC_Sandbox.IA_NextVisualOverride 皮肤切换，已断开该连线（移除皮肤切换功能），M 键现专用于 NPC 移动测试。

## 运行时链路

**CastFailed 旁路**：`cast_i.CastFailed → gac_{i+1}.execute`（i=1..9）。任意 NPC 找不到时不会中断整条链，后续 NPC 仍会继续发起移动。

## 已知限制

Level BP 未加入 PrintString/Log，cast 失败或 MoveResult=Failed 时不会在屏幕或日志中提示。后续需要排查移动失败时，在 CastFailed 和 MoveToLocation 返回后加 PrintString 日志。
