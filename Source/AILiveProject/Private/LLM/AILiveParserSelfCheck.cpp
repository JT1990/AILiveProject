#include "LLM/AILiveParserClient.h"
#include "LLM/AILiveParserVersion.h"

#include "Memory/AILiveEventTypes.h"

#include "Async/Async.h"
#include "HAL/IConsoleManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "SQLiteDatabase.h"
#include "SQLitePreparedStatement.h"

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

	void RunCaseE_RegistryConsistency()
	{
		const FString MetaDbPath = FPaths::ProjectSavedDir() / TEXT("Games") / TEXT("_meta.db");
		FSQLiteDatabase Reader;
		if (!Reader.Open(*MetaDbPath, ESQLiteDatabaseOpenMode::ReadOnly))
		{
			UE_LOG(LogAILiveMemory, Error,
				TEXT("[ParserSelfCheck:E_RegistryConsistency] cannot open _meta.db ReadOnly: %s (%s)"),
				*MetaDbPath, *Reader.GetLastError());
			UE_LOG(LogAILiveMemory, Display,
				TEXT("[ParserSelfCheck:E_RegistryConsistency] verdict=FAIL (no _meta.db; run AILive.Test.BeginGame first)"));
			return;
		}

		FString RegFromDb;
		FString VerFromDb;
		FString ModelFromDb;

		const TCHAR* Sql = TEXT("SELECT key, value FROM schema_meta WHERE key IN ('parser_prompt_registry_path','parser_version','parser_model');");
		const int64 Rows = Reader.Execute(Sql,
			[&RegFromDb, &VerFromDb, &ModelFromDb](const FSQLitePreparedStatement& Stmt)
			{
				FString K, V;
				Stmt.GetColumnValueByIndex(0, K);
				Stmt.GetColumnValueByIndex(1, V);
				if (K == TEXT("parser_prompt_registry_path")) RegFromDb = V;
				else if (K == TEXT("parser_version"))         VerFromDb = V;
				else if (K == TEXT("parser_model"))           ModelFromDb = V;
				return ESQLitePreparedStatementExecuteRowResult::Continue;
			});
		Reader.Close();

		const FString PromptPathFromApi = AILiveParser::GetCurrentParserPromptPath();
		const FString Reconstructed     = RegFromDb + TEXT("v") + VerFromDb + TEXT(".txt");

		UE_LOG(LogAILiveMemory, Display,
			TEXT("[ParserSelfCheck:E_RegistryConsistency] rows=%lld reg='%s' ver='%s' model='%s' reconstructed='%s' api='%s'"),
			Rows, *RegFromDb, *VerFromDb, *ModelFromDb, *Reconstructed, *PromptPathFromApi);

		const bool bPass = (Rows == 3)
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

static FAutoConsoleCommand CCmdParserSelfCheck(
	TEXT("AILive.Test.ParserSelfCheck"),
	TEXT("Run T2.5 Parser self-check: 6 acceptance cases (D/E sync, A/B/C async)."),
	FConsoleCommandDelegate::CreateStatic(&RunParserSelfCheck));
