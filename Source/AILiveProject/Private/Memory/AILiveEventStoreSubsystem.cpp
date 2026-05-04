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

// ===== T4 helpers (read API) =====

/**
 * 统一的事件列清单。所有读 SQL（QueryEventsByActor / Quote / QuoteByRound /
 * QuoteRecentRounds / ListMy* / SearchHistory / QuoteByEventTypeAndTick / ListVotes）
 * 都拼这个常量，配套 RowToEvent 按相同顺序逐列读出 FAILiveEvent。
 *
 * tick_no（第 3 列）于 T6 加入：FAILiveEvent.TickNo 字段供 PromptAssembler 按
 * principles §7 模板渲染 `Tick {tick_no} round_no={round_no}` 行。**canonical
 * JSON 仍显式排除该列**（CanonicalJsonOf 注释 + AILive.Test.CanonicalEcho 守护），
 * 哈希链对老库继续兼容。
 *
 * 18 列，索引顺序：
 *  0=event_id  1=game_id  2=seq             3=tick_no   4=round_no  5=phase
 *  6=actor     7=event_type  8=speech_act_type  9=visibility  10=addressed_to
 * 11=payload  12=parent_event_id  13=parser_version  14=raw_llm_output
 * 15=prev_event_hash  16=event_hash  17=wall_clock
 */
const TCHAR* const kEventSelectColumns =
	TEXT("e.event_id, e.game_id, e.seq, e.tick_no, e.round_no, e.phase, "
	     "e.actor, e.event_type, e.speech_act_type, e.visibility, e.addressed_to, "
	     "e.payload, e.parent_event_id, e.parser_version, e.raw_llm_output, "
	     "e.prev_event_hash, e.event_hash, e.wall_clock");

/**
 * 读侧 viewer 展开规则——write 侧 event_visibility 行按字面量存（'public' /
 * 'NPC03' / 'orchestrator' 等不展开成 per-agent 行），所以读侧必须把 NPC viewer
 * 扩成 IN 集合才能取到 visibility=["public"] 的事件。
 *
 * 返回空集表示"任何事件都不可见"——典型场景：viewer 传 "self" / 自由文本 / 空串。
 * 调用方拿到空集应跳过 SQL 直接返回空结果。
 */
TArray<FString> ExpandViewerForJoin(const FString& InViewer)
{
	TArray<FString> Out;

	if (InViewer.IsEmpty() || InViewer == TEXT("self"))
	{
		UE_LOG(LogAILiveMemory, Warning,
			TEXT("ExpandViewerForJoin: rejected viewer='%s' "
			     "('self' / empty 一律返回不可见——必须传具体 actor ID)"), *InViewer);
		return Out;
	}

	if (InViewer == TEXT("public"))
	{
		Out.Add(TEXT("public"));
		return Out;
	}
	if (InViewer == TEXT("orchestrator"))
	{
		Out.Add(TEXT("orchestrator"));
		return Out;
	}
	if (InViewer == TEXT("system"))
	{
		Out.Add(TEXT("system"));
		return Out;
	}
	if (InViewer == TEXT("audience"))
	{
		Out.Add(TEXT("audience"));
		Out.Add(TEXT("public"));
		return Out;
	}
	if (IsValidViewerString(InViewer))
	{
		// NPC<NN> 或 Faction<X> —— 自己定向 + 公开都能看
		Out.Add(InViewer);
		Out.Add(TEXT("public"));
		return Out;
	}

	UE_LOG(LogAILiveMemory, Warning,
		TEXT("ExpandViewerForJoin: unrecognized viewer='%s' — returning empty set"), *InViewer);
	return Out;
}

/** 按 ExpandViewerForJoin 返回的尺寸生成 SQL 占位符串 "?,?,?". 0 个返回空串。 */
FString MakeViewerInPlaceholders(int32 Count)
{
	if (Count <= 0) return FString();
	FString Out;
	Out.Reserve(Count * 2);
	for (int32 i = 0; i < Count; ++i)
	{
		if (i > 0) Out.Append(TEXT(","));
		Out.Append(TEXT("?"));
	}
	return Out;
}

/**
 * 转义 LIKE 模式里的元字符：% / _ / \ → \% / \_ / \\。配套 SQL `ESCAPE '\\'`。
 * 防止用户输入 `100%` 这类内容被当成 SQL 通配符误匹配。
 */
FString EscapeLikePattern(const FString& InKeyword)
{
	FString Out;
	Out.Reserve(InKeyword.Len() + 4);
	for (TCHAR C : InKeyword)
	{
		if (C == TEXT('\\') || C == TEXT('%') || C == TEXT('_'))
		{
			Out.AppendChar(TEXT('\\'));
		}
		Out.AppendChar(C);
	}
	return Out;
}

/**
 * 按 kEventSelectColumns 顺序从 prepared statement 读 18 列，组装 FAILiveEvent。
 * 调用方必须保证 SQL SELECT 列与 kEventSelectColumns 完全一致。
 */
