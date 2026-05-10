#include "AILiveProtocolJson.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/DateTime.h"
#include "Misc/Guid.h"
#include "Internationalization/Regex.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace AILiveProtocol
{
	// ---- helpers --------------------------------------------------------

	FString NewIdempotencyKey()
	{
		return FGuid::NewGuid()
			.ToString(EGuidFormats::DigitsWithHyphens)
			.ToLower();
	}

	FString IsoUtcNow()
	{
		return FDateTime::UtcNow().ToIso8601() + TEXT("Z");
	}

	bool IsValidGameIdUuidV4(const FString& In)
	{
		// UUID v4 form, lowercase: 8-4-4-4-12 with version nibble '4' and variant nibble in [89ab].
		const FRegexPattern Pattern(TEXT("^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$"));
		FRegexMatcher Matcher(Pattern, In);
		return Matcher.FindNext();
	}

	// ---- JsonSemanticEqual ---------------------------------------------

	static bool JsonValueEqual(const TSharedPtr<FJsonValue>& A, const TSharedPtr<FJsonValue>& B);

	static bool JsonObjectEqual(const TSharedPtr<FJsonObject>& A, const TSharedPtr<FJsonObject>& B)
	{
		if (!A.IsValid() || !B.IsValid()) { return A.IsValid() == B.IsValid(); }
		if (A->Values.Num() != B->Values.Num()) { return false; }
		for (const auto& Pair : A->Values)
		{
			const TSharedPtr<FJsonValue>* Other = B->Values.Find(Pair.Key);
			if (!Other) { return false; }
			if (!JsonValueEqual(Pair.Value, *Other)) { return false; }
		}
		return true;
	}

	static bool JsonArrayEqual(const TArray<TSharedPtr<FJsonValue>>& A, const TArray<TSharedPtr<FJsonValue>>& B)
	{
		if (A.Num() != B.Num()) { return false; }
		for (int32 i = 0; i < A.Num(); ++i)
		{
			if (!JsonValueEqual(A[i], B[i])) { return false; }
		}
		return true;
	}

	static bool JsonValueEqual(const TSharedPtr<FJsonValue>& A, const TSharedPtr<FJsonValue>& B)
	{
		if (!A.IsValid() || !B.IsValid()) { return A.IsValid() == B.IsValid(); }
		if (A->Type != B->Type)
		{
			// Numbers may surface as Number on either side; coerce.
			if (A->Type == EJson::Number && B->Type == EJson::Number) { /* fall through */ }
			else { return false; }
		}
		switch (A->Type)
		{
		case EJson::Null:    return B->Type == EJson::Null;
		case EJson::Boolean: return A->AsBool() == B->AsBool();
		case EJson::Number:
		{
			const double Da = A->AsNumber();
			const double Db = B->AsNumber();
			return FMath::IsNearlyEqual(Da, Db, 1e-9);
		}
		case EJson::String:  return A->AsString() == B->AsString();
		case EJson::Array:   return JsonArrayEqual(A->AsArray(), B->AsArray());
		case EJson::Object:  return JsonObjectEqual(A->AsObject(), B->AsObject());
		default:             return false;
		}
	}

	bool JsonSemanticEqual(const FString& A, const FString& B)
	{
		auto ParseAny = [](const FString& Text, TSharedPtr<FJsonValue>& Out) -> bool
		{
			// FJsonSerializer requires a top-level array or object; wrap to allow scalars too.
			const FString Wrapped = TEXT("{\"v\":") + Text + TEXT("}");
			TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Wrapped);
			TSharedPtr<FJsonObject> Obj;
			if (!FJsonSerializer::Deserialize(Reader, Obj) || !Obj.IsValid()) { return false; }
			Out = Obj->TryGetField(TEXT("v"));
			return Out.IsValid();
		};
		TSharedPtr<FJsonValue> Va, Vb;
		if (!ParseAny(A, Va) || !ParseAny(B, Vb)) { return false; }
		return JsonValueEqual(Va, Vb);
	}

	// ---- enum <-> wire string ------------------------------------------

	FString CurrentActionToWire(E_AIL_NpcAction In)
	{
		switch (In)
		{
		case E_AIL_NpcAction::Idle:     return TEXT("idle");
		case E_AIL_NpcAction::Moving:   return TEXT("moving");
		case E_AIL_NpcAction::Sitting:  return TEXT("sitting");
		case E_AIL_NpcAction::Speaking: return TEXT("speaking");
		}
		return TEXT("idle");
	}

	bool WireToCurrentAction(const FString& In, E_AIL_NpcAction& Out)
	{
		if (In == TEXT("idle"))     { Out = E_AIL_NpcAction::Idle;     return true; }
		if (In == TEXT("moving"))   { Out = E_AIL_NpcAction::Moving;   return true; }
		if (In == TEXT("sitting"))  { Out = E_AIL_NpcAction::Sitting;  return true; }
		if (In == TEXT("speaking")) { Out = E_AIL_NpcAction::Speaking; return true; }
		return false;
	}

	FString OutcomeToWire(E_AIL_ActionOutcome In)
	{
		return In == E_AIL_ActionOutcome::Succeeded ? TEXT("succeeded") : TEXT("failed");
	}

	bool WireToOutcome(const FString& In, E_AIL_ActionOutcome& Out)
	{
		if (In == TEXT("succeeded")) { Out = E_AIL_ActionOutcome::Succeeded; return true; }
		if (In == TEXT("failed"))    { Out = E_AIL_ActionOutcome::Failed;    return true; }
		return false;
	}

	FString SpeechStatusToWire(E_AIL_SpeechStatus In)
	{
		return In == E_AIL_SpeechStatus::Succeeded ? TEXT("succeeded") : TEXT("failed");
	}

	bool WireToSpeechStatus(const FString& In, E_AIL_SpeechStatus& Out)
	{
		if (In == TEXT("succeeded")) { Out = E_AIL_SpeechStatus::Succeeded; return true; }
		if (In == TEXT("failed"))    { Out = E_AIL_SpeechStatus::Failed;    return true; }
		return false;
	}

	FString RejectReasonToWire(E_AIL_RejectReason In)
	{
		switch (In)
		{
		case E_AIL_RejectReason::ActorNotInRoster:          return TEXT("ACTOR_NOT_IN_ROSTER");
		case E_AIL_RejectReason::AudienceOutsideVisibility: return TEXT("AUDIENCE_OUTSIDE_VISIBILITY");
		case E_AIL_RejectReason::IntentNotInOntology:       return TEXT("INTENT_NOT_IN_ONTOLOGY");
		case E_AIL_RejectReason::SentenceTooLong:           return TEXT("SENTENCE_TOO_LONG");
		}
		return TEXT("ACTOR_NOT_IN_ROSTER");
	}

	bool WireToRejectReason(const FString& In, E_AIL_RejectReason& Out)
	{
		if (In == TEXT("ACTOR_NOT_IN_ROSTER"))          { Out = E_AIL_RejectReason::ActorNotInRoster;          return true; }
		if (In == TEXT("AUDIENCE_OUTSIDE_VISIBILITY"))  { Out = E_AIL_RejectReason::AudienceOutsideVisibility; return true; }
		if (In == TEXT("INTENT_NOT_IN_ONTOLOGY"))       { Out = E_AIL_RejectReason::IntentNotInOntology;       return true; }
		if (In == TEXT("SENTENCE_TOO_LONG"))            { Out = E_AIL_RejectReason::SentenceTooLong;           return true; }
		return false;
	}

	// ---- low-level helpers --------------------------------------------

	static FString SerializeObject(const TSharedRef<FJsonObject>& Obj)
	{
		FString Out;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(Obj, Writer);
		return Out;
	}

	static bool ParseObject(const FString& In, TSharedPtr<FJsonObject>& Out)
	{
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(In);
		return FJsonSerializer::Deserialize(Reader, Out) && Out.IsValid();
	}

	bool ToJson_Position3D(const FAIL_Position3D& In, TSharedRef<FJsonObject> Out)
	{
		Out->SetNumberField(TEXT("x"), In.X);
		Out->SetNumberField(TEXT("y"), In.Y);
		Out->SetNumberField(TEXT("z"), In.Z);
		return true;
	}

	bool FromJson_Position3D(const TSharedRef<FJsonObject>& In, FAIL_Position3D& Out)
	{
		double X = 0, Y = 0, Z = 0;
		if (!In->TryGetNumberField(TEXT("x"), X)) { return false; }
		if (!In->TryGetNumberField(TEXT("y"), Y)) { return false; }
		if (!In->TryGetNumberField(TEXT("z"), Z)) { return false; }
		Out.X = X; Out.Y = Y; Out.Z = Z;
		return true;
	}

	static TSharedRef<FJsonObject> Position3DToJsonRef(const FAIL_Position3D& In)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		ToJson_Position3D(In, Obj);
		return Obj;
	}

	static TArray<TSharedPtr<FJsonValue>> StringArrayToJson(const TArray<FString>& In)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		Out.Reserve(In.Num());
		for (const FString& S : In) { Out.Add(MakeShared<FJsonValueString>(S)); }
		return Out;
	}

	static bool JsonToStringArray(const TArray<TSharedPtr<FJsonValue>>& In, TArray<FString>& Out)
	{
		Out.Reset();
		for (const TSharedPtr<FJsonValue>& V : In)
		{
			if (!V.IsValid() || V->Type != EJson::String) { return false; }
			Out.Add(V->AsString());
		}
		return true;
	}

	// ---- HealthResponse -----------------------------------------------

	bool ToJsonString(const FAIL_HealthResponse& In, FString& Out)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("protocol_version"), In.ProtocolVersion);
		Obj->SetStringField(TEXT("server_time"),      In.ServerTime);
		Obj->SetStringField(TEXT("status"),           In.Status);
		Out = SerializeObject(Obj);
		return true;
	}

	bool FromJsonString(const FString& In, FAIL_HealthResponse& Out)
	{
		TSharedPtr<FJsonObject> Obj;
		if (!ParseObject(In, Obj)) { return false; }
		if (!Obj->TryGetStringField(TEXT("status"),           Out.Status))          { return false; }
		if (!Obj->TryGetStringField(TEXT("protocol_version"), Out.ProtocolVersion)) { return false; }
		if (!Obj->TryGetStringField(TEXT("server_time"),      Out.ServerTime))      { return false; }
		return true;
	}

	// ---- SessionCreateRequest -----------------------------------------

	bool ToJsonString(const FAIL_SessionCreateRequest& In, FString& Out)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		if (In.bHasClientLabel) { Obj->SetStringField(TEXT("client_label"), In.ClientLabel); }
		Out = SerializeObject(Obj);
		return true;
	}

	bool FromJsonString(const FString& In, FAIL_SessionCreateRequest& Out)
	{
		TSharedPtr<FJsonObject> Obj;
		if (!ParseObject(In, Obj)) { return false; }
		FString ClientLabel;
		if (Obj->TryGetStringField(TEXT("client_label"), ClientLabel))
		{
			Out.bHasClientLabel = true;
			Out.ClientLabel = ClientLabel;
		}
		return true;
	}

	// ---- SessionCreateResponse ----------------------------------------

	bool ToJsonString(const FAIL_SessionCreateResponse& In, FString& Out)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("game_id"),          In.GameId);
		Obj->SetStringField(TEXT("protocol_version"), In.ProtocolVersion);
		Obj->SetStringField(TEXT("server_time"),      In.ServerTime);
		Out = SerializeObject(Obj);
		return true;
	}

	bool FromJsonString(const FString& In, FAIL_SessionCreateResponse& Out)
	{
		TSharedPtr<FJsonObject> Obj;
		if (!ParseObject(In, Obj)) { return false; }
		if (!Obj->TryGetStringField(TEXT("game_id"),          Out.GameId))          { return false; }
		if (!Obj->TryGetStringField(TEXT("protocol_version"), Out.ProtocolVersion)) { return false; }
		if (!Obj->TryGetStringField(TEXT("server_time"),      Out.ServerTime))      { return false; }
		return true;
	}

	// ---- RosterRegisterRequest ----------------------------------------

	bool ToJsonString(const FAIL_RosterRegisterRequest& In, FString& Out)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> Arr;
		Arr.Reserve(In.Roster.Num());
		for (const FAIL_NPCEntry& Entry : In.Roster)
		{
			TSharedRef<FJsonObject> EntryObj = MakeShared<FJsonObject>();
			EntryObj->SetStringField(TEXT("actor_id"),     Entry.ActorId);
			EntryObj->SetStringField(TEXT("display_name"), Entry.DisplayName);
			if (Entry.bHasInitialPosition)
			{
				EntryObj->SetObjectField(TEXT("initial_position"), Position3DToJsonRef(Entry.InitialPosition));
			}
			Arr.Add(MakeShared<FJsonValueObject>(EntryObj));
		}
		Obj->SetArrayField(TEXT("roster"), Arr);
		Out = SerializeObject(Obj);
		return true;
	}

	bool FromJsonString(const FString& In, FAIL_RosterRegisterRequest& Out)
	{
		TSharedPtr<FJsonObject> Obj;
		if (!ParseObject(In, Obj)) { return false; }
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (!Obj->TryGetArrayField(TEXT("roster"), Arr)) { return false; }
		Out.Roster.Reset();
		for (const TSharedPtr<FJsonValue>& V : *Arr)
		{
			if (!V.IsValid() || V->Type != EJson::Object) { return false; }
			const TSharedPtr<FJsonObject>& EntryObj = V->AsObject();
			FAIL_NPCEntry Entry;
			if (!EntryObj->TryGetStringField(TEXT("actor_id"),     Entry.ActorId))     { return false; }
			if (!EntryObj->TryGetStringField(TEXT("display_name"), Entry.DisplayName)) { return false; }
			const TSharedPtr<FJsonObject>* PosObj = nullptr;
			if (EntryObj->TryGetObjectField(TEXT("initial_position"), PosObj) && PosObj && PosObj->IsValid())
			{
				if (!FromJson_Position3D(PosObj->ToSharedRef(), Entry.InitialPosition)) { return false; }
				Entry.bHasInitialPosition = true;
			}
			Out.Roster.Add(MoveTemp(Entry));
		}
		return true;
	}

	// ---- RosterRegisterResponse ---------------------------------------

	bool ToJsonString(const FAIL_RosterRegisterResponse& In, FString& Out)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(TEXT("accepted_count"), In.AcceptedCount);
		Obj->SetStringField(TEXT("game_id"),        In.GameId);
		Out = SerializeObject(Obj);
		return true;
	}

	bool FromJsonString(const FString& In, FAIL_RosterRegisterResponse& Out)
	{
		TSharedPtr<FJsonObject> Obj;
		if (!ParseObject(In, Obj)) { return false; }
		double Count = 0;
		if (!Obj->TryGetNumberField(TEXT("accepted_count"), Count)) { return false; }
		Out.AcceptedCount = static_cast<int32>(Count);
		if (!Obj->TryGetStringField(TEXT("game_id"), Out.GameId))   { return false; }
		return true;
	}

	// ---- WorldStatePushRequest ---------------------------------------

	bool ToJsonString(const FAIL_WorldStatePushRequest& In, FString& Out)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(TEXT("client_sample_id"),     In.ClientSampleId);
		Obj->SetStringField(TEXT("sample_wall_clock_ts"), In.SampleWallClockTs);

		TArray<TSharedPtr<FJsonValue>> ObsArr;
		ObsArr.Reserve(In.Observations.Num());
		for (const FAIL_NpcObservation& Obs : In.Observations)
		{
			TSharedRef<FJsonObject> ObsObj = MakeShared<FJsonObject>();
			ObsObj->SetStringField(TEXT("actor_id"),       Obs.ActorId);
			ObsObj->SetStringField(TEXT("current_action"), CurrentActionToWire(Obs.CurrentAction));
			ObsObj->SetNumberField(TEXT("facing_degrees"), Obs.FacingDegrees);
			ObsObj->SetObjectField(TEXT("position"),       Position3DToJsonRef(Obs.Position));

			TArray<TSharedPtr<FJsonValue>> SightArr;
			for (const FAIL_SightEntry& S : Obs.SightedActors)
			{
				TSharedRef<FJsonObject> SObj = MakeShared<FJsonObject>();
				SObj->SetNumberField(TEXT("distance_cm"),     S.DistanceCm);
				SObj->SetNumberField(TEXT("rel_yaw_degrees"), S.RelYawDegrees);
				SObj->SetStringField(TEXT("target_actor_id"), S.TargetActorId);
				SightArr.Add(MakeShared<FJsonValueObject>(SObj));
			}
			ObsObj->SetArrayField(TEXT("sighted_actors"), SightArr);

			TArray<TSharedPtr<FJsonValue>> HearArr;
			for (const FAIL_HeardSound& H : Obs.HeardSounds)
			{
				TSharedRef<FJsonObject> HObj = MakeShared<FJsonObject>();
				HObj->SetNumberField(TEXT("age_seconds"),     H.AgeSeconds);
				HObj->SetNumberField(TEXT("loudness"),        H.Loudness);
				HObj->SetNumberField(TEXT("rel_yaw_degrees"), H.RelYawDegrees);
				HObj->SetStringField(TEXT("source_actor_id"), H.SourceActorId);
				HearArr.Add(MakeShared<FJsonValueObject>(HObj));
			}
			ObsObj->SetArrayField(TEXT("heard_sounds"), HearArr);

			ObsArr.Add(MakeShared<FJsonValueObject>(ObsObj));
		}
		Obj->SetArrayField(TEXT("observations"), ObsArr);

		Out = SerializeObject(Obj);
		return true;
	}

	bool FromJsonString(const FString& In, FAIL_WorldStatePushRequest& Out)
	{
		TSharedPtr<FJsonObject> Obj;
		if (!ParseObject(In, Obj)) { return false; }
		double Csid = 0;
		if (!Obj->TryGetNumberField(TEXT("client_sample_id"), Csid)) { return false; }
		Out.ClientSampleId = static_cast<int32>(Csid);
		if (!Obj->TryGetStringField(TEXT("sample_wall_clock_ts"), Out.SampleWallClockTs)) { return false; }

		const TArray<TSharedPtr<FJsonValue>>* ObsArr = nullptr;
		if (!Obj->TryGetArrayField(TEXT("observations"), ObsArr)) { return false; }
		Out.Observations.Reset();
		for (const TSharedPtr<FJsonValue>& V : *ObsArr)
		{
			if (!V.IsValid() || V->Type != EJson::Object) { return false; }
			const TSharedPtr<FJsonObject>& O = V->AsObject();
			FAIL_NpcObservation Obs;
			if (!O->TryGetStringField(TEXT("actor_id"), Obs.ActorId)) { return false; }
			FString Action;
			if (!O->TryGetStringField(TEXT("current_action"), Action) ||
				!WireToCurrentAction(Action, Obs.CurrentAction)) { return false; }
			double Facing = 0;
			if (!O->TryGetNumberField(TEXT("facing_degrees"), Facing)) { return false; }
			Obs.FacingDegrees = Facing;
			const TSharedPtr<FJsonObject>* PosObj = nullptr;
			if (!O->TryGetObjectField(TEXT("position"), PosObj) || !PosObj || !PosObj->IsValid()) { return false; }
			if (!FromJson_Position3D(PosObj->ToSharedRef(), Obs.Position)) { return false; }

			const TArray<TSharedPtr<FJsonValue>>* Sight = nullptr;
			if (O->TryGetArrayField(TEXT("sighted_actors"), Sight))
			{
				for (const TSharedPtr<FJsonValue>& SV : *Sight)
				{
					if (!SV.IsValid() || SV->Type != EJson::Object) { return false; }
					const TSharedPtr<FJsonObject>& SO = SV->AsObject();
					FAIL_SightEntry S;
					double D = 0, Yaw = 0;
					if (!SO->TryGetNumberField(TEXT("distance_cm"),     D))   { return false; }
					if (!SO->TryGetNumberField(TEXT("rel_yaw_degrees"), Yaw)) { return false; }
					if (!SO->TryGetStringField(TEXT("target_actor_id"), S.TargetActorId)) { return false; }
					S.DistanceCm = D; S.RelYawDegrees = Yaw;
					Obs.SightedActors.Add(S);
				}
			}
			const TArray<TSharedPtr<FJsonValue>>* Heard = nullptr;
			if (O->TryGetArrayField(TEXT("heard_sounds"), Heard))
			{
				for (const TSharedPtr<FJsonValue>& HV : *Heard)
				{
					if (!HV.IsValid() || HV->Type != EJson::Object) { return false; }
					const TSharedPtr<FJsonObject>& HO = HV->AsObject();
					FAIL_HeardSound H;
					double Age = 0, Loud = 0, Yaw = 0;
					if (!HO->TryGetNumberField(TEXT("age_seconds"),     Age))  { return false; }
					if (!HO->TryGetNumberField(TEXT("loudness"),        Loud)) { return false; }
					if (!HO->TryGetNumberField(TEXT("rel_yaw_degrees"), Yaw))  { return false; }
					if (!HO->TryGetStringField(TEXT("source_actor_id"), H.SourceActorId)) { return false; }
					H.AgeSeconds = Age; H.Loudness = Loud; H.RelYawDegrees = Yaw;
					Obs.HeardSounds.Add(H);
				}
			}
			Out.Observations.Add(MoveTemp(Obs));
		}
		return true;
	}

	// ---- OntologyV1Intent (oneOf) -------------------------------------

	static TSharedRef<FJsonObject> IntentToJsonObject(const FAIL_OntologyV1Intent& In)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("action_ontology_version"), In.ActionOntologyVersion);
		Obj->SetStringField(TEXT("name"), In.Name);
		if (In.Name == TEXT("move_to"))
		{
			TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
			if (In.MoveToParams.bHasCoords)
			{
				Params->SetObjectField(TEXT("coords"), Position3DToJsonRef(In.MoveToParams.Coords));
			}
			if (In.MoveToParams.bHasTargetNpc)
			{
				Params->SetStringField(TEXT("target_npc"), In.MoveToParams.TargetNpc);
			}
			if (In.MoveToParams.bHasTargetZone)
			{
				Params->SetStringField(TEXT("target_zone"), In.MoveToParams.TargetZone);
			}
			Obj->SetObjectField(TEXT("params"), Params);
		}
		else if (In.Name == TEXT("sit"))
		{
			TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
			Params->SetStringField(TEXT("target_smartobject"), In.SitParams.TargetSmartObject);
			Obj->SetObjectField(TEXT("params"), Params);
		}
		else if (In.Name == TEXT("wait"))
		{
			if (In.WaitParams.bHasParams)
			{
				TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
				if (In.WaitParams.bHasReason)
				{
					Params->SetStringField(TEXT("reason"), In.WaitParams.Reason);
				}
				Obj->SetObjectField(TEXT("params"), Params);
			}
		}
		return Obj;
	}

	static bool IntentFromJsonObject(const TSharedRef<FJsonObject>& In, FAIL_OntologyV1Intent& Out)
	{
		if (!In->TryGetStringField(TEXT("action_ontology_version"), Out.ActionOntologyVersion)) { return false; }
		if (!In->TryGetStringField(TEXT("name"),                    Out.Name))                  { return false; }
		const TSharedPtr<FJsonObject>* ParamsObj = nullptr;
		const bool bHasParamsField = In->TryGetObjectField(TEXT("params"), ParamsObj) && ParamsObj && ParamsObj->IsValid();

		if (Out.Name == TEXT("move_to"))
		{
			if (!bHasParamsField) { return false; }
			const TSharedPtr<FJsonObject>* CoordsObj = nullptr;
			if ((*ParamsObj)->TryGetObjectField(TEXT("coords"), CoordsObj) && CoordsObj && CoordsObj->IsValid())
			{
				if (!FromJson_Position3D(CoordsObj->ToSharedRef(), Out.MoveToParams.Coords)) { return false; }
				Out.MoveToParams.bHasCoords = true;
			}
			FString Tn;
			if ((*ParamsObj)->TryGetStringField(TEXT("target_npc"), Tn))
			{
				Out.MoveToParams.TargetNpc = Tn;
				Out.MoveToParams.bHasTargetNpc = true;
			}
			FString Tz;
			if ((*ParamsObj)->TryGetStringField(TEXT("target_zone"), Tz))
			{
				Out.MoveToParams.TargetZone = Tz;
				Out.MoveToParams.bHasTargetZone = true;
			}
		}
		else if (Out.Name == TEXT("sit"))
		{
			if (!bHasParamsField) { return false; }
			if (!(*ParamsObj)->TryGetStringField(TEXT("target_smartobject"),
				Out.SitParams.TargetSmartObject)) { return false; }
		}
		else if (Out.Name == TEXT("wait"))
		{
			if (bHasParamsField)
			{
				Out.WaitParams.bHasParams = true;
				FString Reason;
				if ((*ParamsObj)->TryGetStringField(TEXT("reason"), Reason))
				{
					Out.WaitParams.Reason = Reason;
					Out.WaitParams.bHasReason = true;
				}
			}
		}
		else
		{
			return false;
		}
		return true;
	}

	// ---- ActionPullResponse -----------------------------------------

	bool ToJsonString(const FAIL_ActionPullResponse& In, FString& Out)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> EvArr;
		for (const FAIL_ActionIntentEvent& Ev : In.Events)
		{
			TSharedRef<FJsonObject> EvObj = MakeShared<FJsonObject>();
			EvObj->SetStringField(TEXT("actor_id"), Ev.ActorId);
			EvObj->SetObjectField(TEXT("intent"),   IntentToJsonObject(Ev.Intent));
			EvObj->SetNumberField(TEXT("seq"),      static_cast<double>(Ev.Seq));
			EvArr.Add(MakeShared<FJsonValueObject>(EvObj));
		}
		Obj->SetArrayField(TEXT("events"),       EvArr);
		Obj->SetNumberField(TEXT("next_cursor"), static_cast<double>(In.NextCursor));
		Out = SerializeObject(Obj);
		return true;
	}

	bool FromJsonString(const FString& In, FAIL_ActionPullResponse& Out)
	{
		TSharedPtr<FJsonObject> Obj;
		if (!ParseObject(In, Obj)) { return false; }
		const TArray<TSharedPtr<FJsonValue>>* EvArr = nullptr;
		if (!Obj->TryGetArrayField(TEXT("events"), EvArr)) { return false; }
		Out.Events.Reset();
		for (const TSharedPtr<FJsonValue>& V : *EvArr)
		{
			if (!V.IsValid() || V->Type != EJson::Object) { return false; }
			const TSharedPtr<FJsonObject>& EvObj = V->AsObject();
			FAIL_ActionIntentEvent Ev;
			if (!EvObj->TryGetStringField(TEXT("actor_id"), Ev.ActorId)) { return false; }
			double Seq = 0;
			if (!EvObj->TryGetNumberField(TEXT("seq"), Seq)) { return false; }
			Ev.Seq = static_cast<int64>(Seq);
			const TSharedPtr<FJsonObject>* IntentObj = nullptr;
			if (!EvObj->TryGetObjectField(TEXT("intent"), IntentObj) || !IntentObj || !IntentObj->IsValid()) { return false; }
			if (!IntentFromJsonObject(IntentObj->ToSharedRef(), Ev.Intent)) { return false; }
			Out.Events.Add(MoveTemp(Ev));
		}
		double Cursor = 0;
		if (!Obj->TryGetNumberField(TEXT("next_cursor"), Cursor)) { return false; }
		Out.NextCursor = static_cast<int64>(Cursor);
		return true;
	}

	// ---- ActionResultRequest ----------------------------------------

	bool ToJsonString(const FAIL_ActionResultRequest& In, FString& Out)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("actor_id"),       In.ActorId);
		Obj->SetNumberField(TEXT("duration_ms"),    In.DurationMs);
		if (In.bHasErrorReason) { Obj->SetStringField(TEXT("error_reason"), In.ErrorReason); }
		Obj->SetObjectField(TEXT("final_position"), Position3DToJsonRef(In.FinalPosition));
		Obj->SetNumberField(TEXT("intent_seq"),     static_cast<double>(In.IntentSeq));
		Obj->SetStringField(TEXT("outcome"),        OutcomeToWire(In.Outcome));
		Out = SerializeObject(Obj);
		return true;
	}

	bool FromJsonString(const FString& In, FAIL_ActionResultRequest& Out)
	{
		TSharedPtr<FJsonObject> Obj;
		if (!ParseObject(In, Obj)) { return false; }
		if (!Obj->TryGetStringField(TEXT("actor_id"), Out.ActorId)) { return false; }
		double Dur = 0; if (!Obj->TryGetNumberField(TEXT("duration_ms"), Dur)) { return false; }
		Out.DurationMs = static_cast<int32>(Dur);
		FString Err;
		if (Obj->TryGetStringField(TEXT("error_reason"), Err))
		{
			Out.bHasErrorReason = true;
			Out.ErrorReason = Err;
		}
		const TSharedPtr<FJsonObject>* Pos = nullptr;
		if (!Obj->TryGetObjectField(TEXT("final_position"), Pos) || !Pos || !Pos->IsValid()) { return false; }
		if (!FromJson_Position3D(Pos->ToSharedRef(), Out.FinalPosition)) { return false; }
		double Seq = 0; if (!Obj->TryGetNumberField(TEXT("intent_seq"), Seq)) { return false; }
		Out.IntentSeq = static_cast<int64>(Seq);
		FString Outcome;
		if (!Obj->TryGetStringField(TEXT("outcome"), Outcome) ||
			!WireToOutcome(Outcome, Out.Outcome)) { return false; }
		return true;
	}

	// ---- SpeechPullResponse ----------------------------------------

	bool ToJsonString(const FAIL_SpeechPullResponse& In, FString& Out)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> EvArr;
		for (const FAIL_SpeechPublicEvent& Ev : In.Events)
		{
			TSharedRef<FJsonObject> EvObj = MakeShared<FJsonObject>();
			EvObj->SetStringField(TEXT("actor_id"),   Ev.ActorId);
			EvObj->SetArrayField(TEXT("addressed_to"), StringArrayToJson(Ev.AddressedTo));
			EvObj->SetNumberField(TEXT("seq"),         static_cast<double>(Ev.Seq));
			EvObj->SetStringField(TEXT("text"),        Ev.Text);
			EvArr.Add(MakeShared<FJsonValueObject>(EvObj));
		}
		Obj->SetArrayField(TEXT("events"),       EvArr);
		Obj->SetNumberField(TEXT("next_cursor"), static_cast<double>(In.NextCursor));
		Out = SerializeObject(Obj);
		return true;
	}

	bool FromJsonString(const FString& In, FAIL_SpeechPullResponse& Out)
	{
		TSharedPtr<FJsonObject> Obj;
		if (!ParseObject(In, Obj)) { return false; }
		const TArray<TSharedPtr<FJsonValue>>* EvArr = nullptr;
		if (!Obj->TryGetArrayField(TEXT("events"), EvArr)) { return false; }
		Out.Events.Reset();
		for (const TSharedPtr<FJsonValue>& V : *EvArr)
		{
			if (!V.IsValid() || V->Type != EJson::Object) { return false; }
			const TSharedPtr<FJsonObject>& EvObj = V->AsObject();
			FAIL_SpeechPublicEvent Ev;
			if (!EvObj->TryGetStringField(TEXT("actor_id"), Ev.ActorId)) { return false; }
			const TArray<TSharedPtr<FJsonValue>>* Addr = nullptr;
			if (!EvObj->TryGetArrayField(TEXT("addressed_to"), Addr) ||
				!JsonToStringArray(*Addr, Ev.AddressedTo)) { return false; }
			double Seq = 0; if (!EvObj->TryGetNumberField(TEXT("seq"), Seq)) { return false; }
			Ev.Seq = static_cast<int64>(Seq);
			if (!EvObj->TryGetStringField(TEXT("text"), Ev.Text)) { return false; }
			Out.Events.Add(MoveTemp(Ev));
		}
		double Cursor = 0;
		if (!Obj->TryGetNumberField(TEXT("next_cursor"), Cursor)) { return false; }
		Out.NextCursor = static_cast<int64>(Cursor);
		return true;
	}

	// ---- SpeechResultRequest --------------------------------------

	bool ToJsonString(const FAIL_SpeechResultRequest& In, FString& Out)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("actor_id"),    In.ActorId);
		Obj->SetNumberField(TEXT("duration_ms"), In.DurationMs);
		if (In.bHasErrorReason) { Obj->SetStringField(TEXT("error_reason"), In.ErrorReason); }
		Obj->SetNumberField(TEXT("speech_seq"),  static_cast<double>(In.SpeechSeq));
		Obj->SetStringField(TEXT("status"),      SpeechStatusToWire(In.Status));
		Out = SerializeObject(Obj);
		return true;
	}

	bool FromJsonString(const FString& In, FAIL_SpeechResultRequest& Out)
	{
		TSharedPtr<FJsonObject> Obj;
		if (!ParseObject(In, Obj)) { return false; }
		if (!Obj->TryGetStringField(TEXT("actor_id"), Out.ActorId)) { return false; }
		double Dur = 0; if (!Obj->TryGetNumberField(TEXT("duration_ms"), Dur)) { return false; }
		Out.DurationMs = static_cast<int32>(Dur);
		FString Err;
		if (Obj->TryGetStringField(TEXT("error_reason"), Err))
		{
			Out.bHasErrorReason = true;
			Out.ErrorReason = Err;
		}
		double Seq = 0; if (!Obj->TryGetNumberField(TEXT("speech_seq"), Seq)) { return false; }
		Out.SpeechSeq = static_cast<int64>(Seq);
		FString Status;
		if (!Obj->TryGetStringField(TEXT("status"), Status) ||
			!WireToSpeechStatus(Status, Out.Status)) { return false; }
		return true;
	}

	// ---- IngressRejectRequest -------------------------------------

	bool ToJsonString(const FAIL_IngressRejectRequest& In, FString& Out)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("actor_id"),            In.ActorId);
		Obj->SetNumberField(TEXT("original_seq"),        static_cast<double>(In.OriginalSeq));
		Obj->SetStringField(TEXT("raw_message_snippet"), In.RawMessageSnippet);
		Obj->SetStringField(TEXT("reject_reason"),       RejectReasonToWire(In.RejectReason));
		Out = SerializeObject(Obj);
		return true;
	}

	bool FromJsonString(const FString& In, FAIL_IngressRejectRequest& Out)
	{
		TSharedPtr<FJsonObject> Obj;
		if (!ParseObject(In, Obj)) { return false; }
		if (!Obj->TryGetStringField(TEXT("actor_id"), Out.ActorId)) { return false; }
		double Seq = 0; if (!Obj->TryGetNumberField(TEXT("original_seq"), Seq)) { return false; }
		Out.OriginalSeq = static_cast<int64>(Seq);
		if (!Obj->TryGetStringField(TEXT("raw_message_snippet"), Out.RawMessageSnippet)) { return false; }
		FString Reason;
		if (!Obj->TryGetStringField(TEXT("reject_reason"), Reason) ||
			!WireToRejectReason(Reason, Out.RejectReason)) { return false; }
		return true;
	}

	// ---- ErrorResponse ----------------------------------------------

	static TSharedRef<FJsonObject> ErrorDetailsToJson(const FAIL_ErrorDetails& In)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		if (In.bHasCurrentMinSeq)    { Obj->SetNumberField(TEXT("current_min_seq"),     static_cast<double>(In.CurrentMinSeq)); }
		if (In.bHasField)            { Obj->SetStringField(TEXT("field"),               In.Field); }
		if (In.bHasHint)             { Obj->SetStringField(TEXT("hint"),                In.Hint); }
		if (In.bHasProvidedSeq)      { Obj->SetNumberField(TEXT("provided_seq"),        static_cast<double>(In.ProvidedSeq)); }
		if (In.bHasProvidedSinceSeq) { Obj->SetNumberField(TEXT("provided_since_seq"),  static_cast<double>(In.ProvidedSinceSeq)); }
		if (In.bHasReceived)         { Obj->SetStringField(TEXT("received"),            In.Received); }
		return Obj;
	}

	static bool ErrorDetailsFromJson(const TSharedRef<FJsonObject>& In, FAIL_ErrorDetails& Out)
	{
		double N = 0;
		if (In->TryGetNumberField(TEXT("current_min_seq"), N))
		{
			Out.bHasCurrentMinSeq = true;
			Out.CurrentMinSeq = static_cast<int64>(N);
		}
		FString S;
		if (In->TryGetStringField(TEXT("field"), S)) { Out.bHasField = true; Out.Field = S; }
		if (In->TryGetStringField(TEXT("hint"),  S)) { Out.bHasHint  = true; Out.Hint  = S; }
		if (In->TryGetNumberField(TEXT("provided_seq"), N))
		{
			Out.bHasProvidedSeq = true;
			Out.ProvidedSeq = static_cast<int64>(N);
		}
		if (In->TryGetNumberField(TEXT("provided_since_seq"), N))
		{
			Out.bHasProvidedSinceSeq = true;
			Out.ProvidedSinceSeq = static_cast<int64>(N);
		}
		if (In->TryGetStringField(TEXT("received"), S)) { Out.bHasReceived = true; Out.Received = S; }
		return true;
	}

	bool ToJsonString(const FAIL_ErrorResponse& In, FString& Out)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		if (In.bHasDetails)
		{
			Obj->SetObjectField(TEXT("details"), ErrorDetailsToJson(In.Details));
		}
		Obj->SetStringField(TEXT("error_code"), In.ErrorCode);
		Obj->SetStringField(TEXT("message"),    In.Message);
		Obj->SetBoolField  (TEXT("retryable"),  In.Retryable);
		Out = SerializeObject(Obj);
		return true;
	}

	bool FromJsonString(const FString& In, FAIL_ErrorResponse& Out)
	{
		TSharedPtr<FJsonObject> Obj;
		if (!ParseObject(In, Obj)) { return false; }
		if (!Obj->TryGetStringField(TEXT("error_code"), Out.ErrorCode)) { return false; }
		if (!Obj->TryGetStringField(TEXT("message"),    Out.Message))   { return false; }
		if (!Obj->TryGetBoolField  (TEXT("retryable"),  Out.Retryable)) { return false; }
		const TSharedPtr<FJsonObject>* DetailsObj = nullptr;
		if (Obj->TryGetObjectField(TEXT("details"), DetailsObj) && DetailsObj && DetailsObj->IsValid())
		{
			Out.bHasDetails = true;
			if (!ErrorDetailsFromJson(DetailsObj->ToSharedRef(), Out.Details)) { return false; }
		}
		return true;
	}

	// ---- EventQueryResponse / EventMeta ---------------------------

	static TSharedRef<FJsonObject> EventMetaToJsonObject(const FAIL_EventMeta& In)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("actor"),         In.Actor);
		Obj->SetArrayField (TEXT("addressed_to"),  StringArrayToJson(In.AddressedTo));
		Obj->SetStringField(TEXT("event_hash"),    In.EventHash);
		Obj->SetStringField(TEXT("event_type"),    In.EventType);
		Obj->SetStringField(TEXT("game_id"),       In.GameId);
		if (In.bHasParentEventId)
		{
			Obj->SetNumberField(TEXT("parent_event_id"), static_cast<double>(In.ParentEventId));
		}
		else
		{
			Obj->SetField(TEXT("parent_event_id"), MakeShared<FJsonValueNull>());
		}

		// Payload: pass-through arbitrary JSON. PayloadJson holds the original
		// serialized sub-object; re-parse and embed the parsed value.
		TSharedRef<TJsonReader<>> PayloadReader = TJsonReaderFactory<>::Create(
			TEXT("{\"v\":") + In.PayloadJson + TEXT("}"));
		TSharedPtr<FJsonObject> PayloadWrap;
		if (FJsonSerializer::Deserialize(PayloadReader, PayloadWrap) && PayloadWrap.IsValid())
		{
			TSharedPtr<FJsonValue> PayloadVal = PayloadWrap->TryGetField(TEXT("v"));
			if (PayloadVal.IsValid())
			{
				Obj->SetField(TEXT("payload"), PayloadVal);
			}
			else
			{
				Obj->SetField(TEXT("payload"), MakeShared<FJsonValueNull>());
			}
		}
		else
		{
			Obj->SetField(TEXT("payload"), MakeShared<FJsonValueNull>());
		}

		Obj->SetStringField(TEXT("phase"), In.Phase);
		if (In.bHasPrevHash)
		{
			Obj->SetStringField(TEXT("prev_hash"), In.PrevHash);
		}
		else
		{
			Obj->SetField(TEXT("prev_hash"), MakeShared<FJsonValueNull>());
		}
		Obj->SetNumberField(TEXT("round_no"),      static_cast<double>(In.RoundNo));
		Obj->SetNumberField(TEXT("seq"),           static_cast<double>(In.Seq));
		Obj->SetArrayField (TEXT("visibility"),    StringArrayToJson(In.Visibility));
		Obj->SetStringField(TEXT("wall_clock_ts"), In.WallClockTs);
		return Obj;
	}

	static bool EventMetaFromJsonObject(const TSharedRef<FJsonObject>& In, FAIL_EventMeta& Out)
	{
		if (!In->TryGetStringField(TEXT("actor"),      Out.Actor))     { return false; }
		const TArray<TSharedPtr<FJsonValue>>* Addr = nullptr;
		if (!In->TryGetArrayField(TEXT("addressed_to"), Addr) ||
			!JsonToStringArray(*Addr, Out.AddressedTo)) { return false; }
		if (!In->TryGetStringField(TEXT("event_hash"), Out.EventHash)) { return false; }
		if (!In->TryGetStringField(TEXT("event_type"), Out.EventType)) { return false; }
		if (!In->TryGetStringField(TEXT("game_id"),    Out.GameId))    { return false; }

		const TSharedPtr<FJsonValue> ParentVal = In->TryGetField(TEXT("parent_event_id"));
		if (!ParentVal.IsValid()) { return false; }
		if (ParentVal->Type == EJson::Null)
		{
			Out.bHasParentEventId = false;
			Out.ParentEventId = 0;
		}
		else
		{
			double D = 0;
			if (!ParentVal->TryGetNumber(D)) { return false; }
			Out.bHasParentEventId = true;
			Out.ParentEventId = static_cast<int64>(D);
		}

		const TSharedPtr<FJsonValue> PayloadVal = In->TryGetField(TEXT("payload"));
		if (!PayloadVal.IsValid()) { return false; }
		// Re-serialize the payload to the original JSON-fragment shape.
		FString Wrapped;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Wrapped);
		TSharedRef<FJsonObject> Wrap = MakeShared<FJsonObject>();
		Wrap->SetField(TEXT("v"), PayloadVal);
		FJsonSerializer::Serialize(Wrap, Writer);
		// Wrapped is `{"v":<payload-json>}`. Strip wrapper.
		const int32 Open = Wrapped.Find(TEXT("\"v\":"));
		if (Open == INDEX_NONE) { return false; }
		Out.PayloadJson = Wrapped.Mid(Open + 4, Wrapped.Len() - Open - 4 - 1);

		if (!In->TryGetStringField(TEXT("phase"), Out.Phase)) { return false; }

		const TSharedPtr<FJsonValue> PrevVal = In->TryGetField(TEXT("prev_hash"));
		if (!PrevVal.IsValid()) { return false; }
		if (PrevVal->Type == EJson::Null)
		{
			Out.bHasPrevHash = false;
			Out.PrevHash.Reset();
		}
		else
		{
			Out.bHasPrevHash = true;
			Out.PrevHash = PrevVal->AsString();
		}

		double Round = 0; if (!In->TryGetNumberField(TEXT("round_no"), Round)) { return false; }
		Out.RoundNo = static_cast<int64>(Round);
		double Seq = 0; if (!In->TryGetNumberField(TEXT("seq"), Seq)) { return false; }
		Out.Seq = static_cast<int64>(Seq);

		const TArray<TSharedPtr<FJsonValue>>* Vis = nullptr;
		if (!In->TryGetArrayField(TEXT("visibility"), Vis) ||
			!JsonToStringArray(*Vis, Out.Visibility)) { return false; }

		if (!In->TryGetStringField(TEXT("wall_clock_ts"), Out.WallClockTs)) { return false; }
		return true;
	}

	bool ToJsonString(const FAIL_EventQueryResponse& In, FString& Out)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> EvArr;
		for (const FAIL_EventMeta& Ev : In.Events)
		{
			EvArr.Add(MakeShared<FJsonValueObject>(EventMetaToJsonObject(Ev)));
		}
		Obj->SetArrayField (TEXT("events"),      EvArr);
		Obj->SetNumberField(TEXT("next_cursor"), static_cast<double>(In.NextCursor));
		Out = SerializeObject(Obj);
		return true;
	}

	bool FromJsonString(const FString& In, FAIL_EventQueryResponse& Out)
	{
		TSharedPtr<FJsonObject> Obj;
		if (!ParseObject(In, Obj)) { return false; }
		const TArray<TSharedPtr<FJsonValue>>* EvArr = nullptr;
		if (!Obj->TryGetArrayField(TEXT("events"), EvArr)) { return false; }
		Out.Events.Reset();
		for (const TSharedPtr<FJsonValue>& V : *EvArr)
		{
			if (!V.IsValid() || V->Type != EJson::Object) { return false; }
			FAIL_EventMeta Ev;
			if (!EventMetaFromJsonObject(V->AsObject().ToSharedRef(), Ev)) { return false; }
			Out.Events.Add(MoveTemp(Ev));
		}
		double Cursor = 0;
		if (!Obj->TryGetNumberField(TEXT("next_cursor"), Cursor)) { return false; }
		Out.NextCursor = static_cast<int64>(Cursor);
		return true;
	}
}
