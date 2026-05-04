#include "LLM/AILiveParserClient.h"
#include "LLM/AILiveParserVersion.h"

#include "LLM/AILiveAgentRoster.h"
#include "LLM/OpenAIChatClient.h"
#include "Memory/AILiveEventTypes.h"

#include "Dom/JsonObject.h"
#include "HAL/CriticalSection.h"
#include "HAL/PlatformTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
	// Parser provider 切到 DeepSeek（T7 烟测决定，2026-05-04）：
	// 原 Qwen3 选型在 10 NPC 并发时 dashscope endpoint 限流，HTTP 0 / reasoner_timeout
	// 失败率 70%+；切 DeepSeek 后 winner 路径可见，L1 #1/#2 才能机械验。
	// 注：未 bump parser_version——prompt/schema/regex 不变，仅承载模型变。
	// 如后续行为差异显著需要 bump，按文档流程加 v2.txt + GetCurrentParserVersion。
	// _meta.db.schema_meta 由下次 BeginGame 自动 upsert parser_model 字段。
	constexpr ELLMProvider kParserProvider = ELLMProvider::DeepSeek;

	constexpr const TCHAR* kParserVersion          = TEXT("1");
	constexpr const TCHAR* kParserPromptRegistry   = TEXT("Content/Prompts/Parser/");
	constexpr const TCHAR* kParserPromptRelative   = TEXT("Content/Prompts/Parser/v1.txt");

	// 模块作用域 cache：首次 ParseFourChannels 调用时同步加载 prompt 文件
	FCriticalSection GParserPromptLock;
	bool             GParserPromptLoaded = false;
	bool             GParserPromptLoadOk = false;
	FString          GParserSystemPrompt;

	FCriticalSection GParserModelLock;
	bool             GParserModelResolved = false;
	FString          GParserModelCached;

	FString MakeAbsolutePromptPath()
	{
		return FPaths::ProjectContentDir() / TEXT("Prompts/Parser/v1.txt");
	}

	bool EnsureSystemPromptLoaded()
	{
		FScopeLock Lock(&GParserPromptLock);
		if (GParserPromptLoaded)
		{
			return GParserPromptLoadOk;
		}
		GParserPromptLoaded = true;

		const FString AbsPath = MakeAbsolutePromptPath();
		if (!FFileHelper::LoadFileToString(GParserSystemPrompt, *AbsPath))
		{
			UE_LOG(LogAILiveMemory, Error,
				TEXT("AILiveParser: failed to load Parser system prompt at %s"),
				*AbsPath);
			GParserSystemPrompt.Reset();
			GParserPromptLoadOk = false;
			return false;
		}
		GParserPromptLoadOk = !GParserSystemPrompt.IsEmpty();
		if (!GParserPromptLoadOk)
		{
			UE_LOG(LogAILiveMemory, Error,
				TEXT("AILiveParser: Parser system prompt at %s is empty"),
				*AbsPath);
		}
		return GParserPromptLoadOk;
	}

	int32 FindFirstSubstring(const FString& Haystack, const TCHAR* Needle)
	{
		return Haystack.Find(Needle, ESearchCase::CaseSensitive, ESearchDir::FromStart, 0);
	}

	bool ExtractTagBody(const FString& Raw, const TCHAR* OpenTag, const TCHAR* CloseTag, FString& OutBody)
	{
		const int32 OpenAt = FindFirstSubstring(Raw, OpenTag);
		if (OpenAt == INDEX_NONE)
		{
			return false;
		}
		const int32 OpenLen = FCString::Strlen(OpenTag);
		const int32 CloseAt = Raw.Find(CloseTag, ESearchCase::CaseSensitive, ESearchDir::FromStart, OpenAt + OpenLen);
		if (CloseAt == INDEX_NONE)
		{
			return false;
		}
		OutBody = Raw.Mid(OpenAt + OpenLen, CloseAt - (OpenAt + OpenLen));
		return true;
	}

	bool TryParseJsonObject(const FString& Text, TSharedPtr<FJsonObject>& OutObj)
	{
		const FString Trimmed = Text.TrimStartAndEnd();
		if (Trimmed.IsEmpty())
		{
			return false;
		}
		const TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(Trimmed);
		return FJsonSerializer::Deserialize(Reader, OutObj) && OutObj.IsValid();
	}

	FString SerializeJsonObjectCondensed(const TSharedPtr<FJsonObject>& Obj)
	{
		if (!Obj.IsValid())
		{
			return FString();
		}
		FString Out;
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
		FJsonSerializer::Serialize(Obj.ToSharedRef(), Writer);
		return Out;
	}
}

