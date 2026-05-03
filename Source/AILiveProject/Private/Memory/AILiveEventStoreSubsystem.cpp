#include "Memory/AILiveEventStoreSubsystem.h"

#include "LLM/AILiveParserVersion.h"

#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "Misc/Paths.h"
#include "SQLitePreparedStatement.h"

namespace AILiveSchemaMigration
{
	bool ApplyMigrationV0ToV1(FSQLiteDatabase& InDb, bool bIsMetaDb, const TCHAR* FtsTokenizer);
}

const TCHAR* const UAILiveEventStoreSubsystem::kGenesisHash =
	TEXT("0000000000000000000000000000000000000000000000000000000000000000");

namespace
{

FString MakeGameDbPath(const FString& InGameId)
{
	const FString Dir = FPaths::ProjectSavedDir() / TEXT("Games");
	return Dir / (InGameId + TEXT(".db"));
}

FString MakeMetaDbPath()
{
	const FString Dir = FPaths::ProjectSavedDir() / TEXT("Games");
	return Dir / TEXT("_meta.db");
}

bool EnsureSavedGamesDir()
{
	const FString Dir = FPaths::ProjectSavedDir() / TEXT("Games");
	return IFileManager::Get().MakeDirectory(*Dir, /*Tree=*/true);
}

UWorld* GetActiveWorldForConsole()
{
	if (GEngine)
	{
		for (const FWorldContext& Ctx : GEngine->GetWorldContexts())
		{
			if ((Ctx.WorldType == EWorldType::PIE || Ctx.WorldType == EWorldType::Game) && Ctx.World())
			{
				return Ctx.World();
			}
		}
	}
	return GWorld;
}

UAILiveEventStoreSubsystem* GetSubsystemForConsole()
{
	UWorld* World = GetActiveWorldForConsole();
	if (!World)
	{
		UE_LOG(LogAILiveMemory, Error, TEXT("Console: no active PIE/Game world"));
		return nullptr;
	}
	UGameInstance* GI = World->GetGameInstance();
	if (!GI)
	{
		UE_LOG(LogAILiveMemory, Error, TEXT("Console: world has no GameInstance"));
		return nullptr;
	}
	UAILiveEventStoreSubsystem* Sys = GI->GetSubsystem<UAILiveEventStoreSubsystem>();
	if (!Sys)
	{
		UE_LOG(LogAILiveMemory, Error, TEXT("Console: UAILiveEventStoreSubsystem not found on GameInstance"));
	}
	return Sys;
}

void LogQueryRows(FSQLiteDatabase& InDb, const TCHAR* InSql, const TCHAR* InLabel, int32 InColumnCount)
{
	UE_LOG(LogAILiveMemory, Display, TEXT("[%s] %s"), InLabel, InSql);
	const int64 Rows = InDb.Execute(InSql, [InLabel, InColumnCount](const FSQLitePreparedStatement& Stmt)
	{
		FString Line;
		for (int32 i = 0; i < InColumnCount; ++i)
		{
			FString Val;
			Stmt.GetColumnValueByIndex(i, Val);
			if (i > 0) Line += TEXT(" | ");
			Line += Val;
		}
		UE_LOG(LogAILiveMemory, Display, TEXT("  [%s] %s"), InLabel, *Line);
		return ESQLitePreparedStatementExecuteRowResult::Continue;
	});
	UE_LOG(LogAILiveMemory, Display, TEXT("  [%s] -> %lld rows"), InLabel, Rows);
}

} // namespace anonymous

void UAILiveEventStoreSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	UE_LOG(LogAILiveMemory, Log, TEXT("EventStoreSubsystem initialized"));
}

void UAILiveEventStoreSubsystem::Deinitialize()
{
	if (IsGameOpen())
	{
		EndGame();
	}
	Super::Deinitialize();
}