FAILiveEvent RowToEvent(const FSQLitePreparedStatement& InStmt)
{
	FAILiveEvent Ev;
	int64 SeqRead = 0;
	int64 TickNoRead = 0;
	int64 RoundNoRead = 0;
	FString PhaseStr, EventTypeStr, SpeechActStr, VisibilityJson, AddressedToJson;

	InStmt.GetColumnValueByIndex(0,  Ev.EventId);
	InStmt.GetColumnValueByIndex(1,  Ev.GameId);
	InStmt.GetColumnValueByIndex(2,  SeqRead);                                Ev.Seq = SeqRead;
	InStmt.GetColumnValueByIndex(3,  TickNoRead);                             Ev.TickNo = TickNoRead;
	InStmt.GetColumnValueByIndex(4,  RoundNoRead);                            Ev.RoundNo = (int32)RoundNoRead;
	InStmt.GetColumnValueByIndex(5,  PhaseStr);                               Ev.Phase = AILiveEvent::PhaseFromString(PhaseStr);
	InStmt.GetColumnValueByIndex(6,  Ev.Actor);
	InStmt.GetColumnValueByIndex(7,  EventTypeStr);                           Ev.EventType = AILiveEvent::EventTypeFromString(EventTypeStr);
	InStmt.GetColumnValueByIndex(8,  SpeechActStr);                           Ev.SpeechActType = AILiveEvent::SpeechActFromString(SpeechActStr);
	InStmt.GetColumnValueByIndex(9,  VisibilityJson);                         Ev.Visibility = AILiveEvent::JsonStringToArray(VisibilityJson);
	InStmt.GetColumnValueByIndex(10, AddressedToJson);                        Ev.AddressedTo = AILiveEvent::JsonStringToArray(AddressedToJson);
	InStmt.GetColumnValueByIndex(11, Ev.PayloadJson);
	InStmt.GetColumnValueByIndex(12, Ev.ParentEventId);
	InStmt.GetColumnValueByIndex(13, Ev.ParserVersion);
	InStmt.GetColumnValueByIndex(14, Ev.RawLLMOutput);
	InStmt.GetColumnValueByIndex(15, Ev.PrevEventHash);
	InStmt.GetColumnValueByIndex(16, Ev.EventHash);
	InStmt.GetColumnValueByIndex(17, Ev.WallClock);
	return Ev;
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
	DetectedFtsTokenizer.Reset();
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

	// 必须在任何 early return 之前缓存 game db 的 events_fts 实际 tokenizer——
	// SearchHistory 启动后据此决定 LIKE / MATCH 路径。原实现只在迁移路径里赋值，
	// 已在 schema_version=1 的旧 DB 重开时会留空 → SearchHistory 静默退化为 LIKE，
	// 无声绕过 FTS5 与"FTS5 模糊"目标不一致。
	if (!bIsMetaDb)
	{
		FString ExistingFtsSql;
		InDb.Execute(
			TEXT("SELECT sql FROM sqlite_master WHERE name='events_fts';"),
			[&ExistingFtsSql](const FSQLitePreparedStatement& Stmt)
			{
				Stmt.GetColumnValueByIndex(0, ExistingFtsSql);
				return ESQLitePreparedStatementExecuteRowResult::Continue;
			});
		if (!ExistingFtsSql.IsEmpty())
		{
			// events_fts 已在 schema 里 —— 解析其 CREATE VIRTUAL TABLE DDL，
			// 反映 db 真实使用的 tokenizer（trigram / unicode61 / 其它）。
			if (ExistingFtsSql.Contains(TEXT("tokenize='trigram'")) ||
				ExistingFtsSql.Contains(TEXT("tokenize=\"trigram\"")) ||
				ExistingFtsSql.Contains(TEXT("tokenize=trigram")))
			{
				DetectedFtsTokenizer = TEXT("trigram");
			}
			else
			{
				DetectedFtsTokenizer = TEXT("unicode61");
			}
		}
		else
		{
			// events_fts 还未建（首次迁移路径）—— 用 DetectFtsTokenizer 探测当前 SQLite
			// 是否支持 trigram；下面 RunMigrations 会用这个 tokenizer 真建 FTS5 虚表。
			DetectedFtsTokenizer = DetectFtsTokenizer(InDb);
		}
	}

	if (ExistingVersion == kCurrentSchemaVersion)
	{
		UE_LOG(LogAILiveMemory, Log, TEXT("EnsureSchema(%s): schema_version=%d, no migration (fts_tokenizer=%s)"),
			bIsMetaDb ? TEXT("meta") : TEXT("game"), ExistingVersion,
			bIsMetaDb ? TEXT("n/a") : *DetectedFtsTokenizer);
		return true;
	}
	if (ExistingVersion > kCurrentSchemaVersion)
	{
		UE_LOG(LogAILiveMemory, Error,
			TEXT("EnsureSchema(%s): existing schema_version=%d > supported=%d (downgrade refused)"),
			bIsMetaDb ? TEXT("meta") : TEXT("game"), ExistingVersion, kCurrentSchemaVersion);
		return false;
	}

	const TCHAR* TokenizerPtr = bIsMetaDb ? nullptr : *DetectedFtsTokenizer;
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
// T4 — 读 API（视角隔离 EXISTS）+ T9 (hash chain audit) stub.
//
// 共同约定：
//   - 所有 NPC 视角的读路径都通过 event_visibility EXISTS 子查询强制隔离，
//     不靠上层"自觉"过滤 payload（principles 硬约束 6）。
//   - viewer 字符串经 ExpandViewerForJoin 展开成 IN 集合（NPC03 → {NPC03,public}），
//     "self" / 自由文本 一律展开为空集 → 直接返回不可见，不下发 SQL。
//   - 内部用的 QuoteByEventTypeAndTick 例外（无 viewer 过滤——调用方是 orchestrator）。
//   - 所有 SELECT 都用 kEventSelectColumns 18 列（含 tick_no，T6 加入；canonical
//     JSON 仍排除该列），RowToEvent 按相同顺序解列。
//   - 短中文 keyword（< 3 字）走 LIKE fallback；FTS5 trigram 不可用时也走 LIKE。
// ---------------------------------------------------------------

TArray<FAILiveEvent> UAILiveEventStoreSubsystem::QueryEventsByActor(const FString& Actor, int32 LimitCount) const
{
	TArray<FAILiveEvent> Out;
	if (!IsGameOpen() || Actor.IsEmpty()) return Out;

	// self-viewer JOIN：viewer = actor 自身。task card D4 要求所有读路径走 JOIN。
	const TArray<FString> Viewers = ExpandViewerForJoin(Actor);
	if (Viewers.Num() == 0) return Out;

	const FString Sql = FString::Printf(
		TEXT("SELECT %s FROM events e "
		     "WHERE e.game_id = ?1 AND e.actor = ?2 "
		     "  AND EXISTS (SELECT 1 FROM event_visibility v "
		     "              WHERE v.event_id = e.event_id AND v.viewer IN (%s)) "
		     "ORDER BY e.seq%s;"),
		kEventSelectColumns,
		*MakeViewerInPlaceholders(Viewers.Num()),
		LimitCount > 0 ? TEXT(" LIMIT ?") : TEXT(""));

	FSQLitePreparedStatement Stmt;
	if (!Stmt.Create(const_cast<FSQLiteDatabase&>(Db), *Sql))
	{
		UE_LOG(LogAILiveMemory, Error, TEXT("QueryEventsByActor: prepare failed: %s"), *Db.GetLastError());
		return Out;
	}
	int32 BindIdx = 1;
	Stmt.SetBindingValueByIndex(BindIdx++, CurrentGameId);
	Stmt.SetBindingValueByIndex(BindIdx++, Actor);
	for (const FString& V : Viewers) Stmt.SetBindingValueByIndex(BindIdx++, V);
	if (LimitCount > 0) Stmt.SetBindingValueByIndex(BindIdx++, (int64)LimitCount);

	while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		Out.Add(RowToEvent(Stmt));
	}
	return Out;
}

bool UAILiveEventStoreSubsystem::VerifyHashChain(int64& OutFirstBadSeq) const
{
	UE_LOG(LogAILiveMemory, Verbose, TEXT("VerifyHashChain stub (T9)"));
	OutFirstBadSeq = INDEX_NONE;
	return false;
}

bool UAILiveEventStoreSubsystem::Quote(int64 InSeq, const FString& InViewer, FAILiveEvent& OutEvent) const
{
	OutEvent = FAILiveEvent();
	if (!IsGameOpen()) return false;

	const TArray<FString> Viewers = ExpandViewerForJoin(InViewer);
	if (Viewers.Num() == 0) return false;

	const FString Sql = FString::Printf(
		TEXT("SELECT %s FROM events e "
		     "WHERE e.game_id = ?1 AND e.seq = ?2 "
		     "  AND EXISTS (SELECT 1 FROM event_visibility v "
		     "              WHERE v.event_id = e.event_id AND v.viewer IN (%s)) "
		     "LIMIT 1;"),
		kEventSelectColumns,
		*MakeViewerInPlaceholders(Viewers.Num()));

	FSQLitePreparedStatement Stmt;
	if (!Stmt.Create(const_cast<FSQLiteDatabase&>(Db), *Sql))
	{
		UE_LOG(LogAILiveMemory, Error, TEXT("Quote: prepare failed: %s"), *Db.GetLastError());
		return false;
	}
	int32 BindIdx = 1;
	Stmt.SetBindingValueByIndex(BindIdx++, CurrentGameId);
	Stmt.SetBindingValueByIndex(BindIdx++, InSeq);
	for (const FString& V : Viewers) Stmt.SetBindingValueByIndex(BindIdx++, V);

	if (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		OutEvent = RowToEvent(Stmt);
		return true;
	}
	return false;
}

TArray<FAILiveEvent> UAILiveEventStoreSubsystem::QuoteByRound(int32 InRoundNo, const FString& InActor,
                                                              const FString& InViewer) const
{
	TArray<FAILiveEvent> Out;
	if (!IsGameOpen()) return Out;

	const TArray<FString> Viewers = ExpandViewerForJoin(InViewer);
	if (Viewers.Num() == 0) return Out;

	const FString Sql = FString::Printf(
		TEXT("SELECT %s FROM events e "
		     "WHERE e.game_id = ?1 AND e.round_no = ?2 AND e.actor = ?3 "
		     "  AND EXISTS (SELECT 1 FROM event_visibility v "
		     "              WHERE v.event_id = e.event_id AND v.viewer IN (%s)) "
		     "ORDER BY e.seq;"),
		kEventSelectColumns,
		*MakeViewerInPlaceholders(Viewers.Num()));

	FSQLitePreparedStatement Stmt;
	if (!Stmt.Create(const_cast<FSQLiteDatabase&>(Db), *Sql))
	{
		UE_LOG(LogAILiveMemory, Error, TEXT("QuoteByRound: prepare failed: %s"), *Db.GetLastError());
		return Out;
	}
	int32 BindIdx = 1;
	Stmt.SetBindingValueByIndex(BindIdx++, CurrentGameId);
	Stmt.SetBindingValueByIndex(BindIdx++, (int64)InRoundNo);
	Stmt.SetBindingValueByIndex(BindIdx++, InActor);
	for (const FString& V : Viewers) Stmt.SetBindingValueByIndex(BindIdx++, V);

	while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		Out.Add(RowToEvent(Stmt));
	}
	return Out;
}

