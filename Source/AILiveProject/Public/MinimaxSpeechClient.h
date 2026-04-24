#pragma once

#include "CoreMinimal.h"

namespace MinimaxSpeech
{
	struct FRequest
	{
		FString ApiKey;
		FString Text;
		FString VoiceId   = TEXT("male-qn-qingse");
		FString Model     = TEXT("speech-2.8-turbo");
		FString Endpoint  = TEXT("https://api.minimaxi.com/v1/t2a_v2");
		FString LanguageBoost = TEXT("Chinese");
		int32   SampleRate = 16000;
		float   Speed      = 1.f;
		float   Volume     = 1.f;
		int32   Pitch      = 0;
	};

	struct FResult
	{
		bool          bSuccess = false;
		FString       ErrorMessage;
		FString       TraceId;
		TArray<int16> Samples;
		int32         SampleRate  = 0;
		int32         NumChannels = 1;
		float         DurationSec = 0.f;
	};

	AILIVEPROJECT_API FResult RequestBlocking(const FRequest& Req);
}
