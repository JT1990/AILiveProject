# NPC 听觉感知（AI Perception Hearing + TTS noise hook）

日期：2026-04-29

## 目标

NPC2 通过 TTS 播放 PRD"病毒游戏触碰机制"段台词时，自动产生一次 AI Perception Hearing 刺激事件；NPC1（与 NPC2 在 L_prison 大厅内有一定距离、无墙体遮挡）感知该声源事件，输出"声源身份/距离/方向/位置/响度"作为后续 LLM Mind 模块听觉输入数据契约的最小可行版本。

视野（Sight）已在 2026-04-28 里程碑落地。本里程碑沿用同一感知通路（`AIC_NPC_SmartObject` + `UAIPerceptionStimuliSourceComponent` + `UAILiveProjectPerceptionLogger`），新增 Hearing sense 与"TTS 入口产生 noise event"链路。

## 技术决策

**Audio target 与 Noise instigator 必须分离**。项目里这两件事属于**两个 actor**：

- **Audio target**（挂 `UACEAudioCurveSourceComponent`，Face AnimBP 的 `ApplyACEAnimation` 节点要从该组件读 curve）= 可见 MetaHuman child actor（NPC2 Pawn 的 `VisualOverride: ChildActorComponent` spawn 出来的实例）。
- **Noise instigator**（Hearing 刺激发出方，AI 感知归因）= NPC2 Pawn 逻辑 actor（`SandboxCharacter_Mover` 子类）。

把 Pawn 当 audio target 会导致 ACE 组件挂在错误 actor 上，Face AnimBP 拿不到 curve（脸不动）；把 child visible actor 当 noise instigator 会让 AI 感知归因到非 IAILiveAgent 实例。当前 `TriggerMinimaxSpeech(Character)` 单参数 API 揉了这两件事。

为最小破坏既有调用点，**新增 `TriggerMinimaxSpeechWithNoise(AudioTarget, NoiseInstigator, ...)` 函数**（不改原 `TriggerMinimaxSpeech` 签名）。原函数保留纯 TTS 行为，新函数同时上报 Hearing。

**`UAISense_Hearing::ReportNoiseEvent` 必须在 GameThread 调用，且 Loc 在 GameThread 内取**。MiniMax HTTP lambda 跑在 `EAsyncExecution::ThreadPool`，AISense API 不是线程安全的（操作 `UPerceptionSystem` / World actors）。新函数 lambda 末尾用 `AsyncTask(ENamedThreads::GameThread, ...)` 跳回游戏线程，跨线程用 `TWeakObjectPtr<AActor>` 守 Pawn 生命周期；`Pawn->GetActorLocation()` 在 GameThread lambda 内取，避免数据竞争。

**Hearing 事件触发点 = 音频实际开播那一刻**，不是 HTTP 请求发起前。这意味着按 N 键到 hearing 命中之间有 1-4 秒 TTS HTTP 延迟。Level BP 测试 hook 必须自动覆盖延迟窗口——加一个 8 秒周期 timer（0.5s 间隔，16 ticks counter 自动停），不能依赖人按键时机精确。

**hearing.range=2000 与 sight.radius=2000 一致**。覆盖 L_prison 大厅、便于后续 Mind 模块写规则。`UAISense_Hearing` 不做 line-of-sight 障碍检查（仅按距离 + Loudness 判定），墙体不阻挡——隔墙感知问题留给后续"声场遮蔽"工作。`max_age=3.0` 比 sight `5.0` 短，反映"声音事件比视觉短暂"。

## 资产改动

### 1. C++ Logger 扩展

`Source/AILiveProject/Public/AILiveProjectPerceptionLogger.h`：

