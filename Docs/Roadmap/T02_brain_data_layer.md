# T2 · Brain 数据层骨架（EventStore + Schema + 视角隔离 + 投影 + 召回 + `_meta.db`）

> 主仓库：BrainService（已在 T1 初始化）
> 前置卡：T1 ✅（协议骨架 + schema/examples + commit `76f3dd5`）
> 下游消费者：T3（Reasoner 派生事件需走 EventStore）、T4 / T5（投影 + 召回输入）、T6（UE 端事件查询路径需要的 query_events / quote 接口）、T8（Delete 协议挂钩）

---

## 1. 目标

建立 BrainService **数据层全部基础设施**，作为协议层与 UE 接入卡共享的"真相源"。本卡只产出 Python 库（不含 HTTP server、不含 Reasoner / Validator / 任何 LLM 调用），实现 memory_principles §四 数据层契约：append-only EventStore + 哈希链 + 视角隔离 + 投影 + 召回工具 + 跨局 `_meta.db` 最小骨架。

实施层选型：**SQLite + triggers**（理由：append-only trigger 易实现、单文件、零运行依赖、写入串行化语义清晰）。runtime 依赖只锁定 stdlib + `jsonschema`；test 依赖 `pytest`。

---

## 2. 必读上下文锚点

按顺序读完再动手：

1. **`Docs/Roadmap/00_total_plan.md`**（重点：「边界」「阶段 0 / 1」「关键依赖图」「需要保留的 UE 资产」与「风险与开放决策」）。
2. **`Docs/Roadmap/handbook.md`** §0 工作流总览 + §4 已收敛项目级决策。
3. **`Docs/memory_principles.md`** 关键章节：
   - §〇 硬约束 1–6（特别是硬约束 5 viewer 封闭集合、硬约束 6 视角隔离行粒度）
   - §一 两层架构（数据层 vs 协议层职责）
   - §2.1 数据层不变量（append-only / canonical JSON / 串行化等 7 条）
   - §三 数据层反模式（避免向量主路径 / fact triple / payload @hidden 等）
   - §4.1.1 事件字段（必备 + 条件必填）
   - §4.1.2 事件类型枚举全表
   - §4.1.3 事件写入约束
   - §4.1.4 `speech_act_type` 标注归属
   - §4.1.5 默认事件可见性模板
   - §4.2 投影语义 + 投影列表
   - §4.4 召回工具契约
   - §6.1 EventStore 写入与哈希链
   - §6.4 commitments 投影：防赖账（三源覆盖）
   - §7.3 Delete 协议（用于 `_meta.db` 设计）
4. **`Docs/Roadmap/T01_protocol_skeleton.md`** §3.2 / §3.3 / §3.4 —— 已冻结的 endpoint 表 + schema 名册 + 命名空间。
5. **BrainService 真相源**（已生成）：
   - `BrainService/protocol/protocol.md`（人读契约）
   - `BrainService/protocol/schemas/*.schema.json`（12 份）
   - `BrainService/protocol/examples/*.json`（fixture）
   - `BrainService/protocol/validate.py`（schema 校验脚本）
6. **`Docs/PRD.md`**：「项目核心」「AI 心智决策系统」「让博弈对 AI 自己而言"重要"」三节（理解 actor_id / Delete 设计意图）。
7. **`CLAUDE.md`**（项目根） + 用户级 `~/.claude/CLAUDE.md`。

不需要读 DevLog（T2 不动 UE 资产）。

---

## 3. 范围内（DO）

### 3.1 EventStore 基础设施（持久化 + 哈希链 + 串行化）

- **DB 引擎**：SQLite（单文件，建议 `data/eventstore.sqlite3`，运行时由 BrainService 启动初始化；db 文件路径可配）。
- **events 表 schema**：列与 §4.1.1 必备 + §4.1.1.2 条件必填字段 1:1 对应：
  - `game_id TEXT NOT NULL`
  - `seq INTEGER NOT NULL`（一局内单调；多局通过 `game_id` 隔离；由应用层发号）
  - `round_no INTEGER NOT NULL`
  - `phase TEXT NOT NULL`
  - `actor TEXT NOT NULL`（可为 `orchestrator` / `system` / `<actor_id>` / 执行系统标识）
  - `event_type TEXT NOT NULL`
  - `visibility TEXT NOT NULL`（JSON array）
  - `addressed_to TEXT NOT NULL`（JSON array，无 @ 时为 `[]`）
  - `payload TEXT NOT NULL`（canonical JSON 文本）
  - `parent_event_id INTEGER`（可空；引用同一 `game_id` 内的 `events.seq`）
  - `prev_hash TEXT NOT NULL`
  - `event_hash TEXT NOT NULL`
  - `wall_clock_ts TEXT NOT NULL`（ISO 8601 UTC；信息性，**不**参与排序）
  - `output_contract_version TEXT`（条件必填）
  - `validator_version TEXT`（条件必填）
  - `raw_llm_output TEXT`（条件必填，见 §4.1.1.2）
  - `source_contract_version TEXT`
  - `source_event_id INTEGER`（可空；引用同一 `game_id` 内的源事件 `seq`）
  - `PRIMARY KEY (game_id, seq)`
