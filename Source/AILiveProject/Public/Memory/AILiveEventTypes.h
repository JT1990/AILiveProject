#pragma once

#include "CoreMinimal.h"
#include "Logging/LogMacros.h"
#include "AILiveEventTypes.generated.h"

AILIVEPROJECT_API DECLARE_LOG_CATEGORY_EXTERN(LogAILiveMemory, Log, All);

UENUM(BlueprintType)
enum class EAILivePhase : uint8
{
	Setup        UMETA(DisplayName = "setup"),
	DayDiscuss   UMETA(DisplayName = "day_discuss"),
	Vote         UMETA(DisplayName = "vote"),
	NightAction  UMETA(DisplayName = "night_action"),
	Reveal       UMETA(DisplayName = "reveal"),
	GameOver     UMETA(DisplayName = "game_over"),
};

UENUM(BlueprintType)
enum class EAILiveEventType : uint8
{
	SpeechPublic           UMETA(DisplayName = "speech.public"),
	SpeechScratchpad       UMETA(DisplayName = "speech.scratchpad"),
	SpeechIntended         UMETA(DisplayName = "speech.intended"),
	SpeechNote             UMETA(DisplayName = "speech.note"),
	Bid                    UMETA(DisplayName = "bid"),
	Reflection9Q           UMETA(DisplayName = "reflection.9q"),
	Vote                   UMETA(DisplayName = "vote"),
	PrivateMsg             UMETA(DisplayName = "private_msg"),
	AlliancePropose        UMETA(DisplayName = "alliance_propose"),
	AllianceAccept         UMETA(DisplayName = "alliance_accept"),
	AllianceBetray         UMETA(DisplayName = "alliance_betray"),
	ActionIntent           UMETA(DisplayName = "action.intent"),
	ActionResolved         UMETA(DisplayName = "action.resolved"),
	ActionCancelled        UMETA(DisplayName = "action.cancelled"),
	OrchestratorResolved   UMETA(DisplayName = "orchestrator.round_resolved"),
	OrchestratorTickAnchor UMETA(DisplayName = "orchestrator.tick_anchor"),
	OrchestratorTickResolved UMETA(DisplayName = "orchestrator.tick_resolved"),
	OrchestratorTickAudit  UMETA(DisplayName = "orchestrator.tick_audit"),
	SystemRoleAssigned     UMETA(DisplayName = "system.role_assigned"),
	SystemAgentTimeout     UMETA(DisplayName = "system.agent_timeout"),
	SystemParseFailed      UMETA(DisplayName = "system.parse_failed"),
	SystemLLMInflight      UMETA(DisplayName = "system.llm_inflight"),
	SystemDeleteExecuted   UMETA(DisplayName = "system.delete_executed"),
	WinnerDecision         UMETA(DisplayName = "winner_decision"),
};

UENUM(BlueprintType)
enum class EAILiveSpeechActType : uint8
{
	None     UMETA(DisplayName = ""),
	Claim    UMETA(DisplayName = "claim"),
	Accuse   UMETA(DisplayName = "accuse"),
	Defend   UMETA(DisplayName = "defend"),
	Commit   UMETA(DisplayName = "commit"),
	Deny     UMETA(DisplayName = "deny"),
	Question UMETA(DisplayName = "question"),
	Reveal   UMETA(DisplayName = "reveal"),
};

