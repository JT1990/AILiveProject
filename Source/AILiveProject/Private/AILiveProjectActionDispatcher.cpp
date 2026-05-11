#include "AILiveProjectActionDispatcher.h"

#include "AILiveProjectActionCompletionWrapper.h"
#include "AILiveProjectActionResultReporter.h"
#include "AILiveProjectBrainHttpClient.h"
#include "AILiveProjectBrainSessionSubsystem.h"
#include "AILiveProjectIngressValidator.h"
#include "AILiveProjectLog.h"
#include "AILiveProjectRosterSubsystem.h"
#include "AILiveProjectSettings.h"
#include "AILiveProtocolJson.h"
#include "AILiveProtocolTypes.h"
#include "AIController.h"
#include "Async/Async.h"
#include "EngineUtils.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "Navigation/PathFollowingComponent.h"
#include "TimerManager.h"
#include "UObject/UnrealType.h"

namespace AILiveActionDispatcherImpl
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

	// SandboxCharacter_Mover BP 函数 MoveAndLookAtLocation 的参数布局：
	// 输入 MoveLocation (FVector)、LookTarget (AActor*)，输出 bSucceeded (bool)。
	// 字段顺序/类型必须 1:1 匹配 BP 签名，否则 ProcessEvent 会按 BP 期望的偏移
	// 解读未初始化字节 → BP VM 解引用垃圾指针 → AV。
	// 详见 DevLog/2026-04-29_npc_scatter_to_target.md §踩坑 8。
	struct FMoveAndLookAtLocationParams
	{
		FVector MoveLocation = FVector::ZeroVector;
		AActor* LookTarget = nullptr;
		bool bSucceeded = false;
	};

	struct FMoveAndLookAtParams
	{
		AActor* MoveTarget = nullptr;
		AActor* LookTarget = nullptr;
		bool bSucceeded = false;
	};

	bool CallBPMoveToLocation(APawn* Pawn, const FVector& Location, AActor* LookTarget)
	{
		if (!Pawn) { return false; }
		// BP MoveAndLookAtLocation 主路径需要 valid LookTarget（内部 Cast<Actor>
		// 失败直接走失败 Return 不移动）。没有 look target 时直接走
		// AIController::MoveToLocation，朝向交给 GASP 默认 face-velocity。
		if (LookTarget != nullptr)
		{
			UFunction* Func = Pawn->FindFunction(FName(TEXT("MoveAndLookAtLocation")));
			if (Func)
			{
				FMoveAndLookAtLocationParams P;
				P.MoveLocation = Location;
				P.LookTarget = LookTarget;
				Pawn->ProcessEvent(Func, &P);
				return true;
			}
		}
		if (AAIController* AIC = Pawn->GetController<AAIController>())
		{
			AIC->MoveToLocation(Location, /*AcceptanceRadius=*/ 50.f);
			return true;
		}
		return false;
	}

	bool CallBPMoveToActor(APawn* Pawn, AActor* Target, AActor* LookTarget)
	{
		if (!Pawn || !Target) { return false; }
		if (LookTarget != nullptr)
		{
			// BP 实际名是 MoveAndLookAt（不是 MoveAndLookAtActor），签名
			// (AActor* MoveTarget, AActor* LookTarget) → bool bSucceeded。
			UFunction* Func = Pawn->FindFunction(FName(TEXT("MoveAndLookAt")));
			if (Func)
			{
				FMoveAndLookAtParams P;
				P.MoveTarget = Target;
				P.LookTarget = LookTarget;
				Pawn->ProcessEvent(Func, &P);
				return true;
			}
		}
		if (AAIController* AIC = Pawn->GetController<AAIController>())
		{
			AIC->MoveToActor(Target, /*AcceptanceRadius=*/ 80.f);
			return true;
		}
		return false;
	}

	AActor* FindSmartObjectByLabelOrName(UWorld* World, const FString& LabelOrName)
	{
		if (!World) { return nullptr; }
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* A = *It;
			if (!A) { continue; }
			if (A->GetActorNameOrLabel().Equals(LabelOrName, ESearchCase::IgnoreCase) ||
				A->GetName().Equals(LabelOrName, ESearchCase::IgnoreCase))
			{
				// Caller verifies USmartObjectComponent presence before use.
				return A;
			}
		}
		return nullptr;
	}
}
using namespace AILiveActionDispatcherImpl;

void UAILiveProjectActionDispatcher::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	LastActionSeq = -1;
}

