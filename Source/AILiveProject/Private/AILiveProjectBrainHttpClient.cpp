#include "AILiveProjectBrainHttpClient.h"

#include "AILiveProjectLog.h"
#include "AILiveProtocolJson.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Async/Async.h"
#include "TimerManager.h"
#include "Engine/Engine.h"
#include "Engine/World.h"

namespace
{
	bool ShouldRetry(int32 StatusCode, bool bNetworkOk)
	{
		if (!bNetworkOk) { return true; }
		return StatusCode >= 500 && StatusCode < 600;
	}

	/**
	 * RAII wrapper around TPromise that auto-fulfills with a default value if
	 * destroyed before SetValue is called. Without this, when a callback
	 * lambda capturing the Promise is destroyed unfulfilled (e.g. PIE end /
	 * CancelAllInFlight tears down OnProcessRequestComplete delegates without
	 * firing them), TPromise's destructor asserts on State->IsComplete().
	 */
	template <typename T>
	struct TAILivePromiseGuard
	{
		TPromise<T> Promise;
		T DefaultValue;
		bool bFulfilled = false;

		explicit TAILivePromiseGuard(T InDefault) : DefaultValue(MoveTemp(InDefault)) {}

		void SetValue(T Value)
		{
			if (!bFulfilled)
			{
				Promise.SetValue(MoveTemp(Value));
				bFulfilled = true;
			}
		}

		~TAILivePromiseGuard()
		{
			if (!bFulfilled)
			{
				Promise.SetValue(MoveTemp(DefaultValue));
			}
		}
	};
}

FAILiveProjectBrainHttpClient::FAILiveProjectBrainHttpClient(
	FString InBaseUrl, FString InApiToken,
	int32 InTimeoutMs, int32 InMaxRetries, float InBackoffBaseSec)
	: BaseUrl(MoveTemp(InBaseUrl))
	, ApiToken(MoveTemp(InApiToken))
	, TimeoutMs(InTimeoutMs)
	, MaxRetries(InMaxRetries)
	, BackoffBaseSec(InBackoffBaseSec)
	, LifeGuard(MakeShared<uint8, ESPMode::ThreadSafe>(0))
{
	while (BaseUrl.EndsWith(TEXT("/"))) { BaseUrl.LeftChopInline(1); }
	UE_LOG(LogAILiveBrain, Log, TEXT("HTTP client base_url=%s"), *BaseUrl);
	if (!BaseUrl.Contains(TEXT("://")))
	{
		UE_LOG(LogAILiveBrain, Error,
			TEXT("BrainBaseUrl '%s' looks malformed — UE INI parser strips '//' as comment unless quoted. Quote the value in DefaultGame.ini: BrainBaseUrl=\"http://host:port\""),
			*BaseUrl);
	}
}

FAILiveProjectBrainHttpClient::~FAILiveProjectBrainHttpClient()
{
	CancelAllInFlight();
}

void FAILiveProjectBrainHttpClient::CancelAllInFlight()
{
	for (TWeakPtr<IHttpRequest, ESPMode::ThreadSafe>& Weak : InFlight)
	{
		if (TSharedPtr<IHttpRequest, ESPMode::ThreadSafe> Req = Weak.Pin())
		{
			Req->CancelRequest();
		}
	}
	InFlight.Reset();
}

FHttpRequestPtr FAILiveProjectBrainHttpClient::BuildRequest(const FRetryContext& Ctx) const
{
	FHttpRequestPtr Req = FHttpModule::Get().CreateRequest();
	Req->SetURL(Ctx.Url);
	Req->SetVerb(Ctx.Verb);
	Req->SetTimeout(TimeoutMs / 1000.0);
	if (Ctx.bAuthRequired && !ApiToken.IsEmpty())
	{
		Req->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *ApiToken));
	}
	if (!Ctx.IdempotencyKey.IsEmpty())
	{
		Req->SetHeader(TEXT("Idempotency-Key"), Ctx.IdempotencyKey);
	}
	if (Ctx.Verb == TEXT("POST") || Ctx.Verb == TEXT("PUT") || Ctx.Verb == TEXT("PATCH"))
	{
		Req->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
		Req->SetContentAsString(Ctx.Body);
	}
	return Req;
}

