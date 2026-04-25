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

	/**
	 * 从项目根目录的 .env 文件读取任意 key 的 value。key 大小写不敏感。
	 * 进程内 cache，首次调用扫一次，后续直接 map 查。注释（#）/ 空行 / 引号 已处理。
	 * 找不到或读不到返回空字符串。
	 */
	UFUNCTION(BlueprintCallable, Category = "AI Live|Env")
	static FString GetEnvValueFromProjectEnv(const FString& KeyName);

	/**
	 * 启动时安全自检：验证 `.env` 在 `.gitignore` 里，避免误提交泄露 API key。
	 * 失败会以 UE_LOG(Error) 报警；返回 true/false 供 BP 二次校验。
	 */
	UFUNCTION(BlueprintCallable, Category = "AI Live|Env")
	static bool VerifyEnvSafety();

	/**
	 * 日志 mask helper：只保留前 4 + 后 4 字符，中间用 `...` 替代。
	 * 短于 12 字符直接返回 `****`。所有打印 API key 的地方统一走它。
	 */
	UFUNCTION(BlueprintCallable, Category = "AI Live|Env")
	static FString MaskKey(const FString& Key);
};
