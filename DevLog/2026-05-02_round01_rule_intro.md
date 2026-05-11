# ROUND01 游戏规则介绍

日期：2026-05-02

> 命名约定：与 Brain ↔ UE 协议 (`round_no`) 对齐，本仓库内部把章节统一称作 ROUND。Blueprint 资产 `BP_Act01Director`、关卡文件夹 `/Acts/Act01` 与 level 实例 `BP_Act01Director_C_0` 是历史命名，尚未在 UE 资产侧重命名；本 DevLog 提及它们时保留原名，新代码符号一律 Round01。

## 目标

ROUND01 是 Zombie Game 规则介绍的开场幕。按 `数字键1` 或 console `round01.start` 触发：1s 内打开牢门 → 10 NPC通过 EQS 散点到电视前 → `MediaPlate` 播放 `game-introduction.mp4` → 视频结束完成。

## 触发方式

- 按键：`数字键1`（`StartKey` UPROPERTY 可改）
- Console：`round01.start` / `round01.cancel`（运行时自动注册到 `IConsoleManager`）

## 状态机

```
Idle
  ↓ press 1 / round01.start
Reset NPC transforms + close doors
  ↓
OpeningDoors (1s lerp, 10 SM_Door RelativeRotation 0 → Yaw=-120)
  ↓
NPCsMovingToTV (EQS scatter dispatch + 2s settle delay + AreAllNPCsIdle wait)
  ↓
PlayingVideo (MediaPlate Open 异步 → IsReady → Play → 等 OnEndReached)
  ↓
Idle (Broadcast OnRound01Completed)
```

## 资产改动

### C++

- `Source/AILiveProject/Public/Rounds/Round01RuleIntroDirector.h`（新）
- `Source/AILiveProject/Private/Rounds/Round01RuleIntroDirector.cpp`（新）
- `Source/AILiveProject/AILiveProject.Build.cs`：新增 `"MediaAssets"` + `"MediaPlate"` 依赖

### 资产

- `Content/Blueprints/Acts/BP_Round01Director`：C++ 类的 placeable wrapper，CDO 持软引用与配置。父类已 reparent 到 `ARound01RuleIntroDirector`（依赖 `DefaultEngine.ini` 里的 CoreRedirect 让旧 .uasset 找到新 C++ 类）。Content Browser 文件夹保留旧名 `/Game/Blueprints/Acts/`（仅 Content Browser 展示，不影响运行）。
- `Content/Prison/Blueprints/BP_cell`：`SM_Door.bCanEverAffectNavigation = False`（关键 nav 修复，见踩坑#2）
- `Content/MyAssets/Levels/L_prison.umap`：放 `BP_Act01Director_C_0` 实例（class 已是 `BP_Round01Director_C`，internal FName 与 label 保留旧名，无运行时影响），World Outliner 文件夹 `/Acts/Act01` → `/Rounds/Round01`

### 复用

- `BP_NavTarget_1`（电视前散点中心）
- `BP_NavLookTarget_TV`（电视位置看向目标）
- `MediaPlate_1`（视频屏幕，已绑 `Content/Videos/game-introduction.mp4` 作 MediaSource）
- `EQS_ScatterAroundTarget` 资产 + `UAILiveProjectScatterMover::ScatterNPCsAroundTarget`（DevLog 2026-04-29_npc_scatter_to_target.md）
- `SandboxCharacter_Mover.MoveAndLookAtLocation`（BP 函数，scatter mover 内部反射调用）

## 关键 UPROPERTY（BP CDO 持有）

