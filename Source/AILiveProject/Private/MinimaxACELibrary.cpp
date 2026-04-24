#include "MinimaxACELibrary.h"

#include "MinimaxSpeechClient.h"

#include "Async/Async.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "UObject/WeakObjectPtrTemplates.h"

#include "ACERuntimeModule.h"
#include "ACEAudioCurveSourceComponent.h"
#include "A2FProvider.h"

DEFINE_LOG_CATEGORY_STATIC(LogMinimaxACE, Log, All);

namespace
{
	UACEAudioCurveSourceComponent* GetOrAddCurveSource(AActor* Character)
	{
		check(IsInGameThread());
		if (!IsValid(Character))
		{
			return nullptr;
		}
		if (UACEAudioCurveSourceComponent* Existing =
				Character->FindComponentByClass<UACEAudioCurveSourceComponent>())
		{
			return Existing;
		}
		UACEAudioCurveSourceComponent* NewComp = NewObject<UACEAudioCurveSourceComponent>(
			Character, UACEAudioCurveSourceComponent::StaticClass(), TEXT("ACEAudioCurve"));
		if (!NewComp)
		{
			return nullptr;
		}
		NewComp->SetupAttachment(Character->GetRootComponent());
		NewComp->RegisterComponent();
		Character->AddInstanceComponent(NewComp);
		UE_LOG(LogMinimaxACE, Log, TEXT("Added UACEAudioCurveSourceComponent to %s"), *Character->GetName());
		return NewComp;
	}
}

void UMinimaxACELibrary::PrewarmA2F(FName A2FProviderName)
{
	FACERuntimeModule::Get().AllocateA2F3DResources(A2FProviderName);
	UE_LOG(LogMinimaxACE, Log, TEXT("A2F resources allocation requested for provider=%s"), *A2FProviderName.ToString());
}

TArray<FName> UMinimaxACELibrary::GetAvailableA2FProviders()
{
	TArray<FName> Names = IA2FProvider::GetAvailableProviderNames();
	FString Joined;
	for (const FName& N : Names)
	{
		if (!Joined.IsEmpty())
		{
			Joined += TEXT(", ");
		}
		Joined += N.ToString();
	}
	UE_LOG(LogMinimaxACE, Log, TEXT("Available A2F providers: [%s]"), *Joined);
	return Names;
}

FString UMinimaxACELibrary::GetMinimaxApiKeyFromProjectEnv()
{
	const FString Path = FPaths::ProjectDir() / TEXT(".env");
	FString Contents;
	if (!FFileHelper::LoadFileToString(Contents, *Path))
	{
		UE_LOG(LogMinimaxACE, Warning, TEXT("Could not read .env at %s"), *Path);
		return FString();
	}
	TArray<FString> Lines;
	Contents.ParseIntoArrayLines(Lines);
	for (FString& Line : Lines)
	{
		Line.TrimStartAndEndInline();
		if (Line.IsEmpty() || Line.StartsWith(TEXT("#")))
		{
			continue;
		}
		FString Key, Value;
		if (Line.Split(TEXT("="), &Key, &Value))
		{
			Key.TrimStartAndEndInline();
			Value.TrimStartAndEndInline();
			Value.TrimQuotesInline();
			if (Key.Equals(TEXT("minimax"), ESearchCase::IgnoreCase))
			{
				return Value;
			}
		}
	}
	UE_LOG(LogMinimaxACE, Warning, TEXT(".env has no `minimax=` line"));
	return FString();
}

void UMinimaxACELibrary::TriggerMinimaxSpeech(
	UObject* /*WorldContextObject*/,
	AActor* Character,
	const FString& Text,
	const FString& ApiKey,
	const FString& VoiceId,
	const FString& Endpoint,
	FName A2FProviderName)
{
	if (!IsValid(Character))
	{
		UE_LOG(LogMinimaxACE, Warning, TEXT("TriggerMinimaxSpeech: invalid Character"));
		return;
	}
	if (Text.IsEmpty() || ApiKey.IsEmpty())
	{
		UE_LOG(LogMinimaxACE, Warning, TEXT("TriggerMinimaxSpeech: empty Text or ApiKey"));
		return;
	}

	UACEAudioCurveSourceComponent* Consumer = GetOrAddCurveSource(Character);
	if (!Consumer)
	{
		UE_LOG(LogMinimaxACE, Warning, TEXT("TriggerMinimaxSpeech: failed to get/add UACEAudioCurveSourceComponent"));
		return;
	}

	TWeakObjectPtr<UACEAudioCurveSourceComponent> WeakConsumer(Consumer);

	MinimaxSpeech::FRequest Req;
	Req.ApiKey    = ApiKey;
	Req.Text      = Text;
	Req.VoiceId   = VoiceId;
	Req.Endpoint  = Endpoint;
	Req.SampleRate = 16000;

	Async(EAsyncExecution::ThreadPool, [Req = MoveTemp(Req), WeakConsumer, A2FProviderName]()
	{
		const MinimaxSpeech::FResult Result = MinimaxSpeech::RequestBlocking(Req);
		if (!Result.bSuccess)
		{
			UE_LOG(LogMinimaxACE, Error, TEXT("MiniMax TTS failed: %s"), *Result.ErrorMessage);
			return;
		}

		UACEAudioCurveSourceComponent* Live = WeakConsumer.Get();
		if (!Live)
		{
			UE_LOG(LogMinimaxACE, Warning, TEXT("Consumer was destroyed before audio could be delivered"));
			return;
		}

		UE_LOG(LogMinimaxACE, Log,
			TEXT("Dispatching %d samples @ %d Hz to ACE (duration %.2fs)"),
			Result.Samples.Num(), Result.SampleRate, Result.DurationSec);

		const bool bOk = FACERuntimeModule::Get().AnimateFromAudioSamples(
			Live,
			TArrayView<const int16>(Result.Samples.GetData(), Result.Samples.Num()),
			/*NumChannels*/ 1,
			Result.SampleRate,
			/*bEndOfSamples*/ true,
			TOptional<FAudio2FaceEmotion>{},
			/*Params*/ nullptr,
			A2FProviderName);

		UE_LOG(LogMinimaxACE, Log, TEXT("AnimateFromAudioSamples returned %s"), bOk ? TEXT("true") : TEXT("false"));
	});
}
