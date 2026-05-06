// =============================================================================
// 中文教学：MinimaxACELibrary.cpp —— TTS + A2F + AI 感知 三件事的总编排实现
//
// 这是 ~470 行的「整合层」。所有 TTS 入口都在这里实现，分工：
//
//   - PrewarmA2F：首次发声前预编译 TRT，避免冷启动延迟
//   - GetAvailableA2FProviders：列出当前可用的 A2F provider（取决于安装的插件）
//   - GetMinimaxApiKeyFromProjectEnv：从 .env 读 minimax 密钥（开发期专用）
//   - TriggerMinimaxSpeech：基础版本，仅 TTS + A2F，没听觉感知上报
//   - TriggerMinimaxSpeechWithNoise：基础版 + AISense_Hearing 噪声事件（NPC 能感知）
//   - TriggerMinimaxSpeechFromPawnWithNoise：BP 友好包装（解析 VisualOverride child actor）
//   - TriggerMinimaxSpeechFromPawnNative：C++ only 版，带完成 delegate
//   - GetVisualOverrideAudioTarget：BP 工具，从 Pawn 找到挂 ACE 组件的可见 actor
//
// 关键 UE / C++ 概念：
//
//   1) check(IsInGameThread())
//      断言只在游戏线程调。失败则 PIE crash。GetOrAddCurveSource 必须在
//      GameThread 因为它要 NewObject + RegisterComponent，那些都不是线程安全的。
//
//   2) FindComponentByClass<T>() / NewObject<T>() / SetupAttachment / RegisterComponent
//      运行时给 actor 挂组件的标准三步：
//        a) 先查有没有现成的 → FindComponentByClass
//        b) 没有就 NewObject 创建
//        c) SetupAttachment + RegisterComponent + AddInstanceComponent
//      漏 RegisterComponent 组件不会 tick / 接事件。
//
//   3) TWeakObjectPtr 跨线程守 actor
//      LLM 调用动辄几秒，actor 可能在期间被销毁（玩家退出 PIE / 关卡切换）。
//      把 weak ptr 闭包捕获到 lambda；切回 GameThread 后 .Get() / .IsValid() 检查。
//
//   4) AsyncTask(ENamedThreads::GameThread, [...](){ ... })
//      把 lambda 调度到游戏线程下一帧执行。所有 ACE/A2F/AISense 调用都必须
//      在游戏线程。
//
//   5) FACERuntimeModule::AnimateFromAudioSamples
//      ACE 模块的核心入口：传 PCM 数据 + provider name → 它内部把数据流送给
//      A2F 引擎，由 UACEAudioCurveSourceComponent 输出 curve 给 face AnimBP。
//      调用是阻塞-streaming（持续数秒），所以噪声事件要在这之前发，避免命中
//      推迟到音频几乎播完时。
//
//   6) UAISense_Hearing::ReportNoiseEvent
//      给 AI Perception 系统报一个噪声刺激事件。线程不安全，必须 GameThread。
//      所有听见这个声音的 AIController 会收到 OnPerceptionUpdated 回调。
// =============================================================================

#include "MinimaxACELibrary.h"

#include "MinimaxSpeechClient.h"

#include "Async/Async.h"
#include "Components/ChildActorComponent.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "UObject/WeakObjectPtrTemplates.h"

#include "ACERuntimeModule.h"                  // FACERuntimeModule
#include "ACEAudioCurveSourceComponent.h"      // UACEAudioCurveSourceComponent
#include "A2FProvider.h"                       // IA2FProvider 列出可用 provider

#include "GameFramework/Pawn.h"
#include "Perception/AISense_Hearing.h"        // UAISense_Hearing::ReportNoiseEvent

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

