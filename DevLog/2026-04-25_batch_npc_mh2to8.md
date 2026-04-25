# 2026-04-25 · 批量升级 MH_Character_2~8 为 GASP NPC + 清理 GM_Sandbox

## Prompt

```
1. 复制模式到 BP_MH_Character_2~8 的场景 NPC
2. 解决 T 键按下时 8 个 NPC 同时响应的问题（改 ChildActor BeginPlay 的 EnableInput 策略 或换代码驱动）
3. 可选清理 GM_Sandbox.VisualOverrides[6]
```

## 功能描述

把上一轮对 `MH_Character_1` 的 "NPC 壳子 + MetaHuman 视觉 + GASP Mover + A2F" 模式复制到 `MH_Character_2~8`。结果：场景里 8 个 MetaHuman NPC 全部是 `SandboxCharacter_Mover` 子类实例，都带 GASP Motion Matching + Idle 不 T-Pose + A2F 面部组件（代码可驱动任意 NPC 说话）。

**交付物**：

| 类型       | 数量 | 说明                                                                                                                |
| ---------- | ---: | ------------------------------------------------------------------------------------------------------------------- |
| 修改 BP    |    7 | `BP_MH_Character_2~8`：Body 3 属性 + Face.AnimClass + 加 ACEAudioCurveSource + BeginPlay 10 节点链                  |
| 新建 BP    |    7 | `BP_NPC_MH_Character_2~8`：`SandboxCharacter_Mover` 子类，`FixedVisualOverrideClass` 指向对应 `BP_MH_Character_N_C` |
| 关卡       |    7 | 删 `BP_MH_Character_N_C_1` NPC、原位 spawn `BP_NPC_MH_Character_N`（label `NPC_MH_Character_N`）                    |
| GM_Sandbox |    1 | 删 `VisualOverrides[6] = BP_MH_Character_1_C`（玩家不走 GASP 路径，冗余）                                           |

---

## 实现逻辑

### T 键多响应问题 —— 自动消失

预检发现 `BP_MH_Character_2~8` **本来就没有 T 键触发链**（EventGraph 里只有 LiveLinkSetup 相关节点，`K2Node_InputKey` 不存在）。A2F/TTS 集成只在 `BP_MH_Character_1` 做过。

所以场景里按 T 时：

- `NPC_MH_Character_1` 的 VisualOverride ChildActor = `BP_MH_Character_1`（有 T 键链 + `EnableInput`）→ 响应 T → `TriggerMinimaxSpeech`
- `NPC_MH_Character_2~8` 的 VisualOverride ChildActor = `BP_MH_Character_2~8`（无 T 键链）→ 不响应

**结论**：Task #13 不需要做任何代码。生产代码驱动路径 `TriggerMinimaxSpeech(NPC.VisualOverride.ChildActor, text, apiKey, ...)` 对所有 NPC 都适用（`ACEAudioCurveSource` 组件已在每个 `BP_MH_Character_N` 里）。

### 批量改造模式

对每个 `BP_MH_Character_N` (N=2..8)，4 步：

1. **Body 3 属性**：
   - `AnimClass` = `ABP_GenericRetarget_C`
   - `ComponentTags` += `RTG_UEFN_to_Metahuman_nrw`
   - `VisibilityBasedAnimTickOption` = `AlwaysTickPoseAndRefreshBones`
2. **Face 1 属性**：
   - `AnimClass` = `Face_Archetype_Skeleton_AnimBP_C`（包含 `FAnimNode_ApplyACEAnimation`）
3. **新增 `ACEAudioCurveSource` 组件**（`ACEAudioCurveSourceComponent`，parent=`Face`）
4. **BeginPlay 链**（10 节点，照 `BP_MH_Character_1` 模式但去掉 `EnableInput`/`PrewarmA2F`）：
   ```
   Event BeginPlay → Delay Until Next Tick
     → Branch#1(IsValid GetAttachParent(Root))
       → True: Branch#2(IsValid GetAttachParent(GetAttachParent(Root)))
         → True: AddTickPrerequisiteComponent(Body, UEFN_Mesh)
   ```

对每个 `BP_NPC_MH_Character_N` (N=2..8)，5 步：

1. `create_blueprint` 继承 `SandboxCharacter_Mover_C`
2. `add_variable FixedVisualOverrideClass : class:Actor (instance_editable, category="NPC")`
3. `set_variable_defaults` → `/Game/MetaHumans/MH_Character_N/BP_MH_Character_N.BP_MH_Character_N_C`
4. EventGraph 加 3 节点 + 2 连线：`VariableGet AC_VisualOverrideManager` + `VariableGet FixedVisualOverrideClass` + `CallFunction SetFixedAndApply`（内部：get_ac→set.self，get_fixed→set.FixedClass）
5. 外部连：`K2Node_CallParentFunction_0.then` → `SetFixedAndApply.execute`

关卡替换：`delete_actors` 7 个旧实例 + `place_blueprint_actor` 7 个新 NPC，坐标同原位。

### 批量策略

所有 7 个 BP 的操作完全同构，利用 MCP 并行调用：

