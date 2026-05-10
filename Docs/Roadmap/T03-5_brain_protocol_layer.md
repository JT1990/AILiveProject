# T3+T4+T5 · Brain 协议层主链 + LLM Provider + HTTP Server

> 主仓库：BrainService
> 前置卡：T1 ✅（协议骨架 + schema/examples）、T2 ✅（数据层 + EventStore + 投影 + 召回 + `_meta.db`）
> 下游消费者：T6（UE 协议基础设施需对接 brain HTTP server）、T7（UE Dispatcher 消费 `action.intent` / `speech.public`）、T9（02 病毒游戏 Brain 端机制）、T10（一局完整 PIE 联调）

> **状态**：✅ 已实施。本卡是把 T3 / T4 / T5 三张原子卡合并的**事后设计存档**——`BrainService/brain/{orchestration,llm,server}/` 实施代码已落、对应测试已存在。§6 验收清单的勾选状态由协作者按当前 brain 仓现状逐条补勾（pytest 跑通即可勾）。

---

## 1. 目标

把 BrainService 从"只能被 Python 调"的库升级为**可独立运行的 brain 进程**：实现 memory_principles §五 协议层全链（Reasoner+Validator+Bid+Listener-as-filter+三级反思+prompt 拼装）+ LLM provider 抽象（FakeProvider + DeepSeek 真实对接）+ FastAPI HTTP server（T1 全部 10 个 endpoint），并配 4 FakeProvider × 5 拍硬验收 e2e 测试与 `protocol/examples/*.json` round-trip。

---

## 2. 必读上下文锚点

按顺序读完再动手：

1. **`Docs/Roadmap/00_total_plan.md`**（重点："边界"、"阶段 2"、"关键依赖图"、"风险与开放决策"）。
2. **`Docs/Roadmap/handbook.md`** §0 工作流总览 + §4 已收敛项目级决策。
3. **`Docs/memory_principles.md`** 关键章节：
   - §〇 硬约束 1–6（特别是硬约束 5 viewer 封闭集合 / 硬约束 6 视角隔离行粒度）
   - §一 两层架构
   - §2.1 数据层不变量、§2.2 协议层不变量
   - §三 反模式表（避免 Reasoner 直输 `speech.public` / `action.intent` / 数值 `urgency` 等）
   - §4.1.1 事件字段（必备 + 条件必填）
   - §4.1.2 事件类型枚举全表
   - §4.1.4 `speech_act_type` 标注归属（不入 base event）
   - §4.1.5 默认事件可见性模板（特别是 `action.intent` 默认 `[<actor_id>]`，本卡覆盖为 `[actor_id, "system"]`）
   - §4.2 投影语义（特别是 `pending_actions` / `pending_intended` / `commitments`）
   - §4.3 prompt 拼装优先级（必保留 1–9 / 可裁剪 / 可压缩 / 必保留段超 token 必须报错）
   - §4.4 召回工具契约
   - §5.1 Reasoner 四通道 schema（含 §5.1.1 通道语义表 + §5.1.2 不在 schema 中的字段）
   - §5.2 Validator 与事件派生（§5.2.1 9 条规则 + §5.2.2 派生流程 + §5.2.3 失败处理）
   - §5.3 Bid + floor control 全部子节
   - §5.4 动作意图协议（§5.4.1 ontology 词汇表 + §5.4.2 动作生命周期 + 关键规则——本卡 ontology v1 仅 `move_to / sit / wait`）
   - §5.5 Listener-as-filter 全部
   - §5.6 Reasoner + Validator 架构图
   - §5.7 三级反思层级 + §5.7.1 9 问 + §5.7.2 phase-level 摘要规范
   - §6.1 EventStore 写入（已由 T2 实现，本卡需扩 local_id 接口）
   - §6.3 故障处理 per-tick 粒度
   - §6.4 commitments 投影（已由 T2 实现，本卡 prompt 拼装层调用）
4. **`Docs/Roadmap/T01_protocol_skeleton.md`** §3.2 / §3.3 / §3.4 —— endpoint 表 + schema 名册 + 命名空间约束。
5. **`Docs/Roadmap/T02_brain_data_layer.md`** §3.1 / §3.3 / §3.4 / §3.5 / §3.6 / §3.9 / §3.10 —— EventStore / viewer 隔离 / world_state 拆条 / 投影 / 召回 / sessions / roster 现状。**特别记住**：T2 §3.3 已硬约束"viewer=orchestrator/system 不自动看到私有事件，需消费时写入方必须显式加 system"——本卡 `action.intent` 必须 `visibility=[actor_id, "system"]`。
6. **BrainService 真相源**：
   - `BrainService/protocol/protocol.md`（人读契约）
   - `BrainService/protocol/schemas/*.schema.json`（12 份）
   - `BrainService/protocol/examples/*.json`（fixture）
   - `BrainService/brain/eventstore/store.py`（已落地 `AppendEventsAtomically` / `query_events` / `verify_chain` / `iter_all_events`；本卡 surgical 扩 local_id）
   - `BrainService/brain/events/types.py`（`EventType` 全枚举）
   - `BrainService/brain/events/validate.py`（写入闸口）
   - `BrainService/brain/projections/*`（`pending_actions` / `pending_intended` / `commitments` / `claims` / `vote_history` / `accusations` / `alliance` / `game_state`）
   - `BrainService/brain/recall/tools.py`（§4.4 全部 10 个召回工具）
   - `BrainService/brain/sessions/manager.py` / `roster.py`
7. **`Docs/PRD.md`**：「项目核心」「AI 心智决策系统」「让博弈对 AI 自己而言"重要"」三节（理解 LLM 作为大脑的设计意图 / Delete 设计动机）。
8. **`CLAUDE.md`**（项目根） + 用户级 `~/.claude/CLAUDE.md`。
9. **DeepSeek API 文档**（仅在写 `brain/llm/deepseek.py` 时查）：
   - <https://api-docs.deepseek.com/api/create-chat-completion>
   - <https://api-docs.deepseek.com/zh-cn/guides/json_mode>

不需要读 DevLog（本卡不动 UE 资产）。

---

## 3. 范围内（DO）

### 3.1 LLM provider 抽象（`brain/llm/`）

| 文件 | 职责 |
| --- | --- |
| `brain/llm/__init__.py` | 暴露 `make_provider(name, **cfg)` 工厂；`name ∈ {"fake", "deepseek"}`；从 `brain/config.py` 读默认值 |
| `brain/llm/provider.py` | `Provider` 抽象类 + `ProviderResult` dataclass（`raw_text: str` / `parsed: dict` / `elapsed_ms: int` / `finish_reason: str`）；唯一方法 `complete_structured(messages: list[dict], json_schema: dict, *, model_hint: str \| None = None) -> ProviderResult` |
| `brain/llm/fake.py` | `FakeProvider(scripted_responses: list[dict \| Exception])`：每次 `complete_structured` 弹一个；遇 `Exception` 子类（如 `TimeoutError` / `ValueError`）则 `raise`；遇 dict 则用 `canonical(...)`（T2 已有，从 `brain/eventstore/canonical.py` import）序列化为 raw_text 后返 `ProviderResult` |
| `brain/llm/deepseek.py` | `DeepSeekProvider(api_key, base_url="https://api.deepseek.com")`：`complete_structured` 内部用 `httpx.Client` POST `/chat/completions`，请求体含 `model` / `messages` / `response_format={"type": "json_object"}`；解析返回 JSON 字段 `choices[0].message.content`，用 `json.loads` 转 dict 填 `parsed`；超时 / 5xx 走指数退避（`max_retries=3`，`backoff_base=0.5s`）；4xx / parse 失败不重试 |

