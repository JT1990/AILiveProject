# T25 — 联盟可视化 Debug HUD

## 目标
做一个调试用的 3D 头顶 HUD：每 NPC 头上显示其当前认定的盟友编号 + 当前 RequestDecision 状态 + 最近一次 intent。**polish + 调试性，不影响游戏机制**。

## 前置
T24（多轮循环跑得通）

## DoD
- [ ] 创建 `WBP_NPCDebugBadge` UMG 3D widget（**用 Monolith MCP 创建 + 编辑图**，CLAUDE.md 强约束）：
  - NPC 名（如 NPC_3）
  - 盟友列表（"vs npc_5,7"）
  - 当前 Mind State（Idle / Building / Calling / Acting / Cooldown）
  - 当前 Intent（来自 `MindComponent.CurrentIntent`，由 T19 `Decision` Action 写入）
  - 当前 ContextManager 状态：buffer token 数 + bIsCompacting flag（debug 用）
- [ ] `BP_NPC_MH_Character` 父类加 `WidgetComponent`，挂 `WBP_NPCDebugBadge`，3D space，billboard 朝向相机
- [ ] WidgetComponent 默认隐藏；按 `F9` 键 toggle 全局可见性
- [ ] Tick 频率 2Hz（每 0.5s 更新一次，避免每帧刷新）
- [ ] 按 NPC 状态着色：Idle=灰 / Building/Calling=黄 / Acting=绿 / bIsCompacting=蓝
- [ ] 联盟连线（可选）：用 `DrawDebugLine` 在 alive NPC 之间画线，颜色对应"互相认定盟友" - F9 toggle
- [ ] **可选**: 增加 `F10` 键打开 NPC 历史详情面板，调 `MemoryService /memory/by_tag` debug 端点查指定 NPC 的历史 intent / public 发言（debug 用，不参与游戏决策）

## 关键文件
- 新建 `Content/MyAssets/UI/WBP_NPCDebugBadge.uasset`
- 修改 `BP_NPC_MH_Character`（父类）加 WidgetComponent
- F9 / F10 输入处理：在 PlayerController 或 GameMode 加 InputAction，触发广播事件给所有 NPC widget

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
      Name = NPC.Config.AgentIdStable
      State = MindComponent.State enum
      Intent = MindComponent.CurrentIntent
      BufferTokens = MindComponent.Context.EstimateTotalTokens()
      Compacting = MindComponent.Context.bIsCompacting
      Allies = (cast GM to MinorityRule) → Get player[me].AlliancePerception → join ","
    Update Color based on State + Compacting
```

```cpp
// 全局 toggle
// 在 BP_FlyCamera 或 PlayerController 加 InputAction "ToggleDebugHUD" (F9)
On F9 →
    For all BP_NPC_MH_Character actors in level:
        DebugBadge->SetVisibility(!visible)

On F10 →
    Open WBP_NPCHistoryPanel (按选中 NPC 调 /memory/by_tag debug 端点)
```

## 验收信号
- PIE Level_MinorityRule 跑多轮少数决
- 按 F9：每 NPC 头上出现一个浮动徽章
- 徽章上能看到 NPC 名 + 当前状态 + intent + buffer token 数
- Negotiate 阶段：能看到至少 2 NPC 的联盟标记互相指向
- 关键时刻（如 Vote 后）能从徽章看出谁背叛了谁（intent 显示 yes 但实际投 no）—— 通过观察很容易注意到
- Compact 触发瞬间：bIsCompacting=true 时徽章变蓝（持续 2-15s 直到 Summarizer 完成）

## 不在范围
- 美观度 polish（默认黑底白字够用）
- 玩家交互（除 F9 toggle 和可选 F10 历史面板）
- 多关卡通用 — 本卡只针对 Minority Rule，骗子酒馆暂不做（也可复用，不强求）

## 风险
- WidgetComponent 在 PIE 下 spawn 后初次显示可能有 1 帧空白，可接受
- 8 NPC 同时画 widget 性能 OK，但若加上 DrawDebugLine 联盟连线（C(8,2)=28 条线）每帧画会有性能影响 — 限制只画"互相认定"的真正盟友
- F9 / F10 与 GASP 现有按键无冲突（GASP 用 WASD/Space，F9/F10 安全）
- DebugBadge 显示的 `MindComponent.CurrentIntent` 字段需要 T19 `Decision` Action 实施时已经写入；如果 LLM 从未调 Decision Action，徽章 Intent 段会一直为空——这是预期行为
- F10 历史面板调 `/memory/by_tag` 是 debug 用，不影响游戏决策路径；如该端点未实现可降级为只读 Memory Service `/memory/event` 端点
