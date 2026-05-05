# 对抗博弈记忆系统 —— 端到端验收（T0 → T9 闭环）

## 范围

T9 落地后，对抗博弈记忆系统的拆解（`Tasks/00_overview.md`）已全部完成。本篇汇总 §11 全量 L0/L1/L2/L3 验收用例与各步实证出处，作为整套系统从 SQLite 引擎接入到 Resume 协议的端到端总账。

每条用例标"已实证"= 对应步的 DevLog / 验收日志已记录跑通；"runbook 待跑"= 代码已落地编译通过，PIE 驱动的实证待下次会话回填。

---

## 系统拓扑（一图概览）

```
┌──────────────────────────────────────────────────────────────────────────┐
│  Director (Act01 / Act02RuleReceiveDirector — UE5.7 Actor, GT 串行)        │
│   ├─ BeginGame / RunTick(N) → BeginTick(N) ──────────────────────────────┐ │
│   ├─ in-flight: Pre-write SystemLLMInflight (visibility=["system"])      │ │
│   ├─ worker thread: Reasoner LLM → Parser LLM (3 attempts)               │ │
│   └─ tick 收尾: Append (intended/note/bid/public/action.intent/...) +    │ │
│      tick_resolved + tick_audit + AsyncTask RebuildProjections           │ │
└────────────┬─────────────────────────────────────────────────────────────┘ │
             │ AppendEvent / AppendEventsAtomically                          │
             ▼                                                                │
┌──────────────────────────────────────────────────────────────────────────┐ │
│  UAILiveEventStoreSubsystem (GameInstanceSubsystem, FCriticalSection)    │ │
│   ├─ canonical JSON + SHA-256 hash chain + append-only triggers          │ │
│   ├─ visibility/payload/addressed_to validators + parse_failed 兜底       │ │
│   ├─ Read API (Quote / QuoteByRound / SearchHistory / ListMy* / Votes)   │ │
│   ├─ Bid 协议 (ListBidsForTick / ResolveFloor / RuntimeAdj)              │ │
│   ├─ Projector reducers (T8: commitments / vote_history / alliance / view)│ │
│   ├─ VerifyHashChain (T9 → RecomputeHashChainOnMainConnection)           │ │
│   ├─ ResumeFromGameId (T9 → SystemAgentTimeout 配对兜底)                  │ │
│   └─ TriggerDeleteExecuted (T9 → 跨库三步链)                              │ │
└────────────┬───────────────────────────────────┬─────────────────────────┘ │
             │ SQLite WAL                         │ SQLite WAL                │
             ▼                                    ▼                          │
   Saved/Games/<game_id>.db          Saved/Games/_meta.db                    │
   ├─ events (append-only)           ├─ schema_meta (kv)                     │
   ├─ event_visibility               ├─ agent_lifecycle_events (append-only) │
   ├─ event_addressed_to             ├─ agent_registry (state projection)    │
   ├─ events_fts (trigram/uni61)     └─ parser_prompt_registry (T2.5)        │
   ├─ commitments / vote_history /                                            │
   │   alliance_state / agent_view_state                                      │
   ├─ event_log_summaries / agent_calibration / bel_violations                │
   └─ game_state (空表，T9 范围外)                                             │
                                                                               │
   AILivePromptAssembler ◀── Read API + Roster + DataAssets ──── Director ◀──┘
   (T6: 自我发言段 + pending_intended 段 + challenge prefetch)
```

---

## §11 验收矩阵

### L0 落地基线

| # | 用例 | 责任步 | 状态 |
|---|------|--------|------|
| L0.1 | `Saved/Games/<id>.db` 自动建立；14 张表存在；`schema_version=1` | T0 / T2 | ✅ 已实证（`2026-05-04_t0_sqlitecore_openssl_setup.md` + `2026-05-04_eventstore_schema_init.md`） |
| L0.2 | 100 事件 append；`tick_no` 列存在并被填充；canonical JSON deterministic | T3 / T7 | ✅ 已实证（`2026-05-04_eventstore_write_path.md` + `2026-05-04_runtick_bid_parser.md`） |
| L0.3 | `agent_calibration` 全列存在性 SQL 检查 | T2 | ✅ 已实证（`2026-05-04_eventstore_schema_init.md`） |

