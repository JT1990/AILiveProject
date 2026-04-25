# 2026-04-26 · L_prison 关卡接入 GASP Mover 第三人称可控角色

## Prompt

```
我希望在 Content/MyAssets/Levels/L_prison.umap 关卡中使用
Content/Levels/DefaultLevel.umap 关卡中的 PlayerStart，
我希望可以有一个具有 GASP Mover2 功能的第三人称可控制的角色，
目的是用于跑图。
```

## 功能描述

让新导入的 `L_prison` 监狱关卡可 PIE 跑图——出生一个第三人称可控角色（GASP Mover 2.0 + IMC_Sandbox 输入），用于人工巡视 3458 个 SM、监狱牢房、围栏等场景资产。

**交付物**：

| 类型           | 路径                                    | 改动                                                                                                                                        |
| -------------- | --------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------- |
| 关卡（改）     | `Content/MyAssets/Levels/L_prison.umap` | spawn `PlayerStart_Prison` at (0,0,0)                                                                                                       |
| 引擎配置（改） | `Config/DefaultEngine.ini`              | `[/Script/EngineSettings.GameMapsSettings]` 加 `+GameModeMapPrefixes=(Name="L_prison",GameMode="/Game/Blueprints/GM_Sandbox.GM_Sandbox_C")` |

---

## 实现逻辑

### 预检发现

DefaultLevel 用的是 `GM_Sandbox`：

- `DefaultPawnClass` = `/Game/Blueprints/SandboxCharacter_Mover`（GASP Mover 2.0 的第三人称角色）
- `PlayerControllerClass` = `/Game/Blueprints/PC_Sandbox`
- DefaultLevel 的 `PlayerStart` 在 `(-800, 0, 95.53)`

`DefaultEngine.ini.[/Script/EngineSettings.GameMapsSettings]` 当前**没有** `GlobalDefaultGameMode`——这是上一轮 NPC 改动留下的状态：玩家 Pawn 在 `L_prison` 里走引擎默认 GameMode（DefaultPawn 飞行摄像头）。**不能直接加 `GlobalDefaultGameMode`**，会破坏 `L_prison` 的飞行摄像头玩家。

### 方案决策：按关卡名前缀绑定 GameMode

UE 内建 `+GameModeMapPrefixes` 机制——按关卡名前缀匹配 GameMode override。比起 `GlobalDefaultGameMode`：

- 不影响 `DefaultLevel` / `L_prison`
- 不需要在 .umap 的 WorldSettings 里手点 `GameModeOverride`（MCP 的 `set_actor_properties` 只接受 6 个固定字段，写不了 `DefaultGameMode`，否则只能用户手点）
- 改动可控、可逆

最终一行：

```ini
+GameModeMapPrefixes=(Name="L_prison",GameMode="/Game/Blueprints/GM_Sandbox.GM_Sandbox_C")
```

### PlayerStart 位置

用户指定 `(0,0,0)`。`SpawnCollisionHandlingMethod=AlwaysSpawn`（PlayerStart 默认）保证不被 SM 卡住，PIE 时落到地面。

### 运行时数据流

```
PIE Start (L_prison)
  ↓
Engine 读 [GameMapsSettings] → 匹配 Name="L_prison" 前缀 → GameMode = GM_Sandbox
  ↓
GM_Sandbox.DefaultPawnClass = SandboxCharacter_Mover → 在 PlayerStart_Prison(0,0,0) spawn
PC_Sandbox 占用 → IMC_Sandbox 输入挂上 → WASD/Space/Shift 可用
```

---

## 反思

### 做对的事

- **预检 GameMode CDO 再决策**。`get_cdo_properties("GM_Sandbox")` 一次就拿到 DefaultPawnClass / PlayerControllerClass，确认 `SandboxCharacter_Mover` 就是要找的 GASP Mover 角色——不需要新建任何 BP
- **拒绝 `GlobalDefaultGameMode` 的诱惑**。最简单的"全局指一个 GameMode"会回退到 NPC 改造前的状态，破坏 `L_prison` 的玩家飞行摄像头。`+GameModeMapPrefixes` 按关卡前缀匹配，作用域精确
- **承认 MCP 的边界**。`set_actor_properties` 6 字段限制让 WorldSettings.GameModeOverride 写不动，没有硬试，直接走 INI 路由方案

### 踩过的坑

1. **`mesh_query.spawn_actor` 参数名是 `class_or_mesh`，不是 `actor_class`**。第一次调失败报 `Missing required param(s): [class_or_mesh]`，符合 CLAUDE.md 已记录的踩坑模式
2. **`mesh_query.set_actor_properties` 只接受 6 个固定字段**（mobility / simulate_physics / collision_preset / cast_shadow / tags / mass_kg）——WorldSettings.DefaultGameMode 写不进去
3. **Monolith MCP 没有"切换当前 Level"接口**。`get_level_actors` 的 `level_path` 参数其实被忽略，永远返回当前编辑器加载的 level。需要用户手动 File → Open Level 切到 L_prison 才能继续操作
4. **`+GameModeMapPrefixes` 是启动时读取的 INI**——改完 INI 必须重启编辑器才生效。备用路径是 World Settings → GameMode Override（写进 .umap，无需重启），但 MCP 写不了

### 值得沿用的模式

- **"按关卡名前缀绑 GameMode"**：项目里 `L_prison`（NPC 场景，飞行玩家）和 `L_prison`（跑图，第三人称）需要不同 GameMode，`+GameModeMapPrefixes` 按前缀路由是首选——比 `GlobalDefaultGameMode` 精确，比每个 .umap 手点 GameModeOverride 自动化
- **"MCP 写不了的字段走 INI"**：UE 很多 per-map 配置在 INI 里有等价机制（GameModeMapPrefixes / DefaultMaps / MapsToCookFor 等），MCP 在 set_actor_properties / set_cdo_property 卡壳时，先翻一下 EngineSettings 有没有等价 INI key

### 后续改进（未做）

- **PlayerStart 位置**：(0,0,0) 是临时位置，监狱场景里更合适的出生点（牢房门口 / 走廊起点）应在视图里 End 键贴地后挪过去
- **Navmesh**：`get_scene_statistics` 返回 `navmesh_status: no_navdata`。当前是玩家手动跑图，不需要 navmesh；但后续如果在 L_prison 里跑 NPC AI，需要 RecastNavMesh + Build Paths
- **L_prison 的 `Content/Prison/L_level/L_prison`**：项目里还有一份原始监狱关卡（`/Game/Prison/L_level/L_prison`）。当前用的是 `/Game/MyAssets/Levels/L_prison`，两者关系待澄清，前者可能是导入资产，后者是工程内副本

### 验证点

- 切换关卡到 L_prison → spawn PlayerStart_Prison(0,0,0) → Save Level → 改 DefaultEngine.ini → 重启编辑器 → PIE in L_prison
- 出生 `SandboxCharacter_Mover`（GASP Mover 2.0 第三人称角色）✓
- WASD 移动 / Space 跳 / Shift 冲刺等 IMC_Sandbox 输入工作 ✓
- 用户测试通过，已手动 git 提交