```cpp
USTRUCT FHeardSoundInfo {
    FName SourceIdentity;         // 类名去 _C 后缀
    float DistanceCm;             // (StimLoc - PerceiverLoc).Size()
    FRotator DirectionFromPerceiver;
    float RelativeYawDeg;         // [-180, 180] 相对 Perceiver forward
    FVector StimulusLocation;     // 噪声**事件发生时**的位置（不是 source actor 当前位置）
    float Loudness;               // FAIStimulus.Strength
    float StimulusAge;
};

static TArray<FHeardSoundInfo> GatherHearingPerception(AAIController*, UObject*);
static int32 LogHearingPerceptionToOutput(AAIController*, FString Tag, UObject*);
static AActor* GetChildActorOf(UChildActorComponent*);  // BP 包装
```

实现关键点（与 Sight 同构，差异点）：

- `PC->GetCurrentlyPerceivedActors(UAISense_Hearing::StaticClass(), Currently)`
- `S.Type == UAISense::GetSenseID<UAISense_Hearing>()` 过滤 stimuli
- `Loudness = HearingStim.Strength`
- `StimulusLocation = HearingStim.StimulusLocation`（噪声**发生时**的位置；Sight 用的是 source actor 当前位置）
- 复用 `IsAgentActor`（`IAILiveAgent` 过滤）防玩家 Pawn 误入；自感知由 `A == PerceiverPawn` 过滤

`GetChildActorOf` 是个一行包装：`return Component ? Component->GetChildActor() : nullptr;`。原因见"MCP 实施踩坑"第 4 条。

### 2. C++ TTS 入口加 Noise 链路

`Source/AILiveProject/Public/MinimaxACELibrary.h` 新增：

```cpp
UFUNCTION(BlueprintCallable, meta=(WorldContext="WorldContextObject", AdvancedDisplay="VoiceId,Endpoint,A2FProviderName"))
static void TriggerMinimaxSpeechWithNoise(
    UObject* WorldContextObject,
    AActor* AudioTarget,        // 挂 ACE 组件的可见 actor
    AActor* NoiseInstigator,    // AI 感知归因 Pawn
    const FString& Text,
    const FString& ApiKey,
    const FString& VoiceId = ...,
    const FString& Endpoint = ...,
    FName A2FProviderName = ...);
```

`.cpp` 实现：与 `TriggerMinimaxSpeech` 同结构，关键差异：

```cpp
TWeakObjectPtr<AActor> WeakNoiseInstigator(NoiseInstigator);
Async(EAsyncExecution::ThreadPool, [..., WeakNoiseInstigator, ...]() {
    // ... HTTP + AnimateFromAudioSamples 同既有 ...
    AsyncTask(ENamedThreads::GameThread, [WeakNoiseInstigator]() {
        APawn* Pawn = Cast<APawn>(WeakNoiseInstigator.Get());
        if (!Pawn) return;
        UAISense_Hearing::ReportNoiseEvent(
            Pawn, Pawn->GetActorLocation(),
            /*Loudness=*/ 1.f, /*Instigator=*/ Pawn,
            /*MaxRange=*/ 0.f, /*Tag=*/ NAME_None);
    });
});
```

原 `TriggerMinimaxSpeech` 不变，BP 调用点零行为变化。

### 3. AIC_NPC_SmartObject 加 Hearing sense

```
ai_query::configure_hearing_sense
  asset_path=/Game/Blueprints/AI/AIC_NPC_SmartObject
  range=2000
  affiliation={ enemies: true, neutrals: true, friendlies: true }
  max_age=3.0
```

`validate_perception_setup` → `valid: true, sense_count: 2`。

### 4. SandboxCharacter_Mover StimuliSource 加 Hearing

```
ai_query::configure_stimuli_source
  asset_path=/Game/Blueprints/SandboxCharacter_Mover
  sense_types=["Sight", "Hearing"]
```

8 个 NPC 子类全部继承（与 Sight 同样）。

### 5. L_prison Level BP

