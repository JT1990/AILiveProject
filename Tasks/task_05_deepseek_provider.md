# T05 — DeepSeek Provider + TestPing + tool_call 循环

## 目标
实现 `UMindLLMProvider_DeepSeek::RequestCompletion`，用 messages[] 接口调 DeepSeek chat completions API，支持 OpenAI tool_calls 标准（recall 由 LLM 主动调）。本卡验证"能从 UE 异步调用 LLM 拿到 ActionEnvelope JSON + tool_call 循环工作"。

## 前置
T01（HTTP 模块依赖）+ T02（Provider 基类骨架 + `FMindMessage` struct + `FMindModelCapabilities` table）

## DoD
- [ ] `UMindLLMProvider_DeepSeek::RequestCompletion(const TArray<FMindMessage>& Messages, const TArray<FToolSchema>& Tools, FOnLLMResult Done)` 完整实现
- [ ] 内部 `SerializeMessagesToJson` for-loop 平铺 `Messages` 为 OpenAI 格式 `[{role, content, tool_call_id?}]`
- [ ] **tool_call 循环**：while LLM 返回 `tool_calls`，执行注册的 handler → append `role=tool` 消息 → 再调 chat completion；超 `GetMaxToolIterations()=2` 强制最终生成（追加 `[系统] 已达 recall 上限...` user 消息后 tools=[] 重调）
- [ ] override `GetContextWindowSize()` 查 `FMindModelCapabilities` table（默认 128000）
- [ ] override `RegisterTool(FName Name, FString Schema, FOnToolCall Handler)` 收集 tool 注册表
- [ ] 提供静态测试函数 `TestPing(WorldCtx, Prompt, OnDone)` BlueprintCallable，内部包一条 system + 一条 user 临时 messages 调 RequestCompletion
- [ ] HTTP 走 `FHttpModule::Get().CreateRequest()` + `OnProcessRequestComplete` 异步回调（不阻塞游戏线程）
- [ ] HTTP 回调用 `TWeakObjectPtr<UMindLLMProvider_DeepSeek>` 守 + `BeginDestroy` 时 `CancelRequest` 所有 pending HTTP
- [ ] API key 通过 `UMinimaxACELibrary::GetEnvValueFromProjectEnv(ApiKeyEnvName)` 读取（默认 `"DEEPSEEK_API_KEY"`，由 Provider 子类字段配置；不要硬编码字符串）
- [ ] 失败模式日志清晰：网络错误 / 401 / JSON 解析失败 / tool_call 链超限 各自有 `LogMind` 警告
- [ ] 在编辑器测试关卡里，BP 调 `TestPing("回复 pong")`，Output Log 看到 DeepSeek 返回的回答 + tool_call demo（注册 echo tool 验证循环）

## 关键文件
- 修改 `Public/Mind/MindLLMProvider_DeepSeek.h`
- 修改 `Private/Mind/MindLLMProvider_DeepSeek.cpp`
- 新增 `LogMind` 日志类目（在 `AILiveProject.h` 或新建 `MindLog.h` 里 `DECLARE_LOG_CATEGORY_EXTERN`）

## 关键 API / 伪代码

```cpp
// MindLLMProvider_DeepSeek.h
UCLASS()
class UMindLLMProvider_DeepSeek : public UMindLLMProvider {
    GENERATED_BODY()
public:
    UPROPERTY(EditAnywhere) FString ApiBaseEnvName = TEXT("DEEPSEEK_API_BASE");  // 默认 https://api.deepseek.com/v1
    UPROPERTY(EditAnywhere) FString ApiKeyEnvName = TEXT("DEEPSEEK_API_KEY");
    UPROPERTY(EditAnywhere) FString Model = TEXT("deepseek-chat");
    UPROPERTY(EditAnywhere) float Temperature = 0.7f;

    virtual void RequestCompletion(
        const TArray<FMindMessage>& Messages,
        const TArray<FToolSchema>& Tools,
        FOnLLMResult Done) override;

    virtual int32 GetContextWindowSize() const override;     // 查 Capabilities table
    virtual int32 GetMaxToolIterations() const override { return 2; }
    virtual void RegisterTool(FName Name, FString Schema, FOnToolCall Handler) override;

    UFUNCTION(BlueprintCallable, meta=(WorldContext="WorldCtx"))
    static void TestPing(UObject* WorldCtx, FString Prompt);

private:
    TMap<FName, FOnToolCall> ToolHandlers;
    TArray<TWeakPtr<IHttpRequest>> PendingRequests;

    void SendOneIteration(FString Endpoint, FString Key,
        TArray<FMindMessage> Messages, TArray<FToolSchema> Tools,
        int32 Iter, FOnLLMResult Done);
};
```

