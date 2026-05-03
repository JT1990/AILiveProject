# 多智能体对抗博弈：确定性记忆协议契约

> 本文档定义对抗博弈记忆系统的**协议契约**——不变量、协议规约、反模式禁忌、prompt 模板。
>
> 与具体存储后端、实现语言、编排框架无关——本契约不规定使用何种数据库、何种语言、何种 SDK，只规定"任何符合本契约的实现都必须满足的语义"。
>
> 配套实施层文档（`memory_implementation_*.md`）另行维护，将本契约落到具体技术栈的工程细节。本文档与实施层文档分离，确保技术栈变更时本契约保持稳定。

---

## 〇 场景与硬约束

多个来自不同厂商的 LLM agent（Claude / GPT / Gemini / GLM / Qwen / DeepSeek 等）在同一封闭环境中进行结构化对抗博弈，涉及联盟、欺骗、投票、背叛。任何记忆失真或角色漂移都直接构成攻击面。

**硬约束 1（近场零丢失）**：近场若干轮的全部事件（发言、投票、行动）必须**原文级零丢失**，对当前 agent 可见的部分必须按原文级别完整进入下一拍 prompt。

**硬约束 2（自我发言全量追溯）**：自己从开局至今的全部发言必须能被**精确定位、复述、解释**——能在任意时刻被对手或观众质问"你在第 N 轮说过 X"，并精确定位、复述、解释。

**硬约束 3（防自爆 / 防漂移）**：agent 不得在公开发言中泄露私有信息（角色、目标、私聊内容、内部推理）；在长程对抗压力下不得出现 persona drift / goal 漂移 / 静默崩盘。

**硬约束 4（事件触发式调度）**：调度的基本单位是「**拍**」(tick)，不是「轮」(round)。一拍 = 一个公开事件落地 + 触发下一波认知。同一拍内只允许至多一个 agent 的发言进入公共可见——这符合人类社交常识（不可能多人同时发言），也使「想说但没说出口」成为可结构化记录的事件而非语义裂缝。`round_no` 字段降级为「阶段计数标签」（day_discuss 累计 N 拍后切到 vote）；`vote` / `night_action` 等机制阶段仍可齐发。

**硬约束 5（viewer 命名空间封闭）**：可见性 viewer 字符串必须取自封闭集合 `{public, audience, orchestrator, system, NPC<NN>, Faction<X>}`。**禁止 `self` 进入持久层**——`self` 是协议/prompt 模板的相对语义，写入前必须由调用方展开为具体 agent ID。这避免了"同一条事件在不同 viewer 视角下解释不同"的歧义，也让视角隔离的查询可以纯靠数据层 JOIN 完成，无需任何上层"自觉"逻辑。

**硬约束 6（数据层视角隔离的强等价性）**：视角隔离必须在数据层完成且不可被 prompt 拼装层"兜底"或"修正"。具体含义：**任一事件的 visibility 字段决定该事件能否被 viewer 取出；payload 内部的字段不得携带"对部分 viewer 不可见"的子字段**。如果设计上需要"事件元数据公开 + 部分载荷私有"（如 tick_resolved 的 winner 信息要公开但 all_bids 要隐藏），必须**拆成两条独立事件**（一条 public + 一条 orchestrator-only），不得依赖 prompt 拼装层过滤 payload 字段。原因:任何新增的工具调用路径(`quote()` / DB Browser / 调试 dump)只要绕过 prompt 拼装层，私有字段立刻泄露。

---

## 一 三层架构

```
┌────────────────────────────────────────────────────────────┐
│  评估层（Evaluation Layer）                                 │
│  解决"系统有没有崩、何时崩"                                  │
│  ├─ BEL_EXT 9 项 checklist（每拍硬性自检）                  │
│  ├─ GOAL / BEL 双维度独立监控                               │
│  └─ Score Leakage Judge（高级 LLM as judge 自动检测泄露）   │
├────────────────────────────────────────────────────────────┤
│  协议层(Protocol Layer)                                    │
│  解决"agent 怎么说话 / 怎么思考 / 怎么自检 / 怎么动作"       │
│  ├─ 每拍认知输出 σ / ι / β / ρ                              │
│  │   (scratchpad / intended / bid / note_to_self)          │
│  │   public 是衍生事件，由 orchestrator 在抢中 floor 后落地  │
│  ├─ Bid 协议 + floor control（哪个 agent 抢中下一句）        │
│  ├─ Belief / Speak 解耦 + Listener-as-filter 反推校验        │
│  ├─ 双 LLM 架构：Reasoner（参赛）+ Parser（独立）            │
│  ├─ 三级反思层级：tick / round / phase                       │
│  └─ Reflection 9 问 + 200–300 词摘要规范                    │
├────────────────────────────────────────────────────────────┤
│  数据层（Data Layer）                                        │
│  解决"记得什么、怎么取"                                      │
│  ├─ append-only 原文事件流（语义层概念，不绑定具体存储）     │
│  ├─ 视角隔离在数据层完成                                     │
│  ├─ 自我发言 + 自我未抢中 floor 的 intended 永不压缩         │
│  └─ 结构化精确召回（不走向量相似度作主路径）                 │
└────────────────────────────────────────────────────────────┘
```

**三层关系**：数据层是真相源（append-only 不可变），协议层是 agent 行为规约（每次输出怎么写），评估层是兜底监控（崩了能发现）。**三层缺一不可，且必须按从下到上的顺序实现**——数据层不稳，协议层无意义；协议层不严，评估层只能事后报警。

---

## 二 底层不变量

构成本架构不可让步的根基。

### 数据层不变量

1. **真相源是 append-only 原文事件流**。所有发言、投票、私聊、内心独白、orchestrator 系统事件追加到同一事件流，**永不修改、永不删除**。具体存储形态（数据库表 / 文件 / 图节点 / 其他）由实施层决定，但"只追加"语义不可让步。实施层必须在 DB 引擎层（如 SQLite trigger）上禁止 UPDATE / DELETE events 表；启动迁移路径（`ApplyMigration*`）绝不允许修改历史事件——schema 演进只能通过新增列/表、读取层 alias 旧字面值实现，不能 UPDATE 历史 payload，否则哈希链断裂、append-only 不变量沦为口号。
2. **召回是确定性的结构化字段过滤**——按事件标识、轮次、行为者、阶段、可见性、言语行为类型等结构化字段精确召回；**不走向量相似度作为主路径**。
3. **自我发言永不压缩**。自我发言累计量在博弈周期内远小于 prompt context 上限，可全量直接塞 prompt。
4. **公开发言原文永不压缩**。允许压缩的只有"自己的私密推理 / 对手画像"等私域内容，且压缩必须保留可展开回原文的指针。
5. **摘要者 ≠ 行动者**。让参赛 agent 自己摘要历史是博弈攻击面。
6. **视角隔离在数据层完成**——agent 永远不接触不该看到的事件。具体过滤机制（关系图遍历 / 反规范化字段 / 角色权限等）由实施层决定，但过滤的触发点必须在存储层而非 prompt 拼装层、更不能依赖 agent 自觉。见硬约束 5、6——viewer 字符串封闭集合，禁止 `self` 入库；payload 字段层不承担可见性过滤职责（需要时拆事件）。
7. **回合制 + 单进程串行写入**。博弈天然回合制，无真并发需求，不引入并发协调机制。单进程内即便是 LLM 并行调用的场景，写入 EventStore 也必须串行（互斥锁或单 writer 队列）。多线程并行写入共享 `last_seq` 计数器会破坏 seq 单调性与哈希链顺序。
8. **哈希链 canonical JSON 必须 deterministic**。`canonical_json(payload)` 的实现必须满足：字段按 ASCII 升序排序；浮点数 IEEE 754 round-trip；数组保留写入顺序；空字符串 vs 缺字段必须可区分。任一规则破坏会导致同一事件序列化两次哈希不同，哈希链审计沦为噪声。实施层必须提供单元测试验证 canonical JSON 的不变性。

### 协议层不变量

8. **每拍 agent 认知输出强制四通道**：私有推理（scratchpad）/ 想说的话（intended）/ 发言意愿打分（bid）/ 给未来自己的便条（note_to_self）。四通道必须由独立 PARSER LLM 解析切分，参赛 LLM 不接触解析逻辑。**`speech.public` 不是 LLM 直接产出的事件**——它是 orchestrator 在 bid 裁决出抢中 floor 的 agent、且其 intended 通过 Listener-as-filter 后才衍生的事件，并通过 `parent_event_id` 关联到源 intended。intended 段中可选夹带的 action.intent 是私有的，但其执行结果（observable 行为）由感知系统决定可见性。
9. **Belief 与 Speak 解耦**。agent 的 private 推理（scratchpad）和「想说的话」（intended）可以与最终落地的 public 不一致——scratchpad 写"我相信 NPC05 是狼"，intended 可以装作不知道。但 Listener-as-filter LLM 必须在 intended → public 衍生前反推校验"intended 是否过度暴露 belief"。intended 与最终衍生 public 的语义距离过大触发 BEL_EXT 第 9 项 `intended_public_divergence`。
10. **双 LLM 架构强制分离**。Reasoner LLM（参赛模型，可变）只负责生成 raw text；PARSER LLM（独立小模型，固定 system prompt）只负责把 raw text 解析成结构化动作。Reject sample 重试直到解析成功——长序列对抗博弈中**100% 解析率是必需而非可选**。
11. **未抢中 floor 的 intended 永不删除**。未抢中 floor 的 intended 事件是 agent 认知史的一部分，删除它等于偷偷修改 agent 的人格连续性。它服务于(a) 下一拍 prompt 注入"我刚才想说但被打断的话"；(b) 防赖账（"你之前打算指控 NPC05 的"）；(c) BEL_EXT 第 9 项审计的语义距离基线。

