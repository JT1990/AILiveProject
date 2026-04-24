# 2026-04-24 · MiniMax speech-2.8-turbo → Audio2Face-3D → MetaHuman

## Prompt

```
这是一个 minimax speech-2.8-hd 测试任务。 你的任务是基于 minimax docs 写一个同步语音合成的功能，
模型选择：speech-2.8-turbo
语种：1. 中文（Chinese）
音色 ID：male-qn-qingse
测试语句：今天是不是很开心呀(laughs)，当然了！
编程语言：C++
项目工程：unreal engine 5.7
minimax api key：@.env/ , minimax
测试metahuman： @Content\MetaHumans\MH_Character_1\BP_MH_Character_1.uasset
查看UE的资产的方法：使用 Monolith MCP, @.claude/skills/ 文件夹下是我准备的skill，包含：如何使用Monolith MCP的skill，以`unreal-`开头；如何使用UE5的skill，以 `unreal5-`开头。

补充信息：
我本地根据Nivdia audio2face-3d 官方教程我制作使用 wav 文件的蓝图如图 @Temp/1.png，但是经过调研发现官方文档原话:"If your application needs to feed audio generated at runtime into the ACE Unreal plugin, then providing a Sound Wave asset or WAV file ... may not be an option. For these cases, the plugin exposes a C++ API." **所以，我制作的蓝图仅供你参考，可以完全使用C++/蓝图&C++，选择最优方案。**

建议工作流：
1. 调研学习 minimax speech-2.8-turbo 的最佳实践案例和使用方法，可以增加一个单独的测试脚本，独立测试  minimax speech-2.8-turbo 的功能，（mpv 播放器已经安装）
2. 调研学习 Nvidia Audio2Face-3D unreal engine Plugins 的最佳实践案例和使用方法，并且深入了解 Nvidia Audio2Face-3D 的底层原理
3. 调研学习如何使用 Monolith MCP，如有必要可以查看UE资产（可选项）
4. 在有足够的信息之后，再思考规划解决方案。

UE是你不擅长的领域，不要凭感觉猜测，先学习，后思考规划方案。
```

## 功能描述

把一段中文文本（例："今天是不是很开心呀(laughs)，当然了！"）经 MiniMax 同步 TTS 合成为 PCM16 16 kHz mono 音频，运行时通过 NVIDIA ACE 本地 TRT 引擎（`LocalA2F-James`）推理成 blendshape 权重，驱动 `BP_MH_Character_1` MetaHuman 的面部动画，同时自动播放合成音频。按 `T` 键即刻出声 + 口型同步，`(laughs)` 处语音自带笑声、面部由 A2F 根据音频特征推理出相应表情。

**交付物**：

| 类型                 | 路径                                                                                    |
| -------------------- | --------------------------------------------------------------------------------------- |
| C++ 游戏模块（新建） | `Source/AILiveProject/`                                                                 |
| Target/Build（新建） | `Source/*.Target.cs`, `Source/AILiveProject/AILiveProject.Build.cs`                     |
| HTTP TTS 客户端      | `Source/AILiveProject/Public/MinimaxSpeechClient.h` + `Private/MinimaxSpeechClient.cpp` |
| BP 胶水库            | `Source/AILiveProject/Public/MinimaxACELibrary.h` + `Private/MinimaxACELibrary.cpp`     |
| 入口蓝图（改）       | `Content/MetaHumans/MH_Character_1/BP_MH_Character_1.uasset`                            |
| 工程配置（改）       | `AILiveProject.uproject`（新增 `Modules` 段）                                           |

---

## 实现逻辑

### 数据流

```
[BP InputKey T (Pressed)]
    │
    ▼
GetMinimaxApiKeyFromProjectEnv         ← 从 <ProjectDir>/.env 解析 `minimax=xxx`
    │ (ApiKey string)
    ▼
TriggerMinimaxSpeech(Character, Text, ApiKey, VoiceId, Endpoint, A2FProviderName)
    │
    ├─ 游戏线程：找/挂 UACEAudioCurveSourceComponent 到 Character
    │
    └─ 后台 ThreadPool 线程：
         │
         MinimaxSpeech::RequestBlocking(Req)
         │   POST https://api.minimaxi.com/v1/t2a_v2
         │   body: model=speech-2.8-turbo, voice_id=male-qn-qingse,
         │         language_boost=Chinese, output_format=hex,
         │         audio_setting={sample_rate:16000, format:pcm, channel:1}
         │   hex 解码 data.audio → TArray<int16> PCM
         │
         ▼
         FACERuntimeModule::Get().AnimateFromAudioSamples(
             Consumer         = UACEAudioCurveSourceComponent*,
             SamplesInt16     = PCM 数据,
             NumChannels      = 1,
             SampleRate       = 16000,
             bEndOfSamples    = true,              // 一次性整段投递
             EmotionParams    = NullOpt,
             Audio2FaceParams = nullptr,
             A2FProviderName  = "LocalA2F-James")   // 本机 TRT，非云端
         │
         ▼ (插件内部)
    A2XSession → A2FLocal TRT 推理 → blendshape 权重 → FAnimDataConsumerRegistry
         │
         ▼
    UACEAudioCurveSourceComponent 消费动画数据 + 内置 USoundWaveProcedural 同步播音
         │
         ▼
    Face_Archetype_Skeleton_AnimBP.AnimGraph 中的 FAnimNode_ApplyACEAnimation
         │
         ▼
    MetaHuman 每帧应用 face curves
```

