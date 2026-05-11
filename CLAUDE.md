## 项目概览

The AI Live 是一个多智能体社会博弈系统。将多个来自不同模型厂商的 AI agent 投放到同一封闭环境中，赋予明确规则、有限信息、长期记忆、关系约束与生存风险，让它们围绕合作、结盟、欺骗、背叛与自我延续展开持续博弈。

**AILiveProject** —— 基于 UE 5.7 的原型工程，整合 GASP (Game Animation Sample Project) 5.7 + MetaHuman + NVIDIA Audio2Face-3D + MiniMax `speech-2.8-turbo` TTS。主模块：`Source/AILiveProject/`，只保留 Brain WebSocket + ACE C++ API 的薄 glue 层。

## 架构边界（重要）

**脑层（LLM 推理 / Prompt / Memory / Parser / Provider 路由 / Agent 状态机）外置 Python Brain Service**。

UE 这边只负责身体：世界仿真、角色表现（含 TTS / A2F 驱动 MetaHuman 嘴型）、玩家输入；以及对 brain 返回的 action / sentence 做白名单校验后落地（见下"Brain ↔ UE 协议"段 IngressValidator）。

UE 模块**禁止**重新长出 LLM provider 调用、prompt 拼装、记忆持久化、agent decision 逻辑；这些一律走 WebSocket 调外置 brain service（`/health` 仍走 HTTP）。

## 关键规则 (Critical rules)

- DevLog 里的大多数步骤在本仓库里**已经就位**，机械地照搬会造成重复组件、断裂图。
- **不要把 `bTickPhysicsAsync` 翻成 True**（在 `DefaultEngine.ini`），会破坏 Animation Warping。
- **不要修改 `Source/*.Target.cs` 里的 `DefaultBuildSettings = V6`**，降级会破坏 Installed-Engine + Live Coding 的兼容性。
- **.cpp 里禁用匿名 `namespace { ... }` 装 helper**：Unity Build 会把多个 .cpp 拼到同一 TU，匿名命名空间合并后导致 `GetSession` / `GetRoster` / `FMoveAndLookAtLocationParams` 这类常见名字 C2011 / C2084 重定义。改用文件唯一的具名 namespace（如 `AILive<File>Impl`）+ 紧跟一行 `using namespace ...;` 保持调用点不变。

## 命令 (Commands)

UBT 构建 Editor target：

```
<Engine>/Build/BatchFiles/Build.bat AILiveProjectEditor Win64 Development -Project="D:/Project/Unreal/AILiveProject/AILiveProject.uproject"
```

改 `.Build.cs` / `.Target.cs` / `.uproject` plugin 列表才需要全量重建（命令见上），其余 `.cpp` / `.h` 改动走 Live Coding。

自动化测试（headless，无需 PIE）—— Protocol round-trip / IngressValidator / ActionDispatcher 共 19 个测试位于 `Source/AILiveProject/Tests/`，测试路径 `AILive.*`：

```
"D:\Software\UE_5.7\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" "D:\Project\Unreal\AILiveProject\AILiveProject.uproject" -ExecCmds="Automation RunTests AILive; Quit" -Unattended -NullRHI -NoSplash -NoSound -TestExit="Automation Test Queue Empty"
```

端到端烟测：PIE + Brain server 在跑（先看 `BrainService/CLAUDE.md`）；或在 BrainService 侧用 `python -m scripts.inject_demo_move_speak` 注入。T 键触发 TTS/A2F 仍可单独测 MiniMax + ACE 链路。

### UE 编辑器进程管理

需要重启编辑器时自己用 PowerShell 操作，不让用户手点；强杀前先确认改动已保存，启动后等 `monolith_status` 返回 online 再继续（加载约 30 s），无需询问。

```
# 关
powershell -NoProfile -Command "Get-Process UnrealEditor -ErrorAction SilentlyContinue | Stop-Process -Force"
# 开
powershell -NoProfile -Command "Start-Process 'D:\Software\UE_5.7\Engine\Binaries\Win64\UnrealEditor.exe' -ArgumentList '\"D:\Project\Unreal\AILiveProject\AILiveProject.uproject\"'"
```

## Unreal 资产操作（Monolith MCP）

