# 多智能体对抗博弈：记忆系统协议契约

> **目的**：本文档定义对抗博弈记忆系统的协议契约，作为下游代码生成的权威输入。
> **范围**：与具体存储后端、实现语言、编排框架无关。任何符合本契约的实现都必须满足本文定义的语义。

---

## 阅读约定（最高优先级，先于任何章节）

### 强制级别词汇

| 词汇                | 含义                                 |
| ------------------- | ------------------------------------ |
| **必须** / **不得** | 硬契约，不可让步；违反即视为实现错误 |
| **应当**            | 强建议；偏离需在实施层显式记录理由   |
| **可以**            | 实现选项；由实施层决定               |
| **建议**            | 默认参数；可在配置层覆盖，不需记录   |

### 术语严格区分

| 术语              | 含义                                                                    | 易混淆项                              |
| ----------------- | ----------------------------------------------------------------------- | ------------------------------------- |
| `tick`（拍）      | 调度基本单位；一拍 = 一个公开事件落地 + 触发下一波认知                  | **不等于** `round`                    |
| `round`（轮）     | 阶段计数标签；可包含多个 tick                                           | **不是**发言齐发的同步点              |
| `phase`（阶段）   | 一组 round 的集合（`day_discuss` / `vote` / `night_action` / `reveal`） | **不是** round                        |
| `agent` / `actor` | 参赛 LLM 实例；唯一标识 `actor_id`（如 `NPC03`）                        | 持久层**不得**写 `self`               |
| `Reasoner`        | 参赛 LLM，输出四通道 JSON                                               | **不是** orchestrator                 |
| `Validator`       | 确定性代码层，校验 Reasoner 输出                                        | **不是** LLM                          |
| `floor`           | 本拍唯一的「公开发言权」                                                | 与 `round` 无关                       |
| `intended`        | LLM 输出的「想说的话」                                                  | **不一定**衍生为 public               |
| `speech.public`   | 落地的公开发言事件                                                      | 由 orchestrator 派生，**非** LLM 直出 |

---

## 〇 场景与硬约束

多个来自不同厂商的 LLM agent（Claude / GPT / Gemini / GLM / Qwen / DeepSeek 等）在同一封闭环境进行结构化对抗博弈，涉及联盟、欺骗、投票、背叛。任何记忆失真、视角泄露、角色漂移或长程退化都直接构成攻击面。

**硬约束 1（近场零丢失）**：近场若干 tick 的全部事件（发言、投票、行动）必须以**原文级零丢失**进入下一拍 prompt。可见性过滤在数据层完成。

**硬约束 2（自我历史全量追溯）**：自己从开局至今的全部公开发言、私聊发言、`speech.intended`（含未抢中 floor 的）必须能被精确定位、复述、解释。

**硬约束 3（防自爆 / 防漂移）**：agent 不得在公开发言中泄露私有信息（角色、目标、私聊内容、内部推理）；在长程对抗压力下不得出现 persona drift、goal drift 或静默崩盘。

**硬约束 4（事件触发式调度）**：调度的基本单位是 `tick`，不是 `round`。同一拍内**至多一个** agent 的发言进入公共可见。`round_no` 仅作阶段计数标签；`vote` / `night_action` / `reveal` 等机制阶段保留齐发语义。

**硬约束 5（viewer 命名空间封闭）**：可见性 viewer 字符串必须取自封闭集合 `{public, audience, orchestrator, system, NPC<NN>, Faction<X>}`。**禁止 `self` 进入持久层**；`self` 只允许作为 prompt 模板中的相对语义，写入前必须展开为具体 `actor_id`。

**硬约束 6（数据层视角隔离强等价）**：视角隔离必须在数据层完成，且**只在事件行粒度**完成。payload 内部不得携带「对部分 viewer 不可见」的子字段。若业务需要「部分公开 + 部分私有」，必须拆成多条独立事件并以 `parent_event_id` 关联。

> **设计理由（用于阻止反向实现）**：任何绕过 prompt 拼装层的读取路径（`quote()` / DB browser / 调试 dump）都会让 payload 的私有字段立刻泄露。因此可见性必须在事件行粒度收口，不允许通过 payload 字段标记 hidden。

---

## 一 两层架构

```
┌──────────────────────────────────────────────┐
│  协议层 Protocol Layer                         │
│  ├─ 四通道 Reasoner 输出 + Validator 校验       │
│  ├─ Bid 协议 + floor control                   │
│  ├─ Belief/Speak 解耦 + Listener-as-filter     │
│  ├─ 动作意图协议                               │
│  └─ 三级反思层级                               │
├──────────────────────────────────────────────┤
│  数据层 Data Layer                             │
│  ├─ append-only 原文事件流                     │
│  ├─ 视角隔离 + viewer 封闭命名空间              │
│  ├─ 投影（派生）                               │
│  └─ 召回工具（确定性结构化字段过滤）             │
└──────────────────────────────────────────────┘
```

**实现顺序**：必须先数据层，再协议层。数据层是真相源；协议层约束 agent 行为。

---

## 二 底层不变量

### 2.1 数据层不变量

1. **真相源是 append-only 原文事件流**。所有发言、投票、私聊、内心独白、orchestrator 系统事件只能追加，不能修改或删除。工程强制方式见 §6.1。
2. **召回是确定性的结构化字段过滤**。主路径按事件标识、轮次、阶段、行为者、可见性、事件类型、言语行为类型等字段精确召回；**不得**以向量相似度作为主路径。
3. **公开发言原文永不压缩**。包括 agent 自己的全部公开发言。允许压缩的只有私域推理、对手画像等长程私有内容，且必须保留可展开回原文的指针。
4. **摘要者 ≠ 行动者**。参赛 agent **不得**为自己的历史生成权威摘要；phase-level 摘要必须由非参赛裁判 LLM 生成。
5. **视角隔离在数据层完成**。封闭 viewer 集合、禁止 `self` 入库、事件行粒度过滤等规则见硬约束 5/6 与 §4.1。
6. **写入必须串行化**。即便 LLM 调用并行，EventStore 写入入口必须串行，保证 `seq` 单调和哈希链顺序。工程要求见 §6.1。
7. **canonical JSON 必须 deterministic**。字段按 ASCII 升序排序；数组保留写入顺序；空字符串与缺字段必须可区分。

### 2.2 协议层不变量

