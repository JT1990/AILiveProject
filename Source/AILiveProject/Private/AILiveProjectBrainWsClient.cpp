#include "AILiveProjectBrainWsClient.h"

#include "AILiveProjectLog.h"
#include "AILiveProtocolJson.h"
#include "Async/Async.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "IWebSocket.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "WebSocketsModule.h"

struct FAILiveProjectBrainWsClient::FState : public TSharedFromThis<FAILiveProjectBrainWsClient::FState, ESPMode::ThreadSafe>
{
	FString WsUrl;
	FString ApiToken;
	TSharedPtr<IWebSocket> Socket;
	bool bClosing = false;
	int64 LastBrainSeq = -1;

	FConnectedHandler OnConnected;
	FClosedHandler OnClosed;
	FSessionCreatedHandler OnSessionCreated;
	FRosterAcceptedHandler OnRosterAccepted;
	FActionIntentHandler OnActionIntent;
	FActionCancelledHandler OnActionCancelled;
	FSpeechPublicHandler OnSpeechPublic;

	FState(FString InWsUrl, FString InApiToken)
		: WsUrl(MoveTemp(InWsUrl))
		, ApiToken(MoveTemp(InApiToken))
	{
	}
};

namespace
{
	FString SerializeObject(const TSharedRef<FJsonObject>& Obj)
	{
		FString Out;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(Obj, Writer);
		return Out;
	}

	bool ParseObject(const FString& In, TSharedPtr<FJsonObject>& Out)
	{
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(In);
		return FJsonSerializer::Deserialize(Reader, Out) && Out.IsValid();
	}

	TSharedPtr<FJsonValue> JsonStringToValue(const FString& In)
	{
		TSharedPtr<FJsonObject> Obj;
		if (ParseObject(In, Obj) && Obj.IsValid())
		{
			return MakeShared<FJsonValueObject>(Obj.ToSharedRef());
		}
		return MakeShared<FJsonValueObject>(MakeShared<FJsonObject>());
	}

	bool PayloadToString(const TSharedPtr<FJsonObject>& Root, FString& Out)
	{
		if (!Root.IsValid()) { return false; }
		const TSharedPtr<FJsonObject>* PayloadObj = nullptr;
		if (Root->TryGetObjectField(TEXT("payload"), PayloadObj) && PayloadObj && PayloadObj->IsValid())
		{
			Out = SerializeObject(PayloadObj->ToSharedRef());
			return true;
		}
		Out = TEXT("{}");
		return true;
	}

	bool TryGetInt64Field(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Field, int64& Out)
	{
		if (!Obj.IsValid()) { return false; }
		double Number = 0.0;
		if (!Obj->TryGetNumberField(Field, Number)) { return false; }
		Out = static_cast<int64>(Number);
		return true;
	}

	FString BuildEnvelopeJson(const FString& Type, const FString& GameId, const FString& PayloadJson)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("type"), Type);
		Obj->SetStringField(TEXT("msg_id"), AILiveProtocol::NewIdempotencyKey());
		Obj->SetStringField(TEXT("sent_at"), AILiveProtocol::IsoUtcNow());
		if (!GameId.IsEmpty())
		{
			Obj->SetStringField(TEXT("game_id"), GameId);
		}
		Obj->SetField(TEXT("payload"), JsonStringToValue(PayloadJson));
		return SerializeObject(Obj);
	}
}

FAILiveProjectBrainWsClient::FAILiveProjectBrainWsClient(FString InWsUrl, FString InApiToken)
	: State(MakeShared<FState, ESPMode::ThreadSafe>(MoveTemp(InWsUrl), MoveTemp(InApiToken)))
{
	while (State->WsUrl.EndsWith(TEXT("/"))) { State->WsUrl.LeftChopInline(1); }
	UE_LOG(LogAILiveBrain, Log, TEXT("WebSocket client url=%s"), *State->WsUrl);
}

FAILiveProjectBrainWsClient::~FAILiveProjectBrainWsClient()
{
	Close();
}

void FAILiveProjectBrainWsClient::SetOnConnected(FConnectedHandler Handler) { State->OnConnected = MoveTemp(Handler); }
void FAILiveProjectBrainWsClient::SetOnClosed(FClosedHandler Handler) { State->OnClosed = MoveTemp(Handler); }
void FAILiveProjectBrainWsClient::SetOnSessionCreated(FSessionCreatedHandler Handler) { State->OnSessionCreated = MoveTemp(Handler); }
void FAILiveProjectBrainWsClient::SetOnRosterAccepted(FRosterAcceptedHandler Handler) { State->OnRosterAccepted = MoveTemp(Handler); }
void FAILiveProjectBrainWsClient::SetOnActionIntent(FActionIntentHandler Handler) { State->OnActionIntent = MoveTemp(Handler); }
void FAILiveProjectBrainWsClient::SetOnActionCancelled(FActionCancelledHandler Handler) { State->OnActionCancelled = MoveTemp(Handler); }
void FAILiveProjectBrainWsClient::SetOnSpeechPublic(FSpeechPublicHandler Handler) { State->OnSpeechPublic = MoveTemp(Handler); }

