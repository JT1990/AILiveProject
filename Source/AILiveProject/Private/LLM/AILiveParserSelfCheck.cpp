// =============================================================================
// 中文教学：AILiveParserSelfCheck.cpp —— Parser 自检命令（控制台触发）
//
// 这是什么：
//   注册一个控制台命令 `AILive.Test.ParserSelfCheck`，跑 5 个用例验证 Parser
//   端到端工作正常：
//     A) 真发 LLM 调用，期望返回完整四段并通过 Stage 3 校验
//     B) raw 缺 BID 段 → 期望 Stage 1 reject（reason 含 "missing bid section"）
//     C) raw INTENDED 不是 JSON → 期望 Stage 1 reject
//     D) GetCurrentParser* 4 个 getter 一致性
//     E) DB 中 schema_meta 与 API 返回值一致性（要先 BeginGame）
//
// 用法：在 PIE 中 ` 打开 console，敲 AILive.Test.ParserSelfCheck 回车。
//
// 关键 UE / C++ 概念：
//   1) FAutoConsoleCommand
//      静态全局对象，构造即注册控制台命令。析构（模块卸载）即注销。
//      RAII 注册模式 —— 不需要手动调 Register/Unregister。
//
//   2) AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, [](){...})
//      把 lambda 调度到 UE 任务图（TaskGraph）的后台线程池跑。
//      ENamedThreads:: 枚举有：GameThread / RenderThread / AudioThread /
//      AnyBackgroundThreadNormalTask 等。本函数同步阻塞，所以丢后台跑。
//      Lambda 通过值捕获 `[]`（不需要外部状态）。
//
//   3) FConsoleCommandDelegate::CreateStatic
//      把全局 static 函数包成 delegate。如果是成员函数用 CreateUObject 或
//      CreateRaw（区别：UObject 自动检查指针是否还活着）。
//
//   4) GEngine->GetWorldContexts() 找 PIE / Game World
//      多窗口编辑器/PIE 场景下可能有多个 World。这里挑第一个匹配的，拿 GameInstance
//      然后取 Subsystem。生产代码可以做更精细的选择（按 PlayerController）。
// =============================================================================

#include "LLM/AILiveParserClient.h"
#include "LLM/AILiveParserVersion.h"

#include "Memory/AILiveEventStoreSubsystem.h"
#include "Memory/AILiveEventTypes.h"      // LogAILiveMemory

