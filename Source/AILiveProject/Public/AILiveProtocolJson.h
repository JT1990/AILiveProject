#pragma once

#include "CoreMinimal.h"
#include "AILiveProtocolTypes.h"

class FJsonValue;
class FJsonObject;

/**
 * JSON serialization helpers for the AILive Brain <-> UE protocol v0.2.0.
 *
 * Per-type ToJson / FromJson functions instead of templates with explicit
 * specializations: half the structs need bespoke handling (oneOf discriminant,
 * nullable bool-flag pattern, enum string mapping), so a single template path
 * cannot cover them all. Free functions in a namespace keep call sites clear.
 *
 * All wire JSON:
 *   - UTF-8 (FJsonSerializer default).
 *   - Field order is NOT canonical; round-trip parity is checked semantically
 *     via JsonSemanticEqual.
 *   - Nullable fields encode as JSON null when bHasXxx == false; the FromJson
 *     side detects null via TSharedPtr<FJsonValue>->Type == EJson::Null.
 */
namespace AILiveProtocol
{
	// ---- helpers ---------------------------------------------------------

	AILIVEPROJECT_API FString NewIdempotencyKey();
	AILIVEPROJECT_API FString IsoUtcNow();
	AILIVEPROJECT_API bool    IsValidGameIdUuidV4(const FString& In);

	/** Recursively compare two JSON strings: dict key sets equal + each key's
	 *  value equal; array order-sensitive equal; scalars value-equal. */
	AILIVEPROJECT_API bool JsonSemanticEqual(const FString& A, const FString& B);

	// ---- enum <-> wire string -------------------------------------------

	AILIVEPROJECT_API FString CurrentActionToWire(E_AIL_NpcAction In);
	AILIVEPROJECT_API bool    WireToCurrentAction(const FString& In, E_AIL_NpcAction& Out);

	AILIVEPROJECT_API FString OutcomeToWire(E_AIL_ActionOutcome In);
	AILIVEPROJECT_API bool    WireToOutcome(const FString& In, E_AIL_ActionOutcome& Out);

	AILIVEPROJECT_API FString SpeechStatusToWire(E_AIL_SpeechStatus In);
	AILIVEPROJECT_API bool    WireToSpeechStatus(const FString& In, E_AIL_SpeechStatus& Out);

	AILIVEPROJECT_API FString RejectReasonToWire(E_AIL_RejectReason In);
	AILIVEPROJECT_API bool    WireToRejectReason(const FString& In, E_AIL_RejectReason& Out);

	// ---- per-type To/From -----------------------------------------------

	AILIVEPROJECT_API bool ToJson_Position3D(const FAIL_Position3D& In, TSharedRef<FJsonObject> Out);
	AILIVEPROJECT_API bool FromJson_Position3D(const TSharedRef<FJsonObject>& In, FAIL_Position3D& Out);

	AILIVEPROJECT_API bool ToJsonString(const FAIL_HealthResponse& In, FString& Out);
	AILIVEPROJECT_API bool FromJsonString(const FString& In, FAIL_HealthResponse& Out);

	AILIVEPROJECT_API bool ToJsonString(const FAIL_SessionCreateRequest& In, FString& Out);
	AILIVEPROJECT_API bool FromJsonString(const FString& In, FAIL_SessionCreateRequest& Out);

	AILIVEPROJECT_API bool ToJsonString(const FAIL_SessionCreateResponse& In, FString& Out);
	AILIVEPROJECT_API bool FromJsonString(const FString& In, FAIL_SessionCreateResponse& Out);

	AILIVEPROJECT_API bool ToJsonString(const FAIL_RosterRegisterRequest& In, FString& Out);
	AILIVEPROJECT_API bool FromJsonString(const FString& In, FAIL_RosterRegisterRequest& Out);

	AILIVEPROJECT_API bool ToJsonString(const FAIL_RosterRegisterResponse& In, FString& Out);
	AILIVEPROJECT_API bool FromJsonString(const FString& In, FAIL_RosterRegisterResponse& Out);

	AILIVEPROJECT_API bool ToJsonString(const FAIL_WorldStatePushRequest& In, FString& Out);
	AILIVEPROJECT_API bool FromJsonString(const FString& In, FAIL_WorldStatePushRequest& Out);

	AILIVEPROJECT_API bool ToJsonString(const FAIL_ActionPullResponse& In, FString& Out);
	AILIVEPROJECT_API bool FromJsonString(const FString& In, FAIL_ActionPullResponse& Out);

	AILIVEPROJECT_API bool ToJsonString(const FAIL_ActionResultRequest& In, FString& Out);
	AILIVEPROJECT_API bool FromJsonString(const FString& In, FAIL_ActionResultRequest& Out);

	AILIVEPROJECT_API bool ToJsonString(const FAIL_SpeechPullResponse& In, FString& Out);
	AILIVEPROJECT_API bool FromJsonString(const FString& In, FAIL_SpeechPullResponse& Out);

	AILIVEPROJECT_API bool ToJsonString(const FAIL_SpeechResultRequest& In, FString& Out);
	AILIVEPROJECT_API bool FromJsonString(const FString& In, FAIL_SpeechResultRequest& Out);

	AILIVEPROJECT_API bool ToJsonString(const FAIL_IngressRejectRequest& In, FString& Out);
	AILIVEPROJECT_API bool FromJsonString(const FString& In, FAIL_IngressRejectRequest& Out);

	AILIVEPROJECT_API bool ToJsonString(const FAIL_ErrorResponse& In, FString& Out);
	AILIVEPROJECT_API bool FromJsonString(const FString& In, FAIL_ErrorResponse& Out);

	AILIVEPROJECT_API bool ToJsonString(const FAIL_EventQueryResponse& In, FString& Out);
	AILIVEPROJECT_API bool FromJsonString(const FString& In, FAIL_EventQueryResponse& Out);
}