| 字段                          | 默认                              | 说明                      |
| ----------------------------- | --------------------------------- | ------------------------- |
| `StartKey`                    | One                               | 触发键                    |
| `bEnableKeyTrigger`           | true                              | 关闭后仅 console 触发     |
| `NPCMoverClass`               | `SandboxCharacter_Mover_C`        | NPC 过滤类                |
| `NavTargetActor` (soft)       | `BP_NavTarget_C_1`                | 散点中心                  |
| `NavLookTargetActor` (soft)   | `BP_NavLookTarget_C_0`            | 看向目标                  |
| `MediaPlateActor` (soft)      | `MediaPlate_1`                    | 视频屏幕                  |
| `ScatterQueryAsset`           | `EQS_ScatterAroundTarget`         | 散点 EQS                  |
| `CellDoorActorNames`          | 10 个 `SM_blocking_prison_Cube_*` | 门 actor 标签             |
| `CellDoorMeshComponentName`   | `SM_Door`                         | 门子组件名                |
| `DoorOpenRelativeRotation`    | `(0, -120, 0)`                    | 相对开门旋转              |
| `DoorAnimationSeconds`        | 1.0                               | 开门时长                  |
| `MovementTimeoutSeconds`      | 30.0                              | 移动总超时                |
| `ScatterDispatchDelaySeconds` | 2.0                               | EQS 派发延迟，防误判 idle |
| `ArrivalSettleSeconds`        | 0.5                               | 到达后静默等待            |
| `VideoFallbackTimeoutSeconds` | 600.0                             | 视频总超时                |

软引用（NavTarget / NavLookTarget / MediaPlate）支持 fallback：`LoadSynchronous` 失败 → `GetAllActorsOfClass` + Label 匹配 → 退化 first instance。BeginPlay 和 BeginRound01 各执行一次。

## 关键技术决策

### 1. 门旋转用相对而非世界坐标系

cell 上排（270/269/268/259/271/351）actor world rotation `(0, 180, 0)`，下排（285/286/352/353）`(0, 0, 0)`，scale 都是 `[-1, 1, 1]`。

- **世界空间** Yaw=-120：所有门绕世界 Z 同方向转，由于上下排 cell 朝向相反 → 半数外开半数内开。
- **相对空间** Yaw=-120：每扇门绕自己 cell 本地 Z 转，cell 朝向自动决定旋转方向 → 10 扇门统一向走廊外开。

实现：`SetRelativeRotation(Lerp(ClosedRel, ClosedRel + DoorOpenRelativeRotation, alpha))`。

### 2. 视频播放：Open 异步必须等 IsReady

`UMediaPlateComponent::Open()` 是异步的，立即 `Play()` 因媒体未就绪 silent fail。MediaPlate 实例 `bPlayOnOpen=false`，也不能依赖 Open 自动 Play。

`StartVideo` + `TickVideoFallback` 完整流程：

1. 验证 `Playlist->Get(0)` 拿到 MediaSource
2. `SetActorHiddenInGame(false)` + `StaticMeshComponent->SetVisibility(true, true)`（防御性还原）
3. `Player->OnEndReached.AddUniqueDynamic(...)`
4. `PlateComponent->Close()` 清旧状态
5. `SetLoop(false)` + `bPlayOnOpen=false`
6. `Open()` 异步开始
7. Tick 轮询 `Player->IsReady() && !Player->IsPlaying()` → 调 `MediaPlateComponent->Play()` + `Player->Play()`
8. `OnEndReached` → `CompleteRound01`
9. 15s ready 超时 / `VideoFallbackTimeoutSeconds` 全程超时 fallback

### 3. NPC 走位用 EQS 散点

10 NPC 都到同一 NavTarget 会挤一起。复用 `UAILiveProjectScatterMover::ScatterNPCsAroundTarget`（DevLog `2026-04-29_npc_scatter_to_target.md`），EQS 在 NavTarget 周围生成散点，每个 NPC 独立位置 + 都看向 LookTarget。

`ScatterDispatchDelaySeconds=2.0s` 是关键：EQS 异步派发，2s 内 Tick 不允许触发 "all idle → 进入视频" 判定，否则 NPC 还没动就被认为已到达。

### 4. NavMesh：门必须从 navigation 排除

cell 关门时门 collision 阻挡 NavMesh 烤制，NPC 在 cell 内无法寻路出去（path partial）。`BP_cell.SM_Door.bCanEverAffectNavigation = False` 让 NavMesh 烤制忽略门 collision，开关门只影响视觉/物理，不影响导航。

改完必须在编辑器 `Build > Build Paths` 重烤 NavMesh。

### 5. NPC 起始位置缓存

BeginPlay 时枚举 `NPCMoverClass` 实例缓存 `TMap<TWeakObjectPtr<AActor>, FTransform>`。每次按 1 触发先 `SetActorTransform` 拉回 + `AIController::StopMovement`，反复测试 ROUND01 不需要 stop/start PIE。