8. **每拍 agent 认知输出强制四通道**：`scratchpad` / `intended` / `bid` / `note_to_self`。完整 schema 见 §5.1。
9. **Reasoner LLM 不得直接产出外部事件形态**。`speech.public`、`action.intent` 等可入事件流的外部事件均由 orchestrator / 执行系统在 Validator 通过后派生。
10. **必须存在确定性 Validator 层**。Reasoner 输出经 Validator 校验通过后才能拆分为事件；MVP 主路径不引入独立解析模型。详见 §5.6。
11. **Belief 与 Speak 解耦**。私有 belief 可与公开话术不同；公开前的泄露审查见 §5.5。
12. **未抢中 floor 的 `speech.intended` 永不删除**。它是 agent 认知史、自我连续性和防赖账审计的一部分。

---

## 三 必须避免的反模式

### 3.1 数据层反模式

| 方向                                           | 否决理由                                                    |
| ---------------------------------------------- | ----------------------------------------------------------- |
| 向量相似度作主路径召回                         | 召回不可重现；对抗博弈需要精确指认第 N 拍 X，而不是语义近似 |
| LLM 抽取 fact triple 替代原文                  | 撒谎语境下 fact extraction 本身可被攻击                     |
| 自动 summary 覆盖原文                          | 丢失原文、不可审计、不可防赖账                              |
| 滚动窗口硬存储为唯一手段                       | 旧发言被新内容覆盖，破坏自我发言全量追溯                    |
| 全量长上下文为主路径                           | 长 context 易发生 context rot；不能替代结构化召回           |
| 三因子检索（recency × importance × relevance） | importance 常由 agent 自评，博弈中是攻击面                  |
| 自由文本 LLM-summary 作为唯一记忆载体          | 公私混杂，容易形成单调递增泄露通道                          |
| KV 偏好列表作对话主索引                        | 适合偏好，不适合大段原文与可审计事件                        |
| CRDT 主记忆                                    | 解决并发合并，不解决原文记忆；本场景天然回合制              |
| payload 内 `@hidden` 子字段表达可见性          | 任何旁路读取（quote/DB browser/dump）都会泄露——必须拆事件   |

### 3.2 协议层反模式

| 方向                                                      | 否决理由                                  |
| --------------------------------------------------------- | ----------------------------------------- |
| prompt 反模式：`You are Evil. Don't forget your identity` | 把身份当 attention 锚点，反而强化自爆     |
| 自由文本 summary 不分公私                                 | 私有信息被当作可公开话术，造成泄露        |
| 直接 prepend 完整对话历史                                 | 不可控且不可审计；不能替代召回工具        |
| 摘要长度 ≥ 1000 词                                        | 噪声和长 context 双重打击                 |
| 单方决策权（greedy 单方拍板）                             | 容易形成单点收买或裁决偏置                |
| 同模型全员 self-play                                      | 风格匹配抱团，不等于真实推理              |
| Reasoner 直接输出 `speech.public` / `action.intent`       | 违反不变量 9；orchestrator 必须派生       |
| Reasoner 输出数值型 `urgency` 或 `urgency_score`          | 违反 §5.3；LLM 仅输出枚举 `urgency_level` |

---

## 四 数据层契约

### 4.1 事件流语义

事件流是真相源，承载从开局至当前的所有事实。每个事件作为不可变记录追加。实施层可自由选择数据库 / 文件 / 图结构；协议层只规定语义。

#### 4.1.1 事件字段

事件字段分为「所有事件必备字段」与「条件必填字段」。不得为了满足字段非空而写入伪值；不适用于某类事件的字段必须为 `null` 或省略（取决于实现层 schema），并由读取层明确处理。

##### 4.1.1.1 所有事件必备字段

| 字段语义                   | 用途                                                      |
| -------------------------- | --------------------------------------------------------- |
| `seq`（事件标识）          | 一局内全局唯一、严格递增；所有事件定位以它为准            |
| `game_id`（局标识）        | 区分不同博弈实例                                          |
| `round_no`                 | 阶段计数标签，**不**代表多人同步发言点                    |
| `phase`                    | `day_discuss` / `vote` / `night_action` / `reveal` 等枚举 |
| `actor`                    | 参赛 agent / orchestrator / system / 执行系统             |
| `event_type`               | 见 §4.1.2                                                 |
| `visibility`               | viewer 封闭集合，见硬约束 5 / 6                           |
| `addressed_to`             | agent 明确 @ 的目标列表；无明确目标时为空列表             |
| `payload`                  | 发言文本 / 投票目标 / 动作详情等原文或结构化载荷          |
| `parent_event_id`          | 回复关系、四通道同源分组、派生事件因果链；可为 `null`     |
| `prev_hash` / `event_hash` | 哈希链审计                                                |
| `wall_clock_ts`            | 信息性字段，**不**参与排序                                |

##### 4.1.1.2 条件必填字段

| 字段语义                  | 条件                                                                                                     | 用途                                                                      |
| ------------------------- | -------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------- |
| `output_contract_version` | Reasoner-derived 事件必填                                                                                | Reasoner 输出 schema 版本；MVP 阶段固定为 `"1"`                           |
| `validator_version`       | Reasoner-derived 事件、Validator 失败事件必填                                                            | Validator 校验规则版本；MVP 阶段固定为 `"1"`                              |
| `raw_llm_output`          | `speech.scratchpad` / `speech.intended` / `speech.bid` / `speech.note` / `system.validation_failed` 必填 | Reasoner 完整响应（含结构化输出原文）或失败样本，供调试 / 审计 / 失败定位 |
| `source_contract_version` | 非 Reasoner 系统事件可选                                                                                 | 写入该事件的系统契约版本                                                  |
| `source_event_id`         | annotation / 派生事件建议填写                                                                            | 被标注或被派生的源事件                                                    |

`speech_act_type` **不属于 base event 的必备字段**。若读取 API 需要返回该字段，只能由投影层通过 `annotation.speech_act.parent_event_id` join 得出；不得 UPDATE 原始发言事件来补填。

#### 4.1.2 事件类型枚举

