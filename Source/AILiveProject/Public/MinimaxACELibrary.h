#pragma once

// =============================================================================
// 中文教学：MinimaxACELibrary.h —— TTS + A2F (Audio2Face) 整合的 BP 函数库
//
// 这是项目最核心的「会说会动」入口。整条流水线：
//
//   ┌────────────────────────────────────────────────────────────────────┐
//   │  TriggerMinimaxSpeechFromPawnWithNoise(SpeakerPawn, Text, ApiKey)  │
//   │                            │                                        │
//   │              【后台线程 ThreadPool】                                  │
//   │                            ▼                                        │
//   │       MiniMax TTS HTTP   →  PCM16 16kHz mono                        │
//   │                            │                                        │
//   │              【切回 GameThread (AsyncTask)】                          │
//   │                            ▼                                        │
//   │       发 AISense_Hearing 噪声刺激（让其它 NPC 能感知）                  │
//   │                            ▼                                        │
//   │       挂 / 拿 UACEAudioCurveSourceComponent                         │
//   │                            ▼                                        │
//   │       FACERuntimeModule::AnimateFromAudioSamples(PCM)              │
//   │                            ↓                                        │
//   │             NVIDIA A2F 算 face curve 流  +  音频播放                  │
//   │                            ↓                                        │
//   │             MetaHuman face AnimBP 的 ApplyACEAnimation 节点         │
//   │             从组件读 curve 驱动嘴型 + 表情                              │
//   └────────────────────────────────────────────────────────────────────┘
//
// 典型端到端延迟 1~4 秒。首次调用前一定要 PrewarmA2F() 一次，否则 TRT 编译
// 第一次会延迟 5+ 秒。
//
// 关键 UE 概念：
//
//   1) UBlueprintFunctionLibrary
//      只能放静态函数的 UCLASS。蓝图节点通过 UFUNCTION 暴露调用入口。
//      不需要实例化，不进 GC。本项目的 BP TTS 节点都从这里出。
//
//   2) DECLARE_DELEGATE_OneParam（非 dynamic）
//      raw delegate（C++ 直调，不进反射，不暴露给蓝图）。比 DYNAMIC 版快但
//      不能在 BP 里 bind。仅供 C++ 调用方用。
//      OneParam 表示带一个参数（这里是 bool bSuccess）。
//
//   3) UFUNCTION 的 meta=(...) 参数
//      WorldContext = "..."        ：BP 节点上不显示这个参数；运行时用它取 World
//      AdvancedDisplay = "x,y,z"   ：编辑面板里这些参数默认折叠，可点开
//      DisplayName = "..."         ：BP 节点显示的友好名字
//
//   4) FName vs FString vs TCHAR*
//      FName    ：哈希字符串、不可变、比较 O(1)。`A2FProviderName` 是 FName
//                  因为它会被频繁查表（A2FProvider registry）。
//      FString  ：可变字符串、按值/引用传。
//      TCHAR*   ：原生字符串字面量（TEXT() 宏返回的）。
// =============================================================================

#include "CoreMinimal.h"
#include "Delegates/Delegate.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "MinimaxACELibrary.generated.h"

// raw delegate（非 dynamic）：纯 C++ 调用，不进反射、不能 BP 绑。
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
