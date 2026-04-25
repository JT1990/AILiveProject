#include "Mind/MindLLMProvider_DeepSeek.h"
#include "Mind/MindLog.h"

void UMindLLMProvider_DeepSeek::RequestCompletion(
	const FString& /*SystemPrompt*/,
	const FString& /*UserPrompt*/,
	const TArray<TSubclassOf<UMindAction>>& /*AvailableActions*/,
	FOnLLMResult Done)
{
	UE_LOG(LogMind, Warning, TEXT("UMindLLMProvider_DeepSeek::RequestCompletion not implemented (T05)"));
	Done.ExecuteIfBound(false, TEXT("{\"error\":\"deepseek provider not implemented (T05)\"}"));
}
