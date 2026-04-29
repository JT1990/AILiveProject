# 2026-04-25 · GASP Mover 2.0 → BP_MH_Character_1（保留 A2F + MiniMax TTS）

## Prompt

```
将 unreal engine 5.7 GASP Mover2.0 应用到任意metahuman中，我创建的metahuman 路径为：
D:\Project\Unreal\AILiveProject\Content\MetaHumans\MH_Character_1\BP_MH_Character_1.uasset

已完成：
BP_MH_Character_1.uasset以支持 Nvidia audio2face-3D + minimax tts，
开发记录见 DevLog/2026-04-24_minimax_speech_a2f_metahuman.md

建议工作流：
1. 先从UE 官网和社区调研学习如何将 GASP5.7版本中的 Mover2.0 应用到任意metahuman中，并且深入了解底层原理
2. 在有足够的信息之后，再思考规划解决方案。
3. Docs/Lucy_GASP_A2F_Manual.md 是我之前在另一个项目中测试成功后的开发记录，供你参考

UE是你不擅长的领域，不要凭感觉猜测，先学习，后思考规划方案。
```

## 功能描述

把 `BP_MH_Character_1`（女性 MetaHuman，此前已接入 A2F + MiniMax TTS）接到 GASP 的 Mover 2.0 + Motion Matching 运动系统上。PIE 后按 `` ` `` 输入 `DDCvar.VisualOverride 6` 切到 MH_Character_1 的可视形象，WASD 用 GASP 驱动身体跑/走/转向/起停，按 T 仍然出声 + 口型同步。

**交付物**：

| 类型     | 路径                                                         | 改动                                                                |
| -------- | ------------------------------------------------------------ | ------------------------------------------------------------------- |
| 角色 BP  | `Content/MetaHumans/MH_Character_1/BP_MH_Character_1.uasset` | 改 Body 3 属性 + 扩展 BeginPlay 链（10 节点）                       |
| GameMode | `Content/Blueprints/GM_Sandbox.uasset`                       | VisualOverrides 追加 `BP_MH_Character_1_C`（索引 6）                |
| 引擎配置 | `Config/DefaultEngine.ini`                                   | 加 `GlobalDefaultGameMode=/Game/Blueprints/GM_Sandbox.GM_Sandbox_C` |

**未新建资产**。

---

## 实现逻辑

### 用户决策（和 plan 时确定）

1. **就地改 BP_MH_Character_1**，不复制 `_GASP` 变体 —— 简洁，回滚靠 git
2. **用 `DDCvar.VisualOverride` 控制台切换**，PIE 不默认跳到 MH
3. **直接复用 `Face_Archetype_Skeleton_AnimBP`**，不复制角色专属 Face AnimBP —— 少一份资产

### 阶段 0 预检发现

用 Monolith MCP 查 `BP_MH_Character_1` / `GM_Sandbox` / `L_prison` 的当前状态，发现很多东西已经就位：

| 检查项                                | 预检结果                                                                  | 后续动作                           |
| ------------------------------------- | ------------------------------------------------------------------------- | ---------------------------------- |
| `Face.AnimClass`                      | 已是 `Face_Archetype_Skeleton_AnimBP_C`                                   | 不改                               |
| `ACEAudioCurveSource` 组件            | 已存在，挂 Face 下                                                        | 不改                               |
| T 键触发链                            | `InputKey T → GetMinimaxApiKeyFromProjectEnv → TriggerMinimaxSpeech` 已连 | 不改                               |
| BeginPlay 已有节点                    | `Enable Input → Prewarm A2F → Get Available A2FProviders`                 | 末尾继续接                         |
| `A2FProvider Name` 变量               | `LocalA2F-Claire`（女声模型）                                             | 确认无误                           |
| `GM_Sandbox.DefaultPawnClass`         | `SandboxCharacter_Mover_C`                                                | 已是正确值                         |
| `GM_Sandbox.VisualOverrides`          | 6 项（索引 0-5）                                                          | 末尾追加索引 6                     |
| `Body.AnimClass`                      | `None`                                                                    | 改                                 |
| `Body.ComponentTags`                  | `[]`                                                                      | 加 `RTG_UEFN_to_Metahuman_nrw`     |
| `Body.VisibilityBasedAnimTickOption`  | `OnlyTickPoseWhenRendered`                                                | 改 `AlwaysTickPoseAndRefreshBones` |
| `L_prison` 的 GameMode Override       | `None`（用默认 GameMode）                                                 | 改 INI 全局 GameMode               |
| `BeginPlay` 的 AddTickPrerequisite 链 | 不存在                                                                    | 新增 10 节点                       |

**预检的价值**：把原计划里"13 节点"的 BeginPlay 新增量压到 10 节点（`Event BeginPlay / EnableInput / PrewarmA2F` 3 个已有），不做无用功。

### 阶段 1 具体操作

1. **Body 3 属性改动**（`set_component_property`）：
   - `AnimClass` → `/Game/Blueprints/RetargetedCharacters/ABP_GenericRetarget.ABP_GenericRetarget_C`
   - `ComponentTags` → `("RTG_UEFN_to_Metahuman_nrw")`
   - `VisibilityBasedAnimTickOption` → `AlwaysTickPoseAndRefreshBones`

2. **`GM_Sandbox.VisualOverrides` 末尾追加**（`set_cdo_property`）：

   ```
   [0] BP_Echo_C
   [1] BP_Twinblast_C
   [2] BP_Kellan_C
   [3] BP_Manny_C
   [4] BP_Quinn_C
   [5] BP_UE4_Mannequin_C
   [6] BP_MH_Character_1_C   ← 新增
   ```

3. **`Config/DefaultEngine.ini` 加 GameMode 全局默认**：

   ```ini
   [/Script/EngineSettings.GameMapsSettings]
   EditorStartupMap=/Game/MyAssets/L_prison.L_prison
   GameDefaultMap=/Game/MyAssets/L_prison.L_prison
   GlobalDefaultGameMode=/Game/Blueprints/GM_Sandbox.GM_Sandbox_C
   ```

   **注意**：改完要重启编辑器才生效。原因：`DefaultEngine.ini` 在编辑器启动阶段读一次。

4. **BeginPlay 链扩展 10 节点**（`build_blueprint_from_spec` 一次性完成）：

   ```
   已有：Event BeginPlay → EnableInput → PrewarmA2F → GetAvailableA2FProviders
   新增：                                                         → Delay Until Next Tick
                                                                    → Branch#1 (IsValid GetAttachParent(Root))
                                                                       → True: Branch#2 (IsValid GetAttachParent(GetAttachParent(Root)))
                                                                          → True: AddTickPrerequisiteComponent(Body, UEFN_Mesh)
   ```

   数据连线（纯函数链）：
   - `GetRootComponent.ReturnValue` → `GetAttachParent_1.self` + `GetChildComponent.self`
   - `GetAttachParent_1.ReturnValue` → `IsValid_1.Object` + `GetAttachParent_2.self`
   - `GetAttachParent_2.ReturnValue` → `IsValid_2.Object` + `AddTickPrerequisiteComponent.PrerequisiteComponent`
   - `IsValid_1.ReturnValue` → `Branch_1.Condition`
   - `IsValid_2.ReturnValue` → `Branch_2.Condition`
   - `GetChildComponent.ChildIndex = 0` → `GetChildComponent.ReturnValue` → `AddTickPrerequisiteComponent.self`

### 运行时原理

- `SandboxCharacter_Mover`（GASP 的玩家 Pawn）作为 Player Pawn 在 `PlayerStart` 生成
- 它身上的 `AC_VisualOverrideManager` 读 `DDCvar.VisualOverride`；当 CVar 值 = 6，spawn `BP_MH_Character_1` 作为 ChildActor
- `BP_MH_Character_1` 的 `BeginPlay` 两层 GetAttachParent：Root → VisualOverride ChildActorComponent → 隐藏的 UEFN SkeletalMesh
- 两层 IsValid 保护：直接放在关卡里的 NPC 实例（没有父 VisualOverride）会在 Branch#1 False 分支跳过，**不会崩溃也不会连 AddTickPrerequisite**
- 通过 `AddTickPrerequisiteComponent(Body, UEFN_Mesh)` 保证每帧 UEFN Mesh 先 Tick 跑 Motion Matching，Body 再用 `ABP_GenericRetarget` 的 `RetargetPoseFromMesh` 从 UEFN Mesh 读新姿态，经 `RTG_UEFN_to_Metahuman_nrw` 映射到 MetaHuman 骨架
- Face 不受影响：继续走 `Face_Archetype_Skeleton_AnimBP` 的 `ApplyACEAnimation` 节点，消费 `ACEAudioCurveSource` 的 A2F blendshape 曲线

---

## 验证流程

PIE + 控制台输入 `DDCvar.VisualOverride 6` → WASD 身体跑动 ✓ → 按 T 中文女声 + 口型同步 ✓ → 边跑边说 ✓

---

## 反思

### 做对的事

- **先 Monolith MCP 预检，再动手**。原计划以为要加 13 节点、改 Face AnimBP、加 ACEAudioCurveSource 组件，预检发现 8 处中 6 处已就位 —— 实际动手量压到最小
- **`build_blueprint_from_spec` 一次性添节点 + 连线**。10 节点 + 12 连线一个事务完成，比 `add_node × 10 + connect_pins × 12` 的 22 轮往返快得多，且原子性
- **保留旧 WAV 测试链**。DevLog 2026-04-24 提过这条死链（`Print Text → CreateAudio2FaceParameters → SetParametersFromStruct → AnimateCharacterFromWavFileAsync`）被解挂但节点保留；这次也不动，延续"新旧并存，一键切回"的模式
- **双层 IsValid 保护**。把"VisualOverride 模式"和"直接场景 NPC 模式"的代码路径合一：同一个 BP 能作为 VisualOverride 跑 GASP，也能作为场景 NPC 静止站立（Ref Pose），互不冲突

### 踩过的坑

1. **`K2_GetAttachParent` 不存在**。UE5.7 里 `SceneComponent` 的正确函数名是 `GetAttachParent`（无 K2 前缀），搜的时候被"K2_GetRootComponent"误导
2. **`IsValid` 有多个同名函数**。直接 `resolve_node("CallFunction", "IsValid")` 解析到 `SubobjectDataBlueprintFunctionLibrary.IsValid`（输入是 `FSubobjectData`），不是我们要的 Object 版。必须显式指定 `target_class: "KismetSystemLibrary"`
3. **`set_actor_properties` 不支持 `DefaultGameMode`**。Monolith MCP 的 `mesh_query.set_actor_properties` 只接受 mobility/simulate_physics/collision_preset/cast_shadow/tags/mass_kg 6 个字段。改 WorldSettings 的 GameModeOverride 走不通 —— 退而求其次用 `DefaultEngine.ini` 全局 GameMode
4. **INI 改完不生效**。`DefaultEngine.ini` 在编辑器启动时读一次，运行中改了要**重启编辑器**才生效
5. **参数名 `value` vs `property_value`**。第一次调 `set_component_property` 传 `property_value` 失败，实际参数名是 `value`（不加 property\_ 前缀）

### 值得沿用的模式

- **预检 → 增量修改**。不要盲目照搬 Lucy 手册全量执行，先查当前状态，只做差量
- **`build_blueprint_from_spec` + 1 次 `connect_pins` 衔接既有节点**。一次性加新节点并连完内部线；只有"新节点 ↔ 既有节点"的衔接需要单独 `connect_pins`
- **`get_execution_flow` 验证链路**。编译通过不等于连线正确；用 `get_execution_flow` 追一次 `ReceiveBeginPlay`，看分支走向能不能对上 Lucy 3.7 的预期

### 后续改进（未做）

- **10 个场景 MetaHuman NPC 同时触发 T 键**：`L_prison` 里放了 `BP_MH_Character_1_C_1 ~ BP_MH_Character_10_C_1` 10 个实例，每个都在 BeginPlay 调 `EnableInput`。按 T 时 10 个都会触发 TriggerMinimaxSpeech，同时说话。本次未处理；要么按 PlayerController 路由（让只有 VisualOverride 里的那个触发），要么给每个 NPC 单独的 InputContext
- **其他 9 个 MH（BP_MH_Character_2~10）还没接 GASP**：本次只做 MH_Character_1。模式已验证，下次按 8-2-7 步骤能批量复制到其他 BP
- **场景 NPC 的 Body 表现**：场景里直接放的 `BP_MH_Character_1_C_1` 实例，Body.AnimClass = `ABP_GenericRetarget_C` 但没有父 UEFN Mesh → Ref Pose（A-Pose 站立）。用户确认"可接受"，但如果要场景 NPC 也有动画，要给它们自己的 AI Controller 或 ABP
- **WorldSettings.GameModeOverride 直接设置**：当前走的是 `DefaultEngine.ini` 全局 GameMode。如果以后加第二个关卡且不想用 GM_Sandbox，要么在那个关卡的 WorldSettings 里覆盖，要么回头改 INI 方案为按关卡 map 的 `GameModeMap` 形式
- **`DefaultEngine.ini` 需重启才生效**：后续实施手册应明确标注"改完重启编辑器"

### 验证日志样本（成功一次的执行流）

通过 `get_execution_flow` 验证的 BeginPlay 链：

```
事件开始运行 (Event BeginPlay)
  → 启用输入 (Enable Input)
    → Prewarm A2F
      → Get Available A2FProviders
        → 延迟到下个Tick (Delay Until Next Tick)
          → 分支 (Branch#1)
            [True] → 分支 (Branch#2)
              [True] → 添加Tick先决条件组件 (Add Tick Prerequisite Component)
```

编译结果：0 errors, 0 warnings（`BP_MH_Character_1` 和 `GM_Sandbox` 两个资产均 UpToDate）
