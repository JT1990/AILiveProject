#pragma once

#include "CoreMinimal.h"
#include "AILiveProtocolTypes.generated.h"

// USTRUCT mirrors of BrainService protocol v0.1.1 schemas (12 schemas, 30+ structs).
// All names are FAIL_* prefixed to avoid collision with engine / project types.
//
// Nullable / optional schema fields use the "bool flag + value" pattern because
// UE reflection does NOT support TOptional<T> as UPROPERTY. JSON serialization
// helpers in AILiveProtocolJson.h emit JSON null when bHasXxx == false.
//
// Wire enums (current_action / outcome / status / reject_reason) keep schema
// string casing on the wire; UENUM names are internal labels only.


UENUM(BlueprintType)
enum class E_AIL_NpcAction : uint8
{
	Idle      UMETA(DisplayName = "idle"),
	Moving    UMETA(DisplayName = "moving"),
	Sitting   UMETA(DisplayName = "sitting"),
	Speaking  UMETA(DisplayName = "speaking"),
};

UENUM(BlueprintType)
enum class E_AIL_ActionOutcome : uint8
{
	Succeeded UMETA(DisplayName = "succeeded"),
	Failed    UMETA(DisplayName = "failed"),
};

UENUM(BlueprintType)
enum class E_AIL_SpeechStatus : uint8
{
	Succeeded UMETA(DisplayName = "succeeded"),
	Failed    UMETA(DisplayName = "failed"),
};

UENUM(BlueprintType)
enum class E_AIL_RejectReason : uint8
{
	ActorNotInRoster          UMETA(DisplayName = "ACTOR_NOT_IN_ROSTER"),
	AudienceOutsideVisibility UMETA(DisplayName = "AUDIENCE_OUTSIDE_VISIBILITY"),
	IntentNotInOntology       UMETA(DisplayName = "INTENT_NOT_IN_ONTOLOGY"),
	SentenceTooLong           UMETA(DisplayName = "SENTENCE_TOO_LONG"),
};


USTRUCT(BlueprintType)
struct AILIVEPROJECT_API FAIL_Position3D
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadWrite, Category = "AILive|Protocol")
	double X = 0.0;

	UPROPERTY(BlueprintReadWrite, Category = "AILive|Protocol")
	double Y = 0.0;

	UPROPERTY(BlueprintReadWrite, Category = "AILive|Protocol")
	double Z = 0.0;
};


// --- health.schema.json ---

USTRUCT(BlueprintType)
struct AILIVEPROJECT_API FAIL_HealthResponse
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "AILive|Protocol")
	FString Status;

	UPROPERTY(BlueprintReadOnly, Category = "AILive|Protocol")
	FString ProtocolVersion;

	UPROPERTY(BlueprintReadOnly, Category = "AILive|Protocol")
	FString ServerTime;
};


// --- session_create.schema.json ---

USTRUCT(BlueprintType)
struct AILIVEPROJECT_API FAIL_SessionCreateRequest
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadWrite, Category = "AILive|Protocol")
	bool bHasClientLabel = false;

	UPROPERTY(BlueprintReadWrite, Category = "AILive|Protocol")
	FString ClientLabel;
};

USTRUCT(BlueprintType)
struct AILIVEPROJECT_API FAIL_SessionCreateResponse
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "AILive|Protocol")
	FString GameId;

	UPROPERTY(BlueprintReadOnly, Category = "AILive|Protocol")
	FString ProtocolVersion;

	UPROPERTY(BlueprintReadOnly, Category = "AILive|Protocol")
	FString ServerTime;
};


// --- roster_register.schema.json ---

USTRUCT(BlueprintType)
struct AILIVEPROJECT_API FAIL_NPCEntry
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadWrite, Category = "AILive|Protocol")
	FString ActorId;

	UPROPERTY(BlueprintReadWrite, Category = "AILive|Protocol")
	FString DisplayName;

	UPROPERTY(BlueprintReadWrite, Category = "AILive|Protocol")
	bool bHasInitialPosition = false;

	UPROPERTY(BlueprintReadWrite, Category = "AILive|Protocol")
	FAIL_Position3D InitialPosition;
};

USTRUCT(BlueprintType)
struct AILIVEPROJECT_API FAIL_RosterRegisterRequest
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadWrite, Category = "AILive|Protocol")
	TArray<FAIL_NPCEntry> Roster;
};

USTRUCT(BlueprintType)
struct AILIVEPROJECT_API FAIL_RosterRegisterResponse
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "AILive|Protocol")
	FString GameId;

	UPROPERTY(BlueprintReadOnly, Category = "AILive|Protocol")
	int32 AcceptedCount = 0;
};


