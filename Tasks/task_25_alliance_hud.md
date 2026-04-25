# T25 — 联盟可视化 Debug HUD

## 目标
做一个调试用的 3D 头顶 HUD：每 NPC 头上显示其当前认定的盟友编号 + 当前 RequestDecision 状态 + 最近一次 intent。**polish + 调试性，不影响游戏机制**。

## 前置
T24（多轮循环跑得通）

## DoD
- [ ] 创建 `WBP_NPCDebugBadge` UMG 3D widget（**用 Monolith MCP 创建 + 编辑图**，CLAUDE.md 强约束）：
  - NPC 名（如 NPC_3）
  - 盟友列表（"vs npc_5,7"）
  - 当前 Mind State（Idle/Thinking/Acting）
  - 当前 Intent（来自 `MindComponent.CurrentIntent`）
- [ ] `BP_NPC_MH_Character` 父类加 `WidgetComponent`，挂 `WBP_NPCDebugBadge`，3D space，billboard 朝向相机
- [ ] WidgetComponent 默认隐藏；按 `F9` 键 toggle 全局可见性
- [ ] Tick 频率 2Hz（每 0.5s 更新一次，避免每帧刷新）
- [ ] 按 NPC 状态着色：Idle=灰 / Thinking=黄 / Acting=绿
- [ ] 联盟连线（可选）：用 `DrawDebugLine` 在 alive NPC 之间画线，颜色对应"互相认定盟友" - F9 toggle

## 关键文件
- 新建 `Content/MyAssets/UI/WBP_NPCDebugBadge.uasset`
- 修改 `BP_NPC_MH_Character`（父类）加 WidgetComponent
- F9 输入处理：在 PlayerController 或 GameMode 加 InputAction，触发广播事件给所有 NPC widget

## 关键 API / 伪代码

```cpp
// BP_NPC_MH_Character (父类) 添加：
UPROPERTY(VisibleAnywhere) UWidgetComponent* DebugBadge;
// BeginPlay：
DebugBadge->SetWidgetClass(WBP_NPCDebugBadge);
DebugBadge->SetWidgetSpace(EWidgetSpace::Screen);  // billboard
DebugBadge->SetVisibility(false);
```

```cpp
// WBP_NPCDebugBadge BP 逻辑（伪）
Event Tick (每 0.5s) →
    Get owning NPC → MindComponent →
    Update Text:
      Name = NPC.Config.DisplayName
      State = MindComponent.State enum
      Intent = MindComponent.CurrentIntent
      Allies = (cast GM to MinorityRule) → Get player[me].AlliancePerception → join ","
    Update Color based on State
```

```cpp
// 全局 toggle
// 在 BP_FlyCamera 或 PlayerController 加 InputAction "ToggleDebugHUD" (F9)
On F9 →
    For all BP_NPC_MH_Character actors in level:
        DebugBadge->SetVisibility(!visible)
```

## 验收信号
- PIE Level_MinorityRule 跑多轮少数决
- 按 F9：每 NPC 头上出现一个浮动徽章
- 徽章上能看到 NPC 名 + 当前状态 + intent
- Negotiate 阶段：能看到至少 2 NPC 的联盟标记互相指向
- 关键时刻（如 Vote 后）能从徽章看出谁背叛了谁（intent 显示 yes 但实际投 no）— 通过观察很容易注意到

## 不在范围
- 美观度 polish（默认黑底白字够用）
- 玩家交互（点击徽章弹详情）
- 多关卡通用 — 本卡只针对 Minority Rule，骗子酒馆暂不做（也可复用，不强求）

## 风险
- WidgetComponent 在 PIE 下 spawn 后初次显示可能有 1 帧空白，可接受
- 8 NPC 同时画 widget 性能 OK，但若加上 DrawDebugLine 联盟连线（C(8,2)=28 条线）每帧画会有性能影响 — 限制只画"互相认定"的真正盟友
- F9 与 GASP 现有按键无冲突（GASP 用 WASD/Space，F9 安全）
