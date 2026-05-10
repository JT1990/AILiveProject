#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "AILiveProjectSpeechResultReporter.generated.h"

/**
 * Posts TTS playback completion to BrainService. Brain writes
 * speech.playback_resolved (NOT action.resolved); the speech and action
 * channels stay completely independent in schema, endpoint, and dispatcher.
 */
UCLASS()
class AILIVEPROJECT_API UAILiveProjectSpeechResultReporter : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	void ReportSpeechSucceeded(const FString& ActorId, int64 SpeechSeq, int32 DurationMs);
	void ReportSpeechFailed(const FString& ActorId, int64 SpeechSeq, int32 DurationMs, const FString& ErrorReason);
};
