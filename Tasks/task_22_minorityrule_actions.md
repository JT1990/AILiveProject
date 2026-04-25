# T22 — 少数决专属 Action + Speak channel 扩展

## 目标
实现 4 个少数决专属 Action（AskQuestion / Vote / ProposeAlliance / AcceptAlliance），并给 Speak 加 `channel` 参数支持公开广播 vs 私聊。遵循 T11 的 Validate/Apply 拆分——Action 只做 TTS + Done(true)，状态修改交给 GM.Apply。Vote 走 EQS + SmartObject 流程。

## 前置
T21（GM 阶段机）+ T19（通用动作骨架）+ T11（GM 基类含 Validate/Apply 拆分）—— T19.5 / T19.7 / T18 已在 M0 完成，本卡直接消费它们

## DoD
- [ ] `UMindAction_AskQuestion`：解析 `{question_text}` → Speak 配音说出问题 → Done(true) → **GM.Apply 写入 `GM.State.CurrentQuestion` 并广播 OnGameEvent**（Action 不直接改 state）
- [ ] `UMindAction_Vote`：解析 `{choice: "yes"|"no", reasoning?: string}` →
  - **EQS_FindNearestVoteBox(choice)**（T19.5）→ 选 SlotActor
  - **ApproachAndUse(VoteBox slot, "Cast")**（T19.7）→ 走过去 + 触发投票交互
  - 完成后 Speak 掩护台词"我做出选择了" → Done(true)
  - **GM.Apply 写入 `GM.State.Players[i].CurrentVote`（私密，Tally 才公开）**
  - 失败（无可达 / Move 失败）→ Done(false) → GM 视为漏投
- [ ] `UMindAction_ProposeAlliance`：解析 `{target_agent_id, terms?: string}` → Speak 私聊"我想和你结盟" → Done(true) → GM.Apply 创建"待接受邀请"列表（Action 不直接改 state）
- [ ] `UMindAction_AcceptAlliance`：解析 `{from_agent_id}` → Speak 私聊"我接受" → Done(true) → GM.Apply 标记双方互为盟友 + 写双方私有记忆 "with X formed alliance"
- [ ] **私聊术语澄清**：MVP 不做 TTS 空间衰减（"表演层声音"全场可听）；区分发生在"信息层"——`RecordSpeechEvent` 通过 Perception hearing 距离过滤记忆写入。验收要检查 Memory Service 中谁收到了私聊记忆，不只靠人耳听
- [ ] `UMindAction_Speak` 改造：加 `channel` 参数
  - `"public"`（默认）：原行为，TriggerMinimaxSpeech 全场可听
  - `"private:<agent_id>"`：TTS 仍发声（**MVP 不做空间衰减**），但**记忆写入由 AI Perception 的 hearing 距离决定**（T18）：
    - 在发送方 hearing 范围内的 NPC 都写入此条记忆
    - 不在发送方的 sender→target line 但在 hearing 距离内的"第三方 NPC" → tag `eavesdrop=true`（暴露偷听风险）
    - 超出 hearing 范围的 NPC 完全不写
  - `"thought"`：完全不发声（等价 Think）
- [ ] **物理实现（基于 T18 Perception）**：GM `RecordSpeechEvent(speaker, text, channel)` 内部用 `MindComponent.CanSenseActor(other, Hearing)` 判定，自然形成"私聊在物理上就是不被听到"

## 关键文件
- 实现 `Public/Mind/GameMaster/Actions/MindAction_AskQuestion.cpp`
- 实现 `Public/Mind/GameMaster/Actions/MindAction_Vote.cpp`
- 实现 `Public/Mind/GameMaster/Actions/MindAction_ProposeAlliance.cpp`
- 实现 `Public/Mind/GameMaster/Actions/MindAction_AcceptAlliance.cpp`
- 修改 `Public/Mind/Actions/MindAction_Speak.cpp` 加 channel
- 修改 `MindGameMaster_MinorityRule.cpp` 加 alliance 状态管理

## 关键 API / 伪代码

```cpp
// MindAction_Vote.cpp（接 EQS + SmartObject）
UMindAction_Vote::UMindAction_Vote() {
    ActionName = "vote";
    Description = "Walk to a vote box and cast your secret vote (yes or no)";
    ParamSchemaJson = R"({"choice":"yes|no","reasoning":"optional string"})";
}
void UMindAction_Vote::Execute(UMindComponent* Owner, const FString& ParamsJson, FOnActionDone Done) {
    FString Choice; /* 解析 ... */
    UMindEQSHelpers::RunFindNearestVoteBox(Owner, Owner->GetOwner(), Choice,
        FOnEQSDone::CreateLambda([Owner, Choice, Done](bool ok, FVector Loc, AActor* SlotActor) {
            if (!ok || !SlotActor) { Done.ExecuteIfBound(false, "no votebox"); return; }
            UMindSmartObjectHelpers::ApproachAndUse(Owner, Owner->GetOwner(), SlotActor, "Cast",
                FOnSOUseDone::CreateLambda([Owner, Choice, Done](bool ok2, FString Reason) {
                    if (!ok2) { Done.ExecuteIfBound(false, Reason); return; }
                    // 成功后掩护台词（GM.Apply 在 OnActionDone 之后由 MindComponent 调用）
                    UMinimaxACELibrary::TriggerMinimaxSpeech(Owner, Owner->GetOwner(),
                        TEXT("我做出选择了"), /*FString，不是 FText*/ /*...*/);
                    Done.ExecuteIfBound(true, FString::Printf(TEXT("voted (secret) at %s box"), *Choice));
                }));
        }));
}
```