新增变量（category=NPCMove）：
- `SpeechText: string`，default = PRD 病毒游戏触碰机制段
- `VoiceId: string`，default `male-qn-qingse`
- `Endpoint: string`，default `https://api.minimaxi.com/v1/t2a_v2`
- `A2FProviderName: name`，default `LocalA2F-James`
- `HearingPollTicksRemaining: int`，default 0（运行时由 N 键链 set 为 30，每个 tick 自减；30 × 0.5s = 15s 上限窗口）
- `ChildActorComponentClass: class:ActorComponent`，default `/Script/Engine.ChildActorComponent`（专门绕 set_pin_default class pin 限制，见踩坑第 5 条）

#### N 键链（在 EventGraph）

利用既有 `K2Node_ExecutionSequence_0(then_0=M键, then_1=I键)` 的 I 键 Branch `.else` 分支挂载（Sequence 不能动态加 then_2 pin，详见踩坑第 1 条）：

```
EventTick → Sequence
   ├─ then_0 → IfThenElse(M)→ ... 既有移动链
   └─ then_1 → IfThenElse(I)
                ├─ then  → I 键链路（perception logger）
                └─ else  → IfThenElse(N)
                              .then →
                                GetActorOfClass(NPC2Class) → NPC2_Pawn (cast SandboxCharacter_Mover_C)
                                → GetComponentByClass(ChildActorComponentClass) → ChildActorComp (cast ChildActorComponent)
                                → GetChildActorOf → NPC2_VisibleActor
                                → GetMinimaxApiKeyFromProjectEnv → ApiKey
                                → TriggerMinimaxSpeechWithNoise(
                                    AudioTarget=NPC2_VisibleActor,
                                    NoiseInstigator=NPC2_Pawn,
                                    Text=SpeechText, ApiKey, VoiceId, Endpoint, A2FProviderName)
                                → Set HearingPollTicksRemaining=30
                                → K2_SetTimer(self, "LogNPC1HearingTick", Time=0.5, bLooping=true)
```

#### LogNPC1HearingTick 函数

```
FunctionEntry
  → GetActorOfClass(NPC1Class) → NPC1_Actor
  → Cast<Pawn> (None-check)
    → Cast<AIController>(NPC1_Pawn.GetController) (pure)
      → LogHearingPerceptionToOutput(Tag="NPC1")
      → Set HearingPollTicksRemaining = (Get HearingPollTicksRemaining - 1)
      → Branch (HearingPollTicksRemaining <= 0)
          .true → K2_ClearTimer(self, "LogNPC1HearingTick")
```

`0.5s × 30 ticks = 15s` 覆盖窗口：吸收 1-4s TTS HTTP 延迟 + audio 长度 + 3s `max_age` 内的命中。最初按 plan 设 16=8s，PIE 实测发现 ReportNoiseEvent 时机受 TTS 时长波动较大，留 15s 余量更稳。Counter 自动停 timer，避免日志刷屏（仍存在零命中刷屏问题，已记入"已知限制"）。

#### I 键扩展

I 键链路尾接 `LogHearingPerceptionToOutput(Tag="NPC1")`，提供不依赖 timer 的手动单次快照。链路最后两步：

```
... → Cast<AIController> → LogPerceptionToOutput(Tag="NPC1") → LogHearingPerceptionToOutput(Tag="NPC1")
```

## MCP 实施踩坑

### 1. K2Node_ExecutionSequence 不能动态加 then_2 pin

`connect_pins` 到 `K2Node_ExecutionSequence_0.then_2` 报 `Source pin 'then_2' not found ... Available: execute, then_0, then_1`。Sequence pin 是动态的（BP 编辑器 UI 上"Add Pin"），但 MCP `batch_execute` 没有 `add_execution_pin` op。

绕过方案：**让 N 键 Branch 接到 I 键 Branch 的 `.else` 出口**——按键 I/N 同帧物理上不会冲突，逻辑串行清晰：

```
Sequence.then_1 → IfThenElse(I)
                    ├─ .then → I 键链路
                    └─ .else → IfThenElse(N) → N 键链路
```

未来如果要加更多键再考虑彻底重建 Sequence（disconnect → remove → 新建带 N 个 then 的 Sequence → 重连）。

