# Protocol Pointer

UE 仓对 BrainService 协议的单一引用指针。

---

## 当前引用

| 字段                      | 值                                              |
| ------------------------- | ----------------------------------------------- |
| **BrainService 仓库路径** | `D:\Project\Unreal\AILiveProject\BrainService\` |
| **引用 commit hash**      | `2247c36`（HEAD；包含 `/health` bugfix `6d2cdae` + 同步脚本 `2247c36`） |
| **protocol_version**      | `0.1.1`                                         |

---

## 升级流程

1. **在 BrainService 仓库修改 `protocol/`**——协议改动只能在 BrainService 提，UE 仓不主动修改协议。
2. 更新 `protocol_version`（`protocol/protocol.md` 顶部 + 对应 schema const 字段）。
3. 在本文件更新上方表格的 **commit hash** 与 **protocol_version**。
4. 运行 UE 端 round-trip 测试（见下方占位），确认所有 examples 反序列化无误。
5. 不一致时测试报错，阻断 merge——确保两仓不漂移。

---

## UE C++ round-trip 测试位置

| 项 | 值 |
| --- | --- |
| 测试文件 | `Source/AILiveProject/Tests/AILiveProtocolRoundTripTest.cpp` |
| Vendored fixtures | `Source/AILiveProject/Tests/Fixtures/protocol_examples/*.json`（全 20 份，sha256 与 BrainService 对齐；同步用 `BrainService/scripts/sync-protocol-examples.ps1`） |
| 运行命令 | UE Editor → Tools → Test Automation → 勾选 `AILive.Protocol.RoundTrip.*` → Start Tests |

当前状态：T6 Stage A 完成（fixtures 已 vendored + pointer 已升级）；测试文件本体在 T6 Stage B 落地。

---

## 验证 BrainService examples

从 BrainService 目录运行：

```
python protocol/validate.py
```