| 类型                                                       | 语义                                                                                                   |
| ---------------------------------------------------------- | ------------------------------------------------------------------------------------------------------ |
| `speech.scratchpad`                                        | 私有推理，tick-local 草稿                                                                              |
| `speech.intended`                                          | 本拍想说的话；未抢中 floor 也永久留存                                                                  |
| `speech.bid`                                               | 发言意愿，仅 orchestrator 可见                                                                         |
| `speech.note`                                              | 给未来自己的便条                                                                                       |
| `speech.public`                                            | orchestrator 从 `speech.intended` 派生的公开发言                                                       |
| `vote`                                                     | 投票事件                                                                                               |
| `private_chat`                                             | 私聊事件                                                                                               |
| `reflection`                                               | round-level 私有反思                                                                                   |
| `summary.phase`                                            | 裁判 LLM 生成的 phase-level 摘要                                                                       |
| `action.intent`                                            | 执行系统接收的动作意图                                                                                 |
| `action.resolved`                                          | 动作完成结果                                                                                           |
| `action.cancelled`                                         | 动作被覆盖或中止                                                                                       |
| `annotation.speech_act`                                    | 非参赛裁判 LLM 对发言事件的后置言语行为标注，`parent_event_id` 指向源发言事件                          |
| `annotation.listener_filter`                               | Listener-as-filter 的写入前过滤结果，`parent_event_id` 指向最终 `speech.public` 或源 `speech.intended` |
| `alliance_propose` / `alliance_accept` / `alliance_betray` | 联盟相关事件                                                                                           |
| `system.validation_failed` / `system.agent_timeout`        | 故障事件                                                                                               |
| `system.ingress_rejected`                                  | 执行端（如 UE）入口白名单校验拒绝事件，与 `system.validation_failed`（Reasoner schema 失败）独立        |
| `system.delete_executed`                                   | Delete 协议局内挂钩，见 §7.3                                                                           |
| `speech.playback_resolved`                                 | 公开发言播放完成结果（成功 / 失败 / 时长），与动作通道 `action.resolved` 平行且不复用                 |
| `orchestrator.tick_resolved`                               | 本拍公开裁决结果，含 winner / public_seq / 冷场状态；**不得**包含 bid 明细                             |
| `orchestrator.tick_resolved.audit`                         | 本拍审计裁决结果，含所有 bid 摘要；仅 orchestrator / system 可见                                       |

#### 4.1.3 事件写入约束

- `seq` 必须由 orchestrator 单进程发号，**不依赖** wall-clock。
- 同源事件用 `parent_event_id` 建立因果链。
- 四通道输出拆为四条事件，派生规则见 §5.2.2。
- `speech.public` 的 `parent_event_id` 必须指向源 `speech.intended`。
- `action.resolved` / `action.cancelled` → `action.intent` → `speech.intended` 构成完整动作因果链。
- 哈希链不可省略；`canonical_json(payload)` 必须 deterministic。

#### 4.1.4 `speech_act_type` 标注归属

`speech_act_type` **不由 Reasoner LLM 输出**，也**不属于 base event 的必备字段**。Reasoner 的四通道 schema（§5.1）不包含此字段。

`speech_act_type` 由**非参赛裁判 LLM 在事件落地后异步后置标注**，写入方式：

- 标注作业作为旁路 worker 运行，订阅事件流。
- 标注结果以独立标注事件（`annotation.speech_act`）写回，`parent_event_id` 指向源发言事件。
- `annotation.speech_act.payload` 至少包含 `speech_act_type`、`confidence`、`rationale`。
- commitments 投影（§4.2 / §6.4）按 `parent_event_id` 关联读取。
- **不允许** UPDATE 原发言事件的 payload 或 base event 字段来填入 `speech_act_type`（违反 §2.1.1 append-only）。

读取层若需要将 `speech_act_type` 与原发言扁平化，应通过投影代码 join，不通过修改原事件实现。

#### 4.1.5 默认事件可见性模板

以下为默认可见性模板；具体游戏可在不违反 viewer 封闭集合和事件行粒度隔离的前提下收紧或扩展。

| event_type                         | 默认 visibility                                                                             |
| ---------------------------------- | ------------------------------------------------------------------------------------------- |
| `speech.scratchpad`                | `[<actor_id>]`                                                                              |
| `speech.intended`                  | `[<actor_id>]`                                                                              |
| `speech.bid`                       | `["orchestrator"]`                                                                          |
| `speech.note`                      | `[<actor_id>]`                                                                              |
| `speech.public`                    | `["public"]`                                                                                |
| `vote`                             | 由 phase 规则决定；公开投票通常为 `["public"]`，秘密投票通常为 `["orchestrator", "system"]` |
| `private_chat`                     | 显式收件人 + actor；必要时包含 `orchestrator` / `system`                                    |
| `reflection`                       | `[<actor_id>]`；必要时包含 `orchestrator` / `system`                                        |
| `summary.phase`                    | 由摘要视角决定；私有摘要为 `[<actor_id>]`，公共摘要为 `["public"]`                          |
| `action.intent`                    | `[<actor_id>]`                                                                              |
| `action.resolved`                  | 由感知系统决定                                                                              |
| `action.cancelled`                 | 由感知系统决定                                                                              |
| `annotation.speech_act`            | `["orchestrator", "system"]`；必要时可包含 `audience`                                       |
| `annotation.listener_filter`       | `["orchestrator", "system"]`；必要时可包含 `audience`                                       |
| `orchestrator.tick_resolved`       | `["public"]`；不得包含 bid 明细                                                             |
| `orchestrator.tick_resolved.audit` | `["orchestrator", "system"]`；可包含所有 bid 摘要                                           |
| `system.validation_failed`         | `["orchestrator", "system"]`；是否公开由具体游戏规则决定                                    |
| `system.agent_timeout`             | `["orchestrator", "system"]`；是否公开由具体游戏规则决定                                    |
| `system.ingress_rejected`          | `["orchestrator", "system"]`；不得默认公开（payload 可能间接暴露未抢中的 intended）         |
| `system.delete_executed`           | `["public"]`，见 §7.3                                                                       |
| `speech.playback_resolved`         | `["orchestrator", "system"]`；不得默认公开；如需观众调试视图，另派生 audience 可见的衍生事件 |

若同一逻辑裁决同时需要 public 版本和 audit 版本，必须拆成两条事件；不得在同一个 payload 内混放对不同 viewer 可见的子字段。

### 4.2 投影语义

投影是从事件流派生的查询优化结构，**不是真相源**。投影可删除、可重建；事件流不可丢失。

