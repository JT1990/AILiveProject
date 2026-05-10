#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "AILiveProjectIngressValidator.h"
#include "AILiveProjectRosterSubsystem.h"
#include "AILiveProtocolJson.h"
#include "AILiveProtocolTypes.h"
#include "GameFramework/Pawn.h"
#include "UObject/Package.h"

namespace
{
	UAILiveProjectRosterSubsystem* MakeTestRoster(const TArray<FString>& Ids)
	{
		UAILiveProjectRosterSubsystem* Roster = NewObject<UAILiveProjectRosterSubsystem>();
		// Stand-in pawn: use a transient package-rooted Object as the
		// distinguishing identity. The validator only checks the map presence,
		// not the pawn type.
		for (const FString& Id : Ids)
		{
			APawn* StubPawn = NewObject<APawn>(GetTransientPackage());
			Roster->RegisterPawn(Id, StubPawn);
		}
		return Roster;
	}

	FAIL_ActionIntentEvent MakeWaitIntent(const FString& ActorId, int64 Seq)
	{
		FAIL_ActionIntentEvent Ev;
		Ev.ActorId = ActorId;
		Ev.Seq = Seq;
		Ev.Intent.ActionOntologyVersion = TEXT("1");
		Ev.Intent.Name = TEXT("wait");
		Ev.Intent.WaitParams.bHasParams = false;
		return Ev;
	}