- **DB 层禁止 UPDATE / DELETE**：建立 trigger `events_no_update` / `events_no_delete`（`BEFORE UPDATE`/`BEFORE DELETE` → `RAISE(ABORT, ...)`）。schema 演进只能 ALTER TABLE ADD COLUMN，不得改既有列语义。
- **canonical JSON 算法**（独立模块 `brain/eventstore/canonical.py`）：
  - 字段按 **ASCII 升序**递归排序（嵌套 dict 同样排序）
  - 数组保留写入顺序（不排序）
  - `json.dumps(obj, sort_keys=True, ensure_ascii=False, separators=(",", ":"))`
  - 数值规约：禁 `NaN` / `Infinity`（写入前校验）
  - 空字符串 `""` 与缺字段必须可区分（schema 层强制；canonical 不补空）
  - 必须保证 `canonical(canonical_parsed) == canonical(原对象)`（幂等性）
- **哈希链算法**（独立模块 `brain/eventstore/hashchain.py`）：
  - `event_hash = sha256_hex(prev_hash || canonical(meta_dict))`，其中 `meta_dict` 包含除 `prev_hash` / `event_hash` 之外的全部已落表字段；`payload` 字段值在 `meta_dict` 中保持原 dict 形态（不预先序列化），`canonical(meta_dict)` 一次完成递归 canonical 序列化（嵌套 dict 一并按 ASCII 升序排列）。
  - 写入路径与 `verify_chain` 必须使用同一形态：DB 中 payload 列存 canonical 字符串，但读取层 `_row_to_dict` 反序列化回 dict 后再算 hash，与写入时输入形态一致。
  - 每个 `game_id` 第一条事件的 `prev_hash` = `"0" * 64`
  - 提供 `verify_chain(game_id) -> bool` 工具，全表扫描重算并比对，断点报告 `(seq, expected, actual)`
- **写入入口**：`AppendEventsAtomically(game_id: str, events: list[EventInput]) -> list[int]`：
  - 单事务（`BEGIN IMMEDIATE`）
  - **应用层进程内 `threading.Lock`** 保护 `(last_seq, last_hash)` 缓存（§6.1 已警告 BEGIN IMMEDIATE 不保护应用层共享状态；多线程 read+1 → 重复 seq → 哈希链断裂）
  - 启动时 / 进程冷启动后从 DB `SELECT seq, event_hash FROM events WHERE game_id=? ORDER BY seq DESC LIMIT 1` 重建缓存
  - 同一 input list 内的多条事件按列表顺序连续编号 / 串成链；事务失败整体回滚
  - 入参 `EventInput` 是 dataclass / TypedDict，**不含** `seq` / `prev_hash` / `event_hash` / `event_id`（这些由 store 写入时填）
- **索引**：`(game_id, event_type)`、`(game_id, actor)`、`(game_id, parent_event_id)`、`(game_id, round_no, phase)`；`(game_id, seq)` 由复合主键保证唯一。

### 3.2 事件类型枚举（freeze 全表 + 冻结 world.\* 命名）

新建 `brain/events/types.py`，用 `enum.StrEnum`（Python 3.11+）冻结全表：

- §4.1.2 全部事件类型（含 T1 新增的 `system.ingress_rejected` / `speech.playback_resolved`）。
- **T2 新增 freeze 的 `world.*` 事件类型**（来自 `world_state_push` 拆条；T1 §3.2.world_state_push 把命名权留给 T2）：

  | event_type                 | 默认 visibility             | payload 字段（canonical 顺序）                                                                                                                             |
  | -------------------------- | --------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------- |
  | `world.perception.sight`   | `[<observer_actor_id>]`     | `client_sample_id` / `distance_cm` / `observer_actor_id` / `rel_yaw_degrees` / `target_actor_id`（对齐 T1 `sight_entry`）                                  |
  | `world.perception.hearing` | `[<observer_actor_id>]`     | `age_seconds` / `client_sample_id` / `loudness` / `observer_actor_id` / `rel_yaw_degrees` / `source_actor_id`（对齐 T1 `heard_sound`）                     |
  | `world.actor_state`        | `[<actor_id>]`              | `actor_id` / `client_sample_id` / `current_action` / `facing_degrees` / `inventory_summary` / `pos_x_cm` / `pos_y_cm` / `pos_z_cm`                         |
  | `world.client_sample`      | `["orchestrator","system"]` | `client_sample_id` / `fanout_event_count` / `sample_wall_clock_ts`（仅作 `world_state_push` 业务级幂等的审计条目；同 client_sample_id 重复 ingest 不重写） |

  **拆条粒度声明**：T1 已硬约束"按 observer 拆条 + visibility 行粒度过滤"。T2 的具体落实是：1 次 `world_state_push` 请求 → N 条 `world.perception.sight` + M 条 `world.perception.hearing` + K 条 `world.actor_state` + 1 条 `world.client_sample`，全部在同一原子事务内 append。`world_state_push.schema.json` 的 transport 字段名是本卡 ingestion 的唯一输入来源；不得为了匹配上表私自添加 transport 字段。

- **不在 T2 freeze 范围**（留给后续卡）：
  - 物品 / 道具相关 (`world.item.*`)：留给 T8 ontology v2 阶段
  - 病毒游戏 `touch` 事件命名：留给 T9（00_total_plan §风险开放决策 #4 未定）

- **必须把 freeze 后的 world.\* 命名 + visibility 默认值反向同步到 `BrainService/protocol/protocol.md` 附录**（"T2 冻结的 world.\* 事件类型"小节），并在 BrainService 仓 commit；UE 仓 `Docs/protocol_pointer.md` 升级 commit hash + bump `protocol_version` patch（建议 `0.1.0 → 0.1.1`，patch = 文档/澄清，不破 schema）。

### 3.3 视角隔离（行粒度过滤）

