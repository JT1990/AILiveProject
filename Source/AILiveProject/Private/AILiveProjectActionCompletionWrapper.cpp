#include "AILiveProjectActionCompletionWrapper.h"

#include "AILiveProjectActionDispatcher.h"
#include "AILiveProjectLog.h"
#include "AIController.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "Navigation/PathFollowingComponent.h"
#include "TimerManager.h"

// ---- Move watcher --------------------------------------------------------

void UAILiveActionMoveWatcher::Init(AAIController* InAIC, const FString& InActorId,
	int64 InIntentSeq, UAILiveProjectActionDispatcher* InOwner)
{
	AIC = InAIC;
	ActorId = InActorId;
	IntentSeq = InIntentSeq;
	Owner = InOwner;
	StartedAt = FPlatformTime::Seconds();
	bArmed = true;
	bWaitingForFirstTick = true;
}

void UAILiveActionMoveWatcher::CancelSilently()
{
	bArmed = false;
}

TStatId UAILiveActionMoveWatcher::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UAILiveActionMoveWatcher, STATGROUP_Tickables);
}

void UAILiveActionMoveWatcher::Tick(float)
{
	if (!bArmed) { return; }
	AAIController* C = AIC.Get();
	UAILiveProjectActionDispatcher* O = Owner.Get();
	APawn* Pawn = C ? C->GetPawn() : nullptr;
	if (!C || !O || !Pawn)
	{
		bArmed = false;
		return;
	}
	const EPathFollowingStatus::Type Status = C->GetMoveStatus();
	// First tick can land before MoveTo has even started; only treat Idle as
	// completion after we've seen Moving/Paused at least once, OR after a
	// 200ms grace period.
	const double NowMs = (FPlatformTime::Seconds() - StartedAt) * 1000.0;
	if (bWaitingForFirstTick)
	{
		if (Status == EPathFollowingStatus::Moving || NowMs >= 200.0)
		{
			bWaitingForFirstTick = false;
		}
		else if (Status == EPathFollowingStatus::Idle)
		{
			return;
		}
	}
	if (Status == EPathFollowingStatus::Idle)
	{
		bArmed = false;
		const int32 DurationMs = static_cast<int32>(NowMs);
		O->OnActionTerminated(ActorId, IntentSeq,
			E_AIL_ActionOutcome::Succeeded, DurationMs,
			Pawn->GetActorLocation(), FString());
	}
}

// ---- Sit watcher ---------------------------------------------------------

void UAILiveActionSitWatcher::Init(const FString& InActorId, int64 InIntentSeq,
	UAILiveProjectActionDispatcher* InOwner, APawn* InPawn)
{
	ActorId = InActorId;
	IntentSeq = InIntentSeq;
	Owner = InOwner;
	Pawn = InPawn;
	StartedAt = FPlatformTime::Seconds();
	bArmed = true;
}

void UAILiveActionSitWatcher::CancelSilently() { bArmed = false; }

void UAILiveActionSitWatcher::NotifySitSucceeded(int64 InIntentSeq)
{
	if (!bArmed || InIntentSeq != IntentSeq) { return; }
	bArmed = false;
	UAILiveProjectActionDispatcher* O = Owner.Get();
	APawn* P = Pawn.Get();
	if (!O || !P) { return; }
	const int32 DurationMs = static_cast<int32>((FPlatformTime::Seconds() - StartedAt) * 1000.0);
	O->OnActionTerminated(ActorId, IntentSeq,
		E_AIL_ActionOutcome::Succeeded, DurationMs,
		P->GetActorLocation(), FString());
}

void UAILiveActionSitWatcher::NotifySitFailed(int64 InIntentSeq, const FString& Reason)
{
	if (!bArmed || InIntentSeq != IntentSeq) { return; }
	bArmed = false;
	UAILiveProjectActionDispatcher* O = Owner.Get();
	APawn* P = Pawn.Get();
	if (!O || !P) { return; }
	const int32 DurationMs = static_cast<int32>((FPlatformTime::Seconds() - StartedAt) * 1000.0);
	O->OnActionTerminated(ActorId, IntentSeq,
		E_AIL_ActionOutcome::Failed, DurationMs,
		P->GetActorLocation(), Reason);
}

// ---- Wait watcher --------------------------------------------------------

void UAILiveActionWaitWatcher::Init(const FString& InActorId, int64 InIntentSeq,
	UAILiveProjectActionDispatcher* InOwner, APawn* InPawn)
{
	ActorId = InActorId;
	IntentSeq = InIntentSeq;
	Owner = InOwner;
	Pawn = InPawn;
	StartedAt = FPlatformTime::Seconds();
	bArmed = true;
	UWorld* World = InPawn ? InPawn->GetWorld() : nullptr;
	if (World)
	{
		World->GetTimerManager().SetTimerForNextTick(
			FTimerDelegate::CreateUObject(this, &UAILiveActionWaitWatcher::DoFire));
	}
	else
	{
		DoFire();
	}
}

void UAILiveActionWaitWatcher::CancelSilently()
{
	bArmed = false;
	if (UWorld* World = Pawn.IsValid() ? Pawn->GetWorld() : nullptr)
	{
		World->GetTimerManager().ClearTimer(TimerHandle);
	}
}

void UAILiveActionWaitWatcher::DoFire()
{
	if (!bArmed) { return; }
	bArmed = false;
	UAILiveProjectActionDispatcher* O = Owner.Get();
	APawn* P = Pawn.Get();
	if (!O || !P) { return; }
	O->OnActionTerminated(ActorId, IntentSeq,
		E_AIL_ActionOutcome::Succeeded, /*DurationMs=*/ 0,
		P->GetActorLocation(), FString());
}