// --- world_state_push.schema.json ---

USTRUCT(BlueprintType)
struct AILIVEPROJECT_API FAIL_SightEntry
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadWrite, Category = "AILive|Protocol")
	double DistanceCm = 0.0;

	UPROPERTY(BlueprintReadWrite, Category = "AILive|Protocol")
	double RelYawDegrees = 0.0;

	UPROPERTY(BlueprintReadWrite, Category = "AILive|Protocol")
	FString TargetActorId;
};

USTRUCT(BlueprintType)
struct AILIVEPROJECT_API FAIL_HeardSound
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadWrite, Category = "AILive|Protocol")
	double AgeSeconds = 0.0;

	UPROPERTY(BlueprintReadWrite, Category = "AILive|Protocol")
	double Loudness = 0.0;

	UPROPERTY(BlueprintReadWrite, Category = "AILive|Protocol")
	double RelYawDegrees = 0.0;

	UPROPERTY(BlueprintReadWrite, Category = "AILive|Protocol")
	FString SourceActorId;
};

USTRUCT(BlueprintType)
struct AILIVEPROJECT_API FAIL_NpcObservation
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadWrite, Category = "AILive|Protocol")
	FString ActorId;

	UPROPERTY(BlueprintReadWrite, Category = "AILive|Protocol")
	E_AIL_NpcAction CurrentAction = E_AIL_NpcAction::Idle;

	UPROPERTY(BlueprintReadWrite, Category = "AILive|Protocol")
	double FacingDegrees = 0.0;

	UPROPERTY(BlueprintReadWrite, Category = "AILive|Protocol")
	FAIL_Position3D Position;

	UPROPERTY(BlueprintReadWrite, Category = "AILive|Protocol")
	TArray<FAIL_SightEntry> SightedActors;

	UPROPERTY(BlueprintReadWrite, Category = "AILive|Protocol")
	TArray<FAIL_HeardSound> HeardSounds;
};

USTRUCT(BlueprintType)
struct AILIVEPROJECT_API FAIL_WorldStatePushRequest
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadWrite, Category = "AILive|Protocol")
	int32 ClientSampleId = 0;

	UPROPERTY(BlueprintReadWrite, Category = "AILive|Protocol")
	FString SampleWallClockTs;

	UPROPERTY(BlueprintReadWrite, Category = "AILive|Protocol")
	TArray<FAIL_NpcObservation> Observations;
};


// --- action_pull.schema.json (oneOf intent) ---

USTRUCT(BlueprintType)
struct AILIVEPROJECT_API FAIL_MoveToParams
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadWrite, Category = "AILive|Protocol")
	bool bHasCoords = false;

	UPROPERTY(BlueprintReadWrite, Category = "AILive|Protocol")
	FAIL_Position3D Coords;

	UPROPERTY(BlueprintReadWrite, Category = "AILive|Protocol")
	bool bHasTargetNpc = false;

	UPROPERTY(BlueprintReadWrite, Category = "AILive|Protocol")
	FString TargetNpc;

	UPROPERTY(BlueprintReadWrite, Category = "AILive|Protocol")
	bool bHasTargetZone = false;

	UPROPERTY(BlueprintReadWrite, Category = "AILive|Protocol")
	FString TargetZone;
};

USTRUCT(BlueprintType)
struct AILIVEPROJECT_API FAIL_SitParams
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadWrite, Category = "AILive|Protocol")
	FString TargetSmartObject;
};

USTRUCT(BlueprintType)
struct AILIVEPROJECT_API FAIL_WaitParams
{
	GENERATED_BODY()

	// wait.params field itself is optional in the schema; bHasParams=false means
	// the wire JSON omitted "params" entirely.
	UPROPERTY(BlueprintReadWrite, Category = "AILive|Protocol")
	bool bHasParams = false;

	UPROPERTY(BlueprintReadWrite, Category = "AILive|Protocol")
	bool bHasReason = false;

	UPROPERTY(BlueprintReadWrite, Category = "AILive|Protocol")
	FString Reason;
};