- **写入前校验**（`brain/events/viewer.py`）：
  - viewer 字符串必须 ∈ `{public, audience, orchestrator, system, NPC01..NPC10}` 或匹配 `^Faction[A-Za-z0-9_]+$`
  - **`self` 一律拒绝**（硬约束 5）
  - `visibility` 不能为空数组（必须至少有一个合法 viewer）
  - `addressed_to` 列表元素必须是合法 `actor_id`
- **payload 反 `@hidden` 校验**：递归扫描 payload，任何 key 以 `@` 起首一律拒绝（硬约束 6 + §三反模式表"payload 内 `@hidden` 子字段"否决项）。这是写入闸口，不是读取过滤。
- **行粒度查询 API**：
  ```python
  query_events(
      game_id: str,
      viewer: str,
      *,
      since_seq: int = 0,
      event_type: str | list[str] | None = None,
      actor: str | None = None,
      round_range: tuple[int, int] | None = None,
      phase: str | None = None,
      limit: int = 1000,
  ) -> list[Event]
  ```

  - `viewer` 必须合法（非 `self`）
  - 返回 `visibility` 数组中包含有效 viewer 的事件（SQL 层用 `json_each` 子查询；性能不是 MVP 硬指标，先正确再优化）。
  - **有效 viewer 集合**：`viewer == "public"` 时只查 `{"public"}`；其他合法 viewer 默认查 `{viewer, "public"}`，因此 NPC / audience / orchestrator / system 都能看到公开事件，但不会因此看到彼此的私有事件。
  - **不为 `orchestrator` / `system` 开私有后门**：`viewer = "orchestrator"` 不自动看到 `speech.scratchpad` / `speech.intended` 等私有事件。orchestrator 或执行系统需要消费某类事件时，写入方必须在 `visibility` 显式加入 `"orchestrator"` / `"system"`（例如 `speech.bid` 默认就是 `["orchestrator"]`）。这避免"万能视角"成为隐性侧信道。

### 3.4 World state 拆条 ingestion（关键）

新建 `brain/world_state/ingest.py`：

- 函数签名：
  ```python
  ingest_world_state(
      game_id: str,
      push_payload: dict,   # 已通过 T1 world_state_push.schema.json 校验
  ) -> dict  # { "appended_seqs": [...], "deduplicated": bool }
  ```
- **业务级幂等**：`client_sample_id = push_payload["client_sample_id"]`；先查 events 表中是否已存在 `event_type="world.client_sample"` 且同一 `(game_id, client_sample_id)` 的审计事件。命中则用该审计事件自身 `seq` 与 `payload.fanout_event_count` 反推连续区间，返回既有 `appended_seqs`，并返回 `deduplicated=True`，不得重写任何事件行。
- **拆条逻辑**：见 §3.2 表。对每个 `observations[]` 元素，以 `actor_id` 作为 `observer_actor_id`：每条 `sighted_actors[]` 落一条 `world.perception.sight`，每条 `heard_sounds[]` 落一条 `world.perception.hearing`，该 observer 自身位置 / 朝向 / 当前动作 / inventory 摘要落一条 `world.actor_state`，最后追加 1 条 `world.client_sample` 审计行。
- **单事务 atomic write**：调用 `AppendEventsAtomically` 一次提交全部派生事件 + 1 条 `world.client_sample`，整体回滚或整体落地。
- **`wall_clock_ts`** 取自 push_payload 顶层 `sample_wall_clock_ts`（UE 端 wall-clock）；事件 `seq` 由 store 单调发号，与 wall_clock_ts 解耦。

### 3.5 投影代码（§4.2 全表，纯函数）

新建 `brain/projections/`，每个投影一个独立模块。所有投影输入 `(events: Iterable[Event], viewer: str)` 输出 dict / list；**不调任何 LLM**；**不调 db.write**；纯函数，可重复执行。

| 投影模块          | 功能                                                                                                                                                                                                                          |
| ----------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `commitments.py`  | §6.4 防赖账：扫 `speech.public` + 自己未抢中 `speech.intended` + Reflection 9问 第 5/6/7/8 项产物 + `annotation.speech_act` join。三源缺一不可。                                                                              |
| `vote_history.py` | 公开投票事件聚合。                                                                                                                                                                                                            |
| `claims.py`       | 角色声称 / 阵营声称 / 能力声称（从 `speech.public` + `annotation.speech_act` 派生）。                                                                                                                                         |
| `accusations.py`  | 指控关系：扫 `annotation.speech_act` 中 `speech_act_type = "accuse"`，`parent_event_id` join 回原发言。                                                                                                                       |
| `alliance.py`     | `alliance_state`：从 `alliance_propose` / `alliance_accept` / `alliance_betray` 派生。                                                                                                                                        |
| `pending.py`      | 含两个投影：`pending_actions`（每 actor 当前 in-progress `action.intent`，最多 1 行；命中 `action.resolved` / `action.cancelled` 即清空）、`pending_intended`（自己最近 K 拍未派生为 public 的 `speech.intended` 指针列表）。 |
| `game_state.py`   | 当前轮次 / 阶段 / `last_seq` / 胜负状态 / 在场 actor。                                                                                                                                                                        |

**约束**：投影损坏 → 直接 drop + 重建（投影不是真相源）。所有投影对外暴露的接口形态见 `brain/projections/__init__.py` 的 type hints。

### 3.6 召回工具（§4.4 全表）

新建 `brain/recall/tools.py`，覆盖 §4.4 表中的全部工具：