### 2. Live Coding 加新 USTRUCT/UFUNCTION 后既有 BP 节点 self/Tag pin orphaned

第一次 Live Coding 编译完成（加了 `FHeardSoundInfo` USTRUCT + 两个新 UFUNCTION）后，既有 `K2Node_CallFunction "LogPerceptionToOutput"` 节点的所有 pin 全部 `is_orphaned: true`，编译报"在 L_prison 中找不到名为 LogPerceptionToOutput 的函数"。

根因：UClass 反射重建时，新 USTRUCT 改了 class layout 的 hash，旧 BP 节点 cache 的 pin GUID 与 UFUNCTION ParamList 不再匹配。

修法：删除节点（`remove_node`）+ 重新 `add_node` 同名 CallFunction。重连时复用未失效的上游节点（`K2Node_DynamicCast_3.then`/`AsAI控制器`）。

### 3. 改 UFUNCTION 既有签名后 Live Coding 反射不刷新

我尝试给原 `TriggerMinimaxSpeech` 加可选参数 `AActor* NoiseInstigator = nullptr`。Live Coding patch_applied=true，但 `resolve_node` 返回的 pin 列表仍是旧 7 个参数，**新增的 NoiseInstigator pin 不出现**。`monolith_reindex` 也没用。

新函数 `TriggerMinimaxSpeechWithNoise` 反射正常（9 个 pin 都有），新增 USTRUCT `FHeardSoundInfo` 反射也正常。结论：**Live Coding 对"已存在 UFUNCTION 加参数"的 metadata 刷新有 bug**；新增独立 UFUNCTION/USTRUCT 反射友好。

绕过方案：**新增包装函数而非改既有签名**。本里程碑保留 `TriggerMinimaxSpeech` 不变，新增 `TriggerMinimaxSpeechWithNoise`。彻底刷新需要关编辑器走 UBT 全量 build——避开。

### 4. UChildActorComponent::GetChildActor() 不暴露 BP

`search_functions("GetChildActor")` 返回 0；`add_node CallFunction GetChildActor` 报 "Function not found"。UE 5 BP 编辑器右键虽有"Get Child Actor"节点，但实际是 K2 special node 不是 UFUNCTION。`Actor.GetAllChildActors(false)` 暴露但返回数组，MCP 对 `Array_Get` wildcard pin 推断不友好（DevLog 2026-04-28_npc_movement_basic.md 已记）。

绕过方案：在 `UAILiveProjectPerceptionLogger` 加一行 BP-pure 包装函数：

```cpp
static AActor* GetChildActorOf(UChildActorComponent* C) {
    return C ? C->GetChildActor() : nullptr;
}
```

### 5. set_pin_default 不能给 class pin / object pin 设字符串 default

ComponentClass 是 `class:ActorComponent` pin。`set_pin_default value="/Script/Engine.ChildActorComponent"` 编译报 `String NewDefaultValue '...' specified on class pin 'ComponentClass'`。CLAUDE.md 项目级提示已有记录。

绕过方案：加一个 Level BP 变量 `ChildActorComponentClass: TSubclassOf<UActorComponent>` default 设到 `/Script/Engine.ChildActorComponent`（在变量 CDO 里 BP 编译时正确解析），运行时 `VariableGet` → `GetComponentByClass.ComponentClass` pin。

object pin 也有相同限制：试图 `set_pin_default node.self value="/Script/AILiveProject.Default__AILiveProjectPerceptionLogger"` 编译报 `String NewDefaultValue '...' specified on object pin 'self'`。

### 6. BP 编译失败一次后 self pin 卡死在 BPGC archetype stale 引用

新建 `K2Node_CallFunction` 调 BlueprintFunctionLibrary 静态函数，第一次 compile 失败的副作用：BP 把节点的 self pin default 写成 `/Engine/Transient.BPGC_ARCH_FOR_CDO_<ClassName>_X`（错误版本）。后续 compile 一直拒绝该 stale 引用。

