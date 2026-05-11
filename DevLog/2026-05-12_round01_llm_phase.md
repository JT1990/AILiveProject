# Round-001 LLM 3 拍循环接入

日期：2026-05-12

## 结论

UE 端开场动画（Round-001 Tick-0001/0002：广播→开门→EQS 散点→视频）收尾后，自动通过新增的 `round.start_llm_phase` WebSocket 帧触发 Brain 跑 3 拍 LLM 自由发言（Tick-0003/0004/0005，phase=`day_discuss`）。Brain 后台按拍跑 10 NPC × 10 provider 并发 reasoner → bid → Listener-as-filter，每拍产出 1 条 `speech.public` 经现成 fanout 推回 UE；UE 端 SpeakDispatcher 播完 TTS 上报 `speech.result`，Brain 写 `speech.playback_resolved` 解锁下一拍，防 NPC 语音重叠。

协议版本升 MINOR：`0.2.0 → 0.3.0`。

## 入口与触发

`ARound01RuleIntroDirector::CompleteRound01`（`Private/Rounds/Round01RuleIntroDirector.cpp`）在广播 `OnRound01Completed` 前调 `UAILiveProjectBrainSessionSubsystem::RequestStartLLMPhase(1, "day_discuss", 3)`。`HandleVideoEnded` 与视频 fallback 都走 `CompleteRound01`，路径统一。失败仅 log，不阻塞场景推进。

`UAILiveProjectBrainSessionSubsystem::RequestStartLLMPhase` 是 BlueprintCallable，转发到 `FAILiveProjectBrainWsClient::SendRoundStartLLMPhase(GameId, RoundNo, Phase, NTicks)`。前置 `bSessionReady && bRosterAccepted` 检查；不就绪时 log warning 返回 false。

帧 payload：`{round_no, phase: const "day_discuss", n_ticks: 1..10}`。Brain 应答 `ack`：`{run_id (uuid hex), round_no, n_ticks, started_at}`，或 `error`：`ROSTER_NOT_REGISTERED` / `ROSTER_INCOMPLETE`（roster ≠ 10）/ `ROUND_RUN_BUSY`（同 game 还在跑）/ `PROVIDER_UNAVAILABLE`（API key 缺失）。

## 链路

```
UE                                        Brain
──                                        ─────
按 1 / round01.start console
  → 开门(1s lerp) → EQS 散点 → MediaPlate
  → HandleVideoEnded → CompleteRound01
     ├─ OnRound01Completed.Broadcast
     └─ RequestStartLLMPhase(1, day_discuss, 3)
          ─ WS frame round.start_llm_phase ──►
                                              _handle_round_start_llm_phase
                                                schema 校验
                                                list_registered_actors == 10
                                                _active_runs[gid] 防重入
                                                build_agent_specs (10 providers)
                                                build_listener_provider
          ◄── ack {run_id, round_no, n_ticks, started_at}
                                              asyncio.create_task(run_round(...))
                                              for tick in 1..3:
                                                assemble_prompts_for_tick
                                                run_tick (ThreadPoolExecutor × 10)
                                                archive_tick → md
                                                if public_seq is not None:
          ◄── event.speech_public ────────────  (fanout via on_events_appended)
        SpeakDispatcher → MiniMax TTS/A2F
          ── speech.result ───────────────────►
                                                  写 speech.playback_resolved
                                                  wait_for_playback_resolved 解锁
                                                next tick...
```

## 协议升级

`0.2.0 → 0.3.0` 字面值已全仓清理，见 BrainService `protocol/` 与 UE 端 `Source/AILiveProject/{Public,Private,Tests}` 注释 / health 校验 / round-trip 期望值。`Docs/protocol_pointer.md` 指向 BrainService commit `049df58`。

UE 端 vendored fixture 由 `BrainService/scripts/sync-protocol-examples.ps1` 同步到 `Source/AILiveProject/Tests/Fixtures/protocol_examples/`，含新增 `round_start_llm_phase.{success,busy}.json`。

## 设计要点

**playback wait 防 TTS 重叠**：上一拍 `speech.public` 派生后 UE 端 SpeakDispatcher 跑 3–10s 的 TTS+A2F；下一拍若立即推进会让 NPC 语音重叠。Brain 端 `run_round` 改 async，每拍若 `public_seq is not None` 则 `await wait_for_playback_resolved(game_id, public_seq, timeout=60s)`。超时只写一条 `system.round_run_failed(reason=playback_timeout)` 但**继续推进**，不让单次上报失败把整轮卡死。

