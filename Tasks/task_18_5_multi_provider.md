# T18.5 — 第二家 LLM 厂商接入

## 目标

接入第二家 LLM 厂商，证明 `UMindLLMProvider` 抽象真的能扛多家。M4 8 NPC 时混搭 DeepSeek + 第二家观察策略差异（PRD 多厂商博弈核心目的）。

## 前置

T17（M3 完成，骗子酒馆稳定运行）

## DoD

- [ ] **接入 GLM**（用户已确认选 GLM）：
  - endpoint: `https://open.bigmodel.cn/api/paas/v4` (OpenAI-compatible)
  - model: `glm-5.1`
  - `.env` 新增 `GLM_API_BASE=https://open.bigmodel.cn/api/paas/v4` 和 `GLM_API_KEY=...`
- [ ] 创建 `UMindLLMProvider_GLM`，95% 代码复用 DeepSeek Provider（仅 endpoint / key 名 / model 不同）
- [ ] 在 M4 关卡 `Level_MinorityRule` 给 8 NPC 分配 LLM Provider：
  - 4 NPC 用 DeepSeek
  - 4 NPC 用 GLM
  - **同 persona 跨厂商对比**：让 NPC_1 (DeepSeek + 保守 persona) 与 NPC_5 (GLM + 同保守 persona) 在同一局观察行为差异
- [ ] 修改 `DA_AgentConfig_NPC*` 的 ProviderClass 字段
- [ ] 跑 1-2 局少数决并在 DevLog 记录跨厂商行为差异（决策风格 / 响应延迟 / JSON 严格度）

## 关键文件

- 新建 `Public/Mind/MindLLMProvider_<Vendor>.h` + `.cpp`
- 修改 `.env`
- 修改 4 个 `DA_AgentConfig_NPC*` 的 ProviderClass

## 关键 API / 伪代码

```cpp
// GLM 是 OpenAI-compatible，与 DeepSeek 几乎同构
UCLASS()
class UMindLLMProvider_GLM : public UMindLLMProvider {
    GENERATED_BODY()
public:
    UPROPERTY(EditAnywhere) FString ApiBaseEnvName = TEXT("GLM_API_BASE");
    UPROPERTY(EditAnywhere) FString ApiKeyEnvName = TEXT("GLM_API_KEY");
    UPROPERTY(EditAnywhere) FString Model = TEXT("glm-5.1");
    UPROPERTY(EditAnywhere) float Temperature = 0.7f;
    // RequestCompletion: 提取 DeepSeek Provider 的 OpenAI-compat HTTP 流程到基类工具函数
    // UMindLLMProvider::RequestOpenAICompat(base_url, key, model, temp, sys, user, done) 复用
};
```

**重构建议**：T18.5 实施时把 DeepSeek Provider 的 HTTP 调用代码提到基类（或单独的 helper），DeepSeek 和 GLM 两个子类只填配置，不复制 HTTP 逻辑。

## 验收信号

- M4 单轮少数决跑通，8 NPC 一半 DeepSeek 一半第二家
- DevLog 记录至少 3 条可观察的跨厂商差异（如：第二家 JSON 输出更严格 / 响应延迟更长 / 倾向更激进）
- HTTP 调用稳定，无单家厂商 ratelimit 把全局拖垮

## 不在范围

- 接入第 3-16 家（M6+）
- 跨厂商成本对比 / billing
- 厂商失败自动降级（fallback chain）

## 风险

- 第二家的 API 风格不完全 OpenAI-compatible（如 Anthropic messages 格式不同），可能要写更多分支代码 — 选 GLM/Qwen 风险最小
- 不同厂商可能在中文输出上风格差异大，影响 prompt 可移植性
- 同 persona 跨厂商不一定真的能产生有趣差异 — 接受"实验性观察"，不强求结论