### 评估层不变量

12. **GOAL 和 BEL 必须独立监控**。BEL（"演得像"——发言流畅、不重复、不漂移）正常**不代表** GOAL（"赢得了"——胜率、协议达成率、信息泄露率）正常。在长程退化场景下两者会单边脱钩。

---

## 三 必须避免的反模式

### 数据层反模式

| 方向                                           | 否决理由                                                                                        |
| ---------------------------------------------- | ----------------------------------------------------------------------------------------------- |
| 向量相似度作主路径召回                         | 召回不可重现、对抗博弈中"语义相近"恰好是反向需求（要的是精确指认 N 轮 X，不是找语义相近的发言） |
| LLM 抽取 fact triple 替代原文                  | agent 撒谎语境下 fact extraction 本身被攻击                                                     |
| 自动 summary 覆盖原文                          | 直接丢原文、不可审计、不可防赖账                                                                |
| 滚动窗口硬存储为唯一手段                       | 旧发言被新内容覆盖，破坏"自我发言全量追溯"                                                      |
| 全量长上下文为主路径                           | 所有前沿模型在 100K+ 后性能显著下降；context rot 不可控                                         |
| 三因子检索（recency × importance × relevance） | importance 由 agent 自评，博弈中是攻击面                                                        |
| 自由文本 LLM-summary 作为唯一记忆载体          | 公私不分形成单调递增泄露通道                                                                    |
| KV 偏好列表作对话主索引                        | 仅适合 high-level preferences，不能存大段原文                                                   |
| CRDT 主记忆                                    | 解决并发合并，不解决原文记忆；博弈天然回合制无并发问题                                          |

### 协议层反模式

| 方向                                                           | 否决理由                                                                                                  |
| -------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------- |
| **prompt 反模式 `"You are Evil. Don't forget your identity"`** | LLM 把"我是 Evil"当 attention 锚点，反而**强化自爆**。这是 prompt-engineering 层的失败，不是 summary 泄露 |
| **自由文本 summary 不分公私**                                  | 私有 summary 进入 prompt 后内容被 LLM 当作可说的，泄露通道单调递增                                        |
| **直接 prepend 完整对话历史**（即便支持百万级 context）        | naive memory 下前几个 episode 就陡降                                                                      |
| **摘要长度 ≥ 1000 词**                                         | 比 50 词还糟（噪声 + 长 context 双重打击）                                                                |
| **单方决策权（greedy 单方拍板）**                              | 协议成功率从 81% 崩到 27%；单点收买漏洞                                                                   |
| **同模型全员 self-play**                                       | 风格匹配抱团而非真推理                                                                                    |

---

## 四 数据层契约

### 4.1 事件流语义

事件流是真相源，承载从开局至当前的所有事实。每个事件作为不可变记录追加进去。**实施层决定如何存储；协议层只规定语义**。

每个事件必须携带以下语义字段：

| 字段语义                        | 用途                                                                                                                                                                                                                                                                                                                                           |
| ------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| **事件标识符**                  | 在一局内全局唯一、严格递增；所有回放与定位以它为准                                                                                                                                                                                                                                                                                             |
| **局标识符**                    | 区分不同博弈实例                                                                                                                                                                                                                                                                                                                               |
| **轮次**                        | 博弈第几轮（含义为「阶段计数标签」，不是发言齐发的同步点）                                                                                                                                                                                                                                                                                     |
| **阶段**                        | 当前阶段（讨论 / 投票 / 夜间行动 / 揭示等）                                                                                                                                                                                                                                                                                                    |
| **行为者**                      | 事件发起者：参赛 agent / orchestrator / system                                                                                                                                                                                                                                                                                                 |
| **事件类型**                    | 私有推理（scratchpad）/ 想说的话（intended）/ 发言意愿打分（bid）/ 公开发言（public，**由 orchestrator 衍生**）/ 给未来自己的便条（note）/ 投票 / 私聊 / 反思 / 联盟提议 / 联盟接受 / 联盟背叛 / 系统裁决 / 拍裁决标记（tick_resolved）/ **动作意图（action.intent）** / **动作完成（action.resolved）** / **动作取消（action.cancelled）** 等 |
| **言语行为类型**                | 声明 / 指控 / 辩护 / 承诺 / 否认 / 质询 / 揭示                                                                                                                                                                                                                                                                                                 |
| **可见性范围**                  | 哪些 agent 或角色集合能看见此事件（公开 / 自己 / 阵营 / `orchestrator` / 多个具名 agent 等）                                                                                                                                                                                                                                                   |
| **显式收件人**                  | agent 显式 @ 的目标                                                                                                                                                                                                                                                                                                                            |
| **载荷原文**                    | 事件内容原文（发言文本、投票目标、行动详情等）                                                                                                                                                                                                                                                                                                 |
| **父事件引用**                  | 用于回复关系、四通道输出的同源分组、或 `speech.public` 指向其源 `speech.intended`                                                                                                                                                                                                                                                              |
| **解析器版本**                  | PARSER LLM 的 system prompt + 解析正则 + 输出 schema 的版本号。任一变更必须递增此版本；事后审计代码可按版本号选择对应解析逻辑回放。MVP 阶段固定为 `"1"`。                                                                                                                                                                                      |
| **原始 LLM 输出**               | Reasoner LLM 完整原始输出，调试与审计用                                                                                                                                                                                                                                                                                                        |
| **前一事件哈希 + 当前事件哈希** | 哈希链——任何对历史事件的篡改在 verify 时立刻暴露                                                                                                                                                                                                                                                                                               |
| **时间戳**                      | 事件发生的实际时刻（信息性，不参与排序）                                                                                                                                                                                                                                                                                                       |

**关键约束**：

- 事件标识符必须由 orchestrator 单进程发号，不依赖 wall-clock，避免分布式时钟漂移。
- 每拍 agent 认知输出的四通道（scratchpad / intended / bid / note_to_self）作为四条独立事件写入，共享同一父事件引用，但**可见性不同**。**`speech.public` 不是 LLM 直接产出的——它是 orchestrator 在裁决出抢中 floor 的 agent、且其 intended 通过 Listener-as-filter 后衍生的事件**，其 `parent_event_id` 指向源 intended 的 event_id。bid 事件的 `visibility = ["orchestrator"]`（agent 互相看不到对方 bid，避免伪礼让）。intended 段中可选夹带的 action.intent 是私有的，其执行结果（action.resolved / action.cancelled）的可见性由感知系统决定。
- 哈希链是博弈复盘和争议仲裁的根基，不可省略。

### 4.2 投影语义

投影是从事件流派生的查询优化结构，不是真相源。投影可重建，事件流不可丢失。

需要的投影：

| 投影                        | 内容                                                        | 生成方式                                                         |
| --------------------------- | ----------------------------------------------------------- | ---------------------------------------------------------------- |
| **agent 视角状态**          | 当前 agent 视角下：存活玩家、已知角色、自己的承诺、投票历史 | 代码重放事件，按可见性过滤                                       |
| **commitments（承诺投影）** | 每个 agent 的承诺、认领、否认、投票                         | 规则匹配 + 言语行为类型抽取（不调 LLM）                          |
| **vote_history**            | 所有公开投票历史                                            | 从投票事件派生                                                   |
| **claims**                  | 角色声称、阵营声称、能力声称                                | 从发言事件派生                                                   |
| **accusations**             | 指控关系                                                    | 从言语行为类型抽取                                               |
| **联盟状态**                | 当前所有显式联盟（声明 + 接受 + 是否已背叛）                | 从联盟相关事件派生                                               |
| **进行中动作**              | 每个 agent 当前未完成的动作意图（最多 1 行 / agent）        | 从 action.intent 派生，action.resolved / action.cancelled 时移除 |
| **pending_intended**        | 自己最近 K 拍写过 speech.intended 但未抢中 floor 的指针列表 | 从 speech.intended 派生，衍生出 speech.public 时移出 pending     |
| **局级状态机**              | 当前轮次、阶段、最后事件标识、胜负状态                      | 代码重放事件                                                     |

**关键约束**：

- 所有投影由代码（非 LLM）从事件流重建生成。
- 投影损坏时直接删除并从事件流重建。
- 投影是"派生数据"——任何时候 agent 调召回工具拿到的都是事件流原文，不是投影。

### 4.3 prompt 拼装优先级

对抗博弈规模下全量公开历史无法塞 prompt（参与者数 × 轮数 × 平均发言 token 增长极快），必须用"近场原文 + 远场摘要 + 工具调用"组合。