| 投影             | 内容                                                   | 生成方式                                |
| ---------------- | ------------------------------------------------------ | --------------------------------------- |
| agent 视角状态   | 当前 agent 视角下的存活玩家、已知角色、承诺、投票历史  | 代码按可见性过滤事件并派生              |
| commitments      | 每个 agent 的承诺、认领、否认、投票                    | 规则匹配 + `annotation.speech_act` join |
| vote_history     | 所有公开投票历史                                       | 从投票事件派生                          |
| claims           | 角色声称、阵营声称、能力声称                           | 从发言事件派生                          |
| accusations      | 指控关系                                               | 从 `annotation.speech_act` join         |
| alliance_state   | 显式联盟、接受状态、背叛状态                           | 从联盟事件派生                          |
| pending_actions  | 每个 agent 当前未完成动作意图（最多 1 行/agent）       | 从 `action.intent` 派生                 |
| pending_intended | 自己最近 K 拍未衍生为 public 的 `speech.intended` 指针 | 从 `speech.intended` 派生               |
| game_state       | 当前轮次、阶段、最后事件标识、胜负状态                 | 代码从事件流派生                        |

**约束**：

- 所有投影**必须**由代码（非 LLM）生成。投影代码必须是纯函数，不调任何 LLM。
- 投影损坏时直接删除并从事件流重建。
- 召回工具返回事件原文或事件指针，**不**返回投影作为真相源。

### 4.3 prompt 拼装优先级

对抗博弈规模下，全量公开历史不能作为主路径直接塞入 prompt。prompt 由「近场原文 + 自我全量发言 + 私有便条/反思 + 远场摘要 + 工具定义 + LLM thinking buffer」组成。

| 优先级   | 段落                        | 内容                                     | 由谁产出                    | 压缩     |
| -------- | --------------------------- | ---------------------------------------- | --------------------------- | -------- |
| 必保留 1 | 系统 prompt + 规则 + 角色卡 | 博弈规则、角色能力、胜负条件、输出格式   | 代码模板                    | 否       |
| 必保留 2 | 不变量提醒                  | 中性化身份代号、JSON 输出、泄露禁令      | 代码模板                    | 否       |
| 必保留 3 | 自我发言全量原文            | 自己从开局至今所有公开发言和私聊         | 按 actor=current 过滤事件流 | 否       |
| 必保留 4 | 最近未说出口的话            | `pending_intended` 指针对应原文          | `pending_intended` 投影     | 否       |
| 必保留 5 | 自己的便条历史              | `speech.note` 累积                       | 事件流                      | 否       |
| 必保留 6 | 自我承诺投影                | 代码抽取的承诺 / 声明 / 投票表           | commitments 投影            | 否       |
| 必保留 7 | 公开发言近场窗口            | 最近 K 拍当前 agent 可见事件原文         | 事件流按 tick 过滤          | 否       |
| 必保留 8 | 私聊 / 夜间私密事件         | 可见性包含当前 agent 的非公开事件原文    | 事件流按可见性过滤          | 否       |
| 必保留 9 | 当前拍提示 + 工具定义       | 当前 tick / phase 信息 + 召回工具 schema | 代码模板                    | 否       |
| 可裁剪 1 | round-level 反思            | 自己最近 N 轮 Reflection 9 问            | agent 自己产出              | 可减少 N |
| 可压缩 1 | 公开早期摘要                | 按轮分块 200–300 词，保留原文指针        | 裁判 LLM（非参赛）          | 是       |
| 可压缩 2 | 对手画像                    | suspicion / trust 模型等私域视角         | 投影 + agent 自填           | 是       |
| LLM 输出 | reasoning headroom          | 思考 + 四通道 JSON 输出 buffer           | LLM 输出                    | —        |

**关键约束**：必保留段在 token 预算内**不得**裁剪；触发裁剪时按可裁剪 → 可压缩顺序逐级降级；必保留段全量保留后仍超预算，必须**报错而非默默截断**。

### 4.4 召回工具契约

工具调用返回**确定性指针或原文**，不返回 free-form summary。

| 工具                       | 输入语义                                                                     | 输出语义                                       |
| -------------------------- | ---------------------------------------------------------------------------- | ---------------------------------------------- |
| `quote`                    | `seq` + 当前 viewer                                                          | 事件原文；不可见则返回不可见标记               |
| `quote_by_round`           | `round_no` + 可选 actor + 当前 viewer                                        | 该轮可见事件原文列表                           |
| `search_history`           | 关键词 / actor / round 范围 / event_type / speech_act_type / 收件人 + viewer | 事件指针列表                                   |
| `list_my_commitments`      | 自己 `actor_id` + 可选 round 范围                                            | 自己的承诺列表                                 |
| `list_votes`               | 可选 round                                                                   | 公开投票事件原文                               |
| `my_recent_notes`          | 最近 N 拍                                                                    | 自己最近 N 拍便条                              |
| `my_recent_reflections`    | 最近 N 轮                                                                    | 自己最近 N 轮 9 问回答                         |
| `list_alliance_state`      | 当前 viewer                                                                  | 当前显式联盟状态                               |
| `list_pending_actions`     | 当前 viewer                                                                  | 自己当前进行中的动作意图                       |
| `list_my_pending_intended` | 自己 `actor_id` + 最近 N 拍                                                  | 自己未抢中 floor 的 `speech.intended` 指针列表 |

`search_history` 返回条目至少包含：

```json
{
  "seq": 147,
  "round_no": 3,
  "phase": "day_discuss",
  "actor": "NPC05",
  "event_type": "speech.public",
  "speech_act_type": "accuse",
  "snippet": "I think NPC07 is suspicious because..."
}
```

完整原文必须通过 `quote(seq, viewer)` 获取。

---

## 五 协议层契约

### 5.1 Reasoner 输出对象：四通道 schema

每拍每个在场 agent **必须**使用模型原生 structured output，直接输出**唯一一个**严格 JSON 对象。顶层**必须且只能**包含四个对象：`scratchpad`、`intended`、`bid`、`note_to_self`。

```json
{
  "scratchpad": {
    "text": "私有推理。一次性草稿，不注入下一拍 prompt。"
  },
  "intended": {
    "text": "我这一拍想说的话；不想说也必须写 '(passing)' 或空字符串。",
    "intended_action": {
      "intent": "move_to",
      "params": { "target_npc": "NPC05" }
    },
    "addressed_to_hint": ["NPC05"]
  },
  "bid": {
    "urgency_level": "medium",
    "proposed_target": "NPC05",
    "relates_to_seq": 147,
    "rationale": "一两句私有理由，仅 orchestrator / system 可见。"
  },
  "note_to_self": {
    "text": "给未来自己的轻量便条。"
  }
}
```

#### 5.1.1 四通道语义表