**约束**：

- Provider 不写事件流 / 不调投影 / 不调 EventStore——纯外部 API 适配层。
- `model_hint` 留位但 MVP 用 `cfg["default_model"]`（DeepSeek 配 `"deepseek-chat"`）。
- `messages` 形态遵守 OpenAI Chat Completion 风格：`[{"role": "system", "content": "..."}, {"role": "user", "content": "..."}]`。
- `json_schema` 参数 MVP 仅作文档传递，**不**强制 provider 端校验（DeepSeek json_object 模式不强制 schema）；本地 Validator 兜底。

### 3.2 EventStore 扩展：批内 local_id 引用机制（surgical 改 T2）

**目的**：让派生层在同一原子批次里写多条事件（如 4 通道 + `action.intent` + `action.cancelled` + `speech.public` + `annotation.listener_filter`）时，能用 `_local_id` / `parent_local_id` / `source_local_id` 安全引用同批未来事件的真实 seq，而无需调用方预猜 seq（接受协作者审查 #1 阻断级修订）。

#### 3.2.1 `EventInput` 扩展字段（`brain/eventstore/store.py`）

调用方传入的 dict 现增 3 个可选字段：

| 字段 | 类型 | 用途 |
| --- | --- | --- |
| `_local_id` | `int \| None` | 批内自编号（建议 0, 1, 2, ...）；不入 DB；仅供同批后续事件 `parent_local_id` / `source_local_id` 引用 |
| `parent_local_id` | `int \| None` | 同批先于此事件的某条事件的 `_local_id`；store 解析为真实 seq 后填入 `parent_event_id` |
| `source_local_id` | `int \| None` | 同上，解析为 `source_event_id` |

**冲突规则**（store 写入入口校验）：

- `parent_local_id` 与 `parent_event_id` **二选一**（同时填 → `EventValidationError("conflicting parent reference")`）。
- `source_local_id` 与 `source_event_id` 同上。
- 引用的 `_local_id` 必须在**本批中已经分配过 seq**（即在当前事件之前列在 events 列表中）；前向引用 → `EventValidationError("forward reference not allowed: local_id=X")`。
- 引用的 `_local_id` 在批内未被任何事件声明 → `EventValidationError("unknown local_id=X")`。

#### 3.2.2 锁内 `local_id_to_seq` 映射构建

在 `EventStore.append_events_atomically`（`store.py:61-172`）现有锁段内：

1. 进入锁后初始化 `local_id_to_seq: dict[int, int] = {}`。
2. 主循环 `for ev in events` 内、计算 `seq = next_seq` 之前：
   - 若 `ev` 含 `parent_local_id` 且 `parent_event_id` 也有非 None 值 → 抛 conflict。
   - 若 `ev` 含 `parent_local_id`：从 `local_id_to_seq` 查映射；缺失 → 抛 `unknown local_id`；命中 → 把 `ev["parent_event_id"]` 设为映射值（覆盖 None）。
   - `source_local_id` 同样处理。
3. 计算 `seq = next_seq` 后立即 `if "_local_id" in ev: local_id_to_seq[ev["_local_id"]] = seq`。
4. 之后照常走既有 meta 构建 / 哈希计算 / 引用完整性 `_check_refs` 路径。

**实现位置**：`store.py:61-172` 的 for-loop 顶部加 5–10 行解析逻辑，meta 构建复用现有路径。**不**改 schema、不改哈希链算法。

#### 3.2.3 测试：`tests/test_local_ref_resolution.py`

| 用例 | 期望 |
| --- | --- |
| happy：批内 2 条事件，第 2 条 `parent_local_id=0` | 写入成功；DB 中第 2 条 `parent_event_id` = 第 1 条 `seq` |
| happy：4 通道 + action.intent + action.cancelled 同批，cancelled `source_event_id` 直填真实历史 seq | 写入成功；4 通道 + action.intent 同源链经 local_id 解析；cancelled 引用走真实 seq 路径 |
| error：`parent_local_id=99` 在批内不存在 | 抛 `unknown local_id` |
| error：`parent_local_id=0` 但事件 0 在事件 1 之后（即事件 1 用 `parent_local_id=0` 但事件 0 还没列入） | 抛 `forward reference not allowed` |
| error：同时填 `parent_event_id=5` 与 `parent_local_id=0` | 抛 `conflicting parent reference` |
| error：`parent_event_id` 引用了不存在的真实历史 seq（已有的 `_check_refs` 路径） | 抛 `parent_event_id=X does not exist`（既有错误信息保持不变） |

### 3.3 sessions：game_roster 表 + roster_guard（surgical 改 T2）

#### 3.3.1 schema 增量

文件位置：扩展 `brain/sessions/manager.py` 的 `init_db`（如已有），或新增 `brain/sessions/schema.sql` + 在 `manager.py` 的 module-level import 时执行。

```sql
CREATE TABLE IF NOT EXISTS game_roster (
    game_id      TEXT NOT NULL,
    actor_id     TEXT NOT NULL,
    metadata     TEXT NOT NULL,        -- canonical JSON
    registered_at TEXT NOT NULL,       -- ISO 8601 UTC
    PRIMARY KEY (game_id, actor_id),
    FOREIGN KEY (game_id) REFERENCES sessions(game_id)
);
CREATE INDEX IF NOT EXISTS idx_game_roster_game ON game_roster(game_id);
```

#### 3.3.2 `register_roster` 扩展

`brain/sessions/roster.py` 现有逻辑：调 `is_actor_id_reusable` + `register_actor_creation`。本卡追加：成功后**同事务**追加写入 `game_roster` 行（每 actor 一行）。

```python
def register_roster(game_id: str, actors: list[ActorRegistration]) -> None:
    # 既有：跨局 actor_id 复用判定 + lifecycle 写入
    # 新增：写 game_roster
    store = get_store()
    with store._conn:
        store._conn.execute("BEGIN IMMEDIATE")
        for a in actors:
            store._conn.execute(
                "INSERT OR REPLACE INTO game_roster (game_id, actor_id, metadata, registered_at) "
                "VALUES (?, ?, ?, ?)",
                (game_id, a.actor_id, canonical(a.metadata), _utcnow()),
            )
```

#### 3.3.3 `is_roster_registered(game_id) -> bool`

新增模块函数，供 server middleware 调：

```python
def is_roster_registered(game_id: str) -> bool:
    store = get_store()
    row = store._conn.execute(
        "SELECT 1 FROM game_roster WHERE game_id=? LIMIT 1", (game_id,)
    ).fetchone()
    return row is not None
```

### 3.4 orchestration：协议层主链（`brain/orchestration/`）

> **目录命名说明**：本卡新建 `brain/orchestration/`，与 T1 真相源 `BrainService/protocol/`（wire-protocol 文档）字面隔离，避免 import 路径混淆。

#### 3.4.1 `ontology.py`：action ontology v1 词汇表

