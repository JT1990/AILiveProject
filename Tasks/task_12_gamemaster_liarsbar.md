# T12 — `AMindGameMaster_LiarsBar` 阶段机 + 私有状态

## 目标
实现骗子酒馆完整的游戏循环：发牌 → 回合（出牌 / 挑战）→ 翻牌裁定 → 轮盘 → 检查胜负 → 循环。每个阶段切换时正确给对应 agent 注入可用动作集。**遵循 T11 重写的 Validate/Apply 拆分**。

## 前置
T11（GM 基类含 Validate/Apply 拆分）+ T03（LiarsBarState 骨架）

## DoD
- [ ] 阶段定义（FName）：`Setup` / `Deal` / `PlayerTurn` / `ChallengeWindow` / `Reveal` / `Roulette` / `CheckWin` / `GameOver`
- [ ] `Phase_Setup`：洗牌、初始化 4 个玩家 hand + roulette
- [ ] `Phase_Deal`：抽 5 张/人，TransitionToPhase(`PlayerTurn`)
- [ ] `Phase_PlayerTurn`：给 `CurrentTurnAgent` 注册 `[PlayCards, PassTurn]` + 通用 `Speak`；`AwakeAgent(Reason="your_turn")`
- [ ] **`Validate(play_cards)`**：纯只读检查 actual_indices 在 hand 范围内、claim_count==len(actual_indices)
- [ ] **`Apply(play_cards)`**：把 actual 牌从 hand 移到 secret pile，记 claim
- [ ] **`OnAgentActionFinished(play_cards, ok=true)`**：TransitionToPhase(`ChallengeWindow`)
- [ ] `Phase_ChallengeWindow`：给除 `CurrentTurnAgent` 外的 3 个玩家注册 `[Challenge, Speak]`；并发 `AwakeAgent("challenge?")`；先到先得（一人 Challenge 即转 Reveal，其他 drop）
- [ ] **`Validate(challenge)`**：检查 phase==ChallengeWindow（先到先得逻辑：第一个 Validate 通过的赢；之后再来的会因 phase 已切到 Reveal 被 reject）
- [ ] **`Apply(challenge)`**：记录挑战者 ID
- [ ] **`OnAgentActionFinished(challenge, ok=true)`**：TransitionToPhase(`Reveal`)
- [ ] `Phase_Reveal`：翻牌验证 claim 真假；输方 = 谎称者（若 challenge 成功）或挑战者（若失败）→ TransitionToPhase(`Roulette`)
- [ ] `Phase_Roulette`：输方掷骰 1/N（N 由其当前 chamber 数）；命中 → bAlive=false；TransitionToPhase(`CheckWin`)
- [ ] `Phase_CheckWin`：alive 玩家 ≤ 1 → `GameOver`；否则切 `CurrentTurnAgent` 到下一个 alive 玩家 → TransitionToPhase(`PlayerTurn`)
- [ ] `BuildViewFor(Agent)`：返回 `FMindAgentView`，PrivateStateText 写入"我的手牌：[K, Q, K, A, ...]"，PublicStateText 写入"当前玩家 X / 已出牌堆 / 每人剩余 chamber / 当前 claim"
- [ ] `Validate` 严格按当前 phase 检查：`PlayerTurn` 阶段只接 `play_cards/pass_turn`，`ChallengeWindow` 只接 `challenge`
- [ ] 牌堆构成（`DA_GameConfig_LiarsBar`）：32 张 = K×8 + Q×8 + A×8 + Joker×8（沿用游戏卡片描述，可调）

## 关键文件
- 实现 `Public/Mind/GameMaster/MindGameMaster_LiarsBar.h` + `.cpp`
- 新建 DataAsset `Content/MyAssets/MindConfigs/DA_GameConfig_LiarsBar.uasset`

## 关键 API / 伪代码

```cpp
// MindGameMaster_LiarsBar.h
UCLASS()
class AMindGameMaster_LiarsBar : public AMindGameMaster {
    GENERATED_BODY()
public:
    UPROPERTY(EditAnywhere) FLiarsBarTableState State;
    UPROPERTY(EditAnywhere) TObjectPtr<UDataAsset> GameConfig;

    virtual FMindAgentView BuildViewFor(AActor* Agent) override;
    virtual bool Validate(AActor* Agent, const FMindActionEnvelope& Env, FString& OutError) const override;
    virtual void Apply(AActor* Agent, const FMindActionEnvelope& Env) override;
    virtual void OnAgentActionFinished(AActor* Agent, const FMindActionEnvelope& Env, bool bOk) override;
protected:
    virtual void OnGameStart() override { TransitionToPhase("Setup"); }
    virtual void OnEnterPhase(FName P) override;

    void Phase_Setup();
    void Phase_Deal();
    void Phase_PlayerTurn();
    void Phase_ChallengeWindow();
    void Phase_Reveal();
    void Phase_Roulette();
    void Phase_CheckWin();

    FString GetCurrentTurnAgentId() const { return State.CurrentTurnAgentId; }
    AActor* FindActorByAgentId(const FString& Id) const;
    void AdvanceTurn();
    bool VerifyClaim(const TArray<int32>& ActualIndices, const FString& ClaimRank, int32 ClaimCount, FString& Why) const;
};
```

