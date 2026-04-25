# T23 — 少数决单轮端到端 + HUD

## 目标
8 NPC 在 `Level_MinorityRule` 完整跑通一轮（出题 → 60s 谈判 → 投票 → 计票 → 淘汰），HUD 显示关键信息。**M4 总验收**。

## 前置
T22（少数决 Action）+ T20（关卡）+ T21（GM）

## DoD
- [ ] 8 NPC（NPC_1..8）全部接 MindComponent + 各自 DataAsset：
  - `DA_AgentConfig_NPC1` 已存在（M1 留），更新 persona 适合社交博弈
  - 新建 `DA_AgentConfig_NPC6..8`
  - 所有 8 个 Config 用 4 类 persona × 2（保守谈判家 / 激进者 / 联盟构建者 / 跟随者）
  - GM `Participants` 数组配 8 个 NPC 引用
- [ ] 创建 `WBP_MinorityRuleHUD` UMG widget（**用 Monolith MCP 创建 + 编辑图**，CLAUDE.md 强约束）：
  - 顶部：`Phase: <name>` + `Round 1` + `Negotiate Timer: 45s` 倒计时
  - 中部：`Question: <text>`
  - 中下：8 NPC 状态条（Alive 标记 + 钻石数 + 上次 RequestDecision 时间）
  - 底部：事件 log（最近 8 条公开事件）
- [ ] HUD 订阅 GM 的 OnPhaseChanged 和 OnGameEvent
- [ ] PIE 启动后 GM 自动 BeginPlay 调 StartGame
- [ ] 单轮跑通：到 Tally 阶段 HUD 显示票数 + 淘汰名单 → GameOver

## 关键文件
- 新建 `Content/MyAssets/UI/WBP_MinorityRuleHUD.uasset`
- 新建 `DA_AgentConfig_NPC6..8.uasset`
- 修改 `BP_NPC_MH_Character_1..8`（8 个全部加 MindComponent）— 用 MCP 批量改

## 关键操作（Monolith MCP 批量改 BP）

按 CLAUDE.md "改 BP 前预检"原则：

```
for i in 1..8:
    monolith_status
    get_components(BP_NPC_MH_Character_i)
    if has MindComponent: continue (M2 / M1 已加)
    add_component(BP_NPC_MH_Character_i, MindComponent)
    set_default(MindComponent.Config = DA_AgentConfig_NPCi)
    # 不在 BeginPlay 自己找 GM；GM.StartGame 会主动给所有 Participants Initialize
    compile_blueprint
```

GM 引用：在 GM Actor 的 Details 面板手填 `Participants` 数组（8 个 NPC 引用），GM.BeginPlay → StartGame → 给每个 Participant 调 Initialize。

## M4A 验收（单 DeepSeek，本卡）vs M4B（多厂商，T18.5）

**M4A**：8 NPC 全部用 DeepSeek（或 Mock + DeepSeek 混合）跑通少数决单轮。**不引入 GLM**——避免供应商账号、模型名、格式差异、ratelimit 同时上场。
**M4B**：T18.5 完成后，4 DeepSeek + 4 GLM 混搭跑一次。

## 验收信号（M4A 验收清单 - **分级验收**）

启动 Memory Service → PIE Level_MinorityRule：

### A 级（必须达成才算 M4 通过）
1. ✅ 加载后 ~3s，GM 自动 StartGame
2. ✅ HUD 顶部 `Phase: AskQuestion`，被抽中的 NPC 说出一个问题
3. ✅ HUD 中部 `Question: <文字>` 出现
4. ✅ `Phase: Negotiate` 进入，**至少 3 次** NPC 发言（哪怕都很短）
5. ✅ `Phase: Vote`，**至少 6/8 alive NPC 成功投票**（即使 1-2 个超时漏投）
6. ✅ `Phase: Tally`，HUD 显示 yes/no 票数
7. ✅ Memory Service 能 recall 出每 NPC 这一轮的记忆
8. ✅ 全程游戏线程不卡（看 stat fps，不掉到 30 以下）

### B 级（推荐达成，写到 DevLog 但不阻塞 M5 启动）
9. ✅ Negotiate 60s 内至少 1 次 ProposeAlliance 私聊
10. ✅ 全 8 NPC 都成功投票
11. ✅ Eliminate 阶段被淘汰 NPC Hidden 或退场
12. ✅ `Phase: GameOver` 显示完整赢家信息

### 观察项（写到 DevLog）
- 是否有 NPC 在 Negotiate 阶段表达"我们结盟一起投 yes"
- 票数分布是否合理（不是 8:0 或 0:8 这种异常）
- 私聊（ProposeAlliance）是否对应到投票一致性

### 协议入口指标（**必收集**）
- LLM 总数 / JSON parse fallback / Validate reject / Wait 续命 / HTTP 429
- 单局总耗时 / 平均决策延迟 / fps spike

## 不在范围
- 多轮循环（T24）
- 联盟可视化 HUD（T25）
- 私聊空间 attenuation（M5 polish）

## 风险
- 8 NPC 同时 Initialize 时 GM 可能还没就绪 — 在 NPC BeginPlay 用 Timer 0.1s 后再找 GM 重试
- Negotiate 阶段 LLM 调用并发：cooldown=2s + GM 每 10s 主动唤醒 1 个 NPC，其余靠节流自然 drop，整体 QPS 受控
- DeepSeek 在密集 60s 谈判内可能限流 — 监控 HTTP 429 日志，必要时 GM 调长 awake 间隔到 15s
- 8 个 persona 的差异化是否能让谈判产生有意义的结构 — 不能直接保证，靠 prompt 工程
