# 2026-05-11 · Brain 端到端注入：BP_MH_Character_1 移动 + MiniMax 说话

## 任务

用 BrainService 控制 BP_MH_Character_1 「移动到 BP_NavTarget_1 + 看向 BP_NavLookTarget_TV + 说『你好啊，很高兴认识你。』」，限用 UE 已有功能。

## 结论

- ✅ 移动 + 说话：actor_id=NPC01 → BP_NPC_MH_Character_1 → 视觉壳 BP_MH_Character_1
- ❌ 看向 NavLookTarget_TV：ontology v1 冻结为 `move_to/sit/wait`，无 `look_at`，朝向走 GASP 默认 face-velocity

## 改动

| 文件 | 改动 |
| --- | --- |
| `Source/.../AILiveProjectActionDispatcher.cpp` | 修 BP 反射 struct（崩溃根因） |
| `BrainService/brain/server/routes/debug.py` | 新增 `POST /v1/debug/inject` 测试注入 |
| `BrainService/brain/server/app.py` | include debug router |
| `BrainService/scripts/inject_demo_move_speak.py` | 注入脚本，默认顺序模式（先到再说） |

## 关键踩坑：BP 反射 struct 不匹配 → AV at 0x12

第一次跑脚本 UE 立崩在 `RouteMoveTo:320 → CallBPMoveToLocation → Pawn->ProcessEvent`。

DevLog 2026-04-29 §踩坑 8 已写硬约束：**反射调 BP 函数时 struct 字段必须 1:1 匹配 BP 签名（输入先、输出后）**。`SandboxCharacter_Mover.MoveAndLookAtLocation` BP 真实签名：

```
MoveAndLookAtLocation(MoveLocation: Vector, LookTarget: Actor) → bSucceeded: bool
```

但 dispatcher 第一版只塞了 `FVector`，BP VM 把后续未初始化字节当 `AActor*` 解引用 → AV 在 UObject 内部偏移 0x12。`ScatterMover.cpp:14-21` 早就有正确的 `FMoveAndLookAtLocationParams`。

修复要点：
1. struct 三字段对齐 BP（`MoveLocation, LookTarget, bSucceeded`），同款 `MoveAndLookAtActor` 改 BP 真实名 `MoveAndLookAt`
2. BP 主路径需要 valid LookTarget（内部 `Cast<Actor>(LookTarget)` 失败直接 Return 不移动）。当前协议没 look_at 字段，dispatcher 传 `nullptr` → 跳过 BP 走 `AAIController::MoveTo*` fallback，朝向交给 GASP face-velocity

## BrainService 注入通路

`notify_appended` 是**进程内**订阅，脚本独立进程 append 不会触发 server 进程的 ws fanout。最小新增 `POST /v1/debug/inject body={game_id, events}` 在 server 进程内调 `AppendEventsAtomically` → fanout 到 UE。鉴权复用 `require_bearer`，故意不进 protocol schema —— 测试脚手架不是公开契约。

脚本默认顺序：POST move_to → 轮询 SQLite `events.event_type='action.resolved' AND source_event_id=move_seq`（UE ActionResultReporter 上报后 server 写入）→ POST speech.public。`--parallel` 切回旧的边走边说。

## 协议错配（已知不修）

schema `move_to_params` 接受 `target_npc/target_zone` 二选一 + 可选 `coords`；UE 不消费 `target_zone`，只看 coords/npc。脚本写 `target_zone="BP_NavTarget_1"` 占位过 schema，UE 走 coords。留待 ontology v2 一起处理。

## 验证

- `pytest -q`：384 passed（debug 路由不破现有测试）
- UBT 重 build dll：Succeeded
- 手测：`python -m scripts.inject_demo_move_speak` → 走到 (6887, 4753, 279) → 到达后 MiniMax 出声 + A2F 嘴型同步 → 不再崩溃

## 反思

- DevLog 早记了"BP 反射 struct 必须匹配"的硬约束 + ScatterMover 正确范式，brain-glue（764256d/27c0c71/89932e9）写 dispatcher 时没复用 —— 写新 BP 反射前先 grep 同名 BP 函数已有 struct
- BP 函数名约定不统一：`MoveAndLookAt`(actor) vs `MoveAndLookAtLocation`(vec)。第一版猜了 `MoveAndLookAtActor`，靠 FindFunction null 走 fallback 才没立刻崩
- 「看向独立 actor」这类社交动作早晚要进 ontology v2；BP 已能消费 LookTarget，缺的只是协议层
