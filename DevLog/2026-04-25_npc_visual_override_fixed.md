# 2026-04-25 · 场景 NPC 固定视觉 VisualOverride + 玩家回退 DefaultPawn

## Prompt

```
当前场景中有两个 MH_Character_1， 左侧 T Pose 的是原来的，我们称之为NPC，右侧Idle 站立的是你刚创建的metahuman。
- 左侧NPC不支持动作和TTS Audio2face;
- 右侧的支持移动和TTS audio2face；

我现在需要让左侧NPC 支持移动和TTS audio2face，
而右侧PlayerStart 还原到原来看不到状态
```

后来澄清："无需考虑如何控制，后期由代码驱动，只要有GASP Mover的功能+audio2face+tts 的功能，也就是说把 当前右侧metahuman的功能除了控制之外的功能都迁移到左侧的metahuman上"

## 功能描述

把前一轮做的"玩家 VisualOverride 走 GASP"的能力，搬到**场景 NPC 实例**上。具体：

- 玩家 Pawn 回到 **DefaultPawn（飞行摄像头，不可见）**
- 场景里原来的 `BP_MH_Character_1_C_1` 实例换成 `BP_NPC_MH_Character_1`（`SandboxCharacter_Mover` 子类），固定 VisualOverride = `BP_MH_Character_1_C`
- NPC 现在是**可被代码驱动的 Pawn + MetaHuman 视觉**，具备完整 GASP Mover 能力 + A2F + MiniMax TTS
- 按 T 键或外部代码调 `TriggerMinimaxSpeech(ChildActor, ...)` 都能让 NPC 说话 + 面部口型

**交付物**：

| 类型            | 路径                                                   | 改动                                                                                                                                                                        |
| --------------- | ------------------------------------------------------ | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| 组件 BP（改）   | `Content/Blueprints/AC_VisualOverrideManager.uasset`   | 加变量 `FixedVisualOverride` + 改 `FindAndApplyVisualOverride` 前置分支 + 加公共函数 `SetFixedAndApply`                                                                     |
| 角色 BP（新建） | `Content/Blueprints/NPCs/BP_NPC_MH_Character_1.uasset` | 继承 `SandboxCharacter_Mover`；加 `FixedVisualOverrideClass` 默认 `BP_MH_Character_1_C`；BeginPlay 调 `AC_VisualOverrideManager.SetFixedAndApply(FixedVisualOverrideClass)` |
| 关卡（改）      | `Content/MyAssets/L_prison.umap`                       | 删 `BP_MH_Character_1_C_1`，原位 (-13,378,3) spawn `BP_NPC_MH_Character_1`（label=`NPC_MH_Character_1`）                                                                    |
| 引擎配置（改）  | `Config/DefaultEngine.ini`                             | 删 `GlobalDefaultGameMode` 行，回退默认 GameMode                                                                                                                            |

---

## 实现逻辑

### 预检发现（为什么简单路走不通）

看 `AC_VisualOverrideManager` 的运行流程，关键路径：

```
BeginPlay
  → FindAndApplyVisualOverride:
      Cast GameMode → GM_Sandbox →(失败 early return)
      读 GM_Sandbox.VisualOverrides[DDCvar.VisualOverride]
      (CVar 默认 -1 → 返回 None)
      Set AC 自身 VisualOverride 变量 = None
      → OnRep_VisualOverride → ApplyVisualOverride:
          GetComponentsByTag(ChildActorComponent, "VisualOverride")
          → foreach: SetChildActorClass(child, VisualOverride)
          → SetVisibility(UEFN_Mesh, NOT IsValidClass(VisualOverride))
  → 订阅 OnDataDrivenCVarChanged → 再跑 FindAndApplyVisualOverride
```

**核心障碍**：

- 即使把 NPC 实例上 `VisualOverride` ChildActorComponent 的 `ChildActorClass` 直接设为 `BP_MH_Character_1_C`，`ApplyVisualOverride` 会在 BeginPlay 被 CVar=-1 覆写回 None
- AC_VisualOverrideManager 的 `VisualOverride` 变量虽然 `instance_editable=true`，但同样会被 BeginPlay 覆盖
- **所以要么改 AC_VisualOverrideManager 的逻辑，要么禁用它**

### 方案决策：加 per-instance fallback 变量

选方案：在 `AC_VisualOverrideManager` 加一个新变量 `FixedVisualOverride`（instance_editable），让 `FindAndApplyVisualOverride` 头部优先走这条路径。好处：

- 玩家 Pawn 实例的 `FixedVisualOverride = None`（默认），走原 CVar + GM_Sandbox 逻辑 —— 行为不变
- NPC 实例设 `FixedVisualOverride = BP_MH_Character_1_C`，固定视觉，不受 CVar 污染
- 不破坏原有机制

### AC_VisualOverrideManager 具体改动

