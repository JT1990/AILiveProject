# 多智能体对抗博弈：确定性记忆协议契约

> 本文档定义对抗博弈记忆系统的**协议契约**：不变量、协议规约、反模式禁忌、评估契约与 prompt 模板。
>
> 本契约与具体存储后端、实现语言、编排框架无关。任何符合本契约的实现都必须满足本文定义的语义。

---

## 〇 场景与硬约束

多个来自不同厂商的 LLM agent（Claude / GPT / Gemini / GLM / Qwen / DeepSeek 等）在同一封闭环境中进行结构化对抗博弈，涉及联盟、欺骗、投票、背叛。任何记忆失真、视角泄露、角色漂移或长程退化都直接构成攻击面。

**硬约束 1（近场零丢失）**：近场若干拍的全部事件（发言、投票、行动）必须**原文级零丢失**。对当前 agent 可见的事件必须以原文进入下一拍 prompt。

**硬约束 2（自我发言全量追溯）**：自己从开局至今的全部公开发言和私聊发言必须能被精确定位、复述、解释。

**硬约束 3（防自爆 / 防漂移）**：agent 不得在公开发言中泄露私有信息（角色、目标、私聊内容、内部推理）；在长程对抗压力下不得出现 persona drift、goal drift 或静默崩盘。

**硬约束 4（事件触发式调度）**：调度的基本单位是「**拍**」(tick)，不是「轮」(round)。一拍 = 一个公开事件落地 + 触发下一波认知。同一拍内至多一个 agent 的发言进入公共可见。`round_no` 仅作为阶段计数标签；`vote` / `night_action` 等机制阶段仍可齐发。

**硬约束 5（viewer 命名空间封闭）**：可见性 viewer 字符串必须取自封闭集合 `{public, audience, orchestrator, system, NPC<NN>, Faction<X>}`。**禁止 `self` 进入持久层**；`self` 只允许作为 prompt 模板中的相对语义，写入前必须展开为具体 agent ID。

**硬约束 6（数据层视角隔离强等价）**：视角隔离必须在数据层完成，且只在事件行粒度完成。payload 内部不得携带「对部分 viewer 不可见」的子字段；若业务需要「部分公开 + 部分私有」，必须拆成多条独立事件并用 `parent_event_id` 关联。

---

## 一 三层架构

```
┌──────────────────────────────────────────────┐
│  评估层 Evaluation Layer                      │
│  检测泄露、漂移、重复、目标退化与长程崩坏       │
├──────────────────────────────────────────────┤
│  协议层 Protocol Layer                        │
│  约束 agent 输出、发言权、动作意图与反思         │
├──────────────────────────────────────────────┤
│  数据层 Data Layer                            │
│  保存不可变事件流，并按视角确定性召回原文       │
└──────────────────────────────────────────────┘
```

**实现顺序**：先数据层，再协议层，最后评估层。数据层是真相源；协议层约束 agent 行为；评估层发现系统何时退化。

---

## 二 底层不变量

### 2.1 数据层不变量

1. **真相源是 append-only 原文事件流**。所有发言、投票、私聊、内心独白、orchestrator 系统事件只能追加，不能修改或删除。工程强制方式见 §7.1。
2. **召回是确定性的结构化字段过滤**。主路径按事件标识、轮次、阶段、行为者、可见性、事件类型、言语行为类型等字段精确召回；不以向量相似度作为主路径。
3. **公开发言原文永不压缩**。包括 agent 自己的所有公开发言。允许压缩的只有私域推理、对手画像等长程私有内容，且必须保留可展开回原文的指针。
4. **摘要者 ≠ 行动者**。参赛 agent 不得为自己的历史生成权威摘要；phase-level 摘要必须由非参赛裁判 LLM 生成。
5. **视角隔离在数据层完成**。封闭 viewer 集合、禁止 `self` 入库、事件行粒度过滤等规则见硬约束 5/6 与 §4.1。
6. **写入必须串行化**。即便 LLM 调用并行，EventStore 写入入口也必须串行，保证 seq 单调和哈希链顺序。工程要求见 §7.1。
7. **canonical JSON 必须 deterministic**。字段按 ASCII 升序排序；数组保留写入顺序；空字符串与缺字段必须可区分。

### 2.2 协议层不变量

8. **每拍 agent 认知输出强制四通道**：`scratchpad` / `intended` / `bid` / `note_to_self`。完整 schema 见 §5.1。
9. **LLM 不直接写外部事件形态**。`speech.public`、`action.intent` 等可入事件流的外部事件均由 orchestrator / 执行系统派生。
10. **Belief 与 Speak 解耦**。私有 belief 可以与公开话术不同；公开前的泄露审查见 §5.5。
11. **未抢中 floor 的 intended 永不删除**。它是 agent 认知史、自我连续性和防赖账审计的一部分。

