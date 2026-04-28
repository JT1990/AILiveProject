# NPC 视野进入/离开事件 + 上次见到位置

日期：2026-04-29

## 当前结论

本里程碑采用 **C++ `USightMemoryComponent` + C++ `AStoryScenarioDirector`**。

- `USightMemoryComponent` 挂在 `/Game/Blueprints/AI/AIC_NPC_SmartObject` 上，绑定 `UAIPerceptionComponent::OnTargetPerceptionUpdated`，维护每个 NPC 自己的 `CurrentlyVisibleActors` 和 `LastSeenLocations`。
- `AStoryScenarioDirector` 放在 `L_prison` 关卡里，PIE 中按 `O` 启动 NPC1/NPC2 餐厅相遇剧情。
- `L_prison` Level BP 已回退到 `159f708192f6782bc532b9bff7b120aafb33c317` 的听觉验证基线，再只做最小清理：移除未使用的 `NPC3Class` 到 `NPC8Class` 变量。
- 关卡蓝图不再包含旧的 `RunDialogueScene_*` / `Scene_*` 剧情节点，也不再包含 `Speak` 函数或 `SceneStory` 变量。
- 移动仍调用现有 `SandboxCharacter_Mover.MoveAndLookAt`，不重写 GASP / Mover / VisualOverride / NPC 父类链路。
- NPC1 的 `SightMemory.bDebugPrintScreen` 默认仍为 false；剧情开始时 Director 只临时打开 NPC1 controller 上的屏显，结束或失败后恢复。

## SightMemoryComponent

文件：

- `Source/AILiveProject/Public/SightMemoryComponent.h`
- `Source/AILiveProject/Private/SightMemoryComponent.cpp`

行为：

- 只处理 `UAISense_Sight` stimulus。
- 跳过自己和未实现 `AILiveAgent` interface 的 actor。
- 成功感知时立即写入 `LastSeenLocations[Actor] = Stimulus.StimulusLocation`。
- 从不可见转为可见时广播 `OnSightEnter(Other, Location)` 并记录 `[SightMemory] ENTER`。
- 从可见转为不可见时广播 `OnSightExit(Other, LastSeenLocation)`，保留 last-seen 位置不清除，并记录 `[SightMemory] EXIT`。
- BeginPlay 时若 `AIPerceptionComponent` 尚未就绪，按 0.1s 间隔最多重试 30 次绑定。

对 Mind 接管的契约：

- `GetCurrentlyVisibleActors`
- `IsCurrentlyVisible`
- `GetLastSeenLocation`
- `GetAllLastSeenLocations`
- `OnSightEnter`
- `OnSightExit`

## StoryScenarioDirector

文件：

- `Source/AILiveProject/Public/StoryScenarioDirector.h`
- `Source/AILiveProject/Private/StoryScenarioDirector.cpp`
- `Content/MyAssets/Levels/L_prison.umap` 中已放置 `StoryScenarioDirector`

运行流程：

1. PIE 中按 `O`。
2. Director 找到 `BP_NPC_MH_Character_1` 和 `BP_NPC_MH_Character_2` 实例。
3. Runtime 生成三个隐藏 `ATargetPoint`：NPC1 正前方目标、NPC2 原始位置目标、NPC2 回家朝向目标。
4. NPC2 调用现有 `MoveAndLookAt(MeetingTarget, NPC1)`，移动到 NPC1 面前。
5. 到达后播放 6 句对话：
   - NPC2：我们两个结盟吧。
   - NPC1：好的。
   - NPC2：一言为定。
   - NPC1：一言为定。
   - NPC2：回见。
   - NPC1：待会见。
6. 对话结束后停留 3 秒。
7. NPC2 调用 `MoveAndLookAt(HomeMoveTarget, HomeLookTarget)` 回到初始位置。

语音入口使用 `UMinimaxACELibrary::TriggerMinimaxSpeechFromPawnWithNoise`：

- 自动从 Pawn 的 `VisualOverride` ChildActor 找 A2F audio target。
- Pawn 本身作为 Hearing noise instigator。
- 如果 `.env` 缺少 `minimax=`，默认只跳过语音播放但继续移动流程，避免阻塞视野 ENTER/EXIT 验证。

## 验证

已完成：

- UBT 全量构建成功：`AILiveProjectEditor Win64 Development`。
- Monolith 确认 `AIC_NPC_SmartObject` 组件包含 `SightMemory`。
- Monolith 确认 `L_prison` 已保存 `StoryScenarioDirector` actor。
- `L_prison` Level BP 编译成功，0 error / 0 warning。
- `L_prison` Level BP validate 通过：无未使用变量、无断线节点、无 node error。
- `RunDialogueScene` / `Scene_` 搜索结果为 0。
- `DefaultEngine.ini` 中 `bTickPhysicsAsync=False` 未改动。
- `Source/*.Target.cs` 中 `DefaultBuildSettings = BuildSettingsVersion.V6` 未改动。

PIE 烟测步骤：

1. 打开 `L_prison`，进入 PIE。
2. 按 `O`。
3. 观察 NPC2 移动到 NPC1 面前。
4. Output Log 过滤 `LogSightMemory`，应看到 NPC1 controller 对 NPC2 的 `ENTER`。
5. 对话结束并停留 3 秒后，NPC2 回到初始位置。
6. Output Log 应看到 NPC1 controller 对 NPC2 的 `EXIT`，日志里包含 last-seen location。

## 复用边界

`SightMemoryComponent` 是长期系统能力，Mind 模块可直接读取或订阅。`StoryScenarioDirector` 是本里程碑验证入口，后续 Mind 接管时可删除按键触发和固定台词，保留 `SightMemoryComponent` 数据契约不变。
