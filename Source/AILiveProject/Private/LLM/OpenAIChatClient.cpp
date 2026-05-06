// =============================================================================
// 中文教学：OpenAIChatClient.cpp —— HTTP 请求构造与同步发送
//
// 关键 UE / C++ 概念：
//   1) FJsonObject + TSharedRef<FJsonObject>
//      UE 的 JSON 对象用「共享指针」（引用计数智能指针）管理生命周期。
//      - TSharedPtr<T>: 可空，类似 std::shared_ptr
//      - TSharedRef<T>: 不可空，构造时必须指向有效对象（更安全）
//      - MakeShared<T>(): 创建并返回 TSharedRef<T>
//
//   2) FHttpModule::Get().CreateRequest()
//      UE 的 HTTP 模块单例。CreateRequest() 返回 IHttpRequest 共享引用。
//      默认是异步：SetVerb / SetHeader / SetContent 配置完后调 ProcessRequest()
//      会立刻返回，结果通过 OnProcessRequestComplete delegate 回来。
//      这里我们要同步语义，所以用 ProcessRequestUntilComplete()——它内部跑
//      pump loop 直到完成才返回，期间会阻塞调用线程。
//
//   3) FTCHARToUTF8 + SetContent(TArray<uint8>)
//      为什么不用 SetContentAsString：UE 某些版本的 SetContentAsString 会
//      按 FString 内部字节（UTF-16 LE）原样发送，但 LLM endpoint 期望 UTF-8。
//      正确做法：先 FTCHARToUTF8 转字节，再 SetContent(byte array)。
//
//   4) TJsonReader / TJsonWriter（模板）
//      用「TCondensedJsonPrintPolicy」生成无缩进紧凑 JSON（HTTP body 节流量）。
//      "Pretty" 版本会输出带缩进、便于调试；这里用 Condensed。
//
//   5) 匿名命名空间内的 BuildBody
//      只在本 .cpp 内可见的辅助函数（与上一批 ProjectEnvLoader 同样模式）。
//
// 失败重试策略（重要细节）：
//   - 某些 endpoint（如 GLM）不支持 response_format=json_object，会返 HTTP 400
//   - 检测到 400 后，自动重试一次「不带 response_format」的请求
//   - 重试结果上挂 bRetriedWithoutResponseFormat=true，调用方据此决策是否接受
// =============================================================================

#include "LLM/OpenAIChatClient.h"

#include "Dom/JsonObject.h"             // FJsonObject / FJsonValueObject 等
#include "HAL/FileManager.h"            // IFileManager（写 dump 文件）
#include "HAL/PlatformTime.h"           // FPlatformTime::Seconds() 高精度计时
#include "HttpModule.h"                 // FHttpModule
#include "Interfaces/IHttpRequest.h"    // IHttpRequest
#include "Interfaces/IHttpResponse.h"   // IHttpResponse
#include "Misc/FileHelper.h"            // FFileHelper::SaveArrayToFile
#include "Misc/Paths.h"                 // FPaths::ProjectSavedDir
#include "Serialization/JsonReader.h"   // TJsonReader
#include "Serialization/JsonSerializer.h"// FJsonSerializer

DEFINE_LOG_CATEGORY_STATIC(LogOpenAIChat, Log, All);

namespace
{
	// 把 FRequest 序列化成 OpenAI Chat Completions 协议的 JSON body 字符串。
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
	// 从一段任意文本中提取第一个完整 JSON 对象（带括号匹配的最小子串）。
	// 中文教学：这是一个「自己写的小词法分析器」。状态机有三个变量：
	//   Depth  ：当前 { } 嵌套深度
	//   bInString: 是否处在 "..." 字符串字面量内（字符串里的 {} 不算结构括号）
	//   bEscape: 上一字符是否是 \（用于处理 \" \\ \n 等转义，避免误判结束引号）
	FString ExtractFirstJsonObject(const FString& Text)
	{
		int32 Start = INDEX_NONE;
		Text.FindChar(TEXT('{'), Start);    // 找第一个 '{' 起点
		if (Start == INDEX_NONE)
		{
			return FString();               // 全文没有 '{' 就直接放弃
		}
		int32 Depth = 0;
		bool bInString = false;
		bool bEscape = false;
		for (int32 i = Start; i < Text.Len(); ++i)
		{
			const TCHAR C = Text[i];
			if (bEscape)
			{
				bEscape = false;            // 把当前字符当转义字符的「目标」消费掉
				continue;
			}
			if (C == TEXT('\\') && bInString)
			{
				bEscape = true;             // 反斜杠：标记下一字符为转义目标
				continue;
			}
			if (C == TEXT('"'))
			{
				bInString = !bInString;     // 引号：切换字符串状态
				continue;
			}
			if (bInString) continue;        // 字符串里的 { } 跳过不计
			if (C == TEXT('{')) ++Depth;
			else if (C == TEXT('}'))
			{
				--Depth;
				if (Depth == 0)
				{
					// 嵌套回到 0 → 从 Start 到当前 i 是一个完整 JSON
					return Text.Mid(Start, i - Start + 1);
				}
			}
		}
		return FString();                   // 文本扫完都没闭合 → 失败
	}

	// 同步发起 HTTP 请求并解析响应。完整流程：
	//   1) 校验 ApiKey/Endpoint/Model 不空
	//   2) 构造 IHttpRequest，设 URL/Verb/Header/Timeout
	//   3) 序列化 body，编码 UTF-8 字节，SetContent
	//   4) ProcessRequestUntilComplete() 阻塞等结果
	//   5) 解析 JSON：choices[0].message.content / .reasoning_content / usage / finish_reason
	//   6) 用 ExtractFirstJsonObject 从 content 抠 JSON 对象
	FResult RequestBlocking(const FRequest& Req)
	{
		FResult Result;

		// ── 输入校验：早返回；返回 Result 时 bSuccess 默认 false ──────────
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

		const double T0 = FPlatformTime::Seconds();  // 起点时间，用于算 LatencyMs

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

		// 同步 pump：阻塞调用线程直到 HTTP 完成。⚠️ 不要在游戏线程调本函数。
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
