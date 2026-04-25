# T14 — HUD + 端到端验证（4 NPC 一局）

## 目标
做一个最简 UMG HUD 显示游戏状态，并整体验收"4 NPC 在 `Level_LiarsBar` 跑通骗子酒馆"。**M2 总验收（分级）**。

## 前置
T13（3 个 Action 实现）+ T12（GM 阶段机）+ T10（关卡）+ T07（NPC 接 Mind）

## DoD
- [ ] 创建 `WBP_LiarsBarHUD` UMG widget（**用 Monolith MCP 创建 + 编辑图，不要让用户手点编辑器**——CLAUDE.md 强约束）：
  - 顶部：当前 Phase + Round
  - 中央：当前出牌玩家 / 当前 claim（"npc_2 claimed 3 K"）
  - 底部：4 玩家状态条：名字 / chamber / alive 标记 / "上次行动"摘要
- [ ] HUD 订阅 `AMindGameMaster::OnPhaseChanged` 实时刷新
- [ ] HUD 订阅一个新 Multicast `OnGameEvent(FString)`（GM 发出 challenge / reveal / roulette_hit / agent_eliminated 等事件）
- [ ] 4 个 NPC（NPC_2..5）全部接 Mind：
  - 创建 `DA_AgentConfig_NPC2..5`，4 个不同 Persona（如：保守稳重 / 激进好斗 / 善欺善辩 / 保守跟风）
  - 给 4 个 NPC 加 MindComponent + Config 引用（用 MCP 批量改）
  - GameMaster.Participants 数组里填 4 个 NPC 引用
- [ ] PIE 启动后 GM 自动 `StartGame`（在 BeginPlay 调用）
- [ ] 完整跑一局：8-15 个回合后只剩 1 NPC alive，TransitionToPhase("GameOver")，HUD 显示获胜者

## 关键文件
- 新建 `Content/MyAssets/UI/WBP_LiarsBarHUD.uasset`
- 新建 `DA_AgentConfig_NPC2..5.uasset`
- 修改 `BP_NPC_MH_Character_2..5`（加 MindComponent + Config 引用）
- 修改 `AMindGameMaster_LiarsBar`：BeginPlay 自动调 StartGame；新增 `FOnGameEvent` delegate

## 关键操作（Monolith MCP 批量）

```
# 4 个 DA
build_asset(UMindAgentConfig, /Game/MyAssets/MindConfigs/DA_AgentConfig_NPC2)
set_cdo_property(... Persona="保守稳重..." ...)
... NPC3 NPC4 NPC5 同样

# 4 个 BP（按 CLAUDE.md 预检后批量）
for i in 2..5:
    add_component(BP_NPC_MH_Character_i, MindComponent)
    set_default(MindComponent.Config = DA_AgentConfig_NPCi)
    # BeginPlay 加 Initialize 调用
compile_blueprint
```

## 关键 API（HUD）

```cpp
// HUD 在 BP 里实现
Event Construct → bind to WorldGM->OnPhaseChanged / OnGameEvent
On Phase Changed → Refresh Top Bar
On Game Event → Append to Event Log
```

## 验收信号（M2 验收清单 - **分级验收**）

启动 Memory Service → PIE Level_LiarsBar：

### A 级（必须达成才算 M2 通过）
1. ✅ HUD 顶部立刻显示 `Phase: Setup`
2. ✅ 几秒内 HUD 切到 `Phase: PlayerTurn (npc_2)`，npc_2 喊出 `"我打了 X 张 Y"`（口型同步）
3. ✅ 进入 ChallengeWindow 至少一次（要么有人 challenge，要么 auto-reveal）
4. ✅ Reveal 阶段 HUD 显示真假；Roulette 阶段触发（命中或没命中都行）
5. ✅ **至少跑通 3 个完整回合**（PlayerTurn → Challenge/Skip → Reveal → Roulette → CheckWin → 下一回合）
6. ✅ Memory Service 里能查到这 3 回合的 action 记录
7. ✅ Output Log 完整链路无 Error（Warning 可有）

### B 级（推荐达成，写到 DevLog 但不阻塞 M3 启动）
8. ✅ 完整跑一局直到 alive=1（如 LLM ratelimit 或 JSON 漂移导致中断，记录在 DevLog 作为 M3 改进点）
9. ✅ HUD 每个 NPC 状态条 chamber 数随失败正确递减
10. ✅ HUD 显示 `Phase: GameOver` + `Winner: npc_X`

### 观察项（不强求，记录到 DevLog）
- 不同 persona 的 LLM 是否表现出可观察的策略差异
- 谎称频率 / 挑战频率 / 平均存活回合数

**说明**：A 级通过即可启动 M3 + T14.5 性能基线测量。B 级如果未通过，T17 的 5 局压测是天然的复检机会。

## 不在范围
- 表演动画（T15）
- 跨局记忆引用（T16）
- 模型差异化（T16）

## 风险
- LLM 调用并发 4 路：DeepSeek QPS 在 ChallengeWindow 阶段会瞬时 4 次调用——账号 tier 限制可能命中。回退：阶段内串行（一次唤醒一个）
- 一局可能太长（每回合 LLM 调用 ×2 ≈ 4-8s，10 回合 ≈ 1 分钟）— 接受
- 8 NPC 中只有 4 个接 Mind，其他 4 个保留原硬编码 T 键 TTS——避免影响 sandbox
- HUD 数据源：通过 GM 直接 GetWorld 找 actor，简化设计；如果未来支持 split-screen 需要重做
