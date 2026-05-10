# BrainService WebSocket 双向通信

日期：2026-05-11

## 结论

运行时通信从 HTTP action/speech polling 改为 WebSocket 全双工。BrainService 仍是 server，UE 作为 client 主动连接 `WS /v1/ws`；连接建立后 Brain 通过同一条连接主动推送事件，UE 不暴露入站端口。

协议版本升为 `0.2.0`。HTTP 只保留 `/health` 和旧调试路由，UE runtime 不再使用 `/actions/pull` / `/speech/pull`。

## BrainService

新增 `brain/server/routes/ws.py`。UE 上行 `session.*`、`roster.register`、`world_state.push`、result/reject/ack；Brain 下行 `session.*`、`roster.accepted`、`event.action_intent`、`event.action_cancelled`、`event.speech_public`、ack/error。

握手流程：WebSocket upgrade 带 `Authorization` -> `session.create` 创建 `game_id` -> `roster.register` -> runtime。

新增 `brain/eventstore/notifications.py`。EventStore append 成功后通知 transport 层；写入 `action.intent`、`action.cancelled`、`speech.public` 后，在线 UE connection 立即收到对应 frame。通知失败不影响已提交写入。

断线恢复用 `session.resume + last_brain_seq`，Brain replay `seq > last_brain_seq` 的 UE 可消费事件。

## UE

新增 `FAILiveProjectBrainWsClient`，封装 UE Runtime `WebSockets` 模块，负责连接、JSON envelope、Brain frame 解析和 GameThread 回调。

`UAILiveProjectBrainSessionSubsystem` 启动链路：`/health` 校验 `0.2.0` -> 连接 WebSocket -> `session.create` -> 枚举 `IAILiveAgent` Pawn -> `roster.register` -> world state 采样。

Action/Speech dispatcher 改为事件驱动：

- `event.action_intent` -> IngressValidator -> 现有 move/sit/wait。
- `event.action_cancelled` -> 停止对应 active action，不上报 result。
- `event.speech_public` -> IngressValidator -> MiniMax TTS/A2F。

UE 接收 Brain event 后发送 `event.ack`。拒绝走 `ingress.reject`，动作完成走 `action.result`，TTS 完成走 `speech.result`。

`world_state.push` 仍按 `WorldStateSampleIntervalMs` 周期发送；这是世界状态采样，不是 Brain polling。

## 验证

BrainService `python -m pytest -q`：384 passed；`python protocol\validate.py`：20/20 passed。UE `AILiveProjectEditor Win64 Development` 构建通过。PIE 日志确认 health -> WS connect -> session -> roster -> runtime，且不再出现 Action/Speech polling timer。

提交：BrainService `d8bc984`，UE `764256d`。
