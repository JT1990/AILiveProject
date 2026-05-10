#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "MinimaxACELibrary.generated.h"

/**
 * Fired when a TriggerMinimaxSpeechFromPawnWithNoiseEx call resolves.
 *   - bSucceeded: true when audio dispatch began and completed normally;
 *                 false on TTS HTTP failure, ACE provider unavailable, PCM
 *                 issue, or audio target destroyed mid-call.
 *   - DurationSeconds: estimated audio duration (Samples / SampleRate); 0 on failure.
 *   - ErrorReason: human-readable failure description; empty on success.
 *
 * Non-dynamic delegate so C++ callers can BindWeakLambda without exposing
 * a separate UFUNCTION method. SpeakDispatcher is the sole consumer; if a BP
 * caller ever needs the completion event, wrap with a UFUNCTION shim.
 */
DECLARE_DELEGATE_ThreeParams(FOnSpeechCompleted,
	bool /*bSucceeded*/, float /*DurationSeconds*/, FString /*ErrorReason*/);

UCLASS()
class AILIVEPROJECT_API UMinimaxACELibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()
public:
	/*
	 * Calls MiniMax T2A v2 on a background thread, then sends PCM16 samples
	 * into NVIDIA Audio2Face-3D through the ACE runtime component on Character.
	 */
	UFUNCTION(BlueprintCallable, Category = "Minimax|ACE",
		meta = (WorldContext = "WorldContextObject",
			AdvancedDisplay = "VoiceId,Endpoint,A2FProviderName",
			DisplayName = "Trigger Minimax Speech"))
	static void TriggerMinimaxSpeech(
		UObject* WorldContextObject,
		AActor* Character,
		const FString& Text,
		const FString& ApiKey,
		const FString& VoiceId = TEXT("male-qn-qingse"),
		const FString& Endpoint = TEXT("https://api.minimaxi.com/v1/t2a_v2"),
		FName A2FProviderName = FName(TEXT("LocalA2F-James")));

	/*
	 * 与 TriggerMinimaxSpeech 相同的 TTS+A2F 行为，外加：HTTP TTS 成功返回后、
	 * 调用 AnimateFromAudioSamples **之前**，通过 AsyncTask 跳回 GameThread 上报
	 * 一次听觉刺激事件，Instigator/Source = NoiseInstigator pawn (Pawn Location,
	 * Loudness=1.0)。dispatch 之前发是因为 AnimateFromAudioSamples 是阻塞 streaming
	 * 调用（持续数秒），dispatch 之后再上报会让 hearing 命中推迟到 audio 几乎播完时。
	 * AISense API 不是线程安全的，必须在 GameThread 上调用。
	 *
	 * AudioTarget: 挂 UACEAudioCurveSourceComponent 的可见 actor（通常是 Pawn
	 *              的 VisualOverride ChildActorComponent.GetChildActor()，
	 *              因为 Face AnimBP 的 ApplyACEAnimation 节点要从该组件读 curve）。
	 * NoiseInstigator: AI Hearing 感知归因方，逻辑 Pawn actor。
	 */
	UFUNCTION(BlueprintCallable, Category = "Minimax|ACE",
		meta = (WorldContext = "WorldContextObject",
			AdvancedDisplay = "VoiceId,Endpoint,A2FProviderName",
			DisplayName = "Trigger Minimax Speech With Noise"))
	static void TriggerMinimaxSpeechWithNoise(
		UObject* WorldContextObject,
		AActor* AudioTarget,
		AActor* NoiseInstigator,
		const FString& Text,
		const FString& ApiKey,
		const FString& VoiceId = TEXT("male-qn-qingse"),
		const FString& Endpoint = TEXT("https://api.minimaxi.com/v1/t2a_v2"),
		FName A2FProviderName = FName(TEXT("LocalA2F-James")));

	/*
	 * Convenience wrapper for NPC pawns using the VisualOverride child actor as
	 * the ACE/A2F audio target and the pawn itself as the hearing instigator.
	 */
	UFUNCTION(BlueprintCallable, Category = "Minimax|ACE",
		meta = (WorldContext = "WorldContextObject",
			AdvancedDisplay = "VoiceId,Endpoint,A2FProviderName",
			DisplayName = "Trigger Minimax Speech From Pawn With Noise"))
	static bool TriggerMinimaxSpeechFromPawnWithNoise(
		UObject* WorldContextObject,
		AActor* SpeakerPawn,
		const FString& Text,
		const FString& ApiKey,
		const FString& VoiceId = TEXT("male-qn-qingse"),
		const FString& Endpoint = TEXT("https://api.minimaxi.com/v1/t2a_v2"),
		FName A2FProviderName = FName(TEXT("LocalA2F-James")));

	/*
	 * Same TTS+A2F+Hearing behavior as TriggerMinimaxSpeechFromPawnWithNoise
	 * with an additional completion callback.
	 *
	 * The completion timer is scheduled on the GameThread BEFORE the call to
	 * AnimateFromAudioSamples (which is a blocking streaming dispatch lasting
	 * several seconds). Scheduling it after the dispatch would double the
	 * effective latency (dispatch time + audio playback time). Scheduling it
	 * before approximates "audio actually started" because the first chunk
	 * has been queued for the ACE thread by the time the timer ticks.
	 */
	/** C++-only completion variant; see TriggerMinimaxSpeechFromPawnWithNoise for the BP path. */
	static void TriggerMinimaxSpeechFromPawnWithNoiseEx(
		UObject* WorldContextObject,
		AActor* SpeakerPawn,
		const FString& Text,
		const FString& ApiKey,
		FOnSpeechCompleted OnComplete,
		const FString& VoiceId = TEXT("male-qn-qingse"),
		const FString& Endpoint = TEXT("https://api.minimaxi.com/v1/t2a_v2"),
		FName A2FProviderName = FName(TEXT("LocalA2F-James")));

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Minimax|ACE",
		meta = (DisplayName = "Get Visual Override Audio Target"))
	static AActor* GetVisualOverrideAudioTarget(AActor* SpeakerPawn);

	UFUNCTION(BlueprintCallable, Category = "Minimax|ACE")
	static void PrewarmA2F(FName A2FProviderName = FName(TEXT("LocalA2F-James")));

	UFUNCTION(BlueprintCallable, Category = "Minimax|ACE")
	static TArray<FName> GetAvailableA2FProviders();

	/*
	 * Reads the `minimax=...` line from <ProjectDir>/.env for local testing.
	 * Keep this Blueprint function name stable; BP_MH_Character_1 references it.
	 */
	UFUNCTION(BlueprintCallable, Category = "Minimax|ACE")
	static FString GetMinimaxApiKeyFromProjectEnv();
};
