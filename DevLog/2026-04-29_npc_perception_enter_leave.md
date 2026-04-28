# NPC 视野进入/离开事件 + 上次见到位置（C++ SightMemoryComponent + 验证剧情）

日期：2026-04-29

## 目标

- NPC1 检测到目标 NPC 进入 / 离开自己视野时立即触发事件（不靠快照轮询）
- 保留"上次见到位置"作为 Mind 模块决策的基础数据
- 用 NPC1↔NPC2 餐厅相遇剧情（按 O 键触发）端到端验证：NPC2 走到 NPC1 正前方 → 6 句对话 → 等 3s → 离场归位
- C++ component 状态机，Mind 模块未来零改动复用

## 技术决策

**C++ `USightMemoryComponent` 挂在 `AIC_NPC_SmartObject` AIController BP 上**，绑定原生 `UAIPerceptionComponent::OnTargetPerceptionUpdated` 多播 delegate 维护 per-NPC 状态。

为什么挂 AIController 而非 Pawn 父类：CLAUDE.md "GASP/Mover/父类保持原样"——感知层归 AIController，不动 SandboxCharacter_Mover。

为什么不用纯 BP polling：Plan agent 评估 Monolith MCP 的 `K2Node_AddDelegate` 不支持绑定 multicast delegate（generic fallback 没 SetFromProperty 路径），原生 `OnTargetPerceptionUpdated` 走不通；BP 状态在 Level scope，C++ Mind 模块不能直接访问，Mind 接管时必须重写状态机；C++ component 实现 100% 复用。

**Live Coding 反射坑规避**：本里程碑加 1 个新 UCLASS + 2 个 BP-assignable delegate（无 USTRUCT）。由于既有 BP 不依赖该新类，原本计划走 UBT 全量重建以避免 DevLog 2026-04-29 hearing #2 #3 的 Live Coding 反射缓存坑——实测 `editor_query::trigger_build` 调用 Live Coding 即 `patch_applied=true, errors=0` 通过，无需关编辑器。**例外**：编译 Level BP 时既有 `K2Node_CallFunction_20` (TriggerMinimaxSpeechWithNoise N 键 TTS) 和 `K2Node_CallFunction_19` (GetMinimaxApiKeyFromProjectEnv) **self pin 卡死在 BPGC archetype stale 引用**——加新代码触发反射重建后旧节点的 self pin 旧 BPGC 路径无效，必须 `remove_node + add_node` 重建并重新接线。这是 DevLog hearing #6 已记录踩坑模式的复发。

**IAILiveAgent 过滤必须在 component 内部做**。`UAIPerceptionComponent::OnTargetPerceptionUpdated` 触发的 actor 包括所有可被感知的 stimulus source（玩家 Pawn、NPC、其它 SandboxCharacter_Mover_C 实例）。component 内仅过滤 `Stimulus.Type==Sight` + `Actor==OwnerPawn`（自感知）不够——必须复用 `GatherSightPerception` 的 `IsAgentActor` 模式：`Actor->GetClass()->ImplementsInterface(UAILiveAgent::StaticClass())`。否则玩家 Pawn 也会进 Mind 输入。

**`bDebugPrintScreen` 默认 false**。component 挂 AIC_NPC_SmartObject 后所有 8 NPC 都有该 component。PIE 启动时所有 NPC 互相 ENTER 同伴，屏显会刷屏。Level BP 在 O 键剧情入口才对 NPC1 component 调 `Set bDebugPrintScreen=true`。其它 NPC 屏显始终关闭。`UE_LOG(LogSightMemory, Log, ...)` 始终输出，调试用 OutputLog category filter `LogSightMemory` 聚焦。

**WeakObjectPtr 容器**。内部 `TMap<TWeakObjectPtr<AActor>, FVector> LastSeenLocations` + `TSet<TWeakObjectPtr<AActor>>` 不加 UPROPERTY（WeakRef 不需 GC visibility）。查询 API 遍历时 `IsValid()` 跳过 stale。Mind 接管时若需 sweep stale entries，每 N 秒清理一次（暂未实现，8 NPC 场景无问题）。