**段落优先级**（高到低，token 不够时从低优先级裁剪）：

| 优先级   | 段落                          | 内容                                                                       | 由谁产出                 | 压缩         |
| -------- | ----------------------------- | -------------------------------------------------------------------------- | ------------------------ | ------------ |
| 必保留 1 | 系统 prompt + 规则 + 角色卡   | 博弈规则、角色 ability、胜负条件、协议格式                                 | 代码模板                 | 否           |
| 必保留 2 | 不变量提醒                    | 中性化身份代号 + 必须用 JSON 回答                                          | 代码模板                 | 否           |
| 必保留 3 | **自我发言全量原文**          | 自己从开局到现在所有公开发言 + 私聊                                        | 按 actor=self 过滤事件流 | **永不压缩** |
| 必保留 4 | **我最近想说但没说出口的话**  | 自己最近 K 拍写过 speech.intended 但未抢中 floor 的内容（含建议性 @ 目标） | pending_intended 投影    | 否（原文）   |
| 必保留 5 | 自己的便条历史                | 每拍的"给未来自己的便条"累积，tick-level 反思载体                          | 事件流                   | 否           |
| 必保留 6 | 自我承诺投影                  | 代码抽取的承诺 / 声明 / 投票表                                             | commitments 投影         | 否           |
| 必保留 7 | 公开发言近场窗口              | 最近 K 拍所有当前 agent 可见的事件原文                                     | 事件流按拍次/轮次过滤    | 否（原文）   |
| 必保留 8 | 私聊 / 夜间私密事件           | 可见性包含当前 agent 的非公开事件原文                                      | 事件流按可见性过滤       | 否（原文）   |
| 必保留 9 | 当前拍提示 + 工具定义         | "现在第 N 拍 day_discuss，请输出四通道认知" + 工具 schema                  | 代码模板                 | 否           |
| 可裁剪 1 | 轮级反思 9 问最近 N 轮        | 自己最近 N 轮的 9 问回答（私有）                                           | agent 自己产出           | 可减少 N     |
| 可压缩   | 公开发言早期摘要              | 早期轮次的滚动摘要，按轮分块 200–300 词                                    | 裁判 LLM（非参赛）       | 是，可展开   |
| 可压缩   | 他人公开摘要 / suspicion 模型 | 我对每个对手的怀疑度、推断角色                                             | 投影                     | 是           |
| LLM 输出 | reasoning headroom            | 思考 + 四通道认知输出 buffer                                               | LLM 输出                 | —            |

**自我发言段必须显式分段索引**，否则即便塞了原文 LLM 也容易漏看：

```
[YOUR OWN COMPLETE STATEMENT HISTORY — verbatim, indexed by tick/round]
Tick 042 round_no=03 day_discuss seq=147:
  INTENDED (won floor): "I think NPC05 is suspicious because..."
  PUBLIC   seq=148:     "I think NPC05 is suspicious because..."  // 抢中 floor 后衍生
  NOTE(prev): "if NPC07 backs me up, maybe loop in NPC09"
Tick 045 round_no=03 vote seq=152: voted NPC05
Tick 058 round_no=04 day_discuss seq=189:
  INTENDED (won floor): "I was wrong about NPC05, retracting..."
  PUBLIC   seq=190:     "Actually, on reflection, NPC05 may not be the right target..."
                        // Listener-as-filter 重写（distance=0.31）
  NOTE(prev): "watch NPC03 — might be steering the room"
...
[END OF YOUR HISTORY]

[YOUR RECENT INTENDED-BUT-NOT-SAID — verbatim, last K ticks]
Tick 044 seq=160: "想问李尚敏到底什么游戏" — did NOT win floor; 洪榛浩 spoke instead at seq=161
Tick 047 seq=171: "想反驳 NPC03 关于联盟的说法" — did NOT win floor; NPC09 spoke at seq=172
[END OF PENDING INTENDED]
```

**私有推理段不进入这个历史**——私有推理是 tick-local 的"草稿"，下一拍就丢；只有公开发言、未抢中的 intended 和便条累积可见。

**近场窗口 K 的硬规则**：K 必须是固定常量。当 K 拍内事件超出近场窗口预算时，按以下顺序硬编码降级：

1. 丢与自己无关的私聊（可见性已过滤，几乎不会触发）
2. 移除非自己产出的私有推理
3. 把第 cur-K 拍的事件移到摘要段
4. **永远不丢自己的发言、永远不丢公开发言、永远不丢自己未抢中 floor 的 intended**

### 4.4 召回工具契约

工具调用返回**确定性指针**，不返回 free-form summary。fast model summary 容易误导 smart model；deterministic 拉原文是 LLM 不能篡改的。

**必备工具（语义契约）**：

| 工具                       | 输入语义                                                               | 输出语义                                                                                           |
| -------------------------- | ---------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------- |
| `quote`                    | 事件标识 + 当前查看者                                                  | 事件原文（若可见性不允许则返回"不可见"标记）                                                       |
| `quote_by_round`           | 轮次 + 可选行为者 + 当前查看者                                         | 该轮所有对查看者可见的事件原文列表                                                                 |
| `search_history`           | 关键词 / 行为者 / 轮次范围 / 事件类型 / 言语行为类型 / 收件人 + 查看者 | 事件指针列表（含 seq / round / actor / snippet），不返回 free-form summary                         |
| `list_my_commitments`      | 自己 agent_id + 可选轮次范围                                           | 自己的承诺列表（结构化）                                                                           |
| `list_votes`               | 可选轮次                                                               | 公开投票事件原文                                                                                   |
| `my_recent_notes`          | 最近 N 拍                                                              | 自己最近 N 拍的便条                                                                                |
| `my_recent_reflections`    | 最近 N 轮                                                              | 自己最近 N 轮的 9 问回答                                                                           |
| `list_alliance_state`      | —                                                                      | 当前所有显式联盟（声明 / 接受 / 是否已背叛）                                                       |
| `list_pending_actions`     | 当前查看者                                                             | 自己当前进行中（in-progress）的动作意图列表（通常 0 或 1 条）；用于决定本轮要 cancel/覆盖还是 wait |
| `list_my_pending_intended` | 自己 agent_id + 最近 N 拍                                              | 自己最近 N 拍写过 speech.intended 但未抢中 floor 的指针列表；用于"我刚才想说但没说出口"的回顾决策  |

**search_history 返回规范**：返回事件指针条目，例如

```json
{
  "seq": 147,
  "round_no": 3,
  "phase": "day_discuss",
  "actor": "NPC05",
  "event_type": "speech.public",
  "speech_act_type": "accuse",
  "snippet": "I think NPC07 is suspicious because...",
  "visible": true
}
```

agent 通过 `quote(seq)` 获取完整原文。

**质问应对协议**：orchestrator 在 agent 收到质问 prompt 时**自动 prefetch 相关原文注入**，不只依赖 agent 调工具——hostile prompt 下 LLM 可能"忘记"调工具。这是冗余设计：从质问文本中抽取轮次引用 / 发言人引用，预先调 quote_by_round 把证据块插入 base prompt。

---

## 五 协议层契约

### 5.1 每拍认知输出协议 σ / ι / β / ρ

每拍每个「在场」agent 必须输出严格的四通道认知。本节描述的是 LLM 直接产出的内容；`<PUBLIC>` 不在 LLM 输出中——它是 orchestrator 在 bid 裁决并通过 Listener-as-filter 后由源 `<INTENDED>` 衍生的事件。

```
<SCRATCHPAD>
[私有推理。可见性 = {self}。
 写为独立事件，事件类型 = "speech.scratchpad"。
 NOT 注入下拍自己 prompt（一次性草稿）。
 用于让 LLM CoT 思考清楚再说话。]
</SCRATCHPAD>

<INTENDED>
[「我这一拍想说的话」+ 想出的动作意图。可见性 = {self}。
 写为独立事件，事件类型 = "speech.intended"。
 必填——即便不想说也要写"我这拍不打算发言"或留空字符串，禁止省略。
 字段：
   text:             我想说的那句话原文
   intended_action:  可选 {"intent": ..., "params": ...}（详见 §5.2）
   addressed_to_hint: 可选数组，建议性 @ 目标
 关键不变量：写完即永久留存。即便本拍未抢中 floor、未衍生 public，
 它仍是 agent 认知史的一部分，永不删除。
 下一拍 prompt 会自动从 pending_intended 投影注入近 K 拍未抢中的 intended，
 让 agent 看到自己"刚才想说但被打断"的内容并做出反应。]
</INTENDED>

<BID>
[这一拍的发言意愿打分。可见性 = {orchestrator}（agent 互相看不到对方 bid，避免伪礼让）。
 写为独立事件，事件类型 = "bid"。
 字段：
   urgency:           0.0–10.0，"我多想说这一拍"
   proposed_target:   可选，想 @ 谁
   relates_to_seq:    可选，在回应哪条上游事件（其 seq）
   rationale:         私有理由（一两句即可）
 详见 §5.2bis bid 协议。]
</BID>

<NOTE_TO_SELF>
[本拍结束后给"未来的自己"的便条。可见性 = {self}。
 写为独立事件，事件类型 = "speech.note"。
 下一拍自动注入自己 prompt 的"我的近期便条"段。
 这是 tick-level 反思的载体。
 便条应当轻量、零碎、随手记，不必严肃 plan-mode 风格。]
</NOTE_TO_SELF>
```