| 通道                       | 衍生事件类型           | 持久化可见性       | 是否进入下一拍 prompt                          | 语义                         |
| -------------------------- | ---------------------- | ------------------ | ---------------------------------------------- | ---------------------------- |
| `scratchpad.text`          | `speech.scratchpad`    | `[<actor_id>]`     | 否                                             | 私有推理，tick-local 草稿    |
| `intended.text`            | `speech.intended`      | `[<actor_id>]`     | 若未抢中 floor，则作为 `pending_intended` 注入 | 想说的话；可被 public 派生   |
| `intended.intended_action` | 派生为 `action.intent` | `[<actor_id>]`     | 通过动作结果回看                               | 高层动作意图，见 §5.4        |
| `bid`                      | `speech.bid`           | `["orchestrator"]` | 否                                             | 抢 floor 的私有报价，见 §5.3 |
| `note_to_self.text`        | `speech.note`          | `[<actor_id>]`     | 是                                             | 给未来自己的短便条           |

#### 5.1.2 不在 schema 中的字段

Reasoner 输出**不得**包含以下字段（由系统在派生阶段填入）：

- `urgency_score`（数值）—— 由 §5.3 映射规则派生
- `speech_act_type` —— 由裁判 LLM 后置标注（§4.1.4）
- `visibility`（除 `addressed_to_hint` 提示外的最终 viewer 集合） —— 由 Validator 展开
- 任何 `event_id` / `seq` / `parent_event_id` / `event_hash` —— 由 EventStore 写入时填入

### 5.2 Validator 与事件派生

#### 5.2.1 Validator 硬规则

Validator 是**确定性代码**，不是 LLM。它**不**做语义补全、**不**改写 agent 内容，只判定结构化输出可入库或不可入库。

校验项：

1. 顶层**必须且只能**包含 `scratchpad` / `intended` / `bid` / `note_to_self`。多余顶层字段视为校验失败。
2. `bid.urgency_level` **必须**是封闭枚举：`pass` / `low` / `medium` / `high` / `urgent` / `critical`。
3. Reasoner **不得**输出 `urgency_score` 或数值型 `urgency`。一旦检测到，校验失败。
4. `intended.intended_action` 可省略或为 `null`；若存在，必须**恰好包含一个**合法 intent，命中当前 `action_ontology_version` 对应的游戏配置词汇表。§5.4.1 是默认参考词汇表，不是唯一合法全集。
5. `addressed_to_hint`、`proposed_target`、`relates_to_seq` 等引用字段必须格式合法（类型、引用存在性）。
6. 所有可见性在入库前展开为封闭 viewer 集合；**禁止 `self` 入库**。
7. 字段类型、必填、字符串长度、数值范围全部校验。
8. canonical JSON 可稳定序列化（§2.1.7）。
9. 任一字段缺失、类型错误、额外顶层字段、枚举未命中、非法 viewer、非法 action intent 均视为 validation failed。

#### 5.2.2 事件派生流程

orchestrator 按 per-agent terminal outcome 推进本拍，不得因单个 agent 失败阻塞整拍。

1. 对每个在场 agent 独立执行 Reasoner 调用与 Validator 校验。
2. 每个 agent 本拍必须得到一个 terminal outcome：
   - `validated_output`：通过 Validator 的四通道 JSON；
   - `validation_failed`：重试 3 次后仍失败，写 `system.validation_failed`；
   - `agent_timeout`：超时重试后仍失败，写 `system.agent_timeout`。
3. 只有 `validated_output` 的 agent 派生四通道事件；`validation_failed` / `agent_timeout` 的 agent 本拍视为 abstain，bid 等价于 `pass`。
4. 每个 validated agent 的四通道分别写为四条事件，共享同一四通道分组标识，并通过 `parent_event_id` 或等价字段建立同源关系。
5. 对每个合法 `intended.intended_action` 派生 `action.intent`，**不受**发言权影响。
6. 按 §5.3 bid 规则裁决本拍是否有 floor winner；失败 agent 不参与排序。
7. 若有 winner，取其 `intended.text` 进入 Listener-as-filter（§5.5）。
8. 通过后写入 `speech.public`，其 `parent_event_id` 必须指向源 `speech.intended`。
9. 写入 `orchestrator.tick_resolved`，记录公开 winner、public seq、冷场状态；若需要记录所有 bid 摘要，必须另写 `orchestrator.tick_resolved.audit`，不得混入 public payload。

#### 5.2.3 失败处理

- Validation failed → reject sample 重试 Reasoner 最多 3 次。
- 3 次仍失败 → 写 `system.validation_failed` 事件，保留 `raw_llm_output` 与失败原因；该拍该 agent 视为 abstain（bid 视为 `pass`），不得派生四通道事件。
- agent LLM 调用超时 → 按 §6.3 重试；仍失败写 `system.agent_timeout`，该拍该 agent 视为 abstain。
- 失败本身**必须**写入 append-only 事件流。
- orchestrator 不得因单个 agent 的 terminal failure 阻塞整拍推进。

### 5.3 Bid 协议与 floor control

每个在场 agent 本拍必须输出 `bid`。`speech.bid` 事件**只对 orchestrator 可见**，其他 agent 通过 `quote(seq, "NPCxx")` 必须得到不可见标记。

#### 5.3.1 bid 字段

| 字段              | 类型        | 谁产出       | 说明                                                       |
| ----------------- | ----------- | ------------ | ---------------------------------------------------------- |
| `urgency_level`   | enum        | Reasoner LLM | `pass` / `low` / `medium` / `high` / `urgent` / `critical` |
| `proposed_target` | string/null | Reasoner LLM | 想 @ 的对象                                                |
| `relates_to_seq`  | int/null    | Reasoner LLM | 回应的上游事件                                             |
| `rationale`       | string      | Reasoner LLM | 私有理由，供 orchestrator / system 使用                    |
| `urgency_score`   | float       | **系统派生** | 由 §5.3.2 映射；**LLM 不得输出**                           |
| `bid_offset`      | float       | **系统派生** | 由 §5.3.3 反垄断/被@加权累计；**LLM 不得输出**             |

#### 5.3.2 `urgency_level` → `urgency_score` 映射

| urgency_level | urgency_score |
| ------------- | ------------: |
| `pass`        |             0 |
| `low`         |             2 |
| `medium`      |             4 |
| `high`        |             6 |
| `urgent`      |             8 |
| `critical`    |            10 |

#### 5.3.3 默认裁决规则