**BeginPlay PerceptionComponent 未就绪 retry timer**：BeginPlay 第一次 `Cast<AAIController>(GetOwner())->GetPerceptionComponent()` 可能为 nullptr（component 初始化顺序在 AIController BeginPlay 之前）。固定策略 = retry timer (0.1s × 30 次 = 3s 上限)。失败后 `UE_LOG(LogSightMemory, Warning)`。

## 资产改动

### 1. C++ 新增 `USightMemoryComponent`

文件：
- `Source/AILiveProject/Public/SightMemoryComponent.h`
- `Source/AILiveProject/Private/SightMemoryComponent.cpp`

接口：

```cpp
DECLARE_LOG_CATEGORY_EXTERN(LogSightMemory, Log, All);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnSightEnterDelegate, AActor*, Other, FVector, Location);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnSightExitDelegate, AActor*, Other, FVector, LastSeenLocation);

UCLASS(ClassGroup=(AILiveProject), meta=(BlueprintSpawnableComponent), Blueprintable)
class USightMemoryComponent : public UActorComponent {
    UPROPERTY(EditAnywhere, BlueprintReadWrite) bool bDebugPrintScreen = false;
    UPROPERTY(BlueprintAssignable) FOnSightEnterDelegate OnSightEnter;
    UPROPERTY(BlueprintAssignable) FOnSightExitDelegate OnSightExit;
    UFUNCTION(BlueprintCallable, BlueprintPure) bool GetLastSeenLocation(AActor*, FVector&) const;
    UFUNCTION(BlueprintCallable, BlueprintPure) bool IsCurrentlyVisible(AActor*) const;
    UFUNCTION(BlueprintCallable) void GetCurrentlyVisibleActors(TArray<AActor*>&) const;
    UFUNCTION(BlueprintCallable) void GetAllLastSeenLocations(TMap<AActor*, FVector>&) const;
};
```

`OnPerceptionUpdated` 4 步过滤（顺序）：
1. `Actor == nullptr` → return
2. `Stimulus.Type != UAISense::GetSenseID<UAISense_Sight>()` → return
3. `Actor == OwnerAIC->GetPawn()` → return（自感知）
4. `!Actor->GetClass()->ImplementsInterface(UAILiveAgent::StaticClass())` → return

ENTER 触发：`bWasVisible = false, WasSuccessfullySensed=true` → `OnSightEnter.Broadcast` + `LastSeenLocations.Add` + `CurrentlyVisibleActors.Add` + `UE_LOG`/屏显。
EXIT 触发：`bWasVisible = true, WasSuccessfullySensed=false` → `OnSightExit.Broadcast(LastSeenLocations[Actor])` + `CurrentlyVisibleActors.Remove`（**保留 LastSeenLocations 不清——这就是"上次见到位置"持久数据**）。

`EndPlay` unbind delegate + ClearTimer。

### 2. AIC_NPC_SmartObject 加 SightMemoryComponent

`mcp__monolith__blueprint_query::add_component asset_path=/Game/Blueprints/AI/AIC_NPC_SmartObject component_class=/Script/AILiveProject.SightMemoryComponent component_name=SightMemory`。8 个 NPC 子类的 AIController 都用 AIC_NPC_SmartObject_C，自动持有该 component 实例。

### 3. BP_StoryDummy

`Content/Blueprints/Markers/BP_StoryDummy.uasset` (parent=Actor, 无组件)。剧情专用 dummy actor 类，Level BP `SpawnActorFromClass` 用，避免污染 M 键的 BP_NavTarget / BP_NavLookTarget。

### 4. L_prison Level BP

新增 17 个变量（category=SceneStory）：`bSceneInitialized` / `bSceneRunning` / `SceneWaitTickCounter` / `StoryDummyClass` / 6 个 DialogueText / `VoiceId_NPC1` (`male-qn-qingse`) / `VoiceId_NPC2` (`male-qn-jingying`) / `NPC1FrontDummy` / `NPC2HomeDummy` / `NPC2OriginalLocation` / `NPC1Cached` / `NPC2Cached`（后两者类型 `object:SandboxCharacter_Mover_C`，访问 `VisualOverride / bAligningLook / MoveAndLookAt`）。

