# T21 — `AMindGameMaster_MinorityRule` 阶段机 + LLM Budget 分两池

## 目标
实现 8 人版少数决完整阶段机：出题 → 自由谈判（60s）→ 投票 → 计票 → 淘汰。本卡只做单轮（多轮循环 T24）。同时落地全局 LLM Budget 两池机制（防 Negotiate 阶段 QPS 失控 + Compact / scene_end Summarizer 与决策路径解耦）。

## 前置
T19（通用动作完整）+ T11（GM 基类）+ T03（MinorityRuleState 骨架）+ T09（ContextManager + UMindLLMBudgetSubsystem 已就位）

## DoD
- [ ] 阶段：`Setup` / `AskQuestion` / `Negotiate` / `Vote` / `Tally` / `Eliminate` / `RoundEnd` / `GameOver`
- [ ] `Phase_Setup`：初始化 8 玩家（agent_id / alive=true / no_vote）；分配铭牌钻石数量 = 1（象征值）
- [ ] `Phase_AskQuestion`：随机抽 1 个 alive 玩家，调 `RecordPhaseChange(npc, "AskQuestion", view, [AskQuestion])` + `AwakeAgent`；其 `AskQuestion` Submit 后切到 Negotiate
- [ ] `Phase_Negotiate`（60s 现实时间）：
  - 给所有 alive agent 调 `RecordPhaseChange(npc, "Negotiate", view, [Speak, ProposeAlliance, AcceptAlliance, Think, Decision])`
  - 启动一个 60s `FTimerHandle`
  - 期间任何 agent 可多次 RequestDecision（节流由 MindComponent.cooldown=2s 处理）
  - GM 主动每 `NegotiateWakeIntervalSeconds`（来自 DataAsset，基于 T14.5 实测填）给随机 alive agent 调一次 AwakeAgent("negotiate_tick")
  - 60s 到期 → TransitionToPhase("Vote")
- [ ] `Phase_Vote`：调 `RecordPhaseChange(npc, "Vote", view, [Vote])` 给所有 alive；同时 Awake 全员；30s timeout（未投者自动废票，淘汰）
- [ ] `Phase_Tally`：统计票数；多数派全部 elim；少数派全部存活；公开发言 `GM.RecordSpeechEvent(self=GM, "public", "Round X tally: yes=N, no=M, eliminated=[...]", listeners=All)`
- [ ] `Phase_Eliminate`：被淘汰的 NPC 切 alive=false；动画或表演（M5 polish），暂时只 Hidden=true
- [ ] `Phase_RoundEnd`：本卡只做"切到 GameOver"，多轮循环留 T24
- [ ] `Phase_GameOver`：调 `GM.RecordSceneEnd()`（D1: scene = 一局游戏）→ Summarizer 限流非阻塞写 peer_summary
- [ ] `BuildViewFor` 产 `FMindMessage`：private = 自己当前投票意向 + 自己识别的盟友列表；public = 当前问题 / 历史轮次公开数据 / 所有 alive 玩家列表 / 自己的剩余钻石数 / `CurrentIntent`（如有）
- [ ] **`Validate`（只读）**：按 phase 严格验证（AskQuestion 阶段只接 ask；Negotiate 接 Speak/Propose/Accept/Think/Decision；Vote 接 Vote）
- [ ] **`Apply`（修改）**：在 Action.Execute OnDone 后才修改 GM 状态（写 question / vote / alliance）
- [ ] **`OnAgentActionFinished`**：决定是否切阶段（如 AskQuestion → Negotiate）

## 全局 LLM 预算（分两池）

- [ ] `UMindLLMBudgetSubsystem` 持有两池字段：
  - `MaxConcurrentReasoner = 4`（决策路径——MindComponent 主 LLM 调用 + recall tool_call 循环）
  - `MaxConcurrentSummarizer = 2`（Compact 增量摘要 + scene_end per-peer 摘要）
- [ ] Token 类型枚举 `EMindLLMBudgetKind`：`Decision` / `RecallRetry` / `Compact` / `SceneSummary`；前两类计入 reasoner pool，后两类计入 summarizer pool
- [ ] MindComponent 在 `Calling` 状态进入/退出时 `Budget.Acquire(Decision)` / `Release(Decision)`；满 → drop（带日志，不排队）
- [ ] Provider 内部 tool_call 循环不额外占 reasoner slot（同一次 RequestCompletion 串行循环）
- [ ] Summarizer 调 `Budget.SubmitTask(SceneSummary, lambda)` 排队（不 drop——summary 是必须完成的，排队等 slot）
- [ ] Negotiate 阶段对所有 alive NPC 调 `SetPerceptionCanTriggerDecision(false)`，避免感知触发与 GM 唤醒并发；阶段结束恢复

