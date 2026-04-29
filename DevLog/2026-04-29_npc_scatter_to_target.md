# 10 NPC 同时到达目标 + EQS 自动散点

日期：2026-04-29

## 目标

按 **L** 键 → 关卡内 10 个 `BP_NPC_MH_Character_*_C` 各自走到 `BP_NavTarget` 周围 NavMesh 上**不同的散点位置**（不是先聚到目标再分散），到达后看向 `BP_NavLookTarget`。每个 NPC 的最终落点在按 L 那一刻就分别确定，路径从一开始分叉。

LLM Mind 接管前的最小执行基线：验证多 agent 散点协调能跑通整条 EQS → 异步派发 → 单 NPC MoveAndLookAt-by-Location 链路。

## 技术决策

**C++ 主导（EQS 异步管理 + 贪心最近匹配 + 玩家过滤） + 一个新 BP 函数（按 location 移动）**。

| 模块 | 形态 | 原因 |
|---|---|---|
| `EQS_ScatterAroundTarget` | 数据资产 | OnCircle + Project + Overlap，一次性配置。 |
| `UAILiveProjectScatterMover::ScatterNPCsAroundTarget` | C++ BlueprintFunctionLibrary | EQS 异步、回调里 iterate items 并按贪心最近匹配派发到 NPCs；BP 端 `Array_Get` wildcard 在 MCP 下走不通，C++ 一行解决。Mind 模块未来直接复用。 |
| `MoveAndLookAtLocation(FVector, AActor*)` | BP 函数（加在 `SandboxCharacter_Mover`） | 与既有 `MoveAndLookAt` 共用 `bAligningLook` / `LookDirection` / `PollAndAlignLook` / `Get_OrientationIntent` 状态机，新增的位置入口走相同出口。GASP 链路零改动。 |
| Level BP L 键节点链 | 蓝图 | 与 M/I/N/K 一致的最小触发 hook。LLM Mind 接管后整段删除。 |

**未改 NavMesh `agent_radius`**：保持默认 35。增大全局值会影响 `BP_SmartBench` entrance 投影，破坏 K 键 sit。改为在 EQS 里加 Overlap filter 让候选点本身远离墙体，绕开全局副作用。

## 资产改动

### 1. 新增 EQS 查询 `Content/AI/EQS/EQS_ScatterAroundTarget.uasset`

| 段 | 类 | 关键属性 |
|---|---|---|
| Generator | `EnvQueryGenerator_OnCircle` | `CircleRadius=300`，`CircleCenter=EnvQueryContext_Querier`，`SpaceBetween=50`（默认）→ 整圈 ≈37 候选点 |
| Test 0 | `EnvQueryTest_Project` | `TraceMode=Navigation`，purpose Filter（剔除不在 NavMesh 上的点） |
| Test 1 | `EnvQueryTest_Distance` | `DistanceTo=Querier`，purpose Score（评分用，圆周等距所以是 noop，留作扩展） |
| Test 2 | `EnvQueryTest_Overlap` | `OverlapShape=Sphere`，`ExtentX=ExtentY=ExtentZ=80`，`ShapeOffset=(0,0,90)`（避开地面），`OverlapChannel=ECC_WorldStatic`，purpose Filter Match `BoolValue=false`（剔除球体碰到墙的点） |

Querier 由调用方运行时传入（这里传 `BP_NavTarget` 实例）。`validate_eqs_query` 通过 0 error/0 warning。

### 2. 新增 C++ helper `UAILiveProjectScatterMover`

文件：

- `Source/AILiveProject/Public/AILiveProjectScatterMover.h`
- `Source/AILiveProject/Private/AILiveProjectScatterMover.cpp`

签名：

```cpp
UFUNCTION(BlueprintCallable, Category = "AILive|Scatter",
    meta = (WorldContext = "WorldContextObject",
        DisplayName = "Scatter NPCs Around Target"))
static void ScatterNPCsAroundTarget(
    UEnvQuery* QueryAsset,
    AActor* CenterActor,
    const TArray<AActor*>& NPCs,
    AActor* LookTarget,
    UObject* WorldContextObject);
```

行为：