#include "Async/Async.h"                    // AsyncTask + ENamedThreads
#include "Engine/Engine.h"                  // GEngine
#include "Engine/GameInstance.h"            // UGameInstance
#include "Engine/World.h"                   // UWorld / FWorldContext
#include "HAL/IConsoleManager.h"            // FAutoConsoleCommand
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace
{
	const TCHAR* kRawAllSectionsValid = TEXT(
		"<SCRATCHPAD>\n"
		"thinking about NPC05's last move; they sound suspicious.\n"
		"</SCRATCHPAD>\n"
		"<INTENDED>\n"
		"{\"text\":\"NPC05, can you explain the fridge thing?\","
		"\"intended_action\":{\"intent\":\"none\",\"params\":{}},"
		"\"addressed_to_hint\":[\"NPC05\"]}\n"
		"</INTENDED>\n"
		"<BID>\n"
		"{\"urgency\":7.5,\"proposed_target\":\"NPC05\","
		"\"rationale\":\"need to press them now before momentum shifts\"}\n"
		"</BID>\n"
		"<NOTE_TO_SELF>\n"
		"watch NPC05 next tick.\n"
		"</NOTE_TO_SELF>\n");

	const TCHAR* kRawMissingBid = TEXT(
		"<SCRATCHPAD>\n"
		"holding back this round.\n"
		"</SCRATCHPAD>\n"
		"<INTENDED>\n"
		"{\"text\":\"(passing)\"}\n"
		"</INTENDED>\n"
		"<NOTE_TO_SELF>\n"
		"low signal so far.\n"
		"</NOTE_TO_SELF>\n");

	const TCHAR* kRawInvalidIntendedJson = TEXT(
		"<SCRATCHPAD>\n"
		"trying to provoke a slip.\n"
		"</SCRATCHPAD>\n"
		"<INTENDED>\n"
		"not a json object at all\n"
		"</INTENDED>\n"
		"<BID>\n"
		"{\"urgency\":3.0,\"rationale\":\"low priority\"}\n"
		"</BID>\n"
		"<NOTE_TO_SELF>\n"
		"shake them later.\n"
		"</NOTE_TO_SELF>\n");

	void LogResult(const TCHAR* Label, const AILiveParser::FParseResult& R)
	{
		UE_LOG(LogAILiveMemory, Display,
			TEXT("[ParserSelfCheck:%s] bOk=%d stage=%s reason=%s scratch_len=%d intended_len=%d bid_len=%d note_len=%d"),
			Label, R.bOk ? 1 : 0,
			R.FailedStage.IsEmpty() ? TEXT("-") : *R.FailedStage,
			R.ErrorReason.IsEmpty() ? TEXT("-") : *R.ErrorReason,
			R.Scratchpad.Len(), R.IntendedJson.Len(), R.BidJson.Len(), R.NoteText.Len());
	}

	void RunCaseA_RealLLMSmoke()
	{
		AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, []()
		{
			AILiveParser::FParseRequest Req;
			Req.RawText = kRawAllSectionsValid;
			Req.AgentId = TEXT("selfcheck_A");
			const AILiveParser::FParseResult R = AILiveParser::ParseFourChannels(Req);
			LogResult(TEXT("A_RealLLMSmoke"), R);

			const bool bIntendedIsObject = R.IntendedJson.StartsWith(TEXT("{")) && R.IntendedJson.EndsWith(TEXT("}"));
			const bool bPass = R.bOk && !R.Scratchpad.IsEmpty() && !R.IntendedJson.IsEmpty()
				&& !R.BidJson.IsEmpty() && !R.NoteText.IsEmpty() && bIntendedIsObject;
			UE_LOG(LogAILiveMemory, Display,
				TEXT("[ParserSelfCheck:A_RealLLMSmoke] verdict=%s intended_is_object=%d"),
				bPass ? TEXT("PASS") : TEXT("FAIL"), bIntendedIsObject ? 1 : 0);
		});
	}

	void RunCaseB_MissingBid()
	{
		AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, []()
		{
			AILiveParser::FParseRequest Req;
			Req.RawText = kRawMissingBid;
			Req.AgentId = TEXT("selfcheck_B");
			const AILiveParser::FParseResult R = AILiveParser::ParseFourChannels(Req);
			LogResult(TEXT("B_MissingBid"), R);

			const bool bPass = !R.bOk
				&& R.FailedStage == TEXT("raw_prevalidate")
				&& R.ErrorReason.Contains(TEXT("missing bid section"));
			UE_LOG(LogAILiveMemory, Display,
				TEXT("[ParserSelfCheck:B_MissingBid] verdict=%s"), bPass ? TEXT("PASS") : TEXT("FAIL"));
		});
	}

	void RunCaseC_InvalidIntendedJson()
	{
		AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, []()
		{
			AILiveParser::FParseRequest Req;
			Req.RawText = kRawInvalidIntendedJson;
			Req.AgentId = TEXT("selfcheck_C");
			const AILiveParser::FParseResult R = AILiveParser::ParseFourChannels(Req);
			LogResult(TEXT("C_InvalidIntendedJson"), R);

			const bool bPass = !R.bOk
				&& R.FailedStage == TEXT("raw_prevalidate")
				&& R.ErrorReason.Contains(TEXT("intended payload not valid JSON"));
			UE_LOG(LogAILiveMemory, Display,
				TEXT("[ParserSelfCheck:C_InvalidIntendedJson] verdict=%s"), bPass ? TEXT("PASS") : TEXT("FAIL"));
		});
	}

	void RunCaseD_ApiValues()
	{
		const FString Ver         = AILiveParser::GetCurrentParserVersion();
		const FString RegistryDir = AILiveParser::GetCurrentParserPromptRegistryDir();
		const FString PromptPath  = AILiveParser::GetCurrentParserPromptPath();
		const FString Model       = AILiveParser::GetCurrentParserModel();

		const FString AbsPromptPath = FPaths::ProjectContentDir() / TEXT("Prompts/Parser/v1.txt");
		FString FileBody;
		const bool bFileLoaded = FFileHelper::LoadFileToString(FileBody, *AbsPromptPath);

		const bool bVerOk      = Ver == TEXT("1");
		const bool bRegOk      = RegistryDir == TEXT("Content/Prompts/Parser/");
		const bool bRelOk      = PromptPath == TEXT("Content/Prompts/Parser/v1.txt");
		const bool bModelOk    = !Model.IsEmpty() && Model.StartsWith(TEXT("qwen3:"));
		const bool bFileOk     = bFileLoaded && FileBody.Len() > 0;

		UE_LOG(LogAILiveMemory, Display,
			TEXT("[ParserSelfCheck:D_ApiValues] ver='%s' reg='%s' rel='%s' model='%s' file_len=%d"),
			*Ver, *RegistryDir, *PromptPath, *Model, FileBody.Len());

		const bool bPass = bVerOk && bRegOk && bRelOk && bModelOk && bFileOk;
		UE_LOG(LogAILiveMemory, Display,
			TEXT("[ParserSelfCheck:D_ApiValues] verdict=%s ver_ok=%d reg_ok=%d rel_ok=%d model_ok=%d file_ok=%d"),
			bPass ? TEXT("PASS") : TEXT("FAIL"),
			bVerOk?1:0, bRegOk?1:0, bRelOk?1:0, bModelOk?1:0, bFileOk?1:0);
	}

	UAILiveEventStoreSubsystem* GetEventStoreForConsole()
	{
		if (!GEngine)
		{
			return nullptr;
		}
		for (const FWorldContext& Ctx : GEngine->GetWorldContexts())
		{
			if ((Ctx.WorldType == EWorldType::PIE || Ctx.WorldType == EWorldType::Game) && Ctx.World())
			{
				if (UGameInstance* GI = Ctx.World()->GetGameInstance())
				{
					return GI->GetSubsystem<UAILiveEventStoreSubsystem>();
				}
			}
		}
		if (GWorld)
		{
			if (UGameInstance* GI = GWorld->GetGameInstance())
			{
				return GI->GetSubsystem<UAILiveEventStoreSubsystem>();
			}
		}
		return nullptr;
	}

	void RunCaseE_RegistryConsistency()
	{
		// 走子系统主连接读 schema_meta —— UE 5.7 SQLiteCore + WAL 同进程二次
		// 连接（ReadOnly 实测、ReadWrite 实测）都拿不到 -shm 共享映射，回 SQLITE_IOERR。
		UAILiveEventStoreSubsystem* Sys = GetEventStoreForConsole();
		if (!Sys || !Sys->IsGameOpen())
		{
			UE_LOG(LogAILiveMemory, Display,
				TEXT("[ParserSelfCheck:E_RegistryConsistency] verdict=FAIL (no game open; run AILive.Test.BeginGame first)"));
			return;
		}

		TMap<FString, FString> KVs;
		if (!Sys->QueryMetaSchemaRegistry(KVs))
		{
			UE_LOG(LogAILiveMemory, Display,
				TEXT("[ParserSelfCheck:E_RegistryConsistency] verdict=FAIL (QueryMetaSchemaRegistry error)"));
			return;
		}

		const FString RegFromDb   = KVs.FindRef(TEXT("parser_prompt_registry_path"));
		const FString VerFromDb   = KVs.FindRef(TEXT("parser_version"));
		const FString ModelFromDb = KVs.FindRef(TEXT("parser_model"));

		const FString PromptPathFromApi = AILiveParser::GetCurrentParserPromptPath();
		const FString Reconstructed     = RegFromDb + TEXT("v") + VerFromDb + TEXT(".txt");

		UE_LOG(LogAILiveMemory, Display,
			TEXT("[ParserSelfCheck:E_RegistryConsistency] kvs=%d reg='%s' ver='%s' model='%s' reconstructed='%s' api='%s'"),
			KVs.Num(), *RegFromDb, *VerFromDb, *ModelFromDb, *Reconstructed, *PromptPathFromApi);

		const bool bPass = !RegFromDb.IsEmpty() && !VerFromDb.IsEmpty() && !ModelFromDb.IsEmpty()
			&& Reconstructed == PromptPathFromApi
			&& Reconstructed == TEXT("Content/Prompts/Parser/v1.txt")
			&& VerFromDb == TEXT("1")
			&& RegFromDb == TEXT("Content/Prompts/Parser/")
			&& ModelFromDb.StartsWith(TEXT("qwen3:"));
		UE_LOG(LogAILiveMemory, Display,
			TEXT("[ParserSelfCheck:E_RegistryConsistency] verdict=%s"), bPass ? TEXT("PASS") : TEXT("FAIL"));
	}

	void RunParserSelfCheck()
	{
		UE_LOG(LogAILiveMemory, Display, TEXT("[ParserSelfCheck] starting (sync D/E first; A/B/C dispatch async)"));
		RunCaseD_ApiValues();
		RunCaseE_RegistryConsistency();
		RunCaseB_MissingBid();
		RunCaseC_InvalidIntendedJson();
		RunCaseA_RealLLMSmoke();
		UE_LOG(LogAILiveMemory, Display,
			TEXT("[ParserSelfCheck] dispatched; async cases A/B/C verdict will appear when LLM/threadpool returns"));
	}
}

// 中文教学：FAutoConsoleCommand 的全局 static 实例，构造时即注册到 UE console。
// 三个参数：命令名、help 文本（敲 ? 时显示）、要执行的 delegate。
// CreateStatic 把普通函数 RunParserSelfCheck 包成 delegate；如果要绑成员函数
// 需要 CreateRaw / CreateUObject。
static FAutoConsoleCommand CCmdParserSelfCheck(
	TEXT("AILive.Test.ParserSelfCheck"),
	TEXT("Run T2.5 Parser self-check: 5 cases A/B/C/D/E (D/E sync, A/B/C async; A calls real Parser LLM)."),
	FConsoleCommandDelegate::CreateStatic(&RunParserSelfCheck));
