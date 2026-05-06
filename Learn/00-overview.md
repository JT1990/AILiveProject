# 阶段 0 — 鸟瞰：模块边界、依赖、UInterface 标记

读完这一阶段你应当能回答：

- 这个项目有几个 C++ 模块？
- 它依赖哪些引擎模块和插件？哪些是 Required，哪些是 Optional？
- 为什么 `SQLiteCore` 和 `OpenSSL` 都是 PublicDependency？
- `IAILiveAgent` 是干嘛的？为什么是空接口？

---

## 1. 只有一个 C++ 模块

整个项目只声明了一个运行时模块 `AILiveProject`，定义在两处：

### 1.1 `AILiveProject.uproject`（项目配置）

```jsonc
{
    "EngineAssociation": "5.7",
    "Modules": [
        {
            "Name": "AILiveProject",
            "Type": "Runtime",          // 运行时模块（区别于 Editor / DeveloperTool）
            "LoadingPhase": "Default",  // 引擎正常初始化阶段加载
            "AdditionalDependencies": [
                "Engine", "HTTP", "Json", "JsonUtilities",
                "ACERuntime", "ACECore"
            ]
        }
    ],
    ...
}
```

`AdditionalDependencies` 只是**给 IDE 项目生成器用**的提示——影响哪些模块出现在 IntelliSense / 头文件搜索路径里。**真正的链接依赖**是 `Build.cs` 决定的。两处保持一致是惯例，但只有 `Build.cs` 是权威。

### 1.2 `Source/AILiveProject/AILiveProject.Build.cs`（链接依赖）

```csharp
public class AILiveProject : ModuleRules
{
    public AILiveProject(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(new string[]
        {
            "Core", "CoreUObject", "Engine", "InputCore",
            "HTTP", "Json", "JsonUtilities",
            "ACERuntime", "ACECore",
            "AIModule", "NavigationSystem", "SmartObjectsModule",
            "GameplayTags", "MediaAssets", "MediaPlate",
            "SQLiteCore", "OpenSSL",
        });

        PrivateDependencyModuleNames.AddRange(new string[] {});
    }
}
```

**Public vs Private** 的区别值得记住：

- `Public`：本模块的 `.h` 里 `#include` 了它的类型，下游模块也能间接看到
- `Private`：只有 `.cpp` 里用，不污染 public API

这个项目把所有依赖都塞 Public 了——这通常是早期项目的偷懒做法。**改动建议**：以后若加新依赖，能放 Private 就放 Private，能让链接器和编译时间都更轻。

---

## 2. 依赖逐项含义（按职责分组）

| 分组              | 模块                                            | 干什么用                                                                                                |
| ----------------- | ----------------------------------------------- | ------------------------------------------------------------------------------------------------------- |
| **引擎基础**      | `Core` / `CoreUObject` / `Engine` / `InputCore` | 任意 UE 模块的最小集                                                                                    |
| **网络 / 序列化** | `HTTP`                                          | LLM HTTP 客户端、MiniMax TTS 调用                                                                       |
|                   | `Json` / `JsonUtilities`                        | LLM 请求体、事件 payload、prompt 装配                                                                   |
| **NVIDIA ACE**    | `ACERuntime` / `ACECore`                        | Audio2Face-3D 面部动画运行时（驱动 MetaHuman 嘴部）                                                     |
| **AI / 导航**     | `AIModule`                                      | AIController、AISense（听觉感知用于 NPC 听到 speech.public）                                            |
|                   | `NavigationSystem`                              | NPC 寻路（GASP Mover 2.0 配合）                                                                         |
|                   | `SmartObjectsModule`                            | GameplayInteractions 用，目前未深度使用                                                                 |
|                   | `GameplayTags`                                  | NPC 阵营 / 角色 tag                                                                                     |
| **媒体**          | `MediaAssets` / `MediaPlate`                    | Act01 中场景里的 TV 视频播放                                                                            |
| **持久化**        | `SQLiteCore` ⭐                                 | **事件溯源后端**：events / commitments / vote_history / alliance_state / agent_view_state 都存在 SQLite |
| **加密**          | `OpenSSL` ⭐                                    | **SHA-256 哈希链**：每个事件的 `event_hash = SHA256(prev_hash ‖ canonical_json)`                        |