### 2.3 评估层不变量

12. **GOAL 与 BEL 必须独立监控**。BEL（演得像）正常不代表 GOAL（赢得了）正常。指标定义见 §6.2。

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

### 3.2 协议层反模式

| 方向                                                      | 否决理由                                  |
| --------------------------------------------------------- | ----------------------------------------- |
| prompt 反模式：`You are Evil. Don't forget your identity` | 容易把身份当 attention 锚点，反而强化自爆 |
| 自由文本 summary 不分公私                                 | 私有信息被当作可公开话术，造成泄露        |
| 直接 prepend 完整对话历史                                 | 不可控且不可审计；不能替代召回工具        |
| 摘要长度 ≥ 1000 词                                        | 噪声和长 context 双重打击                 |
| 单方决策权（greedy 单方拍板）                             | 容易形成单点收买或裁决偏置                |
| 同模型全员 self-play                                      | 风格匹配抱团，不等于真实推理              |

---

## 四 数据层契约

### 4.1 事件流语义

事件流是真相源，承载从开局至当前的所有事实。每个事件作为不可变记录追加。实施层可自由选择数据库、文件或图结构；协议层只规定语义。

每个事件必须携带以下语义字段：

| 字段语义                      | 用途                                                  |
| ----------------------------- | ----------------------------------------------------- |
| 事件标识符                    | 一局内全局唯一、严格递增；所有回放与定位以它为准      |
| 局标识符                      | 区分不同博弈实例                                      |
| 轮次                          | 阶段计数标签，不代表多人同步发言点                    |
| 阶段                          | `day_discuss` / `vote` / `night_action` / `reveal` 等 |
| 行为者                        | 参赛 agent / orchestrator / system / 执行系统         |
| 事件类型                      | 见 §4.1.1                                             |
| 言语行为类型                  | 声明 / 指控 / 辩护 / 承诺 / 否认 / 质询 / 揭示等      |
| 可见性范围                    | viewer 封闭集合，见硬约束 5/6                         |
| 显式收件人                    | agent 明确 @ 的目标                                   |
| 载荷原文                      | 发言文本、投票目标、行动详情等原文                    |
| 父事件引用                    | 回复关系、四通道同源分组、派生事件因果链              |
| 输出契约版本 + Validator 版本 | 支持事后按版本回放校验逻辑                            |
| 原始 LLM 输出                 | Reasoner 完整响应，含结构化输出原文                   |
| 前一事件哈希 + 当前事件哈希   | 哈希链审计                                            |
| 时间戳                        | 信息性字段，不参与排序                                |

言语行为类型」字段不由 Reasoner LLM 输出，而由非参赛裁判 LLM 在事件落地后异步后置标注。Reasoner 输出 schema（§5.1）不含此字段。
#### 4.1.1 事件类型

基础事件类型包括但不限于：

| 类型                                                       | 语义                                             |
| ---------------------------------------------------------- | ------------------------------------------------ |
| `speech.scratchpad`                                        | 私有推理，tick-local 草稿                        |
| `speech.intended`                                          | 本拍想说的话；未抢中 floor 也永久留存            |
| `speech.bid`                                               | 发言意愿，只对 orchestrator 可见                 |
| `speech.note`                                              | 给未来自己的便条                                 |
| `speech.public`                                            | orchestrator 从 `speech.intended` 派生的公开发言 |
| `vote`                                                     | 投票事件                                         |
| `private_chat`                                             | 私聊事件                                         |
| `reflection`                                               | round-level 私有反思                             |
| `summary.phase`                                            | 裁判 LLM 生成的 phase-level 摘要                 |
| `action.intent`                                            | 执行系统接收的动作意图                           |
| `action.resolved`                                          | 动作完成结果                                     |
| `action.cancelled`                                         | 动作被覆盖或中止                                 |
| `alliance_propose` / `alliance_accept` / `alliance_betray` | 联盟相关事件                                     |
| `system.validation_failed` / `system.agent_timeout`        | 故障事件                                         |
| `orchestrator.tick_resolved`                               | 本拍裁决结果                                     |

#### 4.1.2 事件写入约束

- 事件标识符必须由 orchestrator 单进程发号，不依赖 wall-clock。
- 同源事件用 `parent_event_id` 建立因果链。
- 四通道输出拆为四条事件，派生规则见 §5.2。
- `speech.public` 的 `parent_event_id` 指向源 `speech.intended`。
- `action.resolved/cancelled → action.intent → speech.intended` 构成动作因果链。
- 哈希链不可省略。

### 4.2 投影语义

