#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "AILiveProtocolTypes.h"
#include "AILiveProjectActionDispatcher.generated.h"

class APawn;
class UObject;

USTRUCT()
struct FAIL_ActiveAction
{
	GENERATED_BODY()

	int64 IntentSeq = 0;
	FName Name = NAME_None;   // "move_to" / "sit" / "wait"
	double StartedAtSeconds = 0.0;

	UPROPERTY()
	TWeakObjectPtr<APawn> Pawn;

	UPROPERTY()
	TObjectPtr<UObject> Watcher;
};

/**
 * Receives action.intent events from BrainService WebSocket, validates via
 * IngressValidator, and routes to physical primitives:
 *   - move_to: SandboxCharacter_Mover.MoveAndLookAtLocation BP function (reflection)
 *   - sit:     AIC_NPC_SmartObject.UseSmartObjectAndNotify BP function (reflection)
 *   - wait:    no-op + immediate completion
 *
 * Overlap-cancel double protection (memory_principles §5.4.2):
 *   1. New intent for same actor -> CancelSilently the old watcher first
 *      (disarms; Tick / delegates short-circuit on bArmed=false).
 *   2. OnActionTerminated entry compares (IntentSeq vs ActiveByActorId
 *      current entry) and drops if not matching.
 * Either layer alone can be raced; both together guarantee no displaced
 * intent reports a result. Brain emits action.cancelled in the same
 * transaction as the new action.intent — never both action.cancelled and
 * action.resolved for the same intent.
 */
UCLASS()
class AILIVEPROJECT_API UAILiveProjectActionDispatcher : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	void StartPolling();
	void StopPolling();
	void HandleBrainActionIntent(const FAIL_ActionIntentEvent& Ev);
	void HandleBrainActionCancelled(int64 CancelSeq, int64 SourceIntentSeq, const FString& ActorId);

	// WorldStateCollector queries to derive current_action.
	bool HasActionForPawn(APawn* Pawn) const;
	FName GetActionNameForPawn(APawn* Pawn) const;

	void OnActionTerminated(const FString& ActorId, int64 IntentSeq,
		E_AIL_ActionOutcome Outcome, int32 DurationMs,
		const FVector& FinalPos, const FString& ErrorReason);

	// Physical-stop helpers used by StopActiveAction; exposed for sit watcher
	// post-cancel cleanup of the SmartObject claim slot.
	void StopMovementForActor(const FString& ActorId);

	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

private:
	void TickPull();
	void DispatchOne(const FAIL_ActionIntentEvent& Ev);
	void StopActiveAction(const FString& ActorId);

	void RouteMoveTo(APawn* Pawn, const FAIL_ActionIntentEvent& Ev);
	void RouteSit(APawn* Pawn, const FAIL_ActionIntentEvent& Ev);
	void RouteWait(APawn* Pawn, const FAIL_ActionIntentEvent& Ev);

	UPROPERTY()
	TMap<FString, FAIL_ActiveAction> ActiveByActorId;

	int64 LastActionSeq = -1;
	bool bPullInFlight = false;
	FTimerHandle TimerHandle;
};
