#pragma once

#include "CoreMinimal.h"

namespace AILiveParser
{
	struct FParseRequest
	{
		FString RawText;
		FString AgentId;
	};

	struct FParseResult
	{
		bool    bOk = false;

		FString Scratchpad;
		FString IntendedJson;
		FString BidJson;
		FString NoteText;

		FString ErrorReason;
		FString FailedStage;
		FString ParserRawJson;
	};

	AILIVEPROJECT_API FParseResult ParseFourChannels(const FParseRequest& Req);

	AILIVEPROJECT_API FParseResult PrevalidateRawTaggedSections(const FString& RawText);

	AILIVEPROJECT_API FParseResult ValidateParserOutputJson(const FString& ParserJson);
}