void FAILiveProjectBrainHttpClient::DispatchWithRetry(
	FRetryContext Ctx,
	TFunction<void(int32, const FString&, bool)> OnDone)
{
	FHttpRequestPtr Req = BuildRequest(Ctx);
	InFlight.Add(Req);

	TWeakPtr<IHttpRequest, ESPMode::ThreadSafe> WeakReq = Req;
	TWeakPtr<uint8, ESPMode::ThreadSafe> WeakLife = LifeGuard;
	Req->OnProcessRequestComplete().BindLambda(
		[this, Ctx, OnDone, WeakReq, WeakLife]
		(FHttpRequestPtr R, FHttpResponsePtr Resp, bool bSucceeded) mutable
	{
		// HttpClient may have been destroyed (Subsystem::Deinitialize ->
		// Client.Reset()) between CancelRequest and this delegate firing.
		if (!WeakLife.Pin().IsValid())
		{
			return;
		}

		// Drop weak pointer entry.
		InFlight.RemoveAll([&](const TWeakPtr<IHttpRequest, ESPMode::ThreadSafe>& W)
		{
			return !W.IsValid() || W.HasSameObject(R.Get());
		});

		const int32 Status = Resp.IsValid() ? Resp->GetResponseCode() : 0;
		const FString Body = Resp.IsValid() ? Resp->GetContentAsString() : FString();
		const bool bNetworkOk = bSucceeded && Resp.IsValid();

		if (Ctx.AttemptsRemaining > 0 && ShouldRetry(Status, bNetworkOk))
		{
			--Ctx.AttemptsRemaining;
			const float Delay = Ctx.NextDelaySec;
			Ctx.NextDelaySec *= 2.f;
			UE_LOG(LogAILiveBrain, Warning,
				TEXT("HTTP retry in %.1fs (status=%d, network_ok=%d, url=%s, attempts_left=%d)"),
				Delay, Status, bNetworkOk ? 1 : 0, *Ctx.Url, Ctx.AttemptsRemaining);

			// Schedule the retry on a global timer; if no world is available
			// (early shutdown) we fall back to a direct dispatch.
			UWorld* World = GEngine ? GEngine->GetCurrentPlayWorld() : nullptr;
			if (!World) { World = GEngine ? GEngine->GetWorldContexts().Num() > 0 ? GEngine->GetWorldContexts()[0].World() : nullptr : nullptr; }
			if (World)
			{
				FTimerHandle Handle;
				World->GetTimerManager().SetTimer(Handle,
					FTimerDelegate::CreateLambda([this, Ctx, OnDone, WeakLife]() mutable
					{
						if (!WeakLife.Pin().IsValid()) { return; }
						DispatchWithRetry(MoveTemp(Ctx), OnDone);
					}), Delay, false);
			}
			else
			{
				AsyncTask(ENamedThreads::GameThread,
					[this, Ctx = MoveTemp(Ctx), OnDone, WeakLife]() mutable
				{
					if (!WeakLife.Pin().IsValid()) { return; }
					DispatchWithRetry(MoveTemp(Ctx), OnDone);
				});
			}
			return;
		}

		OnDone(Status, Body, bNetworkOk);
	});
	Req->ProcessRequest();
}

// ---- per-endpoint --------------------------------------------------------

TFuture<TOptional<FAIL_HealthResponse>> FAILiveProjectBrainHttpClient::Health()
{
	TSharedRef<TAILivePromiseGuard<TOptional<FAIL_HealthResponse>>> Promise =
		MakeShared<TAILivePromiseGuard<TOptional<FAIL_HealthResponse>>>(TOptional<FAIL_HealthResponse>{});
	TFuture<TOptional<FAIL_HealthResponse>> Future = Promise->Promise.GetFuture();

	FRetryContext Ctx;
	Ctx.Verb = TEXT("GET");
	Ctx.Url = BaseUrl + TEXT("/health");
	Ctx.bAuthRequired = false;
	Ctx.AttemptsRemaining = MaxRetries;
	Ctx.NextDelaySec = BackoffBaseSec;

	DispatchWithRetry(MoveTemp(Ctx), [Promise](int32 Status, const FString& Body, bool)
	{
		if (Status == 200)
		{
			FAIL_HealthResponse Out;
			if (AILiveProtocol::FromJsonString(Body, Out)) { Promise->SetValue(Out); return; }
		}
		UE_LOG(LogAILiveBrain, Warning, TEXT("Health failed status=%d body=%s"), Status, *Body);
		Promise->SetValue(TOptional<FAIL_HealthResponse>());
	});
	return Future;
}