bool UAILiveEventStoreSubsystem::BeginGame(const FString& InGameId)
{
	if (InGameId.IsEmpty())
	{
		UE_LOG(LogAILiveMemory, Error, TEXT("BeginGame: empty game_id"));
		return false;
	}
	if (IsGameOpen())
	{
		UE_LOG(LogAILiveMemory, Warning, TEXT("BeginGame('%s'): closing previous game '%s'"), *InGameId, *CurrentGameId);
		EndGame();
	}

	if (!EnsureSavedGamesDir())
	{
		UE_LOG(LogAILiveMemory, Error, TEXT("BeginGame: failed to create Saved/Games directory"));
		return false;
	}

	const FString GameDbPath = MakeGameDbPath(InGameId);
	if (!Db.Open(*GameDbPath, ESQLiteDatabaseOpenMode::ReadWriteCreate))
	{
		UE_LOG(LogAILiveMemory, Error, TEXT("BeginGame: failed to open game.db at %s: %s"),
			*GameDbPath, *Db.GetLastError());
		return false;
	}
	ApplyPragmas(Db);
	if (!EnsureSchema(Db, /*bIsMetaDb=*/false))
	{
		Db.Close();
		return false;
	}

	const FString MetaDbPath = MakeMetaDbPath();
	if (!MetaDb.Open(*MetaDbPath, ESQLiteDatabaseOpenMode::ReadWriteCreate))
	{
		UE_LOG(LogAILiveMemory, Error, TEXT("BeginGame: failed to open _meta.db at %s: %s"),
			*MetaDbPath, *MetaDb.GetLastError());
		Db.Close();
		return false;
	}
	ApplyPragmas(MetaDb);
	if (!EnsureSchema(MetaDb, /*bIsMetaDb=*/true))
	{
		MetaDb.Close();
		Db.Close();
		return false;
	}
	if (!EnsureMetaRegistry(MetaDb))
	{
		MetaDb.Close();
		Db.Close();
		return false;
	}

	CurrentGameId = InGameId;
	UE_LOG(LogAILiveMemory, Log, TEXT("BeginGame('%s') OK: %s + %s"), *CurrentGameId, *GameDbPath, *MetaDbPath);
	return true;
}

void UAILiveEventStoreSubsystem::EndGame()
{
	if (Db.IsValid())
	{
		Db.Close();
	}
	if (MetaDb.IsValid())
	{
		MetaDb.Close();
	}
	UE_LOG(LogAILiveMemory, Log, TEXT("EndGame: closed (was '%s')"), *CurrentGameId);
	CurrentGameId.Reset();
}

void UAILiveEventStoreSubsystem::ApplyPragmas(FSQLiteDatabase& InDb)
{
	// journal_mode=WAL is database-level (persisted in the .db header) but
	// repeating it is idempotent and corrects any non-WAL legacy file.
	const TCHAR* Pragmas[] =
	{
		TEXT("PRAGMA journal_mode = WAL;"),
		TEXT("PRAGMA synchronous = NORMAL;"),
		TEXT("PRAGMA foreign_keys = ON;"),
		TEXT("PRAGMA temp_store = MEMORY;"),
		TEXT("PRAGMA mmap_size = 268435456;"),
	};
	for (const TCHAR* P : Pragmas)
	{
		if (!InDb.Execute(P))
		{
			UE_LOG(LogAILiveMemory, Warning, TEXT("PRAGMA failed (%s): %s"), P, *InDb.GetLastError());
		}
	}
}

FString UAILiveEventStoreSubsystem::DetectFtsTokenizer(FSQLiteDatabase& InDb)
{
	InDb.Execute(TEXT("DROP TABLE IF EXISTS temp.test_trigram;"));
	const bool bTrigramOk = InDb.Execute(
		TEXT("CREATE VIRTUAL TABLE temp.test_trigram USING fts5(x, tokenize='trigram');"));
	if (bTrigramOk)
	{
		InDb.Execute(TEXT("DROP TABLE temp.test_trigram;"));
		return TEXT("trigram");
	}
	UE_LOG(LogAILiveMemory, Warning,
		TEXT("FTS5 trigram tokenizer 不可用，降级 unicode61，模糊搜索能力受限。底层错误: %s"),
		*InDb.GetLastError());
	return TEXT("unicode61");
}