1. **加变量** `FixedVisualOverride : TSubclassOf<AActor>`（category=Override, instance_editable=true）
2. **改 `FindAndApplyVisualOverride`**：
   - 原：FunctionEntry → Cast GM_Sandbox → ...
   - 新：FunctionEntry → Branch(IsValidClass(FixedVisualOverride))
     - True: Set VisualOverride = FixedVisualOverride → ApplyVisualOverride → done
     - False: Cast GM_Sandbox → ...（原逻辑）
3. **加公共函数 `SetFixedAndApply(FixedClass : class:Actor)`**：
   - Set FixedVisualOverride = FixedClass → Call FindAndApplyVisualOverride
   - 作用：子类 NPC 的 BeginPlay 可用这个函数一次性设值+应用

### 为什么不在子类 CDO 层直接设 AC_VisualOverrideManager.FixedVisualOverride

试过几条路：

- `set_component_property` on subclass BP → 报 "Component not found"（MCP 不显示继承组件）
- `set_cdo_property("AC_VisualOverrideManager.FixedVisualOverride", ...)` → 报 "Property not found"（点路径不支持）

结论：**通过 MCP 无法在子类 BP 的 CDO 层直接覆盖继承组件的 property 默认值**。只能走运行时路径：在子类 BeginPlay 里调 setter。这就是为什么要加 `SetFixedAndApply` 函数。

### BP_NPC_MH_Character_1 结构

- 继承 `SandboxCharacter_Mover`（Pawn + CharacterMoverComponent + AC_VisualOverrideManager 等一整套 GASP 装备）
- **加变量** `FixedVisualOverrideClass : class:Actor`（default=`BP_MH_Character_1_C`, instance_editable, category="NPC"）—— 让子类支持 per-instance override（其他 NPC 如果想要 BP_MH_Character_2_C 做视觉，改这个变量就行）
- **EventGraph BeginPlay 链**：
  ```
  Event BeginPlay
    → Super::BeginPlay  (K2Node_CallParentFunction_0)
    → AC_VisualOverrideManager.SetFixedAndApply(FixedVisualOverrideClass)
  ```
  Super 先跑，会触发 AC 的 BeginPlay → 原始 FindAndApplyVisualOverride（此时 FixedVisualOverride 还是 None，走 CVar 路径，VisualOverride = None）。然后子类 BeginPlay 调 SetFixedAndApply → 覆盖为 BP_MH_Character_1_C。中间可能有 1 帧没 VisualOverride 的"空"状态，肉眼不可见。

### Level + INI 改动

- `L_prison`：删原 `BP_MH_Character_1_C_1`，原位 (-13,378,3) 放 `BP_NPC_MH_Character_1` label=`NPC_MH_Character_1`
- `DefaultEngine.ini`：删 `GlobalDefaultGameMode=...` 行。GameMode 回到引擎默认（`AGameMode` 或 `AGameModeBase`），`DefaultPawnClass=UDefaultPawn`（飞行摄像头）

### 运行时数据流

```
PIE Start
  ↓
Level 加载 → 默认 GameMode 生效 → DefaultPawn spawn at PlayerStart(0,0,92) [不可见]
Level 里 BP_NPC_MH_Character_1 实例 (-13,378,3) 被 spawn
  ↓
NPC 的 SandboxCharacter_Mover.BeginPlay:
  Components BeginPlay (AC_VisualOverrideManager 先跑):
    FindAndApplyVisualOverride:
      FixedVisualOverride 是 None（子类 BeginPlay 还没跑）
      → 走 else 分支：Cast GM_Sandbox 成功 → 读 CVar(-1) → VisualOverride=None
      → ApplyVisualOverride(None): SetChildActorClass(child, None), UEFN Mesh 可见
  Actor BP BeginPlay (子类 BP_NPC_MH_Character_1.EventGraph):
    Super::BeginPlay (上面的流程)
    GetComponentByClass<AC_VisualOverrideManager> → SetFixedAndApply(BP_MH_Character_1_C):
      Set FixedVisualOverride = BP_MH_Character_1_C
      FindAndApplyVisualOverride:
        FixedVisualOverride 有效 → 走 true 分支
        Set VisualOverride = BP_MH_Character_1_C
        → OnRep_VisualOverride → ApplyVisualOverride(BP_MH_Character_1_C):
          SetChildActorClass(child, BP_MH_Character_1_C) → ChildActor 生成
          SetVisibility(UEFN_Mesh, NOT true=false) → UEFN 隐藏
```

### 为什么 GM_Sandbox 不是 GameMode 也能用

上一轮我担心 `FindAndApplyVisualOverride` Cast GM_Sandbox 失败会 early return 导致整个流程挂掉。实测没事：

- 默认 GameMode 下 Cast 失败，确实 early return —— 但那是在 true 分支跳过后（FixedVisualOverride 有效时根本不走 Cast）
- 所以子类 BeginPlay 调 SetFixedAndApply 内部的 FindAndApplyVisualOverride 会走 true 分支，不会碰到 Cast 失败的问题
- 子类 BeginPlay 之前，Super 跑时的 FindAndApplyVisualOverride Cast 失败了 —— 但也没关系，反正子类 BeginPlay 马上覆盖

---

## 反思

### 做对的事

