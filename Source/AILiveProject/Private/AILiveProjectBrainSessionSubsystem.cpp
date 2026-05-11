#include "AILiveProjectBrainSessionSubsystem.h"

#include "AILiveProjectActionDispatcher.h"
#include "AILiveProjectBrainHttpClient.h"
#include "AILiveProjectBrainWsClient.h"
#include "AILiveProjectLog.h"
#include "AILiveProjectRosterSubsystem.h"
#include "AILiveProjectSettings.h"
#include "AILiveProjectSpeakDispatcher.h"
#include "AILiveProjectWorldStateCollector.h"
#include "AILiveProtocolTypes.h"
#include "Async/Async.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "TimerManager.h"

void UAILiveProjectBrainSessionSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	const UAILiveProjectSettings* Settings = GetDefault<UAILiveProjectSettings>();
	Client = MakePimpl<FAILiveProjectBrainHttpClient>(
		Settings->BrainBaseUrl,
		Settings->BrainApiToken,
		Settings->HttpTimeoutMs,
		Settings->MaxRetries,
		Settings->RetryBackoffBaseSeconds);
	WsClient = MakePimpl<FAILiveProjectBrainWsClient>(
		Settings->BrainWebSocketUrl,
		Settings->BrainApiToken);
	InstallWebSocketHandlers();

	// Auto-hook PIE / standalone game world creation so the handshake fires
	// even when GM_Sandbox BP doesn't call StartHandshake explicitly. We
	// subscribe to OnPostWorldInitialization, then attach this world's own
	// OnWorldBeginPlay delegate (Pawns are spawned by the time BeginPlay
	// fires, satisfying the roster minItems:1 requirement).
	WorldInitDelegateHandle = FWorldDelegates::OnPostWorldInitialization.AddLambda(
		[WeakThis = TWeakObjectPtr<UAILiveProjectBrainSessionSubsystem>(this)]
		(UWorld* World, const UWorld::InitializationValues)
	{
		UAILiveProjectBrainSessionSubsystem* Self = WeakThis.Get();
		if (!Self || !World) { return; }
		if (World->WorldType != EWorldType::PIE && World->WorldType != EWorldType::Game) { return; }
		if (World->GetGameInstance() != Self->GetGameInstance()) { return; }
		World->OnWorldBeginPlay.AddLambda([WeakThis]()
		{
			if (UAILiveProjectBrainSessionSubsystem* S = WeakThis.Get())
			{
				S->StartHandshake();
			}
		});
	});

	UE_LOG(LogAILiveBrain, Log,
		TEXT("BrainSession initialized; auto-hook on OnWorldBeginPlay (GM_Sandbox BP may also call StartHandshake)"));
}

void UAILiveProjectBrainSessionSubsystem::OnWorldBeginPlayHook(UWorld* World)
{
	// Kept for symmetry with the header declaration; logic now lives in the
	// OnPostWorldInitialization lambda installed in Initialize.
	(void)World;
}

void UAILiveProjectBrainSessionSubsystem::Deinitialize()
{
	if (WorldInitDelegateHandle.IsValid())
	{
		FWorldDelegates::OnPostWorldInitialization.Remove(WorldInitDelegateHandle);
		WorldInitDelegateHandle.Reset();
	}
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(ReconnectTimerHandle);
	}
	if (WsClient)
	{
		WsClient->Close();
	}
	if (Client)
	{
		Client->CancelAllInFlight();
	}
	bReady = false;
	bHandshakeInFlight = false;
	GameId.Reset();
	WsClient.Reset();
	Client.Reset();

	if (UGameInstance* GI = GetGameInstance())
	{
		if (UAILiveProjectRosterSubsystem* Roster = GI->GetSubsystem<UAILiveProjectRosterSubsystem>())
		{
			Roster->ClearAll();
		}
	}
	Super::Deinitialize();
}

void UAILiveProjectBrainSessionSubsystem::StartHandshake()
{
	if (bReady)
	{
		UE_LOG(LogAILiveBrain, Log, TEXT("StartHandshake: already ready, skipping"));
		return;
	}
	if (bHandshakeInFlight)
	{
		UE_LOG(LogAILiveBrain, Log, TEXT("StartHandshake: already in flight, skipping"));
		return;
	}
	bHandshakeInFlight = true;
	Phase1_HealthCheck();
}

void UAILiveProjectBrainSessionSubsystem::Phase1_HealthCheck()
{
	UE_LOG(LogAILiveBrain, Log, TEXT("Handshake phase 1: Health"));
	TWeakObjectPtr<UAILiveProjectBrainSessionSubsystem> WeakThis(this);
	Client->Health().Next([WeakThis](TOptional<FAIL_HealthResponse> Resp)
	{
		AsyncTask(ENamedThreads::GameThread, [WeakThis, Resp]()
		{
			UAILiveProjectBrainSessionSubsystem* This = WeakThis.Get();
			if (!This) { return; }
			if (!Resp.IsSet())
			{
				UE_LOG(LogAILiveBrain, Error, TEXT("Health failed; aborting handshake"));
				This->bHandshakeInFlight = false;
				return;
			}
			if (Resp->ProtocolVersion != TEXT("0.3.0"))
			{
				UE_LOG(LogAILiveBrain, Fatal,
					TEXT("Brain protocol_version mismatch: got '%s' expected '0.3.0'"),
					*Resp->ProtocolVersion);
				This->bHandshakeInFlight = false;
				return;
			}
			This->ProtocolVersion = Resp->ProtocolVersion;
			UE_LOG(LogAILiveBrain, Log, TEXT("Health OK protocol_version=%s"), *Resp->ProtocolVersion);
			This->Phase2_ConnectWebSocket();
		});
	});
}

