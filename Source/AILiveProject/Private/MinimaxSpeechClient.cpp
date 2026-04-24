#include "MinimaxSpeechClient.h"

#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

DEFINE_LOG_CATEGORY_STATIC(LogMinimaxSpeech, Log, All);

namespace
{
	FString BuildRequestBody(const MinimaxSpeech::FRequest& Req)
	{
		const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("model"), Req.Model);
		Root->SetStringField(TEXT("text"), Req.Text);
		Root->SetBoolField  (TEXT("stream"), false);
		Root->SetStringField(TEXT("language_boost"), Req.LanguageBoost);
		Root->SetStringField(TEXT("output_format"), TEXT("hex"));

		const TSharedRef<FJsonObject> Voice = MakeShared<FJsonObject>();
		Voice->SetStringField(TEXT("voice_id"), Req.VoiceId);
		Voice->SetNumberField(TEXT("speed"), Req.Speed);
		Voice->SetNumberField(TEXT("vol"),   Req.Volume);
		Voice->SetNumberField(TEXT("pitch"), Req.Pitch);
		Root->SetObjectField(TEXT("voice_setting"), Voice);

		const TSharedRef<FJsonObject> Audio = MakeShared<FJsonObject>();
		Audio->SetNumberField(TEXT("sample_rate"), Req.SampleRate);
		Audio->SetStringField(TEXT("format"),      TEXT("pcm"));
		Audio->SetNumberField(TEXT("channel"),     1);
		Root->SetObjectField(TEXT("audio_setting"), Audio);

		FString Out;
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
		FJsonSerializer::Serialize(Root, Writer);
		return Out;
	}

	FORCEINLINE int32 HexDigit(TCHAR C)
	{
		if (C >= '0' && C <= '9') return C - '0';
		if (C >= 'a' && C <= 'f') return 10 + (C - 'a');
		if (C >= 'A' && C <= 'F') return 10 + (C - 'A');
		return -1;
	}

	bool DecodeHexAudio(const FString& Hex, TArray<int16>& OutSamples)
	{
		const int32 Len = Hex.Len();
		if (Len == 0 || (Len % 4) != 0) // two hex chars per byte, two bytes per int16 sample
		{
			return false;
		}
		const int32 NumSamples = Len / 4;
		OutSamples.SetNumUninitialized(NumSamples);

		const TCHAR* Ptr = *Hex;
		uint8* Out = reinterpret_cast<uint8*>(OutSamples.GetData());
		for (int32 ByteIdx = 0; ByteIdx < Len / 2; ++ByteIdx)
		{
			const int32 Hi = HexDigit(Ptr[ByteIdx * 2]);
			const int32 Lo = HexDigit(Ptr[ByteIdx * 2 + 1]);
			if (Hi < 0 || Lo < 0)
			{
				return false;
			}
			Out[ByteIdx] = static_cast<uint8>((Hi << 4) | Lo);
		}
		return true;
	}
}

MinimaxSpeech::FResult MinimaxSpeech::RequestBlocking(const FRequest& Req)
{
	FResult Result;
	Result.NumChannels = 1;

	if (Req.ApiKey.IsEmpty())
	{
		Result.ErrorMessage = TEXT("ApiKey is empty");
		return Result;
	}
	if (Req.Text.IsEmpty())
	{
		Result.ErrorMessage = TEXT("Text is empty");
		return Result;
	}

	const TSharedRef<IHttpRequest> Http = FHttpModule::Get().CreateRequest();
	Http->SetURL(Req.Endpoint);
	Http->SetVerb(TEXT("POST"));
	Http->SetHeader(TEXT("Content-Type"),  TEXT("application/json"));
	Http->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *Req.ApiKey));
	Http->SetTimeout(60.0);

	const FString Body = BuildRequestBody(Req);
	Http->SetContentAsString(Body);

	UE_LOG(LogMinimaxSpeech, Log, TEXT("POST %s body=%d bytes"), *Req.Endpoint, Body.Len());

	Http->ProcessRequestUntilComplete();

	const FHttpResponsePtr Resp = Http->GetResponse();
	if (!Resp.IsValid())
	{
		Result.ErrorMessage = TEXT("No HTTP response");
		return Result;
	}

	const int32 Status = Resp->GetResponseCode();
	const FString Payload = Resp->GetContentAsString();
	if (Status != 200)
	{
		Result.ErrorMessage = FString::Printf(TEXT("HTTP %d: %s"), Status, *Payload.Left(256));
		return Result;
	}

	const TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(Payload);
	TSharedPtr<FJsonObject> Json;
	if (!FJsonSerializer::Deserialize(Reader, Json) || !Json.IsValid())
	{
		Result.ErrorMessage = TEXT("Response JSON parse failed");
		return Result;
	}

	const TSharedPtr<FJsonObject>* BaseResp = nullptr;
	if (Json->TryGetObjectField(TEXT("base_resp"), BaseResp) && BaseResp && BaseResp->IsValid())
	{
		int32 Code = -1;
		(*BaseResp)->TryGetNumberField(TEXT("status_code"), Code);
		if (Code != 0)
		{
			FString Msg;
			(*BaseResp)->TryGetStringField(TEXT("status_msg"), Msg);
			Result.ErrorMessage = FString::Printf(TEXT("api error %d: %s"), Code, *Msg);
			return Result;
		}
	}

	Json->TryGetStringField(TEXT("trace_id"), Result.TraceId);

	const TSharedPtr<FJsonObject>* DataObj = nullptr;
	FString HexAudio;
	if (!Json->TryGetObjectField(TEXT("data"), DataObj) || !DataObj || !DataObj->IsValid() ||
		!(*DataObj)->TryGetStringField(TEXT("audio"), HexAudio) || HexAudio.IsEmpty())
	{
		Result.ErrorMessage = TEXT("Response missing data.audio");
		return Result;
	}

	const TSharedPtr<FJsonObject>* ExtraObj = nullptr;
	int32 RespSampleRate = Req.SampleRate;
	if (Json->TryGetObjectField(TEXT("extra_info"), ExtraObj) && ExtraObj && ExtraObj->IsValid())
	{
		(*ExtraObj)->TryGetNumberField(TEXT("audio_sample_rate"), RespSampleRate);
	}

	if (!DecodeHexAudio(HexAudio, Result.Samples))
	{
		Result.ErrorMessage = TEXT("Hex decode failed");
		return Result;
	}

	Result.bSuccess    = true;
	Result.SampleRate  = RespSampleRate > 0 ? RespSampleRate : Req.SampleRate;
	Result.DurationSec = Result.Samples.Num() / FMath::Max(1.f, (float)Result.SampleRate);

	UE_LOG(LogMinimaxSpeech, Log,
		TEXT("TTS ok trace=%s samples=%d sr=%d dur=%.2fs"),
		*Result.TraceId, Result.Samples.Num(), Result.SampleRate, Result.DurationSec);

	return Result;
}