- **预检再决策**。开始就用 `get_graph_data` + `get_component_details` 把 AC_VisualOverrideManager 的完整逻辑搞清（3 个函数 + 30 个节点），然后才能看出"设 FixedVisualOverride 前置 Branch"是正确入侵点 —— 不是简单 per-instance prop override 能解决的
- **写 AskUserQuestion 前先把所有可行方案列出来**。用户回答"只要代码能驱动"后，立刻知道不需要 AI Controller，也不需要用户控制切换，最简单的子类 + FixedClass 就够了
- **build_blueprint_from_spec 做函数体**。SetFixedAndApply 函数体只有 2 个节点 + 3 条连线 —— 一次构建 + 一次 connect_pins_bulk 搞定
- **用"继承组件的自动 BP 变量"绕过 class pin 默认值坑**。Inherited components 在子类自动成为 BP 变量（type = `object:AC_VisualOverrideManager_C*`），`VariableGet` 直接拿到**已经类型对齐**的组件指针，比 `GetComponentByClass + Cast` 更简洁，也不用调 pin default

### 踩过的坑

1. **`set_component_property` 在子类 BP 上报 "Component not found"**。`AC_VisualOverrideManager` 虽然能在 get_cdo_properties / get_components 里看到（父类的一部分），但 `set_component_property` 只看本 BP 自己新建的组件。继承组件要在子类里改默认值，要么 UE 编辑器手点，要么走运行时 setter 路径
2. **`set_cdo_property` 不支持点路径**（`AC_VisualOverrideManager.FixedVisualOverride`）报 Property not found。MCP 的 CDO 操作只看顶级属性
3. **class pin 的 `set_pin_default` 只写 `default_value` string，不写 `default_object` 对象引用**。对 `GetComponentByClass.ComponentClass` 这种 `TSubclassOf<T>` pin，设了 string 编译报"无效"。真正生效的是 `default_object`，但 MCP 没有直接写 `default_object` 的 API
4. **绕坑办法**：用 `add_variable` 建一个 class 类型 BP 变量 + `set_variable_defaults` 设默认值（path 字符串），再 `VariableGet` 喂到目标 pin。BP 变量的默认值系统**真的**能解析 class path string
5. **UE 新建的子类 BP 有 disabled 的 Event BeginPlay + Super 模板节点**。一连新节点到 Super 的 then 上，整条链自动启用 —— 不用手动 enable
6. **`GetComponentByClass` + `DynamicCast` 链要 4 节点 2 连线**；用继承组件 `VariableGet` 只要 1 节点 0 连线 —— 类型直接对齐，不需要 Cast

### 值得沿用的模式

- **"加前置分支"替代"整体重写"**。改别人已有的共享 Component 逻辑，最小改动是在入口加 Branch 判断，True 走新路径，False 走原路径。这样调用方可以渐进迁移，Player Pawn 零影响
- **"公共 setter + 子类 BeginPlay 调用"模式**。Component 的属性 per-instance 默认值 MCP 搞不定时，改写时机到运行时 —— 加一个公开函数 `SetXAndApply(X)`，子类 BP BeginPlay 调一次。这个模式对"配置通过数据驱动"场景通用
- **"继承组件自动变量"作为类型安全引用**。MCP 操作继承组件时不走 `GetComponentByClass + Cast`，直接 `VariableGet AC_VisualOverrideManager` —— 返回值类型已是子类，节省 2 个节点

### 后续改进（未做）

- **复用到 BP_MH_Character_2~10**：`BP_NPC_MH_Character_N`（N=2..10）可以复制 `BP_NPC_MH_Character_1`，把 `FixedVisualOverrideClass` 改成对应的 `BP_MH_Character_N_C`。Level 里再把 `BP_MH_Character_N_C_1` 原地替换成 `BP_NPC_MH_Character_N`。预估每个 <5 分钟
- **解决 T 键多 NPC 同时响应**：现在所有 NPC 的 ChildActor 都通过 `EnableInput(PlayerController)` 绑了 T 键，按 T 会让 10 个都说话。生产场景应改为：NPC ChildActor 的 BeginPlay 里**不**调 EnableInput（把 T 键监听改成代码驱动触发），或者给每个 NPC 一个独立的 ID/InputComponent priority
- **`GM_Sandbox.VisualOverrides[6]`**：上一轮加的 `BP_MH_Character_1_C` 保留在数组里。现在玩家走 DefaultPawn 路径不会消费它，可以清掉但无害
- **NPC AI Controller**：当前 SandboxCharacter_Mover 的 `AutoPossessAI` 未改，默认可能 AIController 自动占用但没 BehaviorTree，所以 NPC 站着不动。代码驱动 Mover 时要注意和 AIController 的输入冲突

### 验证点

- 重启编辑器（INI 改动生效）→ PIE → 玩家是飞行摄像头（不可见）→ NPC 是 A-Pose（Idle）站立的 MetaHuman，不 T-Pose → 按 T → NPC 说话 + 口型同步 → Output Log 有 `LogMinimaxACE: AnimateFromAudioSamples returned true`
- 测试通过 ✓
