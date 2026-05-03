#include "Memory/AILiveEventStoreSubsystem.h"

#include "LLM/AILiveParserVersion.h"
#include "Util/AILiveJsonEscape.h"
#include "Util/AILiveSha256.h"

#include "Async/Async.h"
#include "Async/Future.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "HAL/ThreadSafeCounter.h"
#include "Misc/DateTime.h"
#include "Misc/Paths.h"
#include "Misc/Timespan.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
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

// ===== T3 helpers =====

/** Closed-set viewer namespace (schema.yaml header). "self" forbidden. */
bool IsValidViewerString(const FString& V)
{
	if (V == TEXT("public")   || V == TEXT("audience") ||
		V == TEXT("orchestrator") || V == TEXT("system"))
	{
		return true;
	}
	if (V.StartsWith(TEXT("NPC")) && V.Len() >= 4 && V.Len() <= 6)
	{
		for (int32 i = 3; i < V.Len(); ++i)
		{
			if (!FChar::IsDigit(V[i])) return false;
		}
		return true;
	}
	if (V.StartsWith(TEXT("Faction")) && V.Len() > 7)
	{
		return true;
	}
	return false;
}

using FCanonicalWriter =
	TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>;
using FCanonicalWriterRef =
	TSharedRef<FCanonicalWriter>;
using FCanonicalWriterFactory =
	TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>;

/** Format a double for canonical JSON: %.17g + force ".0" suffix when the
 *  value is integer-valued so 1.0 doesn't collapse to "1".
 *  Per memory_principles.md §二底层不变量 #8: floats must IEEE round-trip,
 *  e.g. "1.0" not "1" or "1.000". UE TJsonWriter::WriteValue(double) drops
 *  the trailing ".0" for integer-valued doubles, so we bypass it. */
FString CanonicalDoubleToString(double D)
{
	FString S = FString::Printf(TEXT("%.17g"), D);
	const bool bHasDot = S.Contains(TEXT("."));
	const bool bHasExp = S.Contains(TEXT("e")) || S.Contains(TEXT("E"));
	const bool bIsSpecial = S.Contains(TEXT("nan")) || S.Contains(TEXT("inf"));
	if (!bHasDot && !bHasExp && !bIsSpecial)
	{
		S += TEXT(".0");
	}
	return S;
}

/** Recursively emit a JSON value with deterministic ordering. */
void WriteCanonicalValue(const TSharedPtr<FJsonValue>& V, FCanonicalWriter& W)
{
	if (!V.IsValid())
	{
		W.WriteNull();
		return;
	}
	switch (V->Type)
	{
	case EJson::Null:
		W.WriteNull();
		break;
	case EJson::Boolean:
		W.WriteValue(V->AsBool());
		break;
	case EJson::Number:
		// Bypass WriteValue(double) to control IEEE round-trip formatting.
		W.WriteRawJSONValue(CanonicalDoubleToString(V->AsNumber()));
		break;
	case EJson::String:
		W.WriteValue(V->AsString());
		break;
	case EJson::Array:
	{
		W.WriteArrayStart();
		for (const TSharedPtr<FJsonValue>& Elem : V->AsArray())
		{
			WriteCanonicalValue(Elem, W);
		}
		W.WriteArrayEnd();
		break;
	}
	case EJson::Object:
	{
		const TSharedPtr<FJsonObject> Obj = V->AsObject();
		W.WriteObjectStart();
		if (Obj.IsValid())
		{
			TArray<FString> Keys;
			Obj->Values.GetKeys(Keys);
			Keys.Sort();
			for (const FString& K : Keys)
			{
				W.WriteIdentifierPrefix(K);
				WriteCanonicalValue(Obj->Values[K], W);
			}
		}
		W.WriteObjectEnd();
		break;
	}
	default:
		W.WriteNull();
		break;
	}
}

void WriteCanonicalObject(const TSharedPtr<FJsonObject>& Obj, FCanonicalWriter& W)
{
	W.WriteObjectStart();
	if (Obj.IsValid())
	{
		TArray<FString> Keys;
		Obj->Values.GetKeys(Keys);
		Keys.Sort();
		for (const FString& K : Keys)
		{
			W.WriteIdentifierPrefix(K);
			WriteCanonicalValue(Obj->Values[K], W);
		}
	}
	W.WriteObjectEnd();
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

	if (!LoadHashChainTail())
	{
		UE_LOG(LogAILiveMemory, Error, TEXT("BeginGame('%s'): hash chain tail load failed"), *CurrentGameId);
		MetaDb.Close();
		Db.Close();
		CurrentGameId.Reset();
		return false;
	}

	UE_LOG(LogAILiveMemory, Log,
		TEXT("BeginGame('%s') OK: %s + %s (last_seq=%lld, last_hash_prefix=%s)"),
		*CurrentGameId, *GameDbPath, *MetaDbPath,
		CachedLastSeq, *CachedLastHash.Left(8));
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
	CachedLastSeq = 0;
	CachedLastHash.Reset();
	CachedCurrentTickNo = 0;
}

