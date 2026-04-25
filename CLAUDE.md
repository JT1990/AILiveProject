# CLAUDE.md

本文件为 Claude Code (claude.ai/code) 在本仓库工作时提供指引。

## 项目概览

**AILiveProject** —— 基于 UE 5.7 的原型工程，整合 GASP (Game Animation Sample Project) 5.7 + MetaHuman + NVIDIA Audio2Face-3D + MiniMax `speech-2.8-turbo` TTS。场景中存在 8 个 AI 驱动的 MetaHuman NPC；玩家以不可见的 DefaultPawn 飞行摄像头运行。默认关卡：`Content/MyAssets/Level_AILive.umap`。主模块：`Source/AILiveProject/`（HTTP + ACE C++ API 的薄 glue 层）。

## 快速开始

1. **`<ProjectDir>/.env`** 必须包含 `minimax=sk-api-...`（已 gitignore；`GetMinimaxApiKeyFromProjectEnv()` 运行时解析此文件）。
2. 用 UE 5.7 打开 `AILiveProject.uproject`。
3. 烟测：PIE → 在任一 `BP_NPC_MH_Character_*` 上按 **T** 键。预期听到中文语音并看到 `LocalA2F-James` 驱动的同步口型。观察 Output Log 中的 MiniMax trace ID 和 `LogACE` 日志。首次调用会有 TRT 编译延迟，除非 BeginPlay 里已调用 `PrewarmA2F`。

## 关键规则 (Critical rules)

- **IMPORTANT —— ACE provider 名必须严格匹配**。本地只注册了 `LocalA2F-James`、`LocalA2F-Mark`、`LocalA2F-Claire` 三个 provider。传 `"Default"` 会**静默退回 `RemoteA2F`**（走网络，看起来成功实际上本地什么都没做）。
- **IMPORTANT —— MiniMax endpoint 由 key 前缀决定**。`sk-api-` 前缀 key → `https://api.minimaxi.com/v1/t2a_v2`。`api.minimax.io` 是 JWT 格式 key 的国际版 endpoint，不要混用。响应 audio 是 hex 编码，由 `MinimaxSpeechClient` 解码成 PCM16 16 kHz mono（ACE 唯一接受的格式）。
- **YOU MUST 在改 BP 前做预检**。用 Monolith MCP 的 `get_components` / `get_graph_summary` / `get_variables` / `get_cdo_properties` / `get_execution_flow` 读当前真实状态，再对照计划做差量。DevLog 和 Lucy 手册里的大多数步骤在本仓库里**已经就位**——机械地照搬会造成重复组件、断裂图。
- **改完任何 `Config/Default*.ini` 必须重启编辑器**。UE 运行时不会 hot-reload INI。
- **不要把 `bTickPhysicsAsync` 翻成 True**（在 `DefaultEngine.ini`），会破坏 Animation Warping。
- **不要修改 `Source/*.Target.cs` 里的 `DefaultBuildSettings = V6`**，降级会破坏 Installed-Engine + Live Coding 的兼容性。

## 命令 (Commands)

UBT 构建 Editor target：

```
<Engine>/Build/BatchFiles/Build.bat AILiveProjectEditor Win64 Development -Project="D:/Project/Unreal/AILiveProject/AILiveProject.uproject"
```

对 `AILiveProject` 模块的迭代由 Live Coding / Hot Reload 处理。只有改动 `.Build.cs`、`.Target.cs` 或 `uproject` 的 plugin 列表时才需要全量重建。没有自动测试套件；烟测是上面 PIE + T 键的流程。

## Unreal 资产操作（Monolith MCP）

Blueprint / AnimBP / 资产的读写**一律用 Monolith MCP**（在 `.mcp.json` 配置，proxy 在 `Plugins/Monolith/Binaries/monolith_proxy.exe`）。不要让用户手点编辑器，除非 MCP 确实做不到——那时再明确说明走 fallback。

工作流：

1. `mcp__monolith__monolith_status` —— 确认编辑器在线。
2. 预检目标 BP（见关键规则）。
3. 用 `build_blueprint_from_spec` + `connect_pins` + `compile_blueprint` 一次批量改图，比多轮 `add_node` 往返高效。
4. 验证 BeginPlay / input 链路时 `get_execution_flow` 最直接。

### MCP 踩坑

- 参数名是 `value`，不是 `property_value`。资产参数是 `asset_path`，不是 `blueprint_path`。
- `resolve_node("CallFunction", "IsValid")` 默认解析到错误的重载（`SubobjectDataBlueprintFunctionLibrary.IsValid`）。想拿 `Object` 版必须显式传 `target_class: "KismetSystemLibrary"`。
- UE 5.7 SceneComponent 的 BP-callable 父级访问器叫 `GetAttachParent`——不是 `K2_GetAttachParent`，也不是 `GetParentComponent`。
- `set_actor_properties` 只支持 6 个字段（mobility / simulate_physics / collision_preset / cast_shadow / tags / mass_kg）。要改 GameMode / WorldSettings 就去改 INI 并重启编辑器。
- CDO 数组属性写入（如 `GM_Sandbox.VisualOverrides`）用 `set_cdo_property`，不是 `set_component_property`。
- **子类 BP 上的继承组件不能用 `set_component_property` 或 `set_cdo_property` 点路径改默认值**。绕法：在组件 BP 上加 public setter，子类 BeginPlay 里调。
- **`TSubclassOf<T>` pin 默认值不能用 `set_pin_default`**（只写 string `default_value`）。绕法：`add_variable` 建一个 `class:XXX` 类型变量，用 `set_variable_defaults` 设 class 路径字符串，把 `VariableGet` 接到 pin。
- 继承组件会在子类上自动生成类型化 BP 变量——优先 `add_node(VariableGet, variable_name=<component_name>)`，比 `GetComponentByClass + DynamicCast` 干净且类型对齐。