USTRUCT(BlueprintType)
struct AILIVEPROJECT_API FAIL_OntologyV1Intent
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "AILive|Protocol")
	FString ActionOntologyVersion;

	// Discriminant: "move_to" / "sit" / "wait". Only the matching params struct
	// holds meaningful data; the others remain at default values.
	UPROPERTY(BlueprintReadOnly, Category = "AILive|Protocol")
	FString Name;

	UPROPERTY(BlueprintReadOnly, Category = "AILive|Protocol")
	FAIL_MoveToParams MoveToParams;

	UPROPERTY(BlueprintReadOnly, Category = "AILive|Protocol")
	FAIL_SitParams SitParams;

	UPROPERTY(BlueprintReadOnly, Category = "AILive|Protocol")
	FAIL_WaitParams WaitParams;
};

USTRUCT(BlueprintType)
struct AILIVEPROJECT_API FAIL_ActionIntentEvent
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "AILive|Protocol")
	FString ActorId;

	UPROPERTY(BlueprintReadOnly, Category = "AILive|Protocol")
	int64 Seq = 0;

	UPROPERTY(BlueprintReadOnly, Category = "AILive|Protocol")
	FAIL_OntologyV1Intent Intent;
};

USTRUCT(BlueprintType)
struct AILIVEPROJECT_API FAIL_ActionPullResponse
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "AILive|Protocol")
	TArray<FAIL_ActionIntentEvent> Events;

	UPROPERTY(BlueprintReadOnly, Category = "AILive|Protocol")
	int64 NextCursor = 0;
};


// --- action_result.schema.json ---

USTRUCT(BlueprintType)
struct AILIVEPROJECT_API FAIL_ActionResultRequest
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadWrite, Category = "AILive|Protocol")
	FString ActorId;

	UPROPERTY(BlueprintReadWrite, Category = "AILive|Protocol")
	int64 IntentSeq = 0;

	UPROPERTY(BlueprintReadWrite, Category = "AILive|Protocol")
	E_AIL_ActionOutcome Outcome = E_AIL_ActionOutcome::Succeeded;

	UPROPERTY(BlueprintReadWrite, Category = "AILive|Protocol")
	int32 DurationMs = 0;

	UPROPERTY(BlueprintReadWrite, Category = "AILive|Protocol")
	FAIL_Position3D FinalPosition;

	UPROPERTY(BlueprintReadWrite, Category = "AILive|Protocol")
	bool bHasErrorReason = false;

	UPROPERTY(BlueprintReadWrite, Category = "AILive|Protocol")
	FString ErrorReason;
};


// --- speech_pull.schema.json ---

USTRUCT(BlueprintType)
struct AILIVEPROJECT_API FAIL_SpeechPublicEvent
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "AILive|Protocol")
	FString ActorId;

	UPROPERTY(BlueprintReadOnly, Category = "AILive|Protocol")
	int64 Seq = 0;

	UPROPERTY(BlueprintReadOnly, Category = "AILive|Protocol")
	FString Text;

	UPROPERTY(BlueprintReadOnly, Category = "AILive|Protocol")
	TArray<FString> AddressedTo;
};

USTRUCT(BlueprintType)
struct AILIVEPROJECT_API FAIL_SpeechPullResponse
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "AILive|Protocol")
	TArray<FAIL_SpeechPublicEvent> Events;

	UPROPERTY(BlueprintReadOnly, Category = "AILive|Protocol")
	int64 NextCursor = 0;
};


// --- speech_result.schema.json ---

USTRUCT(BlueprintType)
struct AILIVEPROJECT_API FAIL_SpeechResultRequest
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadWrite, Category = "AILive|Protocol")
	FString ActorId;

	UPROPERTY(BlueprintReadWrite, Category = "AILive|Protocol")
	int64 SpeechSeq = 0;

	UPROPERTY(BlueprintReadWrite, Category = "AILive|Protocol")
	E_AIL_SpeechStatus Status = E_AIL_SpeechStatus::Succeeded;

	UPROPERTY(BlueprintReadWrite, Category = "AILive|Protocol")
	int32 DurationMs = 0;

	UPROPERTY(BlueprintReadWrite, Category = "AILive|Protocol")
	bool bHasErrorReason = false;

	UPROPERTY(BlueprintReadWrite, Category = "AILive|Protocol")
	FString ErrorReason;
};


// --- ingress_reject.schema.json ---

USTRUCT(BlueprintType)
struct AILIVEPROJECT_API FAIL_IngressRejectRequest
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadWrite, Category = "AILive|Protocol")
	FString ActorId;

	UPROPERTY(BlueprintReadWrite, Category = "AILive|Protocol")
	int64 OriginalSeq = 0;

	UPROPERTY(BlueprintReadWrite, Category = "AILive|Protocol")
	FString RawMessageSnippet;

	UPROPERTY(BlueprintReadWrite, Category = "AILive|Protocol")
	E_AIL_RejectReason RejectReason = E_AIL_RejectReason::ActorNotInRoster;
};


