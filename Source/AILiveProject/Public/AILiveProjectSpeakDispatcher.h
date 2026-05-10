#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "AILiveProtocolTypes.h"
#include "AILiveProjectSpeakDispatcher.generated.h"

class APawn;

/**
 * Pulls speech.public events from BrainService and routes them through
 * UMinimaxACELibrary::TriggerMinimaxSpeechFromPawnWithNoiseEx (Stage F
 * extended API with completion delegate).
 *
 * Independent of ActionDispatcher: separate polling timer, separate
 * LastSpeechSeq high-watermark, separate ActiveSpeechByActorId map. Schema
 * isolation is enforced at the brain side; UE just keeps the channels
 * disjoint.
 */
UCLASS()
class AILIVEPROJECT_API UAILiveProjectSpeakDispatcher : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	void StartPolling();
	void StopPolling();

	bool HasSpeechForPawn(APawn* Pawn) const;

	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

private:
	void TickPull();
	void DispatchOne(const FAIL_SpeechPublicEvent& Ev);

	int64 LastSpeechSeq = -1;
	bool bPullInFlight = false;
	TMap<FString, int64> ActiveSpeechByActorId;   // actor_id -> in-progress speech_seq
	TMap<FString, TWeakObjectPtr<APawn>> ActiveSpeechPawns;
	FTimerHandle TimerHandle;
};
