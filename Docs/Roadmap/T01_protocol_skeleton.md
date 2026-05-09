# T1 · 协议骨架定稿 + BrainService 仓库初始化

> 主仓库：BrainService（新建独立 git repo） + UE 仓 `Docs/`
> 前置卡：无
> 下游消费者：T2 / T3 / T4 / T5（按 schema 派生事件 + 校验）、T6（写 UE 端 USTRUCT 镜像）、所有后续卡共享 `protocol_version` 与命名空间

---

## 1. 目标

把 UE ↔ Brain 之间所有跨进程消息的**契约**钉死、版本化、可机器校验，并初始化独立的 `BrainService/` git 仓库作为协议真相源。本卡只产出**契约文档 + JSON Schema + 样例 + 仓库骨架**，不实现任何业务逻辑代码。

---

## 2. 必读上下文锚点

按顺序读完再动手：

1. **`Docs/Roadmap/00_total_plan.md`**（重点：「边界」「阶段 0」「关键依赖图」「风险与开放决策」）—— 总路线图。
2. **`Docs/memory_principles.md`** 关键章节：
   - §〇 硬约束 1–6（特别是硬约束 5 viewer 封闭命名空间、硬约束 6 视角隔离行粒度）
   - §一 两层架构（数据层 vs 协议层职责）
   - §2.1 数据层不变量（append-only / 视角隔离 / canonical JSON 等）
   - §4.1.1 事件字段（必备 + 条件必填）
   - §4.1.2 事件类型枚举全表
   - §4.1.5 默认事件可见性模板
   - §5.1 Reasoner 四通道 schema
   - §5.4.1 intent ontology 默认词汇表（注意 ontology v1 只取子集）
   - §7.3 Delete 协议（用于 actor_id 永久不可复用规则）
3. **`Docs/PRD.md`**（重点：「项目核心」「AI 心智决策系统」「让博弈对 AI 自己而言"重要"」三节）—— 理解 actor_id / Delete / 阵营定义。
4. **`Docs/Roadmap/handbook.md`** §0 工作流总览、§4 已收敛项目级决策。
5. **`CLAUDE.md`**（项目根）—— 关键规则与边界。

不需要读 DevLog（T1 不动 UE 资产）。

---

## 3. 范围内（DO）

### 3.1 BrainService 仓库初始化

