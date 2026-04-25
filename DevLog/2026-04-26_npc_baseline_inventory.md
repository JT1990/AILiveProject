# 2026-04-26 NPC Baseline Inventory（T00 DoD #6 / #10 / #11）

## 目的

T00 环境前置确认要求把当前 8 个场景 NPC 的 BP / 组件 / BeginPlay / T 键 / AIController / speech actor 全部盘下来作为 baseline。后续 M0 起的卡（MindComponent、SetFixedAndApply、ResolveSpeechActor、AIController.MoveTo）都依赖这份事实。

## 类型链（DoD #4）

```
BP_NPC_MH_Character_1..8     (壳 Pawn，每只一个；下面 8 只同形)
        ↓ parent
SandboxCharacter_Mover       (GASP Mover 2.0 character)
        ↓ parent
APawn                        (引擎)
```

✅ **Pawn 子类**——MoveTo / AIController / Perception 前提满足（CLAUDE.md P0 强约束 B）。

## 8 只 NPC 实例 BP（壳 Pawn）

`/Game/Blueprints/NPCs/BP_NPC_MH_Character_1..8`

### 共有形态

| 项 | 值 |
| --- | --- |
| Parent | `SandboxCharacter_Mover_C` |
| Compile status | UpToDate |
| Variables 数 | 1 |
| Functions 数 | 1（仅 `UserConstructionScript`） |
| Components 自身新增 | 0（全部继承自 `SandboxCharacter_Mover`） |
| Graph names | EventGraph + UserConstructionScript |
| has_tick | true（但 EventTick 节点禁用） |

### 唯一变量 `FixedVisualOverrideClass`（NPC 类别，instance editable）

每只指向各自的 visual override：

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

### BeginPlay（壳 Pawn 上）

`事件开始运行 → 父类:开始运行(Super) → SetFixedAndApply(AC_VisualOverrideManager, FixedVisualOverrideClass)`

✅ 与 CLAUDE.md "BeginPlay 时序" 中 `Super → SetFixedAndApply` 部分一致。**注意：壳 Pawn 这一层**没有 `PrewarmA2F`——它在 visual child 上（见下方）。M0 卡里 `MindComponent.Initialize` 应该挂在壳 Pawn 的 BeginPlay 末端、`SetFixedAndApply` 之后。

### CDO 关键属性（DoD #11）

继承自 `APawn`：

- `AIControllerClass` = `/Script/AIModule.AIModule.AIController`（**默认** AAIController，未自定义）
- `AutoPossessAI` = `PlacedInWorld`（放置时自动 possess，不需手动 SpawnDefaultController）
- `AutoPossessPlayer` = `Disabled`

✅ T18 / T19 / T19.7 的前提（`AAIController::MoveToActor` / Perception）成立——无需父类升级。

`AIC_NPC_SmartObject`（在 `Content/Blueprints/AI/`）存在但未被这些 NPC 引用——属于 GASP 模板遗留，本卡不动；如 T19.7 SmartObject 链路需要可考虑作为 AIController 自定义起点。

### 没有 T 键事件 / 没有 MindComponent 残留

EventGraph 仅 8 个节点，T 键的 TTS handler 不在壳 Pawn 上——而是在 visual child `BP_MH_Character_*` 的 BeginPlay 流程里挂的输入（见下方）。

## 8 只 visual child（speech actor）

`/Game/MetaHumans/MH_Character_1..8/BP_MH_Character_*`

抽样查 `BP_MH_Character_1`，其余应同形（MetaHuman 模板生成）。

### 类型链（DoD #10）

```
BP_MH_Character_1
    ↓ parent
AActor
```

是 Actor 子类（不是 Pawn）——这是 visual override 的标准 GASP 形态：壳 Pawn 持身体 ChildActorComponent，可见网格在 ChildActor 上。

### 关键组件（DoD #10）

```
Root (SceneComponent)
└─ Body (SkeletalMeshComponent)
   ├─ Face (SkeletalMeshComponent)
   │   ├─ Hair / Eyebrows / Fuzz / Eyelashes / Mustache / Beard (GroomComponent x6)
   │   └─ ACEAudioCurveSource (UACEAudioCurveSourceComponent)   ← A2F 角色契约第 1 项
   └─ SkeletalMesh / SkeletalMesh1 / SkeletalMesh2 (服装 / 配件)
LODSync (LODSyncComponent)
MetaHuman (MetaHumanComponentUE)
```

