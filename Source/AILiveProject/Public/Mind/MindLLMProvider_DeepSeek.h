#pragma once

#include "CoreMinimal.h"
#include "Mind/MindLLMProvider.h"
#include "MindLLMProvider_DeepSeek.generated.h"

/**
 * DeepSeek LLM Provider。T05 实现 RequestCompletion；T02 仅落骨架，让
 * UMindAgentConfig.ProviderClass 下拉能选到（验证 EditInlineNew + Abstract 子类显示）。
 *
 * non-Abstract 必须显式不带 Abstract 关键字，才能在 ProviderClass 下拉中显示。
 */
UCLASS()
class AILIVEPROJECT_API UMindLLMProvider_DeepSeek : public UMindLLMProvider
{
	GENERATED_BODY()
public:
	UPROPERTY(EditAnywhere, Category="AI Live|Mind")
	FString ApiBaseEnvName = TEXT("DEEPSEEK_API_BASE");

	UPROPERTY(EditAnywhere, Category="AI Live|Mind")
	FString ApiKeyEnvName = TEXT("DEEPSEEK_API_KEY");

	UPROPERTY(EditAnywhere, Category="AI Live|Mind")
	FString Model = TEXT("deepseek-chat");

	UPROPERTY(EditAnywhere, Category="AI Live|Mind")
	float Temperature = 0.7f;

	// T05 实施真 HTTP 调用；T03 占位 override 防止 NewObject 后调 RequestCompletion 触发 LowLevelFatalError
	virtual void RequestCompletion(
		const FString& SystemPrompt,
		const FString& UserPrompt,
		const TArray<TSubclassOf<class UMindAction>>& AvailableActions,
		FOnLLMResult Done) override;
};