- 在 `D:\Project\Unreal\AILiveProject\BrainService\` 下 `git init` 一个**独立** git repo（与 UE 工程的 git 历史完全独立）。
- UE 工程根 `.gitignore` 已包含 `BrainService/`（已核对，第 99 行）—— 无需改 UE 仓的 .gitignore。
- 在 `BrainService/.gitignore` 加 Python 常规忽略项（`__pycache__/`、`*.pyc`、`.venv/`、`.env` 等）。
- 在 `BrainService/README.md` 写仓库简介：用途、与 UE 工程的关系、protocol/ 目录是真相源、`protocol_version` 语义化版本规则。
- **不**配置 Python venv / 不装依赖 / 不写任何 .py 业务代码（属 T2 范畴）。
- 用合适的 commit message 提交首个 commit（含 README + .gitignore + protocol/ 目录全部产出物）。

### 3.2 `BrainService/protocol/protocol.md`（人读契约）

至少包含以下章节：

- **协议版本**：`protocol_version = "0.1.0"`，用 semver；变更规则（major = breaking、minor = 新增 endpoint 或字段、patch = 文档/澄清）。
- **传输形态**：HTTP/1.1 + JSON 文本 body；UE 端用 `FHttpModule` polling；Brain 端实现可选（FastAPI / aiohttp / starlette 都行）。auth 通过 `Authorization: Bearer <token>` 头；token 由 UE 端 `UAILiveProjectSettings` 提供。
- **命名空间**：
  - `game_id`：UUID v4 字符串，每局唯一。由 brain 在 game session 创建时生成并下发给 UE。
  - `seq`：int，一局内全局唯一严格递增（由 brain orchestrator 单进程发号）。
  - `actor_id`：固定枚举 `NPC01` .. `NPC10`，与 UE 关卡内 `BP_NPC_MH_Character_1..10` 1:1 映射。
  - `viewer` 封闭集合：`public` / `audience` / `orchestrator` / `system` / `NPC01..NPC10` / `Faction<X>`（`<X>` 是后续游戏才用，T1 仅留位）。**`self` 严禁入库**。
  - `protocol_version`：版本声明只放在三处——URL 前缀（`/v1/`）、`GET /health` 响应、`POST /v1/games` 响应。**消息 body 内不重复携带 `protocol_version` const 字段**（避免 schema 冗余 + UE round-trip diff 噪声）。UE 启动时调 `GET /health` 与 `POST /v1/games`，校验返回的 `protocol_version` 与 `Docs/protocol_pointer.md` 一致；不一致拒绝运行。
- **Session 生命周期与崩溃语义**：
  - MVP 阶段**不支持** game session 跨 PIE 进程恢复。UE 进程崩溃 / 关闭 PIE 后再启动视为新一局，必须重新调 `POST /v1/games` 取新 `game_id`；旧 game_id 对应的事件流在 brain 端保留作审计。
  - UE 端 `last_processed_seq` 高水位线**不持久化**（仅存内存），随 game session 销毁；新一局从 0 起步。
  - `Idempotency-Key` 存活范围 = 本 game session（brain 端按 `game_id` 隔离 key 表）；session 结束或 brain 进程重启即清空，不跨 game 复用。
- **投递语义（cursor-based polling + 幂等去重）**：
  - 所有 pull endpoint 用 `?since_seq=N` 游标参数；brain 返回所有 `seq > N` 的事件 + `next_cursor`（最大 seq）。
  - UE 端**必须**维护 per-endpoint 的 `last_processed_seq` 高水位线（持久化在 game session 内存中），下次 pull 用它作为 `since_seq`。
  - 同一 `seq` 不得在 UE 端被处理两次（即使 brain 因网络重传同一条事件）—— UE 内部按 seq 幂等去重。
  - brain 端不维护 in-flight / lease 状态（pull endpoint 是无状态的查询）；交付保证由"UE seq 高水位线 + brain append-only 事件流"双向保证。
  - **所有 POST 写入型 endpoint（除 `POST /v1/games` session 创建本身）必须携带 `Idempotency-Key: <uuid v4>` 头**——这是硬要求，不是建议。覆盖范围：`roster`、`world_state`、`actions/result`、`speech/result`、`ingress_reject`。brain 端按 key 去重，同 key 重复请求返回首次结果（含首次的 HTTP status / body）。`world_state_push` 的 request body 还**额外**携带 `client_sample_id`（UE 单调递增 int）字段作为业务级幂等键，方便 brain 在去重命中后判断是否需要重新拆条入库。
- **endpoint 表**：以下 10 个最少 endpoint。每个含 method / 路径 / Required Headers / 请求 schema / 响应 schema / 失败码。**Headers 全局规则**（不在每行重复）：
  - `Authorization: Bearer <token>` —— **所有 endpoint 必带，`GET /health` 除外**。
  - `Idempotency-Key: <uuid v4>` —— **所有 POST 写入型 endpoint 必带，`POST /v1/games` 除外**（session 创建本身不幂等，每次必新建）。覆盖范围：`roster` / `world_state` / `actions/result` / `speech/result` / `ingress_reject`。
  - `Content-Type: application/json` —— 所有带 body 的请求必带。

  | endpoint                                  | Required Headers                             |
  | ----------------------------------------- | -------------------------------------------- |
  | `GET  /health`                            | （无）                                       |
  | `POST /v1/games`                          | Authorization, Content-Type                  |
  | `POST /v1/games/{game_id}/roster`         | Authorization, Idempotency-Key, Content-Type |
  | `POST /v1/games/{game_id}/world_state`    | Authorization, Idempotency-Key, Content-Type |
  | `GET  /v1/games/{game_id}/actions/pull`   | Authorization                                |
  | `POST /v1/games/{game_id}/actions/result` | Authorization, Idempotency-Key, Content-Type |
  | `GET  /v1/games/{game_id}/speech/pull`    | Authorization                                |
  | `POST /v1/games/{game_id}/speech/result`  | Authorization, Idempotency-Key, Content-Type |
  | `POST /v1/games/{game_id}/ingress_reject` | Authorization, Idempotency-Key, Content-Type |
  | `GET  /v1/games/{game_id}/events`         | Authorization                                |

  各 endpoint 语义：
  - `GET  /health` —— 健康检查（brain 端进程是否在线，响应含当前 `protocol_version`）。
  - `POST /v1/games` —— **session 握手**：UE 启动 PIE 时调一次，brain 创建新 game session、生成 `game_id`、返回 `{ game_id, protocol_version, server_time }`。**这是 UE 拿到合法 game_id 的唯一入口**，没有它后续 endpoint 都打不开。
  - `POST /v1/games/{game_id}/roster` —— UE 注册当局 roster（actor_id → metadata，含 Pawn 显示名、初始位置、阵营占位）。`game_id` 必须由 `POST /v1/games` 返回。
  - `POST /v1/games/{game_id}/world_state` —— UE 周期推送世界状态。**这是 transport 层，不是 EventStore 行**：单次请求 body 含全部在场 NPC 的 per-actor 观测（sight/hearing/位置/朝向/当前动作/inventory 摘要）；brain 落库时必须按 observer 拆成独立事件行（每个 actor 的每条 sight / hearing 一行），**不得整包持久化**（memory_principles §硬约束 6 视角隔离行粒度）。**拆条后落成的具体 event_type（如 `world.perception.sight` / `world.perception.hearing` 等）由 T2 数据层卡片冻结**——T1 不预先约束 event_type 命名，仅约束"必须按 observer 拆条 + visibility 行粒度过滤"的拆分语义。
  - `GET  /v1/games/{game_id}/actions/pull?since_seq=N` —— UE 拉取 `seq > N` 的待执行 `action.intent` 列表（仅 ontology v1：move_to / sit / wait）。响应含 `next_cursor`。
  - `POST /v1/games/{game_id}/actions/result` —— UE 上报 action **自然完成结果**（仅 `outcome ∈ {succeeded, failed}`），brain 写入 `action.resolved`。**`action.cancelled` 与 `action.resolved` 是互斥终态**（memory_principles §5.4.2）：当旧 action 被新 intent 覆盖时，brain 在派生新 intent 的同一事务内已写好 `action.cancelled`，UE 仅负责物理停止旧动作，**绝不上报旧 intent 的 result**（不存在 `outcome=interrupted` 这种状态）。每个 in-progress action 一生只有一条终态事件——cancelled 或 resolved 二选一。**仅承载 ontology v1 物理动作，不含 speak**。
  - `GET  /v1/games/{game_id}/speech/pull?since_seq=N` —— UE 拉取 `seq > N` 的待播报 `speech.public` 列表。**与 action 通道完全平行的独立路由**。
  - `POST /v1/games/{game_id}/speech/result` —— UE 上报 speech 播放完成（成功 / 失败 / 时长 / 失败原因），brain 写入新事件类型 `speech.playback_resolved`。**不写入 `action.resolved`**——speech.public 没有对应的 `action.intent`，没有 action 因果链。
  - `POST /v1/games/{game_id}/ingress_reject` —— UE IngressValidator 拒绝时上报，brain 写入 `system.ingress_rejected`（**本卡冻结此事件类型**）。
  - `GET  /v1/games/{game_id}/events?since_seq=N` —— 事件查询（UE 调试 / 审计查看时用）。

- **action ontology v1 冻结**：`move_to / sit / wait` 三个 intent + 各自参数 schema。**`speak` 不进 ontology**——明确写在文档里"speech.public 走独立 endpoint，不属于 action.intent"。
- **T1 新增事件类型的默认 visibility**（与 memory_principles §4.1.5 表对齐，沿用系统类事件惯例）：
  - `speech.playback_resolved`：默认 `["orchestrator", "system"]`。携带成功/失败/时长/失败原因等调试信息，**不得**默认对参赛 agent 可见，避免泄露执行内幕。需要观众调试视图时另派生 audience 可见的衍生事件。
  - `system.ingress_rejected`：默认 `["orchestrator", "system"]`。携带拒绝理由 + 涉事 actor_id + 原始消息摘要，可能间接暴露未抢中 floor 的 intended 内容，必须严格内部可见。是否对 audience 可见由具体游戏规则决定，**不**在 T1 默认开放。
  - 写明：T1 不为这两类事件开放 `["public"]` 默认；任何要把它们公开的需求必须在 T9 或后续游戏卡里显式声明并加 audit 事件配套（参考 §4.1.5 `tick_resolved` / `tick_resolved.audit` 拆条模式）。
- **`speech.public` 与 `action.intent` 通道分离的硬边界声明**：schema 层、endpoint 层、UE Dispatcher 层全程分离。
- **失败码表**：HTTP status + 自定义 `error_code` 字符串（如 `INVALID_ACTOR_ID`、`STALE_SEQ`、`ROSTER_NOT_REGISTERED` 等），每个含触发条件与 UE 端期望行为。
- **顺序与超时约定**：UE polling 间隔（建议 200ms 起，可配）、HTTP 超时、重试策略（指数退避）。

### 3.3 `BrainService/protocol/schemas/*.schema.json`（机器校验 schema）

每条消息一份 JSON Schema（draft 2020-12），文件名一一对应（共 12 份）：

- `common.schema.json`（提取公共类型：`actor_id`、`viewer`、`visibility`、`game_id`、`seq`、`protocol_version`、`event_meta`、`since_seq_query` 公共参数）
- `error_response.schema.json`（统一失败响应：`error_code` / `message` / 可选 `details` / `retryable`）
- `health.schema.json`
- `session_create.schema.json`（`POST /v1/games` 请求 + 响应；含 brain 返回的 `game_id` / `protocol_version` / `server_time`）
- `roster_register.schema.json`（请求 + 响应）
- `world_state_push.schema.json`（per-actor 观测数组；transport 形态，**brain 落库时必须拆条**，T2 的 EventStore 实施需遵守此约束；request body 强制含 `client_sample_id` int 字段做业务级幂等）
- `action_pull.schema.json`（pull 响应含 `events[]` + `next_cursor`）
- `action_result.schema.json`（仅承载 ontology v1 物理动作的**自然完成**：`outcome ∈ {succeeded, failed}`；**不含 speak、不含 interrupted**——cancel 由 brain 同事务派生）
- `speech_pull.schema.json`（pull 响应含 `events[]` + `next_cursor`）
- `speech_result.schema.json`（**新增**：speech 播放结果，落库为 `speech.playback_resolved`，与 `action.resolved` 完全分离）
- `ingress_reject.schema.json`
- `event_query.schema.json`

约束：

- `additionalProperties: false`（所有 object 严格无多余字段）。
- `actor_id` 用 enum，把 `NPC01..NPC10` 全列出。
- `viewer` 类型在 `common.schema.json` 用 `anyOf` 表达封闭集合 + Faction pattern：
  ```json
  {
    "anyOf": [
      {
        "enum": [
          "public",
          "audience",
          "orchestrator",
          "system",
          "NPC01",
          "NPC02",
          "NPC03",
          "NPC04",
          "NPC05",
          "NPC06",
          "NPC07",
          "NPC08",
          "NPC09",
          "NPC10"
        ]
      },
      { "type": "string", "pattern": "^Faction[A-Za-z0-9_]+$" }
    ]
  }
  ```
  `self` 不出现在 enum、不被 pattern 匹配。**不要写成 `enum + pattern` 同级**——JSON Schema 里这两者是互斥的，必须用 `anyOf` 包裹。
- 所有 schema 顶部声明 `$schema` + 引用 `common.schema.json` 的复用类型。
- **消息 body 内不携带 `protocol_version` 字段**（B8）：版本声明只放在 `GET /health` 与 `POST /v1/games` 响应（`session_create.schema.json` / `health.schema.json` 中作为响应字段 const 锁定到 `"0.1.0"`）。除这两处响应外，其余请求 / 响应 schema 均不含此字段；后续升版本时只改这两处响应 schema + URL 前缀（如 `/v2/`），不必逐个 schema 改。

### 3.4 `BrainService/protocol/examples/*.json`（round-trip fixture）

每个 endpoint 至少 2 个样例：1 个 happy path、1 个失败 case（含 `error_code`）。成功样例按对应 endpoint schema 校验；失败样例统一按 `error_response.schema.json` 校验。若某 endpoint 响应 schema 同时覆盖成功 / 失败响应，必须用 `oneOf` 引用成功响应与 `error_response.schema.json`，不得私自重复定义 error 结构。命名规范：`<schema_name>.<case>.json`，如 `roster_register.success.json` / `roster_register.invalid_actor.json`。

要求每个样例都通过对应 schema 的校验（用任一 JSON Schema validator 跑一遍）。

### 3.5 UE 仓 `Docs/protocol_pointer.md`

唯一的、单文件的"指针"。内容：

- BrainService repo 路径（绝对路径或描述）。
- 当前引用的 BrainService commit hash（首个 commit）。
- 当前 `protocol_version`。
- 升级流程（先 BrainService 改 → bump `protocol_version` → UE 仓改这里 hash → 跑 UE round-trip 测试）。
- UE C++ round-trip 测试的位置占位（实际测试由 T6 实现）。

---

## 4. 范围外（DON'T）

- ❌ 不实现 EventStore / 哈希链 / 投影 / 召回工具（属 T2）。
- ❌ 不实现 Reasoner / Validator / 任何 LLM 调用（属 T3）。
- ❌ 不实现 Bid / Listener-as-filter / 反思（属 T4 / T5）。
- ❌ 不写 UE C++ USTRUCT / `UAILiveProjectSettings` / HTTP polling 客户端（属 T6）。
- ❌ 不实现 brain HTTP server 路由代码（属 T2 后期或独立子任务）。
- ❌ 不动任何 UE 资产 / Source/AILiveProject/\* 代码（除了创建 `Docs/protocol_pointer.md`）。
- ❌ 不写 ontology v2（pickup / use_item / inspect / follow / flee_from）—— 属 T8。
- ❌ 不在 `protocol.md` 写病毒游戏专属规则（属 T9）。
- ❌ 不配置 Python venv / `requirements.txt` / `pyproject.toml`（留给 T2 起步时按需选）。

---

## 5. 交付清单

| 路径                                                         | 类型          | 一句话职责                                                                                                             |
| ------------------------------------------------------------ | ------------- | ---------------------------------------------------------------------------------------------------------------------- |
| `BrainService/.git/`                                         | git repo init | 独立 git 历史                                                                                                          |
| `BrainService/.gitignore`                                    | 新建          | Python 常规忽略                                                                                                        |
| `BrainService/README.md`                                     | 新建          | 仓库简介 + protocol/ 真相源说明 + protocol_version 规则                                                                |
| `BrainService/protocol/protocol.md`                          | 新建          | 人读协议契约（endpoint + 命名空间 + 失败码 + 通道分离声明）                                                            |
| `BrainService/protocol/schemas/common.schema.json`           | 新建          | 公共类型抽取                                                                                                           |
| `BrainService/protocol/schemas/error_response.schema.json`   | 新建          | 统一失败响应结构，供失败样例和 endpoint error 响应复用                                                                 |
| `BrainService/protocol/schemas/health.schema.json`           | 新建          | 健康检查（响应含 protocol_version）                                                                                    |
| `BrainService/protocol/schemas/session_create.schema.json`   | 新建          | session 握手（`POST /v1/games`）—— UE 拿 game_id 的唯一入口                                                            |
| `BrainService/protocol/schemas/roster_register.schema.json`  | 新建          | roster 注册请求/响应                                                                                                   |
| `BrainService/protocol/schemas/world_state_push.schema.json` | 新建          | UE → brain 状态推送 transport（落库由 brain 拆条）                                                                     |
| `BrainService/protocol/schemas/action_pull.schema.json`      | 新建          | brain → UE 动作意图拉取（cursor + next_cursor）                                                                        |
| `BrainService/protocol/schemas/action_result.schema.json`    | 新建          | UE → brain 动作执行结果（成功/失败；**不含 interrupted / cancelled 语义**——cancelled 由 brain 派生新 intent 同事务写） |
| `BrainService/protocol/schemas/speech_pull.schema.json`      | 新建          | brain → UE 公开发言拉取（cursor + next_cursor）                                                                        |
| `BrainService/protocol/schemas/speech_result.schema.json`    | 新建          | UE → brain speech 播放结果（落库为 speech.playback_resolved）                                                          |
| `BrainService/protocol/schemas/ingress_reject.schema.json`   | 新建          | UE → brain 白名单拒绝上报（落库为 system.ingress_rejected）                                                            |
| `BrainService/protocol/schemas/event_query.schema.json`      | 新建          | 事件查询                                                                                                               |
| `BrainService/protocol/examples/*.json`                      | 新建若干      | 每个 schema 至少 2 个样例（happy + failure）                                                                           |
| `Docs/protocol_pointer.md`                                   | 新建          | UE 仓指针文件，记 BrainService commit hash + protocol_version                                                          |

---

## 6. 验收（机器化优先）

完成时按顺序自检并给出勾选状态：

- [ ] `cd BrainService && git log` 至少 1 个 commit，工作目录 clean。
- [ ] `BrainService/protocol/protocol.md` 包含 §3.2 列出的全部章节，`protocol_version = "0.1.0"`。
- [ ] `protocol.md` 明确声明：(a) `speech.public` 与 `action.intent` 通道分离的硬边界；(b) `speak` 不属于 ontology v1；(c) `self` 严禁入库；(d) cursor-based polling + UE seq 幂等去重的投递语义；(e) speech 完成走 `speech.playback_resolved` 而非 `action.resolved`；(f) `world_state_push` 是 transport，brain 落库时必须按 observer 拆条，**拆条后的具体 event_type 命名由 T2 冻结，T1 仅约束拆分语义**；(g) `action.cancelled` 与 `action.resolved` 互斥终态——UE 不上报 interrupted；(h) 所有 POST 写入型 endpoint（除 `POST /v1/games` 本身）必须 `Idempotency-Key` 头；`world_state_push` 还有 `client_sample_id` 业务级幂等键；(i) `speech.playback_resolved` 与 `system.ingress_rejected` 默认 visibility = `["orchestrator", "system"]`，不得默认 public；(j) `protocol_version` 仅在 URL 前缀 / `GET /health` 响应 / `POST /v1/games` 响应三处声明，body 不携带；(k) MVP 不支持 game session 跨 PIE 进程恢复，UE 进程崩溃 = 新一局，`Idempotency-Key` 存活范围限本 game session。
- [ ] endpoint 表共 10 个（含 `POST /v1/games` session 握手 + 独立的 `POST .../speech/result`）；表格含 "Required Headers" 列；所有 pull endpoint 接受 `?since_seq=N` 并在响应里返回 `next_cursor`；所有 POST 写入型 endpoint（除 `POST /v1/games`）在 Required Headers 列显式列出 `Idempotency-Key`。
- [ ] `BrainService/protocol/schemas/` 下 12 份 schema 文件全部存在，每份 `additionalProperties: false`，顶部 `$schema` 声明 draft 2020-12。**业务 schema body 内不含 `protocol_version` 字段**（仅 `health.schema.json` / `session_create.schema.json` 响应中声明）。
- [ ] 用任一 JSON Schema validator（如 `jsonschema` Python 库或 `ajv`）逐个校验 `examples/*.json`：成功样例通过对应 endpoint schema，失败样例通过 `error_response.schema.json` 或 endpoint 响应 schema 中引用它的 `oneOf` 分支。建议在 README 写 1 行命令复现校验。
- [ ] `actor_id` enum 在 `common.schema.json` 写死 `NPC01..NPC10`；`viewer` 类型用 `anyOf`（enum + Faction pattern），**不**包含 `self`。
- [ ] `Docs/protocol_pointer.md` 写下 BrainService 首个 commit 的 hash + `protocol_version = "0.1.0"`。
- [ ] UE 工程根 `.gitignore` 第 99 行已存在 `BrainService/`（已核对，无需改动；本卡只需确认）。
- [ ] 由人类（不是 ClaudeCode）评审 `protocol.md` 后通过。

---

## 7. 上下游交接

**前置卡**：无（T1 是所有人开工前的硬门槛）。

**下游消费者**：

| 后续卡                     | 依赖 T1 的什么                                                                                         |
| -------------------------- | ------------------------------------------------------------------------------------------------------ |
| T2 (Brain 数据层)          | `common.schema.json` 的 viewer / actor_id / event_meta 类型；`protocol.md` 的 endpoint 与失败码        |
| T3 (Reasoner + Validator)  | `protocol.md` 的命名空间约束 + `action_pull.schema.json` 的 ontology v1 限制                           |
| T4 / T5 (Brain 协议层余下) | 同 T3                                                                                                  |
| T6 (UE 协议基础设施)       | 全部 schemas + examples（写 UE USTRUCT 镜像 + round-trip 测试）；`Docs/protocol_pointer.md` 是单一指针 |
| T7 (UE Dispatcher)         | `action_pull.schema.json` / `speech_pull.schema.json` 的硬边界                                         |
| T8 (物品 / Delete)         | T8 启动时升 `protocol_version → 0.2.0` 引入 ontology v2，但 T1 不写 v2 内容                            |
| T9 / T10 (病毒游戏)        | T9 启动时按需在 `protocol.md` 加病毒游戏专属附录或独立 game schema 文件                                |

---

## 8. 风险与已知坑

- **协议改动只能在 BrainService 提**：UE 仓的 `Docs/protocol_pointer.md` 只能跟随 BrainService 的版本。任何在 UE 端"先改 USTRUCT 再去同步 schema"的做法都会破坏单一真相源。
- **`protocol_version` 必须 semver**：T8 / T9 阶段升 ontology / 加 game 专属字段时，按 minor 升；breaking 改才升 major。`protocol.md` 顶部要写"如何升版本"。
- **`actor_id` 永久不可复用**：根据 memory_principles §7.3，被 Delete 的 actor_id 在跨局也不能复用。`protocol.md` 应在 actor_id 章节明确这点（即使 T1 不实现 lifecycle store，也要写下契约）。
- **`viewer` 不允许 `self`**：硬约束 5。所有 schema 不得让 `self` 字符串通过。
- **HTTP polling 间隔不写死**：`protocol.md` 给建议值 200ms，但说明"实际值由 UE 端 `UAILiveProjectSettings` 配置"。
- **健康检查 endpoint 与协议版本握手**：`GET /health` 与 `POST /v1/games` 响应里都含 `protocol_version`，UE 启动时检查与本地 `Docs/protocol_pointer.md` 一致；不一致拒绝运行（避免运行时漂移）。**消息 body 不重复携带版本字段**（B8 决议）：避免 schema 噪声 + UE round-trip diff 干扰。
- **`Idempotency-Key` 跨进程语义**：MVP 阶段 key 只存活在 brain 的 game session 内存中（按 game_id 隔离）；UE 进程崩溃即新一局，旧 key 永远不会被重发。不要尝试把 key 持久化跨 session，它的语义就是"本次 session 内 POST 的去重"。
- **不要在 schemas 里嵌入 enum 字符串重复定义**：用 `common.schema.json` `$ref`，避免改一处忘改另一处。
- **examples 的 JSON 必须是 deterministic**：字段按 ASCII 升序排序（与 memory_principles canonical JSON 一致），便于 UE 端 round-trip 测试比对字符串。
- **Brain repo 嵌套在 UE 工程根但 .gitignore 已忽略**：千万不要在 BrainService/ 内执行 UE 工程的 git 命令，反之亦然。每次操作前确认 `pwd`。

---

## 9. 启动 prompt（粘贴到新 ClaudeCode 窗口的开场）

```text
你是 AILive 项目 Memory Principles 路线图的 T1 子任务执行者：协议骨架定稿 + BrainService 仓库初始化。

**必读上下文（按顺序读完再动手）**：
1. Docs/Roadmap/00_total_plan.md ——总路线图（重点："边界" / "阶段 0" / "关键依赖图" / "风险与开放决策"）
2. Docs/Roadmap/handbook.md §0 工作流总览 + §4 已收敛项目级决策
3. Docs/memory_principles.md 关键章节：§〇 硬约束 1–6、§一 两层架构、§2.1 数据层不变量、§4.1.1–§4.1.2、§4.1.5、§5.1、§5.4.1、§7.3
4. Docs/PRD.md：「项目核心」「AI 心智决策系统」「让博弈对 AI 自己而言"重要"」三节
5. CLAUDE.md（项目根）

**任务范围（DO）**：
- 在 D:\Project\Unreal\AILiveProject\BrainService\ 下初始化独立 git repo（与 UE 工程历史完全独立）。
- 写齐 protocol/protocol.md（人读契约，含 protocol_version=0.1.0、**10 个 endpoint**（`GET /health`、`POST /v1/games`、`POST .../roster`、`POST .../world_state`、`GET .../actions/pull`、`POST .../actions/result`、`GET .../speech/pull`、`POST .../speech/result`、`POST .../ingress_reject`、`GET .../events`）、命名空间、ontology v1 仅 3 个 intent（move_to / sit / wait）、speech/action 通道分离硬边界、失败码表、cursor-based polling 投递语义（`?since_seq=N` + UE seq 幂等去重 + Idempotency-Key 头））。
- 写齐 protocol/schemas/*.schema.json（**12 份** JSON Schema draft 2020-12：common / error_response / health / session_create / roster_register / world_state_push / action_pull / action_result / speech_pull / speech_result / ingress_reject / event_query；all additionalProperties=false，actor_id 用 enum 列出 NPC01..NPC10，viewer 用 `anyOf: [{enum:[...]}, {pattern:"^Faction[A-Za-z0-9_]+$"}]` 表达封闭集合 + Faction 兼容，`self` 严禁出现；**业务 schema body 不含 `protocol_version` 字段**——仅 health / session_create 响应中带）。
- 写齐 protocol/examples/*.json（每个 endpoint ≥ 2 个样例，happy + failure；成功样例按对应 endpoint schema 校验，失败样例按 `error_response.schema.json` 或 endpoint `oneOf` error 分支校验）。
- 在 protocol.md 中明确以下硬边界：
  - `speech.public` 与 `action.intent` 在 schema/endpoint/UE Dispatcher 三层全程分离。
  - speech 完成走新事件类型 `speech.playback_resolved`，**不复用** `action.resolved`。
  - `action.cancelled` 与 `action.resolved` 是**互斥终态**（memory_principles §5.4.2）：被新 intent 覆盖的旧 action 由 brain 在派生新 intent 同事务写 cancelled，UE 不上报旧 intent 的 result；自然完成的 action 由 UE 上报 resolved（`outcome ∈ {succeeded, failed}`，**不存在 interrupted**）。
  - `world_state_push` 是 transport 形态，brain 落库时必须按 observer 拆条（硬约束 6 视角隔离行粒度）；request body 含 `client_sample_id` 业务级幂等键。
  - **所有 POST 写入型 endpoint（除 `POST /v1/games` 本身）必须携带 `Idempotency-Key: <uuid v4>` 头**——硬要求。
  - `system.ingress_rejected` 是 T1 冻结的新事件类型，与 `system.validation_failed` 完全独立。
  - `speech.playback_resolved` 与 `system.ingress_rejected` 默认 visibility = `["orchestrator", "system"]`，不得默认 public。
  - `game_id` 唯一入口是 `POST /v1/games`，UE settings 不存 game_id。
  - **MVP 不支持 game session 跨 PIE 进程恢复**：UE 进程崩溃 = 新一局，重新调 `POST /v1/games`；`Idempotency-Key` 存活范围限本 game session（brain 端按 game_id 隔离）。
  - **`protocol_version` 仅在三处声明**：URL 前缀（`/v1/`）+ `GET /health` 响应 + `POST /v1/games` 响应；消息 body 不重复带 const 字段。
  - **`world_state_push` 拆条**：T1 仅约束"必须按 observer 拆条 + visibility 行粒度过滤"的拆分语义，**拆条后落成的具体 event_type 命名（如 `world.perception.sight` 等）由 T2 数据层卡片冻结**，T1 不预先约束。
- 在 UE 仓 Docs/protocol_pointer.md 写下 BrainService 首个 commit hash + protocol_version。
- 提交 BrainService 首个 commit。

**范围外（DON'T）**：
- 不实现 EventStore / Reasoner / Validator / 任何 LLM 调用 / brain HTTP server 路由代码（属 T2 / T3）。
- 不写 UE C++ USTRUCT / Settings / HTTP 客户端（属 T6）。
- 不动 UE 资产 / Source/AILiveProject/* 任何代码。
- 不引入 ontology v2（pickup / use_item / inspect / follow / flee_from）—— 属 T8。
- 不写 02 病毒游戏专属规则—— 属 T9。
- 不配置 Python venv / 装依赖。

**约束**：
- 严格遵守 CLAUDE.md（项目级 + 用户级两份）：暴露假设、surgical changes、goal-driven。
- BrainService 是嵌套但独立的 git repo——操作前先确认 pwd。
- 所有 schema 字段按 ASCII 升序（memory_principles canonical JSON）。
- 完成时按 T01 §6 验收清单逐条勾选。

**第一步**：先读完上述全部必读文件，然后用 1–3 句话给我复述：
(a) 你理解的本卡范围与边界；
(b) `speech.public` 与 `action.intent` 为什么必须通道分离（硬边界来自 memory_principles 哪一节）；
(c) ontology v1 为什么只有 3 个 intent；
(d) `action.cancelled` 与 `action.resolved` 为什么是互斥终态、UE 为什么不上报 interrupted；
(e) 为什么 `Idempotency-Key` 是硬要求（不是建议），以及 `world_state_push` 为什么还要额外加 `client_sample_id`；
(f) `protocol_version` 为什么不放在每条消息 body 里、放在哪三处；
(g) `world_state_push` 落库 event_type 命名为什么 T1 不冻结、留给谁。
等我确认后再动手。
```

---

## 10. 可选前置准备（如果新窗口提前问）

- BrainService Python 版本：暂不锁定，T2 启动时再决定（推荐 3.11+）。
- JSON Schema validator 工具：T1 仅自检，可用 `python -m jsonschema` 或 `npx ajv-cli`。
- 文档语言：中文为主，schema 字段名英文。
- 提交人 git config：用现有 UE 工程的 git config 还是 BrainService 单独配？建议单独配（独立项目独立 author 元数据，便于追溯）。
