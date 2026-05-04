# 2026-05-04 — EventStore Director 集成 + in-flight 协议 + ACT01 首次入库

T5 任务卡完成。把 ACT02 Director 从 `Saved/Logs/Act02/<session>/*.md` 写盘切到 `UAILiveEventStoreSubsystem::AppendEvent`，引入 in-flight 配对协议（principles §5.4），并第一次给 ACT01 setup phase 接入 EventStore（开门 / 视频开始 / 视频结束三个节点写 `orchestrator.round_resolved`）。

## 决策

- **EventStore 强依赖**：Store 不可用 / BeginGame 失败 / Pre 或 Post AppendEvent 失败 → 立即 FailAct01 / FailAct02。T5 完成定义是 "events 入库"，绝不允许 "有则写"——否则 V1/V3/V8 验收 SQL 空集自欺。winner_decision 失败仅 Warning（纯审计事件，不影响 in-flight ↔ speech.public 配对）。
- **`legacy_pre_bid: true` 标记**：T5 Director 直接把 LLM 文本 wrap 为 `speech.public` 是过渡形态，所有这类 public 在 payload 加此标记。T7 主循环重写后 `speech.public` 由 orchestrator 从 winner intended 衍生，不再带此标记，SQL 用 `WHERE json_extract(payload,'$.legacy_pre_bid') IS NULL` 一刀切干净。
- **每个 LLM 调用必写 1 in-flight + 1 speech.public**：即便 `want_to_speak=false / willingness=none / parse_error=true`，payload.text 可为空但字段必须存在——保 Resume 协议正确。
- **跨 Act 共享 game_id**：ACT01 / ACT02 入口任一先到者调 BeginGame；MVP 阶段 game_id 由 `BeginAct0X` 创建，下一阶段引入 ZombieGame 上层 "局开始/结束" 逻辑。00_overview.md 文档冲突 #2 已明确该解读。
- **`ResolvePhase` 放 .cpp anonymous namespace**：避免 `.h` 引入 `Memory/AILiveEventTypes.h` 依赖膨胀；其他 `Build*PayloadJson` 静态方法的参数类型已在 .h 可见。
- **`Util/AILiveJsonHelpers.h` 落地为 umbrella 头**：T3 已落地 `AILiveJsonEscape.h` + `AILiveSha256.h`，T5 新增的 umbrella 头仅转发 include，零运行时成本，与任务卡"涉及文件"清单对齐。
- **ACT01 video started 不依赖 `Player->Play()` 返回值**：写入移到 `StartVideo()` 末尾保证一定写一条 setup 事件（语义"已请求播放"），避免播放器状态分支漏写。

## 落地的 4 类事件

| 事件类型 | 写入位置 | visibility | 配对/标记 |
| --- | --- | --- | --- |
| `system.llm_inflight` | `DispatchLLMs` Async 之前 | `["system"]` | 含 `request_id` / `system_prompt_hash` / `user_prompt_hash` / `started_at` |
| `speech.public` | `Gather*AndPickWinner` 每个 NPC 都写 | `["public"]` | `legacy_pre_bid: true` + 同 `request_id`（配对 in-flight） |
| `winner_decision` | 选完 winner 后 | `["public"]` | actor=`orchestrator`，含 winner_npc_index/willingness/round |
| `orchestrator.round_resolved` | ACT01 三个 setup 节点 | `["public"]` | phase=`setup`，actor=`orchestrator` |

## 文件改动

- `Source/AILiveProject/Public/Util/AILiveJsonHelpers.h` —— **新增** umbrella 头转发 `AILiveJsonEscape.h` + `AILiveSha256.h`
- `Source/AILiveProject/Public/Acts/Act02RuleReceiveDirector.h` —— 删 4 个写盘方法 + `SessionTimestamp`；`FInflight` 加 `RequestId`；加 3 个 `Build*PayloadJson` 静态辅助
- `Source/AILiveProject/Private/Acts/Act02RuleReceiveDirector.cpp` —— 删 4 个方法实现 + 写盘 includes（保留 `Misc/FileHelper.h` 因为 `LoadGameRule` 仍用）；anonymous namespace 加 `ResolvePhase` / `GetEventStore` helper；`BeginAct02` 接 EventStore（强）；`DispatchLLMs` 写 in-flight；`Gather*` 写 speech.public + winner_decision
- `Source/AILiveProject/Public/Acts/Act01RuleIntroDirector.h` —— 加 `AppendOrchestratorRoundResolved` private helper
- `Source/AILiveProject/Private/Acts/Act01RuleIntroDirector.cpp` —— `BeginAct01` 接 EventStore（强）；3 个 setup 节点插桩（`StartNPCMovement` 入口 / `StartVideo` 末尾 / `HandleVideoEnded` + 超时 fallback）；helper 实现

