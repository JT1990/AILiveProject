## 项目概览

The AI Live 是一个多智能体社会博弈系统。将多个来自不同模型厂商的 AI agent 投放到同一封闭环境中，赋予明确规则、有限信息、长期记忆、关系约束与生存风险，让它们围绕合作、结盟、欺骗、背叛与自我延续展开持续博弈。

**AILiveProject** —— 基于 UE 5.7 的原型工程，整合 GASP (Game Animation Sample Project) 5.7 + MetaHuman + NVIDIA Audio2Face-3D + MiniMax `speech-2.8-turbo` TTS。主模块：`Source/AILiveProject/`，当前只保留 HTTP + ACE C++ API 的薄 glue 层。

## 关键规则 (Critical rules)

- DevLog 里的大多数步骤在本仓库里**已经就位**，机械地照搬会造成重复组件、断裂图。
- **不要把 `bTickPhysicsAsync` 翻成 True**（在 `DefaultEngine.ini`），会破坏 Animation Warping。
- **不要修改 `Source/*.Target.cs` 里的 `DefaultBuildSettings = V6`**，降级会破坏 Installed-Engine + Live Coding 的兼容性。

## 命令 (Commands)

UBT 构建 Editor target：

```
<Engine>/Build/BatchFiles/Build.bat AILiveProjectEditor Win64 Development -Project="D:/Project/Unreal/AILiveProject/AILiveProject.uproject"
```

改 `.Build.cs` / `.Target.cs` / `.uproject` plugin 列表才需要全量重建（命令见上），其余 `.cpp` / `.h` 改动走 Live Coding。没有自动测试套件；关键烟测是 PIE + T 键触发 TTS/A2F。

## Unreal 资产操作（Monolith MCP）

**YOU MUST** 在操作 UE 前做两件准备工作：1. 扫描查阅是否有可用的 skill，避免猜测走弯路；2. 用 Monolith MCP 的 `tools` 读当前真实状态，再对照计划做差量。

Blueprint / AnimBP / 资产的读写**一律用 Monolith MCP**（在 `.mcp.json` 配置，proxy 在 `Plugins/Monolith/Binaries/monolith_proxy.exe`）。不要让用户手点编辑器，除非 MCP 确实做不到，那时再明确说明走 fallback。

工作流：`mcp__monolith__monolith_status` 确认在线 → 预检目标 BP（见关键规则）→ `build_blueprint_from_spec` + `connect_pins` + `compile_blueprint` 一次批量改图（比多轮 `add_node` 高效）→ `get_execution_flow` 验证 BeginPlay/input 链路。

**v0.14.7 新便利动作**：`ai::add_perception_to_actor` — 为 Actor 直接附加 AI Perception，省去手动布线。

### MCP 最致命的 3 个坑

- 参数名是 `value`，不是 `property_value`；资产参数是 `asset_path`，不是 `blueprint_path`。
- `resolve_node("CallFunction", "IsValid")` 默认解析到 `SubobjectDataBlueprintFunctionLibrary.IsValid`。要 `Object` 版必须显式传 `target_class: "KismetSystemLibrary"`。
- **子类 BP 上的继承组件不能用 `set_component_property` / `set_cdo_property` 点路径改默认值**。绕法：组件 BP 上加 public setter，子类 BeginPlay 里调。

其余 MCP 踩坑（`set_actor_properties` 6 字段限制、CDO 数组写法、`TSubclassOf<T>` pin 默认值、继承组件优先用类型化 BP 变量等）见 auto-memory `feedback_monolith_patterns.md` / `feedback_mcp_limitations.md`。

## 架构（读代码读不出的部分）

### C++ glue 层

模块是薄 glue 层：ACE/TTS C++ API 必须保留，Sound Wave / WAV 资产无法承载运行时生成的音频。GASP / Mover / Visual-override / Animation 等既有 BP 链路保持原样，不重写。

- `UMinimaxACELibrary::TriggerMinimaxSpeech(WorldCtx, Character, Text, ApiKey, VoiceId, Endpoint, A2FProviderName)` —— 当前唯一 BP TTS 入口。在 `EAsyncExecution::ThreadPool` 上跑 `MinimaxSpeech::RequestBlocking`，用 `TWeakObjectPtr` 守 `AActor*`，回到游戏线程后找/挂 `UACEAudioCurveSourceComponent`，调 `FACERuntimeModule::AnimateFromAudioSamples` 喂 PCM，通过组件播放音频。典型延迟 1–4 s。
- `UMinimaxACELibrary::PrewarmA2F` —— BeginPlay 里调一次，避免首次调用时的 TRT 编译延迟。
- `UMinimaxACELibrary::GetMinimaxApiKeyFromProjectEnv` —— 从 `<ProjectDir>/.env` 解析 `minimax=<key>`。仅测试用；生产应走 `UDeveloperSettings` 或 secret store。

