#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "AILiveProjectActionResultReporter.generated.h"

/**
 * Posts UE-observed natural completion of physical actions (move_to / sit /
 * wait) to BrainService. Strictly does NOT carry cancel — outcome is one of
 * {succeeded, failed}. Cancelled is written by brain in the same transaction
 * as the new intent and is mutually exclusive with action.resolved per
 * memory_principles §5.4.2.
 */
UCLASS()
class AILIVEPROJECT_API UAILiveProjectActionResultReporter : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	void ReportSucceeded(const FString& ActorId, int64 IntentSeq, int32 DurationMs, const FVector& FinalPos);
	void ReportFailed(const FString& ActorId, int64 IntentSeq, int32 DurationMs, const FVector& FinalPos, const FString& ErrorReason);
};
