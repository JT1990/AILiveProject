#include "AILiveProjectIngressValidator.h"

#include "AILiveProjectRosterSubsystem.h"
#include "AILiveProtocolJson.h"

namespace
{
	bool ActorIsInRoster(const FString& ActorId, const UAILiveProjectRosterSubsystem& Roster)
	{
		// We treat presence in ActorIdToPawn (via FindPawnByActorId) as the
		// authoritative answer; an actor_id only enters that map after a Pawn
		// was successfully registered.
		return Roster.FindPawnByActorId(ActorId) != nullptr;
	}
}

FString FAILiveProjectIngressValidator::TruncateSnippet(const FString& In, int32 MaxLen)
{
	return In.Len() <= MaxLen ? In : In.Left(MaxLen);
}

bool FAILiveProjectIngressValidator::ValidateActionIntent(
	const FAIL_ActionIntentEvent& Event,
	const UAILiveProjectRosterSubsystem& Roster,
	FAIL_IngressRejectRequest& OutReject)
{
	OutReject.ActorId = Event.ActorId;
	OutReject.OriginalSeq = Event.Seq;

	if (!ActorIsInRoster(Event.ActorId, Roster))
	{
		OutReject.RejectReason = E_AIL_RejectReason::ActorNotInRoster;
		OutReject.RawMessageSnippet = TruncateSnippet(
			FString::Printf(TEXT("actor_id=%s not in roster"), *Event.ActorId));
		return false;
	}

	const FString& Name = Event.Intent.Name;
	if (Name != TEXT("move_to") && Name != TEXT("sit") && Name != TEXT("wait"))
	{
		OutReject.RejectReason = E_AIL_RejectReason::IntentNotInOntology;
		OutReject.RawMessageSnippet = TruncateSnippet(
			FString::Printf(TEXT("intent.name='%s' not in ontology v1 {move_to,sit,wait}"), *Name));
		return false;
	}

	if (Name == TEXT("move_to"))
	{
		const FAIL_MoveToParams& Mt = Event.Intent.MoveToParams;
		// target_zone is unbound in MVP UE — T9 will introduce zone dictionary.
		// coords override is acceptable; with coords we can resolve regardless of target.
		if (Mt.bHasTargetZone && !Mt.bHasCoords && !Mt.bHasTargetNpc)
		{
			OutReject.RejectReason = E_AIL_RejectReason::IntentNotInOntology;
			OutReject.RawMessageSnippet = TruncateSnippet(
				FString::Printf(TEXT("target_zone='%s' unbound in MVP UE; T9 will introduce zone dictionary"),
					*Mt.TargetZone));
			return false;
		}
		// move_to needs at least one routing source. (Schema enforces target_npc OR
		// target_zone; coords is an override that may stand on its own only if a
		// target was provided. Lacking everything is malformed.)
		if (!Mt.bHasTargetNpc && !Mt.bHasTargetZone && !Mt.bHasCoords)
		{
			OutReject.RejectReason = E_AIL_RejectReason::IntentNotInOntology;
			OutReject.RawMessageSnippet = TruncateSnippet(
				TEXT("move_to params missing target_npc/target_zone/coords"));
			return false;
		}
		if (Mt.bHasTargetNpc && !ActorIsInRoster(Mt.TargetNpc, Roster))
		{
			OutReject.RejectReason = E_AIL_RejectReason::ActorNotInRoster;
			OutReject.RawMessageSnippet = TruncateSnippet(
				FString::Printf(TEXT("move_to target_npc='%s' not in roster"), *Mt.TargetNpc));
			return false;
		}
	}
	else if (Name == TEXT("sit"))
	{
		if (Event.Intent.SitParams.TargetSmartObject.IsEmpty())
		{
			OutReject.RejectReason = E_AIL_RejectReason::IntentNotInOntology;
			OutReject.RawMessageSnippet = TruncateSnippet(TEXT("sit params missing target_smartobject"));
			return false;
		}
	}

	return true;
}

bool FAILiveProjectIngressValidator::ValidateSpeechPublic(
	const FAIL_SpeechPublicEvent& Event,
	const UAILiveProjectRosterSubsystem& Roster,
	FAIL_IngressRejectRequest& OutReject)
{
	OutReject.ActorId = Event.ActorId;
	OutReject.OriginalSeq = Event.Seq;

	if (!ActorIsInRoster(Event.ActorId, Roster))
	{
		OutReject.RejectReason = E_AIL_RejectReason::ActorNotInRoster;
		OutReject.RawMessageSnippet = TruncateSnippet(
			FString::Printf(TEXT("actor_id=%s not in roster"), *Event.ActorId));
		return false;
	}
	if (Event.Text.Len() > 2000)
	{
		OutReject.RejectReason = E_AIL_RejectReason::SentenceTooLong;
		OutReject.RawMessageSnippet = TruncateSnippet(Event.Text);
		return false;
	}
	for (const FString& A : Event.AddressedTo)
	{
		// Literal "public" is NOT a member of the closed actor_id enum and must
		// not appear in addressed_to (an empty array means broadcast).
		if (A == TEXT("public") || !ActorIsInRoster(A, Roster))
		{
			OutReject.RejectReason = E_AIL_RejectReason::AudienceOutsideVisibility;
			OutReject.RawMessageSnippet = TruncateSnippet(
				FString::Printf(TEXT("addressed_to entry '%s' not in roster"), *A));
			return false;
		}
	}
	return true;
}
