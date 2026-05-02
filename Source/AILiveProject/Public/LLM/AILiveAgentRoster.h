#pragma once

#include "CoreMinimal.h"
#include "AILiveAgentRoster.generated.h"

UENUM(BlueprintType)
enum class ELLMProvider : uint8
{
	DeepSeek UMETA(DisplayName = "DeepSeek"),
	GLM      UMETA(DisplayName = "GLM"),
	Qwen3    UMETA(DisplayName = "Qwen3"),
};

USTRUCT(BlueprintType)
struct AILIVEPROJECT_API FNPCAgentConfig
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILive|Roster")
	int32 NPCIndex = 1;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILive|Roster")
	FName NPCActorLabel;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILive|Roster")
	FString DisplayName;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILive|Roster")
	ELLMProvider Provider = ELLMProvider::DeepSeek;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILive|Roster")
	FString Voice;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILive|Roster")
	FString GenderHint;
};

namespace AILiveAgentRoster
{
	AILIVEPROJECT_API TArray<FNPCAgentConfig> GetDefaultRoster();

	AILIVEPROJECT_API FString ProviderToString(ELLMProvider Provider);

	struct FProviderEndpoint
	{
		FString ApiKey;
		FString Endpoint;
		FString Model;
	};

	AILIVEPROJECT_API FProviderEndpoint ResolveProviderEndpoint(ELLMProvider Provider);
}