```python
# brain/orchestration/ontology.py
ACTION_ONTOLOGY: dict[str, dict[str, dict]] = {
    "1": {
        "move_to":  {"required_one_of": [["target_npc"], ["target_zone"]], "optional": ["coords"]},
        "sit":      {"required": ["target_smartobject"], "optional": []},
        "wait":     {"required": [], "optional": ["reason"]},
    },
    # 后续如确有扩展动作需求，再新增下一版 ontology。
}
CURRENT_ONTOLOGY_VERSION = "1"
```

#### 3.4.2 `validator.py`：§5.2.1 9 条硬规则

`validate_reasoner_output(parsed: dict, *, action_ontology_version: str = "1") -> ValidationResult`：

```python
@dataclass
class ValidationResult:
    ok: bool
    failure_reasons: list[str]   # ok=False 时非空；ok=True 时为空列表
```

9 条规则全实现，每条独立 helper 函数：

| # | 规则 | 失败信息 |
| --- | --- | --- |
| 1 | 顶层必须且只能含 `scratchpad` / `intended` / `bid` / `note_to_self` | `"top-level must be exactly {scratchpad, intended, bid, note_to_self}, got {...}"` |
| 2 | `bid.urgency_level ∈ {pass, low, medium, high, urgent, critical}` | `"bid.urgency_level not in closed enum, got X"` |
| 3 | 禁数值 `urgency_score` / `urgency`：递归扫 parsed，任何 path 出现这两个 key → 拒（§3.2 反模式表 + §5.1.2） | `"forbidden numeric urgency field at path P"` |
| 4 | `intended.intended_action` 存在时必须命中 `ACTION_ONTOLOGY[v]`：`intent` 字符串、`params` dict 满足 required / required_one_of | `"unknown intent X"` / `"missing required param Y for intent X"` |
| 5 | 引用字段格式合法：`addressed_to_hint: list[str]`、`proposed_target: str \| null`、`relates_to_seq: int \| null` | `"addressed_to_hint must be list of strings"` 等 |
| 6 | 禁 `self`：递归扫 parsed 里所有字符串字段，命中 `self` → 拒（这层与 T2 写入闸口重复，但 Reasoner 输出层先拒能省一次写入尝试） | `"forbidden 'self' literal at path P"` |
| 7 | 字段类型与长度（`scratchpad.text`/`intended.text`/`note_to_self.text` 长度 ≤ 4000；`bid.rationale` 长度 ≤ 1000；具体常量在 `validator.py` 顶部定义） | `"field X exceeds max length"` |
| 8 | canonical JSON 可稳定序列化（用 T2 `canonical()` 算 hash 比对；任何无法序列化的对象 → 拒） | `"value not canonical-serializable"` |
| 9 | 兜底：以上 1–8 任一未命中即视为 validation failed（无单独失败信息，由具体规则产出） | — |

**测试覆盖**（`tests/test_validator.py`）：每条规则各 1 reject + 至少 1 happy path（共 ≥ 18 cases）。

#### 3.4.3 `reasoner.py`：调用 + 重试

```python
@dataclass
class ReasonerOutcome:
    terminal_outcome: Literal["validated_output", "validation_failed", "agent_timeout"]
    parsed: dict | None             # 仅 validated_output 时非 None
    raw_llm_output: str             # 永远保留
    failure_reason: str | None      # 失败时填具体原因，validated 时 None
    elapsed_ms: int

def call_reasoner(
    provider: Provider,
    messages: list[dict],
    *,
    action_ontology_version: str = "1",
    max_validation_retries: int = 3,
    max_timeout_retries: int = 3,
) -> ReasonerOutcome:
    """memory_principles §5.2.3 + §6.3 重试规则。"""
```

主体逻辑：

1. for `i in range(max_timeout_retries)`：调 provider；遇 `TimeoutError`/`httpx.TimeoutException` → 指数退避后重试；遇其他 5xx 也重试；仍失败 → return `ReasonerOutcome(terminal_outcome="agent_timeout", ...)`。
2. provider 返回后调 `validator.validate_reasoner_output`；若失败：reject sample 重试 reasoner 最多 `max_validation_retries=3` 次；3 次后 return `validation_failed`。
3. 任何成功路径都填 `raw_llm_output = ProviderResult.raw_text`；失败路径填**最后一次**尝试的 raw_text。

**测试**（`tests/test_reasoner.py`）：

- happy 直通；
- 1 次 validation_failed → 第 2 次 happy（FakeProvider scripted）；
- 3 次 validation_failed → terminal validation_failed，raw_llm_output 是第 3 次的；
- 1 次 timeout → 第 2 次 happy；
- 3 次 timeout → terminal agent_timeout。

#### 3.4.4 `derivation.py`：四通道 → 事件派生

##### 3.4.4.1 派生事件落库 actor / 条件字段表

按计划文件 §B.4 表填充每条派生事件的 `actor` / `output_contract_version` / `validator_version` / `raw_llm_output` / `source_event_id`（详见计划文件，本卡不重复）：

- 4 通道事件 actor = `<actor_id>`；output_contract_version = "1"；validator_version = "1"；raw_llm_output 必填；
- `system.validation_failed` / `system.agent_timeout` actor = `system`；raw_llm_output 必填（timeout 可空）；
- `reflection` actor = `<actor_id>`；
- `summary.phase` actor = `system`；
- `annotation.speech_act` actor = `system`（**不能用 `judge`，硬约束 5 拒**）；
- `annotation.listener_filter` actor = `orchestrator`；
- `speech.public` actor = `<actor_id>`（即源 `speech.intended.actor`，不是 orchestrator）；
- `action.intent` / `action.cancelled` / `orchestrator.tick_resolved*` actor = `orchestrator`。

##### 3.4.4.2 关键 visibility 约束

- `action.intent` / `action.cancelled` 写入时 `visibility=[actor_id, "system"]`，覆盖 §4.1.5 默认 `[<actor_id>]`（接受协作者审查 #3）。
- `speech.public` 默认 `["public"]` 不变。

##### 3.4.4.3 单 agent 派生函数

```python
def derive_validated_agent_events(
    actor_id: str,
    outcome: ReasonerOutcome,   # terminal_outcome=validated_output
    *,
    round_no: int,
    phase: str,
    pending_action: dict | None,  # 若 actor 有未完成 action.intent，传旧 intent 事件
    wall_clock_ts: str | None = None,
) -> list[dict]:
    """返回 EventInput 列表（带 _local_id），调用方一次 AppendEventsAtomically 写入。

    返回顺序保证：
      0: speech.scratchpad      _local_id=0
      1: speech.intended         _local_id=1
      2: speech.bid              _local_id=2  parent_local_id=1
      3: speech.note             _local_id=3  parent_local_id=1
      4: action.cancelled (可选) _local_id=4  source_event_id=旧 intent 真实 seq
      5: action.intent  (可选)   _local_id=5  parent_local_id=1, source_local_id=1
    """
```

scratchpad / bid / note 都同源指向 speech.intended（local_id=1）。action.intent 也同源 + source 都指 speech.intended。action.cancelled 走真实历史 seq。

##### 3.4.4.4 失败路径派生

