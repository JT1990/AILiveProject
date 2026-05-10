#include "AILiveProjectBrainSessionSubsystem.h"

#include "AILiveProjectActionDispatcher.h"
#include "AILiveProjectBrainHttpClient.h"
#include "AILiveProjectLog.h"
#include "AILiveProjectRosterSubsystem.h"
#include "AILiveProjectSettings.h"
#include "AILiveProjectSpeakDispatcher.h"
#include "AILiveProjectWorldStateCollector.h"
#include "AILiveProtocolTypes.h"
#include "Async/Async.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"

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
	if (Client)
	{
		Client->CancelAllInFlight();
	}
	bReady = false;
	bHandshakeInFlight = false;
	GameId.Reset();
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
			if (Resp->ProtocolVersion != TEXT("0.1.1"))
			{
				UE_LOG(LogAILiveBrain, Fatal,
					TEXT("Brain protocol_version mismatch: got '%s' expected '0.1.1'"),
					*Resp->ProtocolVersion);
				This->bHandshakeInFlight = false;
				return;
			}
			This->ProtocolVersion = Resp->ProtocolVersion;
			UE_LOG(LogAILiveBrain, Log, TEXT("Health OK protocol_version=%s"), *Resp->ProtocolVersion);
			This->Phase2_CreateSession();
		});
	});
}

void UAILiveProjectBrainSessionSubsystem::Phase2_CreateSession()
{
	UE_LOG(LogAILiveBrain, Log, TEXT("Handshake phase 2: CreateSession"));
	TWeakObjectPtr<UAILiveProjectBrainSessionSubsystem> WeakThis(this);
	FAIL_SessionCreateRequest Req;
	Client->CreateSession(Req).Next([WeakThis](TOptional<FAIL_SessionCreateResponse> Resp)
	{
		AsyncTask(ENamedThreads::GameThread, [WeakThis, Resp]()
		{
			UAILiveProjectBrainSessionSubsystem* This = WeakThis.Get();
			if (!This) { return; }
			if (!Resp.IsSet())
			{
				UE_LOG(LogAILiveBrain, Error, TEXT("CreateSession failed; aborting handshake"));
				This->bHandshakeInFlight = false;
				return;
			}
			This->GameId = Resp->GameId;
			UE_LOG(LogAILiveBrain, Log, TEXT("Session created game_id=%s"), *This->GameId);
			This->Phase3_RegisterRoster();
		});
	});
}

void UAILiveProjectBrainSessionSubsystem::Phase3_RegisterRoster()
{
	UE_LOG(LogAILiveBrain, Log, TEXT("Handshake phase 3: RegisterRoster"));

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

	TWeakObjectPtr<UAILiveProjectBrainSessionSubsystem> WeakThis(this);
	Client->RegisterRoster(GameId, Req).Next([WeakThis, Registered](TOptional<FAIL_RosterRegisterResponse> Resp)
	{
		AsyncTask(ENamedThreads::GameThread, [WeakThis, Resp, Registered]()
		{
			UAILiveProjectBrainSessionSubsystem* This = WeakThis.Get();
			if (!This) { return; }
			if (!Resp.IsSet())
			{
				UE_LOG(LogAILiveBrain, Error, TEXT("RegisterRoster failed; aborting handshake"));
				This->bHandshakeInFlight = false;
				return;
			}
			UE_LOG(LogAILiveBrain, Log, TEXT("Roster registered count=%d"), Resp->AcceptedCount);
			This->bReady = true;
			This->bHandshakeInFlight = false;
			This->Phase4_StartPolling();
		});
	});
}

void UAILiveProjectBrainSessionSubsystem::Phase4_StartPolling()
{
	UE_LOG(LogAILiveBrain, Log, TEXT("Handshake phase 4: StartPolling"));
	UWorld* World = GetWorld();
	if (!World) { return; }
	if (auto* C = World->GetSubsystem<class UAILiveProjectWorldStateCollector>()) { C->StartPolling(); }
	if (auto* A = World->GetSubsystem<class UAILiveProjectActionDispatcher>())     { A->StartPolling(); }
	if (auto* S = World->GetSubsystem<class UAILiveProjectSpeakDispatcher>())      { S->StartPolling(); }
}