| 工具                       | 签名                                                                                                                                                                                                                        |
| -------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `quote`                    | `(game_id, seq, viewer)` → 事件原文 dict；不可见返回 `{"visible": False, "seq": seq}`                                                                                                                                       |
| `quote_by_round`           | `(game_id, round_no, viewer, actor=None)` → list                                                                                                                                                                            |
| `search_history`           | `(game_id, viewer, keywords=None, actor=None, round_range=None, event_type=None, speech_act_type=None, addressed_to=None)` → list of `{seq, round_no, phase, actor, event_type, speech_act_type, snippet}`（§4.4 末尾示例） |
| `list_my_commitments`      | `(game_id, actor_id, round_range=None)`                                                                                                                                                                                     |
| `list_votes`               | `(game_id, viewer, round=None)`                                                                                                                                                                                             |
| `my_recent_notes`          | `(game_id, actor_id, n)`                                                                                                                                                                                                    |
| `my_recent_reflections`    | `(game_id, actor_id, n)`                                                                                                                                                                                                    |
| `list_alliance_state`      | `(game_id, viewer)`                                                                                                                                                                                                         |
| `list_pending_actions`     | `(game_id, viewer)`                                                                                                                                                                                                         |
| `list_my_pending_intended` | `(game_id, actor_id, n)`                                                                                                                                                                                                    |

约束：

- 召回工具**只走结构化字段过滤**（§2.1.2）；**不引入向量召回**。
- 召回返回事件**原文或指针**，**不返回** free-form summary（§4.4）。
- 所有 viewer 参数走同一行粒度过滤路径（§3.3 `query_events`）—— **不允许**召回工具私自绕过 viewer 过滤。
- `search_history` 的 `keywords` MVP 走 SQL `LIKE`（多关键词 AND）；性能不是 MVP 硬指标。
- `snippet` 长度建议 ≤ 120 字符；完整原文必须经 `quote(game_id, seq, viewer)` 取。

### 3.7 跨局 `_meta.db` 最小骨架（§7.3）

新建 `brain/meta/lifecycle.py` + 独立 SQLite 文件 `data/_meta.sqlite3`。

- **`agent_lifecycle_events` 表**：
  - `event_id TEXT PRIMARY KEY`（uuid v4）
  - `event_type TEXT NOT NULL`（枚举：`created` / `delete_proposed` / `delete_vetoed` / `delete_executed` / `revived` / `archived`）
  - `actor_id TEXT NOT NULL`
  - `game_id TEXT NOT NULL`（触发它的局；MVP 所有 lifecycle 事件都必须归属一局）
  - `triggered_seq INTEGER`（局内事件 seq，可空 —— 例如 `created` 在游戏开始前就发生）
  - `payload TEXT NOT NULL`（canonical JSON）
  - `affects_persona_continuity INTEGER NOT NULL DEFAULT 1`（bool；为 1 时该 actor_id 永久不可复用）
  - `wall_clock_ts TEXT NOT NULL`
- **MVP 仅实现两条事件类型的写入接口**（其余作 schema 占位但 MVP 不调用）：
  - `register_actor_creation(actor_id, game_id, payload, *, triggered_seq=None) -> event_id`
  - `register_actor_deletion(actor_id, game_id, triggered_seq, payload, *, affects_persona_continuity=True) -> event_id`
- **查询接口**：
  - `is_actor_id_reusable(actor_id) -> bool`：对 actor_id 是否曾出现过 `delete_executed` 且 `affects_persona_continuity=1` 的事件做判定；命中即返回 `False`（永久不可复用）。
  - `get_lifecycle_event(event_id) -> dict | None`
- **DB trigger 同样禁 UPDATE/DELETE** `agent_lifecycle_events`（与 events 表一致约束）。
- **本卡不做** `delete_proposed` / `delete_vetoed` / `revived` / `archived` 的写入接口；schema 留位，调用 → `NotImplementedError`。

### 3.8 局内 `system.delete_executed` 写入约束（数据层闸口）

为支撑硬验收"`_meta.db` 写入 `created`/`delete_executed` 后可被局内 `system.delete_executed` 正确引用"，在 EventStore 写入入口加额外校验：

- 当 `event_type == "system.delete_executed"` 时，`payload.lifecycle_event_id` 必填，且必须命中 `_meta.db.agent_lifecycle_events.event_id` 中一条 `event_type="delete_executed"` 的记录；若 payload 同时携带 `actor_id`，还必须与 lifecycle 记录的 `actor_id` 一致。否则写入拒绝。
- 此校验只在写入端做；查询端不再二次查 `_meta.db`，避免读路径耦合两库。

### 3.9 Session 元数据（最小）

新建 `brain/sessions/manager.py`：

- `sessions` 表（在 events 同库；列：`game_id PRIMARY KEY` / `created_ts` / `protocol_version`）。
- `create_session(protocol_version: str = PROTOCOL_VERSION) -> game_id`：生成 uuid v4、insert、返回 game_id。**T2 不暴露 HTTP**；`POST /v1/games` 的 server 路由由后续卡接入时调本函数。
- `get_session(game_id) -> SessionMeta | None`。
- `protocol_version` 从配置读取（建议 `brain/config.py` 暴露常量，对齐 BrainService `protocol/protocol.md` 顶部声明；T2 完成并追加 world.\* 附录后应为 `0.1.1`）。

### 3.10 Roster 注册（数据层 hook）

新建 `brain/sessions/roster.py`：

