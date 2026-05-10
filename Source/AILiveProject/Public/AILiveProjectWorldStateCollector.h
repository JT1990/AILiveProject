#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "AILiveProjectWorldStateCollector.generated.h"

/**
 * Periodically pushes a transport-form world snapshot to the BrainService.
 *
 * Implementation reuses UAILiveProjectPerceptionLogger gather APIs verbatim
 * (no rewrite of perception). For each registered Pawn (via the Roster
 * subsystem), the collector assembles one FAIL_NpcObservation:
 *   - position from APawn::GetActorLocation()
 *   - facing_degrees from rotation Yaw
 *   - sighted_actors / heard_sounds via PerceptionLogger (whose returns are
 *     BP class FName entries; reverse-mapped through Roster to actor_id)
 *   - current_action heuristic: ActionDispatcher / SpeakDispatcher signals
 *     fall back to Idle when neither claims the Pawn.
 *
 * The collector emits a single transport request per tick; brain fans the
 * payload out into per-event rows (sight / hearing / actor_state / sample).
 *
 * ENTER/EXIT diff is intentionally NOT collected — the current schema does not
 * carry that field; brain derives ENTER/EXIT from successive sample diffs.
 */
UCLASS()
class AILIVEPROJECT_API UAILiveProjectWorldStateCollector : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	void StartPolling();
	void StopPolling();

	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

private:
	void TickPush();

	int32 NextClientSampleId = 0;
	bool bPushInFlight = false;
	FTimerHandle TimerHandle;
};
