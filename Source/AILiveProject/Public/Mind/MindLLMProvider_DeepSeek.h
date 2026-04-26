#pragma once

#include "CoreMinimal.h"
#include "Mind/MindLLMProvider.h"
#include "MindLLMProvider_DeepSeek.generated.h"

DECLARE_DYNAMIC_DELEGATE_TwoParams(FDeepSeekTestPingDone, bool, bOk, FString, Text);

/**
 * DeepSeek LLM Provider。T05 实现 RequestCompletion（OpenAI 兼容 /chat/completions）+
 * BP 可调的 TestPing / BatchPing 两个静态测试函数。
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

	virtual void RequestCompletion(
		const FString& SystemPrompt,
		const FString& UserPrompt,
		const TArray<TSubclassOf<class UMindAction>>& AvailableActions,
		FOnLLMResult Done) override;

	/** 一次性 BP 测试入口。内部 NewObject + AddToRoot 临时持有，回调里 RemoveFromRoot + MarkAsGarbage。 */
	UFUNCTION(BlueprintCallable, Category="AI Live|Mind|Test", meta=(WorldContext="WorldCtx"))
	static void TestPing(UObject* WorldCtx, const FString& Prompt, FDeepSeekTestPingDone OnDone);

	/** Ratelimit 实测：串行触发 N 次（间隔 IntervalMs），完成后日志输出 avg/p95/429 计数。 */
	UFUNCTION(BlueprintCallable, Category="AI Live|Mind|Test", meta=(WorldContext="WorldCtx"))
	static void BatchPing(UObject* WorldCtx, const FString& Prompt, int32 N = 10, int32 IntervalMs = 500);
};
