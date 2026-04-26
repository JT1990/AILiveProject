#include "Mind/MindLLMProvider_DeepSeek.h"
#include "Mind/MindLog.h"
#include "MinimaxACELibrary.h"

#include "Engine/Engine.h"
#include "Engine/World.h"
#include "TimerManager.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
	using FOnDeepSeekRawDone = TFunction<void(bool /*bOk*/, int32 /*HttpCode*/, FString /*Content*/)>;

	void SendDeepSeekChat(
		const FString& Base,
		const FString& Key,
		const FString& Model,
		float Temperature,
		const FString& SystemPrompt,
		const FString& UserPrompt,
		FOnDeepSeekRawDone OnDone)
	{
		if (Base.IsEmpty() || Key.IsEmpty())
		{
			UE_LOG(LogMind, Warning, TEXT("[DeepSeek] missing env: BaseEmpty=%d KeyEmpty=%d"),
				Base.IsEmpty() ? 1 : 0, Key.IsEmpty() ? 1 : 0);
			OnDone(false, 0, TEXT("missing env"));
			return;
		}

		FString Endpoint = Base;
		if (!Endpoint.EndsWith(TEXT("/")))
		{
			Endpoint += TEXT("/");
		}
		Endpoint += TEXT("chat/completions");

		TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
		Body->SetStringField(TEXT("model"), Model);
		Body->SetNumberField(TEXT("temperature"), Temperature);

		TArray<TSharedPtr<FJsonValue>> Messages;
		{
			TSharedRef<FJsonObject> SysMsg = MakeShared<FJsonObject>();
			SysMsg->SetStringField(TEXT("role"), TEXT("system"));
			SysMsg->SetStringField(TEXT("content"), SystemPrompt);
			Messages.Add(MakeShared<FJsonValueObject>(SysMsg));
		}
		{
			TSharedRef<FJsonObject> UserMsg = MakeShared<FJsonObject>();
			UserMsg->SetStringField(TEXT("role"), TEXT("user"));
			UserMsg->SetStringField(TEXT("content"), UserPrompt);
			Messages.Add(MakeShared<FJsonValueObject>(UserMsg));
		}
		Body->SetArrayField(TEXT("messages"), Messages);

		FString JsonStr;
		TSharedRef<TJsonWriter<TCHAR>> Writer = TJsonWriterFactory<TCHAR>::Create(&JsonStr);
		FJsonSerializer::Serialize(Body, Writer);

		const int32 PromptLen = SystemPrompt.Len() + UserPrompt.Len();
		UE_LOG(LogMind, Display, TEXT("[DeepSeek] request model=%s promptLen=%d key=%s"),
			*Model, PromptLen, *UMinimaxACELibrary::MaskKey(Key));

		auto Req = FHttpModule::Get().CreateRequest();
		Req->SetURL(Endpoint);
		Req->SetVerb(TEXT("POST"));
		Req->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *Key));
		Req->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
		Req->SetTimeout(60.f);
		Req->SetContentAsString(JsonStr);

		Req->OnProcessRequestComplete().BindLambda(
			[OnDone](FHttpRequestPtr R, FHttpResponsePtr P, bool bSucceeded)
			{
				if (!bSucceeded || !P.IsValid())
				{
					UE_LOG(LogMind, Warning, TEXT("[DeepSeek] transport failed (bSucceeded=%d resp.IsValid=%d)"),
						bSucceeded ? 1 : 0, P.IsValid() ? 1 : 0);
					OnDone(false, 0, TEXT("transport error"));
					return;
				}
				const int32 Code = P->GetResponseCode();
				const FString RawBody = P->GetContentAsString();

				if (Code == 401)
				{
					UE_LOG(LogMind, Warning, TEXT("[DeepSeek] HTTP 401 (key invalid)"));
					OnDone(false, Code, RawBody);
					return;
				}
				if (Code < 200 || Code >= 300)
				{
					UE_LOG(LogMind, Warning, TEXT("[DeepSeek] HTTP %d body=%s"),
						Code, *RawBody.Left(256));
					OnDone(false, Code, RawBody);
					return;
				}

				TSharedPtr<FJsonObject> Json;
				TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(RawBody);
				if (!FJsonSerializer::Deserialize(Reader, Json) || !Json.IsValid())
				{
					UE_LOG(LogMind, Warning, TEXT("[DeepSeek] JSON parse failed: %s"), *RawBody.Left(256));
					OnDone(false, Code, RawBody);
					return;
				}
				const TArray<TSharedPtr<FJsonValue>>* Choices = nullptr;
				if (!Json->TryGetArrayField(TEXT("choices"), Choices) || !Choices || Choices->Num() == 0)
				{
					UE_LOG(LogMind, Warning, TEXT("[DeepSeek] response missing choices: %s"), *RawBody.Left(256));
					OnDone(false, Code, RawBody);
					return;
				}
				const TSharedPtr<FJsonObject>* Choice0 = nullptr;
				if (!(*Choices)[0].IsValid() || !(*Choices)[0]->TryGetObject(Choice0) || !Choice0)
				{
					UE_LOG(LogMind, Warning, TEXT("[DeepSeek] choices[0] not object"));
					OnDone(false, Code, RawBody);
					return;
				}
				const TSharedPtr<FJsonObject>* Message = nullptr;
				if (!(*Choice0)->TryGetObjectField(TEXT("message"), Message) || !Message)
				{
					UE_LOG(LogMind, Warning, TEXT("[DeepSeek] choices[0].message missing"));
					OnDone(false, Code, RawBody);
					return;
				}
				FString Content;
				if (!(*Message)->TryGetStringField(TEXT("content"), Content))
				{
					UE_LOG(LogMind, Warning, TEXT("[DeepSeek] choices[0].message.content missing"));
					OnDone(false, Code, RawBody);
					return;
				}

				UE_LOG(LogMind, Display, TEXT("[DeepSeek] response (HTTP %d) len=%d: %s"),
					Code, Content.Len(), *Content.Left(200));
				OnDone(true, Code, Content);
			});
		Req->ProcessRequest();
	}

	struct FBatchPingRecord
	{
		double StartSec = 0.0;
		double EndSec = 0.0;
		int32 HttpCode = 0;
		bool bOk = false;
	};

	struct FBatchPingState
	{
		FString Prompt;
		FString Base;
		FString Key;
		FString Model;
		float Temperature = 0.7f;
		int32 N = 0;
		int32 IntervalMs = 500;
		int32 FiredCount = 0;
		int32 CompletedCount = 0;
		TArray<FBatchPingRecord> Records;
		TWeakObjectPtr<UWorld> World;
		FTimerHandle TimerHandle;
	};

	void OnBatchPingDone(TSharedRef<FBatchPingState> State, int32 Index, bool bOk, int32 Code)
	{
		if (State->Records.IsValidIndex(Index))
		{
			State->Records[Index].EndSec = FPlatformTime::Seconds();
			State->Records[Index].HttpCode = Code;
			State->Records[Index].bOk = bOk;
		}
		State->CompletedCount++;

		if (State->CompletedCount < State->N)
		{
			return;
		}

		TArray<double> LatenciesMs;
		LatenciesMs.Reserve(State->Records.Num());
		int32 Count429 = 0;
		int32 OkCount = 0;
		for (const FBatchPingRecord& R : State->Records)
		{
			LatenciesMs.Add((R.EndSec - R.StartSec) * 1000.0);
			if (R.HttpCode == 429) { Count429++; }
			if (R.bOk) { OkCount++; }
		}
		double Avg = 0.0;
		for (double M : LatenciesMs) { Avg += M; }
		if (LatenciesMs.Num() > 0) { Avg /= LatenciesMs.Num(); }

		TArray<double> Sorted = LatenciesMs;
		Sorted.Sort();
		double P95 = 0.0;
		if (Sorted.Num() > 0)
		{
			const int32 P95Index = FMath::Clamp(FMath::FloorToInt(0.95f * (Sorted.Num() - 1)), 0, Sorted.Num() - 1);
			P95 = Sorted[P95Index];
		}

		UE_LOG(LogMind, Display,
			TEXT("[DeepSeek] BatchPing summary: N=%d ok=%d 429=%d avg=%.0fms p95=%.0fms"),
			State->N, OkCount, Count429, Avg, P95);
		for (int32 i = 0; i < State->Records.Num(); ++i)
		{
			const FBatchPingRecord& R = State->Records[i];
			UE_LOG(LogMind, Display,
				TEXT("[DeepSeek] BatchPing[%d]: HTTP=%d ok=%d %.0fms"),
				i, R.HttpCode, R.bOk ? 1 : 0, (R.EndSec - R.StartSec) * 1000.0);
		}
	}

	void FireOneBatchPing(TSharedRef<FBatchPingState> State)
	{
		const int32 Index = State->FiredCount;
		State->FiredCount++;

		FBatchPingRecord Rec;
		Rec.StartSec = FPlatformTime::Seconds();
		State->Records.Add(Rec);

		SendDeepSeekChat(State->Base, State->Key, State->Model, State->Temperature,
			TEXT("你是一个友好的中文助手。"), State->Prompt,
			[State, Index](bool bOk, int32 Code, FString /*Content*/)
			{
				OnBatchPingDone(State, Index, bOk, Code);
			});
	}
}

