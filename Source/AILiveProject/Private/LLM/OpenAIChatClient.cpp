#include "LLM/OpenAIChatClient.h"

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

DEFINE_LOG_CATEGORY_STATIC(LogOpenAIChat, Log, All);

namespace
{
	FString BuildBody(const OpenAIChat::FRequest& Req)
	{
		const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("model"), Req.Model);

		// 用 FJsonValueNumberString 显式控制 temperature 字面量字符串：
		// 直接 SetNumberField(double) 会让 UE TJsonWriter 用 "%.17g" 输出，
		// 把 float→double 的 IEEE 精度损失暴露成 "0.69999998807907104"，
		// GLM 等严格校验的 endpoint 拒绝。
		const FString TempStr = FString::SanitizeFloat(Req.Temperature, /*InMinFractionalDigits=*/1);
		Root->SetField(TEXT("temperature"), MakeShared<FJsonValueNumberString>(TempStr));

		Root->SetNumberField(TEXT("max_tokens"), Req.MaxTokens);
		Root->SetBoolField(TEXT("stream"), false);

		if (Req.bResponseFormatJson)
		{
			const TSharedRef<FJsonObject> Format = MakeShared<FJsonObject>();
			Format->SetStringField(TEXT("type"), TEXT("json_object"));
			Root->SetObjectField(TEXT("response_format"), Format);
		}

		if (!Req.ThinkingType.IsEmpty())
		{
			const TSharedRef<FJsonObject> Thinking = MakeShared<FJsonObject>();
			Thinking->SetStringField(TEXT("type"), Req.ThinkingType);
			Root->SetObjectField(TEXT("thinking"), Thinking);
		}

		TArray<TSharedPtr<FJsonValue>> Messages;
		if (!Req.SystemPrompt.IsEmpty())
		{
			const TSharedRef<FJsonObject> System = MakeShared<FJsonObject>();
			System->SetStringField(TEXT("role"), TEXT("system"));
			System->SetStringField(TEXT("content"), Req.SystemPrompt);
			Messages.Add(MakeShared<FJsonValueObject>(System));
		}
		const TSharedRef<FJsonObject> User = MakeShared<FJsonObject>();
		User->SetStringField(TEXT("role"), TEXT("user"));
		User->SetStringField(TEXT("content"), Req.UserPrompt);
		Messages.Add(MakeShared<FJsonValueObject>(User));
		Root->SetArrayField(TEXT("messages"), Messages);

		FString Out;
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
		FJsonSerializer::Serialize(Root, Writer);
		return Out;
	}
}

namespace OpenAIChat
{
	FString ExtractFirstJsonObject(const FString& Text)
	{
		int32 Start = INDEX_NONE;
		Text.FindChar(TEXT('{'), Start);
		if (Start == INDEX_NONE)
		{
			return FString();
		}
		int32 Depth = 0;
		bool bInString = false;
		bool bEscape = false;
		for (int32 i = Start; i < Text.Len(); ++i)
		{
			const TCHAR C = Text[i];
			if (bEscape)
			{
				bEscape = false;
				continue;
			}
			if (C == TEXT('\\') && bInString)
			{
				bEscape = true;
				continue;
			}
			if (C == TEXT('"'))
			{
				bInString = !bInString;
				continue;
			}
			if (bInString) continue;
			if (C == TEXT('{')) ++Depth;
			else if (C == TEXT('}'))
			{
				--Depth;
				if (Depth == 0)
				{
					return Text.Mid(Start, i - Start + 1);
				}
			}
		}
		return FString();
	}