新增 1 个 BP Function `Speak(InNPC: SandboxCharacter_Mover_C, InText: String, InVoiceId: String)`：内部 `GetComponentByClass(InNPC, ChildActorComponentClass) → DynamicCast<ChildActorComponent> → GetChildActorOf → VisibleActor` + `GetMinimaxApiKeyFromProjectEnv → ApiKey` + `TriggerMinimaxSpeechWithNoise(VisibleActor, InNPC, InText, ApiKey, InVoiceId, Endpoint, A2FProviderName)`。被 `Scene_StartDialogue` 调用 6 次以最小化重复布线。

新增 5 个 EventGraph CustomEvent：

- **RunDialogueScene_Open**：用 `Sequence` 节点分支。`then_0` 走懒初始化（`Branch(bSceneInitialized).else` → SpawnActor BP_StoryDummy ×2 + Set NPC1FrontDummy/NPC2HomeDummy + Set bSceneInitialized=true）；`then_1` 走主流程（Set bSceneRunning=true + Set SceneWaitTickCounter=0 + GetActorOfClass NPC1/NPC2 + Cast SandboxCharacter_Mover_C + Set NPC1Cached/NPC2Cached + 缓存 NPC2OriginalLocation + 计算 `NPC1.Loc + NPC1.Forward * 250` → SetActorLocation NPC1FrontDummy + SetActorLocation NPC2HomeDummy + `NPC2Cached.MoveAndLookAt(NPC1FrontDummy, NPC1Cached)` → Branch(bSucceeded) → 失败打印 + Set bSceneRunning=false / 成功 K2_SetTimer "Scene_WaitForArrival" 0.2s loop）。

- **Scene_WaitForArrival**（带 30s 超时）：每次 SceneWaitTickCounter+1，>150 → ClearTimer + PrintString "[Scene] arrival timeout" + Set bSceneRunning=false；否则 Branch(NPC2.bAligningLook) → ClearTimer + Set SceneWaitTickCounter=0 + Call Scene_StartDialogue。**bAligningLook 节点 MCP 接不上，需用户手动接（见下）**。

- **Scene_StartDialogue**：`Delay(1s) → Speak(NPC2Cached, "我们两个结盟吧。", VoiceId_NPC2) → Delay(6s) → Speak(NPC1Cached, "好的。", VoiceId_NPC1) → Delay(6s) → ... × 6 句 → Delay(6s 播完) → Delay(3s 任务要求) → Call Scene_NPC2Leave`。

- **Scene_NPC2Leave**：`NPC2Cached.MoveAndLookAt(NPC2HomeDummy, NPC2HomeDummy) → Branch(bSucceeded)` → 失败打印 + Set bSceneRunning=false / 成功 Set SceneWaitTickCounter=0 + K2_SetTimer "Scene_WaitForLeaveComplete" 0.2s loop。

- **Scene_WaitForLeaveComplete**（带 30s 超时）：与 WaitForArrival 同结构，不同点：成功路径 Set bSceneRunning=false + PrintString "[Scene] complete; expect [SightMemory] EXIT NPC2"。

O 键链路挂在既有 N 键 IfThenElse `.else` 末端：`IfThenElse_3.else → IfThenElse(WasInputKeyJustPressed("O")).then → Branch(bSceneRunning).false → CallFunction RunDialogueScene_Open`。

M 键互斥：原 `IfThenElse_0.then → CallFunction_2 (GetActorOfClass NavTarget)` 改为 `IfThenElse_0.then → IfThenElse(bSceneRunning).false → CallFunction_2`，剧情中按 M 不触发。

## MCP 实施踩坑

### 1. add_variable 参数名是 `name` / `type`，不是 `variable_name` / `variable_type`

CLAUDE.md 已记 MCP 参数名坑（`value` 不是 `property_value`，`asset_path` 不是 `blueprint_path`）。变量加上：`add_variable(asset_path, name, type, default_value, category)`。

### 2. add_node `node_type` 限制清单

支持类型：CallFunction、VariableGet、VariableSet、CustomEvent、Branch、Sequence、MacroInstance、SpawnActorFromClass、DynamicCast、Self、Return、MakeStruct、BreakStruct、SwitchOnEnum/Int/String、FormatText、MakeArray、Select。也接受任意 UK2Node_ 类名作为 generic fallback。

