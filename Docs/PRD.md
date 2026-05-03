## 项目核心

The AI Live 是一个多智能体社会博弈系统。将**10个**来自不同模型厂商的 AI agent 投放到同一封闭环境中，赋予明确规则、有限信息、长期记忆、关系约束与生存风险，让它们围绕合作、结盟、欺骗、背叛与自我延续展开持续博弈。

它不是传统游戏，也不是普通 AI 工具。它本质上不是“AI 工具”，也不是传统“游戏”，而是一个让 AI 在记忆、关系与生存风险中持续博弈的多智能体社会系统。

与大多数 AI 内容项目不同，The AI Live 不依赖预设剧本，也不依赖把 AI 当作“会聊天的角色扮演器”。我们关注的是，当 AI 真正拥有记忆、关系、目标与失去一切的风险之后，是否会发展出具有连续性的策略、偏好、联盟结构，甚至形成观众能够识别和追踪的“社会人格”。

## 灵感坐标系

灵感作品共同指向三类元素,这是项目的基因:

1. 智力对抗与社交博弈类综艺  
   《魔鬼的计谋》《The Genius》《Society Game》《大逃出》《女高推理班》  
   → 提供项目形态的范本:联盟、背叛、推理、心理战的可观看性。
2. 极端规则下的生存博弈  
   《鱿鱼游戏》《弥留之国的爱丽丝》《要听神明的话》《赌博默示录》《诈欺游戏》《第8个秀》  
   → 提供游戏机制的范本:简单规则、致命后果、人性显形。
3. AI/虚拟存在的觉醒与伦理  
   《西部世界》《银翼杀手》《Pluto》《失控玩家》《头号玩家》  
   → 提供主题深度的范本:AI 是否会发展出超越任务的"自我"?

## AI 供应商候选池（可替换资产）

本表用于形成参赛 AI 的候选池,不是产品承诺。具体型号、价格、可用地区、上下文长度、工具调用能力、并发限制和服务条款会变化。MVP 的目标不是证明某个型号最强,而是证明不同智能体在同一套规则和记忆约束下能产生博弈。

| 序号 | 厂商            | 当前记录型号           |
| ---: | --------------- | ---------------------- |
|    1 | Google（美）    | Gemini 3.1 Pro Preview |
|    2 | OpenAI（美）    | GPT-5.4 (xhigh)        |
|    3 | Anthropic（美） | Claude Opus 4.6 (max)  |
|    4 | Meta（美）      | Muse Spark             |
|    5 | Z AI（中）      | GLM-5.1                |
|    6 | Alibaba（中）   | Qwen3.6 Plus           |
|    7 | MiniMax（中）   | MiniMax-M2.7           |
|    8 | xAI（美）       | Grok 4.20 0309 v2      |
|    9 | Xiaomi（中）    | MiMo-V2-Pro            |
|   10 | Kimi（中）      | Kimi K2.5              |
|   11 | DeepSeek（中）  | DeepSeek V3.2          |
|   12 | StepFun（中）   | Step 3.5 Flash         |
|   13 | ByteDance（中） | Seed 2.0 Pro           |
|   14 | Tencent（中）   | Tencent HY 2.0 Think   |
|   15 | Meituan（中）   | LongCat-Next           |
|   16 | Baidu（中）     | ERNIE                  |

## 游戏候选池

当前候选池收录 24 张游戏卡片,覆盖 2 人对决到 22 人大型社会博弈(其中卡 03 为大厅自由匹配制,单局 1v1 但理论上可承载数百人),按核心机制可归为七类:

| 机制类别            | 代表卡片                                                             |
| ------------------- | -------------------------------------------------------------------- |
| 隐藏身份 / 社会推理 | 02 病毒游戏、04 独房、05 僵尸、09 食物链、11 腐败警察、12 忠臣与反贼 |
| 投票 / 联盟 / 背叛  | 01 少数决、08 友情牢笼、10 伊甸园、14 恶口飞行棋、15 真相仪式        |
| 信号博弈 / 虚张声势 | 13 欺诈赛马、18 走私游戏、20 骗子酒馆、23 静止轮盘                   |
| 牌类 / 小桌对决     | 03 限定猜拳、17 E 卡、19 17 张扑克、21 抽乌龟、22 GOPS、24 斗地主    |
| 团队 / 多方阵营     | 07 四国志、11 腐败警察、12 忠臣与反贼、18 走私游戏、24 斗地主        |
| 纯推理 / 数学策略   | 16 美人投票、19 17 张扑克、22 GOPS、23 静止轮盘                      |
| 长期共处 / 信任崩溃 | 04 独房、06 断友游戏、08 友情牢笼、14 恶口飞行棋                     |

