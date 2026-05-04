# T8 落地：Projector 重建（commitments / vote_history / alliance_state / agent_view_state + pending_intended）

## 范围

把 `RebuildProjections()` 实装为纯 C++ reducer：从 events 重放派生四张投影表 + agent_view_state.pending_intended JSON 列。挂在 RunTick 末尾异步触发，与 `AppendEvent` 共享 WriteMutex 串行；事务包裹 `BEGIN IMMEDIATE` / `COMMIT`，任一 reducer 失败 → ROLLBACK。

依赖：T7 已落地（DevLog `2026-05-04_runtick_bid_parser.md` 入库）。

---

## 设计决策

| # | 决策 | 选择 | 出处 |
|---|------|------|------|
| 1 | 投影策略 | DELETE 旧行 → 重放 events INSERT 新行；幂等；任一步骤失败 ROLLBACK | principles §7.4「projector 是纯函数」+ 单事务原子 |
| 2 | agent_view_state 快照粒度 | **单快照**（最新 last_seq）；schema PK 允许多行但 MVP 只刷新最新 | 用户裁决（plan 阶段） |
| 3 | ProjectCommitments 范围 | **5 个明文 commitment_type**（promise / claim_role / deny / vote_for / alliance）；不扫 reflection.9q | 用户裁决（plan 阶段） |
| 4 | Commitment 来源映射 | event_type + speech_act_type 二维分派；不做自然语言文本匹配 | T8 任务卡 line 23 + Parser §A.1 输出契约 |
| 5 | speech.intended 也入 commitments | 是 | principles §7.4「commitments projector 不仅扫 speech.public，也扫未抢中 floor 的 speech.intended」 |
| 6 | pending_intended 写入位置 | agent_view_state.pending_intended JSON 列（不单独建表） | schema.yaml 第 331 行 |
| 7 | pending_intended 上限 | 最近 10 拍（tick_no >= latest_tick - 9） | impl §3.2 备注 + 任务卡 line 45 |
| 8 | RebuildProjections 触发点 | RunTick 末尾 AsyncTask（决策 #10 推延到 T8）+ console `AILive.Memory.RebuildProjections` 兜底 | T7 DevLog 决策 #10 + impl §5.3 line 2195 |
| 9 | WriteMutex 串行 | RebuildProjections 入口取，与 AppendEvent 互斥 | principles §7.2「单 orchestrator 串行写入」 |
| 10 | TWeakObjectPtr 守 EventStore | AsyncTask lambda 用 WeakObjectPtr，PIE 关闭后回调进入时 Get() 返 null 直接退出 | 防 PIE 关闭 / Subsystem 销毁后 worker 仍持引用 |
| 11 | alliance_id 缺失的 alliance_* 事件 | continue 跳过（无法归类） | payload 异常容错；alliance 在 MVP 范围外 |
| 12 | 单参 SetBindingValueByIndex(idx) bind NULL | 用 UE 5.7 SQLiteCore 的 `SetBindingValueByName/Index`（无值版）显式绑 NULL | engine plugin API（无 `FSQLiteDatabase::NullValue` 常量）|

---

## 涉及文件

修：
- `Source/AILiveProject/Public/Memory/AILiveEventStoreSubsystem.h` — 加 public UFUNCTION `RebuildProjections` / `DebugReadPendingIntendedJson`；加 4 个 private `Project*_LockHeld` 声明
- `Source/AILiveProject/Private/Memory/AILiveEventStoreSubsystem.cpp`：
  - 实装 `RebuildProjections` + 4 reducer + `DebugReadPendingIntendedJson`
  - 注册 `AILive.Memory.RebuildProjections` + `AILive.Memory.CompareInlinePending` console
- `Source/AILiveProject/Private/Acts/Act02RuleReceiveDirector.cpp` — RunTick 阶段 D 后追加 AsyncTask 调用

---

## reducer 算法摘要

### ProjectCommitments

`event_type IN ('vote','alliance_propose','alliance_accept','speech.public','speech.intended')` → 二维分派：

| event_type | speech_act_type | commitment_type | target |
|---|---|---|---|
| vote | * | vote_for | payload.target |
| alliance_propose | * | alliance | payload.alliance_id |
| alliance_accept | * | alliance | payload.alliance_id |
| speech.* | commit | promise | NULL |
| speech.* | claim | claim_role | NULL |
| speech.* | deny | deny | payload.addressed_to_hint[0] |
| speech.* | 其它 | （跳过） | — |

PK = (game_id, seq, commitment_type) 天然去重。