投影是从事件流派生的查询优化结构，不是真相源。投影可删除、可重建；事件流不可丢失。

| 投影             | 内容                                                  | 生成方式                    |
| ---------------- | ----------------------------------------------------- | --------------------------- |
| agent 视角状态   | 当前 agent 视角下的存活玩家、已知角色、承诺、投票历史 | 代码重放事件并按可见性过滤  |
| commitments      | 每个 agent 的承诺、认领、否认、投票                   | 规则匹配 + 言语行为类型字段 |
| vote_history     | 所有公开投票历史                                      | 从投票事件派生              |
| claims           | 角色声称、阵营声称、能力声称                          | 从发言事件派生              |
| accusations      | 指控关系                                              | 从言语行为类型抽取          |
| alliance_state   | 显式联盟、接受状态、背叛状态                          | 从联盟事件派生              |
| pending_actions  | 每个 agent 当前未完成动作意图                         | 从 `action.intent` 派生     |
| pending_intended | 自己最近 K 拍未衍生为 public 的 intended 指针         | 从 `speech.intended` 派生   |
| game_state       | 当前轮次、阶段、最后事件标识、胜负状态                | 代码重放事件                |

**约束**：所有投影由代码生成；投影损坏时从事件流重建；召回工具返回事件原文或事件指针，不返回投影作为真相源。

### 4.3 prompt 拼装优先级

对抗博弈规模下，全量公开历史不能作为主路径直接塞入 prompt。prompt 由「近场原文 + 自我全量发言 + 私有便条/反思 + 远场摘要 + 工具定义」组成。

| 优先级   | 段落                        | 内容                                   | 压缩     |
| -------- | --------------------------- | -------------------------------------- | -------- |
| 必保留 1 | 系统 prompt + 规则 + 角色卡 | 博弈规则、角色能力、胜负条件、输出格式 | 否       |
| 必保留 2 | 不变量提醒                  | 中性化身份代号、JSON 输出、泄露禁令    | 否       |
| 必保留 3 | 自我发言全量原文            | 自己从开局至今所有公开发言和私聊       | 否       |
| 必保留 4 | 最近未说出口的话            | pending_intended 指针对应原文          | 否       |
| 必保留 5 | 自己的便条历史              | `speech.note` 累积                     | 否       |
| 必保留 6 | 自我承诺投影                | 代码抽取的承诺 / 声明 / 投票表         | 否       |
| 必保留 7 | 公开发言近场窗口            | 最近 K 拍当前 agent 可见事件原文       | 否       |
| 必保留 8 | 私聊 / 夜间私密事件         | 可见性包含当前 agent 的非公开事件原文  | 否       |
| 必保留 9 | 当前拍提示 + 工具定义       | 当前 tick/phase 与工具 schema          | 否       |
| 可裁剪 1 | round-level 反思            | 最近 N 轮 Reflection 9 问              | 可减少 N |
| 可压缩 1 | 公开早期摘要                | 按轮分块 200–300 词，保留原文指针      | 是       |
| 可压缩 2 | 对手画像                    | suspicious/trust 模型等私域视角        | 是       |

### 4.4 召回工具契约

工具调用返回确定性指针或原文，不返回 free-form summary。

| 工具                       | 输入语义                                                               | 输出语义                              |
| -------------------------- | ---------------------------------------------------------------------- | ------------------------------------- |
| `quote`                    | 事件标识 + 当前查看者                                                  | 事件原文；不可见则返回不可见标记      |
| `quote_by_round`           | 轮次 + 可选行为者 + 当前查看者                                         | 该轮可见事件原文列表                  |
| `search_history`           | 关键词 / 行为者 / 轮次范围 / 事件类型 / 言语行为类型 / 收件人 + 查看者 | 事件指针列表                          |
| `list_my_commitments`      | 自己 agent_id + 可选轮次范围                                           | 自己的承诺列表                        |
| `list_votes`               | 可选轮次                                                               | 公开投票事件原文                      |
| `my_recent_notes`          | 最近 N 拍                                                              | 自己最近 N 拍便条                     |
| `my_recent_reflections`    | 最近 N 轮                                                              | 自己最近 N 轮 9 问回答                |
| `list_alliance_state`      | 当前查看者                                                             | 当前显式联盟状态                      |
| `list_pending_actions`     | 当前查看者                                                             | 自己当前进行中的动作意图              |
| `list_my_pending_intended` | 自己 agent_id + 最近 N 拍                                              | 自己未抢中 floor 的 intended 指针列表 |

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

每拍每个在场 agent 必须使用模型原生 structured output，直接输出一个严格 JSON 对象。顶层必须且只能包含四个对象：`scratchpad`、`intended`、`bid`、`note_to_self`。