TArray<FAILiveEvent> UAILiveEventStoreSubsystem::QuoteRecentRounds(int32 InCurrentRound, int32 InK,
                                                                    const FString& InViewer) const
{
	TArray<FAILiveEvent> Out;
	if (!IsGameOpen() || InK <= 0) return Out;

	const TArray<FString> Viewers = ExpandViewerForJoin(InViewer);
	if (Viewers.Num() == 0) return Out;

	const int32 RoundStart = FMath::Max(0, InCurrentRound - InK + 1);
	const int32 RoundEnd   = InCurrentRound;

	const FString Sql = FString::Printf(
		TEXT("SELECT %s FROM events e "
		     "WHERE e.game_id = ?1 AND e.round_no BETWEEN ?2 AND ?3 "
		     "  AND EXISTS (SELECT 1 FROM event_visibility v "
		     "              WHERE v.event_id = e.event_id AND v.viewer IN (%s)) "
		     "ORDER BY e.seq;"),
		kEventSelectColumns,
		*MakeViewerInPlaceholders(Viewers.Num()));

	FSQLitePreparedStatement Stmt;
	if (!Stmt.Create(const_cast<FSQLiteDatabase&>(Db), *Sql))
	{
		UE_LOG(LogAILiveMemory, Error, TEXT("QuoteRecentRounds: prepare failed: %s"), *Db.GetLastError());
		return Out;
	}
	int32 BindIdx = 1;
	Stmt.SetBindingValueByIndex(BindIdx++, CurrentGameId);
	Stmt.SetBindingValueByIndex(BindIdx++, (int64)RoundStart);
	Stmt.SetBindingValueByIndex(BindIdx++, (int64)RoundEnd);
	for (const FString& V : Viewers) Stmt.SetBindingValueByIndex(BindIdx++, V);

	while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		Out.Add(RowToEvent(Stmt));
	}
	return Out;
}

namespace
{

/** ListMyStatements/Notes/Reflections 共用：actor + event_type + viewer JOIN。 */
TArray<FAILiveEvent> ListSelfEventsByType(
	const FSQLiteDatabase& Db, const FString& GameId,
	const FString& AgentId, const FString& EventTypeStr,
	const TArray<FString>& Viewers,
	bool bDescByRound, int32 LimitCount)
{
	TArray<FAILiveEvent> Out;

	const FString OrderBy = bDescByRound
		? FString(TEXT("ORDER BY e.round_no DESC, e.seq DESC"))
		: FString(TEXT("ORDER BY e.seq"));

	const FString LimitClause = LimitCount > 0
		? FString(TEXT(" LIMIT ?"))
		: FString();

	const FString Sql = FString::Printf(
		TEXT("SELECT %s FROM events e "
		     "WHERE e.game_id = ?1 AND e.actor = ?2 AND e.event_type = ?3 "
		     "  AND EXISTS (SELECT 1 FROM event_visibility v "
		     "              WHERE v.event_id = e.event_id AND v.viewer IN (%s)) "
		     "%s%s;"),
		kEventSelectColumns,
		*MakeViewerInPlaceholders(Viewers.Num()),
		*OrderBy, *LimitClause);

	FSQLitePreparedStatement Stmt;
	if (!Stmt.Create(const_cast<FSQLiteDatabase&>(Db), *Sql))
	{
		UE_LOG(LogAILiveMemory, Error, TEXT("ListSelfEventsByType: prepare failed: %s"), *Db.GetLastError());
		return Out;
	}
	int32 BindIdx = 1;
	Stmt.SetBindingValueByIndex(BindIdx++, GameId);
	Stmt.SetBindingValueByIndex(BindIdx++, AgentId);
	Stmt.SetBindingValueByIndex(BindIdx++, EventTypeStr);
	for (const FString& V : Viewers) Stmt.SetBindingValueByIndex(BindIdx++, V);
	if (LimitCount > 0) Stmt.SetBindingValueByIndex(BindIdx++, (int64)LimitCount);

	while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		Out.Add(RowToEvent(Stmt));
	}
	return Out;
}

} // namespace anonymous

TArray<FAILiveEvent> UAILiveEventStoreSubsystem::ListMyStatements(const FString& InAgentId) const
{
	TArray<FAILiveEvent> Out;
	if (!IsGameOpen()) return Out;
	const TArray<FString> Viewers = ExpandViewerForJoin(InAgentId);
	if (Viewers.Num() == 0) return Out;

	// "自我发言全量追溯"（principles 硬约束 2）含 speech.public + private_msg 两通道——
	// 协议层把这二者都视为 agent 发出的"统辞"，T6 PromptAssembler 的"自我发言不可压缩段"
	// 也按这个集合组装。speech.intended/note/reflection 走各自的 ListMy* API。
	// 注：private_msg 能否被 actor 自己 quote 取决于写入侧把 actor 自身放进 visibility
	//（T5/T7 写入侧纪律）；read API 不做旁路，全部走 JOIN。
	const FString Sql = FString::Printf(
		TEXT("SELECT %s FROM events e "
		     "WHERE e.game_id = ?1 AND e.actor = ?2 "
		     "  AND e.event_type IN ('speech.public', 'private_msg') "
		     "  AND EXISTS (SELECT 1 FROM event_visibility v "
		     "              WHERE v.event_id = e.event_id AND v.viewer IN (%s)) "
		     "ORDER BY e.seq;"),
		kEventSelectColumns,
		*MakeViewerInPlaceholders(Viewers.Num()));

	FSQLitePreparedStatement Stmt;
	if (!Stmt.Create(const_cast<FSQLiteDatabase&>(Db), *Sql))
	{
		UE_LOG(LogAILiveMemory, Error, TEXT("ListMyStatements: prepare failed: %s"), *Db.GetLastError());
		return Out;
	}
	int32 BindIdx = 1;
	Stmt.SetBindingValueByIndex(BindIdx++, CurrentGameId);
	Stmt.SetBindingValueByIndex(BindIdx++, InAgentId);
	for (const FString& V : Viewers) Stmt.SetBindingValueByIndex(BindIdx++, V);

	while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		Out.Add(RowToEvent(Stmt));
	}
	return Out;
}

TArray<FAILiveEvent> UAILiveEventStoreSubsystem::ListMyNotes(const FString& InAgentId, int32 InRecentN) const
{
	if (!IsGameOpen()) return {};
	const TArray<FString> Viewers = ExpandViewerForJoin(InAgentId);
	if (Viewers.Num() == 0) return {};
	return ListSelfEventsByType(Db, CurrentGameId, InAgentId,
		AILiveEvent::EventTypeToString(EAILiveEventType::SpeechNote),
		Viewers, /*bDescByRound=*/true, /*LimitCount=*/InRecentN);
}

TArray<FAILiveEvent> UAILiveEventStoreSubsystem::ListMyReflections(const FString& InAgentId, int32 InRecentN) const
{
	if (!IsGameOpen()) return {};
	const TArray<FString> Viewers = ExpandViewerForJoin(InAgentId);
	if (Viewers.Num() == 0) return {};
	return ListSelfEventsByType(Db, CurrentGameId, InAgentId,
		AILiveEvent::EventTypeToString(EAILiveEventType::Reflection9Q),
		Viewers, /*bDescByRound=*/true, /*LimitCount=*/InRecentN);
}

TArray<FAILiveEvent> UAILiveEventStoreSubsystem::ListMyPendingIntended(const FString& InAgentId, int32 InRecentN) const
{
	TArray<FAILiveEvent> Out;
	if (!IsGameOpen()) return Out;

	const TArray<FString> Viewers = ExpandViewerForJoin(InAgentId);
	if (Viewers.Num() == 0) return Out;

	// 走 events 表 + parent 链直算——不读 agent_view_state 投影表。
	// 即使 agent_view_state 被 DROP，本查询仍正确返回（task card 新增 L1 用例）。
	const FString Sql = FString::Printf(
		TEXT("SELECT %s FROM events e "
		     "WHERE e.game_id = ?1 AND e.actor = ?2 AND e.event_type = 'speech.intended' "
		     "  AND EXISTS (SELECT 1 FROM event_visibility v "
		     "              WHERE v.event_id = e.event_id AND v.viewer IN (%s)) "
		     "  AND NOT EXISTS (SELECT 1 FROM events c "
		     "                  WHERE c.game_id = e.game_id "
		     "                    AND c.event_type = 'speech.public' "
		     "                    AND c.parent_event_id = e.event_id) "
		     "ORDER BY e.seq DESC%s;"),
		kEventSelectColumns,
		*MakeViewerInPlaceholders(Viewers.Num()),
		InRecentN > 0 ? TEXT(" LIMIT ?") : TEXT(""));

	FSQLitePreparedStatement Stmt;
	if (!Stmt.Create(const_cast<FSQLiteDatabase&>(Db), *Sql))
	{
		UE_LOG(LogAILiveMemory, Error, TEXT("ListMyPendingIntended: prepare failed: %s"), *Db.GetLastError());
		return Out;
	}
	int32 BindIdx = 1;
	Stmt.SetBindingValueByIndex(BindIdx++, CurrentGameId);
	Stmt.SetBindingValueByIndex(BindIdx++, InAgentId);
	for (const FString& V : Viewers) Stmt.SetBindingValueByIndex(BindIdx++, V);
	if (InRecentN > 0) Stmt.SetBindingValueByIndex(BindIdx++, (int64)InRecentN);

	while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		Out.Add(RowToEvent(Stmt));
	}
	return Out;
}