bool UAILiveEventStoreSubsystem::LoadHashChainTail()
{
	CachedLastSeq = 0;
	CachedLastHash = FString(kGenesisHash);
	CachedCurrentTickNo = 0;

	FSQLitePreparedStatement Stmt;
	if (!Stmt.Create(Db,
		TEXT("SELECT seq, event_hash FROM events WHERE game_id=?1 ORDER BY seq DESC LIMIT 1;")))
	{
		UE_LOG(LogAILiveMemory, Error,
			TEXT("LoadHashChainTail: prepare failed: %s"), *Db.GetLastError());
		return false;
	}
	if (!Stmt.SetBindingValueByIndex(1, CurrentGameId))
	{
		UE_LOG(LogAILiveMemory, Error, TEXT("LoadHashChainTail: bind game_id failed"));
		return false;
	}
	const ESQLitePreparedStatementStepResult Step = Stmt.Step();
	if (Step == ESQLitePreparedStatementStepResult::Row)
	{
		Stmt.GetColumnValueByIndex(0, CachedLastSeq);
		Stmt.GetColumnValueByIndex(1, CachedLastHash);
	}
	else if (Step != ESQLitePreparedStatementStepResult::Done)
	{
		UE_LOG(LogAILiveMemory, Error,
			TEXT("LoadHashChainTail: step failed: %s"), *Db.GetLastError());
		return false;
	}
	return true;
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

bool UAILiveEventStoreSubsystem::QueryMetaSchemaRegistry(TMap<FString, FString>& OutKVs)
{
	OutKVs.Reset();
	if (!MetaDb.IsValid())
	{
		UE_LOG(LogAILiveMemory, Error, TEXT("QueryMetaSchemaRegistry: MetaDb 未打开（先 BeginGame）"));
		return false;
	}
	const int64 Rows = MetaDb.Execute(
		TEXT("SELECT key, value FROM schema_meta;"),
		[&OutKVs](const FSQLitePreparedStatement& Stmt)
		{
			FString K, V;
			Stmt.GetColumnValueByIndex(0, K);
			Stmt.GetColumnValueByIndex(1, V);
			OutKVs.Add(K, V);
			return ESQLitePreparedStatementExecuteRowResult::Continue;
		});
	if (Rows == INDEX_NONE)
	{
		UE_LOG(LogAILiveMemory, Error,
			TEXT("QueryMetaSchemaRegistry: SELECT failed: %s"), *MetaDb.GetLastError());
		return false;
	}
	return true;
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
// Stubs for T4 (read API) and T9 (hash chain audit).
// ---------------------------------------------------------------

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
// T3 — Static validation
// ---------------------------------------------------------------

bool UAILiveEventStoreSubsystem::ValidateVisibility(const TArray<FString>& InVisibility, FString& OutError)
{
	if (InVisibility.Num() == 0)
	{
		OutError = TEXT("visibility array must be non-empty");
		return false;
	}
	for (const FString& V : InVisibility)
	{
		if (V == TEXT("self"))
		{
			OutError = TEXT("visibility contains 'self' — must be expanded to a concrete actor "
				"ID before AppendEvent (see schema.yaml viewer namespace)");
			return false;
		}
		if (!IsValidViewerString(V))
		{
			OutError = FString::Printf(TEXT("invalid viewer string: '%s' "
				"(must be one of: public/audience/orchestrator/system/NPC<NN>/Faction<X>)"), *V);
			return false;
		}
	}
	return true;
}

bool UAILiveEventStoreSubsystem::ValidatePayloadJson(const FString& InPayloadJson, FString& OutError)
{
	if (InPayloadJson.IsEmpty())
	{
		OutError = TEXT("payload is empty");
		return false;
	}
	TSharedPtr<FJsonObject> Obj;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(InPayloadJson);
	if (!FJsonSerializer::Deserialize(Reader, Obj) || !Obj.IsValid())
	{
		OutError = TEXT("payload is not valid JSON");
		return false;
	}
	if (!Obj->HasField(TEXT("text")))
	{
		OutError = TEXT("payload must contain 'text' field (FTS5 + UI dependency)");
		return false;
	}
	return true;
}

bool UAILiveEventStoreSubsystem::IsAddressedToSubsetOfVisibility(
	const TArray<FString>& InAddressedTo,
	const TArray<FString>& InVisibility,
	FString& OutError)
{
	if (InAddressedTo.Num() == 0)
	{
		return true;
	}
	if (InVisibility.Contains(TEXT("public")))
	{
		return true;
	}
	TSet<FString> VisSet;
	VisSet.Append(InVisibility);
	for (const FString& Target : InAddressedTo)
	{
		if (!VisSet.Contains(Target))
		{
			OutError = FString::Printf(
				TEXT("addressed_to target '%s' not present in visibility set"), *Target);
			return false;
		}
	}
	return true;
}

// ---------------------------------------------------------------
// T3 — Canonical JSON
//
// Field set (ASCII ascending): actor, addressed_to, event_id,
// event_type, game_id, parent_event_id, parser_version, payload,
// phase, raw_llm_output, round_no, seq, speech_act_type, visibility.
//
// Excluded: tick_no (protocol index), prev_event_hash (already in
// hash via Combined = prev || canonical), event_hash (self), wall_clock
// (DB DEFAULT, in-memory != row), payload_text (GENERATED column).
// ---------------------------------------------------------------

FString UAILiveEventStoreSubsystem::CanonicalJsonOf(const FAILiveEvent& InEvent)
{
	FString Out;
	const FCanonicalWriterRef W = FCanonicalWriterFactory::Create(&Out);
	W->WriteObjectStart();

	W->WriteValue(TEXT("actor"), InEvent.Actor);

	W->WriteArrayStart(TEXT("addressed_to"));
	for (const FString& T : InEvent.AddressedTo)
	{
		W->WriteValue(T);
	}
	W->WriteArrayEnd();

	W->WriteValue(TEXT("event_id"), InEvent.EventId);
	W->WriteValue(TEXT("event_type"), AILiveEvent::EventTypeToString(InEvent.EventType));
	W->WriteValue(TEXT("game_id"), InEvent.GameId);
	W->WriteValue(TEXT("parent_event_id"), InEvent.ParentEventId);
	W->WriteValue(TEXT("parser_version"), InEvent.ParserVersion);

	W->WriteIdentifierPrefix(TEXT("payload"));
	{
		TSharedPtr<FJsonObject> PayloadObj;
		const TSharedRef<TJsonReader<>> Rdr = TJsonReaderFactory<>::Create(InEvent.PayloadJson);
		if (FJsonSerializer::Deserialize(Rdr, PayloadObj) && PayloadObj.IsValid())
		{
			WriteCanonicalObject(PayloadObj, *W);
		}
		else
		{
			// Should not happen — ValidatePayloadJson guards. Fall back to null.
			W->WriteNull();
		}
	}

	W->WriteValue(TEXT("phase"), AILiveEvent::PhaseToString(InEvent.Phase));
	W->WriteValue(TEXT("raw_llm_output"), InEvent.RawLLMOutput);
	W->WriteValue(TEXT("round_no"), (int64)InEvent.RoundNo);
	W->WriteValue(TEXT("seq"), InEvent.Seq);
	W->WriteValue(TEXT("speech_act_type"), AILiveEvent::SpeechActToString(InEvent.SpeechActType));

	W->WriteArrayStart(TEXT("visibility"));
	for (const FString& V : InEvent.Visibility)
	{
		W->WriteValue(V);
	}
	W->WriteArrayEnd();

	W->WriteObjectEnd();
	W->Close();
	return Out;
}

FString UAILiveEventStoreSubsystem::ComputeEventHash(const FString& PrevHash, const FString& CanonicalPayload) const
{
	const FString Combined = PrevHash + CanonicalPayload;
	return AILiveUtil::Sha256Fingerprint(Combined);
}

// ---------------------------------------------------------------
// T3 — UUIDv7 (RFC 9562)
// ---------------------------------------------------------------

FString UAILiveEventStoreSubsystem::GenerateUuidV7()
{
	uint8 B[16];

	const FDateTime Now = FDateTime::UtcNow();
	const int64 UnixMs = Now.ToUnixTimestamp() * 1000LL +
		(int64)((Now.GetTicks() % ETimespan::TicksPerSecond) / ETimespan::TicksPerMillisecond);

	B[0] = (uint8)((UnixMs >> 40) & 0xFF);
	B[1] = (uint8)((UnixMs >> 32) & 0xFF);
	B[2] = (uint8)((UnixMs >> 24) & 0xFF);
	B[3] = (uint8)((UnixMs >> 16) & 0xFF);
	B[4] = (uint8)((UnixMs >> 8) & 0xFF);
	B[5] = (uint8)(UnixMs & 0xFF);

	for (int32 i = 6; i < 16; ++i)
	{
		B[i] = (uint8)(FMath::Rand() & 0xFF);
	}

	// Version 7 in high nibble of byte 6
	B[6] = (uint8)((B[6] & 0x0F) | 0x70);
	// Variant 10xx in high bits of byte 8
	B[8] = (uint8)((B[8] & 0x3F) | 0x80);

	return FString::Printf(
		TEXT("%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x"),
		B[0], B[1], B[2], B[3], B[4], B[5], B[6], B[7],
		B[8], B[9], B[10], B[11], B[12], B[13], B[14], B[15]);
}

// ---------------------------------------------------------------
// T3 — parser_version dynamic resolve (B-plan)
// Read _meta.db.schema_meta('parser_version') at write time so that
// manual UPDATEs propagate to subsequent events. Empty / unavailable
// → fall back to compile-time constant from the parser registry.
// ---------------------------------------------------------------

FString UAILiveEventStoreSubsystem::ResolveLiveParserVersion()
{
	TMap<FString, FString> KVs;
	if (QueryMetaSchemaRegistry(KVs))
	{
		const FString* Found = KVs.Find(TEXT("parser_version"));
		if (Found && !Found->IsEmpty())
		{
			return *Found;
		}
	}
	return AILiveParser::GetCurrentParserVersion();
}

// ---------------------------------------------------------------
// T3 — Lock-held insert: assumes WriteMutex held + BEGIN IMMEDIATE in flight.
// Updates by-ref local seq/hash trackers ONLY; never touches Cached* members.
// ---------------------------------------------------------------

int64 UAILiveEventStoreSubsystem::InsertEventBypassValidation_LockHeld(
	FAILiveEvent& InOutEvent,
	int64& InOutLocalLastSeq,
	FString& InOutLocalLastHash)
{
	InOutEvent.GameId = CurrentGameId;
	InOutEvent.Seq = InOutLocalLastSeq + 1;
	InOutEvent.EventId = GenerateUuidV7();
	InOutEvent.ParserVersion = ResolveLiveParserVersion();
	InOutEvent.PrevEventHash = InOutLocalLastHash;

	const FString CanonicalPayload = CanonicalJsonOf(InOutEvent);
	const FString NewHash = ComputeEventHash(InOutLocalLastHash, CanonicalPayload);
	InOutEvent.EventHash = NewHash;

	bool bOk = true;

	{
		FSQLitePreparedStatement Ins;
		if (!Ins.Create(Db, TEXT(
			"INSERT INTO events ("
			"  event_id, game_id, seq, round_no, phase, actor, event_type,"
			"  speech_act_type, visibility, addressed_to, payload, parent_event_id,"
			"  parser_version, raw_llm_output, prev_event_hash, event_hash, tick_no"
			") VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16,?17);")))
		{
			UE_LOG(LogAILiveMemory, Error,
				TEXT("InsertEventBypassValidation_LockHeld: prepare failed: %s"),
				*Db.GetLastError());
			return -1;
		}
		bOk = bOk && Ins.SetBindingValueByIndex(1,  InOutEvent.EventId);
		bOk = bOk && Ins.SetBindingValueByIndex(2,  InOutEvent.GameId);
		bOk = bOk && Ins.SetBindingValueByIndex(3,  InOutEvent.Seq);
		bOk = bOk && Ins.SetBindingValueByIndex(4,  (int64)InOutEvent.RoundNo);
		bOk = bOk && Ins.SetBindingValueByIndex(5,  AILiveEvent::PhaseToString(InOutEvent.Phase));
		bOk = bOk && Ins.SetBindingValueByIndex(6,  InOutEvent.Actor);
		bOk = bOk && Ins.SetBindingValueByIndex(7,  AILiveEvent::EventTypeToString(InOutEvent.EventType));
		bOk = bOk && Ins.SetBindingValueByIndex(8,  AILiveEvent::SpeechActToString(InOutEvent.SpeechActType));
		bOk = bOk && Ins.SetBindingValueByIndex(9,  AILiveEvent::ArrayToJsonString(InOutEvent.Visibility));
		bOk = bOk && Ins.SetBindingValueByIndex(10, AILiveEvent::ArrayToJsonString(InOutEvent.AddressedTo));
		bOk = bOk && Ins.SetBindingValueByIndex(11, InOutEvent.PayloadJson);
		bOk = bOk && Ins.SetBindingValueByIndex(12, InOutEvent.ParentEventId);
		bOk = bOk && Ins.SetBindingValueByIndex(13, InOutEvent.ParserVersion);
		bOk = bOk && Ins.SetBindingValueByIndex(14, InOutEvent.RawLLMOutput);
		bOk = bOk && Ins.SetBindingValueByIndex(15, InOutEvent.PrevEventHash);
		bOk = bOk && Ins.SetBindingValueByIndex(16, InOutEvent.EventHash);
		bOk = bOk && Ins.SetBindingValueByIndex(17, CachedCurrentTickNo);
		bOk = bOk && Ins.Execute();
		if (!bOk)
		{
			UE_LOG(LogAILiveMemory, Error,
				TEXT("InsertEventBypassValidation_LockHeld: events INSERT failed: %s"),
				*Db.GetLastError());
			return -1;
		}
	}

	{
		FSQLitePreparedStatement Vis;
		if (!Vis.Create(Db, TEXT(
			"INSERT INTO event_visibility(event_id, viewer) VALUES (?1, ?2);")))
		{
			UE_LOG(LogAILiveMemory, Error,
				TEXT("InsertEventBypassValidation_LockHeld: visibility prepare failed: %s"),
				*Db.GetLastError());
			return -1;
		}
		for (const FString& Viewer : InOutEvent.Visibility)
		{
			Vis.Reset();
			bOk = bOk && Vis.SetBindingValueByIndex(1, InOutEvent.EventId);
			bOk = bOk && Vis.SetBindingValueByIndex(2, Viewer);
			bOk = bOk && Vis.Execute();
			if (!bOk)
			{
				UE_LOG(LogAILiveMemory, Error,
					TEXT("InsertEventBypassValidation_LockHeld: event_visibility INSERT failed: %s"),
					*Db.GetLastError());
				return -1;
			}
		}
	}

	if (InOutEvent.AddressedTo.Num() > 0)
	{
		FSQLitePreparedStatement A;
		if (!A.Create(Db, TEXT(
			"INSERT INTO event_addressed_to(event_id, target) VALUES (?1, ?2);")))
		{
			UE_LOG(LogAILiveMemory, Error,
				TEXT("InsertEventBypassValidation_LockHeld: addressed_to prepare failed: %s"),
				*Db.GetLastError());
			return -1;
		}
		for (const FString& Target : InOutEvent.AddressedTo)
		{
			A.Reset();
			bOk = bOk && A.SetBindingValueByIndex(1, InOutEvent.EventId);
			bOk = bOk && A.SetBindingValueByIndex(2, Target);
			bOk = bOk && A.Execute();
			if (!bOk)
			{
				UE_LOG(LogAILiveMemory, Error,
					TEXT("InsertEventBypassValidation_LockHeld: event_addressed_to INSERT failed: %s"),
					*Db.GetLastError());
				return -1;
			}
		}
	}

	InOutLocalLastSeq = InOutEvent.Seq;
	InOutLocalLastHash = NewHash;
	return InOutEvent.Seq;
}

// ---------------------------------------------------------------
// T3 — Public bypass: takes lock + BEGIN/COMMIT itself.
// On failure to commit the only safe action is UE_LOG(Fatal): the
// truth-log fallback path is past its responsibility boundary.
// ---------------------------------------------------------------

int64 UAILiveEventStoreSubsystem::InsertEventBypassValidation(FAILiveEvent& InOutEvent)
{
	if (!IsGameOpen() || !Db.IsValid())
	{
		UE_LOG(LogAILiveMemory, Error, TEXT("InsertEventBypassValidation: no game open"));
		return -1;
	}

	FScopeLock Lock(&WriteMutex);

	if (!Db.Execute(TEXT("BEGIN IMMEDIATE;")))
	{
		UE_LOG(LogAILiveMemory, Fatal,
			TEXT("InsertEventBypassValidation BEGIN IMMEDIATE failed: %s — "
				 "EventStore is past its responsibility boundary."),
			*Db.GetLastError());
		return -1;
	}

	int64 LocalLastSeq = CachedLastSeq;
	FString LocalLastHash = CachedLastHash;
	const int64 NewSeq = InsertEventBypassValidation_LockHeld(InOutEvent, LocalLastSeq, LocalLastHash);
	if (NewSeq < 0)
	{
		Db.Execute(TEXT("ROLLBACK;"));
		UE_LOG(LogAILiveMemory, Fatal,
			TEXT("InsertEventBypassValidation INSERT failed (game=%s, actor=%s) — "
				 "EventStore is past its responsibility boundary."),
			*CurrentGameId, *InOutEvent.Actor);
		return -1;
	}
	if (!Db.Execute(TEXT("COMMIT;")))
	{
		Db.Execute(TEXT("ROLLBACK;"));
		UE_LOG(LogAILiveMemory, Fatal,
			TEXT("InsertEventBypassValidation COMMIT failed: %s — "
				 "EventStore is past its responsibility boundary."),
			*Db.GetLastError());
		return -1;
	}

	CachedLastSeq = LocalLastSeq;
	CachedLastHash = LocalLastHash;
	return NewSeq;
}

// ---------------------------------------------------------------
// T3 — Truth-log fallback for static-validation rejection.
// Internally builds a guaranteed-legal payload (visibility=["system"],
// payload contains "text"), so the bypass insert won't recurse into
// AppendSystemParseFailure even on schema conformance.
// ---------------------------------------------------------------

int64 UAILiveEventStoreSubsystem::AppendSystemParseFailure(
	const FString& InOriginalActor,
	const FString& InOriginalEventTypeStr,
	const FString& InErrorReason,
	const FString& InOriginalPayloadSnippet)
{
	const FString Snippet = InOriginalPayloadSnippet.Left(256);

	// Build payload through TJsonWriter so every field — including the human-readable
	// `text` summary — gets correctly escaped. A literal `"` or newline in the actor
	// name used to break the JSON, which then failed `events.payload_text`'s
	// `json_extract(payload,'$.text')` GENERATED column on INSERT and tripped
	// InsertEventBypassValidation's UE_LOG(Fatal).
	FString PayloadJson;
	{
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&PayloadJson);
		Writer->WriteObjectStart();
		Writer->WriteValue(TEXT("text"), FString::Printf(
			TEXT("parse failed for actor=%s event_type=%s"),
			*InOriginalActor, *InOriginalEventTypeStr));
		Writer->WriteValue(TEXT("original_actor"), InOriginalActor);
		Writer->WriteValue(TEXT("original_event_type"), InOriginalEventTypeStr);
		Writer->WriteValue(TEXT("reason"), InErrorReason);
		Writer->WriteValue(TEXT("original_payload_snippet"), Snippet);
		Writer->WriteObjectEnd();
		Writer->Close();
	}

	FAILiveEvent Sys;
	Sys.Actor = TEXT("system");
	Sys.EventType = EAILiveEventType::SystemParseFailed;
	Sys.SpeechActType = EAILiveSpeechActType::None;
	Sys.Phase = EAILivePhase::Setup;
	Sys.RoundNo = 0;
	Sys.Visibility = { TEXT("system") };
	Sys.PayloadJson = PayloadJson;

	return InsertEventBypassValidation(Sys);
}

// ---------------------------------------------------------------
// T3 — AppendEvent / AppendEventsAtomically
// ---------------------------------------------------------------

int64 UAILiveEventStoreSubsystem::AppendEvent(FAILiveEvent& InOutEvent)
{
	TArray<FAILiveEvent> Group;
	Group.Add(MoveTemp(InOutEvent));
	const int64 FirstSeq = AppendEventsAtomically(Group);
	InOutEvent = MoveTemp(Group[0]);
	return FirstSeq;
}

int64 UAILiveEventStoreSubsystem::AppendEventsAtomically(TArray<FAILiveEvent>& InOutEvents)
{
	if (!IsGameOpen() || !Db.IsValid())
	{
		UE_LOG(LogAILiveMemory, Error, TEXT("AppendEventsAtomically: no game open"));
		return -1;
	}
	if (InOutEvents.Num() == 0)
	{
		UE_LOG(LogAILiveMemory, Error, TEXT("AppendEventsAtomically: empty group"));
		return -1;
	}

	// === Stage 1: static validation (LOCK-FREE on purpose).
	// On rejection write a system.parse_failed via the public bypass — which
	// takes the lock itself. Doing this *outside* WriteMutex avoids a nested
	// BEGIN IMMEDIATE within an already-open transaction.
	for (int32 i = 0; i < InOutEvents.Num(); ++i)
	{
		FString Err;
		if (!ValidateVisibility(InOutEvents[i].Visibility, Err))
		{
			UE_LOG(LogAILiveMemory, Error,
				TEXT("AppendEventsAtomically rejected event[%d] (actor=%s): %s"),
				i, *InOutEvents[i].Actor, *Err);
			AppendSystemParseFailure(
				InOutEvents[i].Actor,
				AILiveEvent::EventTypeToString(InOutEvents[i].EventType),
				Err,
				InOutEvents[i].PayloadJson);
			return -1;
		}
		if (!ValidatePayloadJson(InOutEvents[i].PayloadJson, Err))
		{
			UE_LOG(LogAILiveMemory, Error,
				TEXT("AppendEventsAtomically rejected event[%d] (actor=%s): %s"),
				i, *InOutEvents[i].Actor, *Err);
			AppendSystemParseFailure(
				InOutEvents[i].Actor,
				AILiveEvent::EventTypeToString(InOutEvents[i].EventType),
				Err,
				InOutEvents[i].PayloadJson);
			return -1;
		}
		// Soft check (schema.yaml): addressed_to should be a subset of expanded
		// visibility. Failure logs Warning + writes an audit parse_failed row,
		// but the original event is still written — the protocol allows
		// "暗中点名" use cases where a target is whispered but not visible.
		FString AddrErr;
		if (!IsAddressedToSubsetOfVisibility(
				InOutEvents[i].AddressedTo, InOutEvents[i].Visibility, AddrErr))
		{
			UE_LOG(LogAILiveMemory, Warning,
				TEXT("AppendEventsAtomically event[%d] (actor=%s): %s "
					 "(proceeding with write per schema policy)"),
				i, *InOutEvents[i].Actor, *AddrErr);
			AppendSystemParseFailure(
				InOutEvents[i].Actor,
				AILiveEvent::EventTypeToString(InOutEvents[i].EventType),
				AddrErr,
				InOutEvents[i].PayloadJson);
			// No early return — the event still gets written.
		}
	}

	// === Stage 2: lock + transaction.
	FScopeLock Lock(&WriteMutex);

	if (!Db.Execute(TEXT("BEGIN IMMEDIATE;")))
	{
		UE_LOG(LogAILiveMemory, Error,
			TEXT("AppendEventsAtomically BEGIN IMMEDIATE failed (game=%s): %s"),
			*CurrentGameId, *Db.GetLastError());
		return -1;
	}

	// Local cache copy — global Cached* only updated after successful COMMIT.
	int64 LocalLastSeq = CachedLastSeq;
	FString LocalLastHash = CachedLastHash;
	int64 FirstAssignedSeq = -1;
	bool bOk = true;

	for (int32 i = 0; i < InOutEvents.Num() && bOk; ++i)
	{
		const int64 NewSeq = InsertEventBypassValidation_LockHeld(
			InOutEvents[i], LocalLastSeq, LocalLastHash);
		if (NewSeq < 0)
		{
			bOk = false;
			break;
		}
		if (FirstAssignedSeq == -1)
		{
			FirstAssignedSeq = NewSeq;
		}
	}

	if (bOk && !Db.Execute(TEXT("COMMIT;")))
	{
		UE_LOG(LogAILiveMemory, Error,
			TEXT("AppendEventsAtomically COMMIT failed (game=%s): %s"),
			*CurrentGameId, *Db.GetLastError());
		bOk = false;
	}

	if (!bOk)
	{
		Db.Execute(TEXT("ROLLBACK;"));
		UE_LOG(LogAILiveMemory, Error,
			TEXT("AppendEventsAtomically failed (game=%s, group_size=%d) — rolled back"),
			*CurrentGameId, InOutEvents.Num());
		// Reset assigned identity fields so caller cannot mistakenly trust them.
		for (FAILiveEvent& Ev : InOutEvents)
		{
			Ev.Seq = 0;
			Ev.EventId.Reset();
			Ev.PrevEventHash.Reset();
			Ev.EventHash.Reset();
		}
		return -1;
	}

	// Commit successful — promote local cache to global.
	CachedLastSeq = LocalLastSeq;
	CachedLastHash = LocalLastHash;
	return FirstAssignedSeq;
}

// ---------------------------------------------------------------
// T3 — BeginTick: open a new tick. Saves OldTick so a failure path
// restores the cache (otherwise a failed BeginTick(N) would leak N
// to the next AppendEvent).
// ---------------------------------------------------------------

int64 UAILiveEventStoreSubsystem::BeginTick(int32 InTickNo)
{
	if (!IsGameOpen() || !Db.IsValid())
	{
		UE_LOG(LogAILiveMemory, Error, TEXT("BeginTick: no game open"));
		return -1;
	}

	FScopeLock Lock(&WriteMutex);

	const int64 OldTick = CachedCurrentTickNo;

	if (!Db.Execute(TEXT("BEGIN IMMEDIATE;")))
	{
		UE_LOG(LogAILiveMemory, Error,
			TEXT("BeginTick BEGIN IMMEDIATE failed: %s"), *Db.GetLastError());
		return -1;
	}

	CachedCurrentTickNo = (int64)InTickNo;

	FAILiveEvent Anchor;
	Anchor.Actor = TEXT("orchestrator");
	Anchor.EventType = EAILiveEventType::OrchestratorTickAnchor;
	Anchor.SpeechActType = EAILiveSpeechActType::None;
	Anchor.Phase = EAILivePhase::Setup;
	Anchor.RoundNo = 0;
	Anchor.Visibility = { TEXT("public") };
	Anchor.PayloadJson = FString::Printf(
		TEXT("{\"text\":\"tick anchor N=%d\",\"tick_no\":%d}"),
		InTickNo, InTickNo);

	int64 LocalLastSeq = CachedLastSeq;
	FString LocalLastHash = CachedLastHash;
	const int64 AnchorSeq = InsertEventBypassValidation_LockHeld(
		Anchor, LocalLastSeq, LocalLastHash);

	bool bOk = (AnchorSeq >= 0);
	if (bOk && !Db.Execute(TEXT("COMMIT;")))
	{
		UE_LOG(LogAILiveMemory, Error,
			TEXT("BeginTick COMMIT failed: %s"), *Db.GetLastError());
		bOk = false;
	}

	if (!bOk)
	{
		Db.Execute(TEXT("ROLLBACK;"));
		CachedCurrentTickNo = OldTick;
		return -1;
	}

	CachedLastSeq = LocalLastSeq;
	CachedLastHash = LocalLastHash;
	return AnchorSeq;
}

// ---------------------------------------------------------------
// T3 — Debug helpers (used by console commands).
// ---------------------------------------------------------------

bool UAILiveEventStoreSubsystem::UpsertMetaSchemaKV(const FString& Key, const FString& Value)
{
	if (!MetaDb.IsValid())
	{
		UE_LOG(LogAILiveMemory, Error, TEXT("UpsertMetaSchemaKV: MetaDb not open"));
		return false;
	}
	FSQLitePreparedStatement Stmt;
	if (!Stmt.Create(MetaDb, TEXT("INSERT OR REPLACE INTO schema_meta(key, value) VALUES(?1, ?2);")) ||
		!Stmt.SetBindingValueByIndex(1, Key) ||
		!Stmt.SetBindingValueByIndex(2, Value) ||
		!Stmt.Execute())
	{
		UE_LOG(LogAILiveMemory, Error,
			TEXT("UpsertMetaSchemaKV('%s'): %s"), *Key, *MetaDb.GetLastError());
		return false;
	}
	return true;
}

bool UAILiveEventStoreSubsystem::RecomputeHashChainOnMainConnection(
	int64& OutLastSeq, int64& OutBadCount, int64& OutFirstBadSeq)
{
	OutLastSeq = 0;
	OutBadCount = 0;
	OutFirstBadSeq = -1;
	if (!Db.IsValid())
	{
		return false;
	}
	const TCHAR* Sql = TEXT(
		"SELECT seq, event_id, game_id, round_no, phase, actor, event_type,"
		"       speech_act_type, visibility, addressed_to, payload, parent_event_id,"
		"       parser_version, raw_llm_output, prev_event_hash, event_hash"
		" FROM events ORDER BY seq ASC;");

	FString PrevHash = FString(kGenesisHash);
	int64 LastSeq = 0;
	int64 BadCount = 0;
	int64 FirstBadSeq = -1;

	const int64 Rows = Db.Execute(Sql,
		[this, &PrevHash, &LastSeq, &BadCount, &FirstBadSeq]
		(const FSQLitePreparedStatement& Stmt)
		{
			FAILiveEvent Ev;
			int64 SeqVal = 0;
			int64 RoundNo64 = 0;
			FString PhaseStr, EventTypeStr, SpeechActStr, VisJson, AddrJson;
			FString PrevHashRow, EventHashRow;
			Stmt.GetColumnValueByIndex(0,  SeqVal);
			Stmt.GetColumnValueByIndex(1,  Ev.EventId);
			Stmt.GetColumnValueByIndex(2,  Ev.GameId);
			Stmt.GetColumnValueByIndex(3,  RoundNo64);
			Stmt.GetColumnValueByIndex(4,  PhaseStr);
			Stmt.GetColumnValueByIndex(5,  Ev.Actor);
			Stmt.GetColumnValueByIndex(6,  EventTypeStr);
			Stmt.GetColumnValueByIndex(7,  SpeechActStr);
			Stmt.GetColumnValueByIndex(8,  VisJson);
			Stmt.GetColumnValueByIndex(9,  AddrJson);
			Stmt.GetColumnValueByIndex(10, Ev.PayloadJson);
			Stmt.GetColumnValueByIndex(11, Ev.ParentEventId);
			Stmt.GetColumnValueByIndex(12, Ev.ParserVersion);
			Stmt.GetColumnValueByIndex(13, Ev.RawLLMOutput);
			Stmt.GetColumnValueByIndex(14, PrevHashRow);
			Stmt.GetColumnValueByIndex(15, EventHashRow);

			Ev.Seq = SeqVal;
			Ev.RoundNo = (int32)RoundNo64;
			Ev.Phase = AILiveEvent::PhaseFromString(PhaseStr);
			Ev.EventType = AILiveEvent::EventTypeFromString(EventTypeStr);
			Ev.SpeechActType = AILiveEvent::SpeechActFromString(SpeechActStr);
			Ev.Visibility = AILiveEvent::JsonStringToArray(VisJson);
			Ev.AddressedTo = AILiveEvent::JsonStringToArray(AddrJson);
			Ev.PrevEventHash = PrevHashRow;
			Ev.EventHash = EventHashRow;

			if (PrevHashRow != PrevHash)
			{
				if (FirstBadSeq < 0) FirstBadSeq = SeqVal;
				++BadCount;
			}
			else
			{
				const FString CanonRecompute = UAILiveEventStoreSubsystem::CanonicalJsonOf(Ev);
				const FString ExpectHash = ComputeEventHash(PrevHash, CanonRecompute);
				if (ExpectHash != EventHashRow)
				{
					if (FirstBadSeq < 0) FirstBadSeq = SeqVal;
					++BadCount;
				}
			}
			PrevHash = EventHashRow;
			LastSeq = SeqVal;
			return ESQLitePreparedStatementExecuteRowResult::Continue;
		});
	if (Rows == INDEX_NONE)
	{
		return false;
	}
	OutLastSeq = LastSeq;
	OutBadCount = BadCount;
	OutFirstBadSeq = FirstBadSeq;
	return true;
}

bool UAILiveEventStoreSubsystem::ExecuteDebugSqlOnMainConnection(const FString& Sql, FString& OutError)
{
	if (!Db.IsValid())
	{
		OutError = TEXT("Db not open");
		return false;
	}
	const bool bOk = Db.Execute(*Sql);
	if (!bOk)
	{
		OutError = Db.GetLastError();
	}
	return bOk;
}

bool UAILiveEventStoreSubsystem::SetGameDbQueryOnly(bool bQueryOnly)
{
	if (!Db.IsValid())
	{
		UE_LOG(LogAILiveMemory, Error, TEXT("SetGameDbQueryOnly: Db not open"));
		return false;
	}
	const TCHAR* Sql = bQueryOnly
		? TEXT("PRAGMA query_only = 1;")
		: TEXT("PRAGMA query_only = 0;");
	if (!Db.Execute(Sql))
	{
		UE_LOG(LogAILiveMemory, Error,
			TEXT("SetGameDbQueryOnly(%d) failed: %s"),
			bQueryOnly ? 1 : 0, *Db.GetLastError());
		return false;
	}
	UE_LOG(LogAILiveMemory, Display,
		TEXT("Db query_only=%d — subsequent INSERTs will %s"),
		bQueryOnly ? 1 : 0,
		bQueryOnly ? TEXT("fail with SQLITE_READONLY") : TEXT("be permitted"));
	return true;
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

// ---------------------------------------------------------------
// T3 — debug/test console commands.
// ---------------------------------------------------------------

namespace
{

FAILiveEvent MakeSyntheticEvent(int32 InIndex)
{
	FAILiveEvent Ev;
	Ev.Actor = TEXT("orchestrator");
	Ev.EventType = EAILiveEventType::SystemRoleAssigned;
	Ev.Phase = EAILivePhase::Setup;
	Ev.RoundNo = 0;

	const int32 Mode = InIndex % 3;
	if (Mode == 0)
	{
		Ev.Visibility = { TEXT("public") };
	}
	else if (Mode == 1)
	{
		Ev.Visibility = { TEXT("NPC01"), TEXT("NPC02") };
	}
	else
	{
		Ev.Visibility = { TEXT("orchestrator") };
	}

	Ev.PayloadJson = FString::Printf(
		TEXT("{\"text\":\"synthetic event #%d\",\"index\":%d}"),
		InIndex, InIndex);
	return Ev;
}

} // namespace

static FAutoConsoleCommand GAILiveTestAppendOne(
	TEXT("AILive.Test.AppendOne"),
	TEXT("AILive.Test.AppendOne <text> — append a single public speech with payload.text=<text>"),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
	{
		if (Args.Num() < 1)
		{
			UE_LOG(LogAILiveMemory, Error, TEXT("Usage: AILive.Test.AppendOne <text>"));
			return;
		}
		UAILiveEventStoreSubsystem* Sys = GetSubsystemForConsole();
		if (!Sys || !Sys->IsGameOpen())
		{
			UE_LOG(LogAILiveMemory, Error, TEXT("AppendOne: no game open"));
			return;
		}
		const FString Text = FString::Join(Args, TEXT(" "));
		FAILiveEvent Ev;
		Ev.Actor = TEXT("orchestrator");
		Ev.EventType = EAILiveEventType::SystemRoleAssigned;
		Ev.Phase = EAILivePhase::Setup;
		Ev.RoundNo = 0;
		Ev.Visibility = { TEXT("public") };
		Ev.PayloadJson = FString::Printf(TEXT("{\"text\":%s}"),
			*AILiveUtil::EscapeJsonString(Text));
		const int64 Seq = Sys->AppendEvent(Ev);
		UE_LOG(LogAILiveMemory, Display,
			TEXT("AppendOne -> seq=%lld event_id=%s parser_version=%s"),
			Seq, *Ev.EventId, *Ev.ParserVersion);
	}));

static FAutoConsoleCommand GAILiveTestAppend100(
	TEXT("AILive.Test.Append100"),
	TEXT("AILive.Test.Append100 — append 100 synthetic events; logs total_visibility_entries"),
	FConsoleCommandDelegate::CreateLambda([]()
	{
		UAILiveEventStoreSubsystem* Sys = GetSubsystemForConsole();
		if (!Sys || !Sys->IsGameOpen())
		{
			UE_LOG(LogAILiveMemory, Error, TEXT("Append100: no game open"));
			return;
		}
		int64 TotalVisibility = 0;
		int32 Failures = 0;
		const double T0 = FPlatformTime::Seconds();
		for (int32 i = 0; i < 100; ++i)
		{
			FAILiveEvent Ev = MakeSyntheticEvent(i);
			TotalVisibility += Ev.Visibility.Num();
			const int64 Seq = Sys->AppendEvent(Ev);
			if (Seq < 0) ++Failures;
		}
		const double Dt = FPlatformTime::Seconds() - T0;
		UE_LOG(LogAILiveMemory, Display,
			TEXT("Append100 done: failures=%d, total_visibility_entries=%lld, elapsed_ms=%.1f"),
			Failures, TotalVisibility, Dt * 1000.0);
	}));

static FAutoConsoleCommand GAILiveTestAppendBadVisSelf(
	TEXT("AILive.Test.AppendBadVisSelf"),
	TEXT("AILive.Test.AppendBadVisSelf — try to append with visibility containing 'self' (should be rejected)"),
	FConsoleCommandDelegate::CreateLambda([]()
	{
		UAILiveEventStoreSubsystem* Sys = GetSubsystemForConsole();
		if (!Sys || !Sys->IsGameOpen())
		{
			UE_LOG(LogAILiveMemory, Error, TEXT("AppendBadVisSelf: no game open"));
			return;
		}
		FAILiveEvent Ev;
		Ev.Actor = TEXT("NPC03");
		Ev.EventType = EAILiveEventType::SpeechIntended;
		Ev.Phase = EAILivePhase::DayDiscuss;
		Ev.Visibility = { TEXT("self"), TEXT("NPC03") };
		Ev.PayloadJson = TEXT("{\"text\":\"intended speech\"}");
		const int64 Seq = Sys->AppendEvent(Ev);
		UE_LOG(LogAILiveMemory, Display,
			TEXT("AppendBadVisSelf -> seq=%lld (expect -1)"), Seq);
	}));

static FAutoConsoleCommand GAILiveTestAppendBadVisFreeText(
	TEXT("AILive.Test.AppendBadVisFreeText"),
	TEXT("AILive.Test.AppendBadVisFreeText — try to append with arbitrary viewer string (should be rejected)"),
	FConsoleCommandDelegate::CreateLambda([]()
	{
		UAILiveEventStoreSubsystem* Sys = GetSubsystemForConsole();
		if (!Sys || !Sys->IsGameOpen())
		{
			UE_LOG(LogAILiveMemory, Error, TEXT("AppendBadVisFreeText: no game open"));
			return;
		}
		FAILiveEvent Ev;
		Ev.Actor = TEXT("NPC03");
		Ev.EventType = EAILiveEventType::SpeechPublic;
		Ev.Phase = EAILivePhase::DayDiscuss;
		Ev.Visibility = { TEXT("random_string") };
		Ev.PayloadJson = TEXT("{\"text\":\"hi\"}");
		const int64 Seq = Sys->AppendEvent(Ev);
		UE_LOG(LogAILiveMemory, Display,
			TEXT("AppendBadVisFreeText -> seq=%lld (expect -1)"), Seq);
	}));

static FAutoConsoleCommand GAILiveTestAppendBadPayloadNoText(
	TEXT("AILive.Test.AppendBadPayloadNoText"),
	TEXT("AILive.Test.AppendBadPayloadNoText — try to append with payload missing 'text' (should be rejected)"),
	FConsoleCommandDelegate::CreateLambda([]()
	{
		UAILiveEventStoreSubsystem* Sys = GetSubsystemForConsole();
		if (!Sys || !Sys->IsGameOpen())
		{
			UE_LOG(LogAILiveMemory, Error, TEXT("AppendBadPayloadNoText: no game open"));
			return;
		}
		FAILiveEvent Ev;
		Ev.Actor = TEXT("orchestrator");
		Ev.EventType = EAILiveEventType::SystemRoleAssigned;
		Ev.Phase = EAILivePhase::Setup;
		Ev.Visibility = { TEXT("public") };
		Ev.PayloadJson = TEXT("{\"foo\":\"bar\"}");
		const int64 Seq = Sys->AppendEvent(Ev);
		UE_LOG(LogAILiveMemory, Display,
			TEXT("AppendBadPayloadNoText -> seq=%lld (expect -1)"), Seq);
	}));

static FAutoConsoleCommand GAILiveTestAppendAddressedToOutsideVis(
	TEXT("AILive.Test.AppendAddressedToOutsideVis"),
	TEXT("AILive.Test.AppendAddressedToOutsideVis — append with addressed_to=[NPC07], visibility=[NPC03]; expect Warning + parse_failed but event STILL gets written"),
	FConsoleCommandDelegate::CreateLambda([]()
	{
		UAILiveEventStoreSubsystem* Sys = GetSubsystemForConsole();
		if (!Sys || !Sys->IsGameOpen())
		{
			UE_LOG(LogAILiveMemory, Error, TEXT("AppendAddressedToOutsideVis: no game open"));
			return;
		}
		FAILiveEvent Ev;
		Ev.Actor = TEXT("NPC03");
		Ev.EventType = EAILiveEventType::SpeechIntended;
		Ev.Phase = EAILivePhase::DayDiscuss;
		Ev.Visibility = { TEXT("NPC03") };
		Ev.AddressedTo = { TEXT("NPC07") };  // not in visibility — should warn but not reject
		Ev.PayloadJson = TEXT("{\"text\":\"whispered to NPC07 but only NPC03 sees this\"}");
		const int64 Seq = Sys->AppendEvent(Ev);
		UE_LOG(LogAILiveMemory, Display,
			TEXT("AppendAddressedToOutsideVis -> seq=%lld (expect >0; one parse_failed audit row should also exist)"),
			Seq);
	}));

static FAutoConsoleCommand GAILiveTestBeginTickCmd(
	TEXT("AILive.Test.BeginTick"),
	TEXT("AILive.Test.BeginTick <N> — write a tick_anchor event and set CachedCurrentTickNo=N"),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
	{
		if (Args.Num() < 1)
		{
			UE_LOG(LogAILiveMemory, Error, TEXT("Usage: AILive.Test.BeginTick <N>"));
			return;
		}
		UAILiveEventStoreSubsystem* Sys = GetSubsystemForConsole();
		if (!Sys || !Sys->IsGameOpen())
		{
			UE_LOG(LogAILiveMemory, Error, TEXT("BeginTick: no game open"));
			return;
		}
		const int32 N = FCString::Atoi(*Args[0]);
		const int64 AnchorSeq = Sys->BeginTick(N);
		UE_LOG(LogAILiveMemory, Display,
			TEXT("BeginTick(%d) -> anchor_seq=%lld, GetCurrentTickNo=%lld"),
			N, AnchorSeq, Sys->GetCurrentTickNo());
	}));

