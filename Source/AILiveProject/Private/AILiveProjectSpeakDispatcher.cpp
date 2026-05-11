#include "AILiveProjectSpeakDispatcher.h"

#include "AILiveProjectBrainHttpClient.h"
#include "AILiveProjectBrainSessionSubsystem.h"
#include "AILiveProjectIngressValidator.h"
#include "AILiveProjectLog.h"
#include "AILiveProjectRosterSubsystem.h"
#include "AILiveProjectSettings.h"
#include "AILiveProjectSpeechResultReporter.h"
#include "AILiveProtocolJson.h"
#include "AILiveProtocolTypes.h"
#include "MinimaxACELibrary.h"
#include "Async/Async.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "TimerManager.h"

namespace AILiveSpeakDispatcherImpl
{
	UAILiveProjectBrainSessionSubsystem* GetSession(UWorld* World)
	{
		UGameInstance* GI = World ? World->GetGameInstance() : nullptr;
		return GI ? GI->GetSubsystem<UAILiveProjectBrainSessionSubsystem>() : nullptr;
	}

	UAILiveProjectRosterSubsystem* GetRoster(UWorld* World)
	{
		UGameInstance* GI = World ? World->GetGameInstance() : nullptr;
		return GI ? GI->GetSubsystem<UAILiveProjectRosterSubsystem>() : nullptr;
	}

	FString ResolveMinimaxApiKey()
	{
		const UAILiveProjectSettings* Settings = GetDefault<UAILiveProjectSettings>();
		if (!Settings->MinimaxApiKey.IsEmpty()) { return Settings->MinimaxApiKey; }
		return UMinimaxACELibrary::GetMinimaxApiKeyFromProjectEnv();
	}
}
// Intentionally no file-scope `using namespace`: Unity Build merges multiple .cpp
// into one TU and would pull AILiveActionDispatcherImpl::GetSession into ambiguity
// with the same name from AILiveSpeakDispatcherImpl. Call sites use full qualification.

void UAILiveProjectSpeakDispatcher::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	LastSpeechSeq = -1;
}

void UAILiveProjectSpeakDispatcher::Deinitialize()
{
	StopPolling();
	bPullInFlight = false;
	ActiveSpeechByActorId.Reset();
	ActiveSpeechPawns.Reset();
	Super::Deinitialize();
}

void UAILiveProjectSpeakDispatcher::StartPolling()
{
	UE_LOG(LogAILiveBrain, Log, TEXT("SpeakDispatcher uses WebSocket push; polling timer not started"));
}

void UAILiveProjectSpeakDispatcher::StopPolling()
{
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(TimerHandle);
	}
}

void UAILiveProjectSpeakDispatcher::HandleBrainSpeechPublic(const FAIL_SpeechPublicEvent& Ev)
{
	DispatchOne(Ev);
	if (UAILiveProjectBrainSessionSubsystem* Session = AILiveSpeakDispatcherImpl::GetSession(GetWorld()))
	{
		Session->AckBrainEvent(Ev.Seq);
	}
}

bool UAILiveProjectSpeakDispatcher::HasSpeechForPawn(APawn* Pawn) const
{
	for (const TPair<FString, TWeakObjectPtr<APawn>>& Pair : ActiveSpeechPawns)
	{
		if (Pair.Value == Pawn) { return true; }
	}
	return false;
}

void UAILiveProjectSpeakDispatcher::TickPull()
{
	UWorld* World = GetWorld();
	UAILiveProjectBrainSessionSubsystem* Session = AILiveSpeakDispatcherImpl::GetSession(World);
	if (!Session || !Session->IsReady() || !Session->GetClient()) { return; }
	if (bPullInFlight) { return; }
	bPullInFlight = true;

	const FString GameId = Session->GetGameId();
	TWeakObjectPtr<UAILiveProjectSpeakDispatcher> WeakThis(this);
	Session->GetClient()->PullSpeech(GameId, LastSpeechSeq).Next(
		[WeakThis](TOptional<FAIL_SpeechPullResponse> Resp)
	{
		AsyncTask(ENamedThreads::GameThread, [WeakThis, Resp]()
		{
			UAILiveProjectSpeakDispatcher* This = WeakThis.Get();
			if (!This) { return; }
			This->bPullInFlight = false;
			if (!Resp.IsSet()) { return; }
			for (const FAIL_SpeechPublicEvent& Ev : Resp->Events)
			{
				This->DispatchOne(Ev);
			}
			This->LastSpeechSeq = FMath::Max(This->LastSpeechSeq, Resp->NextCursor);
		});
	});
}