TArray<FAILiveCommitment> UAILiveEventStoreSubsystem::ListMyCommitments(const FString& InAgentId,
                                                                         int32 InRoundStart,
                                                                         int32 InRoundEnd) const
{
	TArray<FAILiveCommitment> Out;
	if (!IsGameOpen()) return Out;

	// 投影表直读（T8 落地后才会有数据；T4 阶段 commitments 表存在但通常为空）。
	const FString Sql =
		TEXT("SELECT game_id, agent_id, round_no, seq, commitment_type, target, text, status "
		     "FROM commitments "
		     "WHERE game_id = ?1 AND agent_id = ?2 "
		     "  AND (?3 < 0 OR round_no >= ?3) "
		     "  AND (?4 < 0 OR round_no <= ?4) "
		     "ORDER BY round_no, seq;");

	FSQLitePreparedStatement Stmt;
	if (!Stmt.Create(const_cast<FSQLiteDatabase&>(Db), *Sql))
	{
		UE_LOG(LogAILiveMemory, Verbose, TEXT("ListMyCommitments: prepare failed (commitments 表不存在或无数据): %s"),
			*Db.GetLastError());
		return Out;
	}
	Stmt.SetBindingValueByIndex(1, CurrentGameId);
	Stmt.SetBindingValueByIndex(2, InAgentId);
	Stmt.SetBindingValueByIndex(3, (int64)InRoundStart);
	Stmt.SetBindingValueByIndex(4, (int64)InRoundEnd);

	while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		FAILiveCommitment C;
		int64 RoundNoRead = 0;
		int64 SeqRead = 0;
		FString TypeStr, StatusStr;
		Stmt.GetColumnValueByIndex(0, C.GameId);
		Stmt.GetColumnValueByIndex(1, C.AgentId);
		Stmt.GetColumnValueByIndex(2, RoundNoRead); C.RoundNo = (int32)RoundNoRead;
		Stmt.GetColumnValueByIndex(3, SeqRead);     C.Seq = SeqRead;
		Stmt.GetColumnValueByIndex(4, TypeStr);     C.CommitmentType = AILiveEvent::CommitmentTypeFromString(TypeStr);
		Stmt.GetColumnValueByIndex(5, C.Target);
		Stmt.GetColumnValueByIndex(6, C.Text);
		Stmt.GetColumnValueByIndex(7, StatusStr);   C.Status = AILiveEvent::CommitmentStatusFromString(StatusStr);
		Out.Add(C);
	}
	return Out;
}

TArray<FAILiveEvent> UAILiveEventStoreSubsystem::SearchHistory(const FString& InKeyword, const FString& InActor,
                                                                int32 InRoundStart, int32 InRoundEnd,
                                                                const FString& InViewer, int32 InLimit) const
{
	TArray<FAILiveEvent> Out;
	if (!IsGameOpen() || InKeyword.IsEmpty()) return Out;

	const TArray<FString> Viewers = ExpandViewerForJoin(InViewer);
	if (Viewers.Num() == 0) return Out;

	// 双路径：< 3 字 keyword 或 trigram 不可用走 LIKE；否则 FTS5 MATCH。
	const bool bUseLike = (InKeyword.Len() < 3) || (DetectedFtsTokenizer != TEXT("trigram"));
	UE_LOG(LogAILiveMemory, Display,
		TEXT("SearchHistory: keyword='%s' viewer='%s' path=%s tokenizer=%s"),
		*InKeyword, *InViewer, bUseLike ? TEXT("LIKE") : TEXT("FTS5"), *DetectedFtsTokenizer);

	const int32 ActualLimit = InLimit > 0 ? InLimit : 100;

	FString Sql;
	if (bUseLike)
	{
		Sql = FString::Printf(
			TEXT("SELECT %s FROM events e "
			     "WHERE e.game_id = ?1 "
			     "  AND e.payload_text LIKE ?2 ESCAPE '\\' "
			     "  AND (?3 = '' OR e.actor = ?3) "
			     "  AND (?4 < 0 OR e.round_no >= ?4) "
			     "  AND (?5 < 0 OR e.round_no <= ?5) "
			     "  AND EXISTS (SELECT 1 FROM event_visibility v "
			     "              WHERE v.event_id = e.event_id AND v.viewer IN (%s)) "
			     "ORDER BY e.seq DESC LIMIT ?;"),
			kEventSelectColumns,
			*MakeViewerInPlaceholders(Viewers.Num()));
	}
	else
	{
		Sql = FString::Printf(
			TEXT("SELECT %s FROM events e "
			     "JOIN events_fts f ON f.rowid = e.rowid "
			     "WHERE e.game_id = ?1 "
			     "  AND events_fts MATCH ?2 "
			     "  AND (?3 = '' OR e.actor = ?3) "
			     "  AND (?4 < 0 OR e.round_no >= ?4) "
			     "  AND (?5 < 0 OR e.round_no <= ?5) "
			     "  AND EXISTS (SELECT 1 FROM event_visibility v "
			     "              WHERE v.event_id = e.event_id AND v.viewer IN (%s)) "
			     "ORDER BY e.seq DESC LIMIT ?;"),
			kEventSelectColumns,
			*MakeViewerInPlaceholders(Viewers.Num()));
	}

	FSQLitePreparedStatement Stmt;
	if (!Stmt.Create(const_cast<FSQLiteDatabase&>(Db), *Sql))
	{
		UE_LOG(LogAILiveMemory, Error, TEXT("SearchHistory: prepare failed: %s"), *Db.GetLastError());
		return Out;
	}
	int32 BindIdx = 1;
	Stmt.SetBindingValueByIndex(BindIdx++, CurrentGameId);
	if (bUseLike)
	{
		const FString Pattern = TEXT("%") + EscapeLikePattern(InKeyword) + TEXT("%");
		Stmt.SetBindingValueByIndex(BindIdx++, Pattern);
	}
	else
	{
		Stmt.SetBindingValueByIndex(BindIdx++, InKeyword);
	}
	Stmt.SetBindingValueByIndex(BindIdx++, InActor);
	Stmt.SetBindingValueByIndex(BindIdx++, (int64)InRoundStart);
	Stmt.SetBindingValueByIndex(BindIdx++, (int64)InRoundEnd);
	for (const FString& V : Viewers) Stmt.SetBindingValueByIndex(BindIdx++, V);
	Stmt.SetBindingValueByIndex(BindIdx++, (int64)ActualLimit);

	while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		Out.Add(RowToEvent(Stmt));
	}
	return Out;
}

TArray<FAILiveEvent> UAILiveEventStoreSubsystem::ListVotes(int32 InRoundNo) const
{
	TArray<FAILiveEvent> Out;
	if (!IsGameOpen()) return Out;

	// 直接查 events 表的 event_type='vote' 公开原文——返回的 FAILiveEvent 含完整
	// EventId/PayloadJson/Visibility/EventHash/parent_event_id 等字段，调用方可
	// 按真事件 quote/审计。**不**读 vote_history 投影表（投影表只能给计数/索引，
	// 提取原文必须回 events）。视角隔离对协议层公开事件 hard-code 'public'。
	const FString Sql = FString::Printf(
		TEXT("SELECT %s FROM events e "
		     "WHERE e.game_id = ?1 AND e.round_no = ?2 AND e.event_type = 'vote' "
		     "  AND EXISTS (SELECT 1 FROM event_visibility v "
		     "              WHERE v.event_id = e.event_id AND v.viewer = 'public') "
		     "ORDER BY e.seq;"),
		kEventSelectColumns);

	FSQLitePreparedStatement Stmt;
	if (!Stmt.Create(const_cast<FSQLiteDatabase&>(Db), *Sql))
	{
		UE_LOG(LogAILiveMemory, Error, TEXT("ListVotes: prepare failed: %s"), *Db.GetLastError());
		return Out;
	}
	Stmt.SetBindingValueByIndex(1, CurrentGameId);
	Stmt.SetBindingValueByIndex(2, (int64)InRoundNo);

	while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		Out.Add(RowToEvent(Stmt));
	}
	return Out;
}