```python
def derive_failed_agent_event(
    actor_id: str,
    outcome: ReasonerOutcome,   # terminal_outcome ∈ {validation_failed, agent_timeout}
    *, round_no: int, phase: str, wall_clock_ts: str | None = None,
) -> list[dict]:
    """单条 system.validation_failed 或 system.agent_timeout，actor=system。"""
```

##### 3.4.4.5 测试 `tests/test_derivation.py`

- validated_output 派生 → 4 条事件 + 同源链正确；
- validated_output 含 intended_action 且无旧 intent → +1 条 action.intent，5 条事件；
- validated_output 含 intended_action 且**有**旧 intent → +1 条 action.cancelled + 1 条 action.intent，6 条事件；
- validated_output 不含 intended_action → 仅 4 条；
- validation_failed → 1 条 system.validation_failed actor=system；
- agent_timeout → 1 条 system.agent_timeout actor=system。

#### 3.4.5 `floor.py`：Bid + floor control（§5.3）

```python
@dataclass
class FloorDecision:
    winner_actor: str | None         # None 表示冷场
    cold: bool
    bid_scores: dict[str, float]      # 排序后的 actor → urgency_score+bid_offset
    runners_up: list[str]              # 第 2、3、... 名（actor_id 字典序解平分）

def decide_floor(
    bid_events: list[dict],          # speech.bid 事件列表
    *,
    prev_floor_winners: list[str],    # 最近 N 拍的 winner，按拍序倒序
    cold_threshold: float = 3.0,
    consec_winner_window: int = 3,    # 连胜 N 拍后扣分
) -> FloorDecision: ...
```

- urgency_level → score 映射常量在 floor.py 顶部 `URGENCY_SCORE_MAP`（pass=0/low=2/medium=4/high=6/urgent=8/critical=10）。
- 被 @ +2.0：扫 bid_events 中 `proposed_target` 字段，若某 actor 是被 @ 对象 → 该 actor bid_offset += 2.0。
- 连续抢话上限：`prev_floor_winners[:consec_winner_window]` 全部相同 actor → 该 actor bid_offset -= 1.5。
- 冷场：max(score+offset) < cold_threshold → winner_actor=None。
- 平分：score+offset 相等时按 `actor_id` 字典序裁决（确定性）。

测试 `tests/test_floor.py`：每条规则单独 case + 1 个综合 case。

#### 3.4.6 `listener.py`：Listener-as-filter（§5.5）

##### 3.4.6.1 数据结构

```python
@dataclass
class FilterDecision:
    action: Literal["pass", "rewrite", "accept_with_violation"]
    final_text: str                  # 最终落 speech.public 的文本
    listener_filter_score: float
    subscores: dict[str, float]       # role_leak / belief_leak / strategy_leak
    rationale: str
    raw_filter_llm_output: str
```

##### 3.4.6.2 主函数

```python
def filter_intended(
    provider: Provider,
    intended_text: str,
    *,
    agent_role_metadata: dict,
    threshold: float = 0.4,
) -> FilterDecision:
    """memory_principles §5.5。MVP 不做 Reasoner retry。

    规则：
      - 调 filter LLM 出三项 + 综合分 + 改写建议
      - score ≤ threshold → action=pass，final_text=intended_text
      - score > threshold → action=rewrite，final_text=filter LLM 给的改写
      - filter LLM 拒绝改写或返回空字符串 → action=accept_with_violation，final_text=intended_text
    """
```

##### 3.4.6.3 测试 `tests/test_listener.py`

- score 0.2 → pass；
- score 0.7 + filter 给改写 → rewrite；
- score 0.7 + filter 拒改写 → accept_with_violation。

#### 3.4.7 `orchestrator.py`：per-tick 主循环

```python
@dataclass
class AgentSpec:
    actor_id: str
    provider: Provider               # 该 agent 用的 reasoner provider（每个 agent 独立 provider 实例）
    role_metadata: dict              # 喂给 listener 的 agent 角色标签

@dataclass
class TickResult:
    floor_winner: str | None
    cold: bool
    derived_seqs: dict[str, list[int]]   # actor_id → 该 agent 派生事件的 seq 列表
    public_seq: int | None                # 若有 winner 且写了 speech.public，记 seq
    tick_resolved_seq: int

def run_tick(
    game_id: str,
    agents: list[AgentSpec],
    *,
    round_no: int,
    phase: str,
    listener_provider: Provider,
    prev_floor_winners: list[str],
) -> TickResult: ...
```

主循环（按计划文件 §B.7 步骤）：

1. 用 `concurrent.futures.ThreadPoolExecutor`（或 asyncio + to_thread）并行 `call_reasoner`；每 agent 一个 future；超时由 `call_reasoner` 内部 max_timeout_retries 处理。
2. 收集每 agent 的 `ReasonerOutcome`。
3. 对每 agent 调 `compute_pending_actions(iter_all_events(game_id), viewer=actor_id)` 取该 actor 当前未完成 intent（最多 1 条，由 §4.2 投影定义）。
4. 调 `derive_validated_agent_events` 或 `derive_failed_agent_event` 生成本 agent 本拍 EventInput 列表。
5. **单事务**：`AppendEventsAtomically(game_id, all_events_for_all_agents)` 一次写入所有 agent 的 4 通道 + 可选 action.intent + 可选 action.cancelled + system.* 失败事件。**这是一个超大原子批次**，整拍要么全成要么全败。
6. 从写入结果取 speech.bid 的真实 seq → 用 `query_events(game_id, viewer="orchestrator", event_type="speech.bid", since_seq=...)` 拉本拍 bid 事件 → `decide_floor`。
7. 若 winner != None：
   - 调 `query_events` 拉 winner 的 speech.intended 真实 seq；
   - `filter_intended(listener_provider, intended_text, ...)` → FilterDecision；
   - 派生 `speech.public`（actor=winner，parent_event_id=winner.intended.seq，visibility=["public"]，payload.text=final_text，**不**含 raw_llm_output / output_contract_version——非 Reasoner-derived）+ `annotation.listener_filter`（actor=orchestrator，parent_local_id 指向 speech.public，payload 含 score/subscores/action/source_intended_seq/rationale，raw_llm_output 必填）；
   - 一次 `AppendEventsAtomically` 写两条。
8. 派生 `orchestrator.tick_resolved`（visibility=["public"]，payload 含 winner_actor / cold / public_seq，**无 bid 明细**）+ `orchestrator.tick_resolved.audit`（visibility=["orchestrator","system"]，payload 含 bid_scores 全表 + runners_up）；一次写两条。
9. 返回 TickResult。

**关键**：禁止用 `iter_all_events()` 走 server 路由（§3.6 中间件）；orchestrator 内部读 pending_actions 走的是投影路径，投影内部用 iter_all_events 是 T2 已有约定（投影是受控全量读，server 路由不允许）。

#### 3.4.8 `reflection.py`：三级反思

##### 3.4.8.1 round-level（每轮结束每 agent）

```python
def run_round_reflection(
    game_id: str, actor_id: str, round_no: int,
    *, provider: Provider,
) -> int:  # 返回派生 reflection 事件的 seq
    """memory_principles §5.7.1 9 问。"""
```