- `register_roster(game_id, actors: list[ActorRegistration])`：
  - 对每个 actor_id 调 `is_actor_id_reusable(actor_id)`；若不可复用 → 整个 register 拒绝，抛 `ActorIdNotReusable`。
  - 对每个新 actor_id 调 `register_actor_creation(actor_id, game_id, payload={...初始化元数据})`。
- 不在本卡处理 HTTP；只提供 Python API，T1 endpoint `POST /v1/games/{game_id}/roster` 的服务端路由由后续卡接入时调本函数。

### 3.11 测试套件（pytest，验收的机器化部分）

新建 `BrainService/tests/`：

| 测试模块                       | 覆盖                                                                                                                                                                                                    |
| ------------------------------ | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `test_canonical.py`            | canonical JSON 幂等 + 字段升序 + ensure_ascii=False + Unicode 正确 + NaN/Infinity 拒绝 + 空字符串与缺字段可区分                                                                                         |
| `test_hashchain.py`            | 第一条 prev_hash = "0"\*64；连续追加 hash 链验证；任意篡改一行 → `verify_chain` 返回 False 并指明 seq                                                                                                   |
| `test_eventstore_triggers.py`  | DB 层尝试 `UPDATE events ...` → SQLite 抛错；尝试 `DELETE FROM events ...` → 抛错；同样校验 `_meta.db.agent_lifecycle_events`                                                                           |
| `test_serialization.py`        | 写入串行化：多线程并发调 `AppendEventsAtomically` → seq 单调 + hash 链不断 + 无重复 seq                                                                                                                 |
| `test_viewer_isolation.py`     | 灌入手写事件流（fixture），验证 `query_events` 在 14 个基础 viewer 视角下行粒度隔离：`public` / `audience` / `orchestrator` / `system` / `NPC01..NPC10` 全覆盖，并额外覆盖 1 个 `FactionAlpha` 正则样例 |
| `test_self_rejection.py`       | 写入 visibility 含 `self` → 拒绝；`addressed_to` 含 `self` → 拒绝；payload 含 `@hidden` 字段 → 拒绝                                                                                                     |
| `test_world_state_split.py`    | 1 次 `world_state_push` → N+M+K+1 条事件落地；同 client_sample_id 重发 → deduplicated=True；observer 视角只看到 public + 自己的 sight/hearing/actor_state                                               |
| `test_projections.py`          | commitments 三源覆盖（speech.public / 自己未抢中 speech.intended / Reflection 9问 第 5/6/7/8 项产物）；其余 7 个投影各至少 1 个 happy path 用例                                                         |
| `test_recall.py`               | §4.4 全部 10 个工具各至少 1 个 happy path + 1 个 viewer 隔离 case；`search_history` 返回字段齐全；`quote` 不可见返回标记                                                                                |
| `test_lifecycle.py`            | `register_actor_creation` / `register_actor_deletion` / `is_actor_id_reusable` 三接口；`delete_executed` 后跨局 register_roster 拒绝复用                                                                |
| `test_delete_executed_link.py` | 写入 `system.delete_executed` 时 `payload.lifecycle_event_id` 不存在 / 指向非 `delete_executed` / actor_id 不匹配 → 拒绝；合法引用 → 通过                                                               |

提供 `pytest -q` 一行命令复现，README 写明。

### 3.12 BrainService Python 项目骨架

- `pyproject.toml`：runtime deps 仅 `jsonschema`；dev deps `pytest`；目标 Python 3.11+（用 `enum.StrEnum`）。
- 目录布局（建议）：

  ```
  BrainService/
    brain/
      __init__.py
      config.py                       # protocol_version、db 路径常量
      eventstore/
        __init__.py
        store.py                      # AppendEventsAtomically + query_events
        canonical.py                  # canonical JSON
        hashchain.py                  # prev_hash / event_hash
        schema.sql                    # events 表 + triggers
        bootstrap.py                  # 启动时建表 + 重建 last_seq/last_hash 缓存
      events/
        __init__.py
        types.py                      # event_type StrEnum + visibility 默认值表
        viewer.py                     # viewer 封闭集合校验
        validate.py                   # 写入闸口（self / @hidden / NaN 等）
      world_state/
        __init__.py
        ingest.py                     # 拆条逻辑
      projections/
        __init__.py
        commitments.py
        vote_history.py
        claims.py
        accusations.py
        alliance.py
        pending.py
        game_state.py
      recall/
        __init__.py
        tools.py
      meta/
        __init__.py
        lifecycle.py
        schema.sql
      sessions/
        __init__.py
        manager.py                    # create_session / get_session
        roster.py                     # register_roster
    protocol/                         # T1 已生成，本卡只追加 world.* 事件类型附录
    data/                             # gitignored；运行时生成 .sqlite3
    tests/
      fixtures/
        sample_event_stream.json      # 手写事件流灌入用
        sample_world_state_push.json
      test_canonical.py
      test_hashchain.py
      test_eventstore_triggers.py
      test_serialization.py
      test_viewer_isolation.py
      test_self_rejection.py
      test_world_state_split.py
      test_projections.py
      test_recall.py
      test_lifecycle.py
      test_delete_executed_link.py
    pyproject.toml
    README.md                         # T1 已建，本卡补充"如何跑测试"
  ```

- `data/` 加入 `BrainService/.gitignore`（如已忽略 `*.sqlite3` 则无需重复）。
- README 补充：项目结构 + `pytest -q` 跑全测 + 数据层 API 简单使用示例（`AppendEventsAtomically` / `query_events` / `quote` 三段最小代码）。

---