### A2F 角色契约

每个 A2F 驱动的角色**两样都必须有**：

1. 可见 skeletal mesh actor 上有 `UACEAudioCurveSourceComponent`（`TriggerMinimaxSpeech` 会自动挂）。
2. **Face AnimBP 里有 `ApplyACEAnimation` 节点**，从该组件读 curve。

缺节点则音频播但脸不动，这是静默失败模式。口型不同步时优先查这里。

### Visual-override 系统

- 活跃角色是 `SandboxCharacter_Mover`（GASP Mover 2.0）。CMC 版兄弟仅作参考保留，不要往里加新逻辑。
- **`AC_VisualOverrideManager`** 通过给 tag 为 `VisualOverride` 的 `ChildActorComponent` 调 `SetChildActorClass` 来切身体。CVar `DDCvar.VisualOverride`（默认 `-1`）选 `GM_Sandbox.VisualOverrides[]` 的 index。
- **NPC 固定 override 路径**：`AC_VisualOverrideManager.FixedVisualOverride` + `SetFixedAndApply(TSubclassOf)`。子类 BP（`Content/Blueprints/NPCs/BP_NPC_MH_Character_1..8`）持有 `FixedVisualOverrideClass` 变量并在 BeginPlay 调 `SetFixedAndApply`。场景 8 个 NPC 都走这条路。
- **BeginPlay 里两层 `IsValid` 是故意的**：同时支持 (a) VisualOverride 模式下可见身体 spawn 到 `ChildActorComponent`、(b) 直接把角色摆进关卡以 Ref Pose 呈现。删掉任一分支会破坏一种模式。
- **`AC_PreCMCTick`** 用 `AddTickPrerequisiteActor` 保证身体动画先于移动处理 tick，Motion Matching 姿态稳定性依赖这点。
- 重定向：`ABP_GenericRetarget` + `RTG_UEFN_to_Metahuman_nrw` 把 GASP 动画（UEFN mannequin 骨架）运行时重定向到 MetaHuman 身体。

### 关卡

`Content/MyAssets/Levels/L_prison.umap`：通过 `DefaultEngine.ini` 中 `+GameModeMapPrefixes` 映射到 `GM_Sandbox`，玩家 Pawn = `SandboxCharacter_Mover_C`，PlayerController = `PC_Sandbox_C`。场景中有 8 个可见 NPC 实例 `BP_NPC_MH_Character_1_C` … `_8_C`，全部由 `AIController`（`AutoPossessAI=PlacedInWorld`）控制，挂有 `NavMoverComponent`（GASP Mover 2.0 AI 寻路接口）。

## 参考 (References)

- `@DevLog/2026-04-24_minimax_speech_a2f_metahuman.md` —— 首次接入 TTS + A2F，记录 hex 解码坑、provider 名坑、PIE input focus 问题。
- `@DevLog/2026-04-25_gasp_mover_metahuman.md` —— GASP Mover 2.0 应用到 MetaHuman 身体。
- `@DevLog/2026-04-25_npc_visual_override_fixed.md` —— NPC 固定 override 模式 + DefaultPawn 飞行摄像头。
- `@DevLog/2026-04-26_npc_baseline_inventory.md` —— NPC / A2F / VisualOverride 基线盘点，若与实际资产冲突，以资产状态为准并更新 DevLog。
- `@DevLog/2026-04-28_npc_movement_basic.md` —— NPC 移动基础能力（已 PIE 验证）：GASP Mover 2.0 + AIController.MoveToLocation，Level BP M 键触发，MCP class pin 绕过方案。

每个里程碑新增一份 `DevLog/YYYY-MM-DD_<topic>.md`。

## 约定

- 用户用中文交流，回复也用中文。
- GASP / Mover / Visual-override / NPC 父类等既有 BP 链路沿用，不重写。蓝图限于配置资产（DataAsset / Curve）、UMG / AnimBP / 关卡蓝图、必须继承既有 BP 父类的场景。
- 当 DevLog / 手册与实际资产状态冲突时，**以资产状态为准**并更新 DevLog。不要为了贴合过时文档去改动资产。
- 文档要扁平化，只写当前确定的结论。**不保留** v1/v2/v3 / 原版 vs 修订 / 修复历史 等迭代痕迹。审查/讨论的过程产物，结论合并进正文后即删。