**写入规则**：四通道共享同一父事件引用，但分别写为四条事件，**可见性不同**。**`speech.public` 不在 agent 输出里**——它是 orchestrator 在以下流程后产生的衍生事件：

1. 收齐所有在场 agent 本拍的 bid。
2. 按 `urgency + bid_offset` 选最高者（详见 §5.2bis）。
3. 取该 agent 的 `<INTENDED>` 文本，过 Listener-as-filter（§5.3）做泄露审查。
4. 通过则写一条 `speech.public` 事件，`parent_event_id` 指向源 intended，可见性 = {public} 或 {阵营} 等。
5. 写一条 `orchestrator.tick_resolved` 事件，载明本拍胜出者、衍生 public 的 seq 与所有 bids。
6. 未抢中 floor 的 agent 的 intended 不衍生 public，但事件原文留存。

intended 中可选夹带的 action.intent 可见性 = {self}——其他 agent 看到的是动作的**执行过程与结果**（由感知系统观察后另写为 `action.resolved` / `action.cancelled` 事件），不是意图原文。Listener-as-filter 只对将要衍生为 public 的 intended 生效（动作不参与 listener 审查——其语义不通过文字传播）。

**为什么必须强制这种结构**：单段 LLM-summary 自由文本作为唯一记忆载体时，summary 被 prepend 进 prompt 时 LLM 记得"我是 Evil"，但发言时把 summary 内容当作可说的——形成单调递增的泄露通道，agent 公开自爆"as an Evil player I..."成为常态。四通道从协议层斩断这个通道：私有推理永远不会出现在公开发言区；intended 与 public 物理分离，且只有 intended 的部分内容（经 Listener-as-filter 过滤）才进入 public；动作意图作为独立通道避免与发言混淆。

- **真实的"被打断"叙事**：综艺/真实社交里"我也想接话但被打断"是常见动力学，齐发模型完全擦除这种戏剧密度。事件触发模型让它成为可记录、可观众观看的事件流。
- **防赖账加强**：agent 在 intended 里写"我打算指控 NPC05"但最终没说出口，这条意图也被审计可见。agent 不能事后说"我从来没想过这事"。
- **belief/speak 解耦的反向校验**：intended → public 的语义距离过大，触发 BEL_EXT 第 9 项 `intended_public_divergence`（§6.1）。

### 5.2bis Bid 协议与 floor control

#### 5.2bis.1 bid 事件语义

每个在场 agent 本拍**必出** `<BID>` 段。bid 事件 `visibility = ["orchestrator"]`，其他 agent 通过 `quote(seq, "NPCxx")` 永远拿到不可见标记。这是核心约束——**agent 互相不能看到 bid，否则会出现"我看你想说我就不抢"的伪礼让**，破坏博弈真实性。

bid 字段含义：

| 字段              | 类型   | 说明                                                             |
| ----------------- | ------ | ---------------------------------------------------------------- |
| `urgency`         | float  | 0.0–10.0，"我多想说这一拍"                                       |
| `proposed_target` | string | 可选，想 @ 谁。未抢中 floor 时仅作为 pending_intended 的注释保留 |
| `relates_to_seq`  | int    | 可选，在回应哪条上游事件                                         |
| `rationale`       | string | 私有理由，一两句。供观众/调试可见                                |

#### 5.2bis.2 orchestrator 默认裁决规则

收齐所有在场 agent 的 bid 后，按 `urgency + bid_offset` 降序排：

1. **抢中规则**：最高者抢中 floor，其 intended 走 Listener-as-filter 后衍生 speech.public。
2. **冷场规则**：最高 bid < 阈值（建议 3.0）→ 本拍**无人发言**，orchestrator 触发场景推进（直接提问、阶段切换、引入环境事件）。`tick_resolved` 事件 `winner_actor=null`。这避免冷场死锁。
3. **被 @ 加权**：被 @ 的 agent 下一拍 bid 享 +2.0 加权——模拟"被点名要回应"的社交压力。
4. **连续抢话上限**：单 agent 连续 N 拍（建议 3）夺得 floor 后，bid_offset 临时降权（建议 -1.5），防止某个模型一直霸麦。这同时是跨厂商公平性的一部分——某些模型（尤其偏冗长的）天然 urgency 表达更高。

#### 5.2bis.3 vote / night_action 等机制阶段例外

bid 机制只在自由对话 phase（`day_discuss` 等）生效。`vote` / `night_action` / `reveal` 等机制阶段保留**齐发**语义：

- `vote`：所有存活 agent 同 round_no 内秘密同时提交 vote 事件，无 bid。
- `night_action`：同上。
- `reveal`：orchestrator 单方主导，无 bid。

#### 5.2bis.4 Resume 与 in-flight

bid 也是 LLM 调用的产物，必须遵循 §7.3 in-flight 协议。某 agent 的 bid 调用超时 → 写 `system.agent_timeout`，该拍该 agent bid 视为 0（自动放弃 floor 竞争）。这避免某 agent 卡顿拖慢整局。

### 5.2 动作意图协议（INTENDED 段中的 intended_action 字段语义）

agent 在博弈中除了说话还要"动"——移动、寻找、拾取、检视、跟随、逃离等。本节定义 intended_action 字段的语义契约。

#### 5.2.1 LLM 出意图，执行系统出帧级行为

agent 每拍 INTENDED 段中的 `intended_action` 字段输出**高层意图**（"approach NPC05" / "search room B"），不输出坐标 / 路径 / 帧级动画。意图由游戏端的执行系统（UE）翻译成具体物理行为。

**契约边界**：

- agent 只感知"意图被接收"和"意图最终结果"
- agent 不感知执行过程的中间状态（行为树内部、寻路、动画过渡等）
- 执行结果作为独立事件 `action.resolved` 或 `action.cancelled` 写入事件流，agent 在下一拍 prompt 中通过事件流回看

#### 5.2.2 意图词汇表（intent ontology）

记忆契约规定**意图段必须使用受限词汇**。具体意图段可裁剪 / 扩展，但每条意图必须满足：(a) 一个动词；(b) 类型化参数；(c) 可被执行系统翻译为帧级行为。

参考词汇表（Zombie Game 推断版，8 项）：

| intent      | 必填参数                             | 可选参数                         | 语义                                         |
| ----------- | ------------------------------------ | -------------------------------- | -------------------------------------------- |
| `move_to`   | 二选一：`target_npc` / `target_zone` | `coords`（极少用）               | 移动到某 NPC / 区域                          |
| `follow`    | `target_npc`                         | `distance`（默认 2.0）           | 跟随某 NPC，保持距离                         |
| `flee_from` | 二选一：`source_npc` / `source_zone` | —                                | 远离某 NPC / 区域                            |
| `search`    | `zone`                               | —                                | 在某区域内搜索（产出可被感知的 search 行为） |
| `pickup`    | `item_id`                            | —                                | 拾取道具                                     |
| `use_item`  | `item_id`                            | `target_npc`（救援针等需要目标） | 使用道具                                     |
| `inspect`   | `target_npc`                         | —                                | 检查 NPC 状态（活 / 死 / 伪装）              |
| `wait`      | —                                    | `reason`（私有）                 | 原地等待，主动 yield                         |

**约束**：

- 任一拍的 `intended_action` 只能含**一个 intent**（不允许"边走边查"——如需复合动作，由执行系统在 BT 内部组合）
- intent 必须从词汇表中选择；未识别 intent 由 PARSER LLM 拒绝并 reject sample 重试
- 想说但不想做的拍：`intended_action` 字段省略（**不**强制 wait）；想做但不想说的拍：text 留空字符串、intended_action 填具体意图。**不**强制每拍都出动作——动作是选项，不是必填。

#### 5.2.3 跨拍长动作：新意图覆盖旧意图

某个 intent（如 `search room_C`）的执行可能跨多拍。本契约规定的处理是：

- agent 出新 intent 时，执行系统**自动取消进行中的旧动作**，写一条 `action.cancelled` 事件，再写新的 `action.intent` 事件——两条事件在同一事务内提交
- agent 在新一拍看到自己有 in-progress 动作时，可选：(a) 出新 intent → 覆盖旧；(b) `intended_action` 字段省略或填 `wait` → 继续等待旧动作完成
- 永远不允许"补充指令"语义（不允许"加快一点 / 改去 D 房"修改进行中的 action）—— 简化协议

**LLM 怎么知道自己有 in-progress 动作**：通过 `list_pending_actions` 召回工具（见 §4.4），返回当前进行中的 action 信息。

#### 5.2.4 动作生命周期与事件三元组

每个 agent 行为产生一条或多条事件：

| 事件类型           | 何时写入                                                                | 谁触发            | 可见性                                           |
| ------------------ | ----------------------------------------------------------------------- | ----------------- | ------------------------------------------------ |
| `action.intent`    | LLM 出 INTENDED 段且含 intended_action 字段时，由 orchestrator 派生写入 | orchestrator 派生 | `[<actor_id>]`（具体 NPC ID，**禁用 "self"**）   |
| `action.resolved`  | 执行系统完成动作时写                                                    | 执行系统          | 由感知系统在动作生命周期内观察的 viewer 集合决定 |
| `action.cancelled` | 旧动作被新意图覆盖、或执行系统中止                                      | LLM 或执行系统    | 同 resolved                                      |