bool UAILiveEventStoreSubsystem::EnsureSchema(FSQLiteDatabase& InDb, bool bIsMetaDb)
{
	int32 ExistingVersion = 0;
	bool bSchemaMetaPresent = false;

	{
		// Probe schema_meta existence; empty result = fresh DB.
		const int64 Rows = InDb.Execute(
			TEXT("SELECT name FROM sqlite_master WHERE type='table' AND name='schema_meta';"),
			[&bSchemaMetaPresent](const FSQLitePreparedStatement&)
			{
				bSchemaMetaPresent = true;
				return ESQLitePreparedStatementExecuteRowResult::Continue;
			});
		if (Rows == INDEX_NONE)
		{
			UE_LOG(LogAILiveMemory, Error, TEXT("EnsureSchema: probe sqlite_master failed: %s"), *InDb.GetLastError());
			return false;
		}
	}

	if (bSchemaMetaPresent)
	{
		const int64 Rows = InDb.Execute(
			TEXT("SELECT value FROM schema_meta WHERE key='schema_version';"),
			[&ExistingVersion](const FSQLitePreparedStatement& Stmt)
			{
				FString Val;
				if (Stmt.GetColumnValueByIndex(0, Val))
				{
					ExistingVersion = FCString::Atoi(*Val);
				}
				return ESQLitePreparedStatementExecuteRowResult::Continue;
			});
		if (Rows == INDEX_NONE)
		{
			UE_LOG(LogAILiveMemory, Error, TEXT("EnsureSchema: read schema_version failed: %s"), *InDb.GetLastError());
			return false;
		}
	}

	if (ExistingVersion == kCurrentSchemaVersion)
	{
		UE_LOG(LogAILiveMemory, Log, TEXT("EnsureSchema(%s): schema_version=%d, no migration"),
			bIsMetaDb ? TEXT("meta") : TEXT("game"), ExistingVersion);
		return true;
	}
	if (ExistingVersion > kCurrentSchemaVersion)
	{
		UE_LOG(LogAILiveMemory, Error,
			TEXT("EnsureSchema(%s): existing schema_version=%d > supported=%d (downgrade refused)"),
			bIsMetaDb ? TEXT("meta") : TEXT("game"), ExistingVersion, kCurrentSchemaVersion);
		return false;
	}

	const FString FtsTokenizer = bIsMetaDb ? FString() : DetectFtsTokenizer(InDb);
	const TCHAR* TokenizerPtr = bIsMetaDb ? nullptr : *FtsTokenizer;

	return RunMigrations(InDb, ExistingVersion, kCurrentSchemaVersion, bIsMetaDb, TokenizerPtr);
}

bool UAILiveEventStoreSubsystem::RunMigrations(FSQLiteDatabase& InDb, int32 FromVersion, int32 ToVersion, bool bIsMetaDb, const TCHAR* FtsTokenizer)
{
	if (FromVersion == 0 && ToVersion == 1)
	{
		const bool bOk = AILiveSchemaMigration::ApplyMigrationV0ToV1(InDb, bIsMetaDb, FtsTokenizer);
		if (!bOk)
		{
			UE_LOG(LogAILiveMemory, Error, TEXT("Migration 0->1 failed (bIsMetaDb=%d)"), bIsMetaDb ? 1 : 0);
		}
		return bOk;
	}
	UE_LOG(LogAILiveMemory, Error, TEXT("RunMigrations: unsupported %d -> %d"), FromVersion, ToVersion);
	return false;
}

