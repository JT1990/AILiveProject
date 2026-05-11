# Protocol Pointer

UE 仓对 BrainService 协议的单一引用指针。

| 字段 | 值 |
| --- | --- |
| BrainService 仓库路径 | `D:\Project\Unreal\AILiveProject\BrainService\` |
| 引用 commit hash | `049df58` |
| protocol_version | `0.3.0` |

## 当前结论

- Runtime 通信使用 `WS /v1/ws`，BrainService 是 server，UE 是 WebSocket client。
- HTTP 只作为 `/health` 与调试兼容入口；UE runtime 不再使用 action/speech polling。
- UE 断线后用 `session.resume + last_brain_seq` 请求 replay。

## 验证

- BrainService: `python -m pytest -q tests/test_server_websocket.py tests/test_server_contract.py`
- UE: 构建 `AILiveProjectEditor Win64 Development`，PIE 后检查日志中出现 WebSocket session ready / roster registered。