```cpp
// 关键阶段切换（伪代码）
void AMindGameMaster_LiarsBar::OnEnterPhase(FName P) {
    if (P == "Setup")            Phase_Setup();
    else if (P == "Deal")        Phase_Deal();
    else if (P == "PlayerTurn")  Phase_PlayerTurn();
    else if (P == "ChallengeWindow") Phase_ChallengeWindow();
    else if (P == "Reveal")      Phase_Reveal();
    else if (P == "Roulette")    Phase_Roulette();
    else if (P == "CheckWin")    Phase_CheckWin();
}

void AMindGameMaster_LiarsBar::Phase_PlayerTurn() {
    AActor* Cur = FindActorByAgentId(State.CurrentTurnAgentId);
    RegisterActionsForAgent(Cur, { UMindAction_PlayCards::StaticClass(), UMindAction_PassTurn::StaticClass() });
    AwakeAgent(Cur, FString::Printf(TEXT("your_turn round=%d"), State.RoundNumber));
}

// === Validate (只读) ===
bool AMindGameMaster_LiarsBar::Validate(AActor* Agent, const FMindActionEnvelope& Env, FString& OutError) const {
    if (CurrentPhase == "PlayerTurn") {
        if (Env.ActionName != "play_cards" && Env.ActionName != "pass_turn") {
            OutError = TEXT("only play_cards/pass_turn allowed in PlayerTurn"); return false;
        }
        if (Env.ActionName == "play_cards") {
            // 解析 ParamsJson 验证 indices 在 hand 范围、count == indices.size
            // 但不修改任何 state
        }
        return true;
    }
    if (CurrentPhase == "ChallengeWindow") {
        if (Env.ActionName != "challenge") {
            OutError = TEXT("only challenge allowed in ChallengeWindow"); return false;
        }
        return true;
    }
    OutError = FString::Printf(TEXT("phase %s does not accept %s"), *CurrentPhase.ToString(), *Env.ActionName);
    return false;
}

// === Apply (在 Action.Execute 成功 OnDone 之后调) ===
void AMindGameMaster_LiarsBar::Apply(AActor* Agent, const FMindActionEnvelope& Env) {
    if (Env.ActionName == "play_cards") {
        // 真正从 hand 移除 actual_indices 的牌、记 claim 到 State
    } else if (Env.ActionName == "challenge") {
        // 记录 challenger ID
    }
}

// === OnAgentActionFinished (Apply 之后调，决定是否切阶段) ===
void AMindGameMaster_LiarsBar::OnAgentActionFinished(AActor* Agent, const FMindActionEnvelope& Env, bool bOk) {
    if (!bOk) return;
    if (CurrentPhase == "PlayerTurn" && Env.ActionName == "play_cards") {
        TransitionToPhase("ChallengeWindow");
    } else if (CurrentPhase == "ChallengeWindow" && Env.ActionName == "challenge") {
        TransitionToPhase("Reveal");
    }
}

FMindAgentView AMindGameMaster_LiarsBar::BuildViewFor(AActor* Agent) {
    FMindAgentView V;
    auto* MC = Agent->FindComponentByClass<UMindComponent>();
    V.OwnAgentId = MC->Config->DisplayName.ToString();
    V.GameStage = CurrentPhase.ToString();

    // private: 自己手牌
    auto* MyState = State.Players.FindByPredicate([&](auto& P){ return P.AgentId == V.OwnAgentId; });
    V.PrivateStateText = FString::Printf(TEXT("Your hand: [%s]\nYour roulette chamber: %d (death prob 1/%d)"),
        *FormatHand(MyState->Hand), MyState->RouletteChamber, MyState->RouletteChamber);

    // public: 桌面状态
    V.PublicStateText = FString::Printf(TEXT("Round %d, Phase %s, Current turn: %s\n%s"),
        State.RoundNumber, *CurrentPhase.ToString(), *State.CurrentTurnAgentId, *FormatPublicState(State));
    return V;
}
```

## 验收信号
- 在 `Level_LiarsBar.umap` 的 GM 上调 `StartGame`
- Output Log 按 `LogMindGM` 过滤能看到完整阶段切换序列：
  `Setup → Deal → PlayerTurn(npc_2) → ChallengeWindow → Reveal → Roulette → CheckWin → PlayerTurn(npc_3) → ...`
- 4 NPC 各自的 RequestDecision 在对应阶段被触发
- 即使 NPC 的 LLM 还没接通完整 prompt，`AwakeAgent` 调用本身能成功（看到节流 / 排队日志）

## 不在范围
- 具体的 Action 实现（T13）
- HUD 显示（T14）
- 表演动画（T15）

## 风险
- `ChallengeWindow` 的并发：3 NPC 同时 RequestDecision，第一个 Challenge 通过后其他在途的 LLM 调用可能晚到——`Validate` 用 `CurrentPhase != "ChallengeWindow"` 拒绝即可
- 牌堆数量：4 人 × 5 张 = 20 张，32 张牌堆充足
- Roulette 概率公式确认：每输一次 chamber-=1，命中 1/chamber；初始 chamber=6 → 第 1 次 1/6，第 2 次 1/5，... 直到 1/1 必死
- 决策 cooldown=2s 与 ChallengeWindow 时间窗的相互作用：ChallengeWindow 没有显式时长，靠先到先得；如果所有 NPC 都不挑战需要 timeout（暂时简化为：每个 NPC 调用一次后若都没挑战则 auto-reveal）
