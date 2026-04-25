# T24 — 多轮淘汰循环 + `/memory/by_tag`

## 目标
把少数决从单轮扩展为多轮（淘汰到剩 1-2 人），并在 Memory Service 加 `/memory/by_tag` 端点用于精确检索"承诺 vs 实际投票"对比。

## 前置
T23（M4 单轮通过）

## DoD
- [ ] `AMindGameMaster_MinorityRule::Phase_RoundEnd`：检查 alive 数量
  - alive ≤ 2：TransitionToPhase("GameOver") + 公布最终赢家
  - alive ≥ 3：清空 RoundEnd 数据 → RoundNumber++ → TransitionToPhase("AskQuestion") 重新出题
- [ ] 多轮间共享同一 GM 实例和同一 Memory Service connection；Participants 数组维持原 8 个，但 alive=false 的 NPC 跳过
- [ ] Memory Service 加 `/memory/by_tag`：
  - body: `{agent_id, tag_key, tag_value, top_n: int}`
  - 返回精确匹配该 tag 的最近 N 条
  - Cypher: `MATCH (a:Agent {id:$id})-[:REMEMBERS]->(m) WHERE m.tags[$key] = $val RETURN ... ORDER BY m.ts DESC LIMIT $n`
- [ ] `UMindMemoryClient::ByTag` 实现
- [ ] `UMindComponent::BuildPromptAndCallLLM` 在少数决投票阶段（Vote phase）额外注入"承诺 vs 实际投票"对比段：
  - ByTag(`type=intent`) 取最近 5 条该 agent 自己的意图声明
  - ByTag(`type=tally`) 取最近 5 条全局 tally 历史
  - ByTag(`channel=public`, `speaker=<peer>`) 取每个其他 alive agent 的最近 3 条公开发言
  - 拼成"承诺/历史/对手公开发言"三段
- [ ] 跨轮 prompt 系统段加上"You are in round N, alive: K. Past rounds eliminated: [...]"

## 关键文件
- 修改 `MindGameMaster_MinorityRule.cpp`（多轮循环）
- 修改 `Tools/MemoryService/main.py` + `neo4j_client.py`（by_tag 端点）
- 修改 `MindMemoryClient`（ByTag 函数）
- 修改 `MindComponent.cpp`（投票前额外检索）

## 关键 API / 伪代码

```python
# main.py
class ByTagReq(BaseModel):
    agent_id: str
    tag_key: str
    tag_value: str
    top_n: int = 5

@app.post("/memory/by_tag")
def by_tag(req: ByTagReq):
    with driver.session() as s:
        rows = s.execute_read(lambda tx: list(tx.run("""
            MATCH (a:Agent {id:$id})-[:REMEMBERS]->(m:Memory)
            WHERE m.tags[$k] = $v
            RETURN m.id AS id, m.content AS content, m.ts AS ts, m.tags AS tags
            ORDER BY m.ts DESC LIMIT $n
        """, id=req.agent_id, k=req.tag_key, v=req.tag_value, n=req.top_n)))
    return [{"id":r["id"],"content":r["content"],"score":1.0,"ts":r["ts"],"tags":r["tags"]} for r in rows]
```

```cpp
// MindGameMaster_MinorityRule.cpp
void AMindGameMaster_MinorityRule::Phase_RoundEnd() {
    int32 Alive = 0;
    for (auto& Pl : State.Players) if (Pl.bAlive) Alive++;
    if (Alive <= 2) {
        WriteEventMemoryToAll(FString::Printf(TEXT("Game over. Survivors: %s"),
            *FString::Join(GetAliveIds(), TEXT(","))), {{"type","game_end"}});
        TransitionToPhase("GameOver");
        return;
    }
    State.RoundNumber++;
    State.EliminatedThisRound.Empty();
    State.CurrentQuestion.Empty();
    for (auto& Pl : State.Players) Pl.CurrentVote = "";
    TransitionToPhase("AskQuestion");
}
```

```cpp
// MindComponent.cpp 投票阶段额外检索
void UMindComponent::BuildPromptAndCallLLM(const FString& Reason) {
    if (Reason == "vote_now" && GameMaster.IsValid()) {
        // 并发 4 个 ByTag 检索
        TFuture<...> intents = Mem->ByTag(AgentId, "type", "intent", 5);
        TFuture<...> tallies = Mem->ByTag(AgentId, "type", "tally", 5);
        // 等所有完成后拼 prompt
    }
    // 否则走原通用路径
}
```

## 验收信号
- 8 NPC 完整跑到剩 1-2 人，期间至少经过 3 轮投票
- 投票前 prompt 中能看到"过往承诺"段（有具体引用，不是空白）
- 至少观察到 1 次 NPC 在 Vote 后 reasoning 里写"我之前承诺投 yes 但因为 X 改投 no"
- HUD 历史记录滚动显示多轮 tally
- Memory Service `/memory/by_tag` 用 curl 测试通过

## 不在范围
- 联盟可视化 HUD（T25）
- 钻石分配 / 终局奖励（M6+）
- 跨 session 持久（重启游戏即清记忆是接受的）

## 风险
- 多轮记忆膨胀：每轮 ~50 条 / 每 NPC，跑到第 5 轮 = 250 条/NPC × 8 = 2000 节点。Neo4j 性能 OK，但 vector index 建议 T08 已建
- 投票阶段 prompt 可能变长（4 段检索 + 系统 + 用户）→ 7K-10K token，DeepSeek 上下文限制 32K 内安全，但响应延迟会变长
- 平票（Tally 中 yes==no）要重新出题；多次平票可能导致死循环 — T21 已设计为重新出题，本卡仅注意 round 不增（重新出题不算新一轮）
