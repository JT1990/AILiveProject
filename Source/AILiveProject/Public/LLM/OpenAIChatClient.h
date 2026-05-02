#pragma once

#include "CoreMinimal.h"

namespace OpenAIChat
{
	struct FRequest
	{
		FString ApiKey;
		FString Endpoint;
		FString Model;
		FString SystemPrompt;
		FString UserPrompt;
		float   Temperature = 0.7f;
		int32   MaxTokens   = 800;
		bool    bResponseFormatJson = true;
		FString ThinkingType;
		float   TimeoutSec  = 60.f;
	};

	struct FResult
	{
		bool    bSuccess = false;
		int32   HttpStatus = 0;
		FString ErrorMessage;
		FString RawContent;
		FString RawResponsePayload;
		FString ReasoningContent;
		FString FinishReason;
		FString ParsedJson;
		int32   PromptTokens = 0;
		int32   CompletionTokens = 0;
		float   LatencyMs = 0.f;
		bool    bRetriedWithoutResponseFormat = false;
	};

	AILIVEPROJECT_API FResult RequestBlocking(const FRequest& Req);

	AILIVEPROJECT_API FString ExtractFirstJsonObject(const FString& Text);
}
