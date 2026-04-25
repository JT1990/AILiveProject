# T10 — `Level_LiarsBar.umap` blockout + NavMesh

## 目标
新建骗子酒馆关卡：极简场景（吧台 + 中央扑克桌 + 4 把高凳），配 NavMesh，预放 4 个 NPC spawn 点 + 1 个 GameMaster Actor。**不做美术**——blockout 用 cube/cylinder，重点是空间布局和导航可达性。

## 前置
T01（NavigationSystem 模块就位）+ T19.7（`BP_PokerSeat_SmartObject` 已就位）

## DoD
- [ ] `Content/MyAssets/Level_LiarsBar.umap` 新建
- [ ] 场景包含：
  - 地板（plane 或 box，约 10×10m）
  - 中央扑克桌（box，约 1.2×1.2m，台面高 0.9m）
  - **4 个 `BP_PokerSeat_SmartObject`（T19.7）**围桌摆，作为 NPC 站位锚点（不可见标记 + SmartObjectComponent）
  - 4 把视觉用高凳（普通 StaticMesh，与 SmartSeat 同位置但不互相耦合）
  - 周围 4 面墙（box 简化）+ 顶光（DirectionalLight + SkyAtmosphere）
- [ ] `NavMeshBoundsVolume` 覆盖整个室内（按 P 键看到绿色 NavMesh）
- [ ] 4 个 `BP_NPC_MH_Character_2..5` 实例（保留 1 号给 sandbox），围桌站位（站立姿势，T10 不要求坐下）
- [ ] 1 个 `AMindGameMaster_LiarsBar` Actor（空 stub 即可，T12 实现内容），位置摆在场景外（视觉无关）
- [ ] PlayerStart 摆在房间一角，玩家飞行摄像头能看到全桌
- [ ] WorldSettings: `GameMode Override = None`（沿用全局默认 DefaultPawn）

## 关键文件
- 新建 `Content/MyAssets/Level_LiarsBar.umap`
- 用 Monolith MCP 完成所有摆放（**不要让用户手点**）

## 关键操作流程（MCP）

```
1. monolith_status
2. create_level(/Game/MyAssets/Level_LiarsBar)
3. spawn_actor(StaticMeshActor, mesh=Cube, scale=10x10x0.1, name=Floor)
4. spawn_actor(...) 桌子 + 4 凳子 + 4 面墙
5. spawn_actor(NavMeshBoundsVolume, scale=10x10x3)
6. spawn_actor(RecastNavMesh)（如果不自动加）
7. spawn_actor(BP_NPC_MH_Character_2_C, location=...) × 4
8. spawn_actor(AMindGameMaster_LiarsBar, location=远处)
9. spawn_actor(PlayerStart, location=房间一角)
10. set_world_settings(...)
11. save_level
12. build_navigation
```

## 关键参数（Blockout）

```
房间内尺寸：10 × 10 × 3m
桌子：(0,0,90) 大小 (120,120,10)
高凳 4 把：
  (-150, 0, 50) 大小 (40,40,90)  → 西
  (150, 0, 50)                    → 东
  (0, -150, 50)                   → 南
  (0, 150, 50)                    → 北
NPC spawn 站位：紧贴对应凳子外侧 30cm，朝向桌心
```

## 验收信号
- 编辑器加载 `Level_LiarsBar.umap` → 看到 4 NPC 围桌站立 + GameMaster 在角落
- 按 `P` 键能看到绿色 NavMesh 覆盖全室内
- PIE → 玩家飞起来能看到全场景
- 4 个 NPC BeginPlay 时各自调 `PrewarmA2F`（沿用现有逻辑）
- 此时 NPC 还没接 GameMaster，按 T 键无反应（因为 T07 只改了 NPC_1）

## 不在范围
- 美术贴图 / 材质（用引擎默认）
- 桌子上的扑克牌物件（M3 起再做表演道具）
- AMindGameMaster_LiarsBar 的实际逻辑（T12）

## 注意
- T19.7（SmartObjects）已在 M0 完成，本卡直接 spawn_actor `BP_PokerSeat_SmartObject`，**无返工**

## 风险
- NavMesh build 时编辑器卡顿（小场景应该秒级）
- BP_NPC_MH_Character 是子类化的 GASP NPC，spawn 后 BeginPlay 会 PrewarmA2F，4 个并发可能 GPU 短暂卡——可接受
- 若 NPC spawn 时位置贴墙，可能在 Floor 下面或穿模——动手时 Z 坐标加点裕量