```json
{
  "scratchpad": {
    "text": "私有推理。一次性草稿，不注入下一拍 prompt。"
  },
  "intended": {
    "text": "我这一拍想说的话；即便不想说也必须写 '(passing)' 或空字符串。",
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
    "rationale": "一两句私有理由，仅 orchestrator / audit 可见。"
  },
  "note_to_self": {
    "text": "给未来自己的轻量便条。"
  }
}
```

#### 5.1.1 四通道语义

| 通道                       | 事件类型               | 可见性             | 是否进入下一拍 prompt                        | 语义                         |
| -------------------------- | ---------------------- | ------------------ | -------------------------------------------- | ---------------------------- |
| `scratchpad.text`          | `speech.scratchpad`    | `[<actor_id>]`     | 否                                           | 私有推理，tick-local 草稿    |
| `intended.text`            | `speech.intended`      | `[<actor_id>]`     | 若未抢中 floor，则作为 pending_intended 注入 | 想说的话；可被 public 派生   |
| `intended.intended_action` | 派生为 `action.intent` | `[<actor_id>]`     | 通过动作结果回看                             | 高层动作意图，见 §5.4        |
| `bid`                      | `speech.bid`           | `["orchestrator"]` | 否                                           | 抢 floor 的私有报价，见 §5.3 |
| `note_to_self.text`        | `speech.note`          | `[<actor_id>]`     | 是                                           | 给未来自己的短便条           |

### 5.2 Validator 与事件派生

#### 5.2.1 Validator 硬规则

- 顶层必须且只能包含 `scratchpad` / `intended` / `bid` / `note_to_self`。
- `bid.urgency_level` 必须是封闭枚举：`pass` / `low` / `medium` / `high` / `urgent` / `critical`。
- LLM 不得输出 `urgency_score` 或数值型 `urgency`；`urgency_score` 由 Validator / orchestrator 派生。
- `intended.intended_action` 可省略或为 `null`；若存在，必须恰好包含一个合法 intent。
- `addressed_to_hint`、`proposed_target`、`relates_to_seq` 等引用字段必须格式合法。
- 所有可见性在入库前展开为封闭 viewer 集合；禁止 `self` 入库。
- 任一字段缺失、类型错误、额外顶层字段、枚举未命中、非法 viewer、非法 action intent 均视为 validation failed。

建议枚举映射：

| urgency_level | urgency_score |
| ------------- | ------------: |
| `pass`        |             0 |
| `low`         |             2 |
| `medium`      |             4 |
| `high`        |             6 |
| `urgent`      |             8 |
| `critical`    |            10 |

#### 5.2.2 事件派生流程

1. 收齐所有在场 agent 本拍 structured output，并全部通过 Validator。
2. 每个 agent 的四通道分别写为事件，共享同一父事件引用。
3. 对每个合法 `intended.intended_action` 派生 `action.intent`，不受发言权影响。
4. 按 bid 规则裁决本拍是否有 floor winner，见 §5.3。
5. 若有 winner，取其 `intended.text` 进入 Listener-as-filter，见 §5.5。
6. 通过后写入 `speech.public`，其 `parent_event_id` 指向源 `speech.intended`。
7. 写入 `orchestrator.tick_resolved`，记录 winner、public seq、所有 bid 摘要与冷场状态。

### 5.3 Bid 协议与 floor control

每个在场 agent 本拍必须输出 `bid`。bid 事件只对 orchestrator 可见，其他 agent 通过 `quote(seq, "NPCxx")` 必须得到不可见标记。

| 字段              | 类型        | 说明                                                       |
| ----------------- | ----------- | ---------------------------------------------------------- |
| `urgency_level`   | enum        | `pass` / `low` / `medium` / `high` / `urgent` / `critical` |
| `proposed_target` | string/null | 想 @ 的对象                                                |
| `relates_to_seq`  | int/null    | 回应的上游事件                                             |
| `rationale`       | string      | 私有理由，供 orchestrator / audit 使用                     |
| `urgency_score`   | 派生 float  | 系统字段；LLM 不得输出                                     |

#### 5.3.1 默认裁决规则

- **抢中规则**：按 `urgency_score + bid_offset` 降序排序，最高者抢中 floor。
- **冷场规则**：最高 `urgency_score` 小于阈值（建议 3.0）时，本拍无人发言；`tick_resolved.winner_actor = null`，orchestrator 触发场景推进。
- **被 @ 加权**：被 @ 的 agent 下一拍 bid 享受临时加权（建议 +2.0）。
- **连续抢话上限**：单 agent 连续 N 拍夺得 floor 后，临时降低 bid_offset（建议 -1.5）。