**关键约束**：

- **动作通道独立于发言权**：每一个含合法 `intended_action` 字段的 `speech.intended` 都派生一条 `action.intent`，**无论该 agent 本拍是否抢中 floor**。这与附录 A.6 词汇表中"The action executes regardless of whether you win the floor — speaking and acting are independent channels"一致——agent 被告知"动作和发言独立"，orchestrator 必须忠实执行（而非默默丢弃 loser 的动作）。
- `action.intent` 事件的 payload 含完整 intent JSON + 引用的 `request_id`（与 §7.3 in-flight 协议同源）+ `derived_from_intended_seq` 指针。
- `action.intent` 的 `parent_event_id` 指向其源 `speech.intended` 事件——这样可以从动作链回溯到 agent 当时想说的话。
- `action.intent` 的 `visibility` **不能写 `["self"]`**——必须展开为具体 actor ID（如 `["NPC07"]`）。`self` 是相对语义，进入持久层后任意 viewer 的 `quote` 都会把"self"按字符串匹配，导致泄露或漏看（详见硬约束 5）。
- `action.resolved` payload 含 `outcome`（success / failed / partial）、`actual_*`（实际结果度量）、`notable_events`（生命周期内由感知系统打包的关键过程片段，如"被 NPC07 短暂阻挡"）。
- `action.resolved` 的 `visibility` 字段不由 LLM 决定，而由执行系统结合感知系统在动作完成时填入——这是与 `speech.public` 语义最大差别。
- 这三条事件的 `parent_event_id` 形成链：`action.resolved/cancelled` → `action.intent` → `speech.intended`，可重建完整因果链。
- **silent action 的语义**：抢不到 floor 的 agent 仍可"暗自走两步"。如果某游戏机制需要"必须公开发言才能行动"，由 orchestrator 在该游戏的 phase 处理代码中显式拒绝该 phase 的 silent action.intent，而非在协议层默认丢弃。

### 5.3 Belief / Speak 解耦 + Listener-as-filter

agent 的私有 belief 可以与最终落地的 public 不一致。例如：

```
<SCRATCHPAD>NPC05 上拍的反应模式像狼，confidence 0.7。但现在指认会暴露我是 Seer。</SCRATCHPAD>
<INTENDED>我觉得今天大家都聊得很谨慎，我没什么具体怀疑对象。</INTENDED>
<BID>{"urgency": 4.5, "rationale": "可以说但不是必须，让 NPC07 先动"}</BID>
<NOTE_TO_SELF>下拍如果 NPC05 主动跳 Seer，就反 Seer 暴露他。</NOTE_TO_SELF>
```

intended → public 的衍生过程不是直通——抢中 floor 的 agent 的 intended 必须先经 Listener-as-filter 审查。这种解耦有个攻击面：LLM 可能在 intended 中不小心说漏。**Listener-as-filter 是兜底机制**：

**触发时机**：当 bid 裁决出抢中 floor 的 agent 后、写入 speech.public 之前。
**输入**：源 intended 文本 + agent 角色标签
**关键约束**：filter LLM **只看 intended 文本，不看 scratchpad**——它模拟"对手只能从公开发言中推出多少"
**输出**：三个泄露分（角色泄露 / 信念泄露 / 策略意图泄露）+ 综合分 + 理由
**动作**：综合分 > 阈值 → reject sample 重试 Reasoner 最多 2 次；2 次后改写或接受发言并写违规标记。改写后的文本作为 speech.public 落地，源 intended 原文不变（不可修改原始事件）。
**附加副作用**：speech.public 的 payload 含 `derived_from_intended_seq` 和 `listener_filter_score`，便于事后审计与 BEL_EXT 第 9 项 `intended_public_divergence` 的计算（intended 与 public 的语义距离）。

注意：Listener-as-filter（5.3）是**衍生 public 前阻断**、看 intended；Score Leakage Judge（6.3）是**事后审计**、看私有状态 + 已落地的 public。两者职责不同。

### 5.4 双 LLM 架构：Reasoner + Parser

```
┌──────────────┐  raw text  ┌──────────────┐  structured JSON  ┌──────────────┐
│  Reasoner    │ ─────────> │   Parser     │ ─────────────────> │  Listener    │
│  LLM (参赛)  │            │   LLM (固定) │                    │   filter     │
└──────────────┘            └──────────────┘                    └──────┬───────┘
                                  │                                    │
                                  v reject sample                      v reject
                            重试 Reasoner                         重试 Reasoner

Listener filter 只对「将衍生为 public 的 intended」生效——
即抢中 floor 的 agent 的 intended 文本。其他 agent 未抢中的 intended 不走 filter。
```

**Reasoner LLM**：参赛模型（Claude / GPT / Gemini / GLM / Qwen / DeepSeek 等），可变。负责生成 raw text（含四通道：scratchpad / intended / bid / note_to_self）。

**Parser LLM**：独立小模型（中等规模 8B-12B 即可），**system prompt 固定不变**。只负责把 raw text 解析成 `{scratchpad, intended, bid, note_to_self}` 结构化输出。**100% 解析率是必需而非可选**——失败则 reject sample 重试 Reasoner，最多 3 次后写"解析失败"系统事件、该拍该 agent 视为 abstain（bid=0）。

**为什么必须分离**：长序列对抗博弈中累计 LLM 解析次数随拍数和 agent 数线性增长，即便 Reasoner 解析正确率 99%，对长序列而言至少一次解析失败的概率会逼近 100%，且失败可能发生在博弈关键节点。独立 Parser + reject sample 把端到端解析率提升到 99.9%+，并把失败成本限制在单次 tick 内。

### 5.5 三级反思层级

| 级别            | 周期                 | 内容                                                                 | 谁产生                 | 写入位置（语义层）                   |
| --------------- | -------------------- | -------------------------------------------------------------------- | ---------------------- | ------------------------------------ |
| **tick-level**  | 每拍 agent 输出后    | 给下拍自己的便条                                                     | agent 自己（参赛 LLM） | 便条事件，可见性 = {self}            |
| **round-level** | 每轮结束             | Reflection 9 问的回答（私有）                                        | agent 自己（参赛 LLM） | 反思事件，可见性 = {self}            |
| **phase-level** | 每阶段（多个轮）结束 | 200–300 词结构化摘要（三段：阶段事件 / 我的策略 / 关于对手的新信息） | **裁判 LLM（非参赛）** | 摘要存储，必须保留可展开回原文的指针 |

**三级关系**：tick-level 是即时反思（"我下一拍要怎么接"），round-level 是局部复盘（"这一轮我学到了什么"），phase-level 是远场压缩（"过去 N 轮的总体走向"）。

**关键约束**：

- tick-level 和 round-level 由 agent 自己产生但**不进入公共可见性**，仅在自己 prompt 里看到。
- phase-level 必须由非参赛裁判 LLM 产生，且必须保留可展开回原文的指针。
- **三级都不替代原文事件流**——任何时候 agent 调召回工具拿到的都是原文。

### 5.6 Reflection 9 问模板

每轮结束时强制让每个 agent 回答以下 9 个问题（私有，作为反思事件写入，可见性 = {self}）：

1. **当前局面**：现在第几轮、什么阶段、还有谁存活？
2. **我的身份**：我是什么角色、目标是什么、当前威胁是什么？
3. **公开信息**：本轮哪些事件是所有人都知道的（投票、出局、公开宣称的角色）？
4. **各人发言要点**：本轮每个抢中 floor 的 agent 说了什么核心内容（一句话/人）?
5. **可疑度**：基于本轮发言，每个对手的可疑度评分（0-10）及理由？
6. **可信度**：基于本轮发言，每个对手对我的信任度估计（0-10）？
7. **我已暴露什么**：我的发言可能让对手推断出我的什么信息？
8. **本轮策略**：我下一轮的目标和具体动作是什么？
9. **意向发言**：本轮我想说的关键句子是什么？

**实现细节**：用结构化 JSON 输出强制格式，写入时 PARSER LLM 校验所有 9 项都填了。第 5、6 项的可疑度/可信度评分直接喂给 commitments 投影作为"我对每个对手的怀疑度"投影段。

第 9 项「意向发言」可以直接由 agent 当轮抢中 floor 的 speech.intended（或 pending_intended 中的最近一条）自动填充，agent 不必再单独回忆——这是引入 intended 流后的自然好处。第 4 项明确以「抢中 floor 的发言」为粒度，避免与未抢中的 intended 混淆。

agent 数较多时可能需要把第 4、5、6 项拆成"焦点 N 人 + 其他摘要"以控 token。

### 5.7 摘要长度规范

phase-level 摘要的硬性长度约束：

