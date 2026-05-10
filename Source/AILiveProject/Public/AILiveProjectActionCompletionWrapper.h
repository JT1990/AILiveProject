#pragma once

#include "CoreMinimal.h"
#include "Tickable.h"
#include "UObject/Object.h"
#include "AILiveProjectActionCompletionWrapper.generated.h"

class AAIController;
class UAILiveProjectActionDispatcher;

/**
 * Polls AAIController::GetMoveStatus until the move resolves; raises
 * OnActionTerminated through the owning ActionDispatcher (which performs the
 * IntentSeq match before forwarding to ActionResultReporter).
 *
 * CancelSilently disarms the watcher: Tick / completion delegates short-circuit
 * via bArmed. Combined with the dispatcher's IntentSeq guard this gives the
 * "double protection" against the action.cancelled / action.resolved double-
 * terminal-state race (memory_principles §5.4.2).
 */
UCLASS()
class AILIVEPROJECT_API UAILiveActionMoveWatcher : public UObject, public FTickableGameObject
{
	GENERATED_BODY()

public:
	void Init(AAIController* InAIC, const FString& InActorId, int64 InIntentSeq, UAILiveProjectActionDispatcher* InOwner);
	void CancelSilently();

	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	virtual bool IsTickable() const override { return bArmed; }
	virtual bool IsTickableWhenPaused() const override { return false; }

private:
	TWeakObjectPtr<AAIController> AIC;
	TWeakObjectPtr<UAILiveProjectActionDispatcher> Owner;
	FString ActorId;
	int64 IntentSeq = 0;
	double StartedAt = 0.0;
	bool bArmed = true;
	bool bWaitingForFirstTick = true;
};

/**
 * Notification target for the AIC_NPC_SmartObject BP wrapper
 * `UseSmartObjectAndNotify`. The BP path calls NotifySitSucceeded /
 * NotifySitFailed back into this watcher; both short-circuit when bArmed=false.
 */
UCLASS()
class AILIVEPROJECT_API UAILiveActionSitWatcher : public UObject
{
	GENERATED_BODY()

public:
	void Init(const FString& InActorId, int64 InIntentSeq, UAILiveProjectActionDispatcher* InOwner, APawn* InPawn);
	void CancelSilently();

	UFUNCTION(BlueprintCallable, Category = "AILive|SmartObject")
	void NotifySitSucceeded(int64 InIntentSeq);

	UFUNCTION(BlueprintCallable, Category = "AILive|SmartObject")
	void NotifySitFailed(int64 InIntentSeq, const FString& Reason);

private:
	TWeakObjectPtr<UAILiveProjectActionDispatcher> Owner;
	TWeakObjectPtr<APawn> Pawn;
	FString ActorId;
	int64 IntentSeq = 0;
	double StartedAt = 0.0;
	bool bArmed = true;
};

/**
 * MVP wait watcher: completes on the next world tick (duration_ms=0). Future
 * iterations may add a configurable duration parameter from FAIL_WaitParams.
 */
UCLASS()
class AILIVEPROJECT_API UAILiveActionWaitWatcher : public UObject
{
	GENERATED_BODY()

public:
	void Init(const FString& InActorId, int64 InIntentSeq, UAILiveProjectActionDispatcher* InOwner, APawn* InPawn);
	void CancelSilently();

private:
	void DoFire();

	TWeakObjectPtr<UAILiveProjectActionDispatcher> Owner;
	TWeakObjectPtr<APawn> Pawn;
	FString ActorId;
	int64 IntentSeq = 0;
	double StartedAt = 0.0;
	FTimerHandle TimerHandle;
	bool bArmed = true;
};