```cpp
// MindAction_Speak.cpp 改造
void UMindAction_Speak::Execute(UMindComponent* Owner, const FString& ParamsJson, FOnActionDone Done) {
    FString Text, Channel = "public";
    // 解析 text + channel
    AActor* Speaker = Owner->GetOwner();
    FString TargetId;
    if (Channel.StartsWith("private:")) {
        TargetId = Channel.Mid(8);
        // 1) TTS 仍发声但调小 attenuation（M4 简化：不调 attenuation，只标记记忆 channel）
    }
    UMinimaxACELibrary::TriggerMinimaxSpeech(Owner, Speaker, Text /*FString*/, ...);

    // 私聊记忆 → GM 决定写给谁
    auto* GM = Owner->GetGameMaster();
    if (GM) GM->RecordSpeechEvent(Owner->GetAgentId(), Text, Channel);

    Done.ExecuteIfBound(true, ...);
}
```

```cpp
// MindGameMaster_MinorityRule.cpp（基于 T18 Perception 实现"信息层私聊"——
// 表演层 TTS 仍全场可听，但记忆写入按 hearing 距离过滤）
void AMindGameMaster_MinorityRule::RecordSpeechEvent(AActor* SpeakerActor, const FString& Text, const FString& Channel) {
    auto* Mem = GetGameInstance()->GetSubsystem<UMindMemoryClient>();
    auto* SpeakerMC = SpeakerActor->FindComponentByClass<UMindComponent>();
    const FString From = SpeakerMC->GetAgentId();

    for (AActor* P : Participants) {
        auto* MC = P->FindComponentByClass<UMindComponent>();
        if (!MC) continue;
        const FString PId = MC->GetAgentId();

        bool bShouldWrite = false;
        TMap<FString,FString> Tags = {{"channel",Channel},{"speaker",From}};

        if (Channel == "public") {
            // 公开发言：所有 alive 都写
            bShouldWrite = true;
        } else if (Channel.StartsWith("private:")) {
            const FString Target = Channel.Mid(8);
            if (PId == From || PId == Target) {
                bShouldWrite = true;
            } else {
                // 第三方：用 Perception 判定能否偷听
                bool bCanHear = MC->CanSenseActor(SpeakerActor, "Hearing");
                if (bCanHear) {
                    bShouldWrite = true;
                    Tags.Add("eavesdrop", "1");
                }
            }
        }

        if (bShouldWrite) {
            FString Content = FString::Printf(TEXT("[%s] %s: %s"), *Channel, *From, *Text);
            Mem->Write(PId, Content, Tags);
        }
    }
}

void AMindGameMaster_MinorityRule::FormAlliance(const FString& A, const FString& B) {
    auto FindP = [&](const FString& Id) -> FMinorityRulePlayer* {
        return State.Players.FindByPredicate([&](auto& P){return P.AgentId==Id;});
    };
    if (auto* PA = FindP(A)) PA->AlliancePerception.AddUnique(B);
    if (auto* PB = FindP(B)) PB->AlliancePerception.AddUnique(A);
}
```

## 验收信号
- 跑 T23 端到端测试时观察：
- AskQuestion 阶段：被抽中 NPC 喊出问题（"你认为应该投资科技股还是消费股？"或类似）
- Negotiate 阶段：能听到至少 1 次 ProposeAlliance 私聊语音 + 接受 / 拒绝
- Vote 阶段：每 NPC 喊"我做出选择了"等掩护台词，但 GM 内部已记录真实票
- Tally 阶段：所有人能在 prompt 里 recall 到自己的私聊记忆 + 公开记忆

## 不在范围
- 私聊空间 attenuation（M5 polish 时再做）
- 联盟违约 / 撕毁动作（M5）
- 投票动画（M5 polish）

## 风险
- LLM 可能在 Vote 阶段直接说出"我投 yes" 暴露选择 — prompt 严格约束 + Speak 的 text 内容由 GM 简单关键词检查（含 yes/no 直接拦截重生成），但 MVP 暂不做拦截，只做约束 prompt
- Alliance 是双向同意但 GM 实现单纯标记两人；如果 A propose 但 B 拒绝（不调 AcceptAlliance），不应形成关系 — 用"待接受"队列，超时自动作废
- TTS 物理上仍全场可听（不做 attenuation），但**记忆侧基于 hearing 距离严格区分** —— LLM 通过记忆获得信息，物理 TTS 只是表演，不影响"信息不对称"
- EQS_FindNearestVoteBox 找不到（极端情况：所有投票箱被某种方式销毁）→ Done(false) → GM 视为漏投，玩家会被淘汰