bool UMinimaxACELibrary::TriggerMinimaxSpeechFromPawnNative(
	UObject* /*WorldContextObject*/,
	AActor* SpeakerPawn,
	const FString& Text,
	const FString& ApiKey,
	const FString& VoiceId,
	const FString& Endpoint,
	FName A2FProviderName,
	FOnMinimaxSpeechFinishedNative OnFinished)
{
	auto FireFinished = [OnFinished](bool bSuccess)
	{
		if (!OnFinished.IsBound())
		{
			return;
		}
		AsyncTask(ENamedThreads::GameThread, [OnFinished, bSuccess]()
		{
			OnFinished.ExecuteIfBound(bSuccess);
		});
	};

	if (!IsValid(SpeakerPawn))
	{
		UE_LOG(LogMinimaxACE, Warning, TEXT("TriggerMinimaxSpeechFromPawnNative: invalid pawn"));
		FireFinished(false);
		return false;
	}
	AActor* AudioTarget = GetVisualOverrideAudioTarget(SpeakerPawn);
	if (!IsValid(AudioTarget))
	{
		UE_LOG(LogMinimaxACE, Warning,
			TEXT("TriggerMinimaxSpeechFromPawnNative: no VisualOverride child actor for %s"),
			*GetNameSafe(SpeakerPawn));
		FireFinished(false);
		return false;
	}
	if (Text.IsEmpty() || ApiKey.IsEmpty())
	{
		UE_LOG(LogMinimaxACE, Warning, TEXT("TriggerMinimaxSpeechFromPawnNative: empty Text or ApiKey"));
		FireFinished(false);
		return false;
	}

	UACEAudioCurveSourceComponent* Consumer = GetOrAddCurveSource(AudioTarget);
	if (!Consumer)
	{
		UE_LOG(LogMinimaxACE, Warning,
			TEXT("TriggerMinimaxSpeechFromPawnNative: failed to get/add UACEAudioCurveSourceComponent"));
		FireFinished(false);
		return false;
	}

	TWeakObjectPtr<UACEAudioCurveSourceComponent> WeakConsumer(Consumer);
	TWeakObjectPtr<AActor> WeakNoiseInstigator(SpeakerPawn);

	MinimaxSpeech::FRequest Req;
	Req.ApiKey = ApiKey;
	Req.Text = Text;
	Req.VoiceId = VoiceId;
	Req.Endpoint = Endpoint;
	Req.SampleRate = 16000;

	Async(EAsyncExecution::ThreadPool,
		[Req = MoveTemp(Req), WeakConsumer, WeakNoiseInstigator, A2FProviderName, OnFinished]()
	{
		auto FireFinishedWorker = [OnFinished](bool bSuccess)
		{
			if (!OnFinished.IsBound())
			{
				return;
			}
			AsyncTask(ENamedThreads::GameThread, [OnFinished, bSuccess]()
			{
				OnFinished.ExecuteIfBound(bSuccess);
			});
		};

		const MinimaxSpeech::FResult Result = MinimaxSpeech::RequestBlocking(Req);
		if (!Result.bSuccess)
		{
			UE_LOG(LogMinimaxACE, Error, TEXT("[Native] MiniMax TTS failed: %s"), *Result.ErrorMessage);
			FireFinishedWorker(false);
			return;
		}

		UACEAudioCurveSourceComponent* Live = WeakConsumer.Get();
		if (!Live)
		{
			UE_LOG(LogMinimaxACE, Warning, TEXT("[Native] Consumer destroyed before audio delivered"));
			FireFinishedWorker(false);
			return;
		}

		UE_LOG(LogMinimaxACE, Log,
			TEXT("[Native] dispatching %d samples @ %d Hz (duration %.2fs)"),
			Result.Samples.Num(), Result.SampleRate, Result.DurationSec);

		AsyncTask(ENamedThreads::GameThread, [WeakNoiseInstigator]()
		{
			APawn* Pawn = Cast<APawn>(WeakNoiseInstigator.Get());
			if (!Pawn)
			{
				return;
			}
			UAISense_Hearing::ReportNoiseEvent(
				Pawn,
				Pawn->GetActorLocation(),
				/*Loudness=*/ 1.f,
				/*Instigator=*/ Pawn,
				/*MaxRange=*/ 0.f,
				/*Tag=*/ NAME_None);
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

		UE_LOG(LogMinimaxACE, Log, TEXT("[Native] AnimateFromAudioSamples returned %s"),
			bOk ? TEXT("true") : TEXT("false"));

		FireFinishedWorker(bOk);
	});

	return true;
}
