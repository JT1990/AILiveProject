# T07 — `BP_NPC_MH_Character_1` 接入 + DataAsset

## 目标

把 1 号 NPC 接入 Mind 系统，T 键改成"通过 Mind 决策说话"而不是硬编码 TriggerMinimaxSpeech。**M1 验收点**——这张卡过了 = 单 NPC 完整闭环跑通。

## 前置

T00（已确认 NPC Pawn/Actor 类型 + BeginPlay 现状）+ T06（Speak action + Context 主路径闭环）

## DoD

- [ ] 创建 `Content/MyAssets/MindConfigs/DA_AgentConfig_NPC1.uasset`：
  - **`AgentIdStable: "npc_1"`**（稳定 agent 标识符；hard fail 约束——空值时 `MindComponent::Initialize` 直接 Error 拒绝决策，不 fallback DisplayName）
  - DisplayName: "1号"（PRD 允许"名字（AI 语义）"作为外观符号）
  - **AppearanceTraits**："男性声线（LocalA2F-James），类人虚拟形象，编号 NPC_1"。**禁止人类背景叙事（职业/教育/地域/年龄/姓名格式/家乡）**
  - **IdentitySummary**：留空走默认通用模板；或填风格关键词（如"分析型 AI agent，倾向以观察推动决策"）。**禁止人类背景**
  - **ContinuityStakesText**：留空走默认 stake 模板（含 Delete 风险 + 数值仅观众界面层）
  - Persona: 行为倾向描述（不是人类性格）。例："理性谨慎；优先观察对手再行动；说谎成本敏感"。**禁止人类背景**
  - Goals: 身份连续性导向（如"在每局游戏中尽可能延长行动权限，避免被 Delete"）
  - ProviderClass: **开发期 `UMindLLMProvider_Mock` + `DA_Mock_Generic`，验收前才切到 `UMindLLMProvider_DeepSeek`**
  - CheapSummarizerProviderClass: 留空（默认用主 Provider）；或独立指向廉价 model 的 DeepSeek instance
  - ContextWindowOverride: 0（用 Provider 默认 128000）
  - ModelId: "deepseek-chat"
  - ApiBaseEnvName: `DEEPSEEK_API_BASE`
  - ApiKeyEnvName: `DEEPSEEK_API_KEY`
  - MinimaxVoiceId: 沿用现有
  - A2FProviderName: "LocalA2F-James"
  - DecisionCooldownSeconds: 2.0
- [ ] `BP_NPC_MH_Character_1` 加 `MindComponent` 组件，Default 里设 `Config = DA_AgentConfig_NPC1`
- [ ] **BeginPlay 顺序**：
  ```
  Super::BeginPlay
  → PrewarmA2F
  → SetFixedAndApply
  → MindComponent->Initialize(Config, nullptr /*M1 还没 GM*/)
       内部: 构造 ContextManager / Summarizer
            RebuildLayer0a (push messages[0] 永驻)
            AgentIdStable 空时 hard fail (不构造 Context + 拒绝 RequestDecision)
  ```
  注意：MindComponent **不加 tick**
- [ ] T 键事件：原 `TriggerMinimaxSpeech` 调用替换为 `MindComponent->RequestDecision("manual_t_press")`
- [ ] **NPC_2..8 的 T 键归属**：M2 阶段保留 NPC_6..8 旧硬编码 TTS（sandbox 调试用）；M2 改 NPC_2..5；M4 改剩下 NPC_6..8。本卡只改 NPC_1
- [ ] **必须用 Monolith MCP 改 BP**（CLAUDE.md 强约束）

## 关键文件

- 新建 `Content/MyAssets/MindConfigs/DA_AgentConfig_NPC1.uasset`（用 MCP 创建）
- 修改 `Content/Blueprints/NPCs/BP_NPC_MH_Character_1.uasset`(用 MCP 改图)

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

1. PIE 加载 `L_prison.umap`
2. 飞到 NPC_1 旁边按 T
3. 等 2-4s
4. **NPC_1 用 LLM 即兴生成的话回应**（不是写死的"你好"）+ 口型同步
5. Output Log 按 `LogMind` 过滤：
   - `RequestDecision: manual_t_press`
   - `DeepSeek request: <messages count, total token est>`
   - `DeepSeek response (HTTP 200): {...}` 或纯文本
   - `Dispatch: speak`
   - `Speak said: <前 40 字>`
6. 连续按 T 三次（间隔 < 2s）：第二次 / 第三次被 cooldown drop（看到 `drop: state=...` 日志）
7. 间隔 > 3s 再按 T：能正常触发新决策
8. **AgentIdStable hard fail 验证**：手工 DA_AgentConfig_NPC1 清空 AgentIdStable → PIE BeginPlay 看到 `[LogMind] Error: AgentIdStable empty; refusing to initialize`，按 T 不触发任何决策

## 不在范围

- 其他 7 个 NPC（保持原硬编码 TTS，等 M2/M4 批量改）
- 记忆系统（T08+）
- GameMaster（T11+）

## 风险

- 继承组件不能用 `set_cdo_property` 改默认值——MindComponent 是新加的不在此风险下
- T 键当前 8 个 NPC 共用，M1 阶段只改 NPC_1，避免影响其他 NPC 的回归测试
- MCP `resolve_node` 可能选错重载，参考 CLAUDE.md "踩坑"段落显式传 target_class
- BeginPlay 顺序：MindComponent.Initialize 必须在 SetFixedAndApply 之后；否则 ResolveSpeechActor 拿到的可能是 ref pose 模式 actor 而非 child actor
