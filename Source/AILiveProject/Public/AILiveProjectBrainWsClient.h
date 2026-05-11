#pragma once

#include "CoreMinimal.h"
#include "AILiveProtocolTypes.h"
#include "Templates/SharedPointer.h"

class IWebSocket;

/**
 * Runtime WebSocket client for the BrainService v0.3 transport.
 *
 * UE is the WebSocket client. BrainService remains the server and can push
 * action/speech events back over the same connection after UE connects.
 * All callbacks are dispatched on the GameThread.
 */
class AILIVEPROJECT_API FAILiveProjectBrainWsClient
{
public:
	using FConnectedHandler = TFunction<void()>;
	using FClosedHandler = TFunction<void()>;
	using FSessionCreatedHandler = TFunction<void(const FAIL_SessionCreateResponse&)>;
	using FRosterAcceptedHandler = TFunction<void(const FAIL_RosterRegisterResponse&)>;
	using FActionIntentHandler = TFunction<void(const FAIL_ActionIntentEvent&)>;
	using FActionCancelledHandler = TFunction<void(int64 CancelSeq, int64 SourceIntentSeq, const FString& ActorId)>;
	using FSpeechPublicHandler = TFunction<void(const FAIL_SpeechPublicEvent&)>;

	FAILiveProjectBrainWsClient(FString InWsUrl, FString InApiToken);
	~FAILiveProjectBrainWsClient();

	void SetOnConnected(FConnectedHandler Handler);
	void SetOnClosed(FClosedHandler Handler);
	void SetOnSessionCreated(FSessionCreatedHandler Handler);
	void SetOnRosterAccepted(FRosterAcceptedHandler Handler);
	void SetOnActionIntent(FActionIntentHandler Handler);
	void SetOnActionCancelled(FActionCancelledHandler Handler);
	void SetOnSpeechPublic(FSpeechPublicHandler Handler);

	void Connect();
	void Close();
	bool IsConnected() const;

	bool SendSessionCreate(const FAIL_SessionCreateRequest& Req);
	bool SendSessionResume(const FString& GameId, int64 LastBrainSeq);
	bool SendRosterRegister(const FString& GameId, const FAIL_RosterRegisterRequest& Req);
	bool SendWorldState(const FString& GameId, const FAIL_WorldStatePushRequest& Req);
	bool SendActionResult(const FString& GameId, const FAIL_ActionResultRequest& Req);
	bool SendSpeechResult(const FString& GameId, const FAIL_SpeechResultRequest& Req);
	bool SendIngressReject(const FString& GameId, const FAIL_IngressRejectRequest& Req);
	bool AckEvent(const FString& GameId, int64 Seq);
	bool SendRoundStartLLMPhase(const FString& GameId, int32 RoundNo, const FString& Phase, int32 NTicks);

	int64 GetLastBrainSeq() const;

private:
	struct FState;

	static void HandleMessage(const TSharedRef<FState>& State, const FString& Message);
	bool SendEnvelope(const FString& Type, const FString& GameId, const FString& PayloadJson);

	TSharedRef<FState> State;
};