`Add_IntInt` / `Greater_IntInt` / `Multiply_VectorFloat` 等数学操作不是 node_type，**用 `CallFunction` + `function_name` + `target_class=/Script/Engine.KismetMathLibrary`**。

`Delay` 同样：`CallFunction function_name=Delay target_class=/Script/Engine.KismetSystemLibrary`，pin: execute / then / Duration（默认 0.2）。

`SpawnActorFromClass` 需要 `actor_class` 参数（路径或类名），不是 `target_class`。

`DynamicCast` 需要 `cast_class` 参数。

### 3. add_node VariableGet 不支持引用外部 BP 类成员

`VariableGet bAligningLook target_class=SandboxCharacter_Mover_C` 返回的节点 pins 为空——MCP 只在当前 BP 自身变量列表查找，外部 BP 类的实例成员变量找不到。同样的限制对 SightMemoryComponent 的 `bDebugPrintScreen` BP 设置也存在（用 BP 的 "Get bDebugPrintScreen" 节点改 component 实例属性）。

**绕过方案**：MCP 加完所有其他节点 + 留 placeholder（CommentNode 标注），用户在 BP 编辑器右键 `Get NPC2Cached` 输出 → 选 "Get bAligningLook"，连到 `Branch_Aligned` 的 Condition pin。本里程碑用此方案。

### 4. UE 5.7 SpawnActorFromClass.SpawnTransform 必须连接

`SpawnActorFromClass.SpawnTransform` 是 by-ref 参数（FTransform），UE 5.7 起强制要求显式连接（不接受 pin default）。**绕过**：加一个 `MakeTransform(KismetMathLibrary, default identity)` 节点输出连到 SpawnTransform。同一 MakeTransform 输出可 multi-connect 给多个 SpawnActor。

### 5. Live Coding 后既有 BP 节点 self pin 卡死 BPGC archetype stale

复发 DevLog 2026-04-29 hearing #6。本次 Live Coding `trigger_build` 后 N 键链既有 `TriggerMinimaxSpeechWithNoise` (K2Node_CallFunction_20) 和 `GetMinimaxApiKeyFromProjectEnv` (K2Node_CallFunction_19) 编译报：

```
"Target" pin value (/Engine/Transient.BPGC_ARCH_FOR_CDO_MinimaxACELibrary_1) invalid: ... isn't a MinimaxACELibrary
```

`set_pin_default self=""` 不行（必须非空）。`monolith_reindex` 不解决。**修法**：`remove_node` + `add_node` 重建该节点，重新连所有 pin。本里程碑重建 2 次（CallFunction_20 → CallFunction_70；CallFunction_19 → CallFunction_71）后编译通过。

避免再踩：未来加 C++ 后跑 Live Coding 前先 `monolith_reindex`，编译失败时优先怀疑 stale 静态库 self pin。

### 6. K2Node_AddDelegate 不被 MCP 支持

Plan agent 验证 `MonolithBlueprintNodeActions.cpp:945-975` generic fallback 没 SetFromProperty 路径——multicast delegate 绑定走不通。所以 OnTargetPerceptionUpdated 必须在 C++ 内部 `AddDynamic`，BP 端只能调 BP-callable / BP-assignable 包装 API。

### 7. CustomEvent 自调用通过 CallFunction 即可

`CallFunction function_name=Scene_StartDialogue` 不带 target_class，MCP 自动解析为 self（Level BP）的 CustomEvent。返回的节点 title 显示 "Scene Start Dialogue\n目标是L Prison"。`Speak` 自定义函数同样这么调。

## PIE 验证（待用户完成）

### 用户手动步骤（MCP 限制无法自动）

1. 打开 L_prison Level Blueprint
2. EventGraph 找到 `Scene_WaitForArrival` 的 `Branch_Aligned`（K2Node_IfThenElse_8）和 `Scene_WaitForLeaveComplete` 的 `Branch_Aligned`（K2Node_IfThenElse_5）
3. 各自从已有 `Get NPC2Cached` 输出 pin 拖出 → 选 "Get bAligningLook"，连到对应 Branch 的 Condition pin
4. Compile + Save Level BP

### PIE 测试步骤