✅ **A2F 角色契约第 1 项达成**：`ACEAudioCurveSource` 作为 `Face` 的子组件已挂上。`TriggerMinimaxSpeech` 时不需要再 attach。

### Face AnimBP（DoD #10）

Face 组件 `AnimClass` = `/Game/MetaHumans/Common/Face/Face_Archetype_Skeleton_AnimBP_C`（MetaHuman 共享，8 只 NPC 共用同一份）。

✅ **A2F 角色契约第 2 项达成**：`Face_Archetype_Skeleton_AnimBP` 的 AnimGraph 含 `AnimGraphNode_ApplyACEAnimation`（"Apply ACE Face Animations"）。`TriggerMinimaxSpeech` 喂入的 PCM 经 `ACEAudioCurveSource` → ApplyACEAnimation → Face mesh，链路完整。

### BeginPlay（visual child 上）

`事件开始运行 → 启用输入(GetPlayerController) → PrewarmA2F → GetAvailableA2FProviders → 延迟到下个Tick → IsValid 分支 → IsValid 分支 → AddTickPrerequisiteComponent`

也带两个绑定事件 `On Anim Initialized (Body)` / `(Face)` → `LiveLinkSetup` → `SetUpdateAnimationInEditor`（Live Link / 编辑器内动画路径，PIE 不影响）。

✅ **PrewarmA2F 在这里**——CLAUDE.md "BeginPlay 时序" 描述的是壳 Pawn 上的顺序（`Super → PrewarmA2F → SetFixedAndApply → MindComponent.Initialize`），但实际工程里 `PrewarmA2F` 在 visual child 上调（`self` = visual actor，自带 ACE 组件）。功能正确——CLAUDE.md 文本应在 M0 卡同步收紧表述。

### T 键 TTS（DoD #6 baseline）

`InputKey T → GetMinimaxApiKeyFromProjectEnv → TriggerMinimaxSpeech`

✅ 当前 TTS 烟测路径正确：T 键事件接 `UMinimaxACELibrary::TriggerMinimaxSpeech`，self 即可见 actor，挂 `ACEAudioCurveSource`，Face AnimBP 含 `ApplyACEAnimation`——闭环。

EventGraph 里另存 `AnimateCharacterFromWavFileAsync` 链路（CreateAudio2FaceParameters → SetParametersFromStruct → AnimateCharacterFromWavFileAsync）但未被 T 键挂入执行链——属于早期 / 替代实现的死分支，**不在 T00 修改范围**，仅记录。

## 关键发现汇总（影响后续卡）

1. **Pawn 子类已落实**：M0 不需要插升 Pawn 卡，T18 / T19 / T19.7 可直接进。
2. **T 键 + TTS handler 在 visual child（不在壳 Pawn）**：T07 / T06 设计 `ResolveSpeechActor` 时直接走 `VisualOverride.GetChildActor()` 返回 visual child。
3. **MindComponent 应挂壳 Pawn**（持稳定 agent_id、AIController、Perception）；speech 转发到 visual child（持 ACE 组件）。两层职责分清。
4. **CLAUDE.md "BeginPlay 时序"两层重写建议**：
   - 壳 Pawn：`Super → SetFixedAndApply → MindComponent.Initialize`
   - Visual child（独立 BeginPlay）：`PrewarmA2F → ...`
   - M0 完成后由 T07 卡同步 CLAUDE.md。
5. **AIControllerClass 默认 AAIController**：T18 perception 组件加在哪里？惯例加在 AIController 上；可考虑用 `AIC_NPC_SmartObject` 做基类自定义，但这是 T18 设计点，T00 仅记录。
6. **AnimateCharacterFromWavFileAsync 死分支**留在 visual child EventGraph 里（CreateAudio2FaceParameters 链路未被任何 input 挂入执行）。本卡不删；M3 polish 阶段或 T15 接入时再清。