static FAutoConsoleCommand GAILiveTestConcurrentAppend(
	TEXT("AILive.Test.ConcurrentAppend"),
	TEXT("AILive.Test.ConcurrentAppend <Threads> <PerThread> — fan out to N threads, each calls AppendEventsAtomically(<PerThread> events)"),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
	{
		if (Args.Num() < 2)
		{
			UE_LOG(LogAILiveMemory, Error, TEXT("Usage: AILive.Test.ConcurrentAppend <Threads> <PerThread>"));
			return;
		}
		UAILiveEventStoreSubsystem* Sys = GetSubsystemForConsole();
		if (!Sys || !Sys->IsGameOpen())
		{
			UE_LOG(LogAILiveMemory, Error, TEXT("ConcurrentAppend: no game open"));
			return;
		}
		const int32 NThreads = FMath::Clamp(FCString::Atoi(*Args[0]), 1, 64);
		const int32 NPerThread = FMath::Clamp(FCString::Atoi(*Args[1]), 1, 1024);

		FThreadSafeCounter SuccessGroups;
		FThreadSafeCounter FailedGroups;
		TArray<TFuture<void>> Futures;
		Futures.Reserve(NThreads);
		const double T0 = FPlatformTime::Seconds();
		for (int32 t = 0; t < NThreads; ++t)
		{
			Futures.Emplace(Async(EAsyncExecution::Thread,
				[Sys, t, NPerThread, &SuccessGroups, &FailedGroups]()
				{
					TArray<FAILiveEvent> Group;
					Group.Reserve(NPerThread);
					for (int32 i = 0; i < NPerThread; ++i)
					{
						FAILiveEvent Ev = MakeSyntheticEvent(t * 1000 + i);
						Ev.Actor = TEXT("orchestrator");
						Ev.PayloadJson = FString::Printf(
							TEXT("{\"text\":\"thread %d event %d\",\"thread\":%d,\"i\":%d}"),
							t, i, t, i);
						Group.Add(MoveTemp(Ev));
					}
					const int64 First = Sys->AppendEventsAtomically(Group);
					if (First < 0)
					{
						FailedGroups.Increment();
					}
					else
					{
						SuccessGroups.Increment();
					}
				}));
		}
		for (TFuture<void>& F : Futures)
		{
			F.Wait();
		}
		const double Dt = FPlatformTime::Seconds() - T0;
		UE_LOG(LogAILiveMemory, Display,
			TEXT("ConcurrentAppend(%d x %d) done: success=%d, failed=%d, elapsed_ms=%.1f"),
			NThreads, NPerThread,
			SuccessGroups.GetValue(), FailedGroups.GetValue(), Dt * 1000.0);
	}));