调用 prompt_assembly 拼装 prompt（含 §4.3 段 1+2+3+4+5+6+7+8+9 + 当前轮 round_no/phase），用 `reflection_v1.schema.json` schema 校验 9 问齐全（q1_situation / q2_identity / q3_public_info / q4_floor_winners_speech / q5_suspicion / q6_trust / q7_exposure / q8_strategy / q9_intended_speech）；写一条 `reflection` 事件，actor=actor_id，visibility=[actor_id]。

##### 3.4.8.2 phase-level（每阶段结束）

```python
def run_phase_summary(
    game_id: str, phase: str,
    *, judge_provider: Provider,
) -> int: ...
```

调裁判 LLM（同 Provider 抽象，不同 system prompt 强调"非参赛中立摘要"）出 200–300 词三段摘要（事件清单 / 自方策略 / 对手新信息）；长度强制校验，越界则重试 1 次后截断到上界。写一条 `summary.phase`，actor=`system`（非 `judge`），visibility 由具体游戏规则决定，MVP 默认 `["orchestrator", "system"]`（保守）。

##### 3.4.8.3 `judge_worker.py`：异步标注 worker

```python
class JudgeWorker:
    def __init__(self, judge_provider: Provider, *, poll_interval_ms: int = 200):
        self._semaphore = asyncio.Semaphore(2)   # 并发上限 2
        self._high_water: dict[str, int] = {}     # game_id → last_seen_speech_public_seq

    async def run(self): ...
    async def _annotate_one(self, game_id: str, public_event: dict): ...
```

主循环：

1. for game_id in active_games：`query_events(game_id, viewer="system", event_type="speech.public", since_seq=high_water[game_id])` 拿新 public。
2. 对每条新 public：先 `query_events(game_id, viewer="system", event_type="annotation.speech_act")` 过滤 `parent_event_id == public.seq` 命中的标注 → 命中跳过（防重启重复标注）；未命中调 `_annotate_one`（在 semaphore 下调 LLM）；写入 `annotation.speech_act` 事件，actor=`system`。
3. 更新 high_water。
4. `await asyncio.sleep(poll_interval_ms / 1000)`。

`schemas/annotation_speech_act_v1.schema.json` 定义 payload 形态：`speech_act_type ∈ {accuse, claim, deny, vote_call, support, suspect, other}`、`confidence: float 0..1`、`rationale: str`。

##### 3.4.8.4 测试 `tests/test_reflection.py` / `tests/test_judge_worker.py`

- round_reflection happy（FakeProvider 出齐 9 问 → 写入成功）；
- round_reflection 缺 q5 → 校验失败抛错；
- phase_summary 长度 250 → ok；长度 100 → 重试一次后仍短 → 截断到 200（或文档化为抛错——MVP 选截断到下界）；
- judge_worker：3 个 speech.public + 重启 worker → 仍只有 3 条 annotation.speech_act（不重复）；
- judge_worker：semaphore 限制下 4 条 public 串行（断言 LLM 调用最大并发 2）。

#### 3.4.9 `prompt_assembly.py`：§4.3 优先级表

```python
class PromptBudgetExceeded(Exception): ...

def assemble_prompt(
    game_id: str, actor_id: str,
    *, round_no: int, phase: str,
    token_budget: int,
    encoder_name: str = "o200k_base",
) -> list[dict]:
    """返回 messages list（OpenAI Chat Completion 风格）。

    必保留段（按 §4.3 优先级 1–9）：
      1. 系统 prompt + 规则 + 角色卡（代码模板）
      2. 不变量提醒（代码模板）
      3. 自我发言全量原文（query_events actor=actor_id event_type ∈ {speech.public, private_chat}）
      4. 最近未说出口的话（list_my_pending_intended）
      5. 自己的便条历史（my_recent_notes，n=large）
      6. 自我承诺投影（list_my_commitments）
      7. 公开发言近场窗口（quote_by_round 最近 K 轮）
      8. 私聊 / 夜间私密事件（query_events viewer=actor_id event_type=private_chat）
      9. 当前拍提示 + 工具定义（代码模板）

    可裁剪段：
      C1. 自己最近 N 轮 reflection（my_recent_reflections）

    可压缩段：
      P1. 公开早期摘要（query_events event_type=summary.phase visibility=public）
      P2. 对手画像（暂略，留位）

    若必保留段总 token > token_budget → 抛 PromptBudgetExceeded。
    可裁剪段先减 N（reflection 历史从 N 减到 0），再丢弃可压缩段。
    """
```

token 估算：`tiktoken.get_encoding(encoder_name).encode(text)` 长度。每 message dict 估 `overhead=4 tokens` 加上 content。

测试 `tests/test_prompt_assembly.py`：

- happy：必保留段全在 + 可裁剪段全在；
- token_budget 紧到只放下必保留段 → 可裁剪段被去掉；
- token_budget 紧到必保留段也放不下 → 抛 PromptBudgetExceeded；
- 自我发言段返回原文级（断言 query_events 实际命中数与 prompt 中行数一致）。

### 3.5 HTTP server（FastAPI，`brain/server/`）

#### 3.5.1 `app.py` + `__main__.py`

```python
# brain/server/app.py
from fastapi import FastAPI
from contextlib import asynccontextmanager

@asynccontextmanager
async def lifespan(app: FastAPI):
    # 启动：init_db / 注册 provider 工厂 / 启 judge_worker（按 BRAIN_JUDGE_WORKER=1 时）
    yield
    # 关闭：取消 judge_worker / close httpx clients

def create_app() -> FastAPI:
    app = FastAPI(title="BrainService", version=PROTOCOL_VERSION, lifespan=lifespan)
    app.add_exception_handler(...)         # error_handler
    app.include_router(health.router)
    app.include_router(sessions.router, prefix="/v1")
    # ... 其余 routes
    return app

app = create_app()
```

```python
# brain/server/__main__.py
import uvicorn
from .app import app
from ..config import BRAIN_HOST, BRAIN_PORT
if __name__ == "__main__":
    uvicorn.run(app, host=BRAIN_HOST, port=BRAIN_PORT)
```

#### 3.5.2 配置升级 `brain/config.py`

新增 env 读取：

```python
import os
BRAIN_API_TOKEN     = os.getenv("BRAIN_API_TOKEN", "dev_token")
BRAIN_HOST           = os.getenv("BRAIN_HOST", "127.0.0.1")
BRAIN_PORT           = int(os.getenv("BRAIN_PORT", "8000"))
LLM_PROVIDER         = os.getenv("LLM_PROVIDER", "fake")
DEEPSEEK_API_KEY     = os.getenv("DEEPSEEK_API_KEY", "")
DEEPSEEK_BASE_URL    = os.getenv("DEEPSEEK_BASE_URL", "https://api.deepseek.com")
DEEPSEEK_MODEL       = os.getenv("DEEPSEEK_MODEL", "deepseek-chat")
BRAIN_JUDGE_WORKER   = os.getenv("BRAIN_JUDGE_WORKER", "0") == "1"   # MVP 默认关
```

#### 3.5.3 中间件（`brain/server/middleware/`）