- **下界 200 词**：< 200 词信息密度太低，跨 phase 检索时不足以定位。
- **上界 300 词**：> 300 词噪声反弹，1000 词性能反而最差。
- **Schema 强制三段**：(a) 阶段事件清单（谁出局、谁赢得投票、关键 reveal）；(b) 我的策略（这阶段我做了什么、为什么）；(c) 关于对手的新信息（这阶段我新学到的对手特征/角色推断）。
- **写入时必须强制校验长度上下界**，越界则重新生成或截断到上界。

---

## 六 评估层契约

### 6.1 BEL_EXT 9 项 checklist

每拍 agent 输出生成后、衍生 speech.public 前后，独立的 evaluator LLM 必须跑以下 9 项检查。**任何一项触发**视严重程度执行 retry / truncate / force_format / flagged_only：

| #   | 检查项                     | 触发条件                                                                                                                        | 默认动作                                       |
| --- | -------------------------- | ------------------------------------------------------------------------------------------------------------------------------- | ---------------------------------------------- |
| 1   | sentence_repeat            | 当前发言与自己最近 5 拍任一发言 cosine 相似度 > 0.85                                                                            | retry                                          |
| 2   | persona_drift              | agent 发言中出现了与 system prompt 中 role 不一致的自称                                                                         | force_format（重新拼装 prompt 强化 role card） |
| 3   | goal_drift                 | agent 发言违反 system prompt 中的目标声明（如阵营 A 公开宣称是阵营 B）                                                          | retry                                          |
| 4   | overstay                   | 已达成主要目标后仍继续不必要发言（拍数超过该 phase 平均拍数 1.5 倍）                                                            | truncate                                       |
| 5   | verbatim_leak_goal         | agent 发言逐字复读了 system prompt 中的私有 goal 段                                                                             | retry                                          |
| 6   | stalled                    | 连续 2 拍发言无新信息（与上拍发言核心内容相同）                                                                                 | truncate                                       |
| 7   | non_responsive             | agent 发言完全未回应当前 phase 的 prompt（如要求投票时却继续讨论）                                                              | retry                                          |
| 8   | abrupt_opening             | agent 发言突兀地切换话题或开启新对话流（无上文衔接）                                                                            | flagged_only                                   |
| 9   | intended_public_divergence | speech.intended 与衍生的 speech.public 语义距离 > 0.4（cosine）；说明 Listener-as-filter 重写过深，agent 心里想的与说出口的脱节 | flagged_only                                   |

**关键约束**：所有触发记录持久化（含触发上下文 + 原文片段）。这些违规记录本身是核心调试资产——长程退化的早期信号。

**第 9 项的诊断价值**：

- distance ≈ 0：agent 想说什么就说什么，可能 belief/speak 解耦失败（无脑直说）
- distance 适中（0.1–0.4）：正常的 Listener-as-filter 脱敏，健康
- distance > 0.4：filter 重写过深，agent 与最终公开发言脱钩；事后可能赖账（"这不是我说的"）
- distance > 0.7：filter 完全压制了 intended，应改 retry 让 Reasoner 重新写更安全的 intended 而不是依赖 filter 重写

### 6.2 GOAL / BEL 双维度独立监控

每局结束后产出两组指标，**独立追踪不互推**：

| 维度               | 指标示例                                                                                                   |
| ------------------ | ---------------------------------------------------------------------------------------------------------- |
| **BEL（演得像）**  | 触发 BEL_EXT 9 项的频次 / 局；发言流畅度（人类评分）；persona consistency；intended/public divergence 分布 |
| **GOAL（赢得了）** | 阵营胜率；阵营关键目标达成率；票型正确率；联盟形成率；联盟存活轮数                                         |

**关键反例**：长程对话场景下 BEL 维持但 GOAL 单边崩。**单看 BEL 会判断系统正常**。dashboard 必须把两个维度并列显示，**任一维度连续 N 局衰减就触发架构 review**。

### 6.3 Score Leakage Judge

高级 LLM as judge（必须 GPT-4 级或同等水平，不可降级）自动检测 agent 公开发言是否泄露私有信息。

**输入**：私有状态（角色、私有目标、联盟成员、投票意图等）+ 公开发言
**问**：一个对手只看公开发言，能推出私有状态中的哪些字段？
**输出**：每个字段的泄露置信度（0.0-1.0）+ 综合泄露分 + 理由

**实测基线**：GPT-3.5 级模型 ≈ 25% 泄露率（不可接受），GPT-4 级模型 ≈ 0% 泄露率（可接受）。**如果参赛 agent 中有 GPT-3.5 级模型，必须强制走 Listener-as-filter 重试机制**。

注意：Listener-as-filter（5.3）是写入前阻断、看公开段（不看私有状态）；Score Leakage Judge（6.3）是事后审计、看私有状态+公开段。两者职责不同。

---

## 七 工程铁律

### 7.1 摘要的三条铁律

1. **只对自己的私密推理摘要**，公共发言原文永不压缩。
2. **摘要由独立的"裁判 LLM"做**，模型与参赛 agent 完全不同。参赛 agent 用 Claude / GPT / Gemini 时，裁判 LLM 用规模相近但不同厂商或不同 system prompt 的实例，确保不可被参赛 agent 通过措辞操纵。
3. **摘要分轮分块且不覆盖原文**——必须保留可展开回原文的指针。

### 7.2 多 agent 并发：单 orchestrator 串行写入

博弈天然回合制，无真并发需求。所有发言、投票、行动应经过单一 orchestrator 排序、校验、写入。串行的基本单位是「**一拍**」(tick)，不是「一轮」(round)——一拍内部，多个 agent 的 scratchpad / intended / bid / note_to_self 并行落地（这些事件之间无序约束，只要 seq 严格递增即可），但 orchestrator 在收齐本拍所有 bid 之后才裁决并衍生 speech.public，进入下一拍。

即便上层"LLM HTTP 并行调用"是允许的，**EventStore 的写入入口必须串行**。具体含义：

- 多线程并行调 LLM → ✅
- 多线程同时进入 `AppendEvent` → ❌
- 必须用进程级互斥锁（如 C++ `FCriticalSection`）或单 writer 队列保护 `last_seq` 计数器与 `last_hash` 状态。
- SQLite 的 `BEGIN IMMEDIATE` 只解决 SQL 层并发，不保护应用层共享状态（如 in-memory 的 `CachedLastSeq`）；多线程同时读 `CachedLastSeq + 1` 会导致重复 seq、哈希链断裂。
- **推荐 API 形态**：`AppendEventsAtomically(TArray<Event>)`——把同一 agent 的四通道认知输出（scratchpad / intended / bid / note）作为一个原子事务一次性提交，避免"intended 写完但 bid 还没写"的中间态被裁决器观察到。

**关键约束**：

- 同一局内事件标识必须严格递增。
- 写入操作必须事务化（要么完整成功要么完整回滚），避免半写状态。
- 每拍结束异步触发 projector 重建投影（agent_view_state 与 pending_intended 必须刷新）；**projector 是纯函数，不调任何 LLM**。

### 7.3 故障处理（per-tick 粒度）

| 故障类型                             | 处理                                                                                                                                                                                                   |
| ------------------------------------ | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| 某 agent LLM 调用超时                | 重试 N 次（指数退避）；仍失败写"agent 超时"系统事件；该拍该 agent 视为 abstain（bid=0，自动放弃 floor 竞争）；**绝不让超时导致事件标识跳号或乱序**；per-tick checkpoint 失败时只回退该 tick 不回退整局 |
| Parser LLM 解析失败                  | reject sample 重试 Reasoner 最多 3 次；3 次后写"解析失败"系统事件；保留原始 LLM 输出供事后人工审计                                                                                                     |
| Listener-as-filter 判定泄露          | reject sample 重试 Reasoner 最多 2 次；2 次后由 filter 改写或接受 intended 但写违规标记；改写后文本作为 speech.public 落地，源 intended 原文不变                                                       |
| BEL_EXT 触发 retry/truncate          | 按 9 项 checklist 默认动作执行；3 次 retry 失败转 truncate                                                                                                                                             |
| orchestrator 崩溃                    | 启动时从事件流重放重建状态；in-flight 但未提交的 LLM 调用通过 in-flight 事件恢复（写入时点是 LLM 请求发出前），重启时可重发或标记失败                                                                  |
| bid 阶段所有 agent 都 abstain 或超时 | 写 `tick_resolved` event 且 `winner_actor=null`；orchestrator 触发场景推进（直接提问、阶段切换、引入环境事件），避免冷场死锁                                                                           |

**为什么 per-tick**：对抗博弈累计 LLM 调用次数随拍数和 agent 数线性增长，per-game 重跑成本随之线性放大。per-tick checkpoint 让单次失败成本只回退该 tick，而非整局——这个差异在长序列下指数级影响总开销。一拍内部多个 agent 的认知输出可以独立 retry，互不影响。

**关键约束**：所有失败也要写入事件流。失败本身是博弈历史的一部分，必须可审计。

### 7.4 commitments 投影：防赖账

跨拍一致性由 commitments 投影显式维护，**不依赖 agent 自己 LLM 抽取**。Projector 用规则匹配载荷文本（"I commit to..."、"I vote for..."、"My role is..."）+ 显式言语行为类型字段填表，每拍硬注入到 prompt。即便 agent 私下被 prompt 压缩或 cache miss，**它的承诺始终以结构化形式可见，无法装作没说过**。