FString UAILiveEventStoreSubsystem::ListAllianceStateJson() const
{
	if (!IsGameOpen()) return TEXT("[]");

	// 投影表直读：alliance_state 全部行 → JSON 数组（T8 落地后才会有数据）。
	const TCHAR* Sql =
		TEXT("SELECT alliance_id, members, proposed_at_seq, accepted_at_seq, "
		     "       betrayed_at_seq, terms FROM alliance_state "
		     "WHERE game_id = ?1;");

	FSQLitePreparedStatement Stmt;
	if (!Stmt.Create(const_cast<FSQLiteDatabase&>(Db), Sql))
	{
		UE_LOG(LogAILiveMemory, Verbose, TEXT("ListAllianceStateJson: prepare failed: %s"), *Db.GetLastError());
		return TEXT("[]");
	}
	Stmt.SetBindingValueByIndex(1, CurrentGameId);

	FString Out;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
	Writer->WriteArrayStart();
	while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		FString AllianceId, Members, Terms;
		int64 ProposedAt = 0, AcceptedAt = 0, BetrayedAt = 0;
		Stmt.GetColumnValueByIndex(0, AllianceId);
		Stmt.GetColumnValueByIndex(1, Members);
		Stmt.GetColumnValueByIndex(2, ProposedAt);
		Stmt.GetColumnValueByIndex(3, AcceptedAt);
		Stmt.GetColumnValueByIndex(4, BetrayedAt);
		Stmt.GetColumnValueByIndex(5, Terms);

		Writer->WriteObjectStart();
		Writer->WriteValue(TEXT("alliance_id"), AllianceId);
		// members 是 JSON 数组字符串（schema "members: array"），原样嵌入。
		Writer->WriteRawJSONValue(TEXT("members"), Members);
		Writer->WriteValue(TEXT("proposed_at_seq"), ProposedAt);
		Writer->WriteValue(TEXT("accepted_at_seq"), AcceptedAt);
		Writer->WriteValue(TEXT("betrayed_at_seq"), BetrayedAt);
		// terms 是普通字符串（schema "terms: string"），用 WriteValue 转义引号——
		// 普通文本直接 raw 写入会产出非法 JSON（如 "terms": some text）。
		Writer->WriteValue(TEXT("terms"), Terms);
		Writer->WriteObjectEnd();
	}
	Writer->WriteArrayEnd();
	Writer->Close();
	return Out;
}

TArray<FAILiveEvent> UAILiveEventStoreSubsystem::QuoteByEventTypeAndTick(
	EAILiveEventType InEventType, int64 InTickNo) const
{
	TArray<FAILiveEvent> Out;
	if (!IsGameOpen()) return Out;

	// 内部用——orchestrator 自身调用，不做 viewer 过滤。
	const FString EventTypeStr = AILiveEvent::EventTypeToString(InEventType);
	const FString Sql = FString::Printf(
		TEXT("SELECT %s FROM events e "
		     "WHERE e.game_id = ?1 AND e.tick_no = ?2 AND e.event_type = ?3 "
		     "ORDER BY e.seq;"),
		kEventSelectColumns);

	FSQLitePreparedStatement Stmt;
	if (!Stmt.Create(const_cast<FSQLiteDatabase&>(Db), *Sql))
	{
		UE_LOG(LogAILiveMemory, Error, TEXT("QuoteByEventTypeAndTick: prepare failed: %s"), *Db.GetLastError());
		return Out;
	}
	Stmt.SetBindingValueByIndex(1, CurrentGameId);
	Stmt.SetBindingValueByIndex(2, InTickNo);
	Stmt.SetBindingValueByIndex(3, EventTypeStr);

	while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		Out.Add(RowToEvent(Stmt));
	}
	return Out;
}

// ---------------------------------------------------------------
// T7 — Bid 协议 + Floor control（principles §5.2bis / §5.4）
// ---------------------------------------------------------------

namespace
{
	// 解 bid payload：{ "urgency": float, "proposed_target"?: str, "rationale"?: str }
	bool ParseBidPayload(const FString& InPayloadJson, float& OutUrgency,
	                     FString& OutProposedTarget, FString& OutRationale)
	{
		OutUrgency = 0.f;
		TSharedPtr<FJsonObject> Obj;
		const auto R = TJsonReaderFactory<TCHAR>::Create(InPayloadJson);
		if (!FJsonSerializer::Deserialize(R, Obj) || !Obj.IsValid()) return false;
		double Tmp = 0.0;
		if (Obj->TryGetNumberField(TEXT("urgency"), Tmp)) OutUrgency = static_cast<float>(Tmp);
		Obj->TryGetStringField(TEXT("proposed_target"), OutProposedTarget);
		Obj->TryGetStringField(TEXT("rationale"), OutRationale);
		return true;
	}

	// 从 intended payload 抽 addressed_to_hint 数组
	TArray<FString> ParseAddressedToHint(const FString& InIntendedPayloadJson)
	{
		TArray<FString> Out;
		TSharedPtr<FJsonObject> Obj;
		const auto R = TJsonReaderFactory<TCHAR>::Create(InIntendedPayloadJson);
		if (!FJsonSerializer::Deserialize(R, Obj) || !Obj.IsValid()) return Out;
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (Obj->TryGetArrayField(TEXT("addressed_to_hint"), Arr) && Arr)
		{
			for (const TSharedPtr<FJsonValue>& V : *Arr)
			{
				FString S;
				if (V.IsValid() && V->TryGetString(S)) Out.Add(S);
			}
		}
		return Out;
	}

	// 从 tick_resolved payload 取 winner_actor + winner_intended_seq
	bool ParseTickResolvedPayload(const FString& InPayloadJson, FString& OutWinner,
	                              int64& OutWinnerIntendedSeq)
	{
		OutWinner.Reset();
		OutWinnerIntendedSeq = 0;
		TSharedPtr<FJsonObject> Obj;
		const auto R = TJsonReaderFactory<TCHAR>::Create(InPayloadJson);
		if (!FJsonSerializer::Deserialize(R, Obj) || !Obj.IsValid()) return false;
		Obj->TryGetStringField(TEXT("winner_actor"), OutWinner);
		double Tmp = 0.0;
		if (Obj->TryGetNumberField(TEXT("winner_intended_seq"), Tmp)) OutWinnerIntendedSeq = static_cast<int64>(Tmp);
		return true;
	}
}

TArray<FAILiveBid> UAILiveEventStoreSubsystem::ListBidsForTick(int64 InTickNo) const
{
	TArray<FAILiveBid> Out;
	if (!IsGameOpen()) return Out;

	// 协议不变量：每 actor 每拍最多 1 条 intended + 1 条 bid。
	// JOIN events i ON i.game_id=b.game_id AND i.tick_no=b.tick_no AND i.actor=b.actor
	// AND i.event_type='speech.intended' 反解 IntendedSeq。
	const FString Sql = FString::Printf(
		TEXT("SELECT b.actor, b.seq, b.payload, COALESCE(i.seq, 0) "
		     "FROM events b "
		     "LEFT JOIN events i "
		     "  ON i.game_id = b.game_id AND i.tick_no = b.tick_no "
		     " AND i.actor = b.actor AND i.event_type = ?3 "
		     "WHERE b.game_id = ?1 AND b.tick_no = ?2 AND b.event_type = ?4 "
		     "ORDER BY b.seq;"));

	FSQLitePreparedStatement Stmt;
	if (!Stmt.Create(const_cast<FSQLiteDatabase&>(Db), *Sql))
	{
		UE_LOG(LogAILiveMemory, Error, TEXT("ListBidsForTick: prepare failed: %s"), *Db.GetLastError());
		return Out;
	}
	const FString IntendedTypeStr = AILiveEvent::EventTypeToString(EAILiveEventType::SpeechIntended);
	const FString BidTypeStr      = AILiveEvent::EventTypeToString(EAILiveEventType::Bid);
	Stmt.SetBindingValueByIndex(1, CurrentGameId);
	Stmt.SetBindingValueByIndex(2, InTickNo);
	Stmt.SetBindingValueByIndex(3, IntendedTypeStr);
	Stmt.SetBindingValueByIndex(4, BidTypeStr);

	while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		FAILiveBid B;
		Stmt.GetColumnValueByIndex(0, B.Actor);
		Stmt.GetColumnValueByIndex(1, B.Seq);
		FString PayloadJson;
		Stmt.GetColumnValueByIndex(2, PayloadJson);
		Stmt.GetColumnValueByIndex(3, B.IntendedSeq);
		ParseBidPayload(PayloadJson, B.Urgency, B.ProposedTarget, B.Rationale);
		// BidOffset / FinalScore / RuntimeAdj 由 ResolveFloor 填
		Out.Add(MoveTemp(B));
	}
	return Out;
}