⭐ 标的是后面会反复出现的两个"特殊"依赖。**这就是阶段 0 自检题的答案**：

> **为什么 SQLiteCore 和 OpenSSL 都是 Required？**
>
> 因为这是个**事件溯源 + 哈希链审计**的项目。所有 NPC 决策、发言、投票都按时序写进 SQLite；每条事件还要哈希链接到上一条以防篡改。SQLiteCore 提供存储、OpenSSL 提供 SHA-256。少了任一个，核心系统编译都过不去。

注意：UE 5.7 自带 `SQLiteCore` 和 `OpenSSL` 模块，**不需要任何外部库**，直接 PublicDependency 就行。

---

## 3. `.uproject` 插件清单：项目继承自 GASP 样板

`.uproject` 的 `Plugins` 段启用了 **20+ 个插件**。我们按"它们一起出现意味着什么"来理解，而不是逐项背：

### 3.1 GASP（Game Animation Sample Project）核心套件

```
AnimationWarping        ← Foot IK / Aim warping
MotionWarping           ← 动画驱动的位移修正
PoseSearch              ← Motion Matching 数据库
AnimationLocomotionLibrary
Chooser                 ← 数据驱动的动画分支
Locomotor               ← UE 5.4+ 的高层移动接口
Mover                   ← Mover 2.0（替代 CMC）
NetworkPrediction       ← Mover 的网络预测后端
HairStrands / RigLogic  ← 头发 / 面部绑定
LiveLink / LiveLinkControlRig
GameplayInteractions    ← SmartObject 交互
SmartObjects
DrawDebugLibrary
CurveExpression
```

这 14 个加在一起就是 **Epic 官方 GASP 5.7 样板的全套依赖**。本项目实际就是从 GASP 样板 fork 出来的，证据：`.uproject` 末尾还有 `EpicSampleNameHash`。

**对你的意义**：

- 凡是涉及 NPC 移动 / 动画的 BP（`SandboxCharacter_Mover`、`ABP_GenericRetarget` 等），改之前先想"这是 GASP 提供的，还是项目自己加的"——前者**绝对不要重写**
- `ABP_GenericRetarget` + `RTG_UEFN_to_Metahuman_nrw` 把 UEFN mannequin 骨架的动画运行时重定向到 MetaHuman 身体（这是 CLAUDE.md 里强调过的）

### 3.2 MetaHuman 套件

```
MetaHuman / MetaHumanCharacter / MetaHumanCharacterUAF
MetaHumanRuntime / MetaHumanLiveLink
MetaHumanCoreTech
MetaHumanCalibrationDiagnostics / MetaHumanCalibrationProcessing
RigLogic
AppleARKitFaceSupport
```

这套提供 NPC 的"脸"（顶点动画 + RigLogic 表情骨骼）。`ACERuntime/ACECore` 在 Build.cs 里负责把 LLM 文本→TTS 音频→MetaHuman 嘴部曲线，但**ACE 插件本身不在 `.uproject` 的 Plugins 段里**。

⚠️ 这是一个**容易踩的小坑**：

> ACE Reference 插件（`A2FLocal` / `A2FRemote` / `AnimStream` / `AIMWrapper` / `ACERuntime` / `ACECore`）是**装在引擎里**的（Installed-Engine 模式 + NVIDIA Reference Sample 安装包），不是项目本地插件。
>
> 这就是为什么 `Build.cs` 能 link 到 `ACERuntime`，但 `.uproject` 里搜不到这个 plugin name。
>
> **影响**：换台机器跑这项目，得先把 NVIDIA ACE Reference 装到 `Engine/Plugins/` 下，否则编译 `MinimaxACELibrary.cpp` 会找不到 `IACERuntimeModule`。

### 3.3 持久化 / 工具