namespace AILiveParser
{
	FString GetCurrentParserVersion()
	{
		return FString(kParserVersion);
	}

	FString GetCurrentParserPromptRegistryDir()
	{
		return FString(kParserPromptRegistry);
	}

	FString GetCurrentParserPromptPath()
	{
		return FString(kParserPromptRelative);
	}

	FString GetCurrentParserModel()
	{
		FScopeLock Lock(&GParserModelLock);
		if (GParserModelResolved)
		{
			return GParserModelCached;
		}
		GParserModelResolved = true;

		const AILiveAgentRoster::FProviderEndpoint Ep =
			AILiveAgentRoster::ResolveProviderEndpoint(kParserProvider);
		if (Ep.Model.IsEmpty())
		{
			UE_LOG(LogAILiveMemory, Error,
				TEXT("AILiveParser: provider '%s' Model 缺失（检查 .env）；GetCurrentParserModel 返回空"),
				*AILiveAgentRoster::ProviderToString(kParserProvider));
			GParserModelCached.Reset();
			return GParserModelCached;
		}
		GParserModelCached = AILiveAgentRoster::ProviderToString(kParserProvider)
			+ TEXT(":") + Ep.Model;
		return GParserModelCached;
	}

	FParseResult PrevalidateRawTaggedSections(const FString& RawText)
	{
		FParseResult R;

		struct FTagSpec
		{
			const TCHAR* Open;
			const TCHAR* Close;
			const TCHAR* MissingMsg;
		};

		const FTagSpec Tags[] = {
			{ TEXT("<SCRATCHPAD>"),   TEXT("</SCRATCHPAD>"),   TEXT("missing scratchpad section") },
			{ TEXT("<INTENDED>"),     TEXT("</INTENDED>"),     TEXT("missing intended section") },
			{ TEXT("<BID>"),          TEXT("</BID>"),          TEXT("missing bid section") },
			{ TEXT("<NOTE_TO_SELF>"), TEXT("</NOTE_TO_SELF>"), TEXT("missing note_to_self section") },
		};

		FString IntendedBody;
		FString BidBody;

		for (const FTagSpec& T : Tags)
		{
			FString Body;
			if (!ExtractTagBody(RawText, T.Open, T.Close, Body))
			{
				R.bOk = false;
				R.ErrorReason = T.MissingMsg;
				R.FailedStage = TEXT("raw_prevalidate");
				return R;
			}
			if (FCString::Strcmp(T.Open, TEXT("<INTENDED>")) == 0)
			{
				IntendedBody = Body;
			}
			else if (FCString::Strcmp(T.Open, TEXT("<BID>")) == 0)
			{
				BidBody = Body;
			}
		}

		TSharedPtr<FJsonObject> IntendedObj;
		if (!TryParseJsonObject(IntendedBody, IntendedObj))
		{
			R.bOk = false;
			R.ErrorReason = TEXT("intended payload not valid JSON");
			R.FailedStage = TEXT("raw_prevalidate");
			return R;
		}

		// principles §5.2bis：bid 必须是 JSON object（urgency 必填，由 Stage 3 校验数值）。
		// 这里只做结构性校验，避免 <BID>not json</BID> 进 Parser LLM。
		TSharedPtr<FJsonObject> BidObj;
		if (!TryParseJsonObject(BidBody, BidObj))
		{
			R.bOk = false;
			R.ErrorReason = TEXT("bid payload not valid JSON");
			R.FailedStage = TEXT("raw_prevalidate");
			return R;
		}

		R.bOk = true;
		return R;
	}