void UAILiveProjectActionDispatcher::Deinitialize()
{
	StopPolling();
	bPullInFlight = false;
	ActiveByActorId.Reset();
	Super::Deinitialize();
}

void UAILiveProjectActionDispatcher::StartPolling()
{
	UE_LOG(LogAILiveBrain, Log, TEXT("ActionDispatcher uses WebSocket push; polling timer not started"));
}

void UAILiveProjectActionDispatcher::StopPolling()
{
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(TimerHandle);
	}
}

void UAILiveProjectActionDispatcher::HandleBrainActionIntent(const FAIL_ActionIntentEvent& Ev)
{
	DispatchOne(Ev);
	if (UAILiveProjectBrainSessionSubsystem* Session = GetSession(GetWorld()))
	{
		Session->AckBrainEvent(Ev.Seq);
	}
}

void UAILiveProjectActionDispatcher::HandleBrainActionCancelled(
	int64 CancelSeq, int64 SourceIntentSeq, const FString& ActorId)
{
	if (SourceIntentSeq >= 0)
	{
		FString ActorToStop;
		for (const TPair<FString, FAIL_ActiveAction>& Pair : ActiveByActorId)
		{
			if (Pair.Value.IntentSeq == SourceIntentSeq)
			{
				ActorToStop = Pair.Key;
				break;
			}
		}
		if (!ActorToStop.IsEmpty())
		{
			StopActiveAction(ActorToStop);
		}
	}
	else if (!ActorId.IsEmpty())
	{
		StopActiveAction(ActorId);
	}

	if (UAILiveProjectBrainSessionSubsystem* Session = GetSession(GetWorld()))
	{
		Session->AckBrainEvent(CancelSeq);
	}
}

bool UAILiveProjectActionDispatcher::HasActionForPawn(APawn* Pawn) const
{
	for (const TPair<FString, FAIL_ActiveAction>& Pair : ActiveByActorId)
	{
		if (Pair.Value.Pawn == Pawn) { return true; }
	}
	return false;
}

FName UAILiveProjectActionDispatcher::GetActionNameForPawn(APawn* Pawn) const
{
	for (const TPair<FString, FAIL_ActiveAction>& Pair : ActiveByActorId)
	{
		if (Pair.Value.Pawn == Pawn) { return Pair.Value.Name; }
	}
	return NAME_None;
}

void UAILiveProjectActionDispatcher::TickPull()
{
	UWorld* World = GetWorld();
	UAILiveProjectBrainSessionSubsystem* Session = GetSession(World);
	if (!Session || !Session->IsReady() || !Session->GetClient()) { return; }
	if (bPullInFlight) { return; }
	bPullInFlight = true;

	const FString GameId = Session->GetGameId();
	TWeakObjectPtr<UAILiveProjectActionDispatcher> WeakThis(this);
	Session->GetClient()->PullActions(GameId, LastActionSeq).Next(
		[WeakThis](TOptional<FAIL_ActionPullResponse> Resp)
	{
		AsyncTask(ENamedThreads::GameThread, [WeakThis, Resp]()
		{
			UAILiveProjectActionDispatcher* This = WeakThis.Get();
			if (!This) { return; }
			This->bPullInFlight = false;
			if (!Resp.IsSet()) { return; }
			for (const FAIL_ActionIntentEvent& Ev : Resp->Events)
			{
				This->DispatchOne(Ev);
			}
			This->LastActionSeq = FMath::Max(This->LastActionSeq, Resp->NextCursor);
		});
	});
}

void UAILiveProjectActionDispatcher::DispatchOne(const FAIL_ActionIntentEvent& Ev)
{
	UWorld* World = GetWorld();
	UAILiveProjectBrainSessionSubsystem* Session = GetSession(World);
	UAILiveProjectRosterSubsystem* Roster = GetRoster(World);
	if (!Session || !Roster) { return; }

	FAIL_IngressRejectRequest Reject;
	if (!FAILiveProjectIngressValidator::ValidateActionIntent(Ev, *Roster, Reject))
	{
		UE_LOG(LogAILiveBrain, Warning,
			TEXT("Action intent rejected actor=%s seq=%lld reason=%s snippet=%s"),
			*Ev.ActorId, Ev.Seq,
			*AILiveProtocol::RejectReasonToWire(Reject.RejectReason),
			*Reject.RawMessageSnippet);
		Session->SendIngressReject(Reject);
		return;
	}

	APawn* Pawn = Roster->FindPawnByActorId(Ev.ActorId);
	if (!Pawn)
	{
		UE_LOG(LogAILiveBrain, Warning, TEXT("Action intent dropped: pawn missing for actor=%s"), *Ev.ActorId);
		return;
	}

	// Overlap-cancel layer 1: silently disarm any prior watcher and physically
	// stop the in-progress action before re-routing.
	StopActiveAction(Ev.ActorId);

	const FString& Name = Ev.Intent.Name;
	if (Name == TEXT("move_to"))      { RouteMoveTo(Pawn, Ev); }
	else if (Name == TEXT("sit"))     { RouteSit(Pawn, Ev); }
	else if (Name == TEXT("wait"))    { RouteWait(Pawn, Ev); }
}

