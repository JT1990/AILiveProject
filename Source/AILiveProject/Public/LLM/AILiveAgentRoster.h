#pragma once

#include "CoreMinimal.h"
#include "Memory/AILiveAgentTypes.h"
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
	FAILiveAgentCore Core;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILive|Roster")
	FAILiveAgentIdentity Identity;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILive|Roster")
	FAILiveAgentBattleConfig Battle;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILive|Roster|Runtime")
	int32 NPCIndex = 1;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILive|Roster|Runtime")
	FName NPCActorLabel;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILive|Roster|Runtime")
	FString VoicePresentationHint;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILive|Roster|Runtime")
	ELLMProvider Provider = ELLMProvider::DeepSeek;
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