## 4. 范围外（DON'T）

- ❌ 不实现 HTTP server / FastAPI / aiohttp 路由（推迟到下一卡按需引入；T2 提供 Python API 即可）。
- ❌ 不实现 Reasoner LLM 调用 / Validator / 任何 prompt 拼装（属 T3 / T5）。
- ❌ 不实现 bid 裁决 / Listener-as-filter / 反思 9 问执行 / phase-level 摘要 worker（属 T4 / T5）。
- ❌ 不冻结 `world.item.*` / `world.touch` 等后续游戏专属事件类型（属 T8 / T9）。
- ❌ 不实现 ontology v2（pickup / use_item / inspect / follow / flee_from）—— 属 T8。
- ❌ 不写 02 病毒游戏专属规则（属 T9）。
- ❌ 不动 UE C++ / Source/AILiveProject/\* / UE 资产 / 蓝图。
- ❌ 不实现向量召回 / fact triple 抽取 / 自动 summary 覆盖原文（§三反模式表）。
- ❌ 不实现跨局 Experience Pool（§7.2 已划界为非 MVP）。

---

## 5. 交付清单

| 路径                                                         | 类型       | 一句话职责                                                                                    |
| ------------------------------------------------------------ | ---------- | --------------------------------------------------------------------------------------------- |
| `BrainService/pyproject.toml`                                | 新建       | 项目元数据 + runtime deps（jsonschema）+ dev deps（pytest）+ Python ≥3.11                     |
| `BrainService/.gitignore`                                    | 改         | 追加 `data/` 与 `*.sqlite3`（如未忽略）                                                       |
| `BrainService/README.md`                                     | 改         | 补充项目结构 + `pytest -q` 命令 + 三段最小 API 示例                                           |
| `BrainService/brain/__init__.py`                             | 新建       | 包入口                                                                                        |
| `BrainService/brain/config.py`                               | 新建       | `PROTOCOL_VERSION` / db 路径常量                                                              |
| `BrainService/brain/eventstore/store.py`                     | 新建       | `AppendEventsAtomically` / `query_events` / `verify_chain` 主入口                             |
| `BrainService/brain/eventstore/canonical.py`                 | 新建       | canonical JSON 序列化算法                                                                     |
| `BrainService/brain/eventstore/hashchain.py`                 | 新建       | prev_hash / event_hash 计算 + 全链 verify                                                     |
| `BrainService/brain/eventstore/schema.sql`                   | 新建       | events 表 + triggers + 索引                                                                   |
| `BrainService/brain/eventstore/bootstrap.py`                 | 新建       | 建表 + 重建 last_seq/last_hash 缓存                                                           |
| `BrainService/brain/events/types.py`                         | 新建       | event_type StrEnum 全表 + 默认 visibility 模板（含 T2 freeze 的 world.\* 命名）               |
| `BrainService/brain/events/viewer.py`                        | 新建       | viewer 封闭集合校验 + Faction 正则                                                            |
| `BrainService/brain/events/validate.py`                      | 新建       | 写入闸口：self / @hidden / NaN / 多余字段 / visibility 空 / 异常类型                          |
| `BrainService/brain/world_state/ingest.py`                   | 新建       | `world_state_push` 拆条 + 业务级幂等                                                          |
| `BrainService/brain/projections/commitments.py`              | 新建       | §6.4 三源 commitments 投影                                                                    |
| `BrainService/brain/projections/vote_history.py`             | 新建       | 投票历史                                                                                      |
| `BrainService/brain/projections/claims.py`                   | 新建       | 角色 / 阵营 / 能力声称                                                                        |
| `BrainService/brain/projections/accusations.py`              | 新建       | 指控关系（join annotation.speech_act）                                                        |
| `BrainService/brain/projections/alliance.py`                 | 新建       | alliance_state                                                                                |
| `BrainService/brain/projections/pending.py`                  | 新建       | pending_actions + pending_intended 两个投影                                                   |
| `BrainService/brain/projections/game_state.py`               | 新建       | 当前轮次/阶段/last_seq/胜负                                                                   |
| `BrainService/brain/recall/tools.py`                         | 新建       | §4.4 全部 10 个召回工具                                                                       |
| `BrainService/brain/meta/lifecycle.py`                       | 新建       | `agent_lifecycle_events` 写入 + 查询 + 跨局复用判定                                           |
| `BrainService/brain/meta/schema.sql`                         | 新建       | \_meta.db 表 + triggers                                                                       |
| `BrainService/brain/sessions/manager.py`                     | 新建       | `create_session` / `get_session`                                                              |
| `BrainService/brain/sessions/roster.py`                      | 新建       | `register_roster`（含 actor_id 复用判定）                                                     |
| `BrainService/tests/fixtures/sample_event_stream.json`       | 新建       | 手写事件流灌入用 fixture                                                                      |
| `BrainService/tests/fixtures/sample_world_state_push.json`   | 新建       | world_state_push 拆条测试 fixture                                                             |
| `BrainService/tests/test_*.py`                               | 新建 11 份 | 见 §3.11 表                                                                                   |
| `BrainService/protocol/protocol.md`                          | 改         | 追加 "T2 冻结的 world.\* 事件类型" 附录小节                                                   |
| `BrainService/protocol/schemas/world_state_push.schema.json` | （评估）   | 若 T1 schema 无需调整 world_state 字段命名以贴合拆条则不动；如需要补 enum 字段，需 bump patch |
| `Docs/protocol_pointer.md`                                   | 改         | 升级 commit hash + bump `protocol_version` 到 `0.1.1`（patch = 文档/澄清，不破 schema）       |

