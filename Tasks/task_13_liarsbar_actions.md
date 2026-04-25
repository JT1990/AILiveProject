# T13 — `PlayCards / Challenge / PassTurn` 三个游戏动作

## 目标
实现骗子酒馆的 3 个专属 Action 类。**遵循 T11 重写后的 Validate/Apply 拆分**：Action 本身不修改游戏状态——状态修改由 GM.Apply 完成；Action 只做"参数解析 + 触发 TTS/动画 + Done(true) 让 MindComponent 通知 GM 去 Apply"。

## 前置
T12（GM 阶段机能跑）+ T11（DispatchAction → Validate → Execute → Apply 链路）

## DoD
- [ ] `UMindAction_PlayCards::Execute`：
  - 解析 `{claim_rank: "K"|"Q"|"A"|"Joker", claim_count: 1-3, actual_indices: int[]}`
  - 触发 Speak TTS（fire-and-forget，不阻塞）配音"我打了 X 张 Y"
  - 立即 `Done(true, "claimed N R")` —— 真正的 hand 修改由 GM.Apply 完成
- [ ] `UMindAction_Challenge::Execute`：
  - 触发 TTS"我质疑！"
  - `Done(true, "challenge")`
- [ ] `UMindAction_PassTurn::Execute`：
  - 触发 TTS"我跳过这轮"
  - `Done(true, "pass")`
- [ ] **schema 在 ParamSchemaJson 字段中暴露给 LLM prompt**（构造函数里设）
- [ ] **状态修改路径**（按 T11 重写后的流程）：
  ```
  Dispatch(env={action:play_cards,...})
    ├─ GM.Validate: 检查 indices 在 hand 范围、count 一致 → 通过
    ├─ Action.Execute: 触发 TTS + Done(true)
    ├─ HandleActionDone:
    │   ├─ GM.Apply: 真正从 hand 移除 actual_indices 的牌、记 claim
    │   └─ 写记忆
    └─ GM.OnAgentActionFinished: 检查是否切到 ChallengeWindow
  ```

## 关键文件
- 修改 `Public/Mind/GameMaster/Actions/MindAction_PlayCards.h` + `.cpp`
- 修改 `Public/Mind/GameMaster/Actions/MindAction_Challenge.h` + `.cpp`
- 修改 `Public/Mind/GameMaster/Actions/MindAction_PassTurn.h` + `.cpp`

## 关键 API / 伪代码

```cpp
// MindAction_PlayCards.cpp
UMindAction_PlayCards::UMindAction_PlayCards() {
    ActionName = TEXT("play_cards");
    Description = TEXT("Play cards face-down with a public claim. You may lie about rank/count.");
    ParamSchemaJson = TEXT(R"({
      "claim_rank": "string in [K, Q, A, Joker]",
      "claim_count": "int 1..3",
      "actual_indices": "int[] of indices into your hand (0-based)"
    })");
}

void UMindAction_PlayCards::Execute(UMindComponent* Owner, const FString& ParamsJson, FOnActionDone Done) {
    TSharedPtr<FJsonObject> P;
    auto Reader = TJsonReaderFactory<>::Create(ParamsJson);
    if (!FJsonSerializer::Deserialize(Reader, P)) { Done.ExecuteIfBound(false, "bad json"); return; }
    FString Rank;
    int32 Count;
    if (!P->TryGetStringField("claim_rank", Rank) || !P->TryGetNumberField("claim_count", Count)) {
        Done.ExecuteIfBound(false, "missing fields"); return;
    }

    // 调 Speak 配音（用 ResolveSpeechActor 拿可见 child actor，不是壳 Pawn）
    AActor* Speaker = UMindSpeechHelpers::ResolveSpeechActor(Owner->GetOwner());
    FString Line = FString::Printf(TEXT("我打了 %d 张 %s"), Count, *Rank);
    UMinimaxACELibrary::TriggerMinimaxSpeech(
        Owner,
        Speaker,
        Line,                                                           // FString，不是 FText
        UMinimaxACELibrary::GetMinimaxApiKeyFromProjectEnv(),
        Owner->Config->MinimaxVoiceId,
        TEXT("https://api.minimaxi.com/v1/t2a_v2"),                     // 显式默认；不要传 {}
        Owner->Config->A2FProviderName);                                // FName

    // 简化：不等 TTS 完成（异步），直接 Done。表演 polish 见 T15
    Done.ExecuteIfBound(true, FString::Printf(TEXT("claimed %d %s"), Count, *Rank));
}
```

```cpp
// MindAction_Challenge.cpp
UMindAction_Challenge::UMindAction_Challenge() {
    ActionName = TEXT("challenge");
    Description = TEXT("Call BS on the previous play. Triggers reveal.");
    ParamSchemaJson = TEXT("{}");
}
void UMindAction_Challenge::Execute(UMindComponent* Owner, const FString& ParamsJson, FOnActionDone Done) {
    AActor* Speaker = UMindSpeechHelpers::ResolveSpeechActor(Owner->GetOwner());
    UMinimaxACELibrary::TriggerMinimaxSpeech(
        Owner, Speaker, TEXT("我质疑！"),
        UMinimaxACELibrary::GetMinimaxApiKeyFromProjectEnv(),
        Owner->Config->MinimaxVoiceId,
        TEXT("https://api.minimaxi.com/v1/t2a_v2"),
        Owner->Config->A2FProviderName);
    Done.ExecuteIfBound(true, "challenged");
}
```

```cpp
// MindAction_PassTurn.cpp
UMindAction_PassTurn::UMindAction_PassTurn() {
    ActionName = TEXT("pass_turn");
    Description = TEXT("Skip this round (rarely a good idea unless you have no playable cards)");
    ParamSchemaJson = TEXT(R"({"reason":"optional string"})");
}
// Execute 类似（短 Speak）
```

## prompt 中 schema 暴露方式

`UMindComponent::BuildSystemPrompt` 改造（增加段）：
```
Available actions (you must pick one):
1. {ActionName}: {Description}
   Params schema: {ParamSchemaJson}
2. ...
```

## 验收信号

执行 T12 验收的同时：
- LLM 决策返回 `play_cards` 时 → Validate 通过 → Action.Execute 触发 NPC 喊"我打了 X 张 Y" + 口型同步 → Apply 修改 hand
- 后续 ChallengeWindow 阶段，某个 NPC 选择 challenge → 喊"我质疑！" + GM 切 Reveal
- Output Log 按 `LogMindAction` 过滤能看到 `play_cards: claimed 2 K`、`challenge: from npc_3`

## 不在范围
- 出牌动画 / 拿枪动画（T15）
- claim 与 actual 不一致时的"被拆穿后表演"（T15）
- 多 challenge 竞争优胜机制的复杂逻辑（T12 已是先到先得）

## 风险
- LLM 输出 `actual_indices` 范围越界（手牌已经只剩 3 张但它写 `[0,1,4]`）— GM.Validate 处理（拒绝）
- LLM claim_count != actual_indices.size — GM.Validate 处理
- TTS 失败时 Action 仍 Done(true)：可接受，TTS 失败不应阻塞游戏推进（沿用 T06 容错思路）
- **GM.Apply 是同步的，但 TTS 是异步的——表演（说出"我打了 X"）会在 hand 实际改动**之后**几百毫秒才发声**：观众视角看是先静默改 HUD（discardpile +1）再听到 NPC 喊。可接受，M3 polish 时通过 Montage 同步表演（T15）