float UAILiveEventStoreSubsystem::ComputeRuntimeAdjustment(
	const FString& InActor, int64 InCurrentTickNo) const
{
	float Adj = 0.f;
	if (!IsGameOpen() || InActor.IsEmpty() || InCurrentTickNo <= 0) return Adj;

	// 反霸麦：扫 tick-1..tick-3 的 tick_resolved.winner_actor。
	// 任务卡常量版：连续 ≥ 3 拍命中 → -1.5（一次性，不叠乘）。
	{
		int32 Streak = 0;
		bool  bBroken = false;
		for (int64 T = InCurrentTickNo - 1; T >= InCurrentTickNo - 3 && T > 0 && !bBroken; --T)
		{
			TArray<FAILiveEvent> TR = QuoteByEventTypeAndTick(
				EAILiveEventType::OrchestratorTickResolved, T);
			bool bFoundWin = false;
			for (const FAILiveEvent& E : TR)
			{
				FString W; int64 _ = 0;
				if (ParseTickResolvedPayload(E.PayloadJson, W, _) && W == InActor)
				{
					bFoundWin = true;
					break;
				}
			}
			if (bFoundWin) ++Streak;
			else { bBroken = true; }
		}
		if (Streak >= 3) Adj += -1.5f;
	}

	// 被 @ 加权：扫上一拍 tick_resolved → winner intended.addressed_to_hint 含本 actor → +2.0。
	if (InCurrentTickNo >= 2)
	{
		TArray<FAILiveEvent> TR = QuoteByEventTypeAndTick(
			EAILiveEventType::OrchestratorTickResolved, InCurrentTickNo - 1);
		for (const FAILiveEvent& E : TR)
		{
			FString W; int64 IntSeq = 0;
			if (!ParseTickResolvedPayload(E.PayloadJson, W, IntSeq) || W.IsEmpty() || IntSeq <= 0) continue;

			// 取 winner intended 事件，无 viewer 过滤（orchestrator 自己用）。
			const FString Sql = FString::Printf(
				TEXT("SELECT %s FROM events e WHERE e.game_id = ?1 AND e.seq = ?2;"),
				kEventSelectColumns);
			FSQLitePreparedStatement St;
			if (!St.Create(const_cast<FSQLiteDatabase&>(Db), *Sql)) continue;
			St.SetBindingValueByIndex(1, CurrentGameId);
			St.SetBindingValueByIndex(2, IntSeq);
			if (St.Step() == ESQLitePreparedStatementStepResult::Row)
			{
				const FAILiveEvent IntEv = RowToEvent(St);
				const TArray<FString> Targets = ParseAddressedToHint(IntEv.PayloadJson);
				if (Targets.Contains(InActor)) { Adj += 2.0f; break; }
			}
		}
	}

	// 沉默加权：本 actor 最近 5 拍无 speech.public（actor=本 NPC）→ +0.5。
	if (InCurrentTickNo >= 6)
	{
		const FString PubTypeStr = AILiveEvent::EventTypeToString(EAILiveEventType::SpeechPublic);
		const FString Sql = TEXT(
			"SELECT 1 FROM events "
			"WHERE game_id = ?1 AND actor = ?2 AND event_type = ?3 "
			"  AND tick_no BETWEEN ?4 AND ?5 LIMIT 1;");
		FSQLitePreparedStatement St;
		if (St.Create(const_cast<FSQLiteDatabase&>(Db), *Sql))
		{
			St.SetBindingValueByIndex(1, CurrentGameId);
			St.SetBindingValueByIndex(2, InActor);
			St.SetBindingValueByIndex(3, PubTypeStr);
			St.SetBindingValueByIndex(4, InCurrentTickNo - 5);
			St.SetBindingValueByIndex(5, InCurrentTickNo - 1);
			const bool bFound = (St.Step() == ESQLitePreparedStatementStepResult::Row);
			if (!bFound) Adj += 0.5f;
		}
	}

	return Adj;
}