1. **过滤玩家 Pawn**：`Cast<APawn>(NPC)->IsPlayerControlled()` 跳过。`L_prison` 玩家也是 `SandboxCharacter_Mover_C` 直接实例，会被 BP 端 `GetAllActorsOfClass(MoverNPCClass)` 抓到。
2. `FEnvQueryRequest Request(QueryAsset, CenterActor)` —— Querier=CenterActor，让 `EnvQueryContext_Querier` 解析到 `BP_NavTarget` 实例位置。
3. `Request.Execute(EEnvQueryRunMode::AllMatching, FQueryFinishedSignature::CreateLambda(...))` —— 异步触发。
4. `WeakNPCs[]` + `WeakLookTarget` 用 `TWeakObjectPtr` 装进 lambda capture 防 dangling。
5. 回调里 `Result->GetAllAsLocations(Locations)` 取所有点。
6. **贪心最近匹配**：`min(NumNPCs, NumPoints)` 轮里，每轮扫描所有未分配 NPC × 未占用 point，挑全局 `DistSquared` 最小的一对锁定。10 NPC × 37 点 × 10 轮 ≈ 3700 比较，零开销。每个 NPC 拿当前最近的可选点，避免"NPC 在东、被分到西边的点"。
7. NPC 数大于点数（罕见）：超出部分降级为复用最后一个点（warning）。
8. 通过 `NPC->FindFunction("MoveAndLookAtLocation") + ProcessEvent` 调 BP 函数派发，避免 C++ 直接依赖 BP 类。

`Build.cs` 不改 —— `UEnvQuery`/`FEnvQueryRequest`/`FEnvQueryResult` 都在已依赖的 `AIModule` 里。

诊断 log（category `LogAILiveScatter`）：

```
[Scatter] firing EQS '<name>' with Querier=<actor> for N NPCs (skipped K player-controlled)
[Scatter] EQS returned M candidate points:
[Scatter]   point[i] = (X, Y, Z)
...
[Scatter] NPC[i] <name>: from (X,Y,Z) -> point[j] (X,Y,Z), dist=...
[Scatter] dispatched N NPCs to M EQS points
```

### 3. `SandboxCharacter_Mover` 新增 BP 函数 `MoveAndLookAtLocation`

签名：`MoveAndLookAtLocation(MoveLocation: Vector, LookTarget: Actor) → bSucceeded: bool`，category `AI`。

主路径（与既有 `MoveAndLookAt` 唯一差异：跳过 Move 端 `Cast<Actor>` + `K2_GetActorLocation`，直接用入参 `MoveLocation` 喂 `MoveToLocation.Dest`）：

```
FunctionEntry
  → Set bAligningLook=false
  → Cast<Actor>(LookTarget)              # None-check
    → Cast<AIController>(GetController)
      → Set CurrentLookTarget = LookTarget
      → AIController.MoveToLocation(
          Dest = MoveLocation,
          AcceptanceRadius=80, bStopOnOverlap=true,
          bUsePathfinding=true, bProjectDestinationToNavigation=true,
          bCanStrafe=false, bAllowPartialPath=false)
      → K2_SetTimer("PollAndAlignLook", 0.1, looping=true, InitialStartDelay=0.5)
      → bSucceeded = NotEqual_ByteByte(MoveResult, 0)
      → Return
```

失败路径（`Cast<Actor>` / `Cast<AIController>` 失败）打 PrintString 后 Return。

`PollAndAlignLook` / `Get_OrientationIntent` / `bAligningLook` / `LookDirection` 状态机零改动复用。

### 4. `L_prison` Level BP 新增 1 变量 + L 键节点链

新增变量（category `NPCMove`）：

- `ScatterQueryAsset: object:EnvQuery`，default `/Game/AI/EQS/EQS_ScatterAroundTarget.EQS_ScatterAroundTarget`

L 键节点链接到 K_Branch (`K2Node_IfThenElse_5`) 的 `.else` 出口：

```
K_Branch.else
  → L_WasInputKey(WasInputKeyJustPressed Key="L", self=GetPlayerController)
    → L_Branch(IfThenElse Condition=ReturnValue)
        .then →
          GetActorOfClass(NavTargetClass)        → CenterActor
          GetActorOfClass(NavLookTargetClass)    → LookActor
          GetAllActorsOfClass(MoverNPCClass)     → NPCArray
          ScatterNPCsAroundTarget(
            QueryAsset=ScatterQueryAsset, CenterActor=CenterActor,
            NPCs=NPCArray, LookTarget=LookActor)
          → PrintString("[Scatter BP] L key dispatched")
```

原 M 键流程保持不变（`bAllNPCs=true → ForEach → MoveAndLookAt`），作为对照基线。

## MCP / 构建踩坑

### 1. `EnvironmentQuery/EnvQueryRequest.h` 不存在

UE 5.7 里 `FEnvQueryRequest` 在 `EnvironmentQuery/EnvQueryManager.h` 同一文件里，不是独立头。include manager 即可。