void UAILiveProjectSpeakDispatcher::DispatchOne(const FAIL_SpeechPublicEvent& Ev)
{
	UWorld* World = GetWorld();
	UAILiveProjectBrainSessionSubsystem* Session = AILiveSpeakDispatcherImpl::GetSession(World);
	UAILiveProjectRosterSubsystem* Roster = AILiveSpeakDispatcherImpl::GetRoster(World);
	UAILiveProjectSpeechResultReporter* Reporter = World ? World->GetSubsystem<UAILiveProjectSpeechResultReporter>() : nullptr;
	if (!Session || !Roster) { return; }

	FAIL_IngressRejectRequest Reject;
	if (!FAILiveProjectIngressValidator::ValidateSpeechPublic(Ev, *Roster, Reject))
	{
		UE_LOG(LogAILiveBrain, Warning,
			TEXT("Speech rejected actor=%s seq=%lld reason=%s"),
			*Ev.ActorId, Ev.Seq,
			*AILiveProtocol::RejectReasonToWire(Reject.RejectReason));
		Session->SendIngressReject(Reject);
		return;
	}

	APawn* Pawn = Roster->FindPawnByActorId(Ev.ActorId);
	if (!Pawn)
	{
		if (Reporter)
		{
			Reporter->ReportSpeechFailed(Ev.ActorId, Ev.Seq, /*DurationMs=*/ 0,
				TEXT("speaker pawn missing"));
		}
		return;
	}

	const FString ApiKey = AILiveSpeakDispatcherImpl::ResolveMinimaxApiKey();
	if (ApiKey.IsEmpty())
	{
		if (Reporter)
		{
			Reporter->ReportSpeechFailed(Ev.ActorId, Ev.Seq, /*DurationMs=*/ 0,
				TEXT("Minimax API key unavailable"));
		}
		return;
	}

	ActiveSpeechByActorId.Add(Ev.ActorId, Ev.Seq);
	ActiveSpeechPawns.Add(Ev.ActorId, Pawn);

	TWeakObjectPtr<UAILiveProjectSpeakDispatcher> WeakThis(this);
	const FString ActorId = Ev.ActorId;
	const int64 SpeechSeq = Ev.Seq;

	FOnSpeechCompleted OnComplete;
	OnComplete.BindWeakLambda(this,
		[WeakThis, ActorId, SpeechSeq](bool bSucceeded, float DurationSeconds, FString ErrorReason)
	{
		UAILiveProjectSpeakDispatcher* This = WeakThis.Get();
		if (!This) { return; }
		This->ActiveSpeechByActorId.Remove(ActorId);
		This->ActiveSpeechPawns.Remove(ActorId);
		UAILiveProjectSpeechResultReporter* R =
			This->GetWorld() ? This->GetWorld()->GetSubsystem<UAILiveProjectSpeechResultReporter>() : nullptr;
		if (!R) { return; }
		const int32 DurationMs = static_cast<int32>(DurationSeconds * 1000.f);
		if (bSucceeded)
		{
			R->ReportSpeechSucceeded(ActorId, SpeechSeq, DurationMs);
		}
		else
		{
			R->ReportSpeechFailed(ActorId, SpeechSeq, DurationMs, ErrorReason);
		}
	});

	UMinimaxACELibrary::TriggerMinimaxSpeechFromPawnWithNoiseEx(
		this, Pawn, Ev.Text, ApiKey, OnComplete);
}