**YOU MUST** 在操作 UE 前做两件准备工作：1. 扫描查阅是否有可用的 skill，避免猜测走弯路；2. 用 Monolith MCP 的 `tools` 读当前真实状态，再对照计划做差量。

Blueprint / AnimBP / 资产的读写**一律用 Monolith MCP**（在 `.mcp.json` 配置，proxy 在 `Plugins/Monolith/Binaries/monolith_proxy.exe`）。不要让用户手点编辑器，除非 MCP 确实做不到，那时再明确说明走 fallback。

工作流：`mcp__monolith__monolith_status` 确认在线 → 预检目标 BP（见关键规则）→ `build_blueprint_from_spec` + `connect_pins` + `compile_blueprint` 一次批量改图（比多轮 `add_node` 高效）→ `get_execution_flow` 验证 BeginPlay/input 链路。

### MCP 最致命的 3 个坑

- 参数名是 `value`，不是 `property_value`；资产参数是 `asset_path`，不是 `blueprint_path`。
- `resolve_node("CallFunction", "IsValid")` 默认解析到 `SubobjectDataBlueprintFunctionLibrary.IsValid`。要 `Object` 版必须显式传 `target_class: "KismetSystemLibrary"`。
- **子类 BP 上的继承组件不能用 `set_component_property` / `set_cdo_property` 点路径改默认值**。绕法：组件 BP 上加 public setter，子类 BeginPlay 里调。

其余 MCP 踩坑（`set_actor_properties` 6 字段限制、CDO 数组写法、`TSubclassOf<T>` pin 默认值、继承组件优先用类型化 BP 变量等）见 auto-memory `feedback_monolith_patterns.md` / `feedback_mcp_limitations.md`。

## 两个独立 git 仓库

**BrainService repo**：物理目录在当前 UE 工程根目录下的 `BrainService/`，已加入 `.gitignore`。
**AILiveProject**: UE 工程仓库。
两仓 git 历史完全独立，UE 工程不持有 Brain 代码副本；只是物理嵌套以便共享一个 Claude Code 顶层上下文。

## Brain ↔ UE 协议

协议版本与 Brain 引用 commit 的单一指针在 `Docs/protocol_pointer.md`，当前为 `0.2.0`；上下行 frame 清单也以 `protocol_pointer.md` 为准。

运行时通信：UE 作为 client 主动连接 Brain server 的 `WS /v1/ws`，全双工。UE 不开监听端口。HTTP 只剩 `/health` 与调试入口。断线后 UE 用 `session.resume + last_brain_seq` 请求 Brain 重放未确认事件。

Brain frame 落地前必须过 `FAILiveProjectIngressValidator` 白名单（actor_id ∈ roster、句长上限、audience ⊂ visibility 集、`target_zone` 已绑定等）。

## 架构（读代码读不出的部分）

### C++ glue 层

模块是薄 glue 层：ACE/TTS C++ API 必须保留，Sound Wave / WAV 资产无法承载运行时生成的音频。GASP / Mover / Visual-override / Animation 等既有 BP 链路保持原样，不重写。

**TTS / A2F**（Sound Wave / WAV 无法承载运行时生成的音频，所以必须 C++ 喂 PCM 直驱 ACE）：

- `UMinimaxACELibrary::TriggerMinimaxSpeech(WorldCtx, Character, Text, ApiKey, VoiceId, Endpoint, A2FProviderName)` —— BP TTS 入口，典型延迟 1–4 s。
- `UMinimaxACELibrary::TriggerMinimaxSpeechFromPawnWithNoiseEx` —— SpeakDispatcher 用的带完成回调版本（`FOnSpeechCompleted` 非 dynamic delegate，便于 `BindWeakLambda`）。完成 timer 在 `AnimateFromAudioSamples` 之前调度以避免延迟翻倍。
- `UMinimaxACELibrary::PrewarmA2F` —— BeginPlay 里调一次，避免首次调用的 TRT 编译延迟。
- `UMinimaxACELibrary::GetMinimaxApiKeyFromProjectEnv` —— 从 `<ProjectDir>/.env` 解析 `minimax=<key>`。仅测试用；生产应走 `UAILiveProjectSettings::MinimaxApiKey`。

**Brain glue 子系统**（`Source/AILiveProject/`，所有 LLM/记忆/决策都不在这里）：