## 关键文件
- 实现 `Public/Mind/GameMaster/MindGameMaster_MinorityRule.h` + `.cpp`
- 完善 `Public/Mind/GameMaster/State/MinorityRuleState.h`
- 实现 `Public/Mind/MindLLMBudget.h/.cpp`（如 T09 未完成则在本卡补）
- 新建 `DA_GameConfig_MinorityRule.uasset`：NegotiateSeconds=60、VoteTimeoutSeconds=30、MinAliveToContinue=2、NegotiateWakeIntervalSeconds=10

## 关键 API / 伪代码

```cpp
// MinorityRuleState.h
USTRUCT(BlueprintType)
struct FMinorityRulePlayer {
    GENERATED_BODY()
    UPROPERTY() FString AgentId;
    UPROPERTY() bool bAlive = true;
    UPROPERTY() int32 Diamonds = 1;
    UPROPERTY() FString CurrentVote;       // "yes" / "no" / "" (未投)
    UPROPERTY() TArray<FString> AlliancePerception;
};

USTRUCT(BlueprintType)
struct FMinorityRuleSessionState {
    GENERATED_BODY()
    UPROPERTY() TArray<FMinorityRulePlayer> Players;
    UPROPERTY() FString CurrentQuestion;
    UPROPERTY() FString CurrentQuestionerId;
    UPROPERTY() int32 RoundNumber = 0;
    UPROPERTY() TArray<FString> EliminatedThisRound;
    UPROPERTY() TArray<FString> PublicHistoryLog;
};
```

```cpp
// MindLLMBudget.h
UENUM()
enum class EMindLLMBudgetKind : uint8 {
    Decision,        // → reasoner pool
    RecallRetry,     // → reasoner pool (实际不占 slot, Provider 内部循环)
    Compact,         // → summarizer pool
    SceneSummary,    // → summarizer pool
};

UCLASS()
class UMindLLMBudgetSubsystem : public UGameInstanceSubsystem {
    GENERATED_BODY()
public:
    UPROPERTY(EditAnywhere) int32 MaxConcurrentReasoner = 4;
    UPROPERTY(EditAnywhere) int32 MaxConcurrentSummarizer = 2;

    bool TryAcquire(EMindLLMBudgetKind Kind);   // 失败返回 false (Decision/RecallRetry: drop)
    void Release(EMindLLMBudgetKind Kind);
    void SubmitTask(EMindLLMBudgetKind Kind, TFunction<void()> Task);   // 排队 (Compact/SceneSummary: 等 slot)

private:
    int32 ActiveReasonerCount = 0;
    int32 ActiveSummarizerCount = 0;
    TQueue<TFunction<void()>> SummarizerQueue;
};
```

