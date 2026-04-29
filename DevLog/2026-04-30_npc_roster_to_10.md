# NPC 阵容：10 个 MetaHuman 全员就位

日期：2026-04-30

## 当前结论

L_prison 关卡共 10 个 NPC 包装实例 `BP_NPC_MH_Character_1_C` … `_10_C`，全部由 `AIController`（`AutoPossessAI=PlacedInWorld`）控制，挂 `NavMoverComponent`，具备 GASP Mover 2.0 移动 / Audio2Face + MiniMax TTS 说话 / 视觉听觉感知 / SmartObject 入位 / EQS 散点等全套既有能力。

10 个 NPC 几何分布（实测 transform）：

| NPC | XY | Z | Yaw | 排 |
|---|---|---:|---:|---|
| BP_NPC_MH_Character_4 | (6983, 5837) | 340 | -90 | row1 |
| BP_NPC_MH_Character_3 | (7471, 5837) | 340 | -90 | row1 |
| BP_NPC_MH_Character_2 | (7972, 5837) | 340 | -90 | row1 |
| BP_NPC_MH_Character_1 | (8470, 5837) | 340 | -90 | row1 |
| BP_NPC_MH_Character_9 | (8978, 5837) | 340 | -90 | row1 |
| BP_NPC_MH_Character_10 | (6986, 3586) | 340 | +90 | row2 |
| BP_NPC_MH_Character_8 | (7468, 3586) | 340 | +90 | row2 |
| BP_NPC_MH_Character_7 | (8002, 3586) | 340 | +90 | row2 |
| BP_NPC_MH_Character_6 | (8494, 3886) | 340 | +90 | row2 |
| BP_NPC_MH_Character_5 | (8984, 3586) | 340 | +90 | row2 |

朝向规则：`row1`（Y=5837）yaw=-90 朝 -Y；`row2`（Y=3586）yaw=+90 朝 +Y。两排隔大厅相对而立。Z=340 是 capsule 中心；HalfHeight=85，脚底 Z=255 = 地板高度。

## 每个 NPC 的"完整性"清单

加新 NPC 时 3 件事必须全做。任何一件缺失就会出现"看似在场但行为异常"的静默失败。

### 1. NPC 包装 Pawn（`/Game/Blueprints/NPCs/BP_NPC_MH_Character_N`）

- 父类 = `SandboxCharacter_Mover_C`（14 组件全继承：Capsule / SkeletalMesh / VisualOverride / GameplayCamera / SpringArm / Camera / CharacterMover / NavMover / MotionWarping / AC_TraversalLogic / AC_FoleyEvents / AC_VisualOverrideManager / AC_SmartObjectAnimation / AIPerceptionStimuliSource）
- 1 个变量 `FixedVisualOverrideClass: class:Actor`（category=NPC，instance_editable=true），默认值指向自家 `BP_MH_Character_N_C`
- 实现 1 个接口 `AILiveAgent`（`/Script/AILiveProject.AILiveAgent`，`is_inherited=false`）——感知 logger 用它过滤掉玩家 Pawn
- 继承自父类 CDO：`AIControllerClass = AIC_NPC_SmartObject_C`、`AutoPossessAI = PlacedInWorld`

### 2. 视觉身体 BP（`/Game/MetaHumans/MH_Character_N/BP_MH_Character_N`）

raw MetaHuman 导出**不直接可用**，必须补 6 件事到与 `BP_MH_Character_2` 等价。**漏 Body 3 属性是经典坑**——会导致 NPC 移动时滑步（动画不驱动 Body mesh）：

- **Body 组件 3 属性**（GASP 动画驱动核心；缺则滑步）：
  - `AnimClass = /Game/Blueprints/RetargetedCharacters/ABP_GenericRetarget.ABP_GenericRetarget_C`（GASP 走路 / Idle / Motion Matching 动画通过此 ABP retarget 喂给 MetaHuman Body）
  - `ComponentTags = ["RTG_UEFN_to_Metahuman_nrw"]`（`ABP_GenericRetarget` 通过此 tag 找到对应 RetargetAsset）
  - `VisibilityBasedAnimTickOption = AlwaysTickPoseAndRefreshBones`（默认 `OnlyTickPoseWhenRendered` 在 NPC 不被相机看见时停 tick，导致动画状态机不前进）