```cpp
// .cpp 伪代码 (tool_call 循环)
void UMindLLMProvider_DeepSeek::RequestCompletion(
    const TArray<FMindMessage>& Messages,
    const TArray<FToolSchema>& Tools, FOnLLMResult Done)
{
    FString Base = UMinimaxACELibrary::GetEnvValueFromProjectEnv(ApiBaseEnvName);
    FString Key = UMinimaxACELibrary::GetEnvValueFromProjectEnv(ApiKeyEnvName);
    if (Key.IsEmpty()) { Done.ExecuteIfBound(false, TEXT("no api key")); return; }
    SendOneIteration(Base + TEXT("/chat/completions"), Key, Messages, Tools, /*iter=*/0, Done);
}

void UMindLLMProvider_DeepSeek::SendOneIteration(
    FString Endpoint, FString Key,
    TArray<FMindMessage> Messages, TArray<FToolSchema> Tools,
    int32 Iter, FOnLLMResult Done)
{
    bool bToolsExhausted = (Iter >= GetMaxToolIterations());
    if (bToolsExhausted) {
        Messages.Add({EMindRole::User, TEXT("[系统] 已达 recall 上限, 请基于现有信息决策")});
    }

    TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
    Body->SetStringField("model", Model);
    Body->SetNumberField("temperature", Temperature);
    Body->SetArrayField("messages", SerializeMessagesToJson(Messages));
    if (Tools.Num() > 0 && !bToolsExhausted) {
        Body->SetArrayField("tools", SerializeToolsToJson(Tools));
    }

    auto Req = FHttpModule::Get().CreateRequest();
    Req->SetURL(Endpoint);
    Req->SetVerb(TEXT("POST"));
    Req->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *Key));
    Req->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
    Req->SetContentAsString(SerializeJson(Body));

    TWeakObjectPtr<UMindLLMProvider_DeepSeek> WeakThis(this);
    Req->OnProcessRequestComplete().BindLambda(
        [WeakThis, Done, Endpoint, Key, Messages, Tools, Iter]
        (FHttpRequestPtr R, FHttpResponsePtr P, bool bOk) mutable {
            if (!WeakThis.IsValid()) return;            // BeginDestroy 已 cancel
            if (!bOk || !P.IsValid()) { Done.ExecuteIfBound(false, TEXT("net error")); return; }

            auto Choice = ParseChoice(P->GetContentAsString());
            if (Choice.tool_calls.Num() > 0) {
                Messages.Add({EMindRole::Assistant, TEXT(""), Choice.tool_calls});
                for (auto& Call : Choice.tool_calls) {
                    FString Result = WeakThis->ToolHandlers[FName(*Call.name)].Execute(Call.arguments);
                    Messages.Add({EMindRole::Tool, Result, /*tool_call_id*/Call.id});
                }
                WeakThis->SendOneIteration(Endpoint, Key, Messages, Tools, Iter+1, Done);
                return;
            }
            Done.ExecuteIfBound(true, Choice.content);   // 最终 envelope
        });
    Req->ProcessRequest();
    PendingRequests.Add(Req);
}
```

## 验收信号
- 编辑器创建一个一次性测试 BP（含 BeginPlay），调 `TestPing("回复一个 'pong'")` 或 `TestPing("将 1+1 的结果用一个数字回答")`
  - **不要用"介绍你自己 / 你是谁"** 类身份提问，会触发 PRD 失败模式 3（元意识："我只是语言模型..."），破坏 T07.5 AI 身份契约
- PIE 后 Output Log（按 `LogMind` 过滤）看到：
  - `[LogMind] DeepSeek request: <messages count, total token est>`
  - `[LogMind] DeepSeek response (HTTP 200): <返回文字>`
- 文字符合 prompt 期待（如 "pong" 或 "2"）
- **tool_call demo**：注册一个 echo tool（输入字符串原样返回），prompt 让 LLM 调用它，验证 tool_call 循环工作 + `max_iterations=2` 上限生效（造作死循环 scenario 验证）
- `BeginDestroy` 时 pending HTTP 被 cancel（手工 destroy Provider 验证无 use-after-free）

### 额外：ratelimit 实测

完成基本 TestPing 后跑一次 batched ping：
- 写一个 BP 节点 `BatchPing(N=10, IntervalMs=500)` 连续 10 次调用，记录每次的请求/响应时间戳
- 拿到结果：实测 QPS 上限、平均延迟、P95 延迟、是否触发 429
- **结果记到** `Tasks/T05_RATELIMIT_BASELINE.md`（手填）：
  ```
  - 账号 tier: <free / pay-go / enterprise>
  - 实测 QPS 上限: <X>/秒
  - 平均延迟: <Y>ms
  - P95 延迟: <Z>ms
  - 触发 429: <yes/no>，触发条件 <if any>
  - 实测 context_window: <128000 或更大>
  - tool_call 支持: <yes/no>
  ```
- 这些数字将影响 T21（Negotiate 阶段 GM 唤醒间隔 + LLM budget 分两池）和 T26（M5 多轮验收）的实际可行性

## 不在范围
- 流式响应（不需要——一次性拿全文）
- GLM / Anthropic Provider（T18.5 接 GLM 时单独 serializer + Capabilities）
- recall tool 的实际 Memory Service 端集成（T09 注册 tool 时把 handler 指向 `MemoryClient.RecallSync`）

## 风险
- DeepSeek 中国境内 endpoint 是 `api.deepseek.com`，账号区域 / 网络可达性需确认
- HTTP 模块默认 timeout 30s，DeepSeek 慢响应时可能切；如果遇到要在 `Engine.ini` 调 `[HTTP]` `HttpReceiveTimeout`
- TLS 证书问题：UE 5.7 默认 OK
- tool_call 循环嵌套时 `Messages` 在 lambda 捕获里被 mutate，注意按值传不要按引用（避免 race）
