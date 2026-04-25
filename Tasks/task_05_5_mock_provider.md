# T05.5 — Mock LLM Provider（开发期加速）

## 目标
做一个 `UMindLLMProvider_Mock`，从 DataAsset 读固定 ActionEnvelope 序列返回（或简单规则生成），开发期默认用它，验收前才切真 DeepSeek。**目的：把每张实施卡的迭代速度提升 5-10×**——免去每次改 prompt 都等 2-4s 真调用。

## 前置
T05（DeepSeek Provider 已实现，证明抽象基类工作）

## DoD
- [ ] `UMindLLMProvider_Mock` 类（`Public/Mind/MindLLMProvider_Mock.h`）继承 `UMindLLMProvider`
- [ ] 持有 `UMindMockResponseTable` DataAsset 引用
- [ ] `RequestCompletion` 行为：
  - 模拟延迟（默认 100ms，可配）
  - 按 trigger reason / phase 匹配 mock 表里的预设响应
  - 没有匹配项就返回固定的 `{action:"speak",params:{text:"<mock idle>"},reasoning:"mock fallback"}`
- [ ] `UMindMockResponseTable`：每行 `{match_pattern:string, response_json:string}`，支持简单 substring match
- [ ] 提供 3 个预设 mock 表 DataAsset：
  - `DA_Mock_LiarsBar.uasset`：覆盖骗子酒馆每个 phase 的几个典型动作（play_cards 真出 / play_cards 谎称 / challenge / pass）
  - `DA_Mock_MinorityRule.uasset`：覆盖少数决（ask_question / vote yes / vote no / propose_alliance / speak）
  - `DA_Mock_Generic.uasset`：M1 dry-run 用，只输出 speak
- [ ] 切换机制：`UMindAgentConfig.ProviderClass` 从 DeepSeek 改 Mock 即可（已存在的字段，无需改 schema）

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
    UPROPERTY(EditAnywhere) FString MatchPattern;     // substring of trigger reason
    UPROPERTY(EditAnywhere, meta=(MultiLine=true)) FString ResponseJson;
    UPROPERTY(EditAnywhere) float Weight = 1.0f;      // 多个 match 时按权重随机
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
    const FString& Sys, const FString& User,
    const TArray<TSubclassOf<UMindAction>>& Actions,
    FOnLLMResult Done)
{
    if (!Table) { Done.ExecuteIfBound(false, "no table"); return; }

    // 简单匹配：在 User prompt 里查 MatchPattern
    TArray<const FMockResponseRow*> Hits;
    for (auto& R : Table->Rows) if (User.Contains(R.MatchPattern)) Hits.Add(&R);
    const FString Out = Hits.Num() > 0
        ? Hits[FMath::RandRange(0, Hits.Num()-1)]->ResponseJson
        : Table->FallbackResponseJson;

    // 模拟延迟
    FTimerHandle H;
    GEngine->GetTimerManager()->SetTimer(H, FTimerDelegate::CreateLambda([Done, Out]() {
        Done.ExecuteIfBound(true, Out);
    }), MockDelaySeconds, false);
}
```

## 关键 mock 数据（举例 DA_Mock_LiarsBar）

```
Row 1: pattern="your_turn"  response={"action":"play_cards","reasoning":"...","params":{"claim_rank":"K","claim_count":2,"actual_indices":[0,1]}}
Row 2: pattern="your_turn"  response={"action":"play_cards","reasoning":"撒谎","params":{"claim_rank":"A","claim_count":3,"actual_indices":[0,1,2]}}
Row 3: pattern="challenge?" response={"action":"challenge","reasoning":"我不信"}
Row 4: pattern="challenge?" response={"action":"speak","params":{"text":"我相信你"}}
Fallback: {"action":"speak","params":{"text":"嗯..."}}
```

## 验收信号
- M2 实施期间把 `DA_AgentConfig_NPC2..5` 的 `ProviderClass` 切到 `UMindLLMProvider_Mock` + `Table=DA_Mock_LiarsBar`
- PIE 启动后整局骗子酒馆走完仅需 30s（vs 真 LLM 的 5-10 分钟）
- 切回 DeepSeek 后行为正常（验收前的最后一步）
- T05 dry-run 仍能用 mock 起步：在 `DA_AgentConfig_NPC1` ProviderClass 切到 Mock + `DA_Mock_Generic`，按 T 看到 mock 速度的 speak

## 不在范围
- 智能 mock（用规则生成更真实的响应）
- 录制真 LLM 响应回放（更高级，M6+）

## 风险
- Mock 行为太理想会让 M2/M4 的真 LLM 验收时暴露真问题（如 JSON 格式漂移）—— **这是好事**：Mock 帮助开发，真 LLM 验收暴露真分布
- 如果 mock 表不全覆盖，遇到 fallback 太多导致游戏推进异常 — 加日志 `LogMind: mock fallback (no match)`，开发期看到太多就补表
