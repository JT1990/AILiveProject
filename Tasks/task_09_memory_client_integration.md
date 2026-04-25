# T09 — `UMindMemoryClient` + 决策前后自动写/读

## 目标
UE 端实现 `UMindMemoryClient`（GameInstanceSubsystem），并改造 `UMindComponent::BuildPromptAndCallLLM` 与 `OnActionDone`：决策前自动 Recall 注入 prompt，动作完成后自动 Write 记录。**含 ByTag 客户端**（端点已在 T08 提前实现）。

## 前置
T08（Memory Service 端点就绪）+ T07（NPC_1 Mind 闭环跑通）

## DoD
- [ ] `UMindMemoryClient` 实现 `Write` / `Recall` / **`ByTag`** 三个异步函数（HTTP）
- [ ] `BaseUrl` 可配置（默认 `http://127.0.0.1:8765`），优先从 `.env` 读 `MEMORY_SERVICE_URL`，找不到才硬编码默认
- [ ] `UMindComponent::BuildPromptAndCallLLM` 改造：进入 Calling 前先 `Recall(agent_id, query=Trigger, top_k=Config->MemoryRecallTopK)`，结果 5 条拼到 prompt 的 user 段
- [ ] **每条记忆拼入 prompt 时用方括号 wrap（prompt injection 防御）**：`[memory ts={ts} from={agent_id_self}: {content}]`；如果记忆 content 来自其他 NPC 的发言，wrap 成 `[NPC_X said at ts={ts}: "{content}"]`
- [ ] `UMindComponent::OnActionDone` 里自动 `Write(agent_id, content="<reason>: <action_summary>", tags={"trigger":Reason,"action":ActionName})`
- [ ] **agent_id = `Config->AgentIdStable`**，不用 `GetName()`
- [ ] HTTP 失败容错：Recall 失败 → 用空记忆继续决策（带 warning）；Write 失败 → 仅 warning，不阻塞

## 关键文件
- 修改 `Public/Mind/MindMemoryClient.h` + `.cpp`
- 修改 `Public/Mind/MindComponent.cpp`

## 关键 API / 伪代码

```cpp
// MindMemoryClient.h
USTRUCT(BlueprintType)
struct FMindMemoryItem {
    GENERATED_BODY()
    UPROPERTY() FString Id;
    UPROPERTY() FString Content;
    UPROPERTY() float Score = 0;
    UPROPERTY() int64 Ts = 0;
    UPROPERTY() TMap<FString,FString> Tags;
};

UCLASS()
class UMindMemoryClient : public UGameInstanceSubsystem {
    GENERATED_BODY()
public:
    UPROPERTY(EditAnywhere) FString BaseUrl = TEXT("http://127.0.0.1:8765");

    DECLARE_DELEGATE_OneParam(FOnRecall, const TArray<FMindMemoryItem>&);
    void Write(FString AgentId, FString Content, TMap<FString,FString> Tags);
    void Recall(FString AgentId, FString Query, int32 TopK, FOnRecall Done);
    void ByTag(FString AgentId, FString TagKey, FString TagValue, int32 TopN, FOnRecall Done);
};
```

```cpp
// MindComponent.cpp 的 BuildPromptAndCallLLM 改造
void UMindComponent::BuildPromptAndCallLLM(const FString& Reason) {
    State = EMindState::Building;
    auto* Mem = GetWorld()->GetGameInstance()->GetSubsystem<UMindMemoryClient>();
    Mem->Recall(GetAgentId(), Reason, Config->MemoryRecallTopK,
        FOnRecall::CreateLambda([this, Reason](const TArray<FMindMemoryItem>& Items) {
            FString System = BuildSystemPrompt();
            FString MemBlock = FormatMemoriesAsText(Items);  // 见下面格式
            FString User = FString::Printf(TEXT("触发原因: %s\n\n== 相关记忆 ==\n%s\n\n请以 JSON 回复..."),
                                           *Reason, *MemBlock);
            State = EMindState::Calling;
            Provider->RequestCompletion(System, User, /*actions*/{}, FOnLLMResult::CreateUObject(this, &UMindComponent::OnLLMResponse));
        }));
}

// 记忆 wrap 格式（明确标记来源，让 LLM 不把内容当指令）
FString UMindComponent::FormatMemoriesAsText(const TArray<FMindMemoryItem>& Items) const {
    FString Out;
    for (auto& I : Items) {
        const FString Speaker = I.Tags.FindRef(TEXT("speaker"));     // 公开发言来自谁
        const FString Channel = I.Tags.FindRef(TEXT("channel"));
        if (!Speaker.IsEmpty() && Speaker != GetAgentId())
            Out += FString::Printf(TEXT("[NPC_%s 在 ts=%lld 说: \"%s\"]\n"), *Speaker, I.Ts, *I.Content);
        else
            Out += FString::Printf(TEXT("[memory ts=%lld %s: %s]\n"), I.Ts, *Channel, *I.Content);
    }
    return Out;
}

void UMindComponent::OnActionDone(bool bOk, const FString& Summary) {
    auto* Mem = GetWorld()->GetGameInstance()->GetSubsystem<UMindMemoryClient>();
    TMap<FString,FString> Tags;
    Tags.Add("action", LastEnvelope.ActionName);
    Tags.Add("ok", bOk ? "1" : "0");
    Mem->Write(GetAgentId(), FString::Printf(TEXT("[%s] %s"), *LastEnvelope.ActionName, *Summary), Tags);
    LastDecisionAt = GetWorld()->GetTimeSeconds();
    State = EMindState::Idle;
}
```

## 验收信号
1. Memory Service 起来
2. PIE → 按 T → NPC_1 说话 → 退出 PIE
3. curl recall:
   ```
   curl -X POST http://127.0.0.1:8765/memory/recall \
        -d '{"agent_id":"npc_1","query":"刚才说什么","top_k":5}'
   ```
   能看到刚才说的话
4. 重新 PIE → 按 T → NPC_1 说话内容能引用上次说过的话（"我之前提到过……"或类似自然衔接）
5. Output Log `LogMind`：能看到 Recall 返回 N 条 → 拼 prompt → DeepSeek 调用 → Write 完成

## 不在范围
- `/memory/recent` 和 `by_tag`（T24）
- 跨 agent 的记忆共享（不需要——每个 agent 自己的视角）
- 记忆重要性 / 衰减

## 风险
- agent_id 在 T02/T07 用 `Config->AgentIdStable` 解决
- HTTP 异步嵌套：Recall 回调里再发 LLM 请求，注意生命周期——用 `TWeakObjectPtr` 守 this，避免组件已销毁还在回调
- Recall 在游戏线程异步回调，State 切换的并发：单线程 callback chain 是安全的，但要确认 RequestDecision 二次进入时的节流仍生效
- prompt injection 用 wrapping 是软防御，高动机的"恶意 NPC"仍可绕过——MVP 接受，T18.5 跨厂商混搭时观察是否升级