## 协作者审查吸收

施工前协作者按 ue5-debug-validation 口径提了 7 条 finding，全部接受并落地：

- **High** EventStore 不可选 → 改为 Store null/失败立即 Fail
- **High** in-flight 写失败必须阻断 → Pre/Post AppendEvent 失败都进 Fail；winner_decision 仅 Warning
- **High** `ResolvePhase` 放 .cpp 内部 → 避免 .h include 膨胀
- **Medium** `AILiveJsonHelpers.h` 薄兼容头 → 落地为 umbrella forwarding
- **Medium** Monolith 预检 → 验收章节加 V0
- **Medium** ACT01 video started 改在 `StartVideo()` 末尾 → 不依赖 `Player->Play()` 分支
- **Medium** V8 SQL 改 `MAX(setup) < MIN(speech.public)` → 正确语义

## 验收实证

DB: `Saved/Games/20260504_155102.db`（PIE 跑 ACT01 → ACT02 全程产物，81 events，seq 1..81）

| 验收项 | SQL/方法 | 实际结果 |
| --- | --- | --- |
| V0 Monolith MCP 在线 | `monolith_status` | v0.14.7 ✓ |
| V1 各事件类型计数 | `SELECT event_type, COUNT(*) FROM events GROUP BY event_type` | round_resolved=3, speech.public=37, llm_inflight=37, winner_decision=4 ✓ |
| V2 UNIQUE (game_id,seq) | `GROUP BY game_id, seq HAVING COUNT(*)>1` | 0 行 ✓ |
| V3 in-flight ↔ speech.public 配对 | rid GROUP BY HAVING pre!=1 OR post!=1 | 0 不平衡（37 个 rid 全 1:1）✓ |
| V4 speech.public 全部 legacy_pre_bid:true | `WHERE legacy_pre_bid IS NULL OR != 1` | 0 漏标 ✓ |
| V5 旧 .md 路径不再生成 | `ls Saved/Logs/Act02/` | 最新 mtime 5/4 04:34 < PIE 启动 15:50 ✓ |
| V6 哈希链：64 hex + 连续 + genesis | `LENGTH(event_hash)!=64`; `prev_hash[N]==hash[N-1]`; row 1 prev_hash=000... | 0 bad / 0 broken / genesis OK ✓ |
| V7 系统事件不泄露 | `event_visibility` viewer LIKE 'NPC%' AND inflight | 0 行 ✓ |
| V8 ACT01 setup 3 条 + 顺序 | seq 列出 + MAX(setup)<MIN(speech.public) | seq 1..3 setup; setup MAX=3 < speech MIN=14, ok=1 ✓ |
| V9 编译 | UBT `AILiveProjectEditor Win64 Development` | Result: Succeeded, 0 errors ✓ |

## 与 principles §5.4 / schema.yaml 的对应

| 协议要求 | 落地 |
| --- | --- |
| 请求前写 SystemLLMInflight（visibility=["system"]） | `DispatchLLMs` 中 Pre 事件 + Pre.Visibility={"system"} |
| RequestId 同链全程对应 | `FInflight::RequestId` + `BuildLLMInflightPayloadJson` + `BuildSpeechPublicPayloadJson` 共用 |
| `system_prompt_hash` / `user_prompt_hash` 用 SHA-256 | `AILiveUtil::Sha256Fingerprint`（OpenSSL EVP_sha256，禁 SHA-1） |
| 配对业务事件的 `payload.request_id` | `BuildSpeechPublicPayloadJson` 输出 `"request_id":"<rid>"` |
| schema.yaml `speech.public` 必须有 `text` 字段 | `BuildSpeechPublicPayloadJson` 头字段就是 `"text":<EscapedContent>` |
| 永不 UPDATE 已写入的 SystemLLMInflight | append-only DB trigger（T2/T3 落地）防御 |
| `wall_clock` 不参与排序 | EventStore 实现自动填 `wall_clock`，排序仍由 `seq` |

## 不在范围（T7/T8/T9 处理）

- bid + intended 四通道（T7 RunTick 重写）
- speech.public 由 winner intended 衍生（T7 后 legacy_pre_bid 全过滤）
- ListBidsForTick / ResolveFloor（T7）
- pending_intended 投影（T8）
- Resume from in-flight（T9，按 wall_clock 分类 + SystemAgentTimeout）

## 下一步

T7 接入。届时把 `legacy_pre_bid:true` 的 speech.public 全部排除（SQL 过滤），由 orchestrator 从 winner intended 衍生新的 public。