TFuture<TOptional<FAIL_SessionCreateResponse>>
FAILiveProjectBrainHttpClient::CreateSession(const FAIL_SessionCreateRequest& Req)
{
	TSharedRef<TAILivePromiseGuard<TOptional<FAIL_SessionCreateResponse>>> Promise =
		MakeShared<TAILivePromiseGuard<TOptional<FAIL_SessionCreateResponse>>>(TOptional<FAIL_SessionCreateResponse>{});
	TFuture<TOptional<FAIL_SessionCreateResponse>> Future = Promise->Promise.GetFuture();

	FString Body;
	AILiveProtocol::ToJsonString(Req, Body);

	FRetryContext Ctx;
	Ctx.Verb = TEXT("POST");
	Ctx.Url = BaseUrl + TEXT("/v1/games");
	Ctx.Body = Body;
	// Per spec, POST /v1/games does NOT require Idempotency-Key.
	Ctx.bAuthRequired = true;
	Ctx.AttemptsRemaining = MaxRetries;
	Ctx.NextDelaySec = BackoffBaseSec;

	DispatchWithRetry(MoveTemp(Ctx), [Promise](int32 Status, const FString& Resp, bool)
	{
		if (Status == 200)
		{
			FAIL_SessionCreateResponse Out;
			if (AILiveProtocol::FromJsonString(Resp, Out)) { Promise->SetValue(Out); return; }
		}
		UE_LOG(LogAILiveBrain, Warning, TEXT("CreateSession failed status=%d body=%s"), Status, *Resp);
		Promise->SetValue(TOptional<FAIL_SessionCreateResponse>());
	});
	return Future;
}

TFuture<TOptional<FAIL_RosterRegisterResponse>>
FAILiveProjectBrainHttpClient::RegisterRoster(const FString& GameId, const FAIL_RosterRegisterRequest& Req)
{
	TSharedRef<TAILivePromiseGuard<TOptional<FAIL_RosterRegisterResponse>>> Promise =
		MakeShared<TAILivePromiseGuard<TOptional<FAIL_RosterRegisterResponse>>>(TOptional<FAIL_RosterRegisterResponse>{});
	TFuture<TOptional<FAIL_RosterRegisterResponse>> Future = Promise->Promise.GetFuture();

	FString Body;
	AILiveProtocol::ToJsonString(Req, Body);

	FRetryContext Ctx;
	Ctx.Verb = TEXT("POST");
	Ctx.Url = FString::Printf(TEXT("%s/v1/games/%s/roster"), *BaseUrl, *GameId);
	Ctx.Body = Body;
	Ctx.IdempotencyKey = AILiveProtocol::NewIdempotencyKey();
	Ctx.AttemptsRemaining = MaxRetries;
	Ctx.NextDelaySec = BackoffBaseSec;

	DispatchWithRetry(MoveTemp(Ctx), [Promise](int32 Status, const FString& Resp, bool)
	{
		if (Status == 200)
		{
			FAIL_RosterRegisterResponse Out;
			if (AILiveProtocol::FromJsonString(Resp, Out)) { Promise->SetValue(Out); return; }
		}
		UE_LOG(LogAILiveBrain, Warning, TEXT("RegisterRoster failed status=%d body=%s"), Status, *Resp);
		Promise->SetValue(TOptional<FAIL_RosterRegisterResponse>());
	});
	return Future;
}

TFuture<bool> FAILiveProjectBrainHttpClient::PushWorldState(
	const FString& GameId, const FAIL_WorldStatePushRequest& Req)
{
	TSharedRef<TAILivePromiseGuard<bool>> Promise = MakeShared<TAILivePromiseGuard<bool>>(false);
	TFuture<bool> Future = Promise->Promise.GetFuture();

	FString Body;
	AILiveProtocol::ToJsonString(Req, Body);

	FRetryContext Ctx;
	Ctx.Verb = TEXT("POST");
	Ctx.Url = FString::Printf(TEXT("%s/v1/games/%s/world_state"), *BaseUrl, *GameId);
	Ctx.Body = Body;
	Ctx.IdempotencyKey = AILiveProtocol::NewIdempotencyKey();
	Ctx.AttemptsRemaining = MaxRetries;
	Ctx.NextDelaySec = BackoffBaseSec;

	DispatchWithRetry(MoveTemp(Ctx), [Promise](int32 Status, const FString& Resp, bool)
	{
		const bool bOk = (Status == 200);
		if (!bOk)
		{
			UE_LOG(LogAILiveBrain, Warning, TEXT("PushWorldState failed status=%d body=%s"), Status, *Resp);
		}
		Promise->SetValue(bOk);
	});
	return Future;
}

