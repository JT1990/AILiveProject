# T24 — 多轮淘汰循环 + 承诺/投票内省

## 目标
把少数决从单轮扩展为多轮（淘汰到剩 1-2 人）；让 NPC 在 Vote 阶段通过 buffer 内 `tags=intent/tally/public/speaker` 自然回看做"承诺 vs 实际投票"对比。**不依赖外部检索端点**——messages[] hot 区 + Layer 1 anchored summary + Layer 0b peer_summary 已经覆盖该信息。

## 前置
T23（M4 单轮通过）

## DoD
- [ ] `AMindGameMaster_MinorityRule::Phase_RoundEnd`：检查 alive 数量
  - alive ≤ 2：TransitionToPhase("GameOver") + 公布最终赢家
  - alive ≥ 3：清空 RoundEnd 数据 → RoundNumber++ → TransitionToPhase("AskQuestion") 重新出题
- [ ] 多轮间共享同一 GM 实例和同一 Memory Service connection；Participants 数组维持原 8 个，但 alive=false 的 NPC 跳过
- [ ] 每轮 Tally 后 `GM.RecordSpeechEvent(self=GM, "public", desc, listeners=All)`，desc 含 tags=`{"type":"tally", "round":N}` —— 进入所有 alive NPC 的 Layer 2 hot
- [ ] Decision Action（T19）写 event 时打 tags=`{"type":"intent"}`，同时 `Context.PushUser(EMindChannel::Private, "[私念意图] ...")` 进入自己 Layer 2 hot
- [ ] **不需要 BuildPromptAndCallLLM 额外检索**——LLM 在 Vote 阶段会自然从自己的 messages[] 中：
  - 看到 Layer 2 hot 中自己最近几轮 `[私念意图]` 条目
  - 看到 Layer 2 hot 中 GM tally 公开广播 `[公开] Round N tally: ...`
  - 看到 Layer 2 hot 中其他 NPC 的 `[公开]` 发言
  - 看到 Layer 0b 中 cross-scene peer_summary（如本场是跨场延续）
- [ ] 跨轮 prompt：`BuildViewFor` 在 messages[1] L0b 段加 "You are in round N, alive: K. Past rounds eliminated: [...]"
- [ ] **极端场景兜底**：如果某局极长（10+ 轮），早期 intent 被 Compact 进 AnchoredSummary，LLM 仍可以主动调 `recall_long_term_memory` tool 查跨场 cold 数据；这条由 Provider tool_call 自动支持，不需要 GM 协调

## 关键文件
- 修改 `MindGameMaster_MinorityRule.cpp`（多轮循环 + Tally 广播 tags）
- 修改 `MindGameMaster_MinorityRule.cpp::BuildViewFor`（加 round / alive 信息）

## 关键 API / 伪代码

```cpp
// MindGameMaster_MinorityRule.cpp
void AMindGameMaster_MinorityRule::Phase_RoundEnd() {
    int32 Alive = 0;
    for (auto& Pl : State.Players) if (Pl.bAlive) Alive++;
    if (Alive <= 2) {
        FString Desc = FString::Printf(TEXT("Game over. Survivors: %s"),
            *FString::Join(GetAliveIds(), TEXT(",")));
        RecordSpeechEvent(this, TEXT("public"), Desc, GetAllActiveAgents(),
            /*tags*/{{"type","game_end"}});
        TransitionToPhase("GameOver");
        return;
    }
    State.RoundNumber++;
    State.EliminatedThisRound.Empty();
    State.CurrentQuestion.Empty();
    for (auto& Pl : State.Players) Pl.CurrentVote = "";
    TransitionToPhase("AskQuestion");
}

void AMindGameMaster_MinorityRule::Phase_Tally() {
    // ... 统计票数 ...
    FString Desc = FString::Printf(TEXT("Round %d tally: yes=%d (%s) | no=%d (%s) | eliminated: %s"),
        State.RoundNumber, Yes, *FString::Join(YesIds, TEXT(",")),
        No, *FString::Join(NoIds, TEXT(",")),
        *FString::Join(Loser, TEXT(",")));
    // 公开广播 + 打 tally tag (其他 NPC 在 Layer 2 hot 中可自然回看)
    RecordSpeechEvent(this, TEXT("public"), Desc, GetAllActiveAgentsBeforeElim(),
        /*tags*/{{"type","tally"}, {"round",FString::FromInt(State.RoundNumber)}});
    TransitionToPhase("Eliminate");
}
```

## 验收信号
- 8 NPC 完整跑到剩 1-2 人，期间至少经过 3 轮投票
- 投票前 Vote 阶段 NPC 的 messages[] Layer 2 hot 中能看到自己最近几轮的 `[私念意图]` 条目（用 LogMind Verbose 抓切片验证）
- Layer 2 hot 中能看到 `[公开] Round N tally: ...` 历史
- 至少观察到 1 次 NPC 在 Vote 后 reasoning 里写"我之前承诺投 yes 但因为 X 改投 no"（说明它确实回看了 buffer 中的 intent）
- HUD 历史记录滚动显示多轮 tally
- 长会话：第 5 轮时验证 Compact 是否触发；如触发，AnchoredSummary 中应保留早期 commitment（手工 spot check）

## 不在范围
- 联盟可视化 HUD（T25）
- 钻石分配 / 终局奖励（M6+）
- 额外的 ByTag 检索（不需要——buffer 自然回看 + recall tool fallback 已覆盖）

## 持久化范围（明确）
- **MVP 验收**：同一 Memory Service 进程内跨关卡、跨局持久（M5 跨 LiarsBar/MinorityRule 验证）
- **可选**：重启 PIE 后 Memory Service 进程不重启则保留；但 ContextManager.buffer 会丢（D2: MVP 不落盘 buffer）
- **不要求**：重启 Neo4j / 清库后的恢复

## 风险
- 多轮 event 累积：每轮 ~50 条 / 每 NPC，跑到第 5 轮 = 250 条/NPC × 8 = 2000 节点。Neo4j 性能 OK
- Vote 阶段 prompt 可能变长（buffer 越往后越大）→ 触发 Compact，AnchoredSummary 可能漂移；M5 spot check 验证
- 平票（Tally 中 yes==no）要重新出题；多次平票可能导致死循环 — T21 已设计为重新出题，本卡仅注意 round 不增（重新出题不算新一轮）
- 极长会话（10+ 轮）下早期 intent 被 Compact 摘要后细节可能模糊——靠 LLM 主动调 recall_long_term_memory tool 兜底