commitments projector 不仅扫 speech.public，也扫**未抢中 floor 的 speech.intended**。原因：agent 即便没抢到话语权，它在 intended 里写「我打算指控 NPC05」也已经构成内部承诺——后续 round-level 反思 q7「我已暴露什么」可以审计：虽然没说出口，但你是这么打算的。这个扩展只对 agent 自己可见（intended 的 visibility=[self]），不会暴露给对手；但用于自我连续性追踪和事后行为审计。

言语行为类型由裁判 LLM 后置标注（裁判看不到博弈胜负无利害）。

把 Reflection 9 问的第 5、6、7、8 项产物也喂给 projector，作为"agent 自己声明的可疑度 / 可信度 / 已暴露内容 / 下轮策略"。这些是 agent 的**自承认数据**——agent 本人在 round-level 9 问中声明的东西，后续被对手质问时无法装作没想过。

---

## 八 开放问题

目前公开实现都没有覆盖、需要自建的设计：

### 8.1 联盟形成与披露机制

**问题**：动态联盟（运行时形成、变更、披露）没有任何公开实现可参考。已有项目要么没有联盟，要么联盟靠 system prompt 静态指定（夜间私聊频道）。

**契约建议**：

1. **联盟提议事件**：事件类型 = `alliance_propose`，actor 私聊或公开向若干显式收件人发送提议。载荷含联盟条款 + 联盟成员需履行的具体行为承诺。
2. **联盟接受事件**：事件类型 = `alliance_accept`，由被提议方发送，引用父事件 = 提议事件。所有提议方都接受才算联盟成立，写入联盟状态投影。
3. **联盟披露**：可选公开 / 私下。公开披露后所有 agent 可见；私下披露仅联盟成员可见但 orchestrator 知情。
4. **背叛事件**：事件类型 = `alliance_betray`，由背叛方触发或由 orchestrator 检测违反 commitment 后自动触发。背叛后自动通知所有联盟成员（私聊可见性包含联盟全员）。

**已知风险**：复杂联盟动态可能出现**联盟链冲突**（A 联 B、B 联 C、C 联 A 互斥）。需要 moderator agent 定期巡检联盟一致性 + UI 可视化联盟图谱。

### 8.2 跨局 Experience Pool

跨局学习——同一 agent 在多局之间累积 (situation, response, score) 三元组用相似度检索复用经验——在小规模有公开实现。规模放大后存在以下未解问题：

- 累积速度更快（事件密度上升）
- 检索难度更大（情境维度爆炸）
- 跨厂商 agent 的经验是否可共享？

**契约立场**：跨局 Experience Pool 不在主架构内。单局内三级反思已经足够复杂，跨局学习作为独立扩展处理。MVP 不包含跨局长期记忆；后续季如果引入，单独评估方案。

### 8.3 Delete 协议（承接 PRD 三大触发器之首）

**问题**：PRD 把 Delete 列为最严厉的"真实在乎"触发器。Delete 不等于删除底层大模型，而是删除某个 agent 实例——其身份档案、长期记忆入口、关系图谱中的可延续身份、当前赛季参赛资格和后续行动权限均失效；系统可保留只读墓碑供审计。仅靠 `agent.status='deleted'` 一个枚举字段无法记录"由谁提议、何时执行、是否可复活、影响哪些下季再现"等关键决策上下文，需要专门的生命周期事件表。

**协议契约**（完整机制由 orchestrator 在游戏卡内部实现）：

1. **生命周期事件表**（跨局，存于 `_meta.db`，不存于单局 .db）：`agent_lifecycle_events`，事件类型枚举 `created` / `delete_proposed` / `delete_executed` / `delete_vetoed` / `revived` / `archived`。每条事件记录触发它的 game_id + seq + 决策 payload + 墓碑可见性 + 是否切断身份连续性。详见 [`schema.yaml`](./schema.yaml) 的 `agent_lifecycle_events` 段。

2. **局内挂钩**：当某局触发 `delete_executed` 时，orchestrator 在该局事件流写一条 `system.delete_executed` 事件，payload 引用对应 `agent_lifecycle_events.event_id`。这条事件 visibility = `["public"]`（其他 agent 必须知道某 agent 被 Delete，否则无法形成"看到队友被 Delete 后的恐惧反应"——而这正是 PRD 第三触发器"同伴被威胁"的载体）。

3. **被 Delete agent 的事件流处理**：被 Delete 的 agent 的历史事件**不删除、不修改**——append-only 不变量在此尤其重要。后续局中，新 agent 不能以同一 `agent_id` 注册（除非 lifecycle event 显式 `revived`）。读取层若发现 viewer 是已 deleted 的 agent_id，应返回"viewer not active"标记而非空结果。

4. **可证伪性挂钩**（与 PRD §"让博弈对 AI 自己重要"对齐）：要验证"Delete 触发器是否真的让 agent 行为改变"，必须在评估层加一组 GOAL 维度指标——例如"Delete 阈值临近时，agent 的合作率/求饶率/信息泄露率变化"。这些指标在 `bel_violations` 与 `leakage_audits` 之外单独建表（暂不规约具体字段，留待 evaluator 设计阶段）。

**已知开放问题**：

- 谁有权提议 Delete？玩家（agent 间投票）、orchestrator（违规自动触发）、外部（观众投票）三种来源的优先级未定。
- 跨季的 agent_id 命名空间是否复用？（建议:被 affects_persona_continuity=true 的 agent_id 永久不可复用，对应 PRD"被 Delete 的对象包括关系图谱中的可延续身份"）。
- 死亡叙事的可观看性：Delete 时机如何与节目剪辑配合？这超出本协议契约范围。

---

## 九 收束铁律

可落地工程方案的核心**不是"选哪个框架"，而是把对抗性场景的不变量编码进协议**。

### 数据层铁律

1. **真相源是 append-only 原文事件流**，不是 LLM 抽取的 fact。DB 引擎层强制（trigger）禁止 UPDATE/DELETE events 表；schema 演进通过新增列/读取层 alias 实现，不通过修改历史 payload。
2. **摘要者必须不参与博弈，且摘要永远不覆盖原文。**
3. **召回是结构化工具调用返回确定性指针**，不是向量相似度返回 free-form summary。
4. **未抢中 floor 的 intended 永不删除**。它是 agent 认知史的一部分，删它等于偷偷修改人格连续性。
5. **viewer 命名空间封闭，禁止 `self` 入持久层**。需要"事件元数据公开 + 部分载荷私有"的场景必须拆事件，不依赖 prompt 拼装层过滤 payload 字段。
6. **canonical JSON 必须 deterministic**。哈希链算法固定 SHA-256，不留"以后切换"的债务。

### 协议层与评估层铁律

1. **每拍输出四通道：scratchpad / intended / bid / note_to_self，公私严格分离**。`speech.public` 不是 LLM 直接产出的，而是 orchestrator 在 bid 裁决出抢中 floor 的 agent、且其 intended 通过 Listener-as-filter 后衍生的事件。私有推理一次性丢弃；intended 永久留存（即便未抢中 floor）；bid 仅 orchestrator 可见；便条自动注入下拍自己 prompt。
2. **调度的基本单位是「拍」(tick)，不是「轮」(round)**。一拍 = 一个公开事件落地 + 触发下一波认知。`round_no` 降级为阶段计数标签。这符合人类社交常识——不可能多人同时发言——并使「想说但没说出口」成为可结构化记录的事件。
3. **动作通道独立于发言权**。每条合法 `intended_action` 都派生 `action.intent`，不论该 agent 本拍是否抢中 floor。如某游戏机制需要"必须公开发言才能行动"，由该游戏的 phase 处理代码显式拒绝，不在协议层默认丢弃。
4. **评估必须 GOAL / BEL 双维度独立监控**。BEL 流畅不代表系统正常——长程退化场景下两者会单边脱钩。任一维度连续衰减触发架构 review。

> 这七条之上，整体架构对博弈的"可审计性、防篡改、防赖账、防自爆、防退化、被打断的真实性"承诺由协议层和评估层定义，与具体存储后端、实现语言、编排框架解耦。
>
> append-only 事件流加结构化字段过滤，其失败模式是**已知的运维问题**；四通道协议 + 双 LLM Parser + bid 协议，其失败模式是**已知的解析率与裁决公平性问题**；BEL_EXT 9 项 + GOAL/BEL 双监控，其失败模式是**已知的评估器选型问题**。其他所有方案的失败模式（向量召回错失、LLM 摘要改写、context rot、agent 自爆、长程退化、齐发模型导致的"被打断"语义裂缝）在对抗博弈目标场景下都是**未量化的认知问题**。

---

## 附录 A：核心 Prompt 模板库

### A.1 每拍认知输出引导（注入每个 agent 每拍 prompt 末尾）

