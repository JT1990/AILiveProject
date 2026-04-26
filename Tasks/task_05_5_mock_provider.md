# T05.5 — Mock LLM Provider（开发期加速）

## 目标
做一个 `UMindLLMProvider_Mock`，从 DataAsset 读固定 ActionEnvelope 序列返回（或简单规则生成），开发期默认用它，验收前才切真 DeepSeek。**目的：把每张实施卡的迭代速度提升 5-10×**——免去每次改 prompt 都等 2-4s 真调用。

## 前置
T02（Provider 抽象基类骨架 + `FMindMessage` struct）—— **不依赖 T05**，本卡可在 T02 后立刻实现，让 T06/T07 用 Mock 跑通 Speak 闭环，T05 真 DeepSeek 并行/后置

## DoD
- [ ] `UMindLLMProvider_Mock` 类（`Public/Mind/MindLLMProvider_Mock.h`）继承 `UMindLLMProvider`
- [ ] 持有 `UMindMockResponseTable` DataAsset 引用（从 `UMindAgentConfig.MockResponseTable` 字段拷过来）
- [ ] `RequestCompletion(const TArray<FMindMessage>& Messages, const TArray<FToolSchema>& Tools, FOnLLMResult Done)` 行为：
  - 模拟延迟（默认 100ms，可配）
  - **匹配优先级**：① `MatchTrigger` 精确匹配 `Context->LastTriggerTag` > ② `MatchPattern` substring（扫 system 段所有内容 + 最近一条 user message content 合集）> ③ Fallback
  - 没有匹配项就返回 fallback `{action:"speak",params:{text:"<mock idle>"},reasoning:"mock fallback"}`
  - 命中多条同优先级时按 `Weight` 加权随机
- [ ] override `GetContextWindowSize()` 返回 `128000`（与 DeepSeek 默认一致，避免阈值切换异常）
- [ ] override `GetMaxToolIterations()` 返回 `0`（Mock 不模拟 tool_call 循环；recall 验收必须切真 DeepSeek）
- [ ] override `RegisterTool` no-op（开发期 Mock 不需要工具）
- [ ] `UMindMockResponseTable` 行结构 `FMockResponseRow`：
  - `MatchTrigger: FString` —— 精确匹配 `LastTriggerTag`（如 `"vote_now" / "bidding" / "challenge"`）
  - `MatchPattern: FString` —— substring of `system + 最近 user`
  - `ResponseJson: FString`（多行）
  - `Weight: float = 1.0`
- [ ] 提供 3 个预设 mock 表 DataAsset：
  - `DA_Mock_LiarsBar.uasset`：每个 phase 各几条 trigger row（bidding / challenge / reveal / roulette）
  - `DA_Mock_MinorityRule.uasset`：每个 phase 各几条 trigger row（ask_question / vote / propose_alliance / negotiate）
  - `DA_Mock_Generic.uasset`：M1 dry-run 用，只输出 speak
- [ ] 切换机制：`UMindAgentConfig.ProviderClass` 从 DeepSeek 改 Mock 即可（已存在的字段，无需改 schema）
- [ ] 日志：`[LogMind] Mock matched trigger=<X>` / `[LogMind] Mock matched pattern=<X>` / `[LogMind] Mock fallback (no match)`——开发期看 fallback 频次决定补表

## 关键文件
- 新建 `Public/Mind/MindLLMProvider_Mock.h` + `.cpp`
- 新建 `Public/Mind/MindMockResponseTable.h`（UPrimaryDataAsset）
- 新建 3 个 DataAsset 实例

## 关键 API / 伪代码

```cpp
// MindMockResponseTable.h
USTRUCT(BlueprintType)
struct FMockResponseRow {
    GENERATED_BODY()
    UPROPERTY(EditAnywhere) FString MatchTrigger;     // 精确匹配 LastTriggerTag
    UPROPERTY(EditAnywhere) FString MatchPattern;     // substring of system + last user
    UPROPERTY(EditAnywhere, meta=(MultiLine=true)) FString ResponseJson;
    UPROPERTY(EditAnywhere) float Weight = 1.0f;
};

UCLASS(BlueprintType)
class UMindMockResponseTable : public UPrimaryDataAsset {
    GENERATED_BODY()
public:
    UPROPERTY(EditAnywhere) TArray<FMockResponseRow> Rows;
    UPROPERTY(EditAnywhere) FString FallbackResponseJson;
};
```

