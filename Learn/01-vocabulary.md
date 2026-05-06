# 阶段 1 — 词汇表：事件、Bid、Agent、Roster

读完这一阶段你应当能回答（这就是计划里的 3 道自检题 + 加 1 题）：

1. `Visibility=['public']` 和 `Visibility=['system']` 的事件，谁能在 prompt 里看到？
2. `EventType::SpeechIntended` 和 `SpeechPublic` 区别？为什么要分？
3. `FAILiveBid.RuntimeAdj` 是用来干嘛的？
4. （加题）为什么 `EAILiveVoicePresentation` 没有 `Male/Female`？

后面所有阶段都基于本阶段的术语。读慢一点没关系。

---

## 1. `AILiveEventTypes.h` — 系统的"语言"

这个文件**没有任何函数实现**，全是类型声明。它定义了系统所说的全部"词"：

### 1.1 `FAILiveEvent` —— 18 个字段，事件原子单位

```cpp
struct FAILiveEvent {
    FString  EventId;        // UUIDv7（时间排序友好）
    FString  GameId;         // 一局游戏的 id
    int64    Seq;            // 单局内自增序号；唯一
    int64    TickNo;         // ⚠ 不参与哈希链，仅作切片 / 调试用
    int32    RoundNo;        // 第几个发言轮次
    EAILivePhase  Phase;     // 游戏阶段：Setup/DayDiscuss/Vote/NightAction/Reveal/GameOver
    FString  Actor;          // 谁产生的事件（NPC1.. / "orchestrator" / "system"）
    EAILiveEventType  EventType;       // 24 种之一（见下）
    EAILiveSpeechActType  SpeechActType;  // 发言意图分类（claim/accuse/...）
    TArray<FString>  Visibility;          // 谁能看到（"public" / "system" / NPCID 列表）
    TArray<FString>  AddressedTo;         // 显式 @ 给谁（影响 bid 加权）
    FString  PayloadJson;                 // 真正的"内容"——JSON 文本
    FString  ParentEventId;               // 派生关系（intended → public 这种）
    FString  ParserVersion;               // 当前固定 "1"，未来改解析格式时 bump
    FString  RawLLMOutput;                // Reasoner 原始输出，调试 / 训练用
    FString  PrevEventHash;               // 链上一条
    FString  EventHash;                   // = SHA256(PrevEventHash || canonical_json(self))
    FString  WallClock;                   // ISO8601 实际写入时间
};
```

**3 个分组**便于记忆：

| 分组                    | 字段                                                                                                                    | 由谁填               |
| ----------------------- | ----------------------------------------------------------------------------------------------------------------------- | -------------------- |
| **业务**                | `GameId` `RoundNo` `Phase` `Actor` `EventType` `SpeechActType` `Visibility` `AddressedTo` `PayloadJson` `ParentEventId` | 调用方填             |
| **审计**                | `ParserVersion` `RawLLMOutput`                                                                                          | 调用方填（可空）     |
| **EventStore 自动回填** | `EventId` `Seq` `TickNo` `PrevEventHash` `EventHash` `WallClock`                                                        | `AppendEvent` 内部填 |

⚠️ **关键不变量（哈希链头号陷阱）**：

> `TickNo` **被故意排除在 canonical JSON 之外**，所以不参与哈希计算。
>
> 原因：哈希按 seq 顺序递增；但 `tick_no` 是上层 BeginTick 决定的"逻辑时序"，可能和物理 seq 顺序不一致（比如 RebuildProjections 修复历史投影时）。如果它进了哈希链，重放或修复都会破链。
>
> 这就是头注释说的：_"canonical JSON 永远排除该字段，仅作 prompt / 调试 / 切片读取用途，不参与哈希链"_。
>
> **改动建议**：你以后想给 `FAILiveEvent` 加任何字段，先想清楚——是审计性的（要进哈希），还是调试性的（要排除）。判错了就破链。

### 1.2 `EAILiveEventType` —— 24 种事件，分 5 类

按命名前缀分组：

```
speech.*   — 发言（4）：public、scratchpad、intended、note
bid        — 1：抢麦投标
reflection.9q — 1：每轮 9 问反思
vote       — 1：投票
private_msg — 1：私聊
alliance.*  — 3：propose / accept / betray
action.*    — 3：intent / resolved / cancelled
orchestrator.* — 4：round_resolved / tick_anchor / tick_resolved / tick_audit
system.*   — 5：role_assigned / agent_timeout / parse_failed / llm_inflight / delete_executed
winner_decision — 1
                                                          总：24
```