FAILiveTickResolution UAILiveEventStoreSubsystem::ResolveFloor(
	int64 InTickNo,
	const TArray<FString>& InEligibleAgentIds,
	const TMap<FString, float>& InAgentBidOffsets,
	float InColdThreshold) const
{
	FAILiveTickResolution Out;
	Out.TickNo = static_cast<int32>(InTickNo);
	if (!IsGameOpen()) return Out;

	TArray<FAILiveBid> AllBids = ListBidsForTick(InTickNo);

	// 过滤到 eligible 集合
	TSet<FString> EligibleSet;
	for (const FString& A : InEligibleAgentIds) EligibleSet.Add(A);

	float BestScore = -FLT_MAX;
	FString BestActor;
	int64   BestIntendedSeq = 0;

	for (FAILiveBid& B : AllBids)
	{
		if (!EligibleSet.Contains(B.Actor)) continue;
		const float* OffPtr = InAgentBidOffsets.Find(B.Actor);
		B.BidOffset  = OffPtr ? *OffPtr : 0.f;
		B.RuntimeAdj = ComputeRuntimeAdjustment(B.Actor, InTickNo);
		B.FinalScore = B.Urgency + B.BidOffset + B.RuntimeAdj;

		// 同分按 lexicographic actor_id 取小（决策 #4）
		const bool bWin =
			(B.FinalScore > BestScore) ||
			(B.FinalScore == BestScore && (BestActor.IsEmpty() || B.Actor < BestActor));
		if (bWin)
		{
			BestScore = B.FinalScore;
			BestActor = B.Actor;
			BestIntendedSeq = B.IntendedSeq;
		}
		Out.AllBids.Add(B);
	}

	if (BestScore >= InColdThreshold && !BestActor.IsEmpty())
	{
		Out.WinnerActor       = BestActor;
		Out.WinnerIntendedSeq = BestIntendedSeq;
	}
	// 否则 WinnerActor 留空（冷场）
	return Out;
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
// Excluded: tick_no (protocol index — T6 加入 FAILiveEvent.TickNo 字段后该
// 字段在内存里非零，但 canonical JSON 序列化必须继续跳过，否则哈希链对老库
// 不兼容；AILive.Test.CanonicalEcho 守护此不变量), prev_event_hash (already
// in hash via Combined = prev || canonical), event_hash (self), wall_clock
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
	// T6: 回填 TickNo 给调用方——原来只 bind 到 SQL 第 17 列（line 1562），
	// 新增 FAILiveEvent.TickNo 字段后必须同步回填，否则 PromptAssembler 拿到
	// 的事件结构体 TickNo 仍是 0。CanonicalJsonOf 跳过 tick_no 字段，所以
	// 回填**不影响**哈希计算。
	InOutEvent.TickNo = CachedCurrentTickNo;

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

// ---------------------------------------------------------------
// T4 — debug/test console commands (read API + deterministic seed).
// ---------------------------------------------------------------

namespace
{

/** Helper：打印 FAILiveEvent 简要信息——console 命令日志统一格式。 */
void LogEventBrief(const TCHAR* Label, const FAILiveEvent& Ev)
{
	UE_LOG(LogAILiveMemory, Display,
		TEXT("[%s] seq=%lld type=%s actor=%s round=%d event_id=%s text=%s"),
		Label, Ev.Seq,
		*AILiveEvent::EventTypeToString(Ev.EventType),
		*Ev.Actor, Ev.RoundNo, *Ev.EventId,
		*Ev.PayloadJson.Left(120));
}

} // namespace anonymous

static FAutoConsoleCommand GAILiveTestSeedReadAcceptance(
	TEXT("AILive.Test.SeedReadAcceptance"),
	TEXT("AILive.Test.SeedReadAcceptance — write deterministic event set covering all T4 L1 acceptance shapes "
	     "(public/intended/orphan-intended/system_inflight/NPC07-only/multi-vis-dedupe/bid)"),
	FConsoleCommandDelegate::CreateLambda([]()
	{
		UAILiveEventStoreSubsystem* Sys = GetSubsystemForConsole();
		if (!Sys || !Sys->IsGameOpen())
		{
			UE_LOG(LogAILiveMemory, Error, TEXT("SeedReadAcceptance: no game open; call AILive.Test.BeginGame first"));
			return;
		}

		// 1) 4 条 NPC03 speech.public 含"结盟"/"联盟"关键词
		const TCHAR* Phrases[] = {
			TEXT("我想提出结盟提议"),
			TEXT("联盟形成，互相承诺"),
			TEXT("继续坚持结盟"),
			TEXT("结盟方案需要再讨论")
		};
		for (int32 i = 0; i < 4; ++i)
		{
			FAILiveEvent Ev;
			Ev.Actor = TEXT("NPC03");
			Ev.EventType = EAILiveEventType::SpeechPublic;
			Ev.Phase = EAILivePhase::DayDiscuss;
			Ev.RoundNo = 1 + i;
			Ev.Visibility = { TEXT("public") };
			Ev.PayloadJson = FString::Printf(TEXT("{\"text\":%s}"),
				*AILiveUtil::EscapeJsonString(Phrases[i]));
			const int64 Seq = Sys->AppendEvent(Ev);
			LogEventBrief(TEXT("seed.public.NPC03"), Ev);
			(void)Seq;
		}

		// 2) NPC04 speech.intended round 1，无 public 子（pending_intended 应命中）
		FAILiveEvent OrphanIntended;
		OrphanIntended.Actor = TEXT("NPC04");
		OrphanIntended.EventType = EAILiveEventType::SpeechIntended;
		OrphanIntended.Phase = EAILivePhase::DayDiscuss;
		OrphanIntended.RoundNo = 1;
		OrphanIntended.Visibility = { TEXT("NPC04") };
		OrphanIntended.PayloadJson = TEXT("{\"text\":\"想接话但没抢中 floor\"}");
		Sys->AppendEvent(OrphanIntended);
		LogEventBrief(TEXT("seed.orphan_intended.NPC04"), OrphanIntended);

		// 3) NPC04 speech.intended round 2，将作为下一条 public 的 parent
		FAILiveEvent ParentIntended;
		ParentIntended.Actor = TEXT("NPC04");
		ParentIntended.EventType = EAILiveEventType::SpeechIntended;
		ParentIntended.Phase = EAILivePhase::DayDiscuss;
		ParentIntended.RoundNo = 2;
		ParentIntended.Visibility = { TEXT("NPC04") };
		ParentIntended.PayloadJson = TEXT("{\"text\":\"想说的内容\"}");
		Sys->AppendEvent(ParentIntended);
		LogEventBrief(TEXT("seed.parent_intended.NPC04"), ParentIntended);

		// 4) NPC04 speech.public round 2，parent_event_id = 步骤 3 的 EventId
		FAILiveEvent DerivedPublic;
		DerivedPublic.Actor = TEXT("NPC04");
		DerivedPublic.EventType = EAILiveEventType::SpeechPublic;
		DerivedPublic.Phase = EAILivePhase::DayDiscuss;
		DerivedPublic.RoundNo = 2;
		DerivedPublic.Visibility = { TEXT("public") };
		DerivedPublic.ParentEventId = ParentIntended.EventId;
		DerivedPublic.PayloadJson = TEXT("{\"text\":\"我说出了刚才想的内容\"}");
		Sys->AppendEvent(DerivedPublic);
		LogEventBrief(TEXT("seed.derived_public.NPC04"), DerivedPublic);

		// 5) NPC07-only 事件（视角隔离用例）
		FAILiveEvent Npc07Only;
		Npc07Only.Actor = TEXT("NPC05");
		Npc07Only.EventType = EAILiveEventType::PrivateMsg;
		Npc07Only.Phase = EAILivePhase::DayDiscuss;
		Npc07Only.RoundNo = 1;
		Npc07Only.Visibility = { TEXT("NPC07") };
		Npc07Only.AddressedTo = { TEXT("NPC07") };
		Npc07Only.PayloadJson = TEXT("{\"text\":\"私信给 NPC07\"}");
		Sys->AppendEvent(Npc07Only);
		LogEventBrief(TEXT("seed.npc07_only"), Npc07Only);

		// 6) system.llm_inflight visibility=["system"]
		FAILiveEvent SystemInflight;
		SystemInflight.Actor = TEXT("system");
		SystemInflight.EventType = EAILiveEventType::SystemLLMInflight;
		SystemInflight.Phase = EAILivePhase::Setup;
		SystemInflight.RoundNo = 1;
		SystemInflight.Visibility = { TEXT("system") };
		SystemInflight.PayloadJson = TEXT("{\"text\":\"LLM call in flight\",\"request_id\":\"req-seed-1\"}");
		Sys->AppendEvent(SystemInflight);
		LogEventBrief(TEXT("seed.system_inflight"), SystemInflight);

		// 7) NPC05 speech.public visibility=["public", "NPC03"]（去重用例）
		FAILiveEvent MultiVis;
		MultiVis.Actor = TEXT("NPC05");
		MultiVis.EventType = EAILiveEventType::SpeechPublic;
		MultiVis.Phase = EAILivePhase::DayDiscuss;
		MultiVis.RoundNo = 1;
		MultiVis.Visibility = { TEXT("public"), TEXT("NPC03") };
		MultiVis.PayloadJson = TEXT("{\"text\":\"复合可见性测试 NPC03 应仅看到一次\"}");
		Sys->AppendEvent(MultiVis);
		LogEventBrief(TEXT("seed.multi_vis"), MultiVis);

		// 8) NPC04 bid visibility=["orchestrator"]
		FAILiveEvent BidEv;
		BidEv.Actor = TEXT("NPC04");
		BidEv.EventType = EAILiveEventType::Bid;
		BidEv.Phase = EAILivePhase::DayDiscuss;
		BidEv.RoundNo = 1;
		BidEv.Visibility = { TEXT("orchestrator") };
		BidEv.PayloadJson = TEXT("{\"text\":\"NPC04 出价\",\"willingness_score\":4.2}");
		Sys->AppendEvent(BidEv);
		LogEventBrief(TEXT("seed.bid"), BidEv);

		// 9) NPC03 private_msg actor=NPC03 visibility=["NPC04", "NPC03"]
		// （写入侧把 actor 自身放进 visibility，让 ListMyStatements 视角隔离 JOIN 仍能命中）
		FAILiveEvent PrivateMsg;
		PrivateMsg.Actor = TEXT("NPC03");
		PrivateMsg.EventType = EAILiveEventType::PrivateMsg;
		PrivateMsg.Phase = EAILivePhase::DayDiscuss;
		PrivateMsg.RoundNo = 1;
		PrivateMsg.Visibility = { TEXT("NPC04"), TEXT("NPC03") };
		PrivateMsg.AddressedTo = { TEXT("NPC04") };
		PrivateMsg.PayloadJson = TEXT("{\"text\":\"NPC03 给 NPC04 的私聊\"}");
		Sys->AppendEvent(PrivateMsg);
		LogEventBrief(TEXT("seed.private_msg.NPC03"), PrivateMsg);

		// 10) NPC03 vote round=1 visibility=["public"]
		FAILiveEvent VoteEv;
		VoteEv.Actor = TEXT("NPC03");
		VoteEv.EventType = EAILiveEventType::Vote;
		VoteEv.Phase = EAILivePhase::Vote;
		VoteEv.RoundNo = 1;
		VoteEv.Visibility = { TEXT("public") };
		VoteEv.AddressedTo = { TEXT("NPC07") };
		VoteEv.PayloadJson = TEXT("{\"text\":\"投 NPC07\",\"target\":\"NPC07\",\"reason\":\"测试\"}");
		Sys->AppendEvent(VoteEv);
		LogEventBrief(TEXT("seed.vote.NPC03"), VoteEv);

		UE_LOG(LogAILiveMemory, Display, TEXT("SeedReadAcceptance done — 13 events written"));
	}));

static FAutoConsoleCommand GAILiveTestQuoteByRoundCmd(
	TEXT("AILive.Test.QuoteByRound"),
	TEXT("AILive.Test.QuoteByRound <round> <actor> <viewer> — list events of <actor> at <round> visible to <viewer>"),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
	{
		if (Args.Num() < 3)
		{
			UE_LOG(LogAILiveMemory, Error, TEXT("Usage: AILive.Test.QuoteByRound <round> <actor> <viewer>"));
			return;
		}
		UAILiveEventStoreSubsystem* Sys = GetSubsystemForConsole();
		if (!Sys || !Sys->IsGameOpen())
		{
			UE_LOG(LogAILiveMemory, Error, TEXT("QuoteByRound: no game open"));
			return;
		}
		const int32 R = FCString::Atoi(*Args[0]);
		const TArray<FAILiveEvent> Rows = Sys->QuoteByRound(R, Args[1], Args[2]);
		UE_LOG(LogAILiveMemory, Display,
			TEXT("QuoteByRound(round=%d, actor=%s, viewer=%s) -> %d rows"),
			R, *Args[1], *Args[2], Rows.Num());
		for (const FAILiveEvent& E : Rows) LogEventBrief(TEXT("QuoteByRound"), E);
	}));

