---
name: visual-override-system
description: NPC / 玩家角色外观运行时切换机制 — AC_VisualOverrideManager + ChildActorComponent + GASP Mover 2.0 的契约与踩坑。包含活跃角色选型 (SandboxCharacter_Mover)、FixedVisualOverride 路径、BeginPlay 双层 IsValid 双模式（VisualOverride spawn 与直接摆关卡 Ref Pose）、AC_PreCMCTick tick 顺序、UEFN→MetaHuman 重定向。Triggers on visual override, 角色外观切换, NPC 模型/外观/身体, ChildActorComponent, SandboxCharacter_Mover, AC_VisualOverrideManager, FixedVisualOverride, SetFixedAndApply, BP_NPC_MH_Character, AC_PreCMCTick, ABP_GenericRetarget, RTG_UEFN_to_Metahuman, Mover 2.0 角色 spawn, BeginPlay IsValid, 角色 Ref Pose, Motion Matching 姿态. 改动 NPC / 玩家可见身体、角色 BP 父类、外观切换或 GASP Mover 2.0 角色 tick 链路时务必加载本 skill。
---

# Visual-override 系统

- 活跃角色是 `SandboxCharacter_Mover`（GASP Mover 2.0）。CMC 版兄弟仅作参考保留，不要往里加新逻辑。
- **`AC_VisualOverrideManager`** 通过给 tag 为 `VisualOverride` 的 `ChildActorComponent` 调 `SetChildActorClass` 来切身体。CVar `DDCvar.VisualOverride`（默认 `-1`）选 `GM_Sandbox.VisualOverrides[]` 的 index。
- **NPC 固定 override 路径**：`AC_VisualOverrideManager.FixedVisualOverride` + `SetFixedAndApply(TSubclassOf)`。子类 BP（`Content/Blueprints/NPCs/BP_NPC_MH_Character_1..10`）持有 `FixedVisualOverrideClass` 变量并在 BeginPlay 调 `SetFixedAndApply`。场景 10 个 NPC 都走这条路。
- **BeginPlay 里两层 `IsValid` 是故意的**：同时支持 (a) VisualOverride 模式下可见身体 spawn 到 `ChildActorComponent`、(b) 直接把角色摆进关卡以 Ref Pose 呈现。删掉任一分支会破坏一种模式。
- **`AC_PreCMCTick`** 用 `AddTickPrerequisiteActor` 保证身体动画先于移动处理 tick，Motion Matching 姿态稳定性依赖这点。
- 重定向：`ABP_GenericRetarget` + `RTG_UEFN_to_Metahuman_nrw` 把 GASP 动画（UEFN mannequin 骨架）运行时重定向到 MetaHuman 身体。