void FAILiveProjectBrainWsClient::Connect()
{
	if (State->Socket.IsValid() && State->Socket->IsConnected())
	{
		return;
	}

	TMap<FString, FString> Headers;
	if (!State->ApiToken.IsEmpty())
	{
		Headers.Add(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *State->ApiToken));
	}

	State->bClosing = false;
	TSharedRef<IWebSocket> Socket = FWebSocketsModule::Get().CreateWebSocket(State->WsUrl, FString(), Headers);
	State->Socket = Socket;
	TWeakPtr<FState, ESPMode::ThreadSafe> WeakState(State);

	Socket->OnConnected().AddLambda([WeakState]()
	{
		AsyncTask(ENamedThreads::GameThread, [WeakState]()
		{
			TSharedPtr<FState, ESPMode::ThreadSafe> Pinned = WeakState.Pin();
			if (!Pinned.IsValid()) { return; }
			UE_LOG(LogAILiveBrain, Log, TEXT("Brain WebSocket connected"));
			if (Pinned->OnConnected) { Pinned->OnConnected(); }
		});
	});
	Socket->OnConnectionError().AddLambda([WeakState](const FString& Error)
	{
		AsyncTask(ENamedThreads::GameThread, [WeakState, Error]()
		{
			TSharedPtr<FState, ESPMode::ThreadSafe> Pinned = WeakState.Pin();
			if (!Pinned.IsValid()) { return; }
			UE_LOG(LogAILiveBrain, Warning, TEXT("Brain WebSocket connection error: %s"), *Error);
			if (!Pinned->bClosing && Pinned->OnClosed) { Pinned->OnClosed(); }
		});
	});
	Socket->OnClosed().AddLambda([WeakState](int32 StatusCode, const FString& Reason, bool bWasClean)
	{
		AsyncTask(ENamedThreads::GameThread, [WeakState, StatusCode, Reason, bWasClean]()
		{
			TSharedPtr<FState, ESPMode::ThreadSafe> Pinned = WeakState.Pin();
			if (!Pinned.IsValid()) { return; }
			UE_LOG(LogAILiveBrain, Warning,
				TEXT("Brain WebSocket closed code=%d clean=%d reason=%s"),
				StatusCode, bWasClean ? 1 : 0, *Reason);
			if (!Pinned->bClosing && Pinned->OnClosed) { Pinned->OnClosed(); }
		});
	});
	Socket->OnMessage().AddLambda([WeakState](const FString& Message)
	{
		AsyncTask(ENamedThreads::GameThread, [WeakState, Message]()
		{
			TSharedPtr<FState, ESPMode::ThreadSafe> Pinned = WeakState.Pin();
			if (!Pinned.IsValid()) { return; }
			FAILiveProjectBrainWsClient::HandleMessage(Pinned.ToSharedRef(), Message);
		});
	});

	Socket->Connect();
}

void FAILiveProjectBrainWsClient::Close()
{
	State->bClosing = true;
	if (State->Socket.IsValid())
	{
		State->Socket->Close();
		State->Socket.Reset();
	}
}

bool FAILiveProjectBrainWsClient::IsConnected() const
{
	return State->Socket.IsValid() && State->Socket->IsConnected();
}

bool FAILiveProjectBrainWsClient::SendSessionCreate(const FAIL_SessionCreateRequest& Req)
{
	FString Payload;
	AILiveProtocol::ToJsonString(Req, Payload);
	return SendEnvelope(TEXT("session.create"), FString(), Payload);
}

bool FAILiveProjectBrainWsClient::SendSessionResume(const FString& GameId, int64 LastBrainSeq)
{
	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("game_id"), GameId);
	Payload->SetNumberField(TEXT("last_brain_seq"), static_cast<double>(LastBrainSeq));
	return SendEnvelope(TEXT("session.resume"), GameId, SerializeObject(Payload));
}

bool FAILiveProjectBrainWsClient::SendRosterRegister(const FString& GameId, const FAIL_RosterRegisterRequest& Req)
{
	FString Payload;
	AILiveProtocol::ToJsonString(Req, Payload);
	return SendEnvelope(TEXT("roster.register"), GameId, Payload);
}

bool FAILiveProjectBrainWsClient::SendWorldState(const FString& GameId, const FAIL_WorldStatePushRequest& Req)
{
	FString Payload;
	AILiveProtocol::ToJsonString(Req, Payload);
	return SendEnvelope(TEXT("world_state.push"), GameId, Payload);
}

bool FAILiveProjectBrainWsClient::SendActionResult(const FString& GameId, const FAIL_ActionResultRequest& Req)
{
	FString Payload;
	AILiveProtocol::ToJsonString(Req, Payload);
	return SendEnvelope(TEXT("action.result"), GameId, Payload);
}

bool FAILiveProjectBrainWsClient::SendSpeechResult(const FString& GameId, const FAIL_SpeechResultRequest& Req)
{
	FString Payload;
	AILiveProtocol::ToJsonString(Req, Payload);
	return SendEnvelope(TEXT("speech.result"), GameId, Payload);
}

