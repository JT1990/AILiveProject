# T20 — `Level_MinorityRule.umap` blockout + NavMesh

## 目标
新建 8 人版少数决关卡：客厅 + 圆桌 + 8 椅 + 投票箱（左右各一，象征 Yes/No） + NavMesh。沿用 T10 的 blockout 风格。

## 前置
T01（NavigationSystem）+ T19.7（SmartObjects 基础已就位，直接 spawn 用）

## DoD
- [ ] `Content/MyAssets/Level_MinorityRule.umap` 新建
- [ ] 场景包含：
  - 客厅地板 ~15×15m
  - 圆桌（cylinder，直径 3m，台面高 0.9m）
  - **8 个 `BP_Chair_SmartObject`（T19.7）** 围桌等距摆放
  - **2 个 `BP_VoteBox_SmartObject`（T19.7）**：1 个 tag `yes` 一侧 / 1 个 tag `no` 另一侧（约离桌 5m）
  - 上方一个"问题展示牌"（plane + 后续可挂 text widget）
  - 4 面墙 + 灯光
- [ ] NavMeshBoundsVolume 覆盖整个客厅
- [ ] 8 个 NPC spawn 点（用 `BP_NPC_MH_Character_1..8`）— 注意 1 号也要接进来（M4 起所有 8 个都接 Mind）
- [ ] 1 个 `AMindGameMaster_MinorityRule` Actor
- [ ] PlayerStart 在房间一角（飞行摄像头可俯瞰全场）

## 关键文件
- 新建 `Content/MyAssets/Level_MinorityRule.umap`
- 用 Monolith MCP 完成所有摆放

## 关键参数

```
房间内尺寸：15 × 15 × 4m
圆桌：(0,0,90) 半径 150
8 把 BP_Chair_SmartObject：等距分布，半径 220 cm 处朝桌心，朝角 0/45/90/.../315°
NPC 初始站位：靠近自己的椅子，BeginPlay 后 SitDown action 自动入座
投票箱：
  BP_VoteBox_SmartObject(tag=yes): (500, 0, 30) 绿色材质
  BP_VoteBox_SmartObject(tag=no):  (-500, 0, 30) 红色材质
```

## 前置依赖
T19.7（SmartObjects）已在 M0 完成；本卡直接 spawn_actor 投票箱 + 椅子 SO，**无返工**。

## 验收信号
- 加载 `Level_MinorityRule.umap`：8 NPC 围圆桌站立 + 2 个投票箱可见
- `P` 键看到 NavMesh 覆盖整个客厅 + 投票箱周边
- PIE 飞行能俯瞰全场
- BeginPlay 8 NPC 各 PrewarmA2F

## 不在范围
- "问题展示牌"上挂 widget（T22 实现）
- 8 NPC 接 MindComponent（T22）
- AMindGameMaster_MinorityRule 内逻辑（T21）
- 美术细节

## 风险
- 8 NPC PrewarmA2F 并发 GPU/CPU 短时占用大，PIE 启动可能 5-10s 卡顿（一次性，可接受）
- 投票箱与圆桌之间路径要 NavMesh 连通——blockout 时如果有遮挡（柱子）需避开