static FAutoConsoleCommand GAILiveTestRecomputeChain(
	TEXT("AILive.Test.RecomputeAndVerifyChain"),
	TEXT("AILive.Test.RecomputeAndVerifyChain — walk events on main connection and recompute hash chain"),
	FConsoleCommandDelegate::CreateLambda([]()
	{
		UAILiveEventStoreSubsystem* Sys = GetSubsystemForConsole();
		if (!Sys || !Sys->IsGameOpen())
		{
			UE_LOG(LogAILiveMemory, Error, TEXT("RecomputeAndVerifyChain: no game open"));
			return;
		}
		int64 LastSeq = 0, BadCount = 0, FirstBadSeq = -1;
		if (!Sys->RecomputeHashChainOnMainConnection(LastSeq, BadCount, FirstBadSeq))
		{
			UE_LOG(LogAILiveMemory, Error,
				TEXT("RecomputeAndVerifyChain: SELECT failed"));
			return;
		}
		if (BadCount == 0)
		{
			UE_LOG(LogAILiveMemory, Display,
				TEXT("chain ok, last_seq=%lld"), LastSeq);
		}
		else
		{
			UE_LOG(LogAILiveMemory, Error,
				TEXT("chain BAD, bad_count=%lld, first_bad_seq=%lld, last_seq=%lld"),
				BadCount, FirstBadSeq, LastSeq);
		}
	}));

