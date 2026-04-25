# T21 — `AMindGameMaster_MinorityRule` 阶段机

## 目标
实现 8 人版少数决完整阶段机：出题 → 自由谈判（60s）→ 投票 → 计票 → 淘汰。本卡只做单轮（多轮循环 T24）。

## 前置
T19（通用动作完整）+ T11（GM 基类）+ T03（MinorityRuleState 骨架）

## DoD
- [ ] 阶段：`Setup` / `AskQuestion` / `Negotiate` / `Vote` / `Tally` / `Eliminate` / `RoundEnd` / `GameOver`
- [ ] `Phase_Setup`：初始化 8 玩家（agent_id / alive=true / no_vote）；分配铭牌钻石数量 = 1（象征值）
- [ ] `Phase_AskQuestion`：随机抽 1 个 alive 玩家，注入 `[AskQuestion]` 动作；Awake 该 agent；其 `AskQuestion` Submit 后切到 Negotiate
- [ ] `Phase_Negotiate`（60s 现实时间）：
  - 给所有 alive agent 注入 `[Speak(channel), ProposeAlliance, AcceptAlliance, Think, Decision]`
  - 启动一个 60s `FTimerHandle`
  - 期间任何 agent 可多次 RequestDecision（节流由 MindComponent.cooldown=2s 处理）
  - GM 主动每 10s 给随机 alive agent 调一次 AwakeAgent("negotiate_tick")，鼓励对话
  - 60s 到期 → TransitionToPhase("Vote")
- [ ] `Phase_Vote`：注入 `[Vote]` 给所有 alive；同时 Awake 全员；30s timeout（未投者自动废票，淘汰）
- [ ] `Phase_Tally`：统计票数；多数派全部 elim；少数派全部存活；写公开记忆"Round X tally: yes=N, no=M, eliminated=[...]"
- [ ] `Phase_Eliminate`：被淘汰的 NPC 切 alive=false；动画或表演（M5 polish），暂时只 Hidden=true
- [ ] `Phase_RoundEnd`：本卡只做"切到 GameOver"，多轮循环留 T24
- [ ] `BuildViewFor`：private = 自己当前投票意向（Vote 阶段才有；Negotiate 阶段空）+ 自己识别的盟友列表；public = 当前问题 / 历史轮次公开数据 / 所有 alive 玩家列表 / 自己的剩余钻石数
- [ ] `ValidateAndApply`：按 phase 严格验证（AskQuestion 阶段只接 ask；Negotiate 接 Speak/Propose/Accept/Think/Decision；Vote 接 Vote）

## 关键文件
- 实现 `Public/Mind/GameMaster/MindGameMaster_MinorityRule.h` + `.cpp`
- 完善 `Public/Mind/GameMaster/State/MinorityRuleState.h`
- 新建 `DA_GameConfig_MinorityRule.uasset`：NegotiateSeconds=60、VoteTimeoutSeconds=30、MinAliveToContinue=2

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
    UPROPERTY() TArray<FString> AlliancePerception;  // 自我认定的盟友
};

USTRUCT(BlueprintType)
struct FMinorityRuleSessionState {
    GENERATED_BODY()
    UPROPERTY() TArray<FMinorityRulePlayer> Players;
    UPROPERTY() FString CurrentQuestion;
    UPROPERTY() FString CurrentQuestionerId;
    UPROPERTY() int32 RoundNumber = 0;
    UPROPERTY() TArray<FString> EliminatedThisRound;
    UPROPERTY() TArray<FString> PublicHistoryLog;  // 历史公开事件
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
    else if (P == "RoundEnd") TransitionToPhase("GameOver");  // 单轮，T24 改
}

void AMindGameMaster_MinorityRule::Phase_Negotiate() {
    for (auto& Pl : State.Players) {
        if (!Pl.bAlive) continue;
        AActor* A = FindActorByAgentId(Pl.AgentId);
        RegisterActionsForAgent(A, {
            UMindAction_Speak::StaticClass(),
            UMindAction_ProposeAlliance::StaticClass(),
            UMindAction_AcceptAlliance::StaticClass(),
            UMindAction_Think::StaticClass(),
            UMindAction_Decision::StaticClass(),
        });
        AwakeAgent(A, FString::Printf(TEXT("negotiate_open question=%s"), *State.CurrentQuestion));
    }

    GetWorld()->GetTimerManager().SetTimer(NegotiateTimer, [this]() {
        TransitionToPhase("Vote");
    }, GameConfig->NegotiateSeconds, false);

    // 每 10s 推一个 agent 说话
    GetWorld()->GetTimerManager().SetTimer(NegotiateTickTimer, [this]() {
        // 随机挑一个 alive 调 AwakeAgent("negotiate_tick")
    }, 10.0f, true);
}

void AMindGameMaster_MinorityRule::Phase_Vote() {
    GetWorld()->GetTimerManager().ClearTimer(NegotiateTickTimer);
    for (auto& Pl : State.Players) {
        if (!Pl.bAlive) continue;
        AActor* A = FindActorByAgentId(Pl.AgentId);
        RegisterActionsForAgent(A, { UMindAction_Vote::StaticClass() });
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
        // 未投 = 自动淘汰
    }
    TArray<FString>& Loser = (Yes == No) ? TArray<FString>{} : (Yes > No ? YesIds : NoIds);
    TArray<FString>& Winner = (Yes == No) ? TArray<FString>{} : (Yes < No ? YesIds : NoIds);
    if (Yes == No) {
        // 重新出题
        TransitionToPhase("AskQuestion");
        return;
    }
    State.EliminatedThisRound = Loser;
    // 给所有 agent 写公开记忆
    FString Desc = FString::Printf(TEXT("Round %d tally: yes=%d (%s) | no=%d (%s) | eliminated: %s"),
        State.RoundNumber, Yes, *FString::Join(YesIds, TEXT(",")), No, *FString::Join(NoIds, TEXT(",")),
        *FString::Join(Loser, TEXT(",")));
    WriteEventMemoryToAll(Desc, {{"type","tally"},{"round",FString::FromInt(State.RoundNumber)}});
    TransitionToPhase("Eliminate");
}
```

## 验收信号
- 在 GM 上调 StartGame
- Output Log `LogMindGM` 看到完整阶段切换：
  `Setup → AskQuestion(npc_X) → Negotiate(60s) → Vote → Tally → Eliminate → GameOver`
- AskQuestion 阶段：被抽中的 NPC 触发 RequestDecision，能调 LLM
- Negotiate 60s 期间：log 显示多个 NPC 调用 RequestDecision（即使 LLM 还没返回有意义的输出 — 验证调度）
- Vote 阶段：所有 alive NPC 触发 RequestDecision

## 不在范围
- AskQuestion / Vote / ProposeAlliance / AcceptAlliance Action 实现（T22）
- 多轮淘汰循环（T24）
- 联盟可视化（T25）
- HUD（T23 包）

## 风险
- Negotiate 60s 内 8 NPC 的 LLM 并发：cooldown=2s 下每 NPC 最多 30 次决策，整体 240 次 — 远超 DeepSeek 限额。**必须配合 GM 主动调度**：每 10s 只 awake 1 个 NPC（rotation），其他人靠 cooldown 自然 drop
- Tally 平票机制：原著 6 小时重新投票，本方案改为重新出题 — 文档化
- 8 NPC 全部 alive=false 的极端情况（理论不可能但要防御）