- **Phase 1**（24 并行）：7 个 BP 的 4 个 `set_component_property`
- **Phase 2**（6 并行）：7 个 `add_component` (MH_Character_2 上一步就做了)
- **Phase 3**（6 并行）：7 个 `build_blueprint_from_spec`（10 节点 + 12 连线 + 1 pin default）
- **Phase 4**（6 并行）：7 个 `connect_pins`
- **Phase 5**（14 并行）：7 个 `compile_blueprint` + 7 个 `save_asset`
- **Phase 6**（7 并行）：7 个 `create_blueprint`
- **Phase 7**（7 并行）：7 个 `add_variable`
- **Phase 8**（7 并行）：7 个 `set_variable_defaults`
- **Phase 9**（7 并行）：7 个 `build_blueprint_from_spec`（3 节点 + 2 连线）
- **Phase 10**（7 并行）：7 个 `connect_pins`
- **Phase 11**（14 并行）：7 个 compile + 7 个 save
- **Phase 12**（1+7 并行）：`delete_actors` 7 个 + `place_blueprint_actor` 7 个
- **Phase 13**（4 并行）：set_cdo_property + compile GM + save GM + save level

**实际用时**：估计比单个 BP 做 14 次 serial 快 ~10 倍。

### 清理 GM_Sandbox

玩家回退 DefaultPawn 飞行摄像头后，`GM_Sandbox.VisualOverrides` 数组不再被消费（NPC 的 AC_VisualOverrideManager 走 `FixedVisualOverride` 前置分支，不读 GM.VisualOverrides）。把上一轮加的 `BP_MH_Character_1_C` 从数组删掉，恢复原始 6 项：

- [0] BP_Echo_C
- [1] BP_Twinblast_C
- [2] BP_Kellan_C
- [3] BP_Manny_C
- [4] BP_Quinn_C
- [5] BP_UE4_Mannequin_C

---

## 验证

PIE L_prison → 所有 8 个 MetaHuman NPC Idle 站立，无 T-Pose → 按 T 仅 `NPC_MH_Character_1` 响应（说话 + 口型）→ 其他 7 个 NPC 保持 Idle 不说话。测试通过 ✓

---

## 反思

### 做对的事

- **批量并行**。对 7 个同构改动用单轮 MCP 并行批处理，总共 12 轮 × 7 并行 ≈ 80 个 MCP 调用 在 ~2 分钟内完成。比逐个 serial 省 ~10x 时间
- **预检一次解决 3 个任务**。Task #13（T 键多响应）的方案本来规划为"加 Tag 过滤" 或 "改 EnableInput"；预检发现 MH_2~8 根本没 T 键链，自动解决。这个发现直接干掉 Task #13 整个子任务
- **复用上一轮的 id 映射惯例**。BP_MH_Character_2~8 和 BP_MH_Character_1 的 EventGraph BeginPlay 头部 `K2Node_Event_0` 一致，新建的 `K2Node_CallFunction_1`（delay）一致，所以不需要逐个查 graph data，可以批量 `connect_pins(K2Node_Event_0.then → K2Node_CallFunction_1.execute)`
- **精简 BeginPlay 链**。上一轮 MH_Character_1 的 BeginPlay 有 `EnableInput + PrewarmA2F + GetAvailableA2FProviders + AddTickPrerequisite` 14 节点；MH_2~8 只要 10 节点（去掉 input/prewarm）—— NPC 代码驱动，不需要用户输入路由；Prewarm 在 MH_Character_1 已经全局调过了

### 踩过的坑

1. **MH_Character_4 的实例名有 `_C_2` 后缀**（不是 `_C_1`）。批量 delete_actors 时要挨个列出准确名字，不能按 pattern。预检时 `get_level_actors` 查清楚再删
2. **7 个 BP 的构建虽然同构，但仍要 7 次独立 `build_blueprint_from_spec`**。`build_blueprint_from_spec` 不支持一次多个 asset_path，只能并行

### 值得沿用的模式

- **"模板-实例"批量改造**。先做 1 个（MH_Character_1），作为模板验证；再并行复制到 N 个同构目标。省掉重复 debug。上一轮 MH_1 跑通后，这轮 MH_2~8 0 个重新 debug 周期
- **"干掉一个需求"作为 Phase 0 结果**。预检不只是"差量发现"，还能"干掉整个任务"。如果预检早做一轮，Task #13 的 Plan Mode 就不需要费神想方案

### 后续改进（未做）

- **代码驱动 TTS 的 BP 接口/C++ 函数**：现在要通过 `NPC.VisualOverride.ChildActor` 间接拿 `BP_MH_Character_N` 实例才能调 `TriggerMinimaxSpeech`。可以在 `AC_VisualOverrideManager` 或 NPC 壳子加个便捷函数 `SpeakViaACE(text, apiKey, ...)` 自动找 ChildActor 分派
- **代码驱动 Mover**：每个 NPC 是独立 `SandboxCharacter_Mover` 实例，`CharacterMoverComponent` API 可驱动移动。未做：封装 `MoveNPCTo(target_loc)` 这类便捷函数；也需要 AI 路径规划 / NavMesh
- **场景 NPC 占位 overlap 问题**：8 个 NPC 都是 `SandboxCharacter_Mover` 壳子 Pawn，带 `Capsule` 碰撞体，靠得近的几个（如 MH_Character_3 和 MH_Character_2 相距 400 单位）可能发生碰撞或挤压。如果后期 AI 驱动移动，要注意避让/导航
- **性能**：8 个独立 Pawn + 8 个 ChildActor + 8 个 MetaHuman Mesh (Face/Body/Grooms/Clothing) 运行时开销大。可以先不优化，PIE 下观察 `stat unit` 和 `stat anim`