TFuture<TOptional<FAIL_ActionPullResponse>>
FAILiveProjectBrainHttpClient::PullActions(const FString& GameId, int64 SinceSeq)
{
	TSharedRef<TAILivePromiseGuard<TOptional<FAIL_ActionPullResponse>>> Promise =
		MakeShared<TAILivePromiseGuard<TOptional<FAIL_ActionPullResponse>>>(TOptional<FAIL_ActionPullResponse>{});
	TFuture<TOptional<FAIL_ActionPullResponse>> Future = Promise->Promise.GetFuture();

	FRetryContext Ctx;
	Ctx.Verb = TEXT("GET");
	Ctx.Url = FString::Printf(TEXT("%s/v1/games/%s/actions/pull?since_seq=%lld"),
		*BaseUrl, *GameId, SinceSeq);
	Ctx.AttemptsRemaining = MaxRetries;
	Ctx.NextDelaySec = BackoffBaseSec;

	DispatchWithRetry(MoveTemp(Ctx), [Promise](int32 Status, const FString& Resp, bool)
	{
		if (Status == 200)
		{
			FAIL_ActionPullResponse Out;
			if (AILiveProtocol::FromJsonString(Resp, Out)) { Promise->SetValue(Out); return; }
		}
		UE_LOG(LogAILiveBrain, Warning, TEXT("PullActions failed status=%d body=%s"), Status, *Resp);
		Promise->SetValue(TOptional<FAIL_ActionPullResponse>());
	});
	return Future;
}

TFuture<bool> FAILiveProjectBrainHttpClient::PostActionResult(
	const FString& GameId, const FAIL_ActionResultRequest& Req)
{
	TSharedRef<TAILivePromiseGuard<bool>> Promise = MakeShared<TAILivePromiseGuard<bool>>(false);
	TFuture<bool> Future = Promise->Promise.GetFuture();

	FString Body;
	AILiveProtocol::ToJsonString(Req, Body);

	FRetryContext Ctx;
	Ctx.Verb = TEXT("POST");
	Ctx.Url = FString::Printf(TEXT("%s/v1/games/%s/actions/result"), *BaseUrl, *GameId);
	Ctx.Body = Body;
	Ctx.IdempotencyKey = AILiveProtocol::NewIdempotencyKey();
	Ctx.AttemptsRemaining = MaxRetries;
	Ctx.NextDelaySec = BackoffBaseSec;

	DispatchWithRetry(MoveTemp(Ctx), [Promise](int32 Status, const FString& Resp, bool)
	{
		const bool bOk = (Status == 200);
		if (!bOk)
		{
			UE_LOG(LogAILiveBrain, Warning, TEXT("PostActionResult failed status=%d body=%s"), Status, *Resp);
		}
		Promise->SetValue(bOk);
	});
	return Future;
}

TFuture<TOptional<FAIL_SpeechPullResponse>>
FAILiveProjectBrainHttpClient::PullSpeech(const FString& GameId, int64 SinceSeq)
{
	TSharedRef<TAILivePromiseGuard<TOptional<FAIL_SpeechPullResponse>>> Promise =
		MakeShared<TAILivePromiseGuard<TOptional<FAIL_SpeechPullResponse>>>(TOptional<FAIL_SpeechPullResponse>{});
	TFuture<TOptional<FAIL_SpeechPullResponse>> Future = Promise->Promise.GetFuture();

	FRetryContext Ctx;
	Ctx.Verb = TEXT("GET");
	Ctx.Url = FString::Printf(TEXT("%s/v1/games/%s/speech/pull?since_seq=%lld"),
		*BaseUrl, *GameId, SinceSeq);
	Ctx.AttemptsRemaining = MaxRetries;
	Ctx.NextDelaySec = BackoffBaseSec;

	DispatchWithRetry(MoveTemp(Ctx), [Promise](int32 Status, const FString& Resp, bool)
	{
		if (Status == 200)
		{
			FAIL_SpeechPullResponse Out;
			if (AILiveProtocol::FromJsonString(Resp, Out)) { Promise->SetValue(Out); return; }
		}
		UE_LOG(LogAILiveBrain, Warning, TEXT("PullSpeech failed status=%d body=%s"), Status, *Resp);
		Promise->SetValue(TOptional<FAIL_SpeechPullResponse>());
	});
	return Future;
}

