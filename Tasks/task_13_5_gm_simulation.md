# T13.5 — Deterministic GM Simulation

## 目标
不接真 LLM，用**手工构造的 `FMindActionEnvelope` 序列**驱动 `AMindGameMaster_LiarsBar` 跑 3 回合。验证规则机器本身正确——把"规则 bug"和"LLM 格式 bug"分开排查。M2 内部插桩，T13 之后、T14 之前。

## 前置
T13（PlayCards / Challenge / PassTurn Action 实现）

## DoD
- [ ] 新增测试入口 `UMindLiarsBarSimulator`（BlueprintFunctionLibrary，仅 Editor build 开放，`#if WITH_EDITOR`）
  - `SimulateMatch(GM, EnvelopeSeed, RngSeed)` —— 按预定 envelope 序列驱动 GM
- [ ] 至少覆盖以下 6 个场景（每个场景独立 entry）：
  - `Sim_PlayCards_Legal`：合法 play_cards → Validate pass → Apply 修改 hand → 切 ChallengeWindow
  - `Sim_PlayCards_Illegal`：actual_indices 越界 → Validate reject → State 不变
  - `Sim_Challenge_FirstWins`：两个 NPC 几乎同时 Challenge，第二个被 Phase 已切走拒绝
  - `Sim_NoChallenge_AutoReveal`：所有 NPC 都不挑战，timeout 后 GM 自动切 Reveal
  - `Sim_Roulette_Hit`：固定 RNG seed → 命中 → bAlive=false
  - `Sim_Roulette_Miss`：固定 seed → 未命中 → chamber-=1
- [ ] 测试期间 **mock 掉 Speak / TTS / Memory Write**：
  - 注入一个 `UMindLLMProvider_Mock` + 简化 ActionRegistry，让 Action.Execute 不真调 TriggerMinimaxSpeech
  - Memory.Write 也走 mock subsystem 仅记日志
- [ ] 每个场景 Output Log 必须输出：
  - 期望阶段序列 vs 实际阶段序列
  - State diff（hand / chamber / discard / current_turn）
  - Validate reject 详情（如有）
- [ ] 全程**不调用** DeepSeek / MiniMax / Memory Service / LLM HTTP

## 关键文件
- 新建 `Source/AILiveProject/Private/Mind/GameMaster/MindLiarsBarSimulator.cpp`（用 `WITH_EDITOR` 包裹）
- 新建 `Source/AILiveProject/Public/Mind/GameMaster/MindLiarsBarSimulator.h`
- 可选：新建 `Content/MyAssets/Tests/BP_LiarsBarSimulatorRunner.uasset`，用 BP 调 simulator 然后 print

## 关键 API / 伪代码

```cpp
#if WITH_EDITOR
UCLASS()
class UMindLiarsBarSimulator : public UBlueprintFunctionLibrary {
    GENERATED_BODY()
public:
    // 一个 entry per scenario，不要塞一个巨大 SimulateAll
    UFUNCTION(BlueprintCallable, Category="AI Live|Test")
    static bool Sim_PlayCards_Legal(AMindGameMaster_LiarsBar* GM);

    UFUNCTION(BlueprintCallable, Category="AI Live|Test")
    static bool Sim_PlayCards_Illegal(AMindGameMaster_LiarsBar* GM);

    // ... 其余 4 个

private:
    static FMindActionEnvelope MakeEnvelope_PlayCards(FString Rank, int32 Count, TArray<int32> Indices);
    static FMindActionEnvelope MakeEnvelope_Challenge();
    static void DispatchManually(AMindGameMaster_LiarsBar* GM, AActor* Agent, const FMindActionEnvelope& Env);
    static void DumpStateDiff(const FLiarsBarTableState& Before, const FLiarsBarTableState& After);
};
#endif
```

`DispatchManually` 直接调 `MindComponent->DispatchAction(env)`，绕开 LLM 路径——这是测试的本质：让规则机自己跑通完整闭环。

## 验收信号

每个 Sim_* 跑完都要：
- 返回 true（场景通过）
- Output Log 看到 `[LiarsBarSim] expect phase: A→B→C, actual: A→B→C` 完全一致
- 失败场景（reject）能看到 `Validate rejected: <reason>` 且 State diff 为空

至少 5/6 场景通过才进 T14。

## 不在范围
- LLM 真调用（T14）
- 表演动画 / TTS（用 mock 跳过）
- HUD（T14）
- 自动化 CI（仅手动跑）

## 风险
- 手工 envelope 的 JSON schema 与未来 LLM 真实输出可能漂移——本卡通过的不代表真 LLM 一定能产出合法 envelope；T14 跑 LLM 仍可能暴露 prompt 工程问题
- `UMindLLMProvider_Mock` 在 simulator 里如何注入到现有 NPC？方案：simulator 内部临时替换 `MindComponent.Provider` 指针，结束后恢复
- 跨多个 simulator 调用时 GM 状态污染——每个 Sim_ 入口先调 GM.StartGame 重置