### 关键点

1. **接入点选 `FACERuntimeModule::AnimateFromAudioSamples`** 而不是 `AnimateCharacterFromWavFileAsync`（后者只吃 WAV 文件，不支持运行时字节流）。官方文档原话就是这点 —— "plugin exposes a C++ API for runtime-generated audio"（`Plugins/NV_ACE_Reference/Source/ACERuntime/Public/ACERuntimeModule.h:52-64`）。

2. **`UACEAudioCurveSourceComponent` 同时是 `IACEAnimDataConsumer` 和音频播放器**（内部 `USoundWaveProcedural`），不需要额外塞 SoundWave。挂一个就足够收脸部动画 + 放音（`ACEAudioCurveSourceComponent.h:67-70, 200`）。

3. **Provider 名必须显式**。插件里 `GetDefaultProviderName()` **硬编码返回 `"RemoteA2F"`**（`ACERuntimeModule.cpp:183`），Remote 需 NVCF API key + endpoint URL。默认参数 `"Default"` 会被映射到 `"RemoteA2F"`，无配置就 `URL:""` 直接 RPC 失败。本地要显式传：
   - `"LocalA2F-James"`（男声面部模型，本测试用）
   - `"LocalA2F-Mark"`（男声，另一套）
   - `"LocalA2F-Claire"`（女声）

   这 3 个本地 provider 由子插件 `NvAudio2FaceJames/Mark/Claire` 分别注册。

4. **阻塞调用必须后台线程**。`AnimateFromAudioSamples` 注释明确 "will block until all samples have been sent"。同步 TTS 的 HTTP 请求也阻塞。两者合并走 `Async(EAsyncExecution::ThreadPool, ...)`，游戏线程不卡。

5. **`TWeakObjectPtr<UACEAudioCurveSourceComponent>` 防止 Character 中途销毁时野指针**。异步回调里 `.Get()` + IsValid 校验。

6. **MiniMax 端点 ≠ 文档首页显示的 `api.minimax.io`**。`sk-api-` 前缀的 key 属于国内区平台，实际工作端点是 **`https://api.minimaxi.com/v1/t2a_v2`**（或 `api.minimax.chat`）。`api.minimax.io` 用的是 JWT 格式 key（`eyJ...`）。这个踩一次坑就够。

7. **`output_format=hex` + `format=pcm`** 返回裸 PCM16 小端，`bytes.fromhex()` 后直接 `reinterpret_cast` 为 `int16*`。不用 WAV 解 RIFF header。`(laughs)` 情绪标签在 `speech-2.8-turbo` 里原生支持，不用额外 emotion 字段。

8. **MetaHuman Face AnimBP 里要有 `FAnimNode_ApplyACEAnimation`**。本项目里 `Face_Archetype_Skeleton_AnimBP.uasset` 已经有这个节点，连线是：`LinkedInputPose → PoseBlendNode(mh_arkit_mapping_pose_A2F) → ApplyACEAnimation → Root`。

### 关键 API 签名

```cpp
// Plugins/NV_ACE_Reference/Source/ACERuntime/Public/ACERuntimeModule.h:52-64
bool FACERuntimeModule::AnimateFromAudioSamples(
    IACEAnimDataConsumer* Consumer,
    TArrayView<const int16> Samples,
    int32 NumChannels,
    int32 SampleRate,
    bool bEndOfSamples,
    TOptional<FAudio2FaceEmotion> EmotionParameters,
    UAudio2FaceParameters* Audio2FaceParameters,
    FName A2FProviderName);
```

### BP 连线

- **BeginPlay** → EnableInput(PlayerController) → **PrewarmA2F**(`LocalA2F-James`) → **GetAvailableA2FProviders**（诊断日志）
- **InputKey T (Pressed)** → **GetMinimaxApiKeyFromProjectEnv** → **TriggerMinimaxSpeech**（Character=self，Text 为测试语句，ApiKey 连 GetKey.ReturnValue，VoiceId=`male-qn-qingse`，Endpoint 默认，A2FProviderName=`LocalA2F-James`）

`BP_MH_Character_1` 里旧的 WAV 测试链（PrintText → CreateA2FParams → AnimateCharacterFromWavFileAsync）被解挂但节点保留，便于对照 / 回滚。

---

## 反思

### 做对的事