## 架构（读代码读不出的部分）

### C++ glue 层

模块只是薄 glue 层。gameplay 逻辑都在 Blueprint 里；C++ 存在仅因为 ACE 插件对"运行时生成的音频"强制要求走 C++ API（Sound Wave / WAV 资产无法承载 TTS 输出）。

- `UMinimaxACELibrary::TriggerMinimaxSpeech(WorldCtx, Character, Text, ApiKey, VoiceId, Endpoint, A2FProviderName)` —— 唯一 BP 入口。在 `EAsyncExecution::ThreadPool` 上跑 `MinimaxSpeech::RequestBlocking`，用 `TWeakObjectPtr` 守 `AActor*`，回到游戏线程后：找/挂 `UACEAudioCurveSourceComponent`，调 `FACERuntimeModule::AnimateFromAudioSamples` 喂 PCM，通过组件播放音频。典型延迟 1–4 s。
- `UMinimaxACELibrary::PrewarmA2F` —— BeginPlay 里调一次，避免首次调用时的 TRT 编译延迟。
- `UMinimaxACELibrary::GetMinimaxApiKeyFromProjectEnv` —— 从 `<ProjectDir>/.env` 解析 `minimax=<key>`。仅测试用；生产应走 `UDeveloperSettings` 或 secret store。

### A2F 角色契约

每个 A2F 驱动的角色**两样都必须有**：

1. 可见 skeletal mesh actor 上有 `UACEAudioCurveSourceComponent`（`TriggerMinimaxSpeech` 会自动挂）。
2. **Face AnimBP 里有 `ApplyACEAnimation` 节点**，从该组件读 curve。

缺节点则音频播但脸不动——**静默失败模式**，口型不同步时优先查这里。

### Visual-override 系统

- 活跃角色是 `SandboxCharacter_Mover`（GASP Mover 2.0）。CMC 版兄弟 `SandboxCharacter_CMC*` 仅作参考保留。
- **`AC_VisualOverrideManager`** 通过给 tag 为 `VisualOverride` 的 `ChildActorComponent` 调 `SetChildActorClass` 来切身体。CVar `DDCvar.VisualOverride`（默认 `-1`）选 `GM_Sandbox.VisualOverrides[]` 的 index。
- **NPC 固定 override 路径**：`AC_VisualOverrideManager.FixedVisualOverride` + `SetFixedAndApply(TSubclassOf)`。子类 BP（`Content/Blueprints/NPCs/BP_NPC_MH_Character_1..8`）持有 `FixedVisualOverrideClass` 变量并在 BeginPlay 调 `SetFixedAndApply`。场景 8 个 NPC 都走这条路。
- **BeginPlay 里两层 `IsValid` 是故意的**：同时支持 (a) VisualOverride 模式下可见身体 spawn 到 `ChildActorComponent`、(b) 直接把角色摆进关卡以 Ref Pose 呈现。删掉任一分支会破坏一种模式。
- **`AC_PreCMCTick`** 用 `AddTickPrerequisiteActor` 保证身体动画先于移动处理 tick——Motion Matching 姿态稳定性依赖这点。
- 重定向：`ABP_GenericRetarget` + `RTG_UEFN_to_Metahuman_nrw` 把 GASP 动画（UEFN mannequin 骨架）运行时重定向到 MetaHuman 身体。

### 关卡

`Content/MyAssets/Level_AILive.umap`：玩家 Pawn 是引擎默认的 `DefaultPawn`（飞行、不可见——`DefaultEngine.ini` **没有** `GlobalDefaultGameMode` 覆盖，所以 engine default 生效）。spawn 了 8 个可见 NPC 实例 `BP_NPC_MH_Character_1_C` … `_8_C`。目前 8 个 NPC 共用同一个 **T** 键（per-NPC input 路由尚未实现）。

### 飞行摄像头输入

`Config/DefaultInput.ini`（EnhancedInput）：WASD + Space/Alt → `FlyCam_MoveFB/LR/UD`，Mouse → `FlyCam_LookUD/LR`。

## 参考 (References)

- `@DevLog/2026-04-24_minimax_speech_a2f_metahuman.md` —— 首次接入 TTS + A2F，记录 hex 解码坑、provider 名坑、PIE input focus 问题。
- `@DevLog/2026-04-25_gasp_mover_metahuman.md` —— 将 GASP Mover 2.0 应用到 MetaHuman 身体。
- `@DevLog/2026-04-25_npc_visual_override_fixed.md` —— NPC 固定 override 模式 + DefaultPawn 飞行摄像头。
- `Docs/Lucy_GASP_A2F_Manual.md` —— 被 DevLog 引用，但**本仓库里当前缺失**。不要假设它存在。

每个里程碑新增一份 `DevLog/YYYY-MM-DD_<topic>.md`。

## 约定

- 用户用中文交流——回复也用中文。
- 当 DevLog / 手册与实际资产状态冲突时，**以资产状态为准**并更新 DevLog。不要为了贴合过时文档去改动资产。
- 编程语言选择 C++
