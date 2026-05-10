#include "AILiveProjectSpeechResultReporter.h"

#include "AILiveProjectBrainSessionSubsystem.h"
#include "AILiveProjectLog.h"
#include "AILiveProtocolTypes.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"

namespace
{
	void DoReport(UWorld* World, const FAIL_SpeechResultRequest& Req)
	{
		UGameInstance* GI = World ? World->GetGameInstance() : nullptr;
		UAILiveProjectBrainSessionSubsystem* Session =
			GI ? GI->GetSubsystem<UAILiveProjectBrainSessionSubsystem>() : nullptr;
		if (!Session || !Session->IsReady())
		{
			UE_LOG(LogAILiveBrain, Warning,
				TEXT("SpeechResult dropped (session not ready) actor=%s seq=%lld"),
				*Req.ActorId, Req.SpeechSeq);
			return;
		}
		Session->SendSpeechResult(Req);
	}
}

void UAILiveProjectSpeechResultReporter::ReportSpeechSucceeded(
	const FString& ActorId, int64 SpeechSeq, int32 DurationMs)
{
	FAIL_SpeechResultRequest Req;
	Req.ActorId = ActorId;
	Req.SpeechSeq = SpeechSeq;
	Req.DurationMs = DurationMs;
	Req.Status = E_AIL_SpeechStatus::Succeeded;
	DoReport(GetWorld(), Req);
}

void UAILiveProjectSpeechResultReporter::ReportSpeechFailed(
	const FString& ActorId, int64 SpeechSeq, int32 DurationMs, const FString& ErrorReason)
{
	FAIL_SpeechResultRequest Req;
	Req.ActorId = ActorId;
	Req.SpeechSeq = SpeechSeq;
	Req.DurationMs = DurationMs;
	Req.Status = E_AIL_SpeechStatus::Failed;
	Req.bHasErrorReason = true;
	Req.ErrorReason = ErrorReason;
	DoReport(GetWorld(), Req);
}