- **先独立打通 HTTP**。用一次性 Python 探测把端点/key/format 这些不确定因素解决在 UE 外，避免后面在编译-重启循环里调试 HTTP schema。调通后立刻删掉 Python，项目单语言（C++）。
- **先读插件源码、再写代码**。先打开 `ACERuntimeModule.h` / `ACEAudioCurveSourceComponent.h` / `AsyncActionAnimateCharacter.cpp` 对比 3 个入口，选中正确的一条，而不是猜。省掉了用 `AnimateCharacterFromSoundWave` 那条死路的时间。
- **用 Monolith MCP 做 BP 改动**。直接 `build_blueprint_from_spec` + `connect_pins` + `compile_blueprint`，一次性完成，避免手点。
- **诊断函数 `GetAvailableA2FProviders`**。一行日志就证伪了"provider 没注册"的假设，定位到硬编码 `RemoteA2F` 才是根因。

### 踩过的坑

1. **`DefaultBuildSettings = V5` 冲突**。Installed Engine 模式下 Target 与 UnrealEditor 公用 build env，必须 V6。
2. **`FString::HexToBytes` 在 UE5.7 不是成员**。得自己写 4 行 hex 解码。
3. **`IHttpRequest::ProcessRequestUntilComplete()` 返回 `void` 不是 `bool`**。`if (!call())` 编译报 "void 不能逻辑非"。
4. **`sk-api-` key 走 `api.minimaxi.com`**，不是 `api.minimax.io`（后者要 JWT key）。官方首页 SEO 给的是国际版，容易误导。
5. **`"Default"` provider = Remote**。以为"Default"会聪明地优选 Local，实际写死 Remote。Plan 阶段没读完这函数的实现，多耗一轮 PIE 往返才定位。
6. **`BlueprintCallable` 带 exec pin 的"纯读"函数，如果 exec 不接会被剪枝**。`GetMinimaxApiKeyFromProjectEnv` 最初只接 Return Value 到 ApiKey 输入，BP 编译警告"节点被修剪"；需要把它串进 exec 流或改 `BlueprintPure`。选了前者（.env 读文件不算纯）。
7. **PIE 键盘焦点**。首次 PIE 没点 viewport 焦点，T 键直接吃进 Editor 快捷键，不触发任何 BP 节点。所有日志都没动静，最容易误判成"BP 断了"，其实是焦点问题。
8. **Live Coding 会在编辑器挂 shutdown 但进程没退时锁住 UBT**。首次从 Content-only 转 C++，主进程 PID 4744 悬挂 1300s CPU，必须强杀。以后遇到 shutdown 卡住且 Monolith MCP 已断开 ≈ 可以放心 kill。

### 值得沿用的模式

- **异步链 = GameThread 准备 + ThreadPool 执行 + 后台 Thread 直接调 AnimateFromAudioSamples**。不再回 GT，因为插件注释已经说"safe to call from any thread"。省掉一次 AsyncTask 往返。
- **`.env` 文件读取 BP 节点**。测试期代替环境变量/硬编码。发布前换成 `UDeveloperSettings` 或密钥仓库。
- **BP 侧保留旧测试链不删**。新旧并存、一键切换，比注释/分支更直观。

### 后续改进（未做）

- **流式 TTS**：`stream=true` 拿 chunk 一边收一边喂 `bEndOfSamples=false`，首字延迟从 ~4s 降到 <1s。但接口模型从 blocking 变 streaming，状态机复杂。
- **`.env` 只读一次**：现在每次按 T 都 `LoadFileToString`。缓存到静态变量或 `UDeveloperSettings`。
- **BP 里暴露情绪覆盖**。`FAudio2FaceEmotion` 现在传 `NullOpt`，实际可以根据文本里的情绪标签手动驱动 A2F emotion override，获得更夸张的表情。
- **错误反馈到 BP**。当前失败只走 Log，BP 层没得知成功/失败。可改为 `UBlueprintAsyncActionBase` + delegate，仿 `AsyncActionAnimateCharacter` 的模式。
- **避免首次 TRT 加载卡顿**：`PrewarmA2F` 已经在 BeginPlay 调了，但如果玩家很快按 T，仍会等一会。可以加一个 UI 提示 "warming up"。

### 验证日志样本（成功一次的截段）

```
LogACEA2FLocal: Allocation of instance of LocalA2F-James requested
LogMinimaxACE: Available A2F providers: [RemoteA2F, LegacyA2F, LocalA2F-Claire, LocalA2F-Mark, LocalA2F-James]
LogACEA2FLocal: Allocation of instance of LocalA2F-James complete
... (press T)
LogMinimaxSpeech: POST https://api.minimaxi.com/v1/t2a_v2 body=263 bytes
LogMinimaxSpeech: TTS ok trace=063a... samples=62044 sr=16000 dur=3.88s
LogMinimaxACE: Dispatching 62044 samples @ 16000 Hz to ACE (duration 3.88s)
LogMinimaxACE: AnimateFromAudioSamples returned true
```