void UMindLLMProvider_DeepSeek::RequestCompletion(
	const FString& SystemPrompt,
	const FString& UserPrompt,
	const TArray<TSubclassOf<UMindAction>>& AvailableActions,
	FOnLLMResult Done)
{
	(void)AvailableActions; // T06 起接 tool schema
	const FString Base = UMinimaxACELibrary::GetEnvValueFromProjectEnv(ApiBaseEnvName);
	const FString Key = UMinimaxACELibrary::GetEnvValueFromProjectEnv(ApiKeyEnvName);

	SendDeepSeekChat(Base, Key, Model, Temperature, SystemPrompt, UserPrompt,
		[Done](bool bOk, int32 /*Code*/, FString Content)
		{
			Done.ExecuteIfBound(bOk, Content);
		});
}

void UMindLLMProvider_DeepSeek::TestPing(UObject* /*WorldCtx*/, const FString& Prompt, FDeepSeekTestPingDone OnDone)
{
	UMindLLMProvider_DeepSeek* P = NewObject<UMindLLMProvider_DeepSeek>(GetTransientPackage());
	P->AddToRoot();

	UMindLLMProvider::FOnLLMResult Proxy;
	Proxy.BindLambda([P, OnDone](bool bOk, FString Text)
	{
		OnDone.ExecuteIfBound(bOk, Text);
		if (P)
		{
			P->RemoveFromRoot();
			P->MarkAsGarbage();
		}
	});

	P->RequestCompletion(TEXT("你是一个友好的中文助手。"), Prompt, {}, Proxy);
}

