# T18.5 — 第二家 LLM 厂商接入（GLM）

## 目标

接入 GLM 作为第二家 LLM 厂商，证明 `UMindLLMProvider` 抽象真的能扛多家 + 兼容 messages[] / tool_call / cache 三大特性。M4 8 NPC 时混搭 DeepSeek + GLM 观察策略差异（PRD 多厂商博弈核心目的）。

## 前置

T23 M4A（先用单 DeepSeek 跑通 8 NPC 少数决单轮）+ T05（DeepSeek Provider messages[]+tool_call 完整）+ T09（`FMindModelCapabilities` table 已就位）

## DoD

- [ ] **接入 GLM**（候选 endpoint/model，**实施日重新核实 provider dashboard 当前可用值**）：
  - 候选 endpoint: `https://open.bigmodel.cn/api/paas/v4` (OpenAI-compatible)
  - 候选 model: `glm-4.6`（PRD 候选池写法；实施日确认是否仍可用）
  - `.env` 新增 `GLM_API_BASE=...` 和 `GLM_API_KEY=...`
- [ ] 创建 `UMindLLMProvider_GLM`：
  - **HTTP helper 复用**：把 DeepSeek Provider 的 `SendOneIteration` HTTP / 异步回调 / TWeakObjectPtr 守 / BeginDestroy cancel 提到基类或 helper（如 `UMindOpenAICompatHelpers::Send`）
  - **serializer + Capabilities + tool_call serializer 单独实现**：每家厂商对中途 system role / tool_calls 字段名 / cache usage 字段格式可能不同，每个 Provider 自己定 `SerializeMessagesToJson` / `SerializeToolsToJson` / `ParseUsageFromResponse`
- [ ] `FMindModelCapabilities` table 加 GLM 4.6 entry：
  - `context_window = 200000`
  - `max_output = 8192`
  - `supports_tools = true`
  - `cache_usage_field = "prompt_tokens_details.cached_tokens"`（与 DeepSeek 的 `prompt_cache_hit_tokens` 不同，要适配）
- [ ] **兼容性测试**（**先跑这步**，再分配 NPC）：
  - 测 messages[] 含中途 system role（Layer 1 anchored summary）—— GLM 是否当 user 处理 / 抛错 / 静默接受？如不兼容，调 SerializeMessagesToJson 把所有 system 合并到 messages[0]
  - 测 tool_call 循环：注册 echo tool，verify GLM 也能正确 tool_calls 字段返回
  - 测 prompt cache：连续调 5 次相同 messages[0]，verify 第 2 次起 cache 命中
- [ ] 在 M4 关卡 `Level_MinorityRule` 给 8 NPC 分配 LLM Provider：
  - 4 NPC 用 DeepSeek
  - 4 NPC 用 GLM
  - **同 persona 跨厂商对比**：让 NPC_1 (DeepSeek + 保守 persona) 与 NPC_5 (GLM + 同保守 persona) 在同一局观察行为差异
- [ ] 修改 `DA_AgentConfig_NPC*` 的 ProviderClass 字段
- [ ] 跑 1-2 局少数决并在 DevLog 记录跨厂商行为差异（决策风格 / 响应延迟 / JSON 严格度 / cache 命中率 / tool_call 行为）

## 关键文件

- 新建 `Public/Mind/MindLLMProvider_GLM.h` + `.cpp`
- 重构 `Public/Mind/MindOpenAICompatHelpers.h`（提取共用 HTTP 流程）
- 修改 `Public/Mind/MindModelCapabilities.h` 加 GLM entry
- 修改 `.env`
- 修改 4 个 `DA_AgentConfig_NPC*` 的 ProviderClass

## 关键 API / 伪代码

```cpp
// MindLLMProvider_GLM.h（与 DeepSeek 同构但 serializer / capabilities 独立）
UCLASS()
class UMindLLMProvider_GLM : public UMindLLMProvider {
    GENERATED_BODY()
public:
    UPROPERTY(EditAnywhere) FString ApiBaseEnvName = TEXT("GLM_API_BASE");
    UPROPERTY(EditAnywhere) FString ApiKeyEnvName = TEXT("GLM_API_KEY");
    UPROPERTY(EditAnywhere) FString Model = TEXT("glm-4.6");
    UPROPERTY(EditAnywhere) float Temperature = 0.7f;

    virtual void RequestCompletion(
        const TArray<FMindMessage>& Messages,
        const TArray<FToolSchema>& Tools,
        FOnLLMResult Done) override;

    virtual int32 GetContextWindowSize() const override;   // 查 Capabilities → 200000
    virtual int32 GetMaxToolIterations() const override { return 2; }
    virtual void RegisterTool(FName Name, FString Schema, FOnToolCall Handler) override;

protected:
    // 单独实现 (DeepSeek / GLM 字段名可能不同)
    virtual TArray<TSharedPtr<FJsonValue>> SerializeMessagesToJson(const TArray<FMindMessage>& Messages) const;
    virtual TArray<TSharedPtr<FJsonValue>> SerializeToolsToJson(const TArray<FToolSchema>& Tools) const;
    virtual int32 ParseUsageCachedTokens(const TSharedPtr<FJsonObject>& Response) const;
};
```

## 验收信号

- 兼容性测试 3 项通过：messages 中途 system role 行为已知 / tool_call 循环工作 / cache 命中率 ≥ 70%
- M4 单轮少数决跑通，8 NPC 一半 DeepSeek 一半 GLM
- DevLog 记录至少 5 条可观察的跨厂商差异（如：GLM JSON 输出更严格 / 响应延迟更长 / 倾向更激进 / cache 命中字段不同 / tool_call 解析差异）
- HTTP 调用稳定，无单家厂商 ratelimit 把全局拖垮（reasoner pool MaxConcurrent=4 兜底）

## 不在范围

- 接入第 3-16 家（M6+）
- Anthropic Claude（messages[] 不在数组内的 system 字段；映射工作量大，留 M6+）
- 跨厂商成本对比 / billing
- 厂商失败自动降级（fallback chain）

## 风险

- GLM 中途 system role 的实际行为未在官方文档显式承诺（示例都是 leading system + user）；如果不兼容，serializer 把所有 system 合并到 messages[0]——但这会破坏 Layer 1 anchored summary 的字符级稳定（每次 anchor 变都污染 messages[0] cache），需要权衡
- 不同厂商可能在中文输出上风格差异大，影响 prompt 可移植性
- 同 persona 跨厂商不一定真的能产生有趣差异——接受"实验性观察"，不强求结论
- tool_call 在 GLM 上的字段名可能与 OpenAI 标准略有差异（如 `tool_calls` vs `function_call`），ParseChoice 要适配