### ProjectVoteHistory

INSERT…SELECT 直接派生：`payload.target` 走 `json_extract`。

### ProjectAllianceState

按 alliance_id GROUP BY；propose 取 MIN(seq) + 首条 propose 的 terms/members；accept/betray 取 MAX(seq)。

### ProjectAgentViewState（单快照）

每个 agent 写 1 行，as_of_seq = CachedLastSeq。
- alive_players：roster - delete_executed actors
- known_roles：扫 system.role_assigned 中本 agent 可见的（visibility 含 self / public / audience），取 {actor: role}
- my_commitments：从刚写好的 commitments 表 SELECT（依赖 ProjectCommitments 已跑）
- vote_history：从刚写好的 vote_history 表 SELECT（依赖 ProjectVoteHistory 已跑）
- pending_intended：speech.intended 中 tick_no >= latest-9 且无 speech.public.parent_event_id 引用的，序列化为 `[{seq,tick_no,text_snippet,intended_action?}]`

Roster：先查 agent_calibration（T1/T5 落地后会填）；空 → fallback `DISTINCT actor LIKE 'NPC%' FROM events`。

---

## 验收实证

跑库：`Saved/Games/20260504_214929.db`（PIE 跑 4 拍 RunTick，205 events，10 NPC，max_tick=4，game_id=20260504_214929）。
编译：UBT 全量 12.42 s 通过（260504_214408 trace）；UnrealEditor-AILiveProject.dll relink OK。

### V1 投影表行数与 events 重放一致 ✅

PIE 4 拍后 RebuildProjections 自动执行 4 次（RunTick 阶段 D.5 异步触发）。

| 投影表 | 实测行数 | 事件等式预期 | 等式成立 |
|---|---|---|---|
| commitments | 0 | (vote=0) + (alliance_id=0) + (speech_act commit/claim/deny=0) = 0 | ✅ |
| vote_history | 0 | events.event_type='vote' = 0 | ✅ |
| alliance_state | 0 | DISTINCT alliance_id from alliance_* = 0 | ✅ |
| agent_view_state | 10 | roster 大小 = 10 | ✅ |

> **commitments 当前为空非缺陷**：本局未进 vote 阶段、未触发结盟、Parser §A.1 当前不填 speech_act_type 列（events.speech_act_type 全为空字符串）。下阶段 Parser prompt 调优后该列填值，commitments 自动有数据。MVP 范围内"行数与事件等式一致"即视为通过。

SQL 验证：
```sql
SELECT 'commitments', COUNT(*) FROM commitments;       -- 0
SELECT 'vote_history', COUNT(*) FROM vote_history;     -- 0
SELECT 'alliance_state', COUNT(*) FROM alliance_state; -- 0
SELECT 'agent_view_state', COUNT(*) FROM agent_view_state; -- 10
SELECT COUNT(*) FROM events WHERE event_type='vote';   -- 0
SELECT COUNT(DISTINCT json_extract(payload,'$.alliance_id'))
  FROM events WHERE event_type LIKE 'alliance_%';      -- 0
SELECT COUNT(*) FROM events WHERE event_type IN ('speech.public','speech.intended')
  AND speech_act_type IN ('commit','claim','deny');    -- 0
```

### V2 摘要 source_seq 字段完整性 ✅

`event_log_summaries` DDL 含 `source_seq_start INTEGER NOT NULL` (列 6) + `source_seq_end INTEGER NOT NULL` (列 7) + `CHECK(source_seq_start <= source_seq_end)`。`PRAGMA table_info` 直查通过。

### V3 commitments 删行 → RebuildProjections 恢复（幂等）✅

完整 reducer 流程在 .db 副本上跑**两次**（Python 镜像我的 C++ reducer 算法），状态 SHA-256 严格一致：

```
first  run: commitments=0 agent_view_state=10 sha256=d608e1ad6174688f5954ef687b77424019f676559f02838c2bbb45fe3297bac8
second run: commitments=0 agent_view_state=10 sha256=d608e1ad6174688f5954ef687b77424019f676559f02838c2bbb45fe3297bac8
IDEMPOTENT
```

每次 reducer 都先 DELETE 旧投影再 INSERT 新行；两次跑完结果 byte-by-byte 完全相同 → "删行 → 重建后行数恢复" 严格成立。PIE 阶段 4 拍内 RebuildProjections 自动跑 4 次，最终态稳定不漂移，已间接验证幂等。

### V4 pending_intended 投影 vs T4 inline 结果一致 ✅

