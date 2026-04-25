# T07 — `BP_NPC_MH_Character_1` 接入 + DataAsset

## 目标
把 1 号 NPC 接入 Mind 系统，T 键改成"通过 Mind 决策说话"而不是硬编码 TriggerMinimaxSpeech。**M1 验收点**——这张卡过了 = 单 NPC 完整闭环跑通。

## 前置
T00（已确认 NPC Pawn/Actor 类型 + BeginPlay 现状）+ T06（Speak action 闭环）

## DoD
- [ ] 创建 `Content/MyAssets/MindConfigs/DA_AgentConfig_NPC1.uasset`：
  - **`AgentIdStable: "npc_1"`**（稳定 agent 标识符，跨关卡 / PIE 重启不变；记忆系统的 agent_id 派生自此字段，不用 GetName）
  - DisplayName: "1号"
  - Persona: 一段中文人格描述（约 100 字）
  - Goals: 简短目标（如"在游戏中存活并赢得最多筹码"）
  - ProviderClass: **开发期建议先用 `UMindLLMProvider_Mock` + `DA_Mock_Generic`（T05.5），验收前才切到 `UMindLLMProvider_DeepSeek`**
  - ModelId: "deepseek-chat"
  - ApiBaseEnvName: `DEEPSEEK_API_BASE`（值在 .env 中: https://api.deepseek.com/v1）
  - ApiKeyEnvName: `DEEPSEEK_API_KEY`
  - MinimaxVoiceId: 沿用现有
  - A2FProviderName: "LocalA2F-James"
  - DecisionCooldownSeconds: 2.0
- [ ] `BP_NPC_MH_Character_1` 加 `MindComponent` 组件，Default 里设 `Config = DA_AgentConfig_NPC1`
- [ ] **BeginPlay 顺序明示**：
  ```
  Super::BeginPlay
  → PrewarmA2F                    # 现有
  → SetFixedAndApply               # 现有 VisualOverride 链路
  → MindComponent->Initialize(Config, nullptr /*M1 还没 GM*/)   # 新增；放在 VisualOverride 之后
  ```
  注意：MindComponent **不加 tick**，事件驱动不需要
- [ ] T 键事件：原 `TriggerMinimaxSpeech` 调用替换为 `MindComponent->RequestDecision("manual_t_press")`
- [ ] **NPC_2..8 的 T 键归属**：M2 阶段保留 NPC_6..8 旧硬编码 TTS（sandbox 调试用）；M2 改 NPC_2..5；M4 改剩下 NPC_6..8。本卡只改 NPC_1
- [ ] **必须用 Monolith MCP 改 BP**（CLAUDE.md 强约束），不要让用户手点

## 关键文件
- 新建 `Content/MyAssets/MindConfigs/DA_AgentConfig_NPC1.uasset`（用 MCP 创建）
- 修改 `Content/Blueprints/NPCs/BP_NPC_MH_Character_1.uasset`（用 MCP 改图）

## 关键操作（Monolith MCP 流程）

按 CLAUDE.md "Monolith MCP 踩坑" + "改 BP 前预检"原则：

```
1. monolith_status                           # 确认编辑器在线
2. get_components(BP_NPC_MH_Character_1)     # 看现有组件
3. get_graph_summary / get_execution_flow    # 看 BeginPlay 和 T 键现状
4. add_component(MindComponent) 到子类       # 不能改父类
5. set_cdo_property(组件 default Config)     # 注意继承组件需绕 setter；新组件可直接设
6. 改 T 键节点：连到 MindComponent.RequestDecision；删掉旧 TriggerMinimaxSpeech 调用
7. compile_blueprint
```

DataAsset 创建：
```
build_asset(class=UMindAgentConfig, path=/Game/MyAssets/MindConfigs/DA_AgentConfig_NPC1)
set_cdo_property(...) 设各字段
```

## 验收信号（M1 总验收）

1. PIE 加载 `Level_AILive.umap`
2. 飞到 NPC_1 旁边按 T
3. 等 2-4s
4. **NPC_1 用 LLM 即兴生成的话回应**（不是写死的"你好"）+ 口型同步
5. Output Log 按 `LogMind` 过滤：
   - `RequestDecision: manual_t_press`
   - `DeepSeek request: ...`
   - `DeepSeek response (HTTP 200): {...}` 或者纯文本
   - `Dispatch: speak`
   - `Speak said: <前 40 字>`
6. 连续按 T 三次（间隔 < 2s）：第二次 / 第三次被 cooldown drop（看到 `drop: state=...` 日志）
7. 间隔 > 3s 再按 T：能正常触发新决策

## 不在范围
- 其他 7 个 NPC（保持原硬编码 TTS，等 M2/M4 批量改）
- 记忆系统（T08+）
- GameMaster（T11+）

## 风险
- 继承组件不能用 `set_cdo_property` 改默认值（CLAUDE.md 已记录）——MindComponent 是新加的，不在此风险下；但 `Config` 指向 DataAsset 的引用如果加在父类需绕 setter
- T 键当前 8 个 NPC 共用（CLAUDE.md），M1 阶段只改 NPC_1，避免影响其他 NPC 的回归测试
- MCP `resolve_node` 可能选错重载，参考 CLAUDE.md "踩坑"段落显式传 target_class
- AgentIdStable 字段如果忘填，回退到 DisplayName.ToString() 但记日志 Warning（避免静默 GetName 后缀坑）
- BeginPlay 顺序如果 MindComponent.Initialize 在 SetFixedAndApply 之前，VisualOverride 还没 spawn 子 actor 时调用 Mind 没影响——但保持文档化的顺序减少未来扩展时的不确定性
