# T16 — 跨局记忆引用 + 多 persona Config + 跨游戏关系模板

## 目标
让 NPC 在新一局开始时能引用上一局的关键事件（"上局 npc_3 在第 4 回合骗了我"），并用 4 套差异化 persona 让多 NPC 行为可观察地不同。在 prompt 中显式利用记忆系统的 per-peer 关系，让 NPC 跨游戏（M4 少数决也用同一 NPC 阵容）能延续"对每个具体同伴的认知"。

## 前置
T14（M2 完整跑通）+ T09（记忆系统已可读写）

## DoD
- [ ] `AMindGameMaster_LiarsBar` 在每个关键事件后追加结构化记忆（不是只 OnActionDone 那条）：
  - `play_cards` 后 → 给所有玩家写一条 `{type:"observation", round, actor, claim, channel:"public"}`
  - `reveal` 后 → 写 `{type:"reveal", round, actor, claim, actual, was_lying}`（**这条对所有 agent 写**——大家都看到了真相）
  - `roulette` 命中 → 写 `{type:"elimination", actor, roulette_chamber}`
- [ ] 一局开始时 GM 给每个参与 agent 调 `MemoryClient->Recall(query="last_session_with_<peer>", top_k=10)` 把上局 highlight 拼到 prompt（M2 的 Recall 已经在做，但 query 不太精准；M3 改用更针对性的 query）
- [ ] 4 个 `DA_AgentConfig_NPC2..5` 用 4 套差异化 persona：
  - NPC_2: "保守稳重，不喜欢冒险，倾向于打真牌；只在牌面对自己极不利时才说谎"
  - NPC_3: "激进好斗，喜欢加码，频繁谎称大牌，享受心理压力"
  - NPC_4: "善于伪装的战略家，混合真假各半，专注观察对手模式"
  - NPC_5: "保守跟风，倾向于跟随场上多数选择，不主动挑战"
- [ ] 同一 `DA_GameConfig_LiarsBar` 多局复用（不需要每局重置 DataAsset）
- [ ] **跨游戏关系 prompt 模板**：`UMindComponent::BuildSystemPrompt` 增加段：
  ```
  == 你与在场同伴的过往（自动从记忆检索） ==
  与 NPC_X：{recall_by_tags(speaker=X, top_3 by score)}
  与 NPC_Y：{recall_by_tags(speaker=Y, top_3 by score)}
  ...
  ```
  对当前 GM.Participants 中每个非自己的 agent 各做一次 ByTag 检索（speaker=peer_id）取 top_3。
  - 在 M3（骗子酒馆 polish）就实现这个段落，但只能拿到本游戏的同伴交互；
  - M4 少数决用同一阵容时，这段会自动包含"骗子酒馆里的过往"——这就是 PRD"具有连续性的社会人格"的落地点

## 关键文件
- 修改 `MindGameMaster_LiarsBar.cpp`（事件级记忆写入）
- 修改 `DA_AgentConfig_NPC2..5`（更新 Persona 文本）

## 关键 API / 伪代码

```cpp
// MindGameMaster_LiarsBar.cpp
void AMindGameMaster_LiarsBar::WriteEventMemoryToAll(const FString& EventDesc, const TMap<FString,FString>& Tags) {
    auto* Mem = GetGameInstance()->GetSubsystem<UMindMemoryClient>();
    for (AActor* P : Participants) {
        auto* MC = P->FindComponentByClass<UMindComponent>();
        if (MC) Mem->Write(MC->GetAgentId(), EventDesc, Tags);
    }
}

// 在 Phase_Reveal 实现里：
{
    bool bLying = !VerifyClaim(...);
    FString Desc = FString::Printf(TEXT("Round %d reveal: %s claimed [%s] but actually played [%s] — %s"),
        State.RoundNumber, *ActorId, *ClaimDesc, *ActualDesc, bLying ? TEXT("LIE") : TEXT("TRUTH"));
    WriteEventMemoryToAll(Desc, {{"type","reveal"}, {"round",FString::FromInt(State.RoundNumber)}, {"actor",ActorId}, {"was_lying", bLying?"1":"0"}});
}
```

## 验收信号
- 连跑 5 局：
  - 第 2 局起，至少 1 个 NPC 在自己回合的 Speak 引用上局事件（"上局你骗了我，我这次不信你"）
  - HUD 历史事件区能看到跨局事件（带 round 0 表示上局）
  - Memory Service 查询单 agent 记忆，能按 `tags.type=reveal` 过滤出所有 reveal 事件（需要 T24 的 by_tag，但本卡先用 by query 检验）
- 4 个 NPC 有可观察的策略差异：
  - NPC_2 的谎称比例 < 30%
  - NPC_3 的谎称比例 > 60%
  - NPC_3 的挑战频率 > 其他人
  - 这些观察记录到 DevLog（T17）

## 不在范围
- 接第二个 LLM 厂商（M5+ 再考虑；MVP 只 DeepSeek）
- 记忆重要性 / 衰减
- 自动多局连跑脚本

## 风险
- 4 个差异化 persona 可能不够——LLM 在同一基础模型下 persona 区分有限，可考虑同时调 temperature（NPC_3 高 temp / NPC_2 低 temp）增强差异
- 跨局记忆膨胀：每局 ~30-50 条事件，5 局后 200+ 条，Recall top_k=5 可能漏关键事件——可在 prompt 里同时附 recent N（M4 之后用 `/memory/recent`）