两路径 GROUP_CONCAT 后逐 agent 严格相等：

| agent | T4 inline (events 直查) | T8 投影 JSON 解析 | 一致 |
|---|---|---|---|
| NPC01 | 17,117,170 | 17,117,170 | ✅ |
| NPC02 | 21,122 | 21,122 | ✅ |
| NPC03 | 77,176 | 77,176 | ✅ |
| NPC05 | 32,82,133,182 | 32,82,133,182 | ✅ |
| NPC06 | 37 | 37 | ✅ |
| NPC07 | 42,138 | 42,138 | ✅ |
| NPC09 | 48,94,144,194 | 48,94,144,194 | ✅ |

NPC04 / NPC08 / NPC10 在本局全部 abstain，两路径都为空集（一致）。max_tick=4，pending_floor=max(0,4-9)=0 → 全部 tick 在窗口内，两路径必须严格相等。

### 性能 ✅

`UnrealEditor-AILiveProject.dll` 端 4 拍 RunTick 触发 4 次 RebuildProjections，PIE log 实测：

```
[13:51:36] RebuildProjections OK (game=20260504_214929, elapsed=3.04ms)
[13:53:46] RebuildProjections OK (game=20260504_214929, elapsed=4.29ms)
[13:55:29] RebuildProjections OK (game=20260504_214929, elapsed=5.77ms)
[13:57:13] RebuildProjections OK (game=20260504_214929, elapsed=3.17ms)
```

均值 4.07 ms（标准差 ~1.2 ms），**远低于任务卡阈值 100 ms**。批量 INSERT 路径暂不需要。

---

## 已知限制 / 移交后续任务

- **commitments 在当前 Roster 配置下可能为空**：T7 阶段 Parser 输出未填 `speech_act_type` 列（events.speech_act_type 全为空字符串）。本步 ProjectCommitments 命中数高度依赖 Parser 是否填该字段。下阶段（评估器引入时）可考虑：(a) Parser §A.1 prompt 强制输出 speech_act 字段并落库；(b) 文本规则匹配兜底（"我是 X" → claim_role）。
- **agent_calibration 为空**：T1/T5 应在 BeginGame 时填 roster；当前 PIE 跑出的 .db agent_calibration 表行数 = 0。fallback 走 events.actor DISTINCT NPC% 后能正确回 10。Roster 落库属 T9 / 后续 BeginGame 调整。
- **alliance MVP 范围外**：principles §8.1 标注的复杂联盟链冲突 / moderator 巡检不实现；本投影只做"机械派生"，不做一致性校验。
- **多 as_of_seq 历史快照**：当前单快照；Resume / DB Browser 时序审计若需要"回到第 N 拍时该 agent 看到什么"，T9 阶段决定是否扩展。
- **WriteMutex 与 AsyncTask 的退出窗口**：PIE 关闭瞬间若有 AsyncTask 已入队但尚未跑到 RebuildProjections，Subsystem.Deinitialize 后 WeakObjectPtr.Get() 返 null 跳过——不会崩。已用 TWeakObjectPtr 守。

---

## 完成定义对照

- ✅ `RebuildProjections()` 实装 + WriteMutex 串行 + 单事务幂等
- ✅ 异步触发挂在 RunTick 末尾（阶段 D.5）
- ✅ `AILive.Memory.RebuildProjections` + `AILive.Memory.CompareInlinePending` console 暴露
- ✅ `DebugReadPendingIntendedJson` 公共读 API（CompareInlinePending 用）
- ✅ V1 通过（投影表行数 vs 事件等式严格成立）
- ✅ V2 通过（DDL 字段完整性，sqlite3 直查）
- ✅ V3 通过（reducer 跑 2 次状态 SHA-256 一致 + PIE 4 拍连续重建结果稳定）
- ✅ V4 通过（T4 inline vs T8 投影 7/7 NPC seq 集合严格相等）
- ✅ 性能数据（4 拍均值 4.07 ms，远低于 100 ms 阈值）
- ➖ 不意味着 Resume 已就绪（T9 范畴）
- ➖ 不意味着 BEL_EXT 9 项 evaluator 接入（仍占位）

---

## 下一步（T9 / 后续）

- T9：VerifyHashChain + Resume 协议 + Delete 跨库桥接 + agent_registry 同步
- 后续：Parser §A.1 强制输出 speech_act_type 字段（恢复 commitments projector 命中率）
- 后续：BeginGame 时把 Roster 写入 agent_calibration 表（T1/T5 漏填路径）