	FParseResult ValidateParserOutputJson(const FString& ParserJson)
	{
		FParseResult R;
		R.ParserRawJson = ParserJson;

		TSharedPtr<FJsonObject> Root;
		if (!TryParseJsonObject(ParserJson, Root))
		{
			R.bOk = false;
			R.ErrorReason = TEXT("parser response not valid JSON");
			R.FailedStage = TEXT("output_validate");
			return R;
		}

		// scratchpad: 必须存在且是 string（TryGetStringField 仅在 string 时返回 true）
		if (!Root->HasField(TEXT("scratchpad")))
		{
			R.bOk = false;
			R.ErrorReason = TEXT("missing scratchpad section");
			R.FailedStage = TEXT("output_validate");
			return R;
		}
		FString Scratchpad;
		if (!Root->TryGetStringField(TEXT("scratchpad"), Scratchpad))
		{
			R.bOk = false;
			R.ErrorReason = TEXT("scratchpad must be string");
			R.FailedStage = TEXT("output_validate");
			return R;
		}

		// intended: 必须 object + intended.text 必须 string
		const TSharedPtr<FJsonObject>* IntendedPtr = nullptr;
		if (!Root->HasField(TEXT("intended")))
		{
			R.bOk = false;
			R.ErrorReason = TEXT("missing intended section");
			R.FailedStage = TEXT("output_validate");
			return R;
		}
		if (!Root->TryGetObjectField(TEXT("intended"), IntendedPtr) || !IntendedPtr || !(*IntendedPtr).IsValid())
		{
			R.bOk = false;
			R.ErrorReason = TEXT("intended payload not valid JSON");
			R.FailedStage = TEXT("output_validate");
			return R;
		}
		FString IntendedText;
		if (!(*IntendedPtr)->TryGetStringField(TEXT("text"), IntendedText))
		{
			R.bOk = false;
			R.ErrorReason = TEXT("intended.text missing or not string");
			R.FailedStage = TEXT("output_validate");
			return R;
		}

		// bid: 必须 object + bid.urgency 必须 number（principles §5.2bis 必填）
		const TSharedPtr<FJsonObject>* BidPtr = nullptr;
		if (!Root->HasField(TEXT("bid")))
		{
			R.bOk = false;
			R.ErrorReason = TEXT("missing bid section");
			R.FailedStage = TEXT("output_validate");
			return R;
		}
		if (!Root->TryGetObjectField(TEXT("bid"), BidPtr) || !BidPtr || !(*BidPtr).IsValid())
		{
			R.bOk = false;
			R.ErrorReason = TEXT("bid payload not valid JSON");
			R.FailedStage = TEXT("output_validate");
			return R;
		}
		double BidUrgency = 0.0;
		if (!(*BidPtr)->TryGetNumberField(TEXT("urgency"), BidUrgency))
		{
			R.bOk = false;
			R.ErrorReason = TEXT("bid.urgency missing or not number");
			R.FailedStage = TEXT("output_validate");
			return R;
		}

		// note_to_self: 必须存在且是 string
		if (!Root->HasField(TEXT("note_to_self")))
		{
			R.bOk = false;
			R.ErrorReason = TEXT("missing note_to_self section");
			R.FailedStage = TEXT("output_validate");
			return R;
		}
		FString NoteText;
		if (!Root->TryGetStringField(TEXT("note_to_self"), NoteText))
		{
			R.bOk = false;
			R.ErrorReason = TEXT("note_to_self must be string");
			R.FailedStage = TEXT("output_validate");
			return R;
		}

		R.bOk = true;
		R.Scratchpad   = Scratchpad;
		R.NoteText     = NoteText;
		R.IntendedJson = SerializeJsonObjectCondensed(*IntendedPtr);
		R.BidJson      = SerializeJsonObjectCondensed(*BidPtr);
		return R;
	}