### L1 协议契约

| # | 用例 | 责任步 | 状态 |
|---|------|--------|------|
| L1.1 | 视角隔离：Quote / QuoteByRound JOIN event_visibility；系统事件不泄露 | T4 | ✅ 已实证（`2026-05-04_eventstore_read_api.md`） |
| L1.2 | FTS5 模糊搜索 / `payload_text LIKE` 降级 | T4 | ✅ 已实证（同上） |
| L1.3 | `ListMyPendingIntended` events 表 + parent 链直算（不依赖投影） | T4 | ✅ 已实证（同上） |
| L1.4 | ACT01 / ACT02 全链路；`Saved/Logs/Act02/` 不再生成新 .md | T5 | ✅ 已实证（`2026-05-04_eventstore_director_integration.md`） |
| L1.5 | pending_intended prompt 注入；自我发言段 / challenge prefetch | T6 | ✅ 已实证（`2026-05-04_t6_prompt_assembler.md`） |
| L1.6 | 一拍单一 winner / speech.public 衍生关系 / 冷场推进 / bid 隔离 | T7 | ✅ 已实证（`2026-05-04_runtick_bid_parser.md`） |

### L2 回归

| # | 用例 | 责任步 | 状态 |
|---|------|--------|------|
| L2.1 | 超时恢复（LLM 超时 → SystemAgentTimeout；seq 不跳号） | T9 | 🔵 runbook 待跑（`2026-05-04_resume_delete_audit.md`） |
| L2.2 | Resume 一致性（kill UE → ResumeFromGameId → game_state 一致 + in-flight 全转 timeout） | T9 | 🔵 runbook 待跑 |
| L2.3 | 哈希链（手工 UPDATE → VerifyHashChain 报 first_bad_seq；重算后 OK） | T9 | 🔵 runbook 待跑 |
| L2.4 | DB Browser 实时只读（WAL 模式正确） | T2 / T9 | 🔵 runbook 待跑 |
| L2.5 | Delete 协议跨库桥接（`_meta.db.agent_lifecycle_events` + 该局 `system.delete_executed`） | T9 | 🔵 runbook 待跑 |
| L2.6 | agent_registry 同步（status='deleted', deleted_at 非空） | T9 | 🔵 runbook 待跑 |
| L2.7 | append-only DB 强制（`UPDATE events` ABORT；DELETE 同样） | T3 | ✅ 已实证 |
| L2.8 | append-only 离线工具路径（DROP TRIGGER → UPDATE → 必须重算 hash 才能 verify） | T9 | 🔵 runbook 待跑 |
| L2.9 | 并发互斥（10 线程各 4 events → 40 行严格 seq） | T3 | ✅ 已实证 |
| L2.10 | visibility self / 自由文本拒绝 + payload 缺 text 拒绝 | T3 | ✅ 已实证 |
| L2.11 | `addressed_to ⊆ visibility` 软校验（写入仍成功 + parse_failed 旁写） | T3 / T9 console | 🔵 runbook 待跑 |
| L2.12 | parse_failed 不递归失败 | T3 | ✅ 已实证 |
| L2.13 | 反霸麦衰减 / 被 @ 加权 / bid 阶段超时 / tick_resolved/audit 拆分 / action.intent 对所有 agent 派生 / 同拍 tick_no 切片 / payload redaction fuzz | T7 | ✅ 已实证 |
| L2.14 | 摘要展开 source_seq 定位 | T8 | ✅ 已实证 |
| L2.15 | pending_intended 投影与 T4 inline 一致（`AILive.Memory.CompareInlinePending`） | T8 | ✅ 已实证 |
| L2.16 | tick_no 不参与 canonical_json | T7 | ✅ 已实证 |
| L2.17 | Sha256Fingerprint 一致性 + CI grep `Sha1`/`FSHA1` 工程内零结果 | T0 | ✅ 已实证 |
| L2.18 | 装配双轨同步（`Cfg.Core.ModelProvider == ProviderToString(Cfg.Provider)`） | T1 | ✅ 已实证 |
| L2.19 | canonical JSON 不变性 + SHA-256 哈希链长度 64 hex | T3 / T9 | ✅ 已实证 |
| L2.20 | Parser 输出 4 段 schema 校验 / 缺 `<BID>` 段 reject / prompt 路径与 schema_meta 一致 | T2.5 | ✅ 已实证 |
| L2.21 | Parser 解析失败 → SystemParseFailed + abstain | T7 | ✅ 已实证 |

