# T12 — `AMindGameMaster_LiarsBar` 阶段机 + 私有状态

## 目标
实现骗子酒馆完整的游戏循环：发牌 → 回合（出牌 / 挑战）→ 翻牌裁定 → 轮盘 → 检查胜负 → 循环。每个阶段切换时通过 `GM.RecordPhaseChange` 把 view + ActionRegistry schema push 到对应 agent 的 `Context.UpdateLayer0bFrame`，再 `AwakeAgent` 触发决策。**遵循 T11 的 Validate/Apply 拆分**。

## 前置
T11（GM 基类含 Validate/Apply + Initialize ownership）+ T03（LiarsBarState 骨架）+ T09（ContextManager 已就位）

## DoD
- [ ] 阶段定义（FName）：`Setup` / `Deal` / `PlayerTurn` / `ChallengeWindow` / `Reveal` / `Roulette` / `CheckWin` / `GameOver`
- [ ] `Phase_Setup`：洗牌、初始化 4 个玩家 hand + roulette
- [ ] `Phase_Deal`：抽 5 张/人，TransitionToPhase(`PlayerTurn`)
- [ ] `Phase_PlayerTurn`：给 `CurrentTurnAgent` 调 `GM.RecordPhaseChange(npc, "PlayerTurn", view_data, [PlayCards, PassTurn, Speak])` → 内部调 `npc.Context.UpdateLayer0bFrame` 全量原地覆盖 messages[1]（含 `Keys.Sort()` 后的 ActionRegistry schema）→ `AwakeAgent(Cur, Reason="your_turn")`
- [ ] **`Validate(play_cards)`**：纯只读检查 actual_indices 在 hand 范围内、claim_count==len(actual_indices)
- [ ] **`Apply(play_cards)`**：把 actual 牌从 hand 移到 secret pile，记 claim
- [ ] **`OnAgentActionFinished(play_cards, ok=true)`**：TransitionToPhase(`ChallengeWindow`)
- [ ] `Phase_ChallengeWindow`：给除 `CurrentTurnAgent` 外的 3 个玩家调 `RecordPhaseChange(..., [Challenge, Speak])`；并发 `AwakeAgent("challenge?")`；先到先得（一人 Challenge 即转 Reveal，其他 drop）
- [ ] **`Validate(challenge)`**：检查 phase==ChallengeWindow（先到先得逻辑：第一个 Validate 通过的赢；之后再来的会因 phase 已切到 Reveal 被 reject）
- [ ] **`Apply(challenge)`**：记录挑战者 ID
- [ ] **`OnAgentActionFinished(challenge, ok=true)`**：TransitionToPhase(`Reveal`)
- [ ] `Phase_Reveal`：翻牌验证 claim 真假；输方 = 谎称者（若 challenge 成功）或挑战者（若失败）→ TransitionToPhase(`Roulette`)
- [ ] `Phase_Roulette`：输方掷骰 1/N（N 由其当前 chamber 数）；命中 → bAlive=false；TransitionToPhase(`CheckWin`)
- [ ] `Phase_CheckWin`：alive 玩家 ≤ 1 → `GameOver`；否则切 `CurrentTurnAgent` 到下一个 alive 玩家 → TransitionToPhase(`PlayerTurn`)
- [ ] `Phase_GameOver`: 调 `GM.RecordSceneEnd()`（D1: scene = 一局游戏）→ Summarizer 限流非阻塞写 peer_summary
- [ ] `BuildViewFor(Agent)`：返回 `FMindMessage`（user role, channel=system, tags={phase, vis, hand}），由 `RecordPhaseChange` push 到 NPC Context.UpdateLayer0bFrame；内容含"我的手牌：[K, Q, K, A, ...]" + 当前 phase / 当前玩家 / 已出牌堆 / 每人剩余 chamber / 当前 claim
- [ ] `Validate` 严格按当前 phase 检查：`PlayerTurn` 阶段只接 `play_cards/pass_turn`，`ChallengeWindow` 只接 `challenge`
- [ ] 牌堆构成（`DA_GameConfig_LiarsBar`）：32 张 = K×8 + Q×8 + A×8 + Joker×8

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

    virtual FMindMessage BuildViewFor(AActor* Agent) override;
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
    void Phase_GameOver();

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
    else if (P == "GameOver")    Phase_GameOver();
}

