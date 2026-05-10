#include "AILiveProjectActionResultReporter.h"

#include "AILiveProjectBrainHttpClient.h"
#include "AILiveProjectBrainSessionSubsystem.h"
#include "AILiveProjectLog.h"
#include "AILiveProtocolTypes.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"

namespace
{
	void DoReport(UWorld* World, const FAIL_ActionResultRequest& Req)
	{
		UGameInstance* GI = World ? World->GetGameInstance() : nullptr;
		UAILiveProjectBrainSessionSubsystem* Session =
			GI ? GI->GetSubsystem<UAILiveProjectBrainSessionSubsystem>() : nullptr;
		if (!Session || !Session->IsReady() || !Session->GetClient())
		{
			UE_LOG(LogAILiveBrain, Warning,
				TEXT("ActionResult dropped (session not ready) actor=%s seq=%lld"),
				*Req.ActorId, Req.IntentSeq);
			return;
		}
		Session->GetClient()->PostActionResult(Session->GetGameId(), Req).Next([](bool) {});
	}
}

void UAILiveProjectActionResultReporter::ReportSucceeded(
	const FString& ActorId, int64 IntentSeq, int32 DurationMs, const FVector& FinalPos)
{
	FAIL_ActionResultRequest Req;
	Req.ActorId = ActorId;
	Req.IntentSeq = IntentSeq;
	Req.DurationMs = DurationMs;
	Req.Outcome = E_AIL_ActionOutcome::Succeeded;
	Req.FinalPosition.X = FinalPos.X;
	Req.FinalPosition.Y = FinalPos.Y;
	Req.FinalPosition.Z = FinalPos.Z;
	DoReport(GetWorld(), Req);
}

void UAILiveProjectActionResultReporter::ReportFailed(
	const FString& ActorId, int64 IntentSeq, int32 DurationMs,
	const FVector& FinalPos, const FString& ErrorReason)
{
	FAIL_ActionResultRequest Req;
	Req.ActorId = ActorId;
	Req.IntentSeq = IntentSeq;
	Req.DurationMs = DurationMs;
	Req.Outcome = E_AIL_ActionOutcome::Failed;
	Req.FinalPosition.X = FinalPos.X;
	Req.FinalPosition.Y = FinalPos.Y;
	Req.FinalPosition.Z = FinalPos.Z;
	Req.bHasErrorReason = true;
	Req.ErrorReason = ErrorReason;
	DoReport(GetWorld(), Req);
}