	FParseResult ParseFourChannels(const FParseRequest& Req)
	{
		const double T0 = FPlatformTime::Seconds();

		// Stage 1: raw text 预检
		FParseResult Pre = PrevalidateRawTaggedSections(Req.RawText);
		if (!Pre.bOk)
		{
			UE_LOG(LogAILiveMemory, Warning,
				TEXT("AILiveParser[%s] Stage1 reject: %s"), *Req.AgentId, *Pre.ErrorReason);
			return Pre;
		}

		// Stage 2: 加载 system prompt + LLM 调用
		if (!EnsureSystemPromptLoaded())
		{
			FParseResult R;
			R.bOk = false;
			R.ErrorReason = TEXT("parser prompt file not found");
			R.FailedStage = TEXT("llm_call");
			return R;
		}

		const AILiveAgentRoster::FProviderEndpoint Ep =
			AILiveAgentRoster::ResolveProviderEndpoint(kParserProvider);
		if (Ep.ApiKey.IsEmpty() || Ep.Endpoint.IsEmpty() || Ep.Model.IsEmpty())
		{
			FParseResult R;
			R.bOk = false;
			R.ErrorReason = TEXT("parser model not configured");
			R.FailedStage = TEXT("llm_call");
			UE_LOG(LogAILiveMemory, Error,
				TEXT("AILiveParser[%s] endpoint missing fields: ApiKey=%d Endpoint=%d Model=%d"),
				*Req.AgentId, Ep.ApiKey.IsEmpty()?0:1, Ep.Endpoint.IsEmpty()?0:1, Ep.Model.IsEmpty()?0:1);
			return R;
		}

		OpenAIChat::FRequest LLMReq;
		LLMReq.ApiKey              = Ep.ApiKey;
		LLMReq.Endpoint            = Ep.Endpoint;
		LLMReq.Model               = Ep.Model;
		LLMReq.SystemPrompt        = GParserSystemPrompt;
		LLMReq.UserPrompt          = Req.RawText;
		LLMReq.Temperature         = 0.f;
		LLMReq.MaxTokens           = 1500;
		LLMReq.bResponseFormatJson = true;
		LLMReq.TimeoutSec          = 60.f;

		const OpenAIChat::FResult LLMResult = OpenAIChat::RequestBlocking(LLMReq);

		if (!LLMResult.bSuccess)
		{
			FParseResult R;
			R.bOk = false;
			R.ErrorReason = FString::Printf(TEXT("parser LLM call failed: %s"), *LLMResult.ErrorMessage);
			R.FailedStage = TEXT("llm_call");
			R.ParserRawJson = LLMResult.RawContent;
			return R;
		}

		// Parser 强约束 response_format=json_object：endpoint 拒绝 → 视为失败
		if (LLMResult.bRetriedWithoutResponseFormat)
		{
			FParseResult R;
			R.bOk = false;
			R.ErrorReason = TEXT("parser endpoint rejected response_format=json_object");
			R.FailedStage = TEXT("llm_call");
			R.ParserRawJson = LLMResult.RawContent;
			UE_LOG(LogAILiveMemory, Error,
				TEXT("AILiveParser[%s] endpoint rejected response_format=json_object on model %s"),
				*Req.AgentId, *Ep.Model);
			return R;
		}

		// Stage 3: 输出 JSON 校验
		const FString JsonCandidate = LLMResult.ParsedJson.IsEmpty() ? LLMResult.RawContent : LLMResult.ParsedJson;
		FParseResult Out = ValidateParserOutputJson(JsonCandidate);
		Out.ParserRawJson = LLMResult.RawContent;

		const double Dt = (FPlatformTime::Seconds() - T0) * 1000.0;
		if (Out.bOk)
		{
			UE_LOG(LogAILiveMemory, Log,
				TEXT("AILiveParser[%s] OK in %.0fms (model=%s tokens=%d/%d)"),
				*Req.AgentId, Dt, *Ep.Model, LLMResult.PromptTokens, LLMResult.CompletionTokens);
		}
		else
		{
			UE_LOG(LogAILiveMemory, Warning,
				TEXT("AILiveParser[%s] Stage3 reject in %.0fms: %s"),
				*Req.AgentId, Dt, *Out.ErrorReason);
		}
		return Out;
	}
}