static FAutoConsoleCommand GAILiveTestQuoteSeqCmd(
	TEXT("AILive.Test.QuoteSeq"),
	TEXT("AILive.Test.QuoteSeq <seq> <viewer> — Quote single event by seq, viewer-isolation enforced"),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
	{
		if (Args.Num() < 2)
		{
			UE_LOG(LogAILiveMemory, Error, TEXT("Usage: AILive.Test.QuoteSeq <seq> <viewer>"));
			return;
		}
		UAILiveEventStoreSubsystem* Sys = GetSubsystemForConsole();
		if (!Sys || !Sys->IsGameOpen())
		{
			UE_LOG(LogAILiveMemory, Error, TEXT("QuoteSeq: no game open"));
			return;
		}
		const int64 Seq = FCString::Atoi64(*Args[0]);
		FAILiveEvent Ev;
		const bool bVisible = Sys->Quote(Seq, Args[1], Ev);
		if (bVisible)
		{
			LogEventBrief(TEXT("QuoteSeq.visible"), Ev);
		}
		else
		{
			UE_LOG(LogAILiveMemory, Display,
				TEXT("QuoteSeq(seq=%lld, viewer=%s) -> NOT VISIBLE / NOT FOUND"),
				Seq, *Args[1]);
		}
	}));

static FAutoConsoleCommand GAILiveTestSearchHistoryCmd(
	TEXT("AILive.Test.SearchHistory"),
	TEXT("AILive.Test.SearchHistory <keyword> <viewer> [limit] — fuzzy search; auto-routes between LIKE and FTS5 trigram"),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
	{
		if (Args.Num() < 2)
		{
			UE_LOG(LogAILiveMemory, Error, TEXT("Usage: AILive.Test.SearchHistory <keyword> <viewer> [limit]"));
			return;
		}
		UAILiveEventStoreSubsystem* Sys = GetSubsystemForConsole();
		if (!Sys || !Sys->IsGameOpen())
		{
			UE_LOG(LogAILiveMemory, Error, TEXT("SearchHistory: no game open"));
			return;
		}
		const int32 Limit = Args.Num() >= 3 ? FCString::Atoi(*Args[2]) : 50;
		const TArray<FAILiveEvent> Rows = Sys->SearchHistory(Args[0], FString(),
			-1, -1, Args[1], Limit);
		UE_LOG(LogAILiveMemory, Display,
			TEXT("SearchHistory(keyword='%s', viewer='%s', limit=%d) -> %d rows"),
			*Args[0], *Args[1], Limit, Rows.Num());
		for (const FAILiveEvent& E : Rows) LogEventBrief(TEXT("SearchHistory"), E);
	}));

static FAutoConsoleCommand GAILiveTestListMyPendingIntendedCmd(
	TEXT("AILive.Test.ListMyPendingIntended"),
	TEXT("AILive.Test.ListMyPendingIntended <actor> <recentN> — list speech.intended without speech.public child; "
	     "events-table-only, does NOT depend on agent_view_state projection"),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
	{
		if (Args.Num() < 2)
		{
			UE_LOG(LogAILiveMemory, Error, TEXT("Usage: AILive.Test.ListMyPendingIntended <actor> <recentN>"));
			return;
		}
		UAILiveEventStoreSubsystem* Sys = GetSubsystemForConsole();
		if (!Sys || !Sys->IsGameOpen())
		{
			UE_LOG(LogAILiveMemory, Error, TEXT("ListMyPendingIntended: no game open"));
			return;
		}
		const int32 N = FCString::Atoi(*Args[1]);
		const TArray<FAILiveEvent> Rows = Sys->ListMyPendingIntended(Args[0], N);
		UE_LOG(LogAILiveMemory, Display,
			TEXT("ListMyPendingIntended(actor=%s, recentN=%d) -> %d rows"),
			*Args[0], N, Rows.Num());
		for (const FAILiveEvent& E : Rows) LogEventBrief(TEXT("PendingIntended"), E);
	}));

static FAutoConsoleCommand GAILiveTestQuoteByEventTypeAndTickCmd(
	TEXT("AILive.Test.QuoteByEventTypeAndTick"),
	TEXT("AILive.Test.QuoteByEventTypeAndTick <event_type_str> <tick_no> — internal slice by tick (no viewer filter)"),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
	{
		if (Args.Num() < 2)
		{
			UE_LOG(LogAILiveMemory, Error,
				TEXT("Usage: AILive.Test.QuoteByEventTypeAndTick <event_type_str> <tick_no> "
				     "(e.g. 'speech.intended' 0)"));
			return;
		}
		UAILiveEventStoreSubsystem* Sys = GetSubsystemForConsole();
		if (!Sys || !Sys->IsGameOpen())
		{
			UE_LOG(LogAILiveMemory, Error, TEXT("QuoteByEventTypeAndTick: no game open"));
			return;
		}
		const EAILiveEventType T = AILiveEvent::EventTypeFromString(Args[0]);
		const int64 TickNo = FCString::Atoi64(*Args[1]);
		const TArray<FAILiveEvent> Rows = Sys->QuoteByEventTypeAndTick(T, TickNo);
		UE_LOG(LogAILiveMemory, Display,
			TEXT("QuoteByEventTypeAndTick(type='%s', tick_no=%lld) -> %d rows"),
			*Args[0], TickNo, Rows.Num());
		for (const FAILiveEvent& E : Rows) LogEventBrief(TEXT("ByEventTypeAndTick"), E);
	}));

static FAutoConsoleCommand GAILiveTestListMyStatementsCmd(
	TEXT("AILive.Test.ListMyStatements"),
	TEXT("AILive.Test.ListMyStatements <agentId> — list speech.public + private_msg of <agentId>; "
	     "validates principles 硬约束 2 (self-utterance full traceability)"),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
	{
		if (Args.Num() < 1)
		{
			UE_LOG(LogAILiveMemory, Error, TEXT("Usage: AILive.Test.ListMyStatements <agentId>"));
			return;
		}
		UAILiveEventStoreSubsystem* Sys = GetSubsystemForConsole();
		if (!Sys || !Sys->IsGameOpen())
		{
			UE_LOG(LogAILiveMemory, Error, TEXT("ListMyStatements: no game open"));
			return;
		}
		const TArray<FAILiveEvent> Rows = Sys->ListMyStatements(Args[0]);
		UE_LOG(LogAILiveMemory, Display,
			TEXT("ListMyStatements(agentId=%s) -> %d rows"), *Args[0], Rows.Num());
		for (const FAILiveEvent& E : Rows) LogEventBrief(TEXT("MyStatements"), E);
	}));

static FAutoConsoleCommand GAILiveTestListVotesCmd(
	TEXT("AILive.Test.ListVotes"),
	TEXT("AILive.Test.ListVotes <round> — list public vote events of round; queries events table directly, "
	     "returns full FAILiveEvent (not vote_history projection synthesis)"),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
	{
		if (Args.Num() < 1)
		{
			UE_LOG(LogAILiveMemory, Error, TEXT("Usage: AILive.Test.ListVotes <round>"));
			return;
		}
		UAILiveEventStoreSubsystem* Sys = GetSubsystemForConsole();
		if (!Sys || !Sys->IsGameOpen())
		{
			UE_LOG(LogAILiveMemory, Error, TEXT("ListVotes: no game open"));
			return;
		}
		const int32 R = FCString::Atoi(*Args[0]);
		const TArray<FAILiveEvent> Rows = Sys->ListVotes(R);
		UE_LOG(LogAILiveMemory, Display,
			TEXT("ListVotes(round=%d) -> %d rows"), R, Rows.Num());
		for (const FAILiveEvent& E : Rows) LogEventBrief(TEXT("Votes"), E);
	}));

static FAutoConsoleCommand GAILiveTestExecDebugSqlCmd(
	TEXT("AILive.Test.ExecDebugSql"),
	TEXT("AILive.Test.ExecDebugSql <raw_sql...> — debug-only: run raw SQL on main game-db connection. "
	     "Used by T4 acceptance to DROP TABLE agent_view_state and re-run ListMyPendingIntended"),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
	{
		if (Args.Num() < 1)
		{
			UE_LOG(LogAILiveMemory, Error, TEXT("Usage: AILive.Test.ExecDebugSql <raw_sql...>"));
			return;
		}
		UAILiveEventStoreSubsystem* Sys = GetSubsystemForConsole();
		if (!Sys || !Sys->IsGameOpen())
		{
			UE_LOG(LogAILiveMemory, Error, TEXT("ExecDebugSql: no game open"));
			return;
		}
		const FString Sql = FString::Join(Args, TEXT(" "));
		FString Err;
		const bool bOk = Sys->ExecuteDebugSqlOnMainConnection(Sql, Err);
		UE_LOG(LogAILiveMemory, Display,
			TEXT("ExecDebugSql(%s) -> %s%s%s"),
			*Sql, bOk ? TEXT("OK") : TEXT("FAIL"),
			bOk ? TEXT("") : TEXT(" err="),
			bOk ? TEXT("") : *Err);
	}));
