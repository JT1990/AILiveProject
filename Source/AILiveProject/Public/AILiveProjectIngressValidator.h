#pragma once

#include "CoreMinimal.h"
#include "AILiveProtocolTypes.h"

class UAILiveProjectRosterSubsystem;

/**
 * Static whitelist checks applied to brain-derived action / speech events
 * before UE dispatches them. Any check that fails fills OutReject with a
 * raw_message_snippet (truncated to 512 chars) and the appropriate
 * reject_reason; caller POSTs the result to /v1/games/{gid}/ingress_reject.
 *
 * UE NEVER fabricates system.validation_failed (Reasoner-layer event with
 * different schema requirements). UE rejects only via system.ingress_rejected
 * derived from the POST body here.
 */
class AILIVEPROJECT_API FAILiveProjectIngressValidator
{
public:
	static bool ValidateActionIntent(
		const FAIL_ActionIntentEvent& Event,
		const UAILiveProjectRosterSubsystem& Roster,
		FAIL_IngressRejectRequest& OutReject);

	static bool ValidateSpeechPublic(
		const FAIL_SpeechPublicEvent& Event,
		const UAILiveProjectRosterSubsystem& Roster,
		FAIL_IngressRejectRequest& OutReject);

	static FString TruncateSnippet(const FString& In, int32 MaxLen = 512);
};