**最重要的区分（就是自检题 #2）**：

| 事件               | 含义                                | 谁能看                                  |
| ------------------ | ----------------------------------- | --------------------------------------- |
| `SpeechIntended`   | NPC 想说但**还没赢得发言权**的话    | 仅 actor 自己 + system；其他 NPC 看不到 |
| `SpeechPublic`     | 经过 Bid 解算**赢家实际播出**的发言 | `Visibility=['public']`，所有人都看到   |
| `SpeechScratchpad` | 内心独白，**永远不公开**            | 仅 actor 自己                           |
| `SpeechNote`       | 给自己的备忘录（"下回再聊这个"）    | 仅 actor 自己                           |

为什么要分？因为社会博弈的核心是**信息不对称**——不分的话 prompt 里就泄露所有人的"真实意图"了。`Bid`+`SpeechIntended` 是密封的"投标信封"，`SpeechPublic` 是开标后才公布的赢家发言。

### 1.3 `EAILivePhase` 与 `EAILiveSpeechActType`

- `Phase`：6 个，看名字就懂——这是**狼人杀风格**的回合制。
- `SpeechActType`：7 种发言意图标签——`Claim / Accuse / Defend / Commit / Deny / Question / Reveal`。这不是 LLM 输出的类型，**是 Parser 后给每条 speech 打的 tag**，方便 reducer 索引"NPC2 一共 accuse 过谁几次"这种查询。

### 1.4 `FAILiveCommitment` —— 派生表的行结构

注意：**这不是事件本身**，是 T8 的 `commitments` reducer **从事件流投影出的结构**。

```cpp
struct FAILiveCommitment {
    FString GameId, AgentId;
    int32   RoundNo;
    int64   Seq;                  // 来源事件的 seq
    EAILiveCommitmentType   CommitmentType;  // promise / claim_role / deny / vote_for / alliance
    FString Target;
    FString Text;
    EAILiveCommitmentStatus Status;          // active / retracted / contradicted
};
```

**为什么需要它？** 因为 prompt 里要回答"我之前承诺过什么、还有效吗"这种问题，每次现 grep 全事件流太慢。Reducer 把承诺类事件**投影成一张表**，prompt 直接查表。

`Status` 是 reducer 算出来的：

- 后续被相同 actor 的 `deny` 撤销 → `Retracted`
- 后续出现矛盾的承诺 → `Contradicted`

### 1.5 `namespace AILiveEvent` —— enum ↔ string 转换

```cpp
PhaseToString / EventTypeToString / SpeechActToString / ...
PhaseFromString / EventTypeFromString / ...
ArrayToJsonString(TArray<FString>) / JsonStringToArray(FString)
```

**为什么单独有这堆？** 因为枚举要往 SQLite 里存的时候**必须落成字符串**（不能存 uint8，否则换 schema 后值漂移）。所有数据库读写都通过这堆函数。**如果你以后要重排或新增 EventType，必须同步改这里**——不然数据库读出的旧记录解不出来。

---

## 2. `AILiveBidTypes.h` — 抢麦协议

只有 2 个 struct，**无任何函数**。但它是 §5.4 反霸麦机制的核心。

### 2.1 `FAILiveBid` —— 一份"投标"

```cpp
struct FAILiveBid {
    FString Actor;            // 投标者
    int64   Seq;              // bid 事件本身的 seq
    int64   IntendedSeq;      // 配对的 speech.intended 事件 seq
    float   Urgency;          // [0..1] LLM 自评：这话有多紧迫
    float   BidOffset;        // 每 NPC 静态偏置（来自 Battle.BidOffset）
    float   RuntimeAdj;       // ⭐ 运行时调整（见下）
    float   FinalScore;       // = Urgency + BidOffset + RuntimeAdj
    FString ProposedTarget;   // 想 @ 的对象
    FString Rationale;        // 简短解释（写给自己未来读）
};
```

### 2.2 `RuntimeAdj` —— 自检题 #3 答案

注释明确写了：