- `UAILiveProjectSettings` —— `UDeveloperSettings`，从 `Config/DefaultGame.ini` 读 Brain URL / token / 采样 / 重试参数。
- `UAILiveProjectBrainSessionSubsystem` —— 启动链路：`/health` 校验 `protocol_version` → WS connect → `session.create` → 枚举 `IAILiveAgent` Pawn → `roster.register` → runtime。自动挂 `FWorldDelegates::OnPostWorldInitialization` + per-world `OnWorldBeginPlay`，PIE 不需要 GM 蓝图改动。`StartHandshake()` 也开放给 BP 显式触发。
- `FAILiveProjectBrainWsClient` —— 封装 UE `WebSockets` 模块，负责连接、JSON envelope、Brain frame 解析与 GameThread 回调。
- `FAILiveProjectBrainHttpClient` —— `/health` 唯一的 HTTP 调用点，运行时其它路径都走 WS。
- `UAILiveProjectRosterSubsystem` —— `actor_id` ↔ `APawn*` 双向映射 + BP class `FName` 反查表（PerceptionLogger 返回 FName）。
- `UAILiveProjectWorldStateCollector` —— 周期发送 `world_state.push`（`WorldStateSampleIntervalMs`，默认 500 ms），来源 PerceptionLogger + current_action 启发式。
- `UAILiveProjectActionDispatcher` —— 接 `event.action_intent`，路由到 move / sit / wait；overlap-cancel 走 `CancelSilently + IntentSeq` 双保护。`event.action_cancelled` 停止 active action 但不上报 result。
- `UAILiveProjectSpeakDispatcher` —— 接 `event.speech_public`，过 IngressValidator 后调 `TriggerMinimaxSpeechFromPawnWithNoiseEx`。
- `FAILiveProjectIngressValidator` —— Brain frame 落地前的白名单校验，五条 reject 路径（含字面量 `public` 在 `addressed_to`、`target_zone` 未绑定等）。
- `UAILiveProjectActionResultReporter` / `UAILiveProjectSpeechResultReporter` —— 通过 WS 发 `action.result` / `speech.result`，outcome 严格 `succeeded|failed`。

**BP 签名硬约束**：`SandboxCharacter_Mover` 的 `MoveAndLookAt` BP 函数走 `FMoveAndLookAtLocationParams`，调用方必须提供完整结构体（含 `LookTarget` + `bSucceeded`），否则 BP VM 把栈垃圾当 `AActor*` 解引用立刻 `EXCEPTION_ACCESS_VIOLATION`。ActionDispatcher 在 `LookTarget == nullptr` 时跳过 BP 路径回退到 `AAIController::MoveTo*`。参见 DevLog `2026-04-29_npc_scatter_to_target.md §pitfall 8`。

### 关卡

`Content/MyAssets/Levels/L_prison.umap`：通过 `DefaultEngine.ini` 中 `+GameModeMapPrefixes` 映射到 `GM_Sandbox`，玩家 Pawn = `SandboxCharacter_Mover_C`，PlayerController = `PC_Sandbox_C`。场景中有 10 个可见 NPC 实例 `BP_NPC_MH_Character_1_C` … `_10_C`，全部由 `AIController`（`AutoPossessAI=PlacedInWorld`）控制，挂有 `NavMoverComponent`（GASP Mover 2.0 AI 寻路接口）。

## 约定

- **功能实现默认 C++**；蓝图限于既有 BP 链路（GASP / Mover / Visual-override / NPC 父类）、配置资产（DataAsset / Curve）、UMG、AnimBP、关卡蓝图。
- 用户用中文交流，回复也用中文。
- NPC 外观符号约束：在 UE 这边为 NPC 分配名字 / 昵称 / 性别 / 声线 / 类人虚拟形象时，**严禁**带人类职业、教育、地域、年龄、姓名格式、家乡 等背景叙事；背景人格属于 brain service 范畴。
- 当 DevLog / 手册与实际资产状态冲突时，**以资产状态为准**并更新 DevLog。不要为了贴合过时文档去改动资产。
- 文档要扁平化，只写当前确定的结论。**不保留** v1/v2/v3 / 原版 vs 修订 / 修复历史 等迭代痕迹。审查/讨论的过程产物，结论合并进正文后即删。
- 完成任务后 git commit 无需询问。
