#pragma once

#include "CoreMinimal.h"
#include "Mind/MindLLMProvider.h"
#include "Mind/MindMockResponseTable.h"
#include "MindLLMProvider_Mock.generated.h"

/**
 * Mock LLM Provider（T05.5）。
 *
 * 开发期默认 Provider，避免每次迭代都等真 LLM 的 2-4s 网络延迟。
 * 行为：在 UserPrompt 里查 Table 中的 MatchPattern（substring），多个命中按 Weight 加权随机选；
 * 无命中走 FallbackResponseJson。延迟用 FTSTicker 实现，与 UWorld 解耦（commandlet/PIE 通用）。
 *
 * 切换路径：UMindAgentConfig.ProviderClass = UMindLLMProvider_Mock
 *           + UMindAgentConfig.MockResponseTable = DA_Mock_*
 * Initialize 时由 MindComponent（T07）拷贝 Config.MockResponseTable -> Provider->Table。
 */
UCLASS()
class AILIVEPROJECT_API UMindLLMProvider_Mock : public UMindLLMProvider
{
	GENERATED_BODY()
public:
	/** 预设响应表。MindComponent::Initialize 时从 UMindAgentConfig.MockResponseTable 拷过来（T07 实施）。 */
	UPROPERTY(EditAnywhere, Category="AI Live|Mind")
	TObjectPtr<UMindMockResponseTable> Table;

	/** 模拟 LLM 延迟（秒）。开发期默认 100ms；走 FTSTicker 不依赖 World。 */
	UPROPERTY(EditAnywhere, Category="AI Live|Mind", meta=(ClampMin="0.0", ClampMax="5.0"))
	float MockDelaySeconds = 0.1f;

	virtual void RequestCompletion(
		const FString& SystemPrompt,
		const FString& UserPrompt,
		const TArray<TSubclassOf<class UMindAction>>& AvailableActions,
		FOnLLMResult Done) override;
};