```cpp
// MindGameMaster_MinorityRule.cpp
void AMindGameMaster_MinorityRule::OnEnterPhase(FName P) {
    if (P == "Setup") Phase_Setup();
    else if (P == "AskQuestion") Phase_AskQuestion();
    else if (P == "Negotiate") Phase_Negotiate();
    else if (P == "Vote") Phase_Vote();
    else if (P == "Tally") Phase_Tally();
    else if (P == "Eliminate") Phase_Eliminate();
    else if (P == "RoundEnd") TransitionToPhase("GameOver");
    else if (P == "GameOver") Phase_GameOver();
}

void AMindGameMaster_MinorityRule::Phase_Negotiate() {
    TArray<TSubclassOf<UMindAction>> NegotiateActions = {
        UMindAction_Speak::StaticClass(),
        UMindAction_ProposeAlliance::StaticClass(),
        UMindAction_AcceptAlliance::StaticClass(),
        UMindAction_Think::StaticClass(),
        UMindAction_Decision::StaticClass(),
    };
    for (auto& Pl : State.Players) {
        if (!Pl.bAlive) continue;
        AActor* A = FindActorByAgentId(Pl.AgentId);
        RecordPhaseChange(A, TEXT("Negotiate"), BuildViewFor(A), NegotiateActions);
        AwakeAgent(A, FString::Printf(TEXT("negotiate_open question=%s"), *State.CurrentQuestion));
    }

    GetWorld()->GetTimerManager().SetTimer(NegotiateTimer, [this]() {
        TransitionToPhase("Vote");
    }, GameConfig->NegotiateSeconds, false);

    GetWorld()->GetTimerManager().SetTimer(NegotiateTickTimer, [this]() {
        // 随机挑一个 alive 调 AwakeAgent("negotiate_tick")
    }, GameConfig->NegotiateWakeIntervalSeconds, true);
}

void AMindGameMaster_MinorityRule::Phase_Vote() {
    GetWorld()->GetTimerManager().ClearTimer(NegotiateTickTimer);
    for (auto& Pl : State.Players) {
        if (!Pl.bAlive) continue;
        AActor* A = FindActorByAgentId(Pl.AgentId);
        RecordPhaseChange(A, TEXT("Vote"), BuildViewFor(A), { UMindAction_Vote::StaticClass() });
        AwakeAgent(A, "vote_now");
    }
    GetWorld()->GetTimerManager().SetTimer(VoteTimer, [this]() {
        TransitionToPhase("Tally");
    }, GameConfig->VoteTimeoutSeconds, false);
}

void AMindGameMaster_MinorityRule::Phase_Tally() {
    int32 Yes = 0, No = 0;
    TArray<FString> YesIds, NoIds;
    for (auto& Pl : State.Players) {
        if (!Pl.bAlive) continue;
        if (Pl.CurrentVote == "yes") { Yes++; YesIds.Add(Pl.AgentId); }
        else if (Pl.CurrentVote == "no") { No++; NoIds.Add(Pl.AgentId); }
    }
    if (Yes == No) { TransitionToPhase("AskQuestion"); return; }
    TArray<FString> Loser = (Yes > No) ? YesIds : NoIds;
    State.EliminatedThisRound = Loser;

    FString Desc = FString::Printf(TEXT("Round %d tally: yes=%d (%s) | no=%d (%s) | eliminated: %s"),
        State.RoundNumber, Yes, *FString::Join(YesIds, TEXT(",")),
        No, *FString::Join(NoIds, TEXT(",")),
        *FString::Join(Loser, TEXT(",")));
    // 公开广播给所有 alive listeners (包括即将被淘汰者, 让他们也"听到"结果)
    RecordSpeechEvent(this, TEXT("public"), Desc, GetAllActiveAgentsBeforeElim());
    TransitionToPhase("Eliminate");
}

void AMindGameMaster_MinorityRule::Phase_GameOver() {
    RecordSceneEnd();   // 限流非阻塞 Summarizer
}
```

## 验收信号
- 在 GM 上调 StartGame
- Output Log `LogMindGM` 看到完整阶段切换：
  `Setup → AskQuestion(npc_X) → Negotiate(60s) → Vote → Tally → Eliminate → GameOver`
- AskQuestion 阶段：被抽中的 NPC 触发 RequestDecision，能调 LLM
- Negotiate 60s 期间：log 显示多个 NPC 调用 RequestDecision；reasoner pool 计数（in-flight ≤ 4）
- Vote 阶段：所有 alive NPC 触发 RequestDecision
- GameOver 时 Summarizer 排队执行（summarizer pool 计数 ≤ 2）；scene 切换不阻塞

## 不在范围
- AskQuestion / Vote / ProposeAlliance / AcceptAlliance Action 实现（T22）
- 多轮淘汰循环（T24）
- 联盟可视化（T25）
- HUD（T23 包）

## 风险
- Negotiate 60s 内 8 NPC 的 LLM 并发：cooldown=2s 下每 NPC 最多 30 次决策；reasoner pool MaxConcurrent=4 兜底，超出 drop（带日志）
- Tally 平票机制：原著 6 小时重新投票，本方案改为重新出题——文档化
- 8 NPC 全部 alive=false 的极端情况（理论不可能但要防御）
- Summarizer 排队：scene_end 一次提交 8 个任务，按 MaxConcurrent=2 串行 4 批，每批可能 5-15s；总耗时 20-60s。**scene 切换不等 summarizer**（非阻塞），下场启动时若 peer_summary 未到达用 fallback