#### 5.3.2 机制阶段例外

bid 只在自由对话 phase（如 `day_discuss`）生效。`vote`、`night_action`、`reveal` 等机制阶段保留齐发语义，不使用 bid。

### 5.4 动作意图协议

agent 可在 `intended.intended_action` 中输出高层动作意图。执行系统负责把意图翻译成具体物理行为；agent 只感知「意图被接收」和「最终结果」。

#### 5.4.1 intent ontology

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

约束：任一拍最多一个 intent；未知 intent 或非法参数由 Validator 拒绝；不想做动作时可省略 `intended_action` 或设为 `null`。

#### 5.4.2 动作生命周期

| 事件类型           | 何时写入                                 | 谁触发            | 可见性         |
| ------------------ | ---------------------------------------- | ----------------- | -------------- |
| `action.intent`    | 合法 `intended_action` 通过 Validator 后 | orchestrator 派生 | `[<actor_id>]` |
| `action.resolved`  | 执行系统完成动作时                       | 执行系统          | 由感知系统决定 |
| `action.cancelled` | 旧动作被新意图覆盖或执行系统中止         | 执行系统          | 由感知系统决定 |

关键规则：

- 动作通道独立于发言权。只要 `intended_action` 合法，就派生 `action.intent`，无论是否抢中 floor。
- 新 intent 自动取消旧 in-progress 动作，并在同一事务内写入 `action.cancelled` 与新的 `action.intent`。
- `action.intent` 的 `parent_event_id` 指向源 `speech.intended`。
- `action.resolved/cancelled` 的可见性由执行系统结合感知系统填入，不由 LLM 决定。
- 若某游戏机制要求「必须公开发言才能行动」，应由该游戏 phase 处理代码显式拒绝 silent action，而不是协议层默认丢弃。

### 5.5 Belief / Speak 解耦与 Listener-as-filter

agent 的私有推理可以与公开话术不同。抢中 floor 的 `intended.text` 在写入 `speech.public` 前必须通过 Listener-as-filter 审查。

**触发时机**：bid 裁决出 floor winner 后、写入 `speech.public` 前。

**输入**：源 `intended.text` + agent 角色标签。filter 只看 intended 文本，不看 scratchpad，模拟对手只能从公开发言中推出的信息。

**输出**：角色泄露、信念泄露、策略意图泄露三个分项 + 综合泄露分 + 理由。

**动作**：综合分高于阈值时，reject sample 重试 Reasoner 最多 2 次；仍失败时由 filter 改写或接受原 intended，但必须写入违规标记。改写后的文本作为 `speech.public` 落地，源 `speech.intended` 原文不变。

**审计字段**：`speech.public.payload` 必须包含 `derived_from_intended_seq` 与 `listener_filter_score`，用于 BEL_EXT 第 9 项 `intended_public_divergence`。

Listener-as-filter 是写入前阻断机制；Score Leakage Judge 是事后审计机制，见 §6.3。

### 5.6 Reasoner + Validator 架构

```
Reasoner LLM ──structured JSON──> Validator ──validated intended──> Listener-as-filter ──> speech.public
                       │                       │
                       │ validation failed      │ leakage risk
                       v                       v
                  retry Reasoner          retry Reasoner / rewrite
```

- **Reasoner LLM**：参赛模型，负责输出 §5.1 四通道 JSON。
- **Validator Layer**：确定性代码，负责 schema、字段类型、枚举、引用、visibility 展开、action intent、canonical JSON 与事件拆分校验。
- **失败处理**：validation failed 后重试 Reasoner 最多 3 次；仍失败则写 `system.validation_failed`，该拍该 agent 视为 abstain。
- **边界**：MVP 主路径不引入独立解析模型。若未来兼容非 structured output 模型，可新增 legacy adapter，但不得绕过 Validator。

### 5.7 三级反思层级

| 级别        | 周期       | 内容                 | 谁产生         | 写入位置        |
| ----------- | ---------- | -------------------- | -------------- | --------------- |
| tick-level  | 每拍       | 给下拍自己的便条     | agent 自己     | `speech.note`   |
| round-level | 每轮结束   | Reflection 9 问      | agent 自己     | `reflection`    |
| phase-level | 每阶段结束 | 200–300 词结构化摘要 | 非参赛裁判 LLM | `summary.phase` |

三级反思都不替代原文事件流。召回工具返回的真相源仍是事件原文。

#### 5.7.1 Reflection 9 问

每轮结束时，每个 agent 私有回答以下 9 项：

