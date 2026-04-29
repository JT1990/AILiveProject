# 2026-04-25 · 批量升级 MH_Character_2~10 为 GASP NPC + 清理 GM_Sandbox

## 功能描述

`BP_MH_Character_2~10` 是通用 NPC 视觉身体基线：它们补齐 GASP Body、Face AnimBP、`ACEAudioCurveSource` 和 BeginPlay tick prerequisite 链，但不复制 `BP_MH_Character_1` 的 T 键输入链与 A2F 调试变量。场景里的 10 个 MetaHuman NPC 全部是 `SandboxCharacter_Mover` 子类包装 Pawn 实例，都带 GASP Motion Matching + Idle 不 T-Pose + A2F 面部组件，代码可驱动任意 NPC 说话。

**当前资产结构**：

| 类型       | 数量 | 说明                                                                                                                |
| ---------- | ---: | ------------------------------------------------------------------------------------------------------------------- |
| 修改 BP    |    9 | `BP_MH_Character_2~10`：Body 3 属性 + Face.AnimClass + 加 ACEAudioCurveSource + BeginPlay 10 节点链                 |
| 新建 BP    |    9 | `BP_NPC_MH_Character_2~10`：`SandboxCharacter_Mover` 子类，`FixedVisualOverrideClass` 指向对应 `BP_MH_Character_N_C` |
| 关卡       |    9 | `BP_NPC_MH_Character_2~10` 包装 Pawn 实例按对应位置放置，视觉身体由 FixedVisualOverride 自动生成                    |
| GM_Sandbox |    1 | `VisualOverrides` 保持原始 6 项；场景 NPC 不读取该数组                                                            |

---

## 实现逻辑

### T 键输入归属

`BP_MH_Character_1` 是个体调试特例，保留 T 键链与 6 个 A2F 调试变量。`BP_MH_Character_2~10` 作为通用 NPC 视觉基线，不包含 `K2Node_InputKey`，EventGraph 只保留 LiveLinkSetup 与 tick prerequisite 相关节点。

场景里按 T 时：

- `NPC_MH_Character_1` 的 VisualOverride ChildActor = `BP_MH_Character_1`（有 T 键链 + `EnableInput`）→ 响应 T → `TriggerMinimaxSpeech`
- `NPC_MH_Character_2~10` 的 VisualOverride ChildActor = `BP_MH_Character_2~10`（无 T 键链）→ 不响应

生产代码驱动路径 `TriggerMinimaxSpeech(NPC.VisualOverride.ChildActor, text, apiKey, ...)` 对所有 NPC 都适用（`ACEAudioCurveSource` 组件已在每个 `BP_MH_Character_N` 里）。

### 通用视觉 BP 基线

每个 `BP_MH_Character_N` (N=2..10) 具备 4 类配置：

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

每个 `BP_NPC_MH_Character_N` (N=2..10) 具备 5 类配置：

1. `create_blueprint` 继承 `SandboxCharacter_Mover_C`
2. `add_variable FixedVisualOverrideClass : class:Actor (instance_editable, category="NPC")`
3. `set_variable_defaults` → `/Game/MetaHumans/MH_Character_N/BP_MH_Character_N.BP_MH_Character_N_C`
4. EventGraph 加 3 节点 + 2 连线：`VariableGet AC_VisualOverrideManager` + `VariableGet FixedVisualOverrideClass` + `CallFunction SetFixedAndApply`（内部：get_ac→set.self，get_fixed→set.FixedClass）
5. 外部连：`K2Node_CallParentFunction_0.then` → `SetFixedAndApply.execute`

关卡中 `BP_NPC_MH_Character_2~10` 均为包装 Pawn 实例，坐标沿用当前 10 NPC 阵容分布。删除或替换关卡 actor 时先用 Monolith 读取真实 actor name，不按 `_C_1` 后缀推断。

### 清理 GM_Sandbox

玩家回退 DefaultPawn 飞行摄像头后，`GM_Sandbox.VisualOverrides` 数组不再被消费。NPC 的 `AC_VisualOverrideManager` 走 `FixedVisualOverride` 前置分支，不读 GM.VisualOverrides。当前数组保持原始 6 项：

- [0] BP_Echo_C
- [1] BP_Twinblast_C
- [2] BP_Kellan_C
- [3] BP_Manny_C
- [4] BP_Quinn_C
- [5] BP_UE4_Mannequin_C

---

## 验证

PIE L_prison → 所有 10 个 MetaHuman NPC Idle 站立，无 T-Pose → 按 T 仅 `NPC_MH_Character_1` 响应（说话 + 口型）→ 其他 9 个 NPC 保持 Idle 不说话。测试通过 ✓

---

## 维护要点

- `BP_MH_Character_2~10` 与 `BP_MH_Character_2` 保持完全等价；`BP_MH_Character_1` 是个体调试特例，不能作为后续 NPC 视觉 BP 的通用基线。
- `BP_MH_Character_2~10` 不包含 `EnableInput`、`PrewarmA2F`、`GetAvailableA2FProviders`、T 键输入链，也不包含 `Path to Wav` / `A2FProvider Name` / `ACEEmotions` / `Face Parameter` 等 6 个 A2F 调试变量。
- BeginPlay tick prerequisite 链可从 `BP_MH_Character_2` 的 EventGraph 复制。`copy_nodes` 保留内部连线但会丢失外部入口，目标 BP 仍需手动连接 `Event BeginPlay.then → DelayUntilNextTick.execute`。
- 代码驱动 TTS 时，audio target 使用 `NPC.VisualOverride.ChildActor`；AI Hearing 的归因 actor 使用 NPC 包装 Pawn。
- 关卡 actor 实例名后缀不稳定。删除或替换实例前先按 label/class 查询真实 name，再执行 `delete_actors`。
- 10 个独立 Pawn + 10 个 ChildActor + 10 个 MetaHuman Mesh 会带来明显动画和渲染开销；性能观察使用 `stat unit` 和 `stat anim`。