static FAutoConsoleCommand GAILiveTestTryUpdate(
	TEXT("AILive.Test.TryUpdate"),
	TEXT("AILive.Test.TryUpdate <seq> — issue UPDATE events SET payload='x' WHERE seq=<seq>; expect events_no_update trigger to ABORT"),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
	{
		if (Args.Num() < 1)
		{
			UE_LOG(LogAILiveMemory, Error, TEXT("Usage: AILive.Test.TryUpdate <seq>"));
			return;
		}
		UAILiveEventStoreSubsystem* Sys = GetSubsystemForConsole();
		if (!Sys || !Sys->IsGameOpen())
		{
			UE_LOG(LogAILiveMemory, Error, TEXT("TryUpdate: no game open"));
			return;
		}
		// Use a non-payload column so we don't trigger the GENERATED column's
		// json_extract validation before the BEFORE UPDATE trigger fires.
		const FString Sql = FString::Printf(
			TEXT("UPDATE events SET round_no=99 WHERE seq=%s;"), *Args[0]);
		FString Err;
		const bool bOk = Sys->ExecuteDebugSqlOnMainConnection(Sql, Err);
		UE_LOG(LogAILiveMemory, Display,
			TEXT("TryUpdate seq=%s -> %s (err='%s'; expect failure containing 'append-only')"),
			*Args[0], bOk ? TEXT("UNEXPECTED SUCCESS") : TEXT("rejected"), *Err);
	}));

