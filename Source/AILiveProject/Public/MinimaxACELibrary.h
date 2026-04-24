#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "MinimaxACELibrary.generated.h"

UCLASS()
class AILIVEPROJECT_API UMinimaxACELibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()
public:
	/**
	 * 同步调用 MiniMax T2A v2（后台线程），把 PCM16 送入 NVIDIA Audio2Face-3D，
	 * 驱动 Character 上的 UACEAudioCurveSourceComponent 与其 Face AnimBP 的 ApplyACEAnimation 节点。
	 * 若 Character 还没有 UACEAudioCurveSourceComponent，会自动添加一个。
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
		const FString& VoiceId  = TEXT("male-qn-qingse"),
		const FString& Endpoint = TEXT("https://api.minimaxi.com/v1/t2a_v2"),
		FName A2FProviderName   = FName(TEXT("LocalA2F-James")));

	/** 预热 A2F 本地 TRT 引擎，降低首次调用延迟。建议在 BeginPlay 里调一次。 */
	UFUNCTION(BlueprintCallable, Category = "Minimax|ACE")
	static void PrewarmA2F(FName A2FProviderName = FName(TEXT("LocalA2F-James")));

	/** 返回当前可用的 A2F provider 名字列表（调试用，便于确认本地 provider 已加载） */
	UFUNCTION(BlueprintCallable, Category = "Minimax|ACE")
	static TArray<FName> GetAvailableA2FProviders();

	/**
	 * 从项目根目录的 .env 文件读取 `minimax=xxx` 行，返回 API key。
	 * 找不到或读不到返回空字符串（测试用；生产别走这条）。
	 */
	UFUNCTION(BlueprintCallable, Category = "Minimax|ACE")
	static FString GetMinimaxApiKeyFromProjectEnv();
};