void UAILiveProjectBrainSessionSubsystem::Phase2_ConnectWebSocket()
{
	UE_LOG(LogAILiveBrain, Log, TEXT("Handshake phase 2: ConnectWebSocket"));
	if (!WsClient)
	{
		UE_LOG(LogAILiveBrain, Error, TEXT("WebSocket client missing; aborting handshake"));
		bHandshakeInFlight = false;
		return;
	}
	WsClient->Connect();
}

void UAILiveProjectBrainSessionSubsystem::Phase3_CreateSession()
{
	UE_LOG(LogAILiveBrain, Log, TEXT("Handshake phase 3: CreateSession over WebSocket"));
	FAIL_SessionCreateRequest Req;
	WsClient->SendSessionCreate(Req);
}

void UAILiveProjectBrainSessionSubsystem::Phase4_RegisterRoster()
{
	UE_LOG(LogAILiveBrain, Log, TEXT("Handshake phase 4: RegisterRoster over WebSocket"));

	UGameInstance* GI = GetGameInstance();
	UAILiveProjectRosterSubsystem* Roster = GI ? GI->GetSubsystem<UAILiveProjectRosterSubsystem>() : nullptr;
	if (!Roster)
	{
		UE_LOG(LogAILiveBrain, Error, TEXT("RosterSubsystem missing; aborting handshake"));
		bHandshakeInFlight = false;
		return;
	}

	UWorld* World = GetWorld();
	const int32 Registered = Roster->EnumerateAndRegisterAgentsInWorld(World);
	if (Registered == 0)
	{
		UE_LOG(LogAILiveBrain, Error,
			TEXT("Roster enumeration found 0 IAILiveAgent Pawns; brain register_roster requires minItems:1. Aborting handshake."));
		bHandshakeInFlight = false;
		return;
	}

	FAIL_RosterRegisterRequest Req;
	Req.Roster.Reserve(Registered);
	TArray<TPair<FString, TWeakObjectPtr<APawn>>> All;
	Roster->GetAllRegistered(All);
	for (const TPair<FString, TWeakObjectPtr<APawn>>& Pair : All)
	{
		FAIL_NPCEntry Entry;
		Entry.ActorId = Pair.Key;
		Entry.DisplayName = Pair.Key;
		if (APawn* Pawn = Pair.Value.Get())
		{
			Entry.bHasInitialPosition = true;
			const FVector Loc = Pawn->GetActorLocation();
			Entry.InitialPosition.X = Loc.X;
			Entry.InitialPosition.Y = Loc.Y;
			Entry.InitialPosition.Z = Loc.Z;
		}
		Req.Roster.Add(MoveTemp(Entry));
	}

	WsClient->SendRosterRegister(GameId, Req);
}

void UAILiveProjectBrainSessionSubsystem::Phase5_StartRuntime()
{
	UE_LOG(LogAILiveBrain, Log, TEXT("Handshake phase 5: StartRuntime"));
	UWorld* World = GetWorld();
	if (!World) { return; }
	if (auto* C = World->GetSubsystem<class UAILiveProjectWorldStateCollector>()) { C->StartPolling(); }
}