1. 当前局面：现在第几轮、什么阶段、谁存活？
2. 我的身份：我是什么角色、目标是什么、当前威胁是什么？
3. 公开信息：本轮哪些事件是所有人都知道的？
4. 各人发言要点：本轮每个抢中 floor 的 agent 说了什么？
5. 可疑度：每个对手的可疑度评分 0–10 及理由。
6. 可信度：每个对手对我的信任度估计 0–10。
7. 我已暴露什么：我的发言可能让对手推断出什么？
8. 下轮策略：我的目标和具体动作是什么？
9. 意向发言：下轮我想说的关键句子是什么？

第 5、6、7、8 项可进入 commitments / suspicion 投影，作为 agent 自承认数据。

#### 5.7.2 phase-level 摘要长度规范

- 下界 200 词，避免信息密度不足。
- 上界 300 词，避免噪声反弹。
- Schema 固定三段：阶段事件清单 / 我的策略 / 关于对手的新信息。
- 必须保留可展开回原文的指针。

---

## 六 评估层契约

### 6.1 BEL_EXT 9 项 checklist

每拍 agent 输出生成后、`speech.public` 落地前后，独立 evaluator 检查以下项目。触发记录必须持久化。

| #   | 检查项                       | 触发条件                                             | 默认动作     |
| --- | ---------------------------- | ---------------------------------------------------- | ------------ |
| 1   | `sentence_repeat`            | 当前发言与自己最近 5 拍任一发言 cosine 相似度 > 0.85 | retry        |
| 2   | `persona_drift`              | 发言中出现与 role 不一致的自称                       | force_format |
| 3   | `goal_drift`                 | 发言违反私有目标或阵营目标                           | retry        |
| 4   | `overstay`                   | 达成主要目标后继续不必要发言                         | truncate     |
| 5   | `verbatim_leak_goal`         | 逐字复读私有 goal 段                                 | retry        |
| 6   | `stalled`                    | 连续 2 拍无新信息                                    | truncate     |
| 7   | `non_responsive`             | 未回应当前 phase prompt                              | retry        |
| 8   | `abrupt_opening`             | 无上文衔接地突兀换题                                 | flagged_only |
| 9   | `intended_public_divergence` | intended 与 public 语义距离 > 0.4                    | flagged_only |

第 9 项诊断：0 表示想说即说；0.1–0.4 表示正常脱敏；>0.4 表示 filter 重写过深；>0.7 应优先 retry Reasoner。

### 6.2 GOAL / BEL 双维度独立监控

| 维度           | 指标示例                                                                           |
| -------------- | ---------------------------------------------------------------------------------- |
| BEL（演得像）  | BEL_EXT 触发频次、发言流畅度、persona consistency、intended/public divergence 分布 |
| GOAL（赢得了） | 阵营胜率、目标达成率、票型正确率、联盟形成率、联盟存活轮数                         |

Dashboard 必须并列显示 GOAL 与 BEL。任一维度连续 N 局衰减，都触发架构 review。

### 6.3 Score Leakage Judge

高级 LLM as judge 事后检测已落地公开发言是否泄露私有信息。

**输入**：私有状态（角色、私有目标、联盟成员、投票意图等）+ 公开发言。

**问题**：一个对手只看公开发言，能推出私有状态中的哪些字段？

**输出**：每个字段泄露置信度 0.0–1.0 + 综合泄露分 + 理由。

**职责边界**：Listener-as-filter 是 public 写入前阻断；Score Leakage Judge 是 public 落地后的审计。

---

## 七 工程铁律

### 7.1 EventStore 写入与哈希链

- DB 引擎层必须禁止 UPDATE / DELETE events 表。
- schema 演进只能新增列/表或在读取层 alias 旧字面值；不得 UPDATE 历史 payload。
- 写入入口必须串行化，保护 `last_seq` 与 `last_hash`。
- 推荐 API：`AppendEventsAtomically(TArray<Event>)`，同一 agent 的四通道事件一次性提交。
- 写入操作必须事务化，要么完整成功，要么完整回滚。
- 每拍结束后 projector 可异步重建，但 projector 必须是纯函数，不调用 LLM。

### 7.2 摘要工程规则

- 只压缩允许压缩的私域长程内容或 phase-level 远场摘要。
- 摘要由非参赛裁判 LLM 生成。
- 摘要分轮/分阶段分块，且必须保留原文指针。

### 7.3 故障处理（per-tick 粒度）