**tick 不入事件 schema**：memory_principles §4.1.1.1 必备字段不含 `tick_no`，§4.1.3 只要求 `seq` 单调 + `parent_event_id` 串同源。tick 在本方案是 `round_runner` 的内部循环计数 + 归档 md 文件名 label；事件按 `seq` 排序 + `orchestrator.tick_resolved` 边界自然推断同拍归属。`tick_label_offset=2` 让 Round-001 LLM 3 拍归档命名为 `Tick0003 / Tick0004 / Tick0005`，对齐用户的全局拍序语义。

**`bReady` 不足以保证 LLM 阶段能跑**：handshake 完成后 `bReady=true`，但 Brain 端 `build_agent_specs` 会按 `NPC_PROVIDER_MAP` 实例化 10 家 provider，需要 10 个 API key 环境变量。`python -m brain.server` 启动时如未自动加载 `.env` 就会 `Missing env var DEEPSEEK_API_KEY`，UE 端收到 `PROVIDER_UNAVAILABLE`。Brain 端已在 `brain/config.py` 加 `_load_dotenv()`（`os.environ.setdefault`，CI 无 .env 时静默跳过）。

**调试日志开关**：Brain 端引入 `BRAIN_VERBOSE_LOG=1`（默认开）的 vlog helper，串到 WS 入帧 / handler 分支 / round.start_llm_phase 全路径。设为 0 关掉所有 verbose。

## UE 侧改动文件

- `Public/AILiveProjectBrainWsClient.h` + `Private/.cpp`：加 `SendRoundStartLLMPhase`。
- `Public/AILiveProjectBrainSessionSubsystem.h` + `Private/.cpp`：加 `RequestStartLLMPhase` BlueprintCallable；health 校验 `0.2.0 → 0.3.0`。
- `Private/Rounds/Round01RuleIntroDirector.cpp`：`CompleteRound01` 加触发调用 + `#include "AILiveProjectBrainSessionSubsystem.h"`。
- `Public/AILiveProtocolTypes.h` / `Public/AILiveProtocolJson.h`：注释 `v0.2.0 → v0.3.0`（grep 一致性）。
- `Tests/AILiveProtocolRoundTripTest.cpp`：health.success 期望值 `0.3.0`。
- `Tests/Fixtures/protocol_examples/*.json`：同步脚本产物。
- `Docs/protocol_pointer.md`：commit hash `049df58`、版本 `0.3.0`。

## BrainService 侧改动概览

详见 `BrainService/DevLog/2026-05-12_round_runner_zombie_round01.md`。共 6 个 commit：

| Hash | 概要 |
|---|---|
| `3bf4d7a` | feat(events): 新增 `system.round_run_failed` 事件类型 + 默认 visibility |
| `92ef2e4` | feat(protocol): 0.2.0 → 0.3.0，新增 `round.start_llm_phase` schema/examples，配置层加 `LLM_OUTPUT_ROOT` 与 `.env` 自动加载 |
| `95fe680` | feat(orch): `rules_cards.py` 僵尸规则文本、`llm_archive.py` 归档 sink、`PreparedTick` 拆分 |
| `ee5db00` | feat(orch): async `run_round` + `wait_for_playback_resolved` + `list_registered_actors` |
| `049df58` | feat(server): WS handler `round.start_llm_phase` 含 _active_runs 防重入 |
| `b7d1808` | chore: `brain/log.py` 引入 `vlog` + `BRAIN_VERBOSE_LOG` 开关 |

## 验证

**Brain 全量测试**：`pytest -q` 451 passed（基线 435 + 16 项新测试，覆盖 archive sink、round runner、WS handler 三个 case + system.round_run_failed 枚举）。

**协议 round-trip**：`python protocol/validate.py` 22/22。UE Automation `AILive.*` 19 项绿（fixtures 同步后）。

**端到端**：BrainService 跑 `python -m brain.server`（自动加载 `.env`）→ PIE 启动自动 handshake → 按 `1` 键 → 视频播完 → UE 自动发 `round.start_llm_phase` → Brain 跑 3 拍 → `BrainService/LLMOutput/{YYYYMMDDHHmm}/` 产出 30 个 md（`Tick000{3,4,5}-NPC{01..10}.md`）。期间 UE 端依次收到 ≤3 条 `event.speech_public`（每拍 floor winner），SpeakDispatcher 触发 TTS/A2F。

## 当前不在范围

- Tick-0001/0002 协议化（开门/视频/无角色广播 TTS 仍走 UE 端 `round01.start` 自驱动）。
- Reflection 9 问、phase summary、judge worker。
- 投票 phase / 触碰 phase / 感染状态机 / 解药机制——僵尸游戏的核心博弈机制本轮仅靠"自由发言"占位，下阶段补。
