# NPC 视野感知（AI Perception + C++ Logger）

日期：2026-04-28

## 目标

NPC1 走进 L_prison 大厅时，**精确感知大厅内同向同排可见的其他 NPC**（具体集合由 sight radius=2000uu 与关卡几何决定：NPC1 在 row1 Y=5837，朝向 -Y；row1 同排 NPC2/NPC3/NPC4/NPC9 全在 sight radius 内，row2 Y=3586 距离 ≈2251uu 接近 sight 边缘视情形定），**不感知套间内被墙体遮挡的 NPC**。结果输出"身份/距离/方向/视线状态"，作为后续 LLM Mind 模块的视野输入数据契约的最小可行版本。

## 技术决策

**感知挂 AIController，不挂 Pawn**。`UAIPerceptionComponent` 加在 `AIC_NPC_SmartObject`；`UAIStimuliSourceComponent` 加在 `SandboxCharacter_Mover`。把 perception 配在 Pawn 上能编译过但 AISystem 不会自动接管，会出现"配了但无感知"的静默失败（unreal-ai skill 第 8 条）。

**复用 `AIC_NPC_SmartObject` 作为统一 NPC AIController**。原本 `SandboxCharacter_Mover` 的 `AIControllerClass` 是引擎默认 `/Script/AIModule.AIController`（无感知）。本里程碑切到 `AIC_NPC_SmartObject_C` —— 未来 LLM 决策 / SmartObject / 感知都集中在这一个控制器，不分裂。

**`UAILiveProjectPerceptionLogger` 用 `UBlueprintFunctionLibrary` 而非 ActorComponent**。无状态、无 Tick、纯查询；按需 BP 调用，未来 Mind 模块 C++ 直接静态调用同一函数取数据契约。

## 资产改动

### 1. `Source/AILiveProject/AILiveProject.Build.cs`

`PublicDependencyModuleNames` 加：

```csharp
"AIModule",          // AAIController, UAIPerceptionComponent, FAIStimulus, UAISense_Sight
"NavigationSystem",  // 显式声明（AIModule 已传递依赖）
```

改 Build.cs 后通过 `editor_query::trigger_build` 触发 Live Coding 全量重建，`patch_applied=true, errors=0` 通过。

### 2. 新增 C++ 类 `UAILiveProjectPerceptionLogger`

文件：
- `Source/AILiveProject/Public/AILiveProjectPerceptionLogger.h`
- `Source/AILiveProject/Private/AILiveProjectPerceptionLogger.cpp`

```cpp
USTRUCT FPerceivedAgentInfo {
    FName Identity;                          // 类名去掉 _C 后缀（BP_NPC_MH_Character_2）
    float DistanceCm;                        // (Target - Perceiver).Size()
    FRotator DirectionFromPerceiver;         // (Target - Perceiver).Rotation()
    bool bCurrentlySensed;                   // FAIStimulus.WasSuccessfullySensed()
    float StimulusAge;                       // FAIStimulus.GetAge()
    FVector LastStimulusLocation;            // FAIStimulus.StimulusLocation
};

static TArray<FPerceivedAgentInfo> GatherSightPerception(AAIController*, UObject* WorldCtx);
static int32 LogPerceptionToOutput(AAIController*, FString Tag, UObject* WorldCtx);
```

实现关键点：

- `PC->GetCurrentlyPerceivedActors(UAISense_Sight::StaticClass(), Known)` 仅取 Sight 已感知列表（不含已遗忘的）。
- `FActorPerceptionBlueprintInfo.LastSensedStimuli` 是**多种 sense 各一条**的列表，要按 `S.Type == UAISense::GetSenseID<UAISense_Sight>()` 过滤，否则会拿到 Touch / Damage 的过期记录。
- 主动过滤 `A == PerceiverPawn` 防自感知（AISystem 默认会过滤但保险一道）。
- `LogPerceptionToOutput` 同时走 `UE_LOG(LogAILivePerception)` + `UKismetSystemLibrary::PrintString`，PIE 屏幕和 Output Log 两边都看得到。

### 3. `AIC_NPC_SmartObject` 加 `AIPerceptionComponent` + Sight Sense

通过 Monolith MCP（不动 EventGraph，原 `StateTreeAI.StartLogic` 链路保留）：

```
ai_query::add_perception_component  asset_path=/Game/Blueprints/AI/AIC_NPC_SmartObject  dominant_sense=Sight
ai_query::configure_sight_sense
  asset_path=/Game/Blueprints/AI/AIC_NPC_SmartObject
  radius=2000, lose_radius=2200, peripheral_angle=90
  affiliation={ enemies: true, neutrals: true, friendlies: true }
  max_age=5.0
```

参数选择：