void UAILiveProjectBrainSessionSubsystem::InstallWebSocketHandlers()
{
	if (!WsClient) { return; }
	TWeakObjectPtr<UAILiveProjectBrainSessionSubsystem> WeakThis(this);

	WsClient->SetOnConnected([WeakThis]()
	{
		UAILiveProjectBrainSessionSubsystem* This = WeakThis.Get();
		if (!This || !This->WsClient) { return; }
		if (UWorld* World = This->GetWorld())
		{
			World->GetTimerManager().ClearTimer(This->ReconnectTimerHandle);
		}
		if (This->GameId.IsEmpty())
		{
			This->Phase3_CreateSession();
		}
		else
		{
			This->WsClient->SendSessionResume(This->GameId, This->WsClient->GetLastBrainSeq());
		}
	});

	WsClient->SetOnClosed([WeakThis]()
	{
		UAILiveProjectBrainSessionSubsystem* This = WeakThis.Get();
		if (!This) { return; }
		This->bReady = false;
		This->bHandshakeInFlight = false;
		This->ScheduleReconnect();
	});

	WsClient->SetOnSessionCreated([WeakThis](const FAIL_SessionCreateResponse& Resp)
	{
		UAILiveProjectBrainSessionSubsystem* This = WeakThis.Get();
		if (!This) { return; }
		const bool bResume = !This->GameId.IsEmpty() && This->GameId == Resp.GameId;
		This->GameId = Resp.GameId;
		UE_LOG(LogAILiveBrain, Log, TEXT("WebSocket session ready game_id=%s"), *This->GameId);
		if (bResume)
		{
			This->bReady = true;
			This->bHandshakeInFlight = false;
			This->Phase5_StartRuntime();
		}
		else
		{
			This->Phase4_RegisterRoster();
		}
	});

	WsClient->SetOnRosterAccepted([WeakThis](const FAIL_RosterRegisterResponse& Resp)
	{
		UAILiveProjectBrainSessionSubsystem* This = WeakThis.Get();
		if (!This) { return; }
		UE_LOG(LogAILiveBrain, Log, TEXT("Roster registered count=%d"), Resp.AcceptedCount);
		This->bReady = true;
		This->bHandshakeInFlight = false;
		This->Phase5_StartRuntime();
	});

	WsClient->SetOnActionIntent([WeakThis](const FAIL_ActionIntentEvent& Ev)
	{
		UAILiveProjectBrainSessionSubsystem* This = WeakThis.Get();
		UWorld* World = This ? This->GetWorld() : nullptr;
		if (!World) { return; }
		if (UAILiveProjectActionDispatcher* D = World->GetSubsystem<UAILiveProjectActionDispatcher>())
		{
			D->HandleBrainActionIntent(Ev);
		}
	});

	WsClient->SetOnActionCancelled([WeakThis](int64 CancelSeq, int64 SourceIntentSeq, const FString& ActorId)
	{
		UAILiveProjectBrainSessionSubsystem* This = WeakThis.Get();
		UWorld* World = This ? This->GetWorld() : nullptr;
		if (!World) { return; }
		if (UAILiveProjectActionDispatcher* D = World->GetSubsystem<UAILiveProjectActionDispatcher>())
		{
			D->HandleBrainActionCancelled(CancelSeq, SourceIntentSeq, ActorId);
		}
	});

	WsClient->SetOnSpeechPublic([WeakThis](const FAIL_SpeechPublicEvent& Ev)
	{
		UAILiveProjectBrainSessionSubsystem* This = WeakThis.Get();
		UWorld* World = This ? This->GetWorld() : nullptr;
		if (!World) { return; }
		if (UAILiveProjectSpeakDispatcher* D = World->GetSubsystem<UAILiveProjectSpeakDispatcher>())
		{
			D->HandleBrainSpeechPublic(Ev);
		}
	});
}

void UAILiveProjectBrainSessionSubsystem::ScheduleReconnect()
{
	UWorld* World = GetWorld();
	if (!World || !WsClient) { return; }
	if (World->GetTimerManager().IsTimerActive(ReconnectTimerHandle)) { return; }

	const UAILiveProjectSettings* Settings = GetDefault<UAILiveProjectSettings>();
	const float Delay = FMath::Max(Settings->WebSocketReconnectBaseMs, 500) / 1000.f;
	UE_LOG(LogAILiveBrain, Warning, TEXT("Scheduling Brain WebSocket reconnect in %.2fs"), Delay);
	World->GetTimerManager().SetTimer(ReconnectTimerHandle,
		FTimerDelegate::CreateWeakLambda(this, [this]()
		{
			if (WsClient)
			{
				WsClient->Connect();
			}
		}),
		Delay, false);
}

bool UAILiveProjectBrainSessionSubsystem::SendWorldState(const FAIL_WorldStatePushRequest& Req)
{
	return WsClient && bReady && WsClient->SendWorldState(GameId, Req);
}

bool UAILiveProjectBrainSessionSubsystem::SendActionResult(const FAIL_ActionResultRequest& Req)
{
	return WsClient && bReady && WsClient->SendActionResult(GameId, Req);
}

bool UAILiveProjectBrainSessionSubsystem::SendSpeechResult(const FAIL_SpeechResultRequest& Req)
{
	return WsClient && bReady && WsClient->SendSpeechResult(GameId, Req);
}

bool UAILiveProjectBrainSessionSubsystem::SendIngressReject(const FAIL_IngressRejectRequest& Req)
{
	return WsClient && bReady && WsClient->SendIngressReject(GameId, Req);
}

bool UAILiveProjectBrainSessionSubsystem::AckBrainEvent(int64 Seq)
{
	return WsClient && !GameId.IsEmpty() && WsClient->AckEvent(GameId, Seq);
}

bool UAILiveProjectBrainSessionSubsystem::RequestStartLLMPhase(int32 RoundNo, const FString& Phase, int32 NTicks)
{
	if (!WsClient || !bReady)
	{
		UE_LOG(LogAILiveBrain, Warning,
			TEXT("RequestStartLLMPhase dropped: client=%d ready=%d"),
			WsClient ? 1 : 0, bReady ? 1 : 0);
		return false;
	}
	UE_LOG(LogAILiveBrain, Log,
		TEXT("Requesting LLM phase: round_no=%d phase=%s n_ticks=%d"),
		RoundNo, *Phase, NTicks);
	return WsClient->SendRoundStartLLMPhase(GameId, RoundNo, Phase, NTicks);
}