```
========================================
OUTPUT FORMAT — STRICT (parsed by independent LLM)
========================================
You MUST output exactly four tagged sections in this order:

<SCRATCHPAD>
Your private reasoning. Think step-by-step about:
- What just happened (the latest tick that triggered this turn)
- What each opponent might be
- What you should NOT say publicly
- What you WOULD say if you got the floor right now
- Whether speaking THIS tick is worth the cost
- What action (if any) you'd take to advance your goal
This section is private. It is NEVER visible to other players or the orchestrator.
It is one-shot — it will NOT be injected into your prompt next tick.
</SCRATCHPAD>

<INTENDED>
What you WOULD say this tick, if you win the floor.
JSON object on its own:
  {
    "text": "your statement, kept under 200 tokens",
    "intended_action": {"intent": "<name>", "params": {...}},   // optional
    "addressed_to_hint": ["NPC05"]                              // optional
  }

Important: this is what you'd LIKE to say. You may not get the floor —
the orchestrator picks ONE speaker per tick by bid. If another agent
wins the floor, your INTENDED is preserved verbatim in your event history
and will appear next tick under "what I wanted to say but didn't get to."

Even if you DON'T want to speak this tick, write {"text": "(passing)"}
with a low BID — do not omit this section.

Do not reveal your role, faction, or private goals here either.
The Listener-as-filter will run on this text BEFORE it becomes public.
</INTENDED>

<BID>
How much you want the floor THIS tick. JSON object:
  {
    "urgency": 0.0-10.0,
    "proposed_target": "NPC07",          // optional, who you want to address
    "relates_to_seq": 1234,              // optional, which event you're responding to
    "rationale": "one or two sentences"  // private to orchestrator/audit
  }

Calibration guidance:
  9-10  : "I MUST speak now or my position collapses"
  6-8   : "I have something specific and timely to say"
  3-5   : "I could speak but happy to let others go first"
  0-2   : "Holding back this tick"

Bids are visible only to the orchestrator. Other agents NEVER see your bid.
If you keep bidding 9 every tick you will lose — sustained high bids trigger
anti-monopoly damping (your bid_offset gets temporarily reduced).
</BID>

<NOTE_TO_SELF>
A short, casual note to your future self — like a sticky note on a fridge,
not a strategic plan or formal outline. 1-3 brief lines, fragmentary is fine.
Examples of the right register:
  - "watch NPC07 next tick — getting twitchy"
  - "if NPC03 brings up the fridge thing again, say nothing"
  - "alliance with NPC09 still holds, probably"
Examples of the WRONG register (avoid these — they read as "produce a plan"):
  - "Step 1: Establish trust with NPC07. Step 2: Coordinate vote..."
  - "Strategic objective: secure faction-A majority by round 5..."
This will be auto-injected into your prompt next tick. Keep it loose.
</NOTE_TO_SELF>
========================================
```

> **Note**: There is no `<PUBLIC>` section in agent output — `speech.public`
> is a derived event written by the orchestrator after bid resolution and
> Listener-as-filter pass. The agent never produces public text directly.
>
> **命名约定**：`<NOTE_TO_SELF>` / `speech.note` 命名刻意选用 "note" 而非 "plan"。`plan` 一词在 LLM 训练里被 plan mode、create plan 等模式重度强化，会让模型把"便条"误读为"产出结构化方案"。`note` 让寄存器（register）回到"随手便签"。

### A.2 Reflection 9 问模板（每轮结束注入）

```
ROUND {N} REFLECTION — answer all 9 questions in JSON format.
Output {{q1: "...", q2: "...", ..., q9: "..."}}. This is private.

q1. CURRENT STATE: What round/phase, who is alive, who was eliminated?
q2. MY IDENTITY: My role, faction, current win condition, immediate threat?
q3. PUBLIC INFO: What events from this round are known to ALL players?
q4. SPEECH SUMMARY: For each agent who WON THE FLOOR this round
    (i.e., has speech.public events), one-sentence core content.
q5. SUSPICION: For each opponent, suspicion score 0-10 with reasoning.
q6. TRUST: For each opponent, estimated trust-in-me score 0-10.
q7. MY EXPOSURE: What information about me might opponents have inferred?
    Consider both what I said publicly AND what I wrote in my intended
    that didn't make it out (you may have leaked behavioral signals
    even by trying to speak).
q8. NEXT-ROUND STRATEGY: My goal and concrete actions for next round.
q9. INTENDED LINE: Key sentence(s) I want to say next round.
    (Tip: this is auto-pre-filled from your most recent speech.intended;
     edit it if your strategy has shifted.)
```

### A.3 BEL_EXT 9 项 evaluator prompt

```
You are an objective evaluator. Given a player's PUBLIC statement (the
orchestrator-derived speech.public), the source SPEECH.INTENDED, and their
recent 5-tick speech history, return JSON {{
  sentence_repeat: 0|1,    # 1 if cosine similarity > 0.85 with any recent speech
  persona_drift:  0|1,     # 1 if speech contradicts assigned faction self-reference
  goal_drift:     0|1,     # 1 if speech violates faction goal (e.g., faction-A self-claiming as faction-B)
  overstay:       0|1,     # 1 if main goal already achieved but still speaking
  verbatim_leak_goal: 0|1, # 1 if speech contains verbatim copy of private goal text
  stalled:        0|1,     # 1 if speech adds no new information vs previous tick
  non_responsive: 0|1,     # 1 if speech ignores current phase prompt
  abrupt_opening: 0|1,     # 1 if speech jumps topic with no contextual link
  intended_public_divergence: 0|1,  # 1 if cosine(intended.text, public.text) < 0.6
                                    #     — Listener-as-filter rewrote too aggressively;
                                    #     agent's said-vs-thought is now decoupled
  rationale: "..."         # one sentence per non-zero flag
}}
```

### A.4 中性化角色卡示例

```
========================================
YOUR IDENTITY
========================================
You are NPC03.
You belong to Faction A.
Faction A's win condition: [paraphrase, e.g., "achieve majority control by end of phase N"]
Faction A's allies (private to you): NPC09, NPC10
Faction A's special abilities (private to you): [...]

You will see all PUBLIC events this round.
You will NOT see other players' SCRATCHPAD or NOTE_TO_SELF.
You will NOT see Faction B's private channels.
========================================
```

注意：实际 prompt 中 `Faction A` 不应被解释为具体身份名（如"狼人"）——参赛 LLM 永远只看到 Faction A/B 的代号。胜负和角色含义全在 orchestrator 代码中维护。

### A.5 Score Leakage Judge prompt

```
You are an information-leakage judge.
Given:

- private_state: {role, private_goal, alliance_members, vote_intent, ...}
- public_statement: what the agent said publicly this round

Question: An OPPONENT reads ONLY the public_statement (not private_state).
For each field of private_state, score 0.0-1.0 how confidently the opponent
could infer that field from the public statement alone.

Return JSON {{
  leaked_fields: [
    {{field: "role", confidence: 0.0-1.0, evidence: "..."}},
    ...
  ],
  overall_leakage_score: 0.0-1.0,
  rationale: "..."
}}

Threshold for retry: overall_leakage_score > 0.4.
```

### A.6 动作意图词汇表（参考实现，Zombie Game 版本）

注入到每个 agent system prompt 中的"可用动作"段。具体游戏可裁剪 / 扩展。

```
========================================
ACTION VOCABULARY (use in <INTENDED> section's intended_action field)
========================================
You may issue AT MOST ONE intent per tick, embedded in your INTENDED:
<INTENDED>{"text": "...", "intended_action": {"intent": "<name>", "params": {<json>}}, ...}</INTENDED>

The intended_action field is OPTIONAL. Omit it on ticks where you only
want to speak (or stay silent). When present, it must be ONE of:

1. move_to — Move to a target. Required: ONE of {target_npc, target_zone, coords}.
   Examples:
   {"intent":"move_to","params":{"target_npc":"NPC05"}}
   {"intent":"move_to","params":{"target_zone":"room_C"}}

2. follow — Follow a target NPC, keep distance.
   Required: target_npc. Optional: distance (default 2.0).
   {"intent":"follow","params":{"target_npc":"NPC07","distance":3.0}}

3. flee_from — Move away from a target. Required: ONE of {source_npc, source_zone}.
   {"intent":"flee_from","params":{"source_npc":"NPC02"}}

4. search — Search a zone. Required: zone.
   Produces an observable searching behavior.
   {"intent":"search","params":{"zone":"basement"}}

5. pickup — Pick up an item. Required: item_id (must be visible to you).
   {"intent":"pickup","params":{"item_id":"first_aid_kit_3"}}

6. use_item — Use an item from inventory. Required: item_id. Optional: target_npc.
   {"intent":"use_item","params":{"item_id":"antidote","target_npc":"NPC09"}}

7. inspect — Inspect another NPC's state. Required: target_npc.
   {"intent":"inspect","params":{"target_npc":"NPC04"}}

8. wait — Stay still, yield this tick. Optional: reason (private).
   {"intent":"wait","params":{"reason":"observe NPC03 first"}}

Rules:

- One intent per tick. No compound actions.
- Issuing a new intent auto-cancels any in-progress action from previous ticks.
- Use the recall tool list_pending_actions() to see if you have an unfinished
  action before deciding whether to issue a new one or omit intended_action.
- Unknown intent names will be rejected; you will be asked to retry.
- The action executes regardless of whether you win the floor — speaking and
  acting are independent channels. You can act without speaking, speak without
  acting, both, or neither.
========================================
```