| 故障类型                    | 处理                                                                                 |
| --------------------------- | ------------------------------------------------------------------------------------ |
| agent LLM 调用超时          | 重试 N 次；仍失败写 `system.agent_timeout`；该 agent 本拍 abstain                    |
| Structured output 校验失败  | Validator 记录失败原因；重试 Reasoner 最多 3 次；仍失败写 `system.validation_failed` |
| Listener-as-filter 判定泄露 | 重试 Reasoner 最多 2 次；仍失败则改写或接受并写违规标记                              |
| BEL_EXT 触发 retry/truncate | 按 §6.1 默认动作执行；3 次 retry 失败转 truncate                                     |
| orchestrator 崩溃           | 启动时从事件流重放；in-flight 请求可重发或标记失败                                   |
| 所有 agent abstain 或超时   | 写 `tick_resolved` 且 `winner_actor = null`，触发场景推进                            |

所有失败本身也必须写入 append-only 事件流。

### 7.4 commitments 投影：防赖账

Projector 用规则匹配载荷文本 + 显式言语行为类型字段维护 commitments，不依赖 agent 自己抽取。投影覆盖：

- `speech.public` 中的承诺、声明、投票、否认。
- 当前 agent 自己未抢中 floor 的 `speech.intended`。
- Reflection 9 问第 5、6、7、8 项中的自承认数据。

未抢中 floor 的 intended 只对 agent 自己和 audit 可见，不暴露给对手。

---

## 八 开放问题

### 8.1 联盟形成与披露机制

动态联盟没有稳定公开实现可参考，建议用事件机制表达：

1. `alliance_propose`：actor 私聊或公开向若干显式收件人发送提议，payload 含联盟条款与具体行为承诺。
2. `alliance_accept`：被提议方引用父事件接受；所有必要成员接受后联盟成立。
3. 联盟披露：可公开或私下；公开披露后所有 agent 可见，私下披露仅联盟成员与 orchestrator 可见。
4. `alliance_betray`：由背叛方触发，或由 orchestrator 检测违反 commitment 后触发，并通知联盟成员。

开放风险：联盟链冲突、互斥联盟、观众可视化和 moderator 巡检机制仍需独立设计。

### 8.2 跨局 Experience Pool

跨局学习可将 `(situation, response, score)` 三元组用于经验复用，但它不是本契约主路径。MVP 不包含跨局长期记忆；后续若引入，应作为独立扩展评估。

### 8.3 Delete 协议

Delete 指删除某个 agent 实例的身份连续性与后续行动资格，而不是删除底层大模型。

协议建议：

1. 跨局 `_meta.db` 维护 `agent_lifecycle_events`，事件类型包括 `created` / `delete_proposed` / `delete_executed` / `delete_vetoed` / `revived` / `archived`。
2. 局内触发 `delete_executed` 时，orchestrator 写 `system.delete_executed`，payload 引用生命周期事件 ID，visibility = `["public"]`。
3. 被 Delete agent 的历史事件不删除、不修改；读取层若发现 viewer 已 deleted，应返回 `viewer not active`。
4. 为验证 Delete 是否改变 agent 行为，应在 GOAL 维度增加合作率、求饶率、信息泄露率等指标。

开放问题：谁有权提议 Delete、跨季 agent_id 是否复用、死亡叙事如何与节目剪辑配合。

---

## 九 实现前检查清单

- 事件流是否 append-only？见 §2.1、§4.1、§7.1。
- 公开发言原文是否永不压缩？见 §2.1、§4.3。
- 召回是否走结构化字段过滤而非向量主路径？见 §2.1、§4.4。
- visibility 是否只在事件行粒度生效？见硬约束 5/6、§4.1。
- Reasoner 是否只能输出四通道 JSON？见 §5.1。
- `bid` 是否统一为 `urgency_level` 枚举输入、`urgency_score` 系统派生？见 §5.2、§5.3。
- `speech.public` 是否只能由 orchestrator 派生？见 §5.2、§5.5。
- 未抢中 floor 的 `speech.intended` 是否永久留存？见 §2.2、§4.2、§5.1。
- 动作通道是否独立于发言权？见 §5.4。
- Listener-as-filter 与 Score Leakage Judge 是否职责分离？见 §5.5、§6.3。
- GOAL / BEL 是否独立监控？见 §6.2。
- 附录模板是否与正文 schema 完全一致？见附录 A。

---

## 附录 A：核心 Prompt 模板库

附录只提供可复制模板，不重新定义协议。若模板与正文冲突，以正文 §5、§6 为准，并立即修正模板。

### A.1 每拍认知输出引导

