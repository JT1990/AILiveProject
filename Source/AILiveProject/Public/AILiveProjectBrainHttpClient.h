#pragma once

#include "CoreMinimal.h"
#include "AILiveProtocolTypes.h"
#include "Templates/SharedPointer.h"
#include "Async/Future.h"

class IHttpRequest;
using FHttpRequestPtr = TSharedPtr<IHttpRequest, ESPMode::ThreadSafe>;

/**
 * Async HTTP client wrapping BrainService health and legacy debug routes.
 *
 * Owned by UAILiveProjectBrainSessionSubsystem (one per GameInstance). Not a
 * UObject — TPimplPtr held by the subsystem.
 *
 * Headers automatically injected on every POST:
 *   - Authorization: Bearer <ApiToken>           (except Health which has no auth)
 *   - Content-Type: application/json
 *   - Idempotency-Key: <fresh uuid v4 per call>  (except CreateSession)
 *
 * Retry policy: 5xx + network errors retried with exponential backoff
 * (BackoffBaseSec * 2^attempt) up to MaxRetries; the same Idempotency-Key
 * is reused across retries. 4xx errors fail fast.
 *
 * All futures resolve on the GameThread (FHttpModule completion is GameThread).
 */
class AILIVEPROJECT_API FAILiveProjectBrainHttpClient
{
public:
	FAILiveProjectBrainHttpClient(FString InBaseUrl, FString InApiToken,
		int32 InTimeoutMs, int32 InMaxRetries, float InBackoffBaseSec);
	~FAILiveProjectBrainHttpClient();

	TFuture<TOptional<FAIL_HealthResponse>>          Health();
	TFuture<TOptional<FAIL_SessionCreateResponse>>   CreateSession(const FAIL_SessionCreateRequest& Req);
	TFuture<TOptional<FAIL_RosterRegisterResponse>>  RegisterRoster(const FString& GameId, const FAIL_RosterRegisterRequest& Req);
	TFuture<bool>                                    PushWorldState(const FString& GameId, const FAIL_WorldStatePushRequest& Req);
	TFuture<TOptional<FAIL_ActionPullResponse>>      PullActions(const FString& GameId, int64 SinceSeq);
	TFuture<bool>                                    PostActionResult(const FString& GameId, const FAIL_ActionResultRequest& Req);
	TFuture<TOptional<FAIL_SpeechPullResponse>>      PullSpeech(const FString& GameId, int64 SinceSeq);
	TFuture<bool>                                    PostSpeechResult(const FString& GameId, const FAIL_SpeechResultRequest& Req);
	TFuture<bool>                                    PostIngressReject(const FString& GameId, const FAIL_IngressRejectRequest& Req);
	TFuture<TOptional<FAIL_EventQueryResponse>>      QueryEvents(const FString& GameId, int64 SinceSeq);

	/** Cancel all in-flight requests (call from PIE EndPlay / Deinitialize). */
	void CancelAllInFlight();

private:
	struct FRetryContext
	{
		FString Verb;
		FString Url;
		FString Body;
		FString IdempotencyKey;   // empty when not applicable
		bool    bAuthRequired = true;
		int32   AttemptsRemaining = 0;
		float   NextDelaySec = 0.f;
	};

	FHttpRequestPtr BuildRequest(const FRetryContext& Ctx) const;
	void DispatchWithRetry(FRetryContext Ctx, TFunction<void(int32 StatusCode, const FString& Body, bool bNetworkOk)> OnDone);

	FString BaseUrl;
	FString ApiToken;
	int32   TimeoutMs;
	int32   MaxRetries;
	float   BackoffBaseSec;

	TArray<TWeakPtr<IHttpRequest, ESPMode::ThreadSafe>> InFlight;

	// Lifeguard sentinel: lambdas posted to HTTP completion / retry timers /
	// AsyncTask capture a TWeakPtr<uint8> aliased to this and bail out when it
	// expires. Without this, Subsystem::Deinitialize -> Client.Reset() leaves
	// async OnProcessRequestComplete callbacks and pending retry timers with a
	// dangling raw `this`, crashing with 0xffffffffffffffff in DispatchWithRetry.
	TSharedPtr<uint8, ESPMode::ThreadSafe> LifeGuard;
};