注意 `NPCMoverClass=SandboxCharacter_Mover_C` 也会抓到玩家 Pawn，ScatterMover 内部已 `IsPlayerControlled()` 过滤。

## MCP / 构建踩坑

### 1. UBT 全量构建必须（不是 Live Coding）

新增 `MediaAssets`/`MediaPlate` 模块依赖、新增 UPROPERTY 字段（`ScatterQueryAsset`、`ScatterDispatchDelaySeconds`、`NavTargetClass`、`NavLookTargetClass`、`NavTargetActorLabel`、`NavLookTargetActorLabel`）都要 UHT regen + 链接器重链。Live Coding 接受不了新依赖和新 reflection schema。

流程：关 UE → `Build.bat AILiveProjectEditor Win64 Development -Project=...` → 起 UE → 等 monolith online。

### 2. NavMesh 门 collision 阻挡

`BP_cell.SM_Door` 默认 `bCanEverAffectNavigation=True` + 关门状态 → NavMesh 在 cell 内部和走廊之间烤不通。NPC 起点在 cell 内 → `MoveToLocation` 返回 partial path → `bAllowPartialPath=false`（既有 `MoveAndLookAt` BP 函数固定值）→ 失败。

修法：set BP_cell.SM_Door `bCanEverAffectNavigation=False`，编辑器 `Build > Build Paths` 重烤。`build_navmesh` MCP 调用单独不够（只刷新 dynamic 部分），需要走编辑器 Build Paths。

### 3. C++ 类无法直接 spawn 到关卡

`mesh_query::spawn_actor` 只接 StaticMesh；`place_blueprint_actor` 拒绝 `/Script/...` C++ 类路径。绕路：`blueprint_query::create_blueprint` 建空 BP（parent_class=`/Script/AILiveProject.Round01RuleIntroDirector`） → `place_blueprint_actor` 放 BP 实例。

### 4. `set_actor_properties` 6 字段限制

placed actor 的 `set_actor_properties` 只接受 `mobility/simulate_physics/collision_preset/cast_shadow/tags/mass_kg`，自定义 UPROPERTY 写不进。改用 `set_cdo_property` 在 BP CDO 写默认值（本里程碑只一个 actor 实例，CDO 默认即生效）。

### 5. TSoftObjectPtr 到 level actor 在 PIE 中需 fallback

BP CDO 写软引用 `/Game/MyAssets/Levels/L_prison.L_prison:PersistentLevel.BP_NavTarget_C_1`，编辑器世界能 `LoadSynchronous` 解析；PIE 把关卡复制到临时 world，原路径不存在 → 返回 null。

`ResolveTargetActors` 加 fallback：解析失败 → `GetAllActorsOfClass(NavTargetClass)` + label 匹配（`NavTargetActorLabel="BP_NavTarget_1"`）→ 仍失败退化 first instance。BeginPlay 和 BeginRound01 各调一次。

### 6. cell label 匹配只用 GetActorNameOrLabel，别用 FName

UE level 序列化时 actor FName 与 label 错位：`SM_blocking_prison_Cube_285` 的实际 `GetFName()` 是 `SM_blocking_prison_Cube_10`（创建顺序编号），`GetActorNameOrLabel()` 才是 `SM_blocking_prison_Cube_285`。

之前同时检查 `GetFName()` 和 `GetActorNameOrLabel()` 命中 wanted set，导致 FName 偶然匹配的 actor（如 label `SM_blocking_prison_Cube_210`）误进缓存触发 false-positive warning。改为只比对 `GetActorNameOrLabel()`，FName 完全无视。

### 7. MediaPlate 实例 `bPlayOnOpen=false` 是 level instance default

UE 5.7 引擎 source 中 `UMediaPlateComponent::bPlayOnOpen` 默认 `true`，但 L_prison 中已放置的 MediaPlate_1 实例 default 覆盖为 `false`。代码只调 `Open()` 假设自动 Play → 视频永不播。

修法：显式 `bPlayOnOpen=false` + 在 `IsReady()` 后手动 `MediaPlateComponent->Play() + Player->Play()`，逻辑明确不依赖 default。