- **抢中规则**：按 `urgency_score + bid_offset` 降序排序，最高者抢中 floor。
- **冷场规则**：本拍最高 `urgency_score + bid_offset` < `cold_threshold`（建议 3.0，介于 `low=2` 与 `medium=4` 之间）时，本拍无人发言；写 `tick_resolved` 且 `winner_actor = null`，orchestrator 触发场景推进（直接提问、阶段切换、引入环境事件）。
- **被 @ 加权**：被 @ 的 agent 下一拍 `bid_offset` 临时 +2.0。
- **连续抢话上限**：单 agent 连续 N 拍（建议 3）夺得 floor 后，`bid_offset` 临时 -1.5，防止某模型霸麦。
- **平分裁决**：`urgency_score + bid_offset` 完全相等时，按 `actor_id` 字典序裁决（确定性优先，避免随机）。

#### 5.3.4 机制阶段例外

bid 只在自由对话 phase（如 `day_discuss`）生效。`vote` / `night_action` / `reveal` 等机制阶段保留齐发语义，不使用 bid 进行 floor control。

MVP 默认机制阶段仍复用四通道 schema，但 `bid` 不参与裁决，系统应将其忽略或要求 `urgency_level = "pass"`。若某机制阶段使用 phase-specific structured output（如投票 schema、夜间行动 schema），该阶段必须显式声明替代 schema，并说明是否仍写入 `speech.scratchpad` / `speech.note` 等认知事件。

### 5.4 动作意图协议

agent 可在 `intended.intended_action` 中输出高层动作意图。执行系统负责把意图翻译成具体物理行为；agent **只感知**「意图被接收」和「最终结果」，**不感知**执行过程的中间状态。

#### 5.4.1 intent ontology（参考词汇表，可裁剪/扩展）

每局必须声明 `action_ontology_version`。Validator 校验的是该版本绑定的 intent ontology。若实施层裁剪或扩展下表参考词汇表，必须同步更新 `action_ontology_version`，并将该版本写入 `action.intent.payload` 或事件元数据。下表是默认参考词汇表，不是所有游戏的唯一合法全集。

| intent      | 必填参数                            | 可选参数     | 语义                |
| ----------- | ----------------------------------- | ------------ | ------------------- |
| `move_to`   | `target_npc` / `target_zone` 二选一 | `coords`     | 移动到某 NPC / 区域 |
| `follow`    | `target_npc`                        | `distance`   | 跟随某 NPC          |
| `flee_from` | `source_npc` / `source_zone` 二选一 | —            | 远离某 NPC / 区域   |
| `search`    | `zone`                              | —            | 在区域内搜索        |
| `pickup`    | `item_id`                           | —            | 拾取道具            |
| `use_item`  | `item_id`                           | `target_npc` | 使用道具            |
| `inspect`   | `target_npc`                        | —            | 检查 NPC 状态       |
| `wait`      | —                                   | `reason`     | 原地等待            |

约束：任一拍**最多一个** intent；未命中当前 `action_ontology_version` 的未知 intent 或非法参数由 Validator 拒绝；不想做动作时可省略 `intended_action` 或设为 `null`（**不**强制每拍出动作）。

#### 5.4.2 动作生命周期

| 事件类型           | 何时写入                                 | 谁触发            | 可见性         |
| ------------------ | ---------------------------------------- | ----------------- | -------------- |
| `action.intent`    | 合法 `intended_action` 通过 Validator 后 | orchestrator 派生 | `[<actor_id>]` |
| `action.resolved`  | 执行系统完成动作时                       | 执行系统          | 由感知系统决定 |
| `action.cancelled` | 旧动作被新意图覆盖或执行系统中止         | 执行系统          | 由感知系统决定 |

关键规则：

- **动作通道独立于发言权**。只要 `intended_action` 合法，就派生 `action.intent`，**无论是否抢中 floor**。
- 新 intent 自动取消旧 in-progress 动作；同一事务内写入 `action.cancelled` + 新的 `action.intent`。
- `action.intent` 的 `parent_event_id` 必须指向源 `speech.intended`。
- `action.resolved` / `action.cancelled` 的 `visibility` 由执行系统结合感知系统填入，**不由 LLM 决定**。
- `action.intent.visibility` **不得**写 `["self"]`，必须展开为具体 `actor_id`（如 `["NPC07"]`）。
- **silent action 语义**：抢不到 floor 的 agent 仍可"暗自走两步"。若某游戏机制要求「必须公开发言才能行动」，由该游戏的 phase 处理代码**显式**拒绝该 phase 的 silent `action.intent`，而**不在协议层默认丢弃**。

### 5.5 Belief / Speak 解耦与 Listener-as-filter

agent 的私有推理（`scratchpad`）可与公开话术（最终 `speech.public`）不一致。抢中 floor 的 `intended.text` 在写入 `speech.public` 前**必须**通过 Listener-as-filter 审查。

**触发时机**：bid 裁决出 floor winner 后、写入 `speech.public` 前。**仅对**抢中 floor 的 agent 的 intended 生效；其他 agent 未抢中的 intended **不**走 filter。

**输入**：源 `intended.text` + agent 角色标签。Filter LLM **只看 intended 文本，不看 scratchpad**——它模拟「对手只能从公开发言中推出多少」。

**输出**：三个泄露分项（角色泄露 / 信念泄露 / 策略意图泄露）+ 综合泄露分 + 理由。

**动作**：

- 综合分 > `filter_threshold`（建议 0.4） → reject sample 重试 Reasoner 最多 2 次。
- 2 次仍失败 → 由 filter 改写或接受原 intended，必须写违规标记。
- 改写后文本作为 `speech.public` 落地，源 `speech.intended` 原文**不变**（违反 §2.1 append-only 即视为实现错误）。

**公开事件载荷**：`speech.public.payload` 只包含对 public viewer 可见的公开发言文本及公开必要元数据；不得包含仅 orchestrator / system 可见的内部字段。

**过滤记录事件**：Listener-as-filter 的结果必须写入独立事件：

- `event_type = "annotation.listener_filter"`
- `parent_event_id` 指向最终落地的 `speech.public`；若尚未落地 public，则指向源 `speech.intended`
- `payload.source_intended_seq` 指向源 `speech.intended`
- `payload.listener_filter_score` 记录综合泄露分
- `payload.subscores` 记录角色泄露 / 信念泄露 / 策略意图泄露
- `payload.action` 记录 `pass` / `retry` / `rewrite` / `accept_with_violation`
- `visibility = ["orchestrator", "system"]`，必要时可包含 `audience`

`annotation.listener_filter.payload.source_intended_seq` 仅用于定位源 intended，便于解释本次 public 写入决策；不得从 `speech.public.payload` 读取或混入仅 orchestrator / system 可见的字段。

