# 2026-04-26 NPC Baseline Inventory

## 目的

记录当前 8 个场景 NPC 的 BP / 组件 / BeginPlay / T 键 / AIController / speech actor 基线。后续如果 DevLog 或手册与资产状态冲突，以资产状态为准并更新本文。

## 8 个 NPC 壳 Pawn

`/Game/Blueprints/NPCs/BP_NPC_MH_Character_1..8`

类型链：

```
BP_NPC_MH_Character_1..8
    -> SandboxCharacter_Mover
    -> APawn
```

结论：这些 NPC 已是 Pawn 子类，满足 `MoveTo` / `AIController` / AI Perception 的基础前提；不需要再做 Actor -> Pawn 升级。

### 共有形态

| 项 | 值 |
| --- | --- |
| Parent | `SandboxCharacter_Mover_C` |
| Compile status | UpToDate |
| Variables 数 | 1 |
| Functions 数 | 1，仅 `UserConstructionScript` |
| Components 自身新增 | 0，全部继承自 `SandboxCharacter_Mover` |
| Graph names | EventGraph + UserConstructionScript |
| has_tick | true，但 EventTick 节点禁用 |

### `FixedVisualOverrideClass`

每个 NPC 壳 Pawn 持有一个 instance editable 的 `FixedVisualOverrideClass`，指向对应 MetaHuman visual override：

| BP | FixedVisualOverrideClass |
| --- | --- |
| BP_NPC_MH_Character_1 | `/Game/MetaHumans/MH_Character_1/BP_MH_Character_1.BP_MH_Character_1_C` |
| BP_NPC_MH_Character_2 | `/Game/MetaHumans/MH_Character_2/BP_MH_Character_2.BP_MH_Character_2_C` |
| BP_NPC_MH_Character_3 | `/Game/MetaHumans/MH_Character_3/BP_MH_Character_3.BP_MH_Character_3_C` |
| BP_NPC_MH_Character_4 | `/Game/MetaHumans/MH_Character_4/BP_MH_Character_4.BP_MH_Character_4_C` |
| BP_NPC_MH_Character_5 | `/Game/MetaHumans/MH_Character_5/BP_MH_Character_5.BP_MH_Character_5_C` |
| BP_NPC_MH_Character_6 | `/Game/MetaHumans/MH_Character_6/BP_MH_Character_6.BP_MH_Character_6_C` |
| BP_NPC_MH_Character_7 | `/Game/MetaHumans/MH_Character_7/BP_MH_Character_7.BP_MH_Character_7_C` |
| BP_NPC_MH_Character_8 | `/Game/MetaHumans/MH_Character_8/BP_MH_Character_8.BP_MH_Character_8_C` |

### BeginPlay（壳 Pawn）

```
Event BeginPlay -> Parent BeginPlay -> SetFixedAndApply(AC_VisualOverrideManager, FixedVisualOverrideClass)
```

壳 Pawn 不调用 `PrewarmA2F`。`PrewarmA2F` 在 visual child 上调用。

### CDO 关键属性

继承自 `APawn`：

- `AIControllerClass` = `/Script/AIModule.AIModule.AIController`，默认 `AAIController`
- `AutoPossessAI` = `PlacedInWorld`
- `AutoPossessPlayer` = `Disabled`

`AIC_NPC_SmartObject` 存在于 `Content/Blueprints/AI/`，但当前这些 NPC 没有引用它；这是 GASP 模板遗留，不属于当前 TTS/A2F 链路。

## 8 个 visual child（speech actor）

`/Game/MetaHumans/MH_Character_1..8/BP_MH_Character_*`

抽样检查 `BP_MH_Character_1`，其余由同一 MetaHuman 模板生成，应保持同形。

类型链：

```
BP_MH_Character_1
    -> AActor
```

visual child 是 Actor 子类，不是 Pawn。这是 visual override 的标准形态：壳 Pawn 持有身体 ChildActorComponent，可见网格在 ChildActor 中。

### 关键组件

```
Root (SceneComponent)
└─ Body (SkeletalMeshComponent)
   ├─ Face (SkeletalMeshComponent)
   │  ├─ Hair / Eyebrows / Fuzz / Eyelashes / Mustache / Beard (GroomComponent x6)
   │  └─ ACEAudioCurveSource (UACEAudioCurveSourceComponent)
   └─ SkeletalMesh / SkeletalMesh1 / SkeletalMesh2
LODSync (LODSyncComponent)
MetaHuman (MetaHumanComponentUE)
```

结论：A2F 角色契约第 1 项达成，`ACEAudioCurveSource` 作为 `Face` 的子组件已挂上。`TriggerMinimaxSpeech` 时不需要再 attach。

### Face AnimBP

Face 组件 `AnimClass` = `/Game/MetaHumans/Common/Face/Face_Archetype_Skeleton_AnimBP_C`，8 个 NPC 共用同一份。

结论：A2F 角色契约第 2 项达成，Face AnimBP 的 AnimGraph 含 `AnimGraphNode_ApplyACEAnimation`。`TriggerMinimaxSpeech` 喂入的 PCM 经 `ACEAudioCurveSource` -> `ApplyACEAnimation` -> Face mesh，链路完整。

### BeginPlay（visual child）

```
Event BeginPlay
-> EnableInput(GetPlayerController)
-> PrewarmA2F
-> GetAvailableA2FProviders
-> Delay Until Next Tick
-> IsValid branch
-> IsValid branch
-> AddTickPrerequisiteComponent
```

还包含 `On Anim Initialized (Body)` / `(Face)` -> `LiveLinkSetup` -> `SetUpdateAnimationInEditor`，用于 Live Link / 编辑器内动画路径，不影响 PIE TTS 烟测。

### T 键 TTS

```
InputKey T -> GetMinimaxApiKeyFromProjectEnv -> TriggerMinimaxSpeech
```

当前 TTS 烟测路径正确：T 键事件接 `UMinimaxACELibrary::TriggerMinimaxSpeech`，`self` 即可见 actor，挂 `ACEAudioCurveSource`，Face AnimBP 含 `ApplyACEAnimation`，闭环完整。

EventGraph 里另有未接入 T 键执行链的 `AnimateCharacterFromWavFileAsync` 路径（`CreateAudio2FaceParameters` -> `SetParametersFromStruct` -> `AnimateCharacterFromWavFileAsync`）。这是早期替代实现的死分支，当前不影响 TTS/A2F 主链。

## 当前结论

1. `BP_NPC_MH_Character_1..8` 是壳 Pawn，负责承载 GASP/Mover、AIController、VisualOverride。
2. `BP_MH_Character_1..8` 是 visual child，负责可见 MetaHuman、ACEAudioCurveSource、Face AnimBP、T 键 TTS。
3. A2F 主链为 `GetMinimaxApiKeyFromProjectEnv -> TriggerMinimaxSpeech -> ACEAudioCurveSource -> ApplyACEAnimation`。
4. `PrewarmA2F` 在 visual child BeginPlay 上调用，不在壳 Pawn 上调用。
5. 壳 Pawn 的 BeginPlay 保持 `Super -> SetFixedAndApply`，不要删两层 `IsValid` 分支。