void UAILiveProjectActionDispatcher::StopActiveAction(const FString& ActorId)
{
	FAIL_ActiveAction* Existing = ActiveByActorId.Find(ActorId);
	if (!Existing) { return; }

	if (UAILiveActionMoveWatcher* Mw = Cast<UAILiveActionMoveWatcher>(Existing->Watcher))
	{
		Mw->CancelSilently();
		StopMovementForActor(ActorId);
	}
	else if (UAILiveActionSitWatcher* Sw = Cast<UAILiveActionSitWatcher>(Existing->Watcher))
	{
		Sw->CancelSilently();
		StopMovementForActor(ActorId);
	}
	else if (UAILiveActionWaitWatcher* Ww = Cast<UAILiveActionWaitWatcher>(Existing->Watcher))
	{
		Ww->CancelSilently();
	}
	ActiveByActorId.Remove(ActorId);
}

void UAILiveProjectActionDispatcher::StopMovementForActor(const FString& ActorId)
{
	UAILiveProjectRosterSubsystem* Roster = GetRoster(GetWorld());
	APawn* Pawn = Roster ? Roster->FindPawnByActorId(ActorId) : nullptr;
	if (!Pawn) { return; }
	if (AAIController* AIC = Pawn->GetController<AAIController>())
	{
		AIC->StopMovement();
	}
}

void UAILiveProjectActionDispatcher::OnActionTerminated(
	const FString& ActorId, int64 IntentSeq,
	E_AIL_ActionOutcome Outcome, int32 DurationMs,
	const FVector& FinalPos, const FString& ErrorReason)
{
	// Overlap-cancel layer 2: only forward when the (actor_id, intent_seq) pair
	// still matches the current active action. Mismatch means a new intent
	// already overwrote this one and brain emitted action.cancelled instead.
	FAIL_ActiveAction* Existing = ActiveByActorId.Find(ActorId);
	if (!Existing || Existing->IntentSeq != IntentSeq)
	{
		UE_LOG(LogAILiveBrain, Verbose,
			TEXT("OnActionTerminated dropped (stale) actor=%s seq=%lld"),
			*ActorId, IntentSeq);
		return;
	}
	ActiveByActorId.Remove(ActorId);

	UAILiveProjectActionResultReporter* Reporter =
		GetWorld() ? GetWorld()->GetSubsystem<UAILiveProjectActionResultReporter>() : nullptr;
	if (!Reporter) { return; }

	if (Outcome == E_AIL_ActionOutcome::Succeeded)
	{
		Reporter->ReportSucceeded(ActorId, IntentSeq, DurationMs, FinalPos);
	}
	else
	{
		Reporter->ReportFailed(ActorId, IntentSeq, DurationMs, FinalPos, ErrorReason);
	}
}

void UAILiveProjectActionDispatcher::RouteMoveTo(APawn* Pawn, const FAIL_ActionIntentEvent& Ev)
{
	UWorld* World = GetWorld();
	UAILiveProjectRosterSubsystem* Roster = GetRoster(World);
	UAILiveProjectActionResultReporter* Reporter = World ? World->GetSubsystem<UAILiveProjectActionResultReporter>() : nullptr;
	if (!Roster) { return; }

	const FAIL_MoveToParams& Mt = Ev.Intent.MoveToParams;
	bool bDispatched = false;
	if (Mt.bHasCoords)
	{
		const FVector Dest(Mt.Coords.X, Mt.Coords.Y, Mt.Coords.Z);
		bDispatched = CallBPMoveToLocation(Pawn, Dest, /*LookTarget=*/ nullptr);
	}
	else if (Mt.bHasTargetNpc)
	{
		AActor* TargetPawn = Roster->FindPawnByActorId(Mt.TargetNpc);
		bDispatched = CallBPMoveToActor(Pawn, TargetPawn, /*LookTarget=*/ nullptr);
	}
	if (!bDispatched)
	{
		if (Reporter)
		{
			Reporter->ReportFailed(Ev.ActorId, Ev.Seq, /*DurationMs=*/ 0,
				Pawn->GetActorLocation(), TEXT("move_to dispatch failed"));
		}
		return;
	}

	UAILiveActionMoveWatcher* Watcher = NewObject<UAILiveActionMoveWatcher>(this);
	Watcher->Init(Pawn->GetController<AAIController>(), Ev.ActorId, Ev.Seq, this);

	FAIL_ActiveAction Active;
	Active.IntentSeq = Ev.Seq;
	Active.Name = FName(TEXT("move_to"));
	Active.StartedAtSeconds = FPlatformTime::Seconds();
	Active.Pawn = Pawn;
	Active.Watcher = Watcher;
	ActiveByActorId.Add(Ev.ActorId, Active);
}