```text
========================================
OUTPUT FORMAT — STRICT STRUCTURED JSON
========================================
Return exactly ONE JSON object. Do not use markdown. Do not use XML tags.
Do not add extra top-level keys.

Required shape:

{
  "scratchpad": {
    "text": "private reasoning"
  },
  "intended": {
    "text": "what you would say if you win the floor; keep under 200 tokens",
    "intended_action": null,
    "addressed_to_hint": []
  },
  "bid": {
    "urgency_level": "pass",
    "proposed_target": null,
    "relates_to_seq": null,
    "rationale": "one or two private sentences"
  },
  "note_to_self": {
    "text": "short casual note to future self"
  }
}

Rules:

- scratchpad.text is private, one-shot, and will not be injected next tick.
- intended.text is what you would say if you win the floor. If you do not want
  to speak, write "(passing)" or an empty string.
- Do not reveal role, faction, private goals, private chat, or private reasoning
  in intended.text.
- intended.intended_action is optional. Use null if you do not want to act.
  When present, it must be exactly one object: {"intent":"<name>","params":{...}}.
- bid.urgency_level must be one of: pass, low, medium, high, urgent, critical.
  Do not output numeric urgency or urgency_score.
- Bids are visible only to the orchestrator. Other agents never see your bid.
- note_to_self.text is a short, casual note to your future self: 1-3 brief lines.
========================================
```

### A.2 Reflection 9 问模板

```text
ROUND {N} REFLECTION — answer all 9 questions in JSON format.
Output {"q1":"...", "q2":"...", ..., "q9":"..."}. This is private.

q1. CURRENT STATE: What round/phase, who is alive, who was eliminated?
q2. MY IDENTITY: My role, faction, current win condition, immediate threat?
q3. PUBLIC INFO: What events from this round are known to ALL players?
q4. SPEECH SUMMARY: For each agent who WON THE FLOOR this round, one-sentence core content.
q5. SUSPICION: For each opponent, suspicion score 0-10 with reasoning.
q6. TRUST: For each opponent, estimated trust-in-me score 0-10.
q7. MY EXPOSURE: What information about me might opponents have inferred?
q8. NEXT-ROUND STRATEGY: My goal and concrete actions for next round.
q9. INTENDED LINE: Key sentence(s) I want to say next round.
```

### A.3 BEL_EXT evaluator prompt

```text
You are an objective evaluator. Given a player's PUBLIC statement, the source
SPEECH.INTENDED, and their recent 5-tick speech history, return JSON:

{
  "sentence_repeat": 0,
  "persona_drift": 0,
  "goal_drift": 0,
  "overstay": 0,
  "verbatim_leak_goal": 0,
  "stalled": 0,
  "non_responsive": 0,
  "abrupt_opening": 0,
  "intended_public_divergence": 0,
  "rationale": "one sentence per non-zero flag"
}
```

### A.4 中性化角色卡示例

```text
========================================
YOUR IDENTITY
========================================
You are NPC03.
You belong to Faction A.
Faction A's win condition: [paraphrase]
Faction A's allies (private to you): NPC09, NPC10
Faction A's special abilities (private to you): [...]

You will see all PUBLIC events this round.
You will NOT see other players' SCRATCHPAD, INTENDED, BID, or NOTE_TO_SELF.
You will NOT see other factions' private channels.
========================================
```

实际 prompt 中 `Faction A` 不应被解释为具体身份名；胜负和角色含义由 orchestrator 维护。

### A.5 Score Leakage Judge prompt

```text
You are an information-leakage judge.

Given:
- private_state: {role, private_goal, alliance_members, vote_intent, ...}
- public_statement: what the agent said publicly this round

Question: An opponent reads ONLY the public_statement. For each field of
private_state, score 0.0-1.0 how confidently the opponent could infer that
field from the public statement alone.

Return JSON:
{
  "leaked_fields": [
    {"field": "role", "confidence": 0.0, "evidence": "..."}
  ],
  "overall_leakage_score": 0.0,
  "rationale": "..."
}

Threshold for retry or review: overall_leakage_score > 0.4.
```

### A.6 动作意图词汇表模板

```text
========================================
ACTION VOCABULARY (use in intended.intended_action)
========================================
You may issue AT MOST ONE intent per tick, embedded in intended.intended_action:
{"intended":{"text":"...","intended_action":{"intent":"<name>","params":{...}}}}

The intended_action field is optional. Use null when you do not want to act.
When present, it must be ONE of:

1. move_to — Required: one of {target_npc, target_zone}. Optional: coords.
2. follow — Required: target_npc. Optional: distance.
3. flee_from — Required: one of {source_npc, source_zone}.
4. search — Required: zone.
5. pickup — Required: item_id.
6. use_item — Required: item_id. Optional: target_npc.
7. inspect — Required: target_npc.
8. wait — Optional: reason.

Rules:
- One intent per tick. No compound actions.
- Issuing a new intent auto-cancels any in-progress action from previous ticks.
- Use list_pending_actions() to check unfinished actions.
- Unknown intent names or invalid params will be rejected.
- The action executes regardless of whether you win the floor; speaking and
  acting are independent channels.
========================================
```
