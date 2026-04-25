#include "MinimaxACELibrary.h"

#include "MinimaxSpeechClient.h"

#include "Async/Async.h"
#include "GameFramework/Actor.h"
#include "HAL/CriticalSection.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "UObject/WeakObjectPtrTemplates.h"

#include "ACERuntimeModule.h"
#include "ACEAudioCurveSourceComponent.h"
#include "A2FProvider.h"

DEFINE_LOG_CATEGORY_STATIC(LogMinimaxACE, Log, All);

namespace
{
	TMap<FString, FString> GEnvCache;
	bool bGEnvCacheLoaded = false;
	FCriticalSection GEnvCacheMutex;

	void LoadEnvCacheOnce()
	{
		FScopeLock Lock(&GEnvCacheMutex);
		if (bGEnvCacheLoaded)
		{
			return;
		}
		const FString EnvPath = FPaths::ProjectDir() / TEXT(".env");
		FString Contents;
		if (!FFileHelper::LoadFileToString(Contents, *EnvPath))
		{
			UE_LOG(LogMinimaxACE, Warning, TEXT("Could not read .env at %s"), *EnvPath);
			bGEnvCacheLoaded = true;
			return;
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
				GEnvCache.Add(Key.ToLower(), Value);
			}
		}
		bGEnvCacheLoaded = true;
		UE_LOG(LogMinimaxACE, Log, TEXT(".env cache loaded with %d entries"), GEnvCache.Num());
	}

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

FString UMinimaxACELibrary::GetEnvValueFromProjectEnv(const FString& KeyName)
{
	LoadEnvCacheOnce();
	return GEnvCache.FindRef(KeyName.ToLower());
}

FString UMinimaxACELibrary::GetMinimaxApiKeyFromProjectEnv()
{
	return GetEnvValueFromProjectEnv(TEXT("minimax"));
}

bool UMinimaxACELibrary::VerifyEnvSafety()
{
	const FString GitignorePath = FPaths::ProjectDir() / TEXT(".gitignore");
	FString Contents;
	if (!FFileHelper::LoadFileToString(Contents, *GitignorePath))
	{
		UE_LOG(LogMinimaxACE, Error, TEXT("SECURITY: cannot read .gitignore at %s — credentials may leak"), *GitignorePath);
		return false;
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
		FString Pattern = Line.StartsWith(TEXT("!")) ? Line.RightChop(1) : Line;
		if (Pattern.Equals(TEXT(".env"))
			|| Pattern.Equals(TEXT("/.env"))
			|| Pattern.Equals(TEXT("**/.env")))
		{
			return true;
		}
	}
	UE_LOG(LogMinimaxACE, Error, TEXT("SECURITY: .env not in .gitignore — risk of leaking API keys"));
	return false;
}

FString UMinimaxACELibrary::MaskKey(const FString& Key)
{
	if (Key.Len() < 12)
	{
		return TEXT("****");
	}
	return Key.Left(4) + TEXT("...") + Key.Right(4);
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