### 8. `IConsoleCommand` 是 struct 不是 class

前向声明必须 `struct IConsoleCommand;`，否则 UHT 报 `C4099` ABI mismatch。`IConsoleManager::Get().RegisterConsoleCommand + FConsoleCommandDelegate::CreateUObject` 注册。

### 9. EndPlay 时务必 UnregisterConsoleObject

PIE 重启时同名 console command 会冲突累加。`EndPlay` 调 `UnregisterDebugConsoleCommands`，`StartCommand` / `CancelCommand` 指针记得清空。

## PIE 验证（已通过 2026-05-02）

按 `数字键1` 或 console `round01.start`：

1. 屏幕 `[Round01] reset N NPC(s) and 10 door(s) to initial state`（白）
2. 1s 内 10 扇门 RelativeRotation `(0,0,0)` → `(0,-120,0)` 平滑 lerp，统一向走廊外开
3. NPC 通过 EQS 散点到 NavTarget 附近不同位置，到达后转身朝 BP_NavLookTarget_TV
4. 屏幕 `[Round01] video open requested: <url>`（绿）
5. 视频加载完后屏幕 `[Round01] video started: <url>`
6. 视频播放结束 → `[Round01] video OnEndReached fired` → `[Round01] complete`

旧 demo（M / I / N / K / O）触发链路保持工作。

### 关键日志

```
LogRound01: [Round01] BeginPlay cached N NPC(s), 10 cell door(s)
LogRound01: [Round01] resolve: NavTarget=... NavLook=... MediaPlate=...
LogRound01: [Round01] StartKey pressed via PC
LogRound01: [Round01] reset N NPC(s) and 10 door(s) to initial state
LogRound01: [Round01] opening cell doors
LogRound01: [Round01] dispatched N NPC(s) via EQS scatter around NavTarget
LogAILiveScatter: [Scatter] firing EQS '...' with Querier=... for N NPCs (skipped K player-controlled)
LogRound01: [Round01] video open requested: file://...
LogRound01: [Round01] waiting for video open elapsed=1.0s url=...
LogRound01: [Round01] video started: file://...
LogRound01: [Round01] video OnEndReached fired
LogRound01: [Round01] complete
```

## 已知限制

- `bCanEverAffectNavigation=False` 让门视觉关上 nav 仍通——AI 路径完全不受门状态影响。如未来需要"门关时阻挡 NPC"，要改用动态 NavMesh 或在门加 `NavModifier`。
- 缓存 NPC 初始位置在 BeginPlay 拍快照。运行时移动 NPC 后再按 1 会被拉回 BeginPlay 时位置。
- `ScatterDispatchDelaySeconds=2.0s` 是经验值。EQS 异步通常 < 100ms，但首次 NavMover possess 慢时可能更久。
- `MediaPlate.bPlayOnOpen=false` 是当前 level instance state；如有人改回 true，逻辑仍工作（Play 重复调用幂等）。
- 视频文件 `Content/Videos/game-introduction.mp4` 打包时需确认 cook/stage（`PackagingSettings.AdditionalNonAssetDirectoriesToCopy` 或 cooked-on-load）。

## 后续

- ROUND02 落地时根据形态决定是否抽 `ARoundChapterBase`（共用 StartKey 轮询、Reset transforms、ScatterMover 派发、MediaPlate 播放）
- LLM Mind 模块接入后 Director 改为 LLM tool entry，按键 + 控制台命令逐步移除
- 待手工：Content Browser 文件夹 `/Game/Blueprints/Acts/` 移到 `/Game/Blueprints/Rounds/`（MCP 没暴露跨文件夹的 asset move；不影响运行，下次开 UE 拖动即可）。
- 待手工：level 实例 internal FName `BP_Act01Director_C_0` 与 World Outliner label `Act01Director`（class 已是 `BP_Round01Director_C` 不影响运行）。
- `DefaultEngine.ini` 末尾的 `[CoreRedirects]` 把旧 C++ 类 `Act01RuleIntroDirector` 映射到新 `Round01RuleIntroDirector`，主要给 .uasset 持有的旧 class 引用做兼容。BP 与 level 已重新保存指向新名，redirector 仅起防御作用，未来确认无遗留再清理。