void UMindLLMProvider_DeepSeek::BatchPing(UObject* WorldCtx, const FString& Prompt, int32 N, int32 IntervalMs)
{
	if (N <= 0)
	{
		UE_LOG(LogMind, Warning, TEXT("[DeepSeek] BatchPing N<=0, abort"));
		return;
	}
	UWorld* World = GEngine ? GEngine->GetWorldFromContextObject(WorldCtx, EGetWorldErrorMode::LogAndReturnNull) : nullptr;
	if (!World)
	{
		UE_LOG(LogMind, Warning, TEXT("[DeepSeek] BatchPing no world from WorldCtx, abort"));
		return;
	}

	TSharedRef<FBatchPingState> State = MakeShared<FBatchPingState>();
	State->Prompt = Prompt;
	State->Base = UMinimaxACELibrary::GetEnvValueFromProjectEnv(TEXT("DEEPSEEK_API_BASE"));
	State->Key = UMinimaxACELibrary::GetEnvValueFromProjectEnv(TEXT("DEEPSEEK_API_KEY"));
	State->Model = TEXT("deepseek-chat");
	State->Temperature = 0.7f;
	State->N = N;
	State->IntervalMs = FMath::Max(1, IntervalMs);
	State->World = World;

	UE_LOG(LogMind, Display, TEXT("[DeepSeek] BatchPing start: N=%d IntervalMs=%d"),
		State->N, State->IntervalMs);

	FireOneBatchPing(State);

	if (N <= 1)
	{
		return;
	}

	World->GetTimerManager().SetTimer(
		State->TimerHandle,
		FTimerDelegate::CreateLambda([State]()
		{
			if (State->FiredCount >= State->N)
			{
				if (UWorld* W = State->World.Get())
				{
					W->GetTimerManager().ClearTimer(State->TimerHandle);
				}
				return;
			}
			FireOneBatchPing(State);
		}),
		State->IntervalMs / 1000.f,
		true);
}