- **Face 组件 `AnimClass` = `Face_Archetype_Skeleton_AnimBP_C`**（含 `ApplyACEAnimation` 节点；缺则音频播但脸不动）
- **`ACEAudioCurveSource` 组件挂在 `Face` 子组件下**（`UACEAudioCurveSourceComponent`；缺则 TTS 数据无处落）
- **EventGraph BeginPlay tick prerequisite 链（10 节点）**：`BeginPlay → DelayUntilNextTick → Branch(IsValid GetAttachParent of RootComponent) → Branch(IsValid GetAttachParent of GetAttachParent) → AddTickPrerequisiteComponent(self=GetChildComponent(RootComponent, 0)=Body, PrerequisiteComponent=GetAttachParent.GetAttachParent=parent Capsule)`——保证视觉 Body 在父 Pawn Capsule 移动后再 tick，GASP/Mover 姿态稳定性依赖此链

不需要 A2F 调试变量。`BP_MH_Character_1` 个体特例多带 6 个 A2F 变量（`Path to Wav`、`A2FProvider Name="LocalA2F-Claire"`、`ACEEmotions`、`Face Parameter` 等）只用于 NPC1 早期单独调试，新加 NPC 不要复制。通用基线是 `BP_MH_Character_2..10`，每个仅 3 个 LiveLink 变量。

### 3. 关卡放置

`spawn_blueprint_actor` 用 NPC 包装 Pawn class（`BP_NPC_MH_Character_N`），不是视觉身体 class。视觉身体由 NPC 包装 Pawn 的 `AC_VisualOverrideManager.SetFixedAndApply(FixedVisualOverrideClass)` 在 BeginPlay 时通过 `VisualOverride: ChildActorComponent` 自动 spawn。直接放视觉身体到关卡只有 mesh，没有 AIController / Mover / Perception / ACE 自动挂载，所有功能都不工作。

放置位置选 row1（Y=5837，yaw=-90）或 row2（Y=3586，yaw=+90），Z=340，X 沿排序排列保持 ~500uu 间距。

## 未来再加 NPC 的 procedure

执行人按此顺序对每个新 NPC 各做一遍（`monolith_status` 确认在线后开工）：

1. **视觉身体补齐**：
   - `set_component_property component_name=Body property_name=AnimClass value=/Script/Engine.AnimBlueprintGeneratedClass'/Game/Blueprints/RetargetedCharacters/ABP_GenericRetarget.ABP_GenericRetarget_C'`
   - `set_component_property component_name=Body property_name=ComponentTags value=("RTG_UEFN_to_Metahuman_nrw")`
   - `set_component_property component_name=Body property_name=VisibilityBasedAnimTickOption value=AlwaysTickPoseAndRefreshBones`
   - `set_component_property component_name=Face property_name=AnimClass value=/Script/Engine.AnimBlueprintGeneratedClass'/Game/MetaHumans/Common/Face/Face_Archetype_Skeleton_AnimBP.Face_Archetype_Skeleton_AnimBP_C'`
   - `add_component component_class=ACEAudioCurveSourceComponent component_name=ACEAudioCurveSource parent=Face`
   - `copy_nodes source_asset=/Game/MetaHumans/MH_Character_2/BP_MH_Character_2 source_graph=EventGraph node_ids=[K2Node_CallFunction_1, _2, _4, _5, _6, _7, _8, _9, K2Node_IfThenElse_0, _1] target_asset=...new BP target_graph=EventGraph` 拷 10 个 tick prerequisite 节点
   - `connect_pins` 把新 BP 的 `K2Node_Event_0.then` 接到 `K2Node_CallFunction_1.execute`（外部连接，copy_nodes 自动丢弃）
   - `compile_blueprint` + `save_asset`
   - 验收：`compare_blueprints` 与 `BP_MH_Character_2` 应 `total_diffs=0`