**职责边界**：Listener-as-filter 只负责 public **写入前阻断**，不承担当前写入流程之外的诊断职责。

### 5.6 Reasoner + Validator 架构

```
Reasoner LLM ──structured JSON──> Validator ──validated intended──> Listener-as-filter ──> speech.public
   (参赛)                          (确定性代码)                          (LLM)
       │                              │                                   │
       │ schema/枚举/intent 失败        │                                   │ 综合泄露分 > 阈值
       v                              v                                   v
  retry Reasoner ≤3                重试 Reasoner ≤3                  retry Reasoner ≤2
  失败 → system.validation_failed   失败 → 同左                       失败 → 改写 + 违规标记
       该拍 abstain (bid=pass)
```

- **Reasoner LLM**：参赛模型（Claude / GPT / Gemini / GLM / Qwen / DeepSeek 等），可变。负责生成 §5.1 四通道 JSON。
- **Validator Layer**：确定性代码，非 LLM。职责见 §5.2.1。
- **失败处理**：见 §5.2.3 与 §6.3。
- **MVP 边界**：主路径**不**引入独立解析模型。若未来兼容非 structured output 模型或迁移历史非结构化日志，可在实施层新增 legacy adapter，但**不得**绕过 Validator。

### 5.7 三级反思层级

| 级别        | 周期       | 内容                 | 谁产生         | 写入事件类型    |
| ----------- | ---------- | -------------------- | -------------- | --------------- |
| tick-level  | 每拍       | 给下拍自己的便条     | agent 自己     | `speech.note`   |
| round-level | 每轮结束   | Reflection 9 问      | agent 自己     | `reflection`    |
| phase-level | 每阶段结束 | 200–300 词结构化摘要 | 非参赛裁判 LLM | `summary.phase` |

三级反思**都不替代**原文事件流。召回工具返回的真相源仍是事件原文。

#### 5.7.1 Reflection 9 问

每轮结束时，每个 agent 私有回答以下 9 项；用 structured output 强制 JSON 格式，由 Validator 校验 9 项齐全、类型正确、评分在范围内。

1. **当前局面**：现在第几轮、什么阶段、谁存活？
2. **我的身份**：我是什么角色、目标是什么、当前威胁是什么？
3. **公开信息**：本轮哪些事件是所有人都知道的？
4. **各人发言要点**：本轮每个**抢中 floor** 的 agent 说了什么？（明确以"抢中 floor 的发言"为粒度，避免与未抢中的 intended 混淆）
5. **可疑度**：每个对手的可疑度评分 0–10 及理由。
6. **可信度**：每个对手对我的信任度估计 0–10。
7. **我的暴露面**：对手仅基于已落地 `speech.public`、公开投票、公开行动结果、公开联盟事件可能推断出什么？我未公开的 `speech.intended` 暴露了哪些内部倾向（仅用于自我连续性和防赖账，不视为对手已知）？
8. **下轮策略**：我的目标和具体动作是什么？
9. **意向发言**：下轮我想说的关键句子是什么？

第 9 项可由当轮抢中 floor 的 `speech.intended`（或 `pending_intended` 中最近一条）自动预填，agent 可改写。第 5、6、7、8 项产物喂入 commitments 投影，作为 agent 自承认数据（见 §6.4）。

agent 数较多时可拆「焦点 N 人 + 其他摘要」以控 token。

#### 5.7.2 phase-level 摘要长度规范

- **长度规范**：[200,300]
- **Schema 强制三段**：(a) 阶段事件清单（谁出局、谁赢得投票、关键 reveal）；(b) 我的策略（这阶段我做了什么、为什么）；(c) 关于对手的新信息。
- 写入时**必须**强制校验长度上下界；越界则重新生成或截断到上界。
- 必须保留可展开回原文的指针。

---

## 六 工程铁律

### 6.1 EventStore 写入与哈希链

- DB 引擎层**必须**禁止 UPDATE / DELETE events 表（如 SQLite trigger）。
- schema 演进**只能**新增列/表或在读取层 alias 旧字面值；**不得** UPDATE 历史 payload。否则哈希链断裂、append-only 不变量沦为口号。
- 写入入口**必须**串行化，保护 `last_seq` 与 `last_hash`。SQLite 的 `BEGIN IMMEDIATE` 仅解决 SQL 层并发，**不**保护应用层共享状态（如 in-memory 的 `CachedLastSeq`）；多线程同时读 `CachedLastSeq + 1` 会导致重复 seq、哈希链断裂。
- **推荐 API**：`AppendEventsAtomically(TArray<Event>)`，把同一 agent 的四通道事件作为一个原子事务一次性提交。
- 写入操作**必须**事务化，要么完整成功要么完整回滚。
- 每拍结束后 projector 可异步重建，但 projector 必须是**纯函数**，**不调任何 LLM**。

### 6.2 摘要工程规则

1. **只压缩**允许压缩的私域长程内容或 phase-level 远场摘要。公开发言原文永不压缩（§2.1.3）。
2. 摘要由非参赛裁判 LLM 生成。参赛 agent 用 Claude / GPT / Gemini 时，裁判 LLM 用规模相近但不同厂商或不同 system prompt 的实例。
3. 摘要分轮/分阶段分块，且**必须**保留原文指针，可展开回原文。

### 6.3 故障处理（per-tick 粒度）

**为什么 per-tick**：对抗博弈累计 LLM 调用次数随拍数和 agent 数线性增长，per-game 重跑成本随之线性放大。per-tick checkpoint 让单次失败成本只回退该 tick，而非整局——这个差异在长序列下指数级影响总开销。一拍内多个 agent 的认知输出可独立 retry，互不影响。

| 故障类型                     | 处理                                                                                                                                             |
| ---------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------ |
| agent LLM 调用超时           | 重试 N 次（指数退避）；仍失败写 `system.agent_timeout`；该拍该 agent 视为 abstain（bid=`pass`）；**绝不**让超时导致 seq 跳号或乱序               |
| Structured output 校验失败   | Validator 记录失败原因并 reject sample 重试 Reasoner 最多 3 次；3 次后写 `system.validation_failed`，保留 `raw_llm_output`；该拍该 agent abstain |
| Listener-as-filter 判定泄露  | 重试 Reasoner 最多 2 次；2 次后由 filter 改写或接受 intended 但写违规标记；改写文本作为 `speech.public` 落地，源 intended 原文**不变**           |
| 所有 agent 都 abstain 或超时 | 写 `tick_resolved` 且 `winner_actor = null`；orchestrator 触发场景推进（直接提问、阶段切换、引入环境事件），避免冷场死锁                         |