> _"T7 加：principles §5.4 **反霸麦衰减 + 被 @ 加权 + 沉默加权**汇总，用于 tick_audit.all_bids[].runtime_adj"_

也就是 `RuntimeAdj` 是**三股力**叠加而成的运行时分数调节：

| 力             | 方向 | 用意                                                               |
| -------------- | ---- | ------------------------------------------------------------------ |
| **反霸麦衰减** | 减分 | 同一 NPC 连续赢 → 下一轮分数被压低，避免一人独占发言权             |
| **被 @ 加权**  | 加分 | 上一轮被 `AddressedTo` 显式 @ 的 NPC → 这轮 bid 加分（"该接话了"） |
| **沉默加权**   | 加分 | 长时间没发过 public 的 NPC → 加分（防止永远沉默）                  |

最终决定谁赢得这一 tick 发言权的公式：

```
FinalScore = Urgency           // LLM 自报紧迫度
           + BidOffset         // 静态人物偏置（爱抢话 vs 内向）
           + RuntimeAdj        // 公平性调节（反霸麦/被点名/沉默）
```

最高 `FinalScore` 赢；**并列时按 Actor 字典序**（确定性，无随机）。

### 2.3 `FAILiveTickResolution` —— 一次 tick 的解算结果

```cpp
struct FAILiveTickResolution {
    int32  TickNo;
    FString WinnerActor;
    int64   WinnerIntendedSeq;     // 赢家投标对应的 speech.intended 事件 seq
    int64   DerivedPublicSeq;      // 即将写入的 speech.public 事件的 seq
    TArray<FAILiveBid> AllBids;    // 所有人的投标，写进 tick_audit
};
```

注意 `DerivedPublicSeq` 是**预计算**的，因为 `tick_resolved` 事件要在 `speech.public` 事件**之前**写入（见阶段 5 的 Stage C 时序）。

---

## 3. `AILiveAgentTypes.h` — 谁是 NPC

3 个 struct + 2 个 enum。**分层是核心**。

### 3.1 三层模型 + 一个底层 enum

```
FAILiveAgentCore       ← 跨局持久（id / 模型 / 状态 / 时间戳）
FAILiveAgentIdentity   ← 表象（名字 / 昵称 / 声音 / 外观）
FAILiveAgentBattleConfig ← 单局可变（阵营 / 角色 / 同盟 / 私货 / bAlive）
EAILiveAgentStatus     ← Core.Status：active/deleted/archived
EAILiveVoicePresentation ← Identity.VoicePresentation
```

为什么分三层？因为它们的**生命周期不同**：

| 层       | 生命周期                                                      | 谁改                      |
| -------- | ------------------------------------------------------------- | ------------------------- |
| Core     | 跨局持久（agent 注册到 `_meta.db.agent_registry` 后基本不变） | T9 Registry / Delete 流程 |
| Identity | 几乎不变（除非用户改了 BP 默认值）                            | 人工编辑                  |
| Battle   | 每局都变（投票、bAlive 翻转、同盟洗牌）                       | runtime + reducer         |

### 3.2 `Core` 字段

```cpp
FString AgentId;            // 全局唯一（如 "agent_001"）
int32   PersonaVersion;     // 人设版本号（改 prompt 时 bump）
FString ModelProvider;      // "deepseek" / "glm" / "qwen3"——和 ELLMProvider 不是直接对应
FString ModelName;          // 模型具体名（如 "deepseek-chat"）
EAILiveAgentStatus Status;  // active / deleted / archived
FString CreatedAt;          // ISO8601
FString DeletedAt;          // 当 Status=Deleted 时回填
```

**注意**：`Core.ModelProvider/ModelName` 是字符串，便于跨工程持久化；运行时另有 `FNPCAgentConfig.Provider`（`ELLMProvider` 枚举）做实际 endpoint 解析。两套字段并存——**枚举给代码用，字符串给数据库用**。

### 3.3 `Identity` 字段（PRD 不变量）

```cpp
FString FullName;
FString Nickname;
EAILiveVoicePresentation VoicePresentation;  // ⚠️ 不是性别
FString Voice;        // 具体 voice id（"presenter_male_1"）
FString Appearance;   // 外观符号（昵称、虚拟形象描述）
```

⚠️ **自检题 #4 答案**——`EAILiveVoicePresentation` 的取值：