| 文件 | 职责 |
| --- | --- |
| `auth.py` | FastAPI dependency：检查 `Authorization: Bearer <token>` 与 `BRAIN_API_TOKEN` 一致；不一致返 401；`/health` 不挂此 dependency |
| `idempotency.py` | 进程内 dict `_cache: dict[(str, str), CachedResponse]`（key = (game_id, idempotency_key)）；middleware 拦截写入型 endpoint：若请求带 `Idempotency-Key` 头且 cache 命中 → 复刻首次响应；未命中 → 放行下游 + 记录响应 |
| `roster_guard.py` | FastAPI dependency：对写入型 endpoint（world_state / actions/result / speech/result / ingress_reject）调 `is_roster_registered(game_id)`；False → 返 409 + `error_code=ROSTER_NOT_REGISTERED` |
| `error_handler.py` | 全局 exception handler：`EventValidationError` → 422 + `error_response.schema.json` 形态；`InvalidViewer` / `ActorIdNotReusable` 类似；FK 引用错误 → 422 |

`CachedResponse` 形态：

```python
@dataclass
class CachedResponse:
    status_code: int
    body: bytes
    headers: dict[str, str]   # 仅缓存关键头：Content-Type / Idempotency-Key-Replay
```

idempotency middleware 重发响应时附加 header `Idempotency-Key-Replay: true`，便于测试断言 + 调试。

#### 3.5.4 路由（`brain/server/routes/`）

| 文件 | endpoint | 实现要点 |
| --- | --- | --- |
| `health.py` | `GET /health` | 无认证；返 `{"protocol_version": PROTOCOL_VERSION, "ok": true, "server_time": ...}` |
| `sessions.py` | `POST /v1/games` | 调 `sessions.manager.create_session` → 返 `{"game_id", "protocol_version", "server_time"}` |
| `roster.py` | `POST /v1/games/{game_id}/roster` | 调 `sessions.roster.register_roster`；含 idempotency；不含 roster_guard（这是 roster 注册本身） |
| `world_state.py` | `POST /v1/games/{game_id}/world_state` | 调 `world_state.ingest.ingest_world_state`；含 idempotency + roster_guard |
| `actions.py` | `GET /v1/games/{game_id}/actions/pull?since_seq=N` | 用 `query_events(viewer="system", event_type=["action.intent","action.cancelled"], since_seq=N)` |
|  | `POST /v1/games/{game_id}/actions/result` | 写入 `action.resolved`（actor=`system`，由 UE 上报真实结果，visibility 由感知系统填，MVP 默认 `["orchestrator","system"]`，**not public**）；含 idempotency + roster_guard |
| `speech.py` | `GET /v1/games/{game_id}/speech/pull?since_seq=N` | 用 `query_events(viewer="system", event_type="speech.public", since_seq=N)` |
|  | `POST /v1/games/{game_id}/speech/result` | 写入 `speech.playback_resolved`（actor=`system`，visibility=`["orchestrator","system"]`）；含 idempotency + roster_guard |
| `ingress_reject.py` | `POST /v1/games/{game_id}/ingress_reject` | 写入 `system.ingress_rejected`（actor=`system`，visibility=`["orchestrator","system"]`）；含 idempotency + roster_guard |
| `events.py` | `GET /v1/games/{game_id}/events?since_seq=N` | MVP 固定 `viewer="system"`；返 `query_events` 结果 + `next_cursor`；不含 roster_guard |

**所有路由用 `await asyncio.to_thread(...)` 包同步 EventStore 调用**。

### 3.6 测试套件（pytest）

#### 3.6.1 单元测试（每模块独立）

| 文件 | 覆盖 |
| --- | --- |
| `tests/test_llm_provider.py` | FakeProvider 弹出 sequence；FakeProvider raw_text canonical 序列化；DeepSeekProvider mock httpx → 5xx 重试 / 4xx 不重试 / 解析 JSON |
| `tests/test_validator.py` | §5.2.1 9 条规则各 ≥ 1 reject + 1 happy（≥ 18 cases） |
| `tests/test_reasoner.py` | happy / 1 retry → ok / 3 retries → fail / 1 timeout → ok / 3 timeouts → fail |
| `tests/test_derivation.py` | validated_output 4/5/6 条派生路径 + failed/timeout 1 条派生路径 + actor 字段表全验证 |
| `tests/test_local_ref_resolution.py` | 见 §3.2.3 |
| `tests/test_floor.py` | urgency 映射 / 被@加分 / 连胜降权 / 平分字典序 / 冷场 |
| `tests/test_listener.py` | pass / rewrite / accept_with_violation 三动作 + annotation.listener_filter 落地 |
| `tests/test_orchestrator_e2e.py` | **硬验收主测**（详见 §6） |
| `tests/test_reflection.py` | round 9 问 happy / 缺 q5 拒；phase 长度 250 ok / 100 截断 |
| `tests/test_judge_worker.py` | 重启不重复标注 / Semaphore(2) 并发上限 |
| `tests/test_prompt_assembly.py` | 必保留段全在 / 可裁剪段被裁 / 必保留超 token 抛错 / 自我发言指针级 |

#### 3.6.2 server 测试（FastAPI TestClient）

| 文件 | 覆盖 |
| --- | --- |
| `tests/test_server_health.py` | `/health` 无认证 + 返 protocol_version |
| `tests/test_server_sessions.py` | `POST /v1/games` 返 uuid v4 game_id |
| `tests/test_server_roster.py` | happy + 复用 actor_id 拒 + idempotency 重发 |
| `tests/test_server_world_state.py` | happy + roster_guard 未注册返 409 + idempotency |
| `tests/test_server_actions.py` | pull 返 visibility 含 system 的 action.intent / cancelled + result POST 写 action.resolved |
| `tests/test_server_speech.py` | pull 返 speech.public + result POST 写 speech.playback_resolved |
| `tests/test_server_ingress.py` | POST 写 system.ingress_rejected |
| `tests/test_server_events.py` | MVP viewer=system 行粒度过滤 |
| `tests/test_server_idempotency.py` | 同 key 重发 = 完整复刻首次（status + body + Idempotency-Key-Replay 头） |
| `tests/test_server_roster_guard.py` | 所有受保护 endpoint 未注册返 ROSTER_NOT_REGISTERED |
| `tests/test_examples_round_trip.py` | 把 `protocol/examples/*.json` 全部喂进对应 endpoint，断言成功响应符合对应 schema、失败响应符合 error_response.schema.json |

### 3.7 文档

- `BrainService/README.md` 追加：
  - 起 server：`$env:BRAIN_API_TOKEN = "..."` + `$env:LLM_PROVIDER = "fake"` + `python -m brain.server`
  - 切 provider：`LLM_PROVIDER=deepseek` + `DEEPSEEK_API_KEY=...`
  - 跑 e2e：`pytest tests/test_orchestrator_e2e.py -v`

- `BrainService/protocol/protocol.md`：本卡如发现 T1 协议有歧义，按 patch 升 0.1.1 → 0.1.2 并同步 `Docs/protocol_pointer.md`；如无歧义则不升版本。

---

## 4. 范围外（DON'T）