	FAIL_ActionIntentEvent MakeMoveToZoneIntent(const FString& ActorId, int64 Seq, const FString& Zone)
	{
		FAIL_ActionIntentEvent Ev;
		Ev.ActorId = ActorId;
		Ev.Seq = Seq;
		Ev.Intent.ActionOntologyVersion = TEXT("1");
		Ev.Intent.Name = TEXT("move_to");
		Ev.Intent.MoveToParams.bHasTargetZone = true;
		Ev.Intent.MoveToParams.TargetZone = Zone;
		return Ev;
	}
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAILiveIngressValidator_NonRosterActor,
	"AILive.IngressValidator.NonRosterActor",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FAILiveIngressValidator_NonRosterActor::RunTest(const FString&)
{
	UAILiveProjectRosterSubsystem* Roster = MakeTestRoster({TEXT("NPC01")});
	FAIL_IngressRejectRequest Reject;
	const bool bOk = FAILiveProjectIngressValidator::ValidateActionIntent(
		MakeWaitIntent(TEXT("NPC11"), 100), *Roster, Reject);
	TestFalse(TEXT("non-roster actor rejected"), bOk);
	TestEqual(TEXT("reject_reason"), (uint8)Reject.RejectReason,
		(uint8)E_AIL_RejectReason::ActorNotInRoster);
	return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAILiveIngressValidator_TargetZoneRejected,
	"AILive.IngressValidator.TargetZoneRejected",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FAILiveIngressValidator_TargetZoneRejected::RunTest(const FString&)
{
	UAILiveProjectRosterSubsystem* Roster = MakeTestRoster({TEXT("NPC03")});
	FAIL_IngressRejectRequest Reject;
	const bool bOk = FAILiveProjectIngressValidator::ValidateActionIntent(
		MakeMoveToZoneIntent(TEXT("NPC03"), 200, TEXT("dining_hall")), *Roster, Reject);
	TestFalse(TEXT("target_zone rejected"), bOk);
	TestEqual(TEXT("reject_reason"), (uint8)Reject.RejectReason,
		(uint8)E_AIL_RejectReason::IntentNotInOntology);
	TestTrue(TEXT("snippet mentions T9 zone dictionary"),
		Reject.RawMessageSnippet.Contains(TEXT("T9 will introduce zone dictionary")));
	return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAILiveIngressValidator_UnknownIntent,
	"AILive.IngressValidator.UnknownIntent",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FAILiveIngressValidator_UnknownIntent::RunTest(const FString&)
{
	UAILiveProjectRosterSubsystem* Roster = MakeTestRoster({TEXT("NPC03")});
	FAIL_ActionIntentEvent Ev;
	Ev.ActorId = TEXT("NPC03");
	Ev.Seq = 300;
	Ev.Intent.ActionOntologyVersion = TEXT("1");
	Ev.Intent.Name = TEXT("dance");

	FAIL_IngressRejectRequest Reject;
	const bool bOk = FAILiveProjectIngressValidator::ValidateActionIntent(Ev, *Roster, Reject);
	TestFalse(TEXT("unknown intent rejected"), bOk);
	TestEqual(TEXT("reject_reason"), (uint8)Reject.RejectReason,
		(uint8)E_AIL_RejectReason::IntentNotInOntology);
	return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAILiveIngressValidator_SpeechSentenceTooLong,
	"AILive.IngressValidator.SpeechSentenceTooLong",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FAILiveIngressValidator_SpeechSentenceTooLong::RunTest(const FString&)
{
	UAILiveProjectRosterSubsystem* Roster = MakeTestRoster({TEXT("NPC02")});
	FAIL_SpeechPublicEvent Ev;
	Ev.ActorId = TEXT("NPC02");
	Ev.Seq = 400;
	Ev.Text = FString::ChrN(2001, 'A');

	FAIL_IngressRejectRequest Reject;
	const bool bOk = FAILiveProjectIngressValidator::ValidateSpeechPublic(Ev, *Roster, Reject);
	TestFalse(TEXT("oversized text rejected"), bOk);
	TestEqual(TEXT("reject_reason"), (uint8)Reject.RejectReason,
		(uint8)E_AIL_RejectReason::SentenceTooLong);
	TestTrue(TEXT("snippet truncated to <= 512"), Reject.RawMessageSnippet.Len() <= 512);
	return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAILiveIngressValidator_SpeechAudienceLiteralPublic,
	"AILive.IngressValidator.SpeechAudienceLiteralPublic",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FAILiveIngressValidator_SpeechAudienceLiteralPublic::RunTest(const FString&)
{
	UAILiveProjectRosterSubsystem* Roster = MakeTestRoster({TEXT("NPC02")});
	FAIL_SpeechPublicEvent Ev;
	Ev.ActorId = TEXT("NPC02");
	Ev.Seq = 500;
	Ev.Text = TEXT("hi");
	Ev.AddressedTo = {TEXT("public")};   // literal "public" must NOT enter the array

	FAIL_IngressRejectRequest Reject;
	const bool bOk = FAILiveProjectIngressValidator::ValidateSpeechPublic(Ev, *Roster, Reject);
	TestFalse(TEXT("literal 'public' rejected"), bOk);
	TestEqual(TEXT("reject_reason"), (uint8)Reject.RejectReason,
		(uint8)E_AIL_RejectReason::AudienceOutsideVisibility);
	return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAILiveIngressValidator_HappyPaths,
	"AILive.IngressValidator.HappyPaths",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FAILiveIngressValidator_HappyPaths::RunTest(const FString&)
{
	UAILiveProjectRosterSubsystem* Roster = MakeTestRoster({TEXT("NPC01"), TEXT("NPC02")});
	FAIL_IngressRejectRequest Reject;

	// wait
	TestTrue(TEXT("wait OK"), FAILiveProjectIngressValidator::ValidateActionIntent(
		MakeWaitIntent(TEXT("NPC01"), 1), *Roster, Reject));

	// move_to coords-only
	{
		FAIL_ActionIntentEvent Ev;
		Ev.ActorId = TEXT("NPC01");
		Ev.Seq = 2;
		Ev.Intent.ActionOntologyVersion = TEXT("1");
		Ev.Intent.Name = TEXT("move_to");
		Ev.Intent.MoveToParams.bHasCoords = true;
		Ev.Intent.MoveToParams.Coords.X = 100;
		TestTrue(TEXT("move_to coords OK"),
			FAILiveProjectIngressValidator::ValidateActionIntent(Ev, *Roster, Reject));
	}

	// move_to target_npc
	{
		FAIL_ActionIntentEvent Ev;
		Ev.ActorId = TEXT("NPC01");
		Ev.Seq = 3;
		Ev.Intent.ActionOntologyVersion = TEXT("1");
		Ev.Intent.Name = TEXT("move_to");
		Ev.Intent.MoveToParams.bHasTargetNpc = true;
		Ev.Intent.MoveToParams.TargetNpc = TEXT("NPC02");
		TestTrue(TEXT("move_to NPC02 OK"),
			FAILiveProjectIngressValidator::ValidateActionIntent(Ev, *Roster, Reject));
	}

	// sit
	{
		FAIL_ActionIntentEvent Ev;
		Ev.ActorId = TEXT("NPC01");
		Ev.Seq = 4;
		Ev.Intent.ActionOntologyVersion = TEXT("1");
		Ev.Intent.Name = TEXT("sit");
		Ev.Intent.SitParams.TargetSmartObject = TEXT("BP_SmartBench_Example");
		TestTrue(TEXT("sit OK"),
			FAILiveProjectIngressValidator::ValidateActionIntent(Ev, *Roster, Reject));
	}

	// speech happy
	{
		FAIL_SpeechPublicEvent Sp;
		Sp.ActorId = TEXT("NPC01");
		Sp.Seq = 5;
		Sp.Text = TEXT("hello");
		Sp.AddressedTo = {TEXT("NPC02")};
		TestTrue(TEXT("speech OK"),
			FAILiveProjectIngressValidator::ValidateSpeechPublic(Sp, *Roster, Reject));
	}

	// speech broadcast (empty addressed_to is allowed)
	{
		FAIL_SpeechPublicEvent Sp;
		Sp.ActorId = TEXT("NPC01");
		Sp.Seq = 6;
		Sp.Text = TEXT("hello");
		TestTrue(TEXT("speech broadcast OK"),
			FAILiveProjectIngressValidator::ValidateSpeechPublic(Sp, *Roster, Reject));
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