static FAutoConsoleCommand GAILiveTestTryDelete(
	TEXT("AILive.Test.TryDelete"),
	TEXT("AILive.Test.TryDelete <seq> — issue DELETE FROM events WHERE seq=<seq>; expect events_no_delete trigger to ABORT"),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
	{
		if (Args.Num() < 1)
		{
			UE_LOG(LogAILiveMemory, Error, TEXT("Usage: AILive.Test.TryDelete <seq>"));
			return;
		}
		UAILiveEventStoreSubsystem* Sys = GetSubsystemForConsole();
		if (!Sys || !Sys->IsGameOpen())
		{
			UE_LOG(LogAILiveMemory, Error, TEXT("TryDelete: no game open"));
			return;
		}
		const FString Sql = FString::Printf(
			TEXT("DELETE FROM events WHERE seq=%s;"), *Args[0]);
		FString Err;
		const bool bOk = Sys->ExecuteDebugSqlOnMainConnection(Sql, Err);
		UE_LOG(LogAILiveMemory, Display,
			TEXT("TryDelete seq=%s -> %s (err='%s'; expect failure containing 'append-only')"),
			*Args[0], bOk ? TEXT("UNEXPECTED SUCCESS") : TEXT("rejected"), *Err);
	}));

static FAutoConsoleCommand GAILiveTestSetMetaSchemaKV(
	TEXT("AILive.Test.SetMetaSchemaKV"),
	TEXT("AILive.Test.SetMetaSchemaKV <key> <value> — INSERT OR REPLACE into _meta.db.schema_meta"),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
	{
		if (Args.Num() < 2)
		{
			UE_LOG(LogAILiveMemory, Error,
				TEXT("Usage: AILive.Test.SetMetaSchemaKV <key> <value>"));
			return;
		}
		UAILiveEventStoreSubsystem* Sys = GetSubsystemForConsole();
		if (!Sys || !Sys->IsGameOpen())
		{
			UE_LOG(LogAILiveMemory, Error, TEXT("SetMetaSchemaKV: no game open"));
			return;
		}
		const bool bOk = Sys->UpsertMetaSchemaKV(Args[0], Args[1]);
		UE_LOG(LogAILiveMemory, Display,
			TEXT("SetMetaSchemaKV('%s'='%s') -> %s"),
			*Args[0], *Args[1], bOk ? TEXT("OK") : TEXT("FAIL"));
	}));