`set_pin_default self="/Script/..."` 字符串绕过失败（见踩坑第 5 条）；`disconnect_pins self` 不影响 default value。

修法：**`remove_node` + `add_node` 重建**——但要等到反射 cache 完整刷新之后。同一会话第一次新建的节点容易踩这个坑（BP 反射 cache 在某个不确定的时机刷新）；之后新建的节点 self pin default 留空，BP 编译时按"static call, no self target"处理通过。

本里程碑修复路径：`monolith_reindex` + `trigger_build` + `remove + add_node` 重建受影响节点（不再手动 `set_pin_default self`）。

### 7. IAILiveAgent interface 在 8 个 NPC BP 上"丢失"了，需重新 implement_interface + save

PIE 验证 hearing 时遇到怪现象：`runtime_get_perceived_actors(NPC1)` 返回 5 个 NPC（sight 完全 OK），但 `LogPerceptionToOutput` 输出 `count=0`。`runtime_check_perception(NPC1, NPC2)` 也显示 NPC2 已被 sight 感知。Logger 把 5 个全过滤掉了——`GatherSightPerception` 内部 `IsAgentActor(A)` 检查 `A->GetClass()->ImplementsInterface(UAILiveAgent::StaticClass())`。

`get_interfaces` 检查 8 个 BP_NPC_MH_Character_*：`count: 0`——marker interface 没在 BP 子类上！前里程碑 DevLog 写的"实现 IAILiveAgent"实际可能从未持久化到子类 .uasset；或某次 UE 重启 / 父类 reparent 时丢了。

修法：8 个子类 BP 各调一次 `blueprint_query::implement_interface`（interface_class="AILiveAgent"），然后 compile + save。**关键约束：必须先 stop PIE 才能 save_asset**——PIE 在跑时 `save_asset` 直接报 `Failed to save asset`，但不会自动告诉你原因；compile 反而能 work（compile 是内存操作）。

### 8. PIE 多次启动累积 Level BP 实例，timer 持续刷屏

日志中 `[L_prison_C_0]` / `[L_prison_C_8]` / `[L_prison_C_10]` 多个 Level BP CDO ID 同时输出 hearing 日志——这是 PIE 多次 stop/restart 时旧 Level BP timer 没被回收。每次按 N 启 1 个新 timer，多个旧 timer + 新 timer 一起 fire，输出量按 instance 数倍增。

不影响功能验证（声源数据 dist/relYaw/loud 在所有 instance 间一致）。生产环境 LLM 决策层接入时 timer + 按键全部移除，不存在该问题。临时缓解：每次重新 PIE 前彻底关闭 PIE 等几秒再按 Play。

### 9. Editor 重启后内存中的 perception 配置丢失，需 save_asset

UE 重启后 `get_perception_config` 显示 sense_count=1（仅 Sight）——上一会话 `configure_hearing_sense` 改的是内存中 BP CDO，没 `save_asset` 持久化。

修法：每次 `configure_*_sense` / `configure_stimuli_source` 后立即 `compile_blueprint` + `save_asset`。`save_asset` 返回 `was_dirty: true` 才说明真有未持久化改动。

## PIE 验证结果（已通过 2026-04-29）

NPC1 位置 `(9658, 5041, 321)`，NPC2 位置 `(8826, 4508, 321)`，水平距离 ≈ 988uu（远小于 hearing range 2000uu）。

按 N 触发后日志摘录（实测）：

```
[NPC1] hearing count=1
[NPC1] heard BP_NPC_MH_Character_2 dist=987.3 relYaw=-7.3 loud=1.00 age=0.31
[NPC1] heard BP_NPC_MH_Character_2 dist=987.3 relYaw=-7.3 loud=1.00 age=0.91
[NPC1] heard BP_NPC_MH_Character_2 dist=987.3 relYaw=-7.3 loud=1.00 age=1.53
[NPC1] heard BP_NPC_MH_Character_2 dist=987.3 relYaw=-7.3 loud=1.00 age=1.84
[NPC1] heard BP_NPC_MH_Character_2 dist=987.3 relYaw=-7.3 loud=1.00 age=2.45
[NPC1] heard BP_NPC_MH_Character_2 dist=987.3 relYaw=-7.3 loud=1.00 age=2.75
[NPC1] hearing count=0   # max_age=3.0 之后过期
```