void AMindGameMaster_LiarsBar::Phase_PlayerTurn() {
    AActor* Cur = FindActorByAgentId(State.CurrentTurnAgentId);
    TArray<TSubclassOf<UMindAction>> Actions = {
        UMindAction_PlayCards::StaticClass(),
        UMindAction_PassTurn::StaticClass(),
        UMindAction_Speak::StaticClass()
    };
    // RecordPhaseChange 内部: BuildViewFor(Cur) 产 FMindMessage
    //                        → Cur.Context.UpdateLayer0bFrame(view, Actions)
    //                                     全量原地覆盖 messages[1] (含 Keys.Sort() 后的 ActionRegistry schema)
    RecordPhaseChange(Cur, TEXT("PlayerTurn"), BuildViewFor(Cur), Actions);
    AwakeAgent(Cur, FString::Printf(TEXT("your_turn round=%d"), State.RoundNumber));
}

void AMindGameMaster_LiarsBar::Phase_GameOver() {
    RecordSceneEnd();   // 触发 Summarizer 限流非阻塞写 peer_summary; scene 立即结束
}

// === Validate (只读) ===
bool AMindGameMaster_LiarsBar::Validate(AActor* Agent, const FMindActionEnvelope& Env, FString& OutError) const {
    if (CurrentPhase == "PlayerTurn") {
        if (Env.ActionName != "play_cards" && Env.ActionName != "pass_turn" && Env.ActionName != "speak") {
            OutError = TEXT("only play_cards/pass_turn/speak allowed in PlayerTurn"); return false;
        }
        if (Env.ActionName == "play_cards") {
            // 解析 ParamsJson 验证 indices 在 hand 范围、count == indices.size
        }
        return true;
    }
    if (CurrentPhase == "ChallengeWindow") {
        if (Env.ActionName != "challenge" && Env.ActionName != "speak") {
            OutError = TEXT("only challenge/speak allowed in ChallengeWindow"); return false;
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

FMindMessage AMindGameMaster_LiarsBar::BuildViewFor(AActor* Agent) {
    auto* MC = Agent->FindComponentByClass<UMindComponent>();
    FString OwnId = MC->GetAgentId();
    auto* MyState = State.Players.FindByPredicate([&](auto& P){ return P.AgentId == OwnId; });

    FString Body = FString::Printf(TEXT(
        "[当前帧]\n"
        "- 阶段: %s (round %d)\n"
        "- 当前玩家: %s\n"
        "- 你的手牌: [%s]\n"
        "- 你的轮盘 chamber: %d (death prob 1/%d)\n"
        "- 公开桌面: %s\n"),
        *CurrentPhase.ToString(), State.RoundNumber, *State.CurrentTurnAgentId,
        *FormatHand(MyState->Hand), MyState->RouletteChamber, MyState->RouletteChamber,
        *FormatPublicState(State));

    return FMindMessage{
        .Role = EMindRole::User,
        .Content = Body,
        .Channel = EMindChannel::System,
        .Tags = {{"phase", CurrentPhase.ToString()}, {"vis", "private+public"}}
    };
}
```

## 验收信号
- 在 `Level_LiarsBar.umap` 的 GM 上调 `StartGame`
- Output Log 按 `LogMindGM` 过滤能看到完整阶段切换序列：
  `Setup → Deal → PlayerTurn(npc_2) → ChallengeWindow → Reveal → Roulette → CheckWin → PlayerTurn(npc_3) → ...`
- 4 NPC 各自的 messages[1] (L0b) 在阶段切换时被 RecordPhaseChange 全量原地覆盖（用 LogMind Verbose 抓 messages 切片验证）
- ActionRegistry schema 渲染顺序 `Keys.Sort()` 字节稳定（连续两次 PlayerTurn 的 messages[1] action 段字符相同）
- GameOver 时 RecordSceneEnd 触发，scene_end summarizer 后台跑（不阻塞）

## 不在范围
- 具体的 Action 实现（T13）
- HUD 显示（T14）
- 表演动画（T15）

## 风险
- `ChallengeWindow` 的并发：3 NPC 同时 RequestDecision，第一个 Challenge 通过后其他在途的 LLM 调用可能晚到——`Validate` 用 `CurrentPhase != "ChallengeWindow"` 拒绝即可
- 牌堆数量：4 人 × 5 张 = 20 张，32 张牌堆充足
- Roulette 概率公式：每输一次 chamber-=1，命中 1/chamber；初始 chamber=6 → 第 1 次 1/6，... 直到 1/1 必死
- 决策 cooldown=2s 与 ChallengeWindow 时间窗的相互作用：ChallengeWindow 没有显式时长，靠先到先得；如果所有 NPC 都不挑战需要 timeout（暂时简化为：每个 NPC 调用一次后若都没挑战则 auto-reveal）