- `radius=2000`：DevLog `2026-04-28_npc_movement_basic.md` 已知大厅路径长度 3000~3200uu，2000uu 够覆盖大厅同排 NPC 的一圈，又不会"穿透"覆盖到套间外更远位置（其实墙挡了也无所谓）。
- `peripheral_angle=90`：FOV 半角 90 → 全 FOV 180°，NPC1 进门朝向时把可见 5 NPC 都包进，避免"视野太窄漏看"。
- `affiliation` 全 true：本里程碑不区分阵营。Team 系统接入后再收紧。

`validate_perception_setup` → `valid: true, sense_count: 1`，`Team ID is 255 (NoTeam)` 提示按计划忽略。

### 4. `SandboxCharacter_Mover` 加 StimuliSource + 切 AIController

```
ai_query::add_stimuli_source_component
  asset_path=/Game/Blueprints/SandboxCharacter_Mover
  register_as_source_for=["Sight"]

blueprint_query::set_cdo_property
  asset_path=/Game/Blueprints/SandboxCharacter_Mover
  property_name=AIControllerClass
  value=/Script/Engine.BlueprintGeneratedClass'/Game/Blueprints/AI/AIC_NPC_SmartObject.AIC_NPC_SmartObject_C'
```

`add_stimuli_source_component` 必须挂被感知方（CLAUDE.md / unreal-ai skill 第 4 条）；挂在 AIController 上没用。所以这条加在 `SandboxCharacter_Mover`（pawn 父类），10 个 NPC 子类全部继承，无需各自配置。

`AIControllerClass` 走 CDO 写，全路径 + `_C` 后缀（DevLog `2026-04-28_npc_move_and_focus.md` 第 7 节坑已记）。

### 5. `L_prison` Level BP 加 I 键查询触发

在 `EventTick.then` 上插一个 `Sequence`，把原 M 键链放到 `then_0`，新 I 键链放到 `then_1`：

```
EventTick
  → Sequence
      ├─ then_0 → IfThenElse(M 键 Branch)        # 原链路保持
      └─ then_1 → IfThenElse(I 键 Branch)
                    .then → GetActorOfClass(NPC1Class)
                              → DynamicCast<Pawn>
                                .then → DynamicCast<AIController>
                                          ├─ Object ← Pawn.GetController()  # pure
                                          └─ then → LogPerceptionToOutput(Tag="NPC1")
```

`WasInputKeyJustPressed.self` 复用现有 `K2Node_CallFunction_0`（GetPlayerController）输出，不重复创建。

## MCP 实施踩坑

1. **DynamicCast 的输出 pin 名是中文** `AsAI控制器`（`Cast To AIController` 节点）。不是英文 `AsAIController`。`connect_pins` 的 pin 名匹配虽是 case-insensitive，但中文/英文不同字面，必须先 `get_node_details` 拿到实际 pin 名再连。`Cast To Pawn` 输出是英文 `AsPawn`，看 K2Node 源码不一致。
2. **`ExecutionSequence` 的输出 pin 命名是 `then_0` / `then_1`**（小写下划线），不是 `Then 0` / `Then0`。
3. **`disconnect_pins` 只需指定一端的 node_id + pin_name**，会自动断开它所有连接。本里程碑断 `EventTick.then`（原本仅连 M 键 Branch），插 Sequence 后再各自接回，干净。
4. **`set_cdo_property` 写 `class:` 类型属性**：value 用 `/Script/Engine.BlueprintGeneratedClass'/Game/.../<BPName>.<BPName>_C'`（外层路径 + 内层引号 + `_C` 后缀，外层不加引号）。
5. **`UFUNCTION` 带 `meta=(WorldContext="WorldContextObject")` 的 BP 函数**，`WorldContextObject` 参数在 BP 节点上**不显示为 pin**（自动隐藏，BP 调用时编辑器自动注入 self）。`resolve_node` 返回 5 个 pin（execute/then/Perceiver/Tag/ReturnValue），没有 WorldContextObject——这是预期行为。
6. **Live Coding 处理新增 C++ 类 + 新 module 依赖**：UE 5.7 + 编辑器运行时调用 `editor_query::trigger_build` 直接走 Live Coding patch，`patch_applied=true, errors=0` 完成，无需关闭编辑器跑 UBT 全量重建。CLAUDE.md 的"改 Build.cs 必须全量重建"规则在 5.7 + Monolith 0.14.7 下可放宽。

## PIE 验证（已通过 2026-04-29）

触发：M 键启动 NPC1 走入大厅（沿用 `MoveAndLookAt`） → NPC1 站定后按 I 键（P 键被全局占用，本里程碑实际用 I）。

| 检查项 | 期望 | 实测 |
|---|---|---|
| 大厅可见 NPC 数 | 5（NPC2–NPC6） | **5 ✓** |
| 套间被遮挡 NPC（NPC7/8） | 不出现 | **未出现 ✓** |
| 距离/方向/视线状态 | 完整输出 | dist 479-906 / relYaw 合理 / age=0.00 ✓ |
| 多余条目 | 0 | **0 ✓** |

