// =============================================================================
// 中文教学：AILiveParserClient.cpp —— Parser 三段流程实现
//
// 文件分四块：
//   1) 匿名命名空间：parser 配置常量 + 模块作用域 cache（prompt 文本、模型名）
//   2) 协议版本 getter：实现 AILiveParserVersion.h 暴露的 4 个查询函数
//   3) PrevalidateRawTaggedSections：raw text 标签结构性预检
//   4) ValidateParserOutputJson + ParseFourChannels：完整流程
//
// 关键 UE / C++ 概念：
//   1) constexpr const TCHAR* + 模块作用域 cache 双重检查锁
//      `EnsureSystemPromptLoaded()` 第一次进锁 → 读盘 → 写 cache，后续只取 cache。
//      经典「double-check locking」简化版（这里没用 atomic 是因为 FScopeLock
//      已经是全锁，性能损失可忽略；如果 prompt 加载是热路径再考虑无锁）。
//
//   2) constexpr ELLMProvider kParserProvider = ELLMProvider::DeepSeek
//      编译期常量。Parser 模型选定后整个进程不变；改 provider 要重编译。
//
//   3) 显式三段错误标识 FailedStage = "raw_prevalidate" / "llm_call" / "output_validate"
//      调用方据此分流：raw_prevalidate 是 NPC 输出格式错（要追责到 Reasoner 模型），
//      llm_call 是网络/endpoint 问题（重试），output_validate 是 Parser 模型本身
//      偏离 schema（少见，往往要换模型或调 prompt）。
//
//   4) AsyncTask vs ThreadPool（在 AILiveParserSelfCheck.cpp 而不是这里）
//      本文件 ParseFourChannels 是阻塞同步函数，永远在调用方所在线程跑。
//      调用方负责把它放到 ThreadPool 后台线程（避免卡游戏线程）。
// =============================================================================

#include "LLM/AILiveParserClient.h"
#include "LLM/AILiveParserVersion.h"

#include "LLM/AILiveAgentRoster.h"
#include "LLM/OpenAIChatClient.h"
#include "Memory/AILiveEventTypes.h"   // LogAILiveMemory 日志类别

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
	// 中文教学：这是「单例 + 懒加载」模式 —— 多线程并发调用时，加锁保证只
	// 加载一次。GParser*Lock 保护对应的 Loaded/Cached 变量。
	FCriticalSection GParserPromptLock;
	bool             GParserPromptLoaded = false;   // 是否已经尝试过加载（不论成败）
	bool             GParserPromptLoadOk = false;   // 加载是否成功
	FString          GParserSystemPrompt;            // 缓存的 prompt 文本

	// 模型名缓存。同样的双重检查模式
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

	// 抽取「<TAG>...</TAG>」之间的文本。OpenTag/CloseTag 必须不重叠且按顺序出现。
	// 返回 true 才填 OutBody。中文教学：这是手写 mini parser，不上正则避免依赖。
	bool ExtractTagBody(const FString& Raw, const TCHAR* OpenTag, const TCHAR* CloseTag, FString& OutBody)
	{
		const int32 OpenAt = FindFirstSubstring(Raw, OpenTag);
		if (OpenAt == INDEX_NONE)
		{
			return false;
		}
		const int32 OpenLen = FCString::Strlen(OpenTag);
		// 从开标签后面开始找闭标签（避免嵌套时找错）
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

	// Stage 1：raw text 预检。
	// 中文教学：在调 LLM 之前先做廉价的本地校验，能挡掉 70%+ 的 NPC 格式错，
	// 节省 LLM 调用 token 和延迟。检查项：
	//   - 四个 XML 风格标签都齐全
	//   - <INTENDED> 内是合法 JSON object
	//   - <BID> 内是合法 JSON object（urgency 字段值留给 Stage 3 校验）
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

	// Stage 3：Parser LLM 输出 JSON 的字段类型校验。
	// 中文教学：每一段都按下面的 schema 严格校验：
	//   {
	//     "scratchpad":   string,      // 必填
	//     "intended":     { "text": string, ... },  // text 必填
	//     "bid":          { "urgency": number, ... },  // urgency 必填
	//     "note_to_self": string       // 必填
	//   }
	// 任一字段缺失或类型错 → bOk=false，FailedStage="output_validate"
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

	// 公开主入口：raw 预检 → LLM 调用 → 输出 JSON 校验。
	// 中文教学：函数体里能看出三段流水线的清晰分割，每段失败都立刻 return，
	// 不试图「部分恢复」。这种「fail-fast」风格让协议错误更容易定位（见 FailedStage）。
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