USTRUCT(BlueprintType)
struct AILIVEPROJECT_API FAILiveEvent
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadWrite, Category = "AILive|Event")
	FString EventId;

	UPROPERTY(BlueprintReadWrite, Category = "AILive|Event")
	FString GameId;

	UPROPERTY(BlueprintReadWrite, Category = "AILive|Event")
	int64 Seq = 0;

	// 写入时由 EventStore 回填为 CachedCurrentTickNo（BeginTick 设定）；
	// canonical JSON 永远排除该字段，仅作 prompt / 调试 / 切片读取用途，
	// 不参与哈希链。
	UPROPERTY(BlueprintReadOnly, Category = "AILive|Event")
	int64 TickNo = 0;

	UPROPERTY(BlueprintReadWrite, Category = "AILive|Event")
	int32 RoundNo = 0;

	UPROPERTY(BlueprintReadWrite, Category = "AILive|Event")
	EAILivePhase Phase = EAILivePhase::Setup;

	UPROPERTY(BlueprintReadWrite, Category = "AILive|Event")
	FString Actor;

	UPROPERTY(BlueprintReadWrite, Category = "AILive|Event")
	EAILiveEventType EventType = EAILiveEventType::SpeechPublic;

	UPROPERTY(BlueprintReadWrite, Category = "AILive|Event")
	EAILiveSpeechActType SpeechActType = EAILiveSpeechActType::None;

	UPROPERTY(BlueprintReadWrite, Category = "AILive|Event")
	TArray<FString> Visibility;

	UPROPERTY(BlueprintReadWrite, Category = "AILive|Event")
	TArray<FString> AddressedTo;

	UPROPERTY(BlueprintReadWrite, Category = "AILive|Event")
	FString PayloadJson;

	UPROPERTY(BlueprintReadWrite, Category = "AILive|Event")
	FString ParentEventId;

	UPROPERTY(BlueprintReadWrite, Category = "AILive|Event")
	FString ParserVersion = TEXT("1");

	UPROPERTY(BlueprintReadWrite, Category = "AILive|Event")
	FString RawLLMOutput;

	UPROPERTY(BlueprintReadOnly, Category = "AILive|Event")
	FString PrevEventHash;

	UPROPERTY(BlueprintReadOnly, Category = "AILive|Event")
	FString EventHash;

	UPROPERTY(BlueprintReadOnly, Category = "AILive|Event")
	FString WallClock;
};

UENUM(BlueprintType)
enum class EAILiveCommitmentType : uint8
{
	Promise   UMETA(DisplayName = "promise"),
	ClaimRole UMETA(DisplayName = "claim_role"),
	Deny      UMETA(DisplayName = "deny"),
	VoteFor   UMETA(DisplayName = "vote_for"),
	Alliance  UMETA(DisplayName = "alliance"),
};

UENUM(BlueprintType)
enum class EAILiveCommitmentStatus : uint8
{
	Active        UMETA(DisplayName = "active"),
	Retracted     UMETA(DisplayName = "retracted"),
	Contradicted  UMETA(DisplayName = "contradicted"),
};

USTRUCT(BlueprintType)
struct AILIVEPROJECT_API FAILiveCommitment
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "AILive|Commitment")
	FString GameId;

	UPROPERTY(BlueprintReadOnly, Category = "AILive|Commitment")
	FString AgentId;

	UPROPERTY(BlueprintReadOnly, Category = "AILive|Commitment")
	int32 RoundNo = 0;

	UPROPERTY(BlueprintReadOnly, Category = "AILive|Commitment")
	int64 Seq = 0;

	UPROPERTY(BlueprintReadOnly, Category = "AILive|Commitment")
	EAILiveCommitmentType CommitmentType = EAILiveCommitmentType::Promise;

	UPROPERTY(BlueprintReadOnly, Category = "AILive|Commitment")
	FString Target;

	UPROPERTY(BlueprintReadOnly, Category = "AILive|Commitment")
	FString Text;

	UPROPERTY(BlueprintReadOnly, Category = "AILive|Commitment")
	EAILiveCommitmentStatus Status = EAILiveCommitmentStatus::Active;
};

namespace AILiveEvent
{
	AILIVEPROJECT_API FString PhaseToString(EAILivePhase Phase);
	AILIVEPROJECT_API FString EventTypeToString(EAILiveEventType Type);
	AILIVEPROJECT_API FString SpeechActToString(EAILiveSpeechActType T);
	AILIVEPROJECT_API FString CommitmentTypeToString(EAILiveCommitmentType T);
	AILIVEPROJECT_API FString CommitmentStatusToString(EAILiveCommitmentStatus S);

	AILIVEPROJECT_API EAILivePhase            PhaseFromString(const FString& S);
	AILIVEPROJECT_API EAILiveEventType        EventTypeFromString(const FString& S);
	AILIVEPROJECT_API EAILiveSpeechActType    SpeechActFromString(const FString& S);
	AILIVEPROJECT_API EAILiveCommitmentType   CommitmentTypeFromString(const FString& S);
	AILIVEPROJECT_API EAILiveCommitmentStatus CommitmentStatusFromString(const FString& S);

	AILIVEPROJECT_API FString          ArrayToJsonString(const TArray<FString>& A);
	AILIVEPROJECT_API TArray<FString>  JsonStringToArray(const FString& Json);
}