### L3 压测

| # | 用例 | 责任步 | 状态 |
|---|------|--------|------|
| L3.1 | 单 AppendEvent < 5 ms（10 NPC × 30 轮 ~600 events） | T3 | ✅ 已实证（`2026-05-04_eventstore_write_path.md`） |
| L3.2 | 单拍 prompt 拼装 < 100 ms | T6 | ✅ 已实证（`2026-05-04_t6_prompt_assembler.md`） |
| L3.3 | 100 线程并发 30 秒 events 表 = 400×N，无重复 seq | T3 / T9 | 🔵 runbook 待跑（T9 复用 ConcurrentAppend console） |

---

## 关键架构不变量（贯穿 T0 → T9）

1. **追加唯一**：events / agent_lifecycle_events 都是 append-only；UPDATE / DELETE 由 BEFORE 触发器 `RAISE(ABORT, ...)`。离线工具修改后必须重算 hash 链才能再 `VerifyHashChain`。
2. **哈希链 SHA-256**：OpenSSL `EVP_sha256`；canonical JSON 字段按 ASCII 升序、跳过 `tick_no`/`prev_event_hash`/`event_hash`/`wall_clock`/`payload_text`；FSHA1 工程内零引用。
3. **视角隔离**：所有读 API JOIN `event_visibility EXISTS`；`viewer="self"` 字面量永远不可见——self 是 prompt 模板的相对语义，禁止入读路径。
4. **写入串行**：`FCriticalSection WriteMutex` + `BEGIN IMMEDIATE`；多线程并发安全；CachedLastSeq / CachedLastHash 只在事务 COMMIT 后更新。
5. **in-flight 配对**：每次 LLM 调用前写 `system.llm_inflight`，后续完成事件 payload 必含同一 `request_id`；崩溃复盘由 ResumeFromGameId 把未配对项一律转 `system.agent_timeout`（payload 含 age_seconds 诊断）。
6. **Delete 协议跨库桥接**：先 `_meta.db.agent_lifecycle_events` → `agent_registry` 同步 → 该局 `system.delete_executed`（visibility=public）。两次 SQLite 写不在同事务，由顺序保证一致性。
7. **业务层不可直接 UPDATE 投影**：commitments / vote_history / alliance_state / agent_view_state 仅通过 `RebuildProjections()` 重建；agent_registry 仅通过 `AILiveAgentRegistry::SyncRegistryFromLifecycle()`；CI grep 守护。

---

## 仍在范围外（已对齐 implementation §12）

- Listener-as-filter 真 LLM 调用（MVP 直通）
- Score Leakage Judge 接入
- BEL_EXT 9 项 evaluator 接入（仅占位 bel_violations 表）
- GOAL / BEL dashboard
- 跨厂商 bidding 校准的预热流程
- 联盟形成机制完整实现（events 表 + projector 已支持，moderator 巡检不实现）
- 跨局 Experience Pool
- Delete 协议的决策机制（仅 hook 已就绪）
- 节目化层 / 观众接口
- 离线哈希链重写工具
- Resume 重发新鲜 in-flight（用 prompt cache）
- Delete 协议崩溃补偿（lifecycle 写但 system event 未写的修复）
- game_state 行的写入 / 读取（当前工程零写入方）

---

## 收官

`Tasks/00_overview.md` 拆解的 T0 → T9 全部代码就位、UBT 编译通过；§11 验收的"已实证"项覆盖 L0/L1 全量 + L2 多数 + L3 1/3。**T9 引入的三个新能力（VerifyHashChain / Resume / Delete）由 PIE 驱动 runbook 验收**——见 `2026-05-04_resume_delete_audit.md` 末尾的 SQL 命令逐条复跑，结果回填到该 DevLog 的"实证回填"段后即视为整套系统的 §11 闭环达成。