按 I 手动快照（sight + hearing 同时输出）：

```
[NPC1] perception count=5
[NPC1] -> BP_NPC_MH_Character_2 dist=987.3 relYaw=-7.3 age=0.31
[NPC1] -> BP_NPC_MH_Character_3 dist=1052.2 relYaw=4.2 age=0.31
[NPC1] -> BP_NPC_MH_Character_4 dist=767.7 relYaw=-11.9 age=0.31
[NPC1] -> BP_NPC_MH_Character_5 dist=881.3 relYaw=17.2 age=0.31
[NPC1] -> BP_NPC_MH_Character_6 dist=635.4 relYaw=9.3 age=0.00
[NPC1] hearing count=0   # max_age 已过
```

| 检查项 | 期望 | 实测 |
|---|---|---|
| 大厅可见 NPC 数 | 5（NPC2-6） | **5 ✓** |
| 套间被遮挡 NPC（NPC7/8） | 不出现 | **未出现 ✓** |
| Hearing 命中 NPC2 | dist≈988, loud=1.00, age<3s | **dist=987.3, loud=1.00, age 0.31→2.75 ✓** |
| Hearing 在 max_age 过期 | 3s 后 count→0 | **2.75s 后 count→0 ✓** |
| 自感知防护 | NPC1/NPC2 不感知自身 | **未自感知 ✓** |
| 视觉一致 | NPC2 嘴型 + A2F 表情正常 | **正常 ✓** |
| 反向校验 | 不按 N 时 hearing count=0 | **hearing count=0 ✓** |

`[NPC1] heard BP_NPC_MH_Character_2 dist=987.3 relYaw=-7.3 loud=1.00 age=0.31` 这一条满足 PRD 任务"输出声源位置"——`dist=987.3 relYaw=-7.3` 即 NPC1 局部坐标下的声源相对方位（几乎正前略偏左），加上 `StimulusLocation` 字段（FAIStimulus 内部已记录），下游 Mind 模块可直接消化。

## 已知限制 / 后续

- `UAISense_Hearing` 不做障碍物遮蔽。隔墙 NPC7/NPC8 在 2000uu range 内会"听到"。后续接 audio occlusion 或自定义 LOS check。
- "听到内容"（speech transcript）不在 hearing sense 数据里——`FAIStimulus.Tag` 只能携带 FName 枚举，不能携带任意字符串。后续 Mind 模块需要独立 chat/speech-event channel 把"谁说了什么"按 hearing 命中名单做投递目标过滤。
- Loudness 当前固定 1.0。后续可按 NPC 状态（说悄悄话 / 喊叫）动态调整。
- `max_age=3.0` 与 Sight `5.0` 不同步；如未来 Mind 需要"上一次听到 X 是什么时候"长期记忆，从 max_age 提到 perception layer 之上做。
- `TriggerMinimaxSpeech` 仍揉了 audio target + noise instigator 两个 actor 角色。本里程碑加新函数 `TriggerMinimaxSpeechWithNoise` 最小破坏接入；未来真正"声学层"接入（多 voice / TTS provider 抽象）时拆成 `Speak(InstigatorPawn, Text)` 高层 API + `PlaySpeechAudio(VisibleActor, AudioBlob)` 低层 API 两段。
- N 键 timer 15s 是测试 hook，LLM 决策层接入后由 Mind 模块按 NPC 周期调 `GatherHearingPerception` 取数据，timer 配合按键全部移除。
- `ChildActorComponentClass` Level BP 变量是 MCP class pin 限制的绕路产物。BP 编辑器 UI 直接选类即可，未来手动调整时可删此变量并把 ComponentClass pin 改回手填。