bool FAILiveProjectBrainWsClient::SendIngressReject(const FString& GameId, const FAIL_IngressRejectRequest& Req)
{
	FString Payload;
	AILiveProtocol::ToJsonString(Req, Payload);
	return SendEnvelope(TEXT("ingress.reject"), GameId, Payload);
}

bool FAILiveProjectBrainWsClient::AckEvent(const FString& GameId, int64 Seq)
{
	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetNumberField(TEXT("seq"), static_cast<double>(Seq));
	return SendEnvelope(TEXT("event.ack"), GameId, SerializeObject(Payload));
}

bool FAILiveProjectBrainWsClient::SendRoundStartLLMPhase(const FString& GameId, int32 RoundNo, const FString& Phase, int32 NTicks)
{
	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetNumberField(TEXT("round_no"), static_cast<double>(RoundNo));
	Payload->SetStringField(TEXT("phase"), Phase);
	Payload->SetNumberField(TEXT("n_ticks"), static_cast<double>(NTicks));
	return SendEnvelope(TEXT("round.start_llm_phase"), GameId, SerializeObject(Payload));
}

int64 FAILiveProjectBrainWsClient::GetLastBrainSeq() const
{
	return State->LastBrainSeq;
}

bool FAILiveProjectBrainWsClient::SendEnvelope(const FString& Type, const FString& GameId, const FString& PayloadJson)
{
	if (!IsConnected())
	{
		UE_LOG(LogAILiveBrain, Warning, TEXT("WebSocket send dropped (not connected) type=%s"), *Type);
		return false;
	}
	State->Socket->Send(BuildEnvelopeJson(Type, GameId, PayloadJson));
	return true;
}

void FAILiveProjectBrainWsClient::HandleMessage(const TSharedRef<FState>& StateRef, const FString& Message)
{
	TSharedPtr<FJsonObject> Root;
	if (!ParseObject(Message, Root))
	{
		UE_LOG(LogAILiveBrain, Warning, TEXT("Invalid WebSocket JSON: %s"), *Message);
		return;
	}

	FString Type;
	if (!Root->TryGetStringField(TEXT("type"), Type))
	{
		UE_LOG(LogAILiveBrain, Warning, TEXT("WebSocket frame missing type"));
		return;
	}

	int64 Seq = -1;
	if (TryGetInt64Field(Root, TEXT("seq"), Seq))
	{
		StateRef->LastBrainSeq = FMath::Max(StateRef->LastBrainSeq, Seq);
	}

	FString PayloadJson;
	PayloadToString(Root, PayloadJson);

	if (Type == TEXT("session.created") || Type == TEXT("session.resumed"))
	{
		FAIL_SessionCreateResponse Resp;
		if (AILiveProtocol::FromJsonString(PayloadJson, Resp))
		{
			if (StateRef->OnSessionCreated) { StateRef->OnSessionCreated(Resp); }
		}
		return;
	}

	if (Type == TEXT("roster.accepted"))
	{
		FAIL_RosterRegisterResponse Resp;
		if (AILiveProtocol::FromJsonString(PayloadJson, Resp))
		{
			if (StateRef->OnRosterAccepted) { StateRef->OnRosterAccepted(Resp); }
		}
		return;
	}

	if (Type == TEXT("event.action_intent"))
	{
		const FString Wrapped = FString::Printf(TEXT("{\"events\":[%s],\"next_cursor\":%lld}"), *PayloadJson, Seq);
		FAIL_ActionPullResponse Resp;
		if (AILiveProtocol::FromJsonString(Wrapped, Resp) && Resp.Events.Num() == 1)
		{
			if (StateRef->OnActionIntent) { StateRef->OnActionIntent(Resp.Events[0]); }
		}
		return;
	}

	if (Type == TEXT("event.action_cancelled"))
	{
		TSharedPtr<FJsonObject> PayloadObj;
		if (ParseObject(PayloadJson, PayloadObj))
		{
			int64 SourceSeq = -1;
			TryGetInt64Field(PayloadObj, TEXT("source_seq"), SourceSeq);
			FString ActorId;
			PayloadObj->TryGetStringField(TEXT("actor_id"), ActorId);
			if (StateRef->OnActionCancelled) { StateRef->OnActionCancelled(Seq, SourceSeq, ActorId); }
		}
		return;
	}

	if (Type == TEXT("event.speech_public"))
	{
		const FString Wrapped = FString::Printf(TEXT("{\"events\":[%s],\"next_cursor\":%lld}"), *PayloadJson, Seq);
		FAIL_SpeechPullResponse Resp;
		if (AILiveProtocol::FromJsonString(Wrapped, Resp) && Resp.Events.Num() == 1)
		{
			if (StateRef->OnSpeechPublic) { StateRef->OnSpeechPublic(Resp.Events[0]); }
		}
		return;
	}

	if (Type == TEXT("error"))
	{
		UE_LOG(LogAILiveBrain, Warning, TEXT("Brain WebSocket error: %s"), *PayloadJson);
		return;
	}
}