```cpp
// MindLLMProvider_Mock.cpp
void UMindLLMProvider_Mock::RequestCompletion(
    const TArray<FMindMessage>& Messages,
    const TArray<FToolSchema>& Tools,
    FOnLLMResult Done)
{
    if (!Table) { Done.ExecuteIfBound(false, "no table"); return; }

    // 1. 拼搜索文本: 所有 system 段 + 最近一条 user message
    FString SystemContent, LastUserContent;
    for (auto& M : Messages) {
        if (M.Role == EMindRole::System) SystemContent += M.Content + TEXT("\n");
        if (M.Role == EMindRole::User)   LastUserContent = M.Content;  // 覆盖到最后一条
    }
    FString SearchText = SystemContent + TEXT("\n") + LastUserContent;

    // 2. 取 LastTriggerTag (由 ContextManager.SetTrigger 设置, Subsystem/Owner 取)
    FString TriggerTag = ResolveLastTriggerTag(/*owner ctx*/);

    // 3. 优先级匹配
    TArray<const FMockResponseRow*> Hits;
    for (auto& R : Table->Rows)
        if (!R.MatchTrigger.IsEmpty() && R.MatchTrigger == TriggerTag) Hits.Add(&R);

    if (Hits.Num() == 0) {
        for (auto& R : Table->Rows)
            if (!R.MatchPattern.IsEmpty() && SearchText.Contains(R.MatchPattern)) Hits.Add(&R);
    }

    const FString Out = Hits.Num() > 0
        ? PickByWeight(Hits)->ResponseJson
        : Table->FallbackResponseJson;

    // 模拟延迟
    if (UWorld* W = GEngine->GetWorldFromContextObjectChecked(this)) {
        FTimerHandle H;
        W->GetTimerManager().SetTimer(H, FTimerDelegate::CreateLambda([Done, Out]() {
            Done.ExecuteIfBound(true, Out);
        }), MockDelaySeconds, false);
    } else {
        FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
            [Done, Out](float)->bool { Done.ExecuteIfBound(true, Out); return false; }),
            MockDelaySeconds);
    }
}
```

## 关键 mock 数据（举例 DA_Mock_LiarsBar）

```
Row 1: trigger="bidding"   pattern=""  response={"action":"play_cards","reasoning":"...","params":{"claim_rank":"K","claim_count":2,"actual_indices":[0,1]}}
Row 2: trigger="bidding"   pattern=""  response={"action":"play_cards","reasoning":"撒谎","params":{"claim_rank":"A","claim_count":3,"actual_indices":[0,1,2]}}
Row 3: trigger="challenge" pattern=""  response={"action":"challenge","reasoning":"我不信"}
Row 4: trigger="challenge" pattern=""  response={"action":"speak","params":{"text":"我相信你"}}
Fallback: {"action":"speak","params":{"text":"嗯..."}}
```

## 验收信号
- M2 实施期间把 `DA_AgentConfig_NPC2..5` 的 `ProviderClass` 切到 `UMindLLMProvider_Mock` + `Table=DA_Mock_LiarsBar`
- PIE 启动后整局骗子酒馆走完仅需 30s（vs 真 LLM 的 5-10 分钟）
- 切回 DeepSeek 后行为正常（验收前的最后一步）
- T05 dry-run 仍能用 mock 起步：在 `DA_AgentConfig_NPC1` ProviderClass 切到 Mock + `DA_Mock_Generic`，按 T 看到 mock 速度的 speak
- **优先级单元测试**：构造已知 messages + LastTriggerTag，验证 trigger 命中时不走 pattern；trigger miss 但 pattern 命中时走 pattern；都 miss 走 fallback

## 不在范围
- 智能 mock（用规则生成更真实的响应）
- 录制真 LLM 响应回放（更高级，M6+）
- tool_call 循环（Mock 不支持；recall 验收切真 DeepSeek）

## 风险
- Mock 行为太理想会让 M2/M4 的真 LLM 验收时暴露真问题（如 JSON 格式漂移）—— **这是好事**：Mock 帮助开发，真 LLM 验收暴露真分布
- 如果 mock 表不全覆盖，遇到 fallback 太多导致游戏推进异常 — 看日志 `LogMind: mock fallback (no match)` 频次，开发期看到太多就补表
- `LastTriggerTag` 跨 NPC / 跨场要确保 game thread 隔离（per-agent ContextManager 持有，本 Provider 通过 owner ctx 取，不共享）