---

## 6. 验收（机器化优先）

完成时按顺序自检并给出勾选状态：

- [ ] `cd BrainService && pip install -e .[dev] && pytest -q` 全绿。
- [ ] DB-level trigger 阻止 `UPDATE events` / `DELETE FROM events` / `UPDATE agent_lifecycle_events` / `DELETE FROM agent_lifecycle_events`；测试覆盖。
- [ ] canonical JSON：`canonical(parse(canonical(x))) == canonical(x)`；字段 ASCII 升序；NaN / Infinity 拒绝；Unicode 不被 escape；空字符串与缺字段在 schema 层可区分。
- [ ] `verify_chain(game_id)` 在干净链上返回 True；篡改任意一行后返回 False 并指出 seq。
- [ ] 多线程并发 `AppendEventsAtomically` 测试 → seq 单调连续 + 无重复 + hash 链不断。
- [ ] 视角隔离：用 fixture 灌入含 14 个基础 viewer + 1 个 Faction 样例的事件后，`query_events` 结果集严格对应（覆盖 `public` / `audience` / `orchestrator` / `system` / `NPC01..NPC10` / `FactionAlpha`）；NPC / audience / orchestrator / system 可见 `public` 事件，但 orchestrator / system 不自动看见私有事件。
- [ ] 写入闸口拒绝：visibility 含 `self` / `addressed_to` 含 `self` / payload 含 `@xxx` 字段 / visibility 为空数组 / payload 含 NaN/Infinity 全部抛错。
- [ ] `world_state_push` 拆条：1 次 push（含 2 observer × 3 sight + 2 hearing + 1 actor_state per observer）→ 落地 6+4+2+1 = 13 条事件；observer A 视角只看到 public + 自己那部分；同 client_sample_id 重发 → `deduplicated=True`，DB 行数不变。
- [ ] commitments 投影覆盖三源：fixture 中包含 (a) 公开发言承诺 (b) 自己未抢中的 intended 承诺 (c) Reflection 9 问第 5/6/7/8 项产物，三类都被 commitments 投影 capture。
- [ ] §4.4 全部 10 个召回工具均有 happy path + viewer 隔离测试；`search_history` 返回字段齐全（`seq` / `round_no` / `phase` / `actor` / `event_type` / `speech_act_type` / `snippet`）；`quote(game_id, seq, viewer)` 不可见时返回 `{"visible": False, "seq": ...}`。
- [ ] `_meta.db.agent_lifecycle_events` 写入 `created` 与 `delete_executed` 两条；后续 `register_roster` 用同 actor_id 注册被拒绝；`is_actor_id_reusable` 返回 False。
- [ ] 局内 `system.delete_executed` 写入时 `payload.lifecycle_event_id` 必须命中 `_meta.db` 现有 `delete_executed` 行且 actor_id 一致；不命中或类型不匹配均拒绝。
- [ ] T2 freeze 的 `world.perception.sight` / `world.perception.hearing` / `world.actor_state` / `world.client_sample` 命名 + 默认 visibility 已追加到 `BrainService/protocol/protocol.md` 附录；BrainService bump `protocol_version` 到 `0.1.1` 并提交 commit；UE 仓 `Docs/protocol_pointer.md` 升级 commit hash + protocol_version。
- [ ] 由人类（不是 ClaudeCode）评审 README 的"如何跑测试"指引可复现。

---

## 7. 上下游交接

**前置卡**：T1 ✅。本卡所有 ingestion 输入形态严格按 T1 schema；任何形态偏离都视为 bug，向前修 protocol（升 minor）而非在 T2 私自加字段。

**下游消费者**：

| 后续卡   | 依赖 T2 的什么                                                                                                                                                                        |
| -------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| T3       | `AppendEventsAtomically` 写 Reasoner 派生的四通道事件；`query_events` 拉前序事件给 Validator 做引用存在性校验                                                                         |
| T4       | `query_events`（拉 bid 事件）+ `pending_intended` 投影（floor control 输入）                                                                                                          |
| T5       | `pending_intended` / `commitments` 投影 + `quote` / `search_history` / `my_recent_notes` / `my_recent_reflections` 召回工具                                                           |
| T6       | 后续 brain HTTP server 接入时调用本卡 `query_events` 与 `quote` 服务 UE 端事件查询路径                                                                                                |
| T7       | UE Dispatcher 拉 `action.intent` / `speech.public` 时可复用 `query_events` 的 `event_type` 过滤，但执行系统要消费的事件必须在写入时显式包含 `system` visibility，不能绕过 viewer 过滤 |
| T8       | `register_actor_deletion` + `system.delete_executed` 写入闸口；ontology v2 升级时新增 intent 写入到 `action.intent`                                                                   |
| T9 / T10 | 病毒游戏专属事件类型（如 `world.touch`）由 T9 在 `events/types.py` 追加；`commitments` 投影规则可能扩展                                                                               |

---

## 8. 风险与已知坑