### 2. Live Coding 失败但不显示具体错误

新增 `.h/.cpp` 文件后 `editor.trigger_build` 返回 failure，`get_compile_output` 只记 "Live coding failed, please see Live console for more information"。Live Coding 控制台是外部窗口，MCP 拿不到具体错误。**新增 .h/.cpp 文件首次编译走 UBT 全量更稳**，之后纯 .cpp 改动 Live Coding 能吃。

### 3. `build_eqs_query_from_spec` 的 "Object not found" warning 是误报

build 后 warning 提示 `Object not found: EnvQueryContext_Querier`，但 `get_eqs_query` 看实际数据 `GenerateAround/DistanceTo/CircleCenter/Context` 都正确解析为 `/Script/AIModule.EnvQueryContext_Querier`，`validate_eqs_query` 也 0 issue。warning 是诊断噪声，不必处理。

### 4. EQS Trace 测试的 context 属性叫 `Context`，distance 测试叫 `DistanceTo`

两个测试 context 字段不同名：`EnvQueryTest_Distance.DistanceTo`、`EnvQueryTest_Trace.Context`。

### 5. `set_variable_defaults` 的参数是 `default_value` 不是 `value`

API schema 字段叫 `default_value`，传 `value` 会被忽略（warning）。

### 6. `object:EnvQuery` 变量 default 用纯 asset path

`/Script/AIModule.EnvQuery'...'` 这种 class-style 写法 EQS asset 默认值不吃，会存成 `None`。**直接用纯 asset path** `/Game/AI/EQS/EQS_ScatterAroundTarget.EQS_ScatterAroundTarget` 才会持久化。注意与 `class:Actor` 变量需要 `/Script/Engine.BlueprintGeneratedClass'..._C'` 的写法不同。

### 7. `EnvQueryGenerator_OnCircle.PointOnCircleSpacingMethod` 在 MCP 下写不进

枚举属性通过 `configure_eqs_generator` / `build_eqs_query_from_spec` 设值不生效，`get_eqs_query` 永远显示空字符串（实际默认值是 `BySpaceBetween`=0）。绕法：用默认 `BySpaceBetween` 模式，搭配合理 `CircleRadius` + `SpaceBetween` 让候选点足够多，C++ 端再做点的最终选择（贪心匹配自动稀疏化）。

### 8. C++ 调用 BP 函数走 `FindFunction + ProcessEvent` + 内联 struct

参数 struct 字段顺序必须匹配 BP 函数（输入先、输出后）：

```cpp
struct FMoveAndLookAtLocationParams
{
    FVector MoveLocation = FVector::ZeroVector;
    AActor* LookTarget = nullptr;
    bool bSucceeded = false;
};
```

### 9. NavMesh `agent_radius` 是全局副作用

最初想把 `agent_radius` 35→65 让 NavMesh 离墙更远以解决 NPC 贴墙。但这是关卡级全局值，会影响 `BP_SmartBench` entrance 投影到 NavMesh 的位置（K 键 sit 依赖），可能破坏既有功能。改为在 EQS 加 Overlap filter 让**候选点本身**远离墙——只影响散点功能，不动其它系统。

## PIE 验证

已通过：

- 按 **M**（对照基线）：10 NPC 全部冲到 BP_NavTarget，到达后看 BP_NavLookTarget。未退化。
- 按 **L**：10 NPC 各自走向不同 NavMesh 位置，路径从一开始分叉，没有 NPC 走最远点（贪心匹配生效）。Output Log 出现 `[Scatter] firing EQS ... for 10 NPCs (skipped 1 player-controlled)` + per-NPC dispatch 日志。NPC 与墙保持 ~80cm 距离（Overlap filter 生效）。
- 按 **K**（坐下）/ **N** / **I** / **O**：既有按键链路未坏。
- Output Log 无 `LogPathFollowing` / `LogAIController` Failed/Aborted。

## 已知限制 / 后续

- EQS 异步：单击 L 后 200ms 内再按 L 会发起两次查询，后一次回调会覆盖前一次（`MoveToLocation` 内部取消旧请求）。验证时不快速连按。
- 散点半径 300 + Overlap 80 写在 EQS 资产里。如要改散点环大小，调 `CircleRadius`；如要更宽松的离墙距离，调 Overlap test 的 `ExtentX/Y/Z`。
- L 键是测试 hook，未来 LLM 决策层接入时整段 Level BP 删除，由 Mind 模块按需调 `ScatterNPCsAroundTarget`；C++ helper + BP 函数 + EQS 资产零改动复用。