```
SQLiteCore              ← 上面已说
ModelingToolsEditorMode ← 仅 Editor，建模辅助
GameplayBehaviorSmartObjects
```

`Monolith` 三件套（`MonolithAI`、`MonolithCore`、`MonolithEditor`）**没**出现在 `.uproject`——它们是**全局 .mcp.json 注册的 Editor-only MCP 工具**，用来给 Claude Code 操作蓝图。**运行时不依赖**，所以打包时不会被链进去。

---

## 4. `AILiveAgent.h` —— 一个"空"接口的意义

```cpp
UINTERFACE(BlueprintType, Blueprintable)
class AILIVEPROJECT_API UAILiveAgent : public UInterface
{
    GENERATED_BODY()
};

class AILIVEPROJECT_API IAILiveAgent
{
    GENERATED_BODY()
};
```

### 4.1 UE 的 UInterface 双类范式

UE 反射系统要求接口写成**两个类**：

- `UAILiveAgent`（继承 `UInterface`）：给反射 / BP 用的"句柄"，永远空
- `IAILiveAgent`（不继承 UObject）：实际的 C++ 接口，方法都在这里

任何 actor 要"声明自己是 AI 代理"，写：

```cpp
class ABP_NPC_MH_Character_X : public ACharacter, public IAILiveAgent
{ ... };
```

然后下游代码可以 `Cast<IAILiveAgent>(Actor)` 或 `Actor->Implements<UAILiveAgent>()` 来识别它。

### 4.2 为什么是空的

注释说得很直白：

> _"Marker interface for AI Live agents. Implementing actors become first-class citizens in perception, memory and social-graph systems. The interface is intentionally empty for now; future milestones will add team / identity hooks."_

也就是：**当前它只起标签作用，将来打算往里加 `GetTeamId()` / `GetAgentIdentity()` 等纯虚方法**。MVP 阶段把"是不是 AI 代理"做成布尔判定足够了。

但这里**目前并不被任何代码 Cast 使用**——你可以在阶段 1-6 里留意一下，看到底有没有出现 `IAILiveAgent` 的 `Cast` 或 `Implements`。**这是个有用的"代码考古"：判断它是真用着，还是埋了个坑等填**（按我的扫描结果，它**还没被引用**——属于"为以后留的接口位"）。

---

## 5. 心智模型

读完阶段 0 你脑里应当有这张图：

```
AILiveProject (单 .Target / 单 Module)
│
├─ 跑在 UE 5.7 Installed-Engine 上
├─ 从 GASP 5.7 样板 fork
├─ 借用 NVIDIA ACE Reference（装在引擎层，不在 .uproject）
├─ MetaHuman 套件给 NPC 提供"脸 + 头发 + 表情骨骼"
├─ SQLiteCore + OpenSSL 撑起事件溯源 + 哈希链
└─ HTTP / Json 撑起 LLM + TTS 网络层

Editor-only 工具（不在 .uproject 链路里）：
└─ Monolith MCP（.mcp.json 注册，Claude Code 操作蓝图用）
```

---

## 6. 自检答案

1. **几个 C++ 模块？** 一个：`AILiveProject`，Runtime 类型，Default 加载阶段。
2. **SQLiteCore + OpenSSL 为何 Required？** 事件溯源存储 + 哈希链审计，去掉任一个核心都编译不过。
3. **GASP 一套插件叠加意味着什么？** 这是 GASP 5.7 样板 fork。NPC 的移动/动画链路是 Epic 提供的，不要重写。
4. **`IAILiveAgent` 现在做什么？** 空标记接口，**目前未被代码引用**，是给后续 milestone 预留的扩展位。

---

## 下一步

进入 **阶段 1 — 词汇表**（必读，30 min）。涉及 4 个 header：

- `Public/Memory/AILiveEventTypes.h`
- `Public/Memory/AILiveBidTypes.h`
- `Public/Memory/AILiveAgentTypes.h`
- `Public/LLM/AILiveAgentRoster.h`

读完回来，我把阶段 1 写进 `Learn/01-vocabulary.md`。