## 实施过程踩坑（除最初规划外的额外发现）

### 1. StateTree 自启动抢占 NavMover

`AIC_NPC_SmartObject` 的 EventGraph BeginPlay 链路：

```
ReceiveBeginPlay → Branch(IsDedicatedServer) → Delay → StateTreeAI.StartLogic
```

切到这个 Controller 后，所有 NPC 一进 PIE 就开始按 StateTree 巡逻，**M 键的 `MoveAndLookAt` 失效**——StateTree 的 Tasks 把 NavMover 输入抢走了。

修法：通过 `disconnect_pins K2Node_Event_0.then` 断开 BeginPlay → Branch 链路，加橙色 `add_comment_node` 标记 DISABLED。`StateTreeAIComponent.start_logic_on_possess=false` 已经是默认（不会自动启），问题仅在于 EventGraph 手动调了 StartLogic。

LLM 决策层接入后：
- 选项 A：彻底删 `StateTreeAIComponent`，由 LLM 直接 dispatch `MoveAndLookAt` / 其它行为。
- 选项 B：保留 StateTree 但精确控制启动时机（按需 dispatch，而不是 BeginPlay 自动启）。

### 2. 子类 BP CDO 不会自动跟随父类 CDO

父类 `SandboxCharacter_Mover.AIControllerClass` 改成 `AIC_NPC_SmartObject_C` 后，子类 `BP_NPC_MH_Character_1..10` 的 CDO **仍然是 `/Script/AIModule.AIController`**（引擎默认）。

根因：UE 反射继承在子类 BP **首次保存时**会把当前所有 inherited property 的值序列化进子类 .uasset 自己的 CDO。之后父类改了，子类 CDO 是旧值，除非显式重新保存或 reset to default。

修法：用 `set_cdo_property` 把 10 个子类 BP 的 `AIControllerClass` 全改一遍，再 compile + save。

诊断方式：`get_cdo_properties` 看子类 CDO，对比父类 CDO 是否一致。

### 3. 关卡 external actor 实例级 override 优先于 CDO

子类 CDO 都改完后，PIE 里 NPC1 的实际 controller 仍然是 `AIController`。`mesh_query::get_actor_properties` 显示关卡实例 `BP_NPC_MH_Character_1_C_1.AIControllerClass = /Script/AIModule.AIController`——**实例级 override** 把改完的 CDO 又压回去了。

L_prison 是 OFPA / external-actor 关卡，每个 actor 实例存在 `__ExternalActors__/.../*.uasset`，实例 override 会保留在那里。

修法（MCP 限制下走 fallback）：用户在 Editor outliner 选 10 个 NPC → Details 面板 AI Controller Class 字段右键 → "Reset to Default" → 保存关卡。

**MCP `set_actor_properties` 6 字段限制**仅支持 mobility/simulate_physics/collision_preset/cast_shadow/tags/mass_kg，无法自动 reset Pawn 类的 AIControllerClass。

避免再次踩坑：未来切控制器一律走"父类 CDO + 子类 reparent / 子类 CDO + 关卡实例 reset"全套，不能省任一步。

### 4. 玩家 Pawn 被 StimuliSource 暴露

PRD 不区分玩家与 AI（全是 AI 博弈），但开发期 PIE 中需要玩家 Pawn 当观察相机，玩家也是 `SandboxCharacter_Mover_C` 实例，继承父类的 `AIPerceptionStimuliSourceComponent`，因此会产生 Sight stimulus。

修法：新增 `IAILiveAgent` marker interface，只让 10 个 `BP_NPC_MH_Character_*` 子类 BP 实现该接口。`GatherSightPerception` 在当前 Sight 感知结果上继续按 `UAILiveAgent` 过滤，只输出真正的 agent；玩家 Pawn 仍可被底层 AISystem 看到，但不会进入 LLM 输入数据。

## 已知限制 / 后续

- 仅 Sight sense。Hearing / Damage / Team / Prediction sense 未接入。
- Affiliation 全 true，未区分阵营。PRD"联盟/背叛"博弈接入需要 `scaffold_team_system` + 配 Attitude Solver。
- `LogPerceptionToOutput` 输出格式是人读字符串。LLM Mind 模块接入需要 `ToJsonString`（下一个里程碑）。
- I 键触发是测试 hook。LLM 决策层接入后该段 Level BP 删除，由 Mind 模块按 NPC 周期调 `UAILiveProjectPerceptionLogger::GatherSightPerception` 拿数据。
- 视野参数（radius=2000 / peripheral=90）按 L_prison 大厅尺寸调，其它关卡需要重配。
- NPC1 走入过程的"边走边看"未做：当前只在站定后按 I 拍快照。如要持续监测，把 I 键改成 BeginPlay 启 K2_SetTimer 周期调 LogPerceptionToOutput。