bool UAILiveEventStoreSubsystem::EnsureMetaRegistry(FSQLiteDatabase& InMetaDb)
{
	const FString RegistryDir   = AILiveParser::GetCurrentParserPromptRegistryDir();
	const FString ParserVersion = AILiveParser::GetCurrentParserVersion();
	const FString ParserModel   = AILiveParser::GetCurrentParserModel();

	if (RegistryDir.IsEmpty() || ParserVersion.IsEmpty() || ParserModel.IsEmpty())
	{
		UE_LOG(LogAILiveMemory, Error,
			TEXT("EnsureMetaRegistry: missing registry field(s) (dir='%s' ver='%s' model='%s')"),
			*RegistryDir, *ParserVersion, *ParserModel);
		return false;
	}

	const TPair<FString, FString> Pairs[] = {
		{ TEXT("parser_prompt_registry_path"), RegistryDir },
		{ TEXT("parser_version"),              ParserVersion },
		{ TEXT("parser_model"),                ParserModel },
	};

	const TCHAR* Sql = TEXT("INSERT OR REPLACE INTO schema_meta(key, value) VALUES(?1, ?2);");

	for (const TPair<FString, FString>& KV : Pairs)
	{
		FSQLitePreparedStatement Stmt;
		if (!Stmt.Create(InMetaDb, Sql) ||
			!Stmt.SetBindingValueByIndex(1, KV.Key) ||
			!Stmt.SetBindingValueByIndex(2, KV.Value) ||
			!Stmt.Execute())
		{
			UE_LOG(LogAILiveMemory, Error,
				TEXT("EnsureMetaRegistry upsert '%s' failed: %s"),
				*KV.Key, *InMetaDb.GetLastError());
			return false;
		}
	}
	UE_LOG(LogAILiveMemory, Log,
		TEXT("EnsureMetaRegistry OK: parser_version=%s parser_model=%s"),
		*ParserVersion, *ParserModel);
	return true;
}

// ---------------------------------------------------------------
// Safe stubs — real implementations land in T3 / T4 / T9.
// ---------------------------------------------------------------

bool UAILiveEventStoreSubsystem::AppendEvent(const FAILiveEvent& InEvent)
{
	UE_LOG(LogAILiveMemory, Verbose, TEXT("AppendEvent stub (T3): event_type=%d actor=%s"),
		(int32)InEvent.EventType, *InEvent.Actor);
	return false;
}

TArray<FAILiveEvent> UAILiveEventStoreSubsystem::QueryEventsByActor(const FString& Actor, int32 LimitCount) const
{
	UE_LOG(LogAILiveMemory, Verbose, TEXT("QueryEventsByActor stub (T4): actor=%s limit=%d"), *Actor, LimitCount);
	return {};
}

bool UAILiveEventStoreSubsystem::VerifyHashChain(int64& OutFirstBadSeq) const
{
	UE_LOG(LogAILiveMemory, Verbose, TEXT("VerifyHashChain stub (T9)"));
	OutFirstBadSeq = INDEX_NONE;
	return false;
}

// ---------------------------------------------------------------
// Console commands — debug helpers, scoped to T2 acceptance.
// ---------------------------------------------------------------

static FAutoConsoleCommand GAILiveTestBeginGame(
	TEXT("AILive.Test.BeginGame"),
	TEXT("AILive.Test.BeginGame <game_id> — open Saved/Games/<game_id>.db + _meta.db, run schema migration"),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
	{
		if (Args.Num() < 1)
		{
			UE_LOG(LogAILiveMemory, Error, TEXT("Usage: AILive.Test.BeginGame <game_id>"));
			return;
		}
		UAILiveEventStoreSubsystem* Sys = GetSubsystemForConsole();
		if (!Sys) return;
		const bool bOk = Sys->BeginGame(Args[0]);
		UE_LOG(LogAILiveMemory, Display, TEXT("AILive.Test.BeginGame('%s') -> %s"), *Args[0], bOk ? TEXT("OK") : TEXT("FAIL"));
	}));

static FAutoConsoleCommand GAILiveTestEndGame(
	TEXT("AILive.Test.EndGame"),
	TEXT("AILive.Test.EndGame — close currently open game.db + _meta.db"),
	FConsoleCommandDelegate::CreateLambda([]()
	{
		UAILiveEventStoreSubsystem* Sys = GetSubsystemForConsole();
		if (!Sys) return;
		Sys->EndGame();
		UE_LOG(LogAILiveMemory, Display, TEXT("AILive.Test.EndGame OK"));
	}));