static FAutoConsoleCommand GAILiveTestSha256(
	TEXT("AILive.Test.Sha256"),
	TEXT("AILive.Test.Sha256 <text> — log Sha256Fingerprint of <text>"),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
	{
		if (Args.Num() < 1)
		{
			UE_LOG(LogAILiveMemory, Error, TEXT("Usage: AILive.Test.Sha256 <text>"));
			return;
		}
		const FString Text = FString::Join(Args, TEXT(" "));
		const FString Hex = AILiveUtil::Sha256Fingerprint(Text);
		UE_LOG(LogAILiveMemory, Display,
			TEXT("Sha256(\"%s\") = %s (len=%d)"),
			*Text, *Hex, Hex.Len());
	}));

static FAutoConsoleCommand GAILiveTestSetQueryOnly(
	TEXT("AILive.Test.SetQueryOnly"),
	TEXT("AILive.Test.SetQueryOnly — apply PRAGMA query_only=1 to the live game.db connection (debug only)"),
	FConsoleCommandDelegate::CreateLambda([]()
	{
		UAILiveEventStoreSubsystem* Sys = GetSubsystemForConsole();
		if (!Sys || !Sys->IsGameOpen())
		{
			UE_LOG(LogAILiveMemory, Error, TEXT("SetQueryOnly: no game open"));
			return;
		}
		const bool bOk = Sys->SetGameDbQueryOnly(true);
		UE_LOG(LogAILiveMemory, Display,
			TEXT("SetQueryOnly -> %s"), bOk ? TEXT("OK") : TEXT("FAIL"));
	}));

