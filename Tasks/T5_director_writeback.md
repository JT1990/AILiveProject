# T5 — Director 写入接入 + in-flight 协议

> 总览：[00_overview.md](00_overview.md)
> 依赖：blockedBy = [T3](T3_eventstore_write_path.md)（写）
> 改动量：**中**（1–4h）
> §11 验收：L1「ACT02 全链路 + events 无丢失」+ 新增「ACT01 首次入库」

## 预检

(a) `AAct02RuleReceiveDirector` / `AAct01RuleIntroDirector` 方法列表用 Read/Grep 查 .h/.cpp（C++ 真实状态，**不**走 Monolith MCP）；(b) PIE 跑 ACT01/02 验收前 `mcp__monolith__monolith_status` 在线（Monolith 用于读 ACT01 涉及的 Door / 视频 BP 状态——确认不动 BP 链路只在 C++ 节点插桩）。

## 目标

把 `AAct02RuleReceiveDirector` 现有 .md 写盘路径删除并改为 AppendEvent；引入 in-flight 配对事件协议（SystemLLMInflight + 配对的业务事件含同一 request_id）。**ACT01 是首次接入 EventStore**——本步为它新增 setup phase 事件写入，**不**是删旧路径（grep 确认 ACT01 现无 WriteLLMLog 等引用）。

## 涉及文件

### 修

- `Source/AILiveProject/Public/Acts/Act02RuleReceiveDirector.h` —— 删 `WriteLLMLog` / `WriteWinnerLog` / `GetSessionDir` / `MakeSubDir` 方法声明；保留 `FInflight` / `FParsedAnswer` / `EAct02State` / `EWillingness`；新增私有静态 `BuildPayloadJson(const FParsedAnswer&, const OpenAIChat::FResult&)` 等
- `Source/AILiveProject/Private/Acts/Act02RuleReceiveDirector.cpp` —— 删 4 个方法实现 + `Saved/Logs/Act02/<session>/` 目录创建逻辑；`BeginAct02()` 入口：若 `!Store->IsGameOpen()` 则 `Store->BeginGame(GameId = FDateTime::Now().ToString(...))`；每次 LLM 请求**前**写 `SystemLLMInflight`（payload 含 RequestId / npc_index / system_prompt_hash / user_prompt_hash / started_at）；请求**后**写对应业务事件——**注意：T5 阶段尚未引入 bid + intended 四通道，Director 直接把 LLM 文本 wrap 为 `speech.public`，这是过渡形态**。所有 T5 写出的 speech.public 必须在 payload 加 **`"legacy_pre_bid": true`** 标记（T7 验收 SQL 必须过滤 `WHERE json_extract(payload,'$.legacy_pre_bid') IS NULL`）；T7 主循环重写后 speech.public 由 orchestrator 从 winner intended 衍生，不再带此标记。winner_decision / vote 等事件不受影响
- `Source/AILiveProject/Public/Acts/Act01RuleIntroDirector.h` + `.cpp` —— **首次**接入 EventStore：BeginAct01 时检查 `Store->IsGameOpen()`（与 ACT02 共用同一 game_id；详见 [00_overview.md](00_overview.md) "文档冲突 / 待澄清" #2 决策），并在 setup phase 关键节点（开门 / 视频开始 / 视频结束）写 `event_type=orchestrator.round_resolved`、`phase=setup`、`actor=orchestrator`、`visibility=["public"]` 的事件。**不**改 Door / 视频 BP 链路，只在已有 C++ 节点处插桩 AppendEvent

### 新增

- `Source/AILiveProject/Public/Util/AILiveJsonHelpers.h` —— `EscapeJsonString` 公开声明（避免 EventStore 与 Director 各自实现）

## 依赖

blockedBy = [T3](T3_eventstore_write_path.md)（写）

## 验收方式

对齐 §11 **L1 烟测**：

- L1「ACT02 全链路」：10 NPC × 3 轮 ~60 事件全程跑通；events 无丢失；`UNIQUE (game_id, seq)` 零冲突
- 手工：DB Browser 打开新 .db → events 表能看到 SystemLLMInflight + speech.public 配对（payload 同一 request_id）
- 手工：`Saved/Logs/Act02/<session>/` 目录不再生成（旧 .md 路径已删）
- **新增「ACT01 首次入库」**：跑 ACT01 一遍 → 同一 .db 中能看到 ≥ 3 条 `event_type=orchestrator.round_resolved` + `phase=setup` 的事件（开门 / 视频开始 / 视频结束三个节点），seq 在 ACT02 任何 speech.public 之前

## 风险点

- `BeginGame` 由谁触发：implementation §5.3 明确 `BeginAct02` 入口检查；但跨 Act 的"局"边界目前无明确定义——本 MVP 暂用 ACT02 入口创建 game_id，下一阶段引入更上层的"局开始/局结束"逻辑
- 现有 `WriteLLMLog` / `WriteWinnerLog` 对外有调用点吗？grep 确认仅 `Act02Director.cpp` 内部使用 → 删除安全；如有 BP 调用要先沟通
- `EscapeJsonString` 现有可能散落在多处——T5 集中到 `Util/AILiveJsonHelpers.h` 后，删除散落副本
- `system_prompt_hash` / `user_prompt_hash` 用 `Sha256Fingerprint`（T3 落地）—— 不允许 FSHA1

## 里程碑 DevLog

`DevLog/2026-MM-DD_eventstore_director_integration.md`，记录 .md 写盘删除 / in-flight 配对协议 / ACT01 首次入库证据

## 完成定义

ACT02 整局走完无 .md 文件生成、events 完整、in-flight 配对正确、ACT01 setup 事件入库 + 里程碑 DevLog 入库。**不**意味着 prompt 已经从 EventStore 拼装（T6）。
