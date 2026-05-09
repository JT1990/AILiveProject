# Protocol Pointer

UE 仓对 BrainService 协议的单一引用指针。

---

## 当前引用

| 字段                      | 值                                              |
| ------------------------- | ----------------------------------------------- |
| **BrainService 仓库路径** | `D:\Project\Unreal\AILiveProject\BrainService\` |
| **引用 commit hash**      | `c271383c8c3c2864a3988ba31e270e39be4552db`      |
| **protocol_version**      | `0.1.1`                                         |

---

## 升级流程

1. **在 BrainService 仓库修改 `protocol/`**——协议改动只能在 BrainService 提，UE 仓不主动修改协议。
2. 更新 `protocol_version`（`protocol/protocol.md` 顶部 + 对应 schema const 字段）。
3. 在本文件更新上方表格的 **commit hash** 与 **protocol_version**。
4. 运行 UE 端 round-trip 测试（见下方占位），确认所有 examples 反序列化无误。
5. 不一致时测试报错，阻断 merge——确保两仓不漂移。

---

## UE C++ round-trip 测试位置（T6 实现后填写）

> 占位：由 T6 卡片实现后在此处填入测试文件路径 + 运行命令。

当前状态：**未实现**（T1 阶段只要求 BrainService examples 通过 JSON Schema 校验；UE 端 USTRUCT 镜像与反序列化测试由 T6 实现）。

---

## 验证 BrainService examples

从 BrainService 目录运行：

```
python protocol/validate.py
```