static FAutoConsoleCommand GAILiveTestCanonicalEcho(
	TEXT("AILive.Test.CanonicalEcho"),
	TEXT("AILive.Test.CanonicalEcho — verify CanonicalJsonOf is byte-for-byte idempotent and structurally excludes tick_no / wall_clock / *_event_hash"),
	FConsoleCommandDelegate::CreateLambda([]()
	{
		FAILiveEvent Ev;
		Ev.GameId = TEXT("g1");
		Ev.Seq = 42;
		Ev.RoundNo = 3;
		Ev.Phase = EAILivePhase::DayDiscuss;
		Ev.Actor = TEXT("NPC07");
		Ev.EventType = EAILiveEventType::SpeechIntended;
		Ev.SpeechActType = EAILiveSpeechActType::Claim;
		Ev.Visibility = { TEXT("public"), TEXT("NPC03") };
		Ev.AddressedTo = { TEXT("NPC03") };
		Ev.PayloadJson = TEXT("{\"text\":\"x\",\"willingness_score\":1.0,\"empty\":\"\",\"nested\":{\"b\":2,\"a\":1}}");
		Ev.ParentEventId = TEXT("");
		Ev.ParserVersion = TEXT("1");
		Ev.RawLLMOutput = TEXT("");
		Ev.EventId = TEXT("00000000-0000-7000-8000-000000000001");
		Ev.PrevEventHash = TEXT("ffffffff");
		Ev.EventHash = TEXT("eeeeeeee");
		Ev.WallClock = TEXT("2026-05-04T00:00:00.000Z");

		const FString A = UAILiveEventStoreSubsystem::CanonicalJsonOf(Ev);
		const FString B = UAILiveEventStoreSubsystem::CanonicalJsonOf(Ev);
		const bool bByteForByte = (A == B);
		const bool bNoTickNo = !A.Contains(TEXT("\"tick_no\""));
		const bool bNoPrevHash = !A.Contains(TEXT("\"prev_event_hash\""));
		const bool bNoEventHash = !A.Contains(TEXT("\"event_hash\""));
		const bool bNoWallClock = !A.Contains(TEXT("\"wall_clock\""));
		const bool bHasFloat = A.Contains(TEXT("1.0"));
		const bool bHasEmpty = A.Contains(TEXT("\"empty\":\"\""));
		const bool bNestedSorted = A.Contains(TEXT("\"nested\":{\"a\":1.0,\"b\":2.0}"));

		UE_LOG(LogAILiveMemory, Display, TEXT("CanonicalEcho A=%s"), *A);
		UE_LOG(LogAILiveMemory, Display,
			TEXT("CanonicalEcho byte_for_byte_match=%d, tick_no_invariance_match=%d, "
			     "no_prev_event_hash=%d, no_event_hash=%d, no_wall_clock=%d, "
			     "float_round_trip=%d, empty_string_kept=%d, nested_keys_sorted=%d"),
			bByteForByte ? 1 : 0, bNoTickNo ? 1 : 0,
			bNoPrevHash ? 1 : 0, bNoEventHash ? 1 : 0, bNoWallClock ? 1 : 0,
			bHasFloat ? 1 : 0, bHasEmpty ? 1 : 0, bNestedSorted ? 1 : 0);
	}));