1. 彻底关 PIE 等几秒（DevLog hearing #8 timer 累积）
2. 启动 PIE，等 NavMesh build
3. **PIE 一启动**：所有 8 NPC AIC 互相 ENTER 同伴，屏显默认 false 不刷屏；OutputLog `LogSightMemory` filter 看 ENTER 事件
4. 不按 O 键时屏显安静
5. **按 O 键启动剧情**（首次按 O 时 NPC1 的 bDebugPrintScreen 才被设 true——TODO：本次未在 RunDialogueScene_Open 里加 enable bDebugPrintScreen 块，因为对 SightMemoryComponent 实例属性的 BP 设置同样受 MCP 限制；用户手动加或在 BP 编辑器里直接把 NPC1 实例的 component bDebugPrintScreen 改 true）：
   - [ ] NPC2 走向 NPC1 → NPC2 进入 NPC1 视野的瞬间 OutputLog 显示 `[SightMemory:AIC_NPC_SmartObject_C_X] ENTER BP_NPC_MH_Character_2 at <vec>`
   - [ ] NPC2 站到 NPC1 面前后开始 6 句对话，嘴型 + A2F 表情正常
   - [ ] 对话结束 + 等 3s 后 NPC2 走回原位
   - [ ] NPC2 走出 NPC1 视野时 OutputLog 显示 `[SightMemory:...] EXIT BP_NPC_MH_Character_2, last seen at <vec>`
6. 数据契约对账：在 BP 测试节点调 `IsCurrentlyVisible(NPC2) == false` + `GetLastSeenLocation(NPC2)` 返回值 ≈ EXIT 事件中的 `<vec>`

## 回归 Checklist

- [ ] M 键默认（bAllNPCs=false）：仅 NPC1 走到 BP_NavTarget 朝 BP_NavLookTarget
- [ ] M 键设 bAllNPCs=true：8 NPC 全员走到 BP_NavTarget
- [ ] **剧情期间按 M 被 NOT bSceneRunning 互斥挡住**（剧情不被打断）
- [ ] I 键：感知快照正常输出 5 NPC sight + hearing
- [ ] N 键：NPC2 TTS + NPC1 hearing 命中 ≤ 3s（注：N 键的 K2Node_CallFunction_19/20 在本里程碑被重建，需重新 PIE 验证）
- [ ] BP_NavTarget / BP_NavLookTarget 位置未被剧情修改
- [ ] AIC_NPC_SmartObject 既有 perception sense_count=2 (Sight+Hearing) 不变

## 已知限制 / 后续

- `bDebugPrintScreen` 启用步骤未自动化：MCP 不能给 SightMemoryComponent 实例属性写 BP 节点。用户在 BP 编辑器里 NPC1 实例 component details 把 `bDebugPrintScreen` 勾上即可，或用户在 RunDialogueScene_Open 懒初始化块里手动加 GetActorOfClass→Cast→GetController→Cast<AIController>→GetComponentByClass(SightMemoryComponent)→Set bDebugPrintScreen=true 链路。
- `bAligningLook` 接线两处需用户手动（同样 MCP 限制）。
- `LastSeenLocations` Mind sweep stale entries 暂未实现。8 NPC 场景 GC 不会回收，无问题。Mind 模块周期 sweep 即可。
- 剧情 6 句对话 Delay 时长固定 6s，Minimax HTTP 抖动极端情况下可能粘连——观察 PIE 实测，必要时调 7-8s。
- O 键剧情是测试 hook，Mind 模块接管时整组 5 个 CustomEvent + Speak Function + 17 SceneStory 变量 + O 键链 + M 键互斥全部删除；C++ component 实例零改动复用。

## Mind 模块未来接管路径

1. 删除 BP 测试 hook（5 个剧情 CustomEvent / O 键链 / M 键互斥 / Speak Function / 17 SceneStory 变量）
2. C++ Mind 模块直接调 `SightMemoryComponent::GetCurrentlyVisibleActors()` / `GetLastSeenLocation()`
3. 或绑定 `OnSightEnter` / `OnSightExit` BP-assignable delegate（Mind 用 C++ AddDynamic 也行）
4. AIC_NPC_SmartObject 上的 component 实例零改动复用
5. 数据契约（`TMap<AActor*, FVector>`）零改动复用
