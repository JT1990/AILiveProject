#pragma once

#include "CoreMinimal.h"
#include "Delegates/Delegate.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "MinimaxACELibrary.generated.h"

DECLARE_DELEGATE_OneParam(FOnMinimaxSpeechFinishedNative, bool /*bSuccess*/);

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

	/*
	 * C++ only variant of TriggerMinimaxSpeechFromPawnWithNoise that fires OnFinished
	 * delegate on the GameThread once the AnimateFromAudioSamples streaming dispatch
	 * returns (i.e. effectively when the audio finished playing). bSuccess=false if
	 * the HTTP TTS call failed or the audio target / pawn was destroyed before
	 * dispatch.
	 */
	static bool TriggerMinimaxSpeechFromPawnNative(
		UObject* WorldContextObject,
		AActor* SpeakerPawn,
		const FString& Text,
		const FString& ApiKey,
		const FString& VoiceId,
		const FString& Endpoint,
		FName A2FProviderName,
		FOnMinimaxSpeechFinishedNative OnFinished);
};