- **SQLite 串行化陷阱**（§6.1 明确警告）：`BEGIN IMMEDIATE` 只解决 SQL 层并发，不保护应用层 in-memory `last_seq` / `last_hash` 缓存。**多线程同时 read+1 → 重复 seq → 哈希链断裂**。必须用 `threading.Lock` 包住整个 `(读缓存 → 算事件 → 写库 → 更新缓存)` 路径，事务也包在锁内。测试必须含并发用例（`test_serialization.py`）。
- **canonical JSON 必须真 deterministic**：Python 默认 `json.dumps` 不排序字段；`sort_keys=True` 只在顶层；嵌套 dict 同样要排序（`sort_keys=True` 实际是递归的，但要测）。`ensure_ascii=False` 必须设；否则中文 escape 后哈希一致但跨语言比对不直观。`separators=(",", ":")` 去除空格。**禁 NaN / Infinity**：写入前递归扫 float，非有限值抛错。
- **canonical JSON 与 wire JSON 区分**：UE 端 round-trip 测试比对的是 wire JSON（T1 examples）；持久层存的是 canonical JSON。两者算法应一致，但 wire 可以含可读空格；**持久层必须严格 canonical**（哈希链依赖）。
- **payload 含 `@hidden` 拒绝**（§三反模式 + 硬约束 6）：递归扫 dict / list；任何 key 起首 `@` 一律拒绝。这是写入闸口而非读取过滤——读取过滤晚一步就泄露。
- **`self` 严禁入库**（硬约束 5）：visibility 数组 + addressed_to 数组必须拒绝 `self`；payload 内若存在命名为 `viewer` / `visibility` / `addressed_to` / `observer_actor_id` / `actor_id` 等具备可见性或 actor 语义的字段，也必须递归拒绝 `self`。写入前由 `events/validate.py` 全量拒绝。
- **viewer 封闭集合**：`{public, audience, orchestrator, system, NPC01..NPC10}` 枚举 + `^Faction[A-Za-z0-9_]+$` 正则；任何不命中抛错。`Faction<X>` MVP 不分配实例，但校验通道留位。
- **`prev_hash` 起始值约定**：每个 `game_id` 第一条事件 `prev_hash = "0" * 64`；`verify_chain` 起点也用此值。这个常量必须在 `hashchain.py` 顶部 const 声明，避免散落多处。
- **`wall_clock_ts` 不参与排序**（§4.1.1）：`seq` 是唯一排序键。索引、查询、投影一律用 `seq` 排序；`wall_clock_ts` 仅信息性返回。
- **同库分行 vs per-game 分库**：T2 用同库分行（schema 简单 + 跨局查询统一），通过 `(game_id, seq)` 复合索引访问。优势是 `_meta.db` 跨局查询无需 attach；劣势是单 db 体积随时间累积——MVP 不是问题，后续按需分文件。
- **`_meta.db` 与 events 同一进程同事务？**：MVP 用两个独立 sqlite 文件 + 各自连接；写 `system.delete_executed` 时**先**写 `_meta.db.agent_lifecycle_events` 拿到 event_id，**再**写本局 events 表（payload 引用此 id）；两阶段写不在同一事务，但 `_meta.db` 写入幂等（uuid v4 主键 + INSERT OR IGNORE 不适用于 uuid 唯一），需要 \_meta 写入失败 → 不进入第二阶段。这是数据层闸口的硬要求。
- **commitments 三源不能漏**：§6.4 明确要求扫 (a) `speech.public` (b) 自己未抢中 `speech.intended` (c) Reflection 9 问第 5/6/7/8 项产物。漏一源就构成防赖账漏洞——测试必须三源各至少一条 fixture 命中。
- **`speech_act_type` 不属 base event**（§4.1.4）：commitments / accusations 投影读 `speech_act_type` 时**只能** join `annotation.speech_act.parent_event_id`，**不得** UPDATE 原发言事件填字段。投影代码必须走 join 路径。
- **投影是纯函数**（§4.2）：投影代码只读、不写、不调 LLM。投影可重建，事件流不可丢。投影代码不允许有任何 `db.execute("INSERT/UPDATE/DELETE")` 调用——CI 应有静态检查或 review 手抓。
- **跨局 actor_id 不可复用的边界**：`affects_persona_continuity=False` 的 `delete_executed` 不阻止复用（MVP 全部默认 True，但接口留位）。
- **`world.client_sample` 不是 `world.perception.*` 的一部分**：它只是审计标记，visibility 限 `["orchestrator", "system"]`；不进入任何 NPC 视角的 prompt。
- **不在本卡引入 protocol_version 的 minor bump**：T2 只追加 `world.*` 事件类型枚举（数据层落库 event_type 命名）+ visibility 模板附录；不改 T1 已冻结的 schema 字段，因此 patch（`0.1.1`）足够。如果发现 T1 schema 需要调整 world_state_push 字段以贴合拆条 —— 那是 T1 schema 漏洞，按 minor 升 + 评审记录。
- **测试 fixture 必须含 unicode**：fixture JSON 包含中文 / emoji / 控制字符，验证 canonical + 哈希链 + viewer 隔离不被字符集影响。

---

## 9. 可选前置准备（如果新窗口提前问）

- Python 版本：锁定 ≥3.11（`enum.StrEnum` 需要）。
- venv 选型：`python -m venv .venv` 即可，`pyproject.toml` 用 `[project.optional-dependencies] dev = ["pytest"]`。
- runtime 是否引入 `jsonschema`：建议引入，用于本卡可选地在写入闸口内对 payload 做轻量 schema 校验（但**不**用 protocol/schemas/ 校验业务事件——那是 transport 形态，不是事件流形态；二者 schema 不同）。如不打算用，runtime deps 可清空。
- 提交人 git config：用 BrainService 单独配（与 UE 工程 author 元数据隔离）。
- 是否引入 alembic 等迁移工具：MVP 不需要；schema.sql 用 `CREATE ... IF NOT EXISTS` 幂等执行即可。
