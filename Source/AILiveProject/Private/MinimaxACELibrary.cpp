#include "MinimaxACELibrary.h"

#include "MinimaxSpeechClient.h"

#include "Async/Async.h"
#include "Components/ChildActorComponent.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "UObject/WeakObjectPtrTemplates.h"

#include "ACERuntimeModule.h"
#include "ACEAudioCurveSourceComponent.h"
#include "A2FProvider.h"

#include "GameFramework/Pawn.h"
#include "Perception/AISense_Hearing.h"

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
	Req.ApiKey = ApiKey;
	Req.Text = Text;
	Req.VoiceId = VoiceId;
	Req.Endpoint = Endpoint;
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

void UMinimaxACELibrary::TriggerMinimaxSpeechWithNoise(
	UObject* /*WorldContextObject*/,
	AActor* AudioTarget,
	AActor* NoiseInstigator,
	const FString& Text,
	const FString& ApiKey,
	const FString& VoiceId,
	const FString& Endpoint,
	FName A2FProviderName)
{
	if (!IsValid(AudioTarget))
	{
		UE_LOG(LogMinimaxACE, Warning, TEXT("TriggerMinimaxSpeechWithNoise: invalid AudioTarget"));
		return;
	}
	if (Text.IsEmpty() || ApiKey.IsEmpty())
	{
		UE_LOG(LogMinimaxACE, Warning, TEXT("TriggerMinimaxSpeechWithNoise: empty Text or ApiKey"));
		return;
	}

	UACEAudioCurveSourceComponent* Consumer = GetOrAddCurveSource(AudioTarget);
	if (!Consumer)
	{
		UE_LOG(LogMinimaxACE, Warning, TEXT("TriggerMinimaxSpeechWithNoise: failed to get/add UACEAudioCurveSourceComponent"));
		return;
	}

	TWeakObjectPtr<UACEAudioCurveSourceComponent> WeakConsumer(Consumer);
	TWeakObjectPtr<AActor> WeakNoiseInstigator(NoiseInstigator);

	MinimaxSpeech::FRequest Req;
	Req.ApiKey = ApiKey;
	Req.Text = Text;
	Req.VoiceId = VoiceId;
	Req.Endpoint = Endpoint;
	Req.SampleRate = 16000;

	Async(EAsyncExecution::ThreadPool,
		[Req = MoveTemp(Req), WeakConsumer, WeakNoiseInstigator, A2FProviderName]()
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
			UE_LOG(LogMinimaxACE, Warning, TEXT("Consumer destroyed before audio delivered"));
			return;
		}

		UE_LOG(LogMinimaxACE, Log,
			TEXT("WithNoise: dispatching %d samples @ %d Hz (duration %.2fs)"),
			Result.Samples.Num(), Result.SampleRate, Result.DurationSec);

		// ReportNoiseEvent 必须在 audio dispatch 之前发：AnimateFromAudioSamples 是
		// 阻塞 streaming dispatch（多个 chunk 串行 send 到 ACE thread），实测可能
		// 持续数秒。如果把 ReportNoiseEvent 放到 dispatch 之后，hearing 命中会推迟到
		// audio 几乎播完时，超出测试 timer 窗口。dispatch 之前发等同"音频开播刹那"。
		// AISense API 不是线程安全的，GetActorLocation 也必须在 GameThread 内取。
		AsyncTask(ENamedThreads::GameThread, [WeakNoiseInstigator]()
		{
			APawn* Pawn = Cast<APawn>(WeakNoiseInstigator.Get());
			if (!Pawn)
			{
				UE_LOG(LogMinimaxACE, Warning,
					TEXT("ReportNoiseEvent skipped: NoiseInstigator null or not a Pawn"));
				return;
			}
			UAISense_Hearing::ReportNoiseEvent(
				Pawn,
				Pawn->GetActorLocation(),
				/*Loudness=*/ 1.f,
				/*Instigator=*/ Pawn,
				/*MaxRange=*/ 0.f,
				/*Tag=*/ NAME_None);
			UE_LOG(LogMinimaxACE, Log,
				TEXT("ReportNoiseEvent: instigator=%s loc=%s"),
				*Pawn->GetName(), *Pawn->GetActorLocation().ToString());
		});

		const bool bOk = FACERuntimeModule::Get().AnimateFromAudioSamples(
			Live,
			TArrayView<const int16>(Result.Samples.GetData(), Result.Samples.Num()),
			/*NumChannels*/ 1,
			Result.SampleRate,
			/*bEndOfSamples*/ true,
			TOptional<FAudio2FaceEmotion>{},
			/*Params*/ nullptr,
			A2FProviderName);

		UE_LOG(LogMinimaxACE, Log, TEXT("WithNoise: AnimateFromAudioSamples returned %s"),
			bOk ? TEXT("true") : TEXT("false"));
	});
}

AActor* UMinimaxACELibrary::GetVisualOverrideAudioTarget(AActor* SpeakerPawn)
{
	if (!IsValid(SpeakerPawn))
	{
		return nullptr;
	}

	TArray<UChildActorComponent*> ChildActorComponents;
	SpeakerPawn->GetComponents<UChildActorComponent>(ChildActorComponents);
	for (UChildActorComponent* Component : ChildActorComponents)
	{
		if (!Component)
		{
			continue;
		}

		const bool bLooksLikeVisualOverride =
			Component->ComponentHasTag(TEXT("VisualOverride")) ||
			Component->GetFName() == TEXT("VisualOverride");
		if (!bLooksLikeVisualOverride)
		{
			continue;
		}

		if (AActor* ChildActor = Component->GetChildActor())
		{
			return ChildActor;
		}
	}

	return nullptr;
}

bool UMinimaxACELibrary::TriggerMinimaxSpeechFromPawnWithNoise(
	UObject* WorldContextObject,
	AActor* SpeakerPawn,
	const FString& Text,
	const FString& ApiKey,
	const FString& VoiceId,
	const FString& Endpoint,
	FName A2FProviderName)
{
	AActor* AudioTarget = GetVisualOverrideAudioTarget(SpeakerPawn);
	if (!IsValid(AudioTarget))
	{
		UE_LOG(LogMinimaxACE, Warning,
			TEXT("TriggerMinimaxSpeechFromPawnWithNoise: no VisualOverride child actor for %s"),
			*GetNameSafe(SpeakerPawn));
		return false;
	}

	TriggerMinimaxSpeechWithNoise(
		WorldContextObject,
		AudioTarget,
		SpeakerPawn,
		Text,
		ApiKey,
		VoiceId,
		Endpoint,
		A2FProviderName);
	return true;
}
