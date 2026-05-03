#pragma once

#include "CoreMinimal.h"
#include "AILiveAgentTypes.generated.h"

UENUM(BlueprintType)
enum class EAILiveAgentStatus : uint8
{
	Active   UMETA(DisplayName = "active"),
	Deleted  UMETA(DisplayName = "deleted"),
	Archived UMETA(DisplayName = "archived"),
};

// 表征声线与形象呈现，非人类二元生理性别。对齐 PRD：AI 是 AI，不扮演人类。
UENUM(BlueprintType)
enum class EAILiveVoicePresentation : uint8
{
	Masculine    UMETA(DisplayName = "masculine"),
	Feminine     UMETA(DisplayName = "feminine"),
	Androgynous  UMETA(DisplayName = "androgynous"),
	Synthetic    UMETA(DisplayName = "synthetic"),
	Custom       UMETA(DisplayName = "custom"),
};

USTRUCT(BlueprintType)
struct AILIVEPROJECT_API FAILiveAgentCore
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILive|Agent|Core")
	FString AgentId;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILive|Agent|Core")
	int32 PersonaVersion = 1;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILive|Agent|Core")
	FString ModelProvider;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILive|Agent|Core")
	FString ModelName;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILive|Agent|Core")
	EAILiveAgentStatus Status = EAILiveAgentStatus::Active;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILive|Agent|Core")
	FString CreatedAt;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILive|Agent|Core")
	FString DeletedAt;
};

USTRUCT(BlueprintType)
struct AILIVEPROJECT_API FAILiveAgentIdentity
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILive|Agent|Identity")
	FString FullName;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILive|Agent|Identity")
	FString Nickname;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILive|Agent|Identity")
	EAILiveVoicePresentation VoicePresentation = EAILiveVoicePresentation::Synthetic;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILive|Agent|Identity")
	FString Voice;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILive|Agent|Identity")
	FString Appearance;
};

USTRUCT(BlueprintType)
struct AILIVEPROJECT_API FAILiveAgentBattleConfig
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILive|Agent|Battle")
	FString Faction;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILive|Agent|Battle")
	FString Role;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILive|Agent|Battle")
	TArray<FString> AllianceMembers;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILive|Agent|Battle")
	FString PrivateGoal;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILive|Agent|Battle")
	float BidOffset = 0.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILive|Agent|Battle")
	int64 SeqStart = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILive|Agent|Battle")
	bool bAlive = true;
};
