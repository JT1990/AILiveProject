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

## 2026-05-12 续：WorldStateCollector 暂关 + 对接 Brain prompt 加固

跑通的首个 Round-001 端到端暴露了 prompt / validator 一批问题（详见 `BrainService/DevLog/2026-05-12_round_runner_zombie_round01.md` 末尾的「prompt + provider 加固」节）。本节只记 UE 侧改动。

### 改动：暂关 WorldStateCollector 轮询

`Source/AILiveProject/Private/AILiveProjectBrainSessionSubsystem.cpp:Phase5_StartRuntime()` 注释掉 `C->StartPolling()` 一行。子系统类 `UAILiveProjectWorldStateCollector` 仍在仓里，只是不再每 500ms 推 `world_state.push`。

**为什么暂关**：

- Brain 端 prompt 段 7（公开发言近场窗口）默认 `quote_by_round` 拉 viewer 全量可见事件，会把 `world.actor_state` / `world.perception.sight` / `world.client_sample` 一起拉进去——payload 没 text 字段，渲染为空，300 行 `seq=X [world.actor_state] system:` 噪声彻底淹没真信号。
- 当前 agent 决策链路（reasoner / floor / listener）并不消费 world.* 事件；推它们到 Brain 只是空跑事件流（SQLite 表过去一晚累计 23 万行 `world.perception.sight`、5 万行 `world.actor_state`）。
- memory_principles §3.1 反模式表明确「滚动窗口硬存储为唯一手段」违反全量追溯——500ms 高频感知噪声把段 7 当滚动窗口写满，是触发这条反模式的典型路径。

**重启前置条件**：要让 agent 真正消费近场感知前，必须先在 Brain 端做「近场感知摘要」投影（去重 + 时间桶聚合：状态变化才出条，非每帧一条），再 prompt 拉投影而非原始事件。届时把 `BrainSessionSubsystem.cpp:218` 那行 `C->StartPolling()` 注释撤掉即可，UE 侧无其他配套改动。

Brain 端配套（已落地，本次 UE 侧不需要改）：

- `brain/orchestration/prompt_assembly.py` 段 7 加 `_SEG7_EVENT_TYPES` 白名单（即便 collector 重启，prompt 也只收叙事性事件，需要感知时显式接入新投影段）。
- seg_1 + seg_2 合并为单条 system 消息（治 minimax）。
- 完整 4 通道 schema 例子内嵌 system message。
- `reasoner_v1.schema.json` 的 `enum` 字段补 `"type": "string"`（治 kimi/moonshot strict）。
- provider 三档模式 + per-provider timeout + retry 预算重排。

### 验证

- UE Automation `AILive.*` 19 项不受影响（不依赖 WorldStateCollector）。
- 完整 Round-001 端到端（`BrainService/LLMOutput/202605121826/`）→ 10/10 NPC `pass: True`，单拍 5-72s（kimi 思考最慢），整轮约 3 分钟，相比修复前的 10+ 分钟 + 大面积 validation_failed 已稳定。

### 下游接手点

1. **可以开始做的**：投票/触碰/感染状态机/解药等僵尸核心机制；Tick-0001/0002 协议化把开门/视频/广播 TTS 也搬到 Brain 驱动。
2. **复用 WorldStateCollector**：先在 Brain 仓做投影，再撤注释。注释行号 `AILiveProjectBrainSessionSubsystem.cpp:218`（`Phase5_StartRuntime`）。
3. **`UAILiveProjectWorldStateCollector` 不要删**：UE 端基础设施（PerceptionLogger + Roster 反查 + transport schema）仍有效，只是上游消费者还没准备好。

## 当前不在范围

- Tick-0001/0002 协议化（开门/视频/无角色广播 TTS 仍走 UE 端 `round01.start` 自驱动）。
- Reflection 9 问、phase summary、judge worker。
- 投票 phase / 触碰 phase / 感染状态机 / 解药机制——僵尸游戏的核心博弈机制本轮仅靠"自由发言"占位，下阶段补。
- 「近场感知摘要」投影（重启 WorldStateCollector 的前置条件）。