```cpp
Masculine / Feminine / Androgynous / Synthetic / Custom
```

注释逐字写着：

> _"表征声线与形象呈现，**非人类二元生理性别**。对齐 PRD：AI 是 AI，不扮演人类。"_

这是 PRD"AI 就是 AI"原则在代码里的具体落点。CLAUDE.md 也说过：**NPC prompt / DataAsset / 任务卡严禁人类职业、教育、地域、年龄、姓名格式、家乡等背景叙事**。这条枚举就是这条原则的**类型级强制**——你不能在 schema 里塞 `Male/Female`，编译都过不去。

### 3.4 `BattleConfig` 字段

```cpp
FString Faction;                // "wolf" / "villager" / 任意自定阵营字符串
FString Role;                   // "seer" / "guard" / "civilian" / ...
TArray<FString> AllianceMembers;  // 当前同盟（reducer 维护）
FString PrivateGoal;            // 这局的私货（仅自己 prompt 看得到）
float   BidOffset;              // 静态投标偏置 → 进 FAILiveBid.BidOffset
int64   SeqStart;               // 这个 agent 注册时的 seq——查"我注册之后发生的事"用
bool    bAlive;                 // 死活——LLM 调度跳过死人
```

`bAlive` 和 `Core.Status` 区别：

- `Core.Status=Deleted` → 跨局淘汰（不再参与任何游戏）
- `Battle.bAlive=false` → 本局淘汰（下一局复活）

阶段 1 不深究，阶段 4c 讨论 Resume/Delete 时还会回来。

---

## 4. `AILiveAgentRoster.h` — 把 agent 配置和 LLM 提供商绑起来

### 4.1 `ELLMProvider` —— 3 选 1

```cpp
enum class ELLMProvider : uint8 { DeepSeek, GLM, Qwen3 };
```

注意只有 3 个！项目目前**没接** OpenAI / Claude / Gemini，只支持这 3 家国内厂商（OpenAI 兼容协议）。增加新供应商要改：

1. 此 enum
2. `AILiveAgentRoster.cpp::ResolveProviderEndpoint()`（读 `.env` 的 `<NEW>_API_KEY` 等）
3. 用枚举值 → 字符串的所有 switch（`ProviderToString`）

### 4.2 `FNPCAgentConfig` —— 运行时信封

```cpp
struct FNPCAgentConfig {
    FAILiveAgentCore           Core;       // 持久身份
    FAILiveAgentIdentity       Identity;   // 表象
    FAILiveAgentBattleConfig   Battle;     // 单局

    // ↓ 运行时绑定
    int32   NPCIndex;              // 1..10（场景里 NPC 编号）
    FName   NPCActorLabel;         // 场景里 actor label，按 label 找 actor
    FString VoicePresentationHint; // MiniMax 调用时的 hint 字符串
    ELLMProvider Provider;
};
```

它的角色是**桥**：把"agent 概念"（Core/Identity/Battle）连到"场景里的 actor 实例"（NPCIndex/NPCActorLabel）和"网络层"（Provider）。

`Act02RuleReceiveDirector` 的 `Roster` 数组就是 `TArray<FNPCAgentConfig>`，每个元素 = 一个 NPC = 一份配置 + 一个 actor 引用 + 一个 LLM 端点。

### 4.3 `namespace AILiveAgentRoster`

```cpp
TArray<FNPCAgentConfig> GetDefaultRoster();              // 内置默认 10 人花名册
FString ProviderToString(ELLMProvider);                  // 枚举 → "deepseek"/"glm"/"qwen3"

struct FProviderEndpoint { FString ApiKey, Endpoint, Model; };
FProviderEndpoint ResolveProviderEndpoint(ELLMProvider); // 读 .env 拼出 endpoint
```

`ResolveProviderEndpoint` 是阶段 4b 的重点。它从 `<ProjectDir>/.env` 读 `DEEPSEEK_API_KEY` / `DEEPSEEK_API_BASE` 等，组装成 `FProviderEndpoint`。**没设 .env 就跑不了 RunTick**。

---

## 5. 心智模型

把 4 个文件串起来：

