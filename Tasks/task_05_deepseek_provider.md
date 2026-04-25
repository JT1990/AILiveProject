# T05 — DeepSeek Provider + TestPing

## 目标
实现 `UMindLLMProvider_DeepSeek::RequestCompletion`，用现有 HTTP 模块调 DeepSeek chat completions API。本卡只验证"能从 UE 异步调用 LLM 并拿到文本"，不接 ActionEnvelope。

## 前置
T01（HTTP 模块依赖）+ T02（Provider 基类骨架）

## DoD
- [ ] `UMindLLMProvider_DeepSeek::RequestCompletion` 完整实现
- [ ] 提供静态测试函数 `UMindLLMProvider_DeepSeek::TestPing(WorldCtx, Prompt, OnDone)`，BlueprintCallable
- [ ] HTTP 调用走 `FHttpModule::Get().CreateRequest()` + `OnProcessRequestComplete` 异步回调（不阻塞游戏线程）
- [ ] API key 通过 `UMinimaxACELibrary::GetEnvValueFromProjectEnv("deepseek")` 读取
- [ ] 失败模式日志清晰：网络错误 / 401 / JSON 解析失败 各自有 `LogMind` 警告
- [ ] 在编辑器测试关卡里，BP 调 `TestPing("你好")`，Output Log 看到 DeepSeek 返回的中文回答

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
    // 用 .env 里的 DEEPSEEK_API_BASE，拼 /chat/completions
    UPROPERTY(EditAnywhere) FString ApiBaseEnvName = TEXT("DEEPSEEK_API_BASE");  // 默认 https://api.deepseek.com/v1
    UPROPERTY(EditAnywhere) FString ApiKeyEnvName = TEXT("DEEPSEEK_API_KEY");
    UPROPERTY(EditAnywhere) FString Model = TEXT("deepseek-chat");
    UPROPERTY(EditAnywhere) float Temperature = 0.7f;

    virtual void RequestCompletion(
        const FString& SystemPrompt,
        const FString& UserPrompt,
        const TArray<TSubclassOf<UMindAction>>& AvailableActions,
        FOnLLMResult Done) override;

    UFUNCTION(BlueprintCallable, meta=(WorldContext="WorldCtx"))
    static void TestPing(UObject* WorldCtx, FString Prompt);
};
```

```cpp
// .cpp 伪代码
void UMindLLMProvider_DeepSeek::RequestCompletion(...) {
    const FString Base = UMinimaxACELibrary::GetEnvValueFromProjectEnv(ApiBaseEnvName);
    const FString Key = UMinimaxACELibrary::GetEnvValueFromProjectEnv(ApiKeyEnvName);
    if (Key.IsEmpty()) { Done.ExecuteIfBound(false, TEXT("no api key")); return; }
    const FString Endpoint = Base + TEXT("/chat/completions");  // OpenAI-compatible 路径

    TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
    Body->SetStringField("model", Model);
    Body->SetNumberField("temperature", Temperature);
    TArray<TSharedPtr<FJsonValue>> Messages;
    // role:system + role:user 两条消息
    Body->SetArrayField("messages", Messages);
    // 本卡不接 tool calling，避免引入 schema 复杂度

    FString Json;
    auto Writer = TJsonWriterFactory<>::Create(&Json);
    FJsonSerializer::Serialize(Body, Writer);

    auto Req = FHttpModule::Get().CreateRequest();
    Req->SetURL(Endpoint);
    Req->SetVerb(TEXT("POST"));
    Req->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *Key));
    Req->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
    Req->SetContentAsString(Json);
    Req->OnProcessRequestComplete().BindLambda(
        [Done](FHttpRequestPtr R, FHttpResponsePtr P, bool bOk) {
            if (!bOk || !P.IsValid()) { Done.ExecuteIfBound(false, TEXT("net error")); return; }
            // 解析 choices[0].message.content
            // 失败时日志 + Done(false)
            Done.ExecuteIfBound(true, ParsedContent);
        });
    Req->ProcessRequest();
}
```

## 验收信号
- 编辑器创建一个一次性测试 BP（含 BeginPlay），调 `TestPing("用一句话介绍你自己")`
- PIE 后 Output Log（按 `LogMind` 过滤）看到：
  - `[LogMind] DeepSeek request: <prompt 长度>`
  - `[LogMind] DeepSeek response (HTTP 200): <返回文字>`
- 文字是符合 DeepSeek 风格的中文自我介绍

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
  ```
- 这些数字将影响 T21（Negotiate 阶段 GM 唤醒间隔）和 T26（M5 多轮验收）的实际可行性

## 不在范围
- Function calling / tool schema（T06 起）
- 多轮对话（不需要——每次决策都是新 context，靠记忆系统提供历史）
- 流式响应（不需要——一次性拿全文）

## 风险
- DeepSeek 中国境内 endpoint 是 `api.deepseek.com`，账号区域 / 网络可达性需确认
- HTTP 模块默认 timeout 30s，DeepSeek 慢响应时可能切；如果遇到要在 `Engine.ini` 调 `[HTTP]` `HttpReceiveTimeout`
- TLS 证书问题：UE 5.7 默认 OK