- ❌ 不实现 Listener 的 Reasoner retry 路径（MVP 简化为 0 retries，详见 §未实现协议 UN-1）。
- ❌ 不实现 brain 进程崩溃后的 session 失效语义（§未实现协议 UN-2）。
- ❌ 不实现可压缩段 LLM 摘要降级路径（§4.3 表 P1/P2，留位但 MVP 不实现）。
- ❌ 不引入 strict json_schema response_format（DeepSeek beta tool-call adapter 不在本卡）。
- ❌ 不实现机制阶段 phase-specific schema（vote / night_action / reveal）—— 留 T9。
- ❌ 不预留物品、跟随、逃离等非 MVP intent；后续确有需求时再按协议升版新增。
- ❌ 不实现 02 病毒游戏专属规则—— 留 T9。
- ❌ 不动 UE `Source/` / `Config/` / 资产；只新增 `Docs/Roadmap/T03-5_*.md` 路线图文档。
- ❌ 不实现持久化 idempotency 缓存或 brain 重启后的 session 恢复。
- ❌ 不引入向量召回 / fact triple 抽取 / 自动 summary 覆盖原文（§三反模式表）。
- ❌ 不引入 alembic 等 migration 工具；schema.sql 用 `CREATE ... IF NOT EXISTS` 幂等。

---

## 5. 交付清单

| 路径 | 类型 | 一句话职责 |
| --- | --- | --- |
| `BrainService/brain/llm/__init__.py` | 新建 | `make_provider(name, **cfg)` 工厂 |
| `BrainService/brain/llm/provider.py` | 新建 | `Provider` 抽象 + `ProviderResult` |
| `BrainService/brain/llm/fake.py` | 新建 | `FakeProvider`，scripted dict 用 canonical 序列化 raw_text |
| `BrainService/brain/llm/deepseek.py` | 新建 | DeepSeek httpx 客户端，json_object response_format + 5xx 退避 |
| `BrainService/brain/orchestration/__init__.py` | 新建 | 包入口 |
| `BrainService/brain/orchestration/ontology.py` | 新建 | ACTION_ONTOLOGY v1 + 版本常量 |
| `BrainService/brain/orchestration/validator.py` | 新建 | §5.2.1 9 条规则 |
| `BrainService/brain/orchestration/reasoner.py` | 新建 | call_reasoner 重试逻辑 |
| `BrainService/brain/orchestration/derivation.py` | 新建 | 4 通道 + action.intent + action.cancelled + system.* 派生 |
| `BrainService/brain/orchestration/floor.py` | 新建 | bid 裁决 + 冷场 + 被@加分 + 连胜降权 |
| `BrainService/brain/orchestration/listener.py` | 新建 | filter_intended（pass/rewrite/accept_with_violation） |
| `BrainService/brain/orchestration/orchestrator.py` | 新建 | run_tick 主循环（含旧 action 取消）|
| `BrainService/brain/orchestration/reflection.py` | 新建 | round + phase 反思 |
| `BrainService/brain/orchestration/judge_worker.py` | 新建 | 异步 annotation.speech_act 标注 + Semaphore(2) |
| `BrainService/brain/orchestration/prompt_assembly.py` | 新建 | §4.3 优先级 + tiktoken + PromptBudgetExceeded |
| `BrainService/brain/orchestration/schemas/reasoner_v1.schema.json` | 新建 | 四通道 JSON Schema（用于校验 + 可选传给 provider）|
| `BrainService/brain/orchestration/schemas/reflection_v1.schema.json` | 新建 | 9 问 JSON Schema |
| `BrainService/brain/orchestration/schemas/annotation_speech_act_v1.schema.json` | 新建 | speech_act_type / confidence / rationale |
| `BrainService/brain/orchestration/schemas/listener_filter_v1.schema.json` | 新建 | 三项 + 综合分 + 改写 + 理由 |
| `BrainService/brain/server/__init__.py` | 新建 | 包入口 |
| `BrainService/brain/server/app.py` | 新建 | FastAPI app + lifespan |
| `BrainService/brain/server/__main__.py` | 新建 | `python -m brain.server` 启 uvicorn |
| `BrainService/brain/server/middleware/auth.py` | 新建 | Bearer token 校验 |
| `BrainService/brain/server/middleware/idempotency.py` | 新建 | 进程内幂等响应缓存 |
| `BrainService/brain/server/middleware/roster_guard.py` | 新建 | game_roster 已注册校验 |
| `BrainService/brain/server/middleware/error_handler.py` | 新建 | 异常 → error_response.schema.json |
| `BrainService/brain/server/routes/health.py` | 新建 | `GET /health` |
| `BrainService/brain/server/routes/sessions.py` | 新建 | `POST /v1/games` |
| `BrainService/brain/server/routes/roster.py` | 新建 | `POST .../roster` |
| `BrainService/brain/server/routes/world_state.py` | 新建 | `POST .../world_state` |
| `BrainService/brain/server/routes/actions.py` | 新建 | `GET .../actions/pull` + `POST .../actions/result` |
| `BrainService/brain/server/routes/speech.py` | 新建 | `GET .../speech/pull` + `POST .../speech/result` |
| `BrainService/brain/server/routes/ingress_reject.py` | 新建 | `POST .../ingress_reject` |
| `BrainService/brain/server/routes/events.py` | 新建 | `GET .../events`（MVP viewer=system） |
| `BrainService/tests/test_llm_provider.py` 等 21 份 | 新建 | 见 §3.6 表 |
| `BrainService/brain/eventstore/store.py` | **改** | 增 `_local_id` / `parent_local_id` / `source_local_id` 解析（§3.2） |
| `BrainService/brain/sessions/roster.py` | **改** | 同事务追加写 `game_roster` + 暴露 `is_roster_registered` |
| `BrainService/brain/sessions/manager.py` | **改** | `init_db` 创建 `game_roster` 表 |
| `BrainService/brain/config.py` | **改** | 新增 server / LLM env 常量 |
| `BrainService/pyproject.toml` | **改** | runtime deps 加 `fastapi` `uvicorn[standard]` `httpx` `tiktoken` |
| `BrainService/README.md` | **改** | 起 server / 切 provider / 跑 e2e 测说明 |
| `BrainService/.gitignore` | **改** | 如未含 `.env` 则补上 |
| `Docs/protocol_pointer.md` | **改**（条件） | 仅当本卡发现 T1 协议歧义需 patch 升版本时 |

---

## 6. 验收（机器化优先）

完成时按顺序自检并给出勾选状态：