```
        FAILiveEvent (审计原子)
             │
             ├─ EventType ┬── speech.* (4)
             │            ├── bid          ──→ FAILiveBid (T7 投标信封)
             │            ├── alliance.*
             │            ├── action.*
             │            ├── orchestrator.tick_resolved ──→ FAILiveTickResolution
             │            └── system.*
             │
             ├─ SpeechActType (7 标签)
             ├─ Visibility[] / AddressedTo[]
             └─ Phase / RoundNo

        FAILiveCommitment (T8 投影行)
             ↑ 由 commitment.* 类事件流推导

        FNPCAgentConfig (运行时信封)
             ├─ Core      (跨局)
             ├─ Identity  (表象，含 VoicePresentation——不是性别)
             ├─ Battle    (单局，含 bAlive / BidOffset)
             ├─ NPCIndex / NPCActorLabel    (绑场景 actor)
             └─ Provider  (绑 ELLMProvider → ResolveProviderEndpoint → .env)
```

---

## 6. 自检答案

**1. `Visibility=['public']` vs `['system']` 谁能在 prompt 里看到？**

- `'public'`：所有 agent 的 prompt 都能看到（在"NEAR WINDOW"段、"YOUR OWN HISTORY"段都会出现）
- `'system'`：**只有调试器/日志/审计能看到**，agent prompt 拉不到

可见性数组是**白名单**：要某个 NPC 在自己的 prompt 里看到这条事件，要么 `'public'` 在数组里，要么该 NPC 的 ID 在数组里。`'system'` 不属于任何 NPC，等于"不给任何 NPC 看"。具体过滤逻辑在阶段 4c 的 `AILiveListenerFilter`。

**2. `SpeechIntended` vs `SpeechPublic` 区别？为什么要分？**

- `Intended`：每个 NPC 这一 tick **想说但还没赢得发言权**的话；可见性默认仅自己 + system
- `Public`：经 Bid 解算赢家**实际播出**的话；可见性 `['public']`

不分会泄漏所有 NPC 的"内心剧本"，破坏社会博弈的信息不对称。**Bid 是密封投标，Intended 是封信，Public 是开标**。

**3. `RuntimeAdj` 是什么？**

抢麦最终分 `FinalScore = Urgency + BidOffset + RuntimeAdj`。`RuntimeAdj` 是 T7 引入的公平性调节，由三股力叠加：

| 力         | 方向 | 防止什么                   |
| ---------- | ---- | -------------------------- |
| 反霸麦衰减 | −    | 一人连赢，独占发言         |
| 被 @ 加权  | +    | 被点名却没分压不过别人     |
| 沉默加权   | +    | 永远抢不到发言权的内向 NPC |

**4. 为什么 `EAILiveVoicePresentation` 没有 `Male/Female`？**

`Masculine / Feminine / Androgynous / Synthetic / Custom`——表征**声线与形象**，不是人类二元生理性别。这是 PRD"AI 就是 AI"原则在 schema 层的强制：你想给 NPC 写"男性/女性"，必须翻译成"masculine/feminine 声线呈现"，避免 NPC 把自己当人类角色扮演（会触发 PRD 失败模式 3 元意识反应）。

---

## 7. 可立即上手的"修改任务"演练

读懂了这一阶段，下面这 3 个改动你应该能马上判断"要动哪些文件、有什么连带"：

| 改动                                      | 至少影响                                                                                                                            |
| ----------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------- |
| 新增 `EventType::AgentApologized`         | `EAILiveEventType` enum + `EventTypeToString/FromString`（数据库往返）+ Reducer（如果要进投影表）+ Prompt（如果要在 prompt 里展示） |
| 新增 `ELLMProvider::OpenAI`               | enum + `ProviderToString` + `ResolveProviderEndpoint`（读新的 `.env` 键）+ 默认 roster 里某个 NPC 切到这个 provider                 |
| 给 `FAILiveBid` 加 `bWasInterrupted` 字段 | struct 加字段 + 写 bid 事件的位置（阶段 5 Stage A）+ tick_audit 序列化 + RuntimeAdj 公式如果要利用它                                |

---

## 下一步

进入 **阶段 2 — 中央枢纽 API**（45 min）。只读一个文件：

```
Public/Memory/AILiveEventStoreSubsystem.h
```

按 5 组 API 分块（生命周期 / 写入 / 读+可见性 / 投影 / 完整性）。读完回来，我把阶段 2 写进 `Learn/02-eventstore-api.md`。