// --- error_response.schema.json ---

USTRUCT(BlueprintType)
struct AILIVEPROJECT_API FAIL_ErrorDetails
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "AILive|Protocol")
	bool bHasField = false;

	UPROPERTY(BlueprintReadOnly, Category = "AILive|Protocol")
	FString Field;

	UPROPERTY(BlueprintReadOnly, Category = "AILive|Protocol")
	bool bHasReceived = false;

	UPROPERTY(BlueprintReadOnly, Category = "AILive|Protocol")
	FString Received;

	UPROPERTY(BlueprintReadOnly, Category = "AILive|Protocol")
	bool bHasHint = false;

	UPROPERTY(BlueprintReadOnly, Category = "AILive|Protocol")
	FString Hint;

	UPROPERTY(BlueprintReadOnly, Category = "AILive|Protocol")
	bool bHasCurrentMinSeq = false;

	UPROPERTY(BlueprintReadOnly, Category = "AILive|Protocol")
	int64 CurrentMinSeq = 0;

	UPROPERTY(BlueprintReadOnly, Category = "AILive|Protocol")
	bool bHasProvidedSeq = false;

	UPROPERTY(BlueprintReadOnly, Category = "AILive|Protocol")
	int64 ProvidedSeq = 0;

	UPROPERTY(BlueprintReadOnly, Category = "AILive|Protocol")
	bool bHasProvidedSinceSeq = false;

	UPROPERTY(BlueprintReadOnly, Category = "AILive|Protocol")
	int64 ProvidedSinceSeq = 0;
};

USTRUCT(BlueprintType)
struct AILIVEPROJECT_API FAIL_ErrorResponse
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "AILive|Protocol")
	FString ErrorCode;

	UPROPERTY(BlueprintReadOnly, Category = "AILive|Protocol")
	FString Message;

	UPROPERTY(BlueprintReadOnly, Category = "AILive|Protocol")
	bool Retryable = false;

	UPROPERTY(BlueprintReadOnly, Category = "AILive|Protocol")
	bool bHasDetails = false;

	UPROPERTY(BlueprintReadOnly, Category = "AILive|Protocol")
	FAIL_ErrorDetails Details;
};


// --- common.event_meta + event_query.schema.json ---

USTRUCT(BlueprintType)
struct AILIVEPROJECT_API FAIL_EventMeta
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "AILive|Protocol")
	int64 Seq = 0;

	UPROPERTY(BlueprintReadOnly, Category = "AILive|Protocol")
	FString GameId;

	UPROPERTY(BlueprintReadOnly, Category = "AILive|Protocol")
	int64 RoundNo = 0;

	UPROPERTY(BlueprintReadOnly, Category = "AILive|Protocol")
	FString Phase;

	UPROPERTY(BlueprintReadOnly, Category = "AILive|Protocol")
	FString Actor;

	UPROPERTY(BlueprintReadOnly, Category = "AILive|Protocol")
	FString EventType;

	UPROPERTY(BlueprintReadOnly, Category = "AILive|Protocol")
	TArray<FString> Visibility;

	UPROPERTY(BlueprintReadOnly, Category = "AILive|Protocol")
	TArray<FString> AddressedTo;

	// Payload structure varies by event_type; passed through as raw JSON string
	// (the helpers store the original sub-object's serialized form here).
	UPROPERTY(BlueprintReadOnly, Category = "AILive|Protocol")
	FString PayloadJson;

	UPROPERTY(BlueprintReadOnly, Category = "AILive|Protocol")
	bool bHasParentEventId = false;

	UPROPERTY(BlueprintReadOnly, Category = "AILive|Protocol")
	int64 ParentEventId = 0;

	UPROPERTY(BlueprintReadOnly, Category = "AILive|Protocol")
	bool bHasPrevHash = false;

	UPROPERTY(BlueprintReadOnly, Category = "AILive|Protocol")
	FString PrevHash;

	UPROPERTY(BlueprintReadOnly, Category = "AILive|Protocol")
	FString EventHash;

	UPROPERTY(BlueprintReadOnly, Category = "AILive|Protocol")
	FString WallClockTs;
};

USTRUCT(BlueprintType)
struct AILIVEPROJECT_API FAIL_EventQueryResponse
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "AILive|Protocol")
	TArray<FAIL_EventMeta> Events;

	UPROPERTY(BlueprintReadOnly, Category = "AILive|Protocol")
	int64 NextCursor = 0;
};