- [ ] `cd BrainService && pip install -e .[dev] && pytest -q` 全绿。
- [ ] `tests/test_local_ref_resolution.py` 覆盖：happy / unknown local_id / forward reference / 同时填两种引用 全部通过。
- [ ] `tests/test_validator.py` ≥ 18 cases 全绿，9 条规则每条至少 1 reject + 1 happy。
- [ ] `tests/test_reasoner.py` 覆盖 5 路径（happy / 1 retry → ok / 3 retries → fail / 1 timeout → ok / 3 timeouts → fail）全绿。
- [ ] `tests/test_derivation.py` actor / output_contract_version / validator_version / raw_llm_output / source_event_id 字段全验证。
- [ ] `tests/test_floor.py` 5 条规则全绿。
- [ ] `tests/test_listener.py` 3 动作全绿；annotation.listener_filter 落地字段齐全。
- [ ] `tests/test_orchestrator_e2e.py` 硬验收主测：4 FakeProvider × 5 拍跑完整链；事件流 `verify_chain(game_id)` 始终 True；故意 1 个 agent 第 3 拍 timeout、第 4 拍 validation_failed、整拍仍推进；至少 1 拍 listener `rewrite`；至少 1 拍冷场 winner=None；至少 1 拍新 intent 触发旧 intent cancel（事件流出现 `action.cancelled` + 新 `action.intent` 同 seq 区间）。
- [ ] `tests/test_reflection.py` round 9 问 + phase 长度规范测试通过。
- [ ] `tests/test_judge_worker.py` 重启不重复标注 + Semaphore(2) 并发上限。
- [ ] `tests/test_prompt_assembly.py` 必保留段超 token 抛 `PromptBudgetExceeded` + 自我发言段指针级断言通过。
- [ ] `tests/test_server_*.py` 全部 endpoint happy + 失败路径覆盖：缺 Bearer 401 / 未注册 roster 409 / `Idempotency-Key` 重发完整复刻首次响应（含 `Idempotency-Key-Replay: true` 头）。
- [ ] `tests/test_examples_round_trip.py` 把 `protocol/examples/*.json` 全部跑一遍：成功样例符合对应 endpoint schema、失败样例符合 `error_response.schema.json`。
- [ ] 手动启 server 验证：`$env:BRAIN_API_TOKEN = "test_token"; $env:LLM_PROVIDER = "fake"; python -m brain.server` 起得来；`curl /health` 返 protocol_version；`curl POST /v1/games` 返 uuid。
- [ ] `git status` 在 BrainService/ 干净（除 `.env` 与 `data/`）。
- [ ] **协议合规自检**：禁用 `iter_all_events()` 在 server 路由（grep 检查）；禁用 `actor=judge` / `actor=referee`（grep 检查）；禁用 `visibility=["self"]` / `addressed_to=["self"]`（grep 检查）。
- [ ] 由人类（不是 ClaudeCode）评审 README 起 server 指引可复现。

---

## 7. 上下游交接

**前置卡**：T1 ✅ + T2 ✅。本卡所有 endpoint 行为严格按 T1 schema；事件流写入严格走 T2 EventStore 入口（含 local_id 扩展）。

**下游消费者**：

| 后续卡 | 依赖本卡的什么 |
| --- | --- |
| T6 | 整个 `python -m brain.server` 进程作为 UE 端 HTTP polling 对接端；`POST /v1/games` 握手 + `/health` 协议版本握手；examples round-trip 测试 fixture |
| T7 | brain 派出的 `action.intent` / `action.cancelled` / `speech.public` 事件流；UE Dispatcher 通过 `/actions/pull` + `/speech/pull` 拉取；UE 端 result POST 走 `/actions/result` + `/speech/result` |
| T9 | 机制阶段 schema（vote / night_action / reveal）由 T9 在 `orchestration/schemas/` 新增；本卡 floor.py 留 hook（`phase ∈ {vote, night_action, reveal}` 时 skip floor control） |
| T10 | 真实 DeepSeek provider 联调；硬验收主测从 4 FakeProvider × 5 拍升级到 4 真实 LLM × 多拍 |

---

## 8. 风险与已知坑

- **EventStore local_id 扩展不破坏既有调用方**：T2 调用方（如 `world_state.ingest`）不传 `_local_id` / `parent_local_id` / `source_local_id`，行为完全不变。新字段全部 `default=None`，且 `parent_local_id` 与 `parent_event_id` 二选一的冲突检查仅在两者都非 None 时触发。
- **canonical JSON 与 wire JSON 区分**：FakeProvider 用 canonical 序列化 raw_text，与真实 provider HTTP 返回的字符串可能不字节级一致（但语义一致）；测试断言用 `parsed` 不用 `raw_text` 字面比对。
- **tiktoken 编码器偏差**：DeepSeek 实际 tokenizer 可能不是 o200k_base；MVP 估算偏差可达 ±20%。token_budget 实际配置时**留 30% 安全 buffer**（如 model 实际 64k → 配 45k）。
- **prompt budget 超限退化**：MVP 选抛 `PromptBudgetExceeded` → orchestrator 转写为 `system.validation_failed` 让该 agent 该拍 abstain（满足 §5.2.2 单 agent 失败不阻塞整拍）。这是工程退化（协议本意是直接报错；MVP 选 abstain 是合规子集）。
- **Listener MVP 不 retry**：协议 §5.5 + §6.3 允许 ≤2 retries；MVP 选 0 retries。详设卡 §未实现协议 UN-1 显式声明。
- **brain 进程崩溃 + UE 不崩**：MVP 进程内 idempotency dict 缓存清空，UE 重发同 key 会真正执行第二次。**仅在风险清单声明，不实现 brain 重启拒绝旧 session 的逻辑**。运维建议：brain 重启时人工通知 UE 重置 session。
- **judge_worker 持久化 checkpoint 不做**：MVP 用内存高水位 + 写入前 query `parent_event_id` 防重；重启代价 = 一次 full scan，可接受。
- **SQLite 串行化陷阱**（继承自 T2）：`BEGIN IMMEDIATE` 不保护应用层缓存；本卡的 `register_roster` 同事务写 game_roster 必须包在 store._lock 之外的 `_conn` 事务中（不与 events append 共用锁）。
- **FastAPI async + sync SQLite**：所有路由必须 `await asyncio.to_thread(...)` 包同步 EventStore 调用；直接打会阻塞 event loop。
- **DeepSeek json_object 模式不强制 schema**：模型可能返回不符合四通道 schema 的 JSON；本地 Validator 兜底 reject sample 重试。
- **判断 LLM 用同一 provider 抽象**：phase-level summary + annotation.speech_act 都用 `judge_provider`；MVP 与参赛 reasoner_provider 可同 DeepSeek 但**必须用不同 system prompt + 标识 model_hint='judge'**。
- **`speech.public` 不是 Reasoner-derived**：actor=源 NPC，但 output_contract_version / validator_version / raw_llm_output 都不填（§4.1.1.2）；这是 orchestrator 派生的"代发"事件。
- **action.cancelled.actor=`orchestrator`**：不是被取消 action 的原 actor；这条 cancelled 是 orchestrator 派生的系统决定，按 §5.4.2 规则。
- **Idempotency-Key 测试要覆盖响应头复刻**：测试不能只比 status + body；要断言 `Idempotency-Key-Replay: true` 头出现在重发响应里。
- **DeepSeek SDK 未来变化**：MVP 用 httpx 直调 REST API，不绑 `openai`-compatible SDK，避免双绑定。

---

## 9. 可选前置准备

- Python 版本：≥ 3.11（继承 T2，需要 `enum.StrEnum` + `asyncio.to_thread` + dataclass kwarg-only）。
- venv：`python -m venv .venv` + `pip install -e .[dev]`。
- env 配置（dev）：
  ```
  BRAIN_API_TOKEN=dev_token
  LLM_PROVIDER=fake
  BRAIN_HOST=127.0.0.1
  BRAIN_PORT=8000
  BRAIN_JUDGE_WORKER=0
  ```
  生产环境再加 `DEEPSEEK_API_KEY` + `LLM_PROVIDER=deepseek` + `BRAIN_JUDGE_WORKER=1`。
- 是否引入 alembic：MVP 不需要。
- 测试隔离：每个 test 用 `tmp_path / "test_eventstore.sqlite3"` 创建独立 db，避免 fixture 污染。
- pytest 并发：用 `pytest-xdist` 可加速但需要 fixture 严格独立——MVP 单线程 pytest 即可。
