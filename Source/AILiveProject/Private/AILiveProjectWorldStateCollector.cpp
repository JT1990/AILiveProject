#include "AILiveProjectWorldStateCollector.h"

#include "AILiveProjectActionDispatcher.h"
#include "AILiveProjectBrainHttpClient.h"
#include "AILiveProjectBrainSessionSubsystem.h"
#include "AILiveProjectLog.h"
#include "AILiveProjectPerceptionLogger.h"
#include "AILiveProjectRosterSubsystem.h"
#include "AILiveProjectSettings.h"
#include "AILiveProjectSpeakDispatcher.h"
#include "AILiveProtocolJson.h"
#include "AILiveProtocolTypes.h"
#include "AIController.h"
#include "Async/Async.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "TimerManager.h"

void UAILiveProjectWorldStateCollector::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
}

void UAILiveProjectWorldStateCollector::Deinitialize()
{
	StopPolling();
	Super::Deinitialize();
}

void UAILiveProjectWorldStateCollector::StartPolling()
{
	UWorld* World = GetWorld();
	if (!World) { return; }
	const UAILiveProjectSettings* Settings = GetDefault<UAILiveProjectSettings>();
	const float Interval = FMath::Max(Settings->PollingIntervalWorldStateMs, 100) / 1000.f;
	World->GetTimerManager().SetTimer(TimerHandle,
		FTimerDelegate::CreateUObject(this, &UAILiveProjectWorldStateCollector::TickPush),
		Interval, true, 0.f);
	UE_LOG(LogAILiveBrain, Log, TEXT("WorldStateCollector polling every %.3fs"), Interval);
}

void UAILiveProjectWorldStateCollector::StopPolling()
{
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(TimerHandle);
	}
	bPushInFlight = false;
	NextClientSampleId = 0;
}

void UAILiveProjectWorldStateCollector::TickPush()
{
	UWorld* World = GetWorld();
	UGameInstance* GI = World ? World->GetGameInstance() : nullptr;
	if (!GI) { return; }
	UAILiveProjectBrainSessionSubsystem* Session = GI->GetSubsystem<UAILiveProjectBrainSessionSubsystem>();
	UAILiveProjectRosterSubsystem* Roster       = GI->GetSubsystem<UAILiveProjectRosterSubsystem>();
	if (!Session || !Roster || !Session->IsReady() || !Session->GetClient()) { return; }
	if (bPushInFlight) { return; }

	UAILiveProjectActionDispatcher* ActionDispatcher = World->GetSubsystem<UAILiveProjectActionDispatcher>();
	UAILiveProjectSpeakDispatcher*  SpeakDispatcher  = World->GetSubsystem<UAILiveProjectSpeakDispatcher>();

	TArray<TPair<FString, TWeakObjectPtr<APawn>>> All;
	Roster->GetAllRegistered(All);
	if (All.Num() == 0) { return; }

	FAIL_WorldStatePushRequest Req;
	Req.ClientSampleId = NextClientSampleId++;
	Req.SampleWallClockTs = AILiveProtocol::IsoUtcNow();

	for (const TPair<FString, TWeakObjectPtr<APawn>>& Pair : All)
	{
		APawn* Pawn = Pair.Value.Get();
		if (!Pawn) { continue; }

		FAIL_NpcObservation Obs;
		Obs.ActorId = Pair.Key;

		const FVector Loc = Pawn->GetActorLocation();
		Obs.Position.X = Loc.X;
		Obs.Position.Y = Loc.Y;
		Obs.Position.Z = Loc.Z;
		Obs.FacingDegrees = Pawn->GetActorRotation().Yaw;

		// current_action heuristic per task card §3.9.1.
		Obs.CurrentAction = E_AIL_NpcAction::Idle;
		if (SpeakDispatcher && SpeakDispatcher->HasSpeechForPawn(Pawn))
		{
			Obs.CurrentAction = E_AIL_NpcAction::Speaking;
		}
		else if (ActionDispatcher && ActionDispatcher->HasActionForPawn(Pawn))
		{
			const FName ActionName = ActionDispatcher->GetActionNameForPawn(Pawn);
			if (ActionName == FName(TEXT("move_to"))) { Obs.CurrentAction = E_AIL_NpcAction::Moving; }
			else if (ActionName == FName(TEXT("sit"))) { Obs.CurrentAction = E_AIL_NpcAction::Sitting; }
			// wait keeps Idle (no externally observable activity).
		}

		// Perception gather via existing PerceptionLogger; map FName -> actor_id.
		AAIController* AIC = Pawn->GetController<AAIController>();
		if (AIC)
		{
			const TArray<FPerceivedAgentInfo> Sight =
				UAILiveProjectPerceptionLogger::GatherSightPerception(AIC, this);
			for (const FPerceivedAgentInfo& Info : Sight)
			{
				if (!Info.bCurrentlySensed) { continue; }
				FString TargetId;
				if (!Roster->TryGetActorIdForBPClassFName(Info.Identity, TargetId)) { continue; }
				FAIL_SightEntry S;
				S.DistanceCm = Info.DistanceCm;
				S.RelYawDegrees = Info.RelativeYawDeg;
				S.TargetActorId = TargetId;
				Obs.SightedActors.Add(MoveTemp(S));
			}

			const TArray<FHeardSoundInfo> Heard =
				UAILiveProjectPerceptionLogger::GatherHearingPerception(AIC, this);
			for (const FHeardSoundInfo& H : Heard)
			{
				FString SourceId;
				if (!Roster->TryGetActorIdForBPClassFName(H.SourceIdentity, SourceId)) { continue; }
				FAIL_HeardSound HS;
				HS.AgeSeconds = H.StimulusAge;
				HS.Loudness = FMath::Clamp(H.Loudness, 0.f, 1.f);
				HS.RelYawDegrees = H.RelativeYawDeg;
				HS.SourceActorId = SourceId;
				Obs.HeardSounds.Add(MoveTemp(HS));
			}
		}
		Req.Observations.Add(MoveTemp(Obs));
	}

	if (Req.Observations.Num() == 0) { return; }

	FAILiveProjectBrainHttpClient* Client = Session->GetClient();
	const FString GameId = Session->GetGameId();
	bPushInFlight = true;
	TWeakObjectPtr<UAILiveProjectWorldStateCollector> WeakThis(this);
	Client->PushWorldState(GameId, Req).Next([WeakThis](bool /*bOk*/)
	{
		AsyncTask(ENamedThreads::GameThread, [WeakThis]()
		{
			if (UAILiveProjectWorldStateCollector* This = WeakThis.Get())
			{
				This->bPushInFlight = false;
			}
		});
	});
}