2. **NPC 包装 Pawn 创建**：
   - `duplicate_blueprint asset_path=/Game/Blueprints/NPCs/BP_NPC_MH_Character_1 new_path=/Game/Blueprints/NPCs/BP_NPC_MH_Character_N`
   - `set_variable_defaults name=FixedVisualOverrideClass default_value=/Script/Engine.BlueprintGeneratedClass'/Game/MetaHumans/MH_Character_N/BP_MH_Character_N.BP_MH_Character_N_C'`
   - `get_interfaces` 兜底验证 AILiveAgent 接口存在
   - `compile_blueprint` + `save_asset`
3. **关卡放置**：
   - `spawn_blueprint_actor blueprint=/Game/Blueprints/NPCs/BP_NPC_MH_Character_N location=[X,Y,340] rotation=[0,Yaw,0] label=BP_NPC_MH_Character_N folder=Players`，rotation 轴顺序 = `[Pitch, Yaw, Roll]`
   - `save_asset asset_path=/Game/MyAssets/Levels/L_prison`

按此 procedure，每个新 NPC ~1 分钟。

## Monolith MCP 实施踩坑

- **参数名严格**：`duplicate_blueprint` 是 `asset_path / new_path`（不是 source_path / dest_path）；`set_variable_defaults` 是 `name`（不是 variable_name）；`spawn_blueprint_actor` 是 `blueprint`（不是 asset_path）；`delete_actors` 是 `actor_names`（数组）。
- **rotation 数组顺序 = `[Pitch, Yaw, Roll]`**。row1 用 `[0, -90, 0]`、row2 用 `[0, 90, 0]`。误把 yaw 写到第 3 位会让角色翻倒。
- **`copy_nodes` 保留内部连接、丢外部连接**。把 BeginPlay tick prerequisite 链 10 节点一次拷过去后，BeginPlay→DelayUntilNextTick 这条外部连接需 `connect_pins` 手工补。
- **关卡 actor 实例 name 后缀漂移**（`_C_4` / `_C_1` 不稳定）。删除关卡占位前必须先 `mesh::get_level_actors class_filter=...` 拿真实 name，再 `delete_actors`。
- **PIE 跑时 `save_asset` 失败**。先 stop PIE 再 save。`compile_blueprint` 是内存编译，PIE 中可正常跑。
- **`set_component_property` 对 Pawn 子类继承组件不生效**（CLAUDE.md 红线）。视觉身体的组件是其自身的，不是继承的，所以本流程的 `Face.AnimClass` 改写正常工作；但在 NPC 包装 Pawn 上别用此招改 SandboxCharacter_Mover 继承的组件默认值。

## PIE 验证

按 M / I / N / K / L / O 全键回归（不预设具体 sight 可见数，按实测落定）：

| 键 | 期望（10 NPC 下）| 关注点 |
|---|---|---|
| M | 10 NPC 全部走向 NavTarget 并看 NavLookTarget | NPC9/10 加入移动队伍；Output Log 无 `LogPathFollowing/LogAIController Failed` |
| I | NPC1 sight snapshot | NPC9 与 NPC1 同排（row1）距离 508uu，应进 sight；NPC10 在 row2 ~2251uu 接近 sight 半径 2000uu 边缘 |
| N | NPC2 说话，NPC1 hearing 命中 NPC2 | 不受新增影响；NPC9/10 若在 hearing range 2000uu 内也应入列表 |
| K | NPC1 走到 BP_SmartBench 并坐下 | 不受新增影响 |
| L | 10 NPC 各自散到 BP_NavTarget 周围不同点 | EQS=37 候选点对 10 NPC 仍宽裕；玩家 Pawn 仍被 IsPlayerControlled 过滤；C++ 算法零改动 |
| O | NPC1 / NPC2 餐厅相遇剧情 | StoryScenarioDirector 仍按硬编路径找 NPC1 / NPC2 |

NPC9 / NPC10 自身验证：
- AutoPossessAI=PlacedInWorld 自动被 AIController possess
- BeginPlay 后 `AC_VisualOverrideManager.SetFixedAndApply(BP_MH_Character_9_C/_10_C)` spawn 可见身体
- 嘴型 + A2F 表情正常（依赖视觉 BP 的 `Face.AnimClass + ACEAudioCurveSource`）
- M 键时走路、L 键时进散点队列、N 键 hearing range 内进 NPC1 hearing 列表