void UAILiveProjectActionDispatcher::RouteSit(APawn* Pawn, const FAIL_ActionIntentEvent& Ev)
{
	UWorld* World = GetWorld();
	UAILiveProjectActionResultReporter* Reporter = World ? World->GetSubsystem<UAILiveProjectActionResultReporter>() : nullptr;

	AActor* SmartObject = FindSmartObjectByLabelOrName(World, Ev.Intent.SitParams.TargetSmartObject);
	if (!SmartObject)
	{
		UE_LOG(LogAILiveBrain, Warning, TEXT("sit: target_smartobject '%s' not found in level"),
			*Ev.Intent.SitParams.TargetSmartObject);
		if (Reporter)
		{
			Reporter->ReportFailed(Ev.ActorId, Ev.Seq, /*DurationMs=*/ 0,
				Pawn->GetActorLocation(), TEXT("target_smartobject not found"));
		}
		return;
	}

	AAIController* AIC = Pawn->GetController<AAIController>();
	if (!AIC)
	{
		if (Reporter)
		{
			Reporter->ReportFailed(Ev.ActorId, Ev.Seq, /*DurationMs=*/ 0,
				Pawn->GetActorLocation(), TEXT("sit: AIController missing"));
		}
		return;
	}

	UAILiveActionSitWatcher* Watcher = NewObject<UAILiveActionSitWatcher>(this);
	Watcher->Init(Ev.ActorId, Ev.Seq, this, Pawn);

	UFunction* Func = AIC->FindFunction(FName(TEXT("UseSmartObjectAndNotify")));
	if (!Func)
	{
		UE_LOG(LogAILiveBrain, Warning,
			TEXT("sit: AIC_NPC_SmartObject lacks UseSmartObjectAndNotify BP function (Stage E BP wiring not done?)"));
		if (Reporter)
		{
			Reporter->ReportFailed(Ev.ActorId, Ev.Seq, /*DurationMs=*/ 0,
				Pawn->GetActorLocation(), TEXT("sit: BP wrapper missing"));
		}
		return;
	}
	struct FUseSmartObjectAndNotifyParms
	{
		AActor* SmartObjectActor;
		int64 IntentSeq;
		UObject* NotifyTarget;
	};
	FUseSmartObjectAndNotifyParms P{ SmartObject, Ev.Seq, Watcher };
	AIC->ProcessEvent(Func, &P);

	FAIL_ActiveAction Active;
	Active.IntentSeq = Ev.Seq;
	Active.Name = FName(TEXT("sit"));
	Active.StartedAtSeconds = FPlatformTime::Seconds();
	Active.Pawn = Pawn;
	Active.Watcher = Watcher;
	ActiveByActorId.Add(Ev.ActorId, Active);
}

void UAILiveProjectActionDispatcher::RouteWait(APawn* Pawn, const FAIL_ActionIntentEvent& Ev)
{
	UAILiveActionWaitWatcher* Watcher = NewObject<UAILiveActionWaitWatcher>(this);

	FAIL_ActiveAction Active;
	Active.IntentSeq = Ev.Seq;
	Active.Name = FName(TEXT("wait"));
	Active.StartedAtSeconds = FPlatformTime::Seconds();
	Active.Pawn = Pawn;
	Active.Watcher = Watcher;
	ActiveByActorId.Add(Ev.ActorId, Active);

	// Init triggers a SetTimerForNextTick that fires DoFire() -> OnActionTerminated.
	Watcher->Init(Ev.ActorId, Ev.Seq, this, Pawn);
}