static FAutoConsoleCommand GAILiveTestSchemaSelfCheck(
	TEXT("AILive.Test.SchemaSelfCheck"),
	TEXT("AILive.Test.SchemaSelfCheck — list tables / triggers / schema_version / events table_xinfo / agent_calibration columns; both DBs"),
	FConsoleCommandDelegate::CreateLambda([]()
	{
		UAILiveEventStoreSubsystem* Sys = GetSubsystemForConsole();
		if (!Sys) return;
		if (!Sys->IsGameOpen())
		{
			UE_LOG(LogAILiveMemory, Error, TEXT("SchemaSelfCheck: no game open; call AILive.Test.BeginGame first"));
			return;
		}
		// Open a separate read-only connection (WAL allows concurrent readers) to inspect schema
		// without touching the live read/write Db owned by the subsystem.
		const FString GameDbPath = FPaths::ProjectSavedDir() / TEXT("Games") / (Sys->GetCurrentGameId() + TEXT(".db"));
		const FString MetaDbPath = FPaths::ProjectSavedDir() / TEXT("Games") / TEXT("_meta.db");

		FSQLiteDatabase Reader;
		if (!Reader.Open(*GameDbPath, ESQLiteDatabaseOpenMode::ReadOnly))
		{
			UE_LOG(LogAILiveMemory, Error, TEXT("SchemaSelfCheck: read-only open failed for %s: %s"),
				*GameDbPath, *Reader.GetLastError());
			return;
		}
		LogQueryRows(Reader,
			TEXT("SELECT name FROM sqlite_master WHERE type IN ('table','view') AND name NOT LIKE 'sqlite_%' AND name NOT LIKE 'events_fts_%' ORDER BY name;"),
			TEXT("game.tables"), 1);
		LogQueryRows(Reader,
			TEXT("SELECT name FROM sqlite_master WHERE type='trigger' ORDER BY name;"),
			TEXT("game.triggers"), 1);
		LogQueryRows(Reader,
			TEXT("SELECT value FROM schema_meta WHERE key='schema_version';"),
			TEXT("game.schema_version"), 1);
		LogQueryRows(Reader,
			TEXT("SELECT cid, name, type, hidden FROM pragma_table_xinfo('events');"),
			TEXT("events.table_xinfo"), 4);
		LogQueryRows(Reader,
			TEXT("SELECT cid, name, type FROM pragma_table_info('events');"),
			TEXT("events.table_info"), 3);
		LogQueryRows(Reader,
			TEXT("SELECT name FROM pragma_table_info('agent_calibration');"),
			TEXT("agent_calibration.cols"), 1);
		LogQueryRows(Reader,
			TEXT("SELECT sql FROM sqlite_master WHERE name='events_fts';"),
			TEXT("events_fts.sql"), 1);
		Reader.Close();

		FSQLiteDatabase MetaReader;
		if (!MetaReader.Open(*MetaDbPath, ESQLiteDatabaseOpenMode::ReadOnly))
		{
			UE_LOG(LogAILiveMemory, Error, TEXT("SchemaSelfCheck: read-only open failed for %s: %s"),
				*MetaDbPath, *MetaReader.GetLastError());
			return;
		}
		LogQueryRows(MetaReader,
			TEXT("SELECT name FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%' ORDER BY name;"),
			TEXT("meta.tables"), 1);
		LogQueryRows(MetaReader,
			TEXT("SELECT name FROM sqlite_master WHERE type='trigger' ORDER BY name;"),
			TEXT("meta.triggers"), 1);
		LogQueryRows(MetaReader,
			TEXT("SELECT value FROM schema_meta WHERE key='schema_version';"),
			TEXT("meta.schema_version"), 1);
		MetaReader.Close();

		UE_LOG(LogAILiveMemory, Display, TEXT("SchemaSelfCheck done"));
	}));
