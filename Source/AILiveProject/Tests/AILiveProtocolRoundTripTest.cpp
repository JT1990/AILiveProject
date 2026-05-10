#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "AILiveProtocolJson.h"
#include "AILiveProtocolTypes.h"
#include "HAL/FileManager.h"
#include "Internationalization/Regex.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace
{
	FString FixtureDir()
	{
		return FPaths::ProjectDir() / TEXT("Source/AILiveProject/Tests/Fixtures/protocol_examples/");
	}

	bool LoadFixture(const FString& Name, FString& Out)
	{
		const FString Path = FixtureDir() / Name;
		return FFileHelper::LoadFileToString(Out, *Path);
	}

	template <typename T>
	bool RoundTrip(FAutomationTestBase& Test, const FString& Fixture)
	{
		FString Json;
		if (!LoadFixture(Fixture, Json))
		{
			Test.AddError(FString::Printf(TEXT("LoadFixture failed: %s"), *Fixture));
			return false;
		}
		T Parsed;
		if (!AILiveProtocol::FromJsonString(Json, Parsed))
		{
			Test.AddError(FString::Printf(TEXT("FromJsonString failed: %s"), *Fixture));
			return false;
		}
		FString Reserialized;
		if (!AILiveProtocol::ToJsonString(Parsed, Reserialized))
		{
			Test.AddError(FString::Printf(TEXT("ToJsonString failed: %s"), *Fixture));
			return false;
		}
		if (!AILiveProtocol::JsonSemanticEqual(Json, Reserialized))
		{
			Test.AddError(FString::Printf(
				TEXT("JsonSemanticEqual failed: %s\n--- input ---\n%s\n--- output ---\n%s"),
				*Fixture, *Json, *Reserialized));
			return false;
		}
		return true;
	}

	bool RoundTripError(FAutomationTestBase& Test, const FString& Fixture)
	{
		return RoundTrip<FAIL_ErrorResponse>(Test, Fixture);
	}
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAILiveProtocolRoundTrip_Health,
	"AILive.Protocol.RoundTrip.Health",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FAILiveProtocolRoundTrip_Health::RunTest(const FString&)
{
	bool Ok = true;
	Ok &= RoundTrip<FAIL_HealthResponse>(*this, TEXT("health.success.json"));
	Ok &= RoundTripError(*this, TEXT("health.unavailable.json"));

	// Spot check: health.success status field round-trips correctly.
	FString Json;
	if (LoadFixture(TEXT("health.success.json"), Json))
	{
		FAIL_HealthResponse Parsed;
		AILiveProtocol::FromJsonString(Json, Parsed);
		TestEqual(TEXT("status"),           Parsed.Status,          FString(TEXT("ok")));
		TestEqual(TEXT("protocol_version"), Parsed.ProtocolVersion, FString(TEXT("0.1.1")));
	}
	return Ok;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAILiveProtocolRoundTrip_Session,
	"AILive.Protocol.RoundTrip.Session",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FAILiveProtocolRoundTrip_Session::RunTest(const FString&)
{
	bool Ok = true;
	Ok &= RoundTrip<FAIL_SessionCreateResponse>(*this, TEXT("session_create.success.json"));
	Ok &= RoundTripError(*this, TEXT("session_create.error.json"));
	return Ok;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAILiveProtocolRoundTrip_Roster,
	"AILive.Protocol.RoundTrip.Roster",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FAILiveProtocolRoundTrip_Roster::RunTest(const FString&)
{
	bool Ok = true;
	Ok &= RoundTrip<FAIL_RosterRegisterRequest>(*this, TEXT("roster_register.success.json"));
	Ok &= RoundTripError(*this, TEXT("roster_register.invalid_actor.json"));

	// Spot check: 10 roster entries with positions parse intact.
	FString Json;
	if (LoadFixture(TEXT("roster_register.success.json"), Json))
	{
		FAIL_RosterRegisterRequest Parsed;
		AILiveProtocol::FromJsonString(Json, Parsed);
		TestEqual(TEXT("roster.Num"), Parsed.Roster.Num(), 10);
		TestEqual(TEXT("roster[0].actor_id"),     Parsed.Roster[0].ActorId,     FString(TEXT("NPC01")));
		TestEqual(TEXT("roster[9].actor_id"),     Parsed.Roster[9].ActorId,     FString(TEXT("NPC10")));
		TestTrue (TEXT("roster[0].bHasInitialPosition"), Parsed.Roster[0].bHasInitialPosition);
	}
	return Ok;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAILiveProtocolRoundTrip_WorldState,
	"AILive.Protocol.RoundTrip.WorldState",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FAILiveProtocolRoundTrip_WorldState::RunTest(const FString&)
{
	bool Ok = true;
	Ok &= RoundTrip<FAIL_WorldStatePushRequest>(*this, TEXT("world_state_push.success.json"));
	Ok &= RoundTripError(*this, TEXT("world_state_push.missing_idempotency_key.json"));
	return Ok;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAILiveProtocolRoundTrip_ActionPull,
	"AILive.Protocol.RoundTrip.ActionPull",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FAILiveProtocolRoundTrip_ActionPull::RunTest(const FString&)
{
	bool Ok = true;
	Ok &= RoundTrip<FAIL_ActionPullResponse>(*this, TEXT("action_pull.success.json"));
	Ok &= RoundTrip<FAIL_ActionPullResponse>(*this, TEXT("action_pull.empty.json"));

	// Spot check: success fixture parses move_to and wait variants of the oneOf.
	FString Json;
	if (LoadFixture(TEXT("action_pull.success.json"), Json))
	{
		FAIL_ActionPullResponse Parsed;
		AILiveProtocol::FromJsonString(Json, Parsed);
		TestEqual(TEXT("events.Num"),       Parsed.Events.Num(),                          2);
		TestEqual(TEXT("events[0].name"),   Parsed.Events[0].Intent.Name,                 FString(TEXT("move_to")));
		TestTrue (TEXT("events[0].has_npc"), Parsed.Events[0].Intent.MoveToParams.bHasTargetNpc);
		TestEqual(TEXT("events[0].npc"),    Parsed.Events[0].Intent.MoveToParams.TargetNpc, FString(TEXT("NPC07")));
		TestEqual(TEXT("events[1].name"),   Parsed.Events[1].Intent.Name,                 FString(TEXT("wait")));
		TestTrue (TEXT("events[1].has_reason"), Parsed.Events[1].Intent.WaitParams.bHasReason);
	}
	return Ok;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAILiveProtocolRoundTrip_ActionResult,
	"AILive.Protocol.RoundTrip.ActionResult",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FAILiveProtocolRoundTrip_ActionResult::RunTest(const FString&)
{
	bool Ok = true;
	Ok &= RoundTrip<FAIL_ActionResultRequest>(*this, TEXT("action_result.success.json"));
	Ok &= RoundTripError(*this, TEXT("action_result.roster_not_registered.json"));
	return Ok;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAILiveProtocolRoundTrip_SpeechPull,
	"AILive.Protocol.RoundTrip.SpeechPull",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FAILiveProtocolRoundTrip_SpeechPull::RunTest(const FString&)
{
	bool Ok = true;
	Ok &= RoundTrip<FAIL_SpeechPullResponse>(*this, TEXT("speech_pull.success.json"));
	Ok &= RoundTrip<FAIL_SpeechPullResponse>(*this, TEXT("speech_pull.empty.json"));
	return Ok;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAILiveProtocolRoundTrip_SpeechResult,
	"AILive.Protocol.RoundTrip.SpeechResult",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FAILiveProtocolRoundTrip_SpeechResult::RunTest(const FString&)
{
	bool Ok = true;
	Ok &= RoundTrip<FAIL_SpeechResultRequest>(*this, TEXT("speech_result.success.json"));
	Ok &= RoundTripError(*this, TEXT("speech_result.stale_seq.json"));
	return Ok;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAILiveProtocolRoundTrip_IngressReject,
	"AILive.Protocol.RoundTrip.IngressReject",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FAILiveProtocolRoundTrip_IngressReject::RunTest(const FString&)
{
	bool Ok = true;
	Ok &= RoundTrip<FAIL_IngressRejectRequest>(*this, TEXT("ingress_reject.success.json"));
	Ok &= RoundTripError(*this, TEXT("ingress_reject.roster_not_registered.json"));
	return Ok;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAILiveProtocolRoundTrip_EventQuery,
	"AILive.Protocol.RoundTrip.EventQuery",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FAILiveProtocolRoundTrip_EventQuery::RunTest(const FString&)
{
	bool Ok = true;
	Ok &= RoundTrip<FAIL_EventQueryResponse>(*this, TEXT("event_query.success.json"));
	Ok &= RoundTripError(*this, TEXT("event_query.stale_seq.json"));

	// Critical: events array must deserialize as TArray<FAIL_EventMeta>, not strings.
	FString Json;
	if (LoadFixture(TEXT("event_query.success.json"), Json))
	{
		FAIL_EventQueryResponse Parsed;
		AILiveProtocol::FromJsonString(Json, Parsed);
		TestEqual(TEXT("events.Num"),                       Parsed.Events.Num(),         2);
		TestEqual(TEXT("events[0].event_type"),             Parsed.Events[0].EventType,  FString(TEXT("action.intent")));
		TestEqual(TEXT("events[0].seq"),                    Parsed.Events[0].Seq,        (int64)42);
		TestTrue (TEXT("events[0].has_parent_event_id"),    Parsed.Events[0].bHasParentEventId);
		TestEqual(TEXT("events[0].parent_event_id"),        Parsed.Events[0].ParentEventId, (int64)38);
		TestEqual(TEXT("events[1].event_type"),             Parsed.Events[1].EventType,  FString(TEXT("speech.public")));
	}
	return Ok;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAILiveProtocolRoundTrip_Helpers,
	"AILive.Protocol.RoundTrip.Helpers",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FAILiveProtocolRoundTrip_Helpers::RunTest(const FString&)
{
	const FString Key1 = AILiveProtocol::NewIdempotencyKey();
	const FString Key2 = AILiveProtocol::NewIdempotencyKey();
	TestNotEqual(TEXT("idempotency keys are unique"), Key1, Key2);
	// Idempotency-Key is just a unique string per HTTP call — brain does not
	// enforce v4 form. Validate basic UUID shape (8-4-4-4-12 hex, lowercase).
	const FRegexPattern UuidShape(TEXT("^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$"));
	{
		FRegexMatcher M(UuidShape, Key1);
		TestTrue(TEXT("Key1 has UUID shape"), M.FindNext());
	}
	{
		FRegexMatcher M(UuidShape, Key2);
		TestTrue(TEXT("Key2 has UUID shape"), M.FindNext());
	}
	// IsValidGameIdUuidV4 is for game_id validation (brain returns strict v4).
	TestFalse(TEXT("rejects garbage"),
		AILiveProtocol::IsValidGameIdUuidV4(TEXT("not-a-uuid")));
	TestTrue(TEXT("known good v4"),
		AILiveProtocol::IsValidGameIdUuidV4(TEXT("a1b2c3d4-e5f6-47ab-89cd-000000000001")));

	const FString Now = AILiveProtocol::IsoUtcNow();
	TestTrue(TEXT("iso ends with Z"), Now.EndsWith(TEXT("Z")));

	TestTrue(TEXT("semantic equal: same"),
		AILiveProtocol::JsonSemanticEqual(TEXT("{\"a\":1,\"b\":2}"), TEXT("{\"b\":2,\"a\":1}")));
	TestFalse(TEXT("semantic equal: different"),
		AILiveProtocol::JsonSemanticEqual(TEXT("{\"a\":1}"), TEXT("{\"a\":2}")));
	TestFalse(TEXT("array order matters"),
		AILiveProtocol::JsonSemanticEqual(TEXT("[1,2]"), TEXT("[2,1]")));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