TFuture<bool> FAILiveProjectBrainHttpClient::PostSpeechResult(
	const FString& GameId, const FAIL_SpeechResultRequest& Req)
{
	TSharedRef<TAILivePromiseGuard<bool>> Promise = MakeShared<TAILivePromiseGuard<bool>>(false);
	TFuture<bool> Future = Promise->Promise.GetFuture();

	FString Body;
	AILiveProtocol::ToJsonString(Req, Body);

	FRetryContext Ctx;
	Ctx.Verb = TEXT("POST");
	Ctx.Url = FString::Printf(TEXT("%s/v1/games/%s/speech/result"), *BaseUrl, *GameId);
	Ctx.Body = Body;
	Ctx.IdempotencyKey = AILiveProtocol::NewIdempotencyKey();
	Ctx.AttemptsRemaining = MaxRetries;
	Ctx.NextDelaySec = BackoffBaseSec;

	DispatchWithRetry(MoveTemp(Ctx), [Promise](int32 Status, const FString& Resp, bool)
	{
		const bool bOk = (Status == 200);
		if (!bOk)
		{
			UE_LOG(LogAILiveBrain, Warning, TEXT("PostSpeechResult failed status=%d body=%s"), Status, *Resp);
		}
		Promise->SetValue(bOk);
	});
	return Future;
}

TFuture<bool> FAILiveProjectBrainHttpClient::PostIngressReject(
	const FString& GameId, const FAIL_IngressRejectRequest& Req)
{
	TSharedRef<TAILivePromiseGuard<bool>> Promise = MakeShared<TAILivePromiseGuard<bool>>(false);
	TFuture<bool> Future = Promise->Promise.GetFuture();

	FString Body;
	AILiveProtocol::ToJsonString(Req, Body);

	FRetryContext Ctx;
	Ctx.Verb = TEXT("POST");
	Ctx.Url = FString::Printf(TEXT("%s/v1/games/%s/ingress_reject"), *BaseUrl, *GameId);
	Ctx.Body = Body;
	Ctx.IdempotencyKey = AILiveProtocol::NewIdempotencyKey();
	Ctx.AttemptsRemaining = MaxRetries;
	Ctx.NextDelaySec = BackoffBaseSec;

	DispatchWithRetry(MoveTemp(Ctx), [Promise](int32 Status, const FString& Resp, bool)
	{
		const bool bOk = (Status == 200);
		if (!bOk)
		{
			UE_LOG(LogAILiveBrain, Warning, TEXT("PostIngressReject failed status=%d body=%s"), Status, *Resp);
		}
		Promise->SetValue(bOk);
	});
	return Future;
}

TFuture<TOptional<FAIL_EventQueryResponse>>
FAILiveProjectBrainHttpClient::QueryEvents(const FString& GameId, int64 SinceSeq)
{
	TSharedRef<TAILivePromiseGuard<TOptional<FAIL_EventQueryResponse>>> Promise =
		MakeShared<TAILivePromiseGuard<TOptional<FAIL_EventQueryResponse>>>(TOptional<FAIL_EventQueryResponse>{});
	TFuture<TOptional<FAIL_EventQueryResponse>> Future = Promise->Promise.GetFuture();

	FRetryContext Ctx;
	Ctx.Verb = TEXT("GET");
	Ctx.Url = FString::Printf(TEXT("%s/v1/games/%s/events?since_seq=%lld"),
		*BaseUrl, *GameId, SinceSeq);
	Ctx.AttemptsRemaining = MaxRetries;
	Ctx.NextDelaySec = BackoffBaseSec;

	DispatchWithRetry(MoveTemp(Ctx), [Promise](int32 Status, const FString& Resp, bool)
	{
		if (Status == 200)
		{
			FAIL_EventQueryResponse Out;
			if (AILiveProtocol::FromJsonString(Resp, Out)) { Promise->SetValue(Out); return; }
		}
		UE_LOG(LogAILiveBrain, Warning, TEXT("QueryEvents failed status=%d body=%s"), Status, *Resp);
		Promise->SetValue(TOptional<FAIL_EventQueryResponse>());
	});
	return Future;
}