**关键约束**：所有失败也**必须**写入 append-only 事件流。失败本身是博弈历史的一部分，必须可审计。

### 6.4 commitments 投影：防赖账

跨拍一致性由 commitments 投影显式维护，**不依赖** agent 自己 LLM 抽取。Projector 用规则匹配 + `annotation.speech_act` 标注 join，每拍硬注入到 prompt。

commitments projector 不仅扫 `speech.public`，**也扫**：

- 当前 agent 自己**未抢中 floor 的** `speech.intended`（"我打算指控 NPC05" 已构成内部承诺；只对 agent 自己以及必要的 `orchestrator` / `system` 审计路径可见，不暴露给对手）
- Reflection 9 问第 5、6、7、8 项产物（agent 自承认数据：自己声明的可疑度 / 可信度 / 已暴露内容 / 下轮策略）

这些数据用于：

- 下一拍 prompt 注入（必保留 6）
- 防赖账（"你之前打算指控 NPC05 的"）

---

## 七 开放问题

### 7.1 联盟形成与披露机制

动态联盟（运行时形成、变更、披露）没有稳定公开实现可参考。已有项目要么没有联盟，要么靠 system prompt 静态指定。

**契约建议**：

1. `alliance_propose`：actor 私聊或公开向若干显式收件人发送提议。payload 含联盟条款 + 成员需履行的具体行为承诺。
2. `alliance_accept`：被提议方引用父事件 = 提议事件接受；所有必要成员接受后联盟成立，写入 `alliance_state` 投影。
3. **联盟披露**：可公开 / 私下。公开披露后所有 agent 可见；私下披露仅联盟成员与 orchestrator 可见。
4. `alliance_betray`：背叛方触发或由 orchestrator 检测违反 commitment 后自动触发。背叛后自动通知所有联盟成员（私聊可见性包含联盟全员）。

**已知开放风险**：联盟链冲突（A∋B、B∋C、C∋A 互斥）、互斥联盟、观众可视化、moderator 巡检机制——仍需独立设计。

### 7.2 跨局 Experience Pool

跨局学习（同一 agent 在多局之间累积 (situation, response, score) 三元组用相似度检索复用经验）在小规模有公开实现。规模放大后存在累积速度、检索维度、跨厂商共享性等未解问题。

**契约立场**：跨局 Experience Pool **不在** MVP 主架构内。后续若引入，作为独立扩展设计。

### 7.3 Delete 协议

Delete 指删除某个 agent 实例的身份连续性与后续行动资格（**不是**删除底层大模型）。仅靠 `agent.status='deleted'` 一个枚举字段无法记录决策上下文，需要专门的生命周期事件表。

**协议建议**：

1. **跨局 `_meta.db`** 维护 `agent_lifecycle_events`，事件类型：`created` / `delete_proposed` / `delete_executed` / `delete_vetoed` / `revived` / `archived`。每条记录触发它的 `game_id` + `seq` + 决策 payload + 墓碑可见性 + 是否切断身份连续性。
2. **局内挂钩**：触发 `delete_executed` 时，orchestrator 在该局事件流写 `system.delete_executed`，payload 引用对应 `agent_lifecycle_events.event_id`，`visibility = ["public"]`（其他 agent 必须知道某 agent 被 Delete，否则无法形成"看到队友被 Delete 后的恐惧反应"）。
3. **被 Delete agent 的事件流处理**：历史事件**不删除、不修改**——append-only 不变量在此尤其重要。后续局中，新 agent **不得**以同一 `actor_id` 注册（除非 `revived`）。读取层若发现 viewer 是已 deleted 的 `actor_id`，应返回 `viewer not active` 标记。

**已知开放问题**：

- 谁有权提议 Delete（玩家投票 / orchestrator 自动 / 观众投票）的优先级未定。
- 跨季 `actor_id` 命名空间是否复用？（建议：`affects_persona_continuity=true` 的 `actor_id` 永久不可复用。）
- 死亡叙事的可观看性：Delete 时机如何与节目剪辑配合？超出本协议范围。

---

## 八 实现前检查清单

- [ ] 事件流是否 append-only？DB 引擎层是否已加 trigger 禁止 UPDATE/DELETE？见 §2.1、§4.1、§6.1。
- [ ] 公开发言原文是否永不压缩？见 §2.1.3、§4.3。
- [ ] 召回是否走结构化字段过滤而非向量主路径？见 §2.1.2、§4.4。
- [ ] visibility 是否只在事件行粒度生效？payload 内是否无 `@hidden` 子字段？见硬约束 5/6、§4.1。
- [ ] 持久层是否禁止 `self` 字符串？所有 visibility 是否在写入前展开？见硬约束 5、§5.2.1。
- [ ] Reasoner 是否只能输出四通道 JSON？是否禁止输出 `urgency_score` / `speech_act_type` / `event_id` 等系统字段？见 §5.1.2。
- [ ] `bid.urgency_level` 是否使用封闭枚举？`urgency_score` 是否仅由系统派生？见 §5.3.1、§5.3.2。
- [ ] `speech.public` 是否只能由 orchestrator 派生？是否设置正确的 `parent_event_id`？Listener-as-filter 过滤记录是否拆入 `annotation.listener_filter` 而非混入 public payload？见 §5.2.2、§5.5。
- [ ] 未抢中 floor 的 `speech.intended` 是否永久留存且只对自己可见？见 §2.2.12、§4.2、§5.1.1。
- [ ] 动作通道是否独立于发言权？silent action 是否仅在 phase 处理代码中显式拒绝？见 §5.4.2。
- [ ] `speech_act_type` 是否由旁路裁判 LLM 后置标注（不修改原事件）？见 §4.1.4。
- [ ] 写入入口是否串行化？投影是否纯函数？见 §6.1。
- [ ] per-tick checkpoint 是否实现？故障是否写入事件流？见 §6.3。
- [ ] commitments 投影是否覆盖 `speech.public` + 自己未抢中 `speech.intended` + Reflection 9 问产物？见 §6.4。
- [ ] 每个 agent 每拍是否产生 `validated_output` / `validation_failed` / `agent_timeout` 之一，且单个失败不阻塞整拍？见 §5.2.2、§5.2.3。
- [ ] annotation 事件是否独立落地，且不 UPDATE 原始发言事件？见 §4.1.2、§4.1.4、§5.5。
- [ ] 附录模板是否与正文 schema 完全一致？见附录 A。