	FResult RequestBlocking(const FRequest& Req)
	{
		FResult Result;

		if (Req.ApiKey.IsEmpty())
		{
			Result.ErrorMessage = TEXT("ApiKey is empty");
			return Result;
		}
		if (Req.Endpoint.IsEmpty())
		{
			Result.ErrorMessage = TEXT("Endpoint is empty");
			return Result;
		}
		if (Req.Model.IsEmpty())
		{
			Result.ErrorMessage = TEXT("Model is empty");
			return Result;
		}

		const double T0 = FPlatformTime::Seconds();

		const TSharedRef<IHttpRequest> Http = FHttpModule::Get().CreateRequest();
		Http->SetURL(Req.Endpoint);
		Http->SetVerb(TEXT("POST"));
		Http->SetHeader(TEXT("Content-Type"), TEXT("application/json; charset=utf-8"));
		Http->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *Req.ApiKey));
		Http->SetTimeout(Req.TimeoutSec);

		const FString Body = BuildBody(Req);

		// 显式 UTF-8 编码 body：UE 的 SetContentAsString 在某些平台/版本上按 FString 原始
		// 字节（UTF-16 LE）拷贝，GLM 等严格 UTF-8 校验的 endpoint 会回 400 / parse error。
		const FTCHARToUTF8 Utf8(*Body);
		TArray<uint8> Bytes;
		Bytes.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
		Http->SetContent(Bytes);

		// 显式覆盖 UE 默认 User-Agent / Accept-Encoding——某些 LLM 网关（GLM v4 PaaS）会
		// 拒绝没有 Accept 或 UE 默认 UA 的请求，错码 1210 "API 调用参数有误"
		Http->SetHeader(TEXT("User-Agent"), TEXT("AILiveProject/1.0"));
		Http->SetHeader(TEXT("Accept"), TEXT("application/json"));
		Http->SetHeader(TEXT("Accept-Encoding"), TEXT("identity"));

		UE_LOG(LogOpenAIChat, Log, TEXT("POST %s model=%s body=%d chars / %d utf8 bytes"),
			*Req.Endpoint, *Req.Model, Body.Len(), Bytes.Num());

		// 调试 dump：把待发 body 写到 Saved/Logs/OpenAIChat/last_<model>.json
		{
			const FString DumpDir = FPaths::ProjectSavedDir() / TEXT("Logs") / TEXT("OpenAIChat");
			IFileManager::Get().MakeDirectory(*DumpDir, true);
			FString SafeModel = Req.Model;
			SafeModel.ReplaceInline(TEXT("/"), TEXT("_"));
			SafeModel.ReplaceInline(TEXT(":"), TEXT("_"));
			const FString DumpPath = DumpDir / FString::Printf(TEXT("last_%s.json"), *SafeModel);
			FFileHelper::SaveArrayToFile(Bytes, *DumpPath);
		}

		Http->ProcessRequestUntilComplete();

		const FHttpResponsePtr Resp = Http->GetResponse();
		Result.LatencyMs = static_cast<float>((FPlatformTime::Seconds() - T0) * 1000.0);

		if (!Resp.IsValid())
		{
			Result.ErrorMessage = TEXT("No HTTP response");
			return Result;
		}
		Result.HttpStatus = Resp->GetResponseCode();
		const FString Payload = Resp->GetContentAsString();
		Result.RawResponsePayload = Payload;

		if (Result.HttpStatus != 200)
		{
			Result.ErrorMessage = FString::Printf(TEXT("HTTP %d: %s"),
				Result.HttpStatus, *Payload.Left(512));
			Result.RawContent = Payload; // 写盘以便复盘
			if (Result.HttpStatus == 400 && Req.bResponseFormatJson)
			{
				UE_LOG(LogOpenAIChat, Warning,
					TEXT("OpenAIChat HTTP 400 model=%s; retrying without response_format"),
					*Req.Model);
				FRequest RetryReq = Req;
				RetryReq.bResponseFormatJson = false;
				FResult RetryResult = RequestBlocking(RetryReq);
				RetryResult.bRetriedWithoutResponseFormat = true;
				if (!RetryResult.bSuccess)
				{
					RetryResult.ErrorMessage = FString::Printf(
						TEXT("Initial HTTP 400 with response_format: %s; retry without response_format: %s"),
						*Result.ErrorMessage, *RetryResult.ErrorMessage);
				}
				return RetryResult;
			}
			return Result;
		}

		const TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(Payload);
		TSharedPtr<FJsonObject> Json;
		if (!FJsonSerializer::Deserialize(Reader, Json) || !Json.IsValid())
		{
			Result.ErrorMessage = TEXT("Response JSON parse failed");
			return Result;
		}

		const TArray<TSharedPtr<FJsonValue>>* Choices = nullptr;
		if (!Json->TryGetArrayField(TEXT("choices"), Choices) || !Choices || Choices->Num() == 0)
		{
			Result.ErrorMessage = TEXT("Response missing choices[]");
			return Result;
		}
		const TSharedPtr<FJsonObject>& First = (*Choices)[0]->AsObject();
		if (!First.IsValid())
		{
			Result.ErrorMessage = TEXT("choices[0] is not an object");
			return Result;
		}
		First->TryGetStringField(TEXT("finish_reason"), Result.FinishReason);
		const TSharedPtr<FJsonObject>* Message = nullptr;
		if (!First->TryGetObjectField(TEXT("message"), Message) || !Message || !(*Message).IsValid())
		{
			Result.ErrorMessage = TEXT("choices[0].message missing");
			return Result;
		}
		(*Message)->TryGetStringField(TEXT("content"), Result.RawContent);
		(*Message)->TryGetStringField(TEXT("reasoning_content"), Result.ReasoningContent);

		const TSharedPtr<FJsonObject>* Usage = nullptr;
		if (Json->TryGetObjectField(TEXT("usage"), Usage) && Usage && (*Usage).IsValid())
		{
			(*Usage)->TryGetNumberField(TEXT("prompt_tokens"), Result.PromptTokens);
			(*Usage)->TryGetNumberField(TEXT("completion_tokens"), Result.CompletionTokens);
		}

		Result.ParsedJson = ExtractFirstJsonObject(Result.RawContent);
		if (Result.RawContent.IsEmpty() && !Result.ReasoningContent.IsEmpty())
		{
			Result.ErrorMessage = TEXT("Response content is empty; reasoning_content is present");
		}
		else if (Result.ParsedJson.IsEmpty())
		{
			Result.ErrorMessage = TEXT("Response content does not contain a JSON object");
		}
		Result.bSuccess = true;

		UE_LOG(LogOpenAIChat, Log,
			TEXT("OpenAIChat ok model=%s latency=%.0fms tokens=%d/%d content_len=%d reasoning_len=%d finish=%s"),
			*Req.Model, Result.LatencyMs, Result.PromptTokens, Result.CompletionTokens,
			Result.RawContent.Len(), Result.ReasoningContent.Len(), *Result.FinishReason);

		return Result;
	}
}