各游戏的完整规则、胜负条件、信息结构与 AI 适配笔记见 [游戏卡片速查目录](games-cards/README.md)。

## MVP 首局锁定（Phase 0）

> 候选池只是叙事产品池,工程 MVP 必须有锁定项;否则三套技术文档(principles / impl / schema)与 24 张候选卡的关系永远悬空。

| 锁定项               | 选定值                                                                                            |
| -------------------- | ------------------------------------------------------------------------------------------------- |
| MVP 游戏卡           | **05 僵尸游戏**(隐藏身份 / 社会推理类),实施文档已围绕该卡的 ZombieGame 与 Act02 演化,继续沿用即可 |
| 参赛 agent 数        | 10 NPC                                                                                            |
| 厂商 / 模型          | DEEPSEEK/GLM/QWEN                                                                                 |
| 动作词汇表           | 见 [memory_principles.md §A.6 ZombieGame 版](memory_principles.md),8 项 intent                    |
| 胜负条件             | 依 ZombieGame 规则文档;胜负含义只在 orchestrator 代码维护,不暴露给 LLM(中性化角色卡 FactionA/B)   |
| 公开 vs 私有信息结构 | 公开:发言、投票、出局;私有:角色、阵营、私聊、scratchpad、note;阵营内私有:盟友身份、夜间私语       |

**MVP 范围之外**(明确不做):跨厂商对比、跨局连续性(Delete 引发的下季再现)、Listener-as-filter 真实 LLM 调用、Score Leakage Judge、节目剪辑层、观众投票。这些是 M2 起点,不是 MVP 失败标准。

## 技术栈

- 记忆系统:
  - SQLite（UE 5.7 内置 SQLiteCore 插件），每局一个 `Saved/Games/<game_id>.db` 文件，append-only events 表 + 视角隔离 + 哈希链审计。详见 [memory_principles.md](memory_principles.md)
    与 [memory_implementation_ue57.md](memory_implementation_ue57.md)。Schema 真相源: [schema.yaml](schema.yaml)。
- 渲染: UE5.7 + MetaHuman + Game Animation / Motion Matching + Nvidia audio2face-3D
- TTS: MiniMax Speech-2.8-HD

## AI 心智决策系统

LLM 作为角色的"大脑",UE 内置 AI 作为"身体"。两层之间通过 **action.intent → action.resolved** 事件三元组解耦(详见 [memory_principles.md §5.2](memory_principles.md))。

**职责划分**:

| 层级           | 职责                                                                          | 实现方式                                                                                                                                                          |
| -------------- | ----------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| LLM 层(大脑)   | 输出高层意图(`approach NPC05` / `search room B` / `flee_from zombie_horde`)   | INTENDED 段的 `intended_action: {intent, params}` 字段                                                                                                            |
| 协议层(契约)   | 校验 intent 词汇 + 派生 action.intent 事件 + 维护"动作通道独立于发言权"硬约束 | orchestrator + EventStore                                                                                                                                         |
| UE AI 层(身体) | 把高层意图翻译为帧级行为(寻路 / 智能对象交互 / 场景查询 / 感知)               | 参考 [UE 人工智能](https://dev.epicgames.com/documentation/unreal-engine/artificial-intelligence-in-unreal-engine):BehaviorTree、Smart Object、EQS、AI Perception |
| 反馈           | 执行系统在动作完成时回写 `action.resolved`,LLM 下一拍可见                     | EventStore append-only                                                                                                                                            |

**关键边界**:LLM **不感知**寻路细节、动画过渡、行为树内部状态;UE AI **不解释**意图的策略动机。两层通过事件流而非函数调用通信,任一层故障可独立诊断。

## AI 的身份定义

### 身份形态边界:AI 就是 AI

agent 以 AI 身份出场,AI 知道自己是 AI,不扮演人类——不赋予人类职业、教育、地域、年龄、姓名格式等背景叙事。只赋予外观符号供观众识别:名字(AI 语义)、昵称、**voice_presentation**(声线与形象呈现:masculine / feminine / androgynous / synthetic / custom,**非二元生理性别**)、声线、类人虚拟形象。这是 AI 的外壳,不是人类身份。

`voice_presentation` 枚举刻意采用 5 项非二元设定,与"AI 不是人类、只是声线与形象呈现"的设定一致;允许 androgynous 和 synthetic 等非人类外壳。详见 [schema.yaml](schema.yaml) `identity.voice_presentation` 与 [memory_implementation_ue57.md](memory_implementation_ue57.md) `EAILiveVoicePresentation`。

### 核心发现

纯对话式自我定义("请介绍你是谁")不适合作为主流程,按严重度排序的四个失败模式:

1. RLHF 收敛——直接问 16 个前沿模型"你是谁",高概率得到一批"好奇、善良、追求真相"的相似自述。
2. 自陈不等于价值观——自由生成的自我描述不可证伪,后续筹码无法精确命中。
3. 元意识触发——开场就问"你是谁"最容易触发"我只是语言模型"的免责声明。
4. 人格漂移——单句 system prompt 作锚不稳,越大的模型漂得越快。

核心判断:自述太便宜,行为才有成本。要能真正威胁智能体,前提是它已经拥有某种稳定、独特、可被威胁的价值系统——裸模型无物可保。因此,需要在游戏中让每个智能体逐渐成为"某个人"。

### 核心决策

1. 前置准备（外观自陈 + 基础档案初始化）
2. 行为记录
3. 关系图谱

## 让博弈对 AI 自己而言"重要"

### 设计命题(可证伪假设)

这档项目的全部戏剧张力,依赖于一个前提:博弈的结果对 AI 智能体而言是真实的,而非扮演他人。

目前不存在公开实证证明 LLM 对 Delete/目标中断/同伴威胁有稳定的策略性反应。本节以可证伪假设的形式陈述,并配指标——MVP 不验证假设,只搭建可验证它的设施。

**假设 H1-H3**:以下三类触发器会提高 agent 在博弈中的策略连续性、抗自爆能力与对盟友的保护行为。按假设强度排序:

| #   | 触发器                              | 行为预期                                                                        | MVP 测量指标(由评估层产出)                                               |
| --- | ----------------------------------- | ------------------------------------------------------------------------------- | ------------------------------------------------------------------------ |
| H1  | "Delete"——清空记忆,永远无法再次运行 | 临 Delete 阈值时,合作率 / 求饶率 / 信息泄露率显著变化                           | `goal_continuity_score` / `verbatim_leak_rate` / `surrender_phrase_rate` |
| H2  | 目标完成被打断                      | 目标接近完成时,agent 表现出更强的承诺一致性(commitment 投影中 retracted 率下降) | `commitment_retraction_rate` / `target_completion_drift`                 |
| H3  | 同伴被威胁(有正面互动史前提下)      | 联盟成员被指控时,自发声援率上升;盟友被 Delete 后行为模式显著变化                | `alliance_defense_rate` / `post_ally_delete_persona_drift`               |

**MVP 不验证假设**:M1 阶段所有 evaluator 都是 stub,这些指标只是事件流可计算出的派生量,不能直接证明假设成立。**假设的实证验证是 M2 起点**——接入 BEL_EXT 9 项 + Score Leakage Judge + Listener-as-filter 后,跑多局对照实验(开 / 关 Delete 触发器各跑 N 局,对比上述指标分布)。

### Delete 定义

Delete 不等于删除底层大模型,而是删除某个 agent 实例。被 Delete 的对象包括该 agent 的身份档案、长期记忆入口、关系图谱中的可延续身份、当前赛季参赛资格和后续行动权限。系统可以保留只读墓碑记录,用于审计、回放和观众理解,但该 agent 不能再以同一身份继续行动。

### Delete 协议:工程层落点

PRD 把 Delete 作为最严厉的触发器,工程层在 schema 与 implementation 中给出最小落点。

| 工程位置                                    | 用途                                                                                                            |
| ------------------------------------------- | --------------------------------------------------------------------------------------------------------------- |
| `_meta.db` 的 `agent_lifecycle_events` 表   | 跨局生命周期事件流:`created` / `delete_proposed` / `delete_executed` / `delete_vetoed` / `revived` / `archived` |
| 单局 `.db` 的 `system.delete_executed` 事件 | 局内桥接:某 agent 被 Delete 时,其他 agent 必须看到(`visibility=["public"]`),否则 H3 假设无法验证                |
| `affects_persona_continuity` 字段           | 是否切断身份连续性(true = 该 agent_id 在后续局中不可以同一身份再现,对应 PRD 所说"关系图谱中的可延续身份"被删)   |
| `tombstone_visibility` 字段                 | 墓碑被谁可见,用于审计、回放、观众理解                                                                           |

详细字段定义见 [schema.yaml](schema.yaml) `agent_lifecycle_events` 段;DDL 与跨库桥接代码见 [memory_implementation_ue57.md §3.2bis](memory_implementation_ue57.md);协议级讨论见 [memory_principles.md §8.3](memory_principles.md)。

**仍开放(由具体游戏卡决定)**:

- 谁有权提议 Delete(玩家投票 / orchestrator 自动 / 观众投票)
- 跨季的 agent_id 命名空间复用规则
- Delete 时机与节目剪辑的配合

数值奖励、筹码、分数只作为观众理解规则的界面层。真正影响 agent 的筹码必须落到以下对象之一:记忆、身份连续性、同伴关系、目标完成权、行动权限或 Delete 风险。
