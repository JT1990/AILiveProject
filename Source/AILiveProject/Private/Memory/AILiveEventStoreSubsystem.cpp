// =============================================================================
// 中文教学：AILiveEventStoreSubsystem.cpp —— 4600+ 行的事件存储实现
//
// 这是项目最大的单文件。不要试图一口气读完——按下面阅读路径分段啃。
//
// 文件结构（按行号粗略分段）：
//   1)   ~1-200   ：includes + 常量 + canonical JSON 编码（hash 链基础）
//   2)  ~200-400  ：UUIDv7 生成 + canonical JSON 工具
//   3)  ~400-700  ：Initialize / Deinitialize / BeginGame / EndGame / Tick
//   4)  ~700-1100 ：ApplyPragmas / EnsureSchema / RunMigrations
//   5) ~1100-1600 ：AppendEvent / AppendEventsAtomically / 校验
//   6) ~1600-2200 ：InsertEventBypassValidation_LockHeld / ComputeEventHash
//   7) ~2200-2800 ：Quote / QuoteByRound / QuoteRecentRounds / List* 读 API
//   8) ~2800-3400 ：T7 Bid 协议 + ResolveFloor + RuntimeAdj
//   9) ~3400-4000 ：T8 Projector：Project*_LockHeld 系列 + RebuildProjections
//  10) ~4000-4649 ：T9 Resume + TriggerDeleteExecuted + Console 命令
//
// 阅读路径建议（C++ 小白先看这几个函数，看完就懂 80%）：
//
//   ① BeginGame / EndGame：理解 SQLite 连接与 schema 怎么挂上来
//   ② AppendEvent → AppendEventsAtomically → InsertEventBypassValidation_LockHeld：理解写入流水线（校验 → 锁 → 事务 → hash 链 → INSERT）
//   ③ CanonicalJsonOf + ComputeEventHash：理解 hash 链怎么算
//   ④ Quote：理解视角隔离（visibility EXISTS 的 SQL 模式）
//   ⑤ RebuildProjections：理解 projector 怎么从 events 重建派生表
//
// 关键 UE / C++ 概念（贯穿全文件）：
//
//   1) FSQLitePreparedStatement
//      预编译语句 = 把 SQL 字符串提前编译，避免每次执行都要解析。
//      流程：Create(Db, "SELECT ... WHERE x=?1") → SetBindingValueByIndex(1, value)
//      → Step() 取一行 → GetColumnValueByIndex(N, OutValue)。
//      Step 返回 ESQLitePreparedStatementStepResult：Row（有数据）/ Done（结束）/ Error。
//      ⚠️ 必须用「预编译 + 参数绑定」防 SQL 注入；绝不要字符串拼接 SQL！
//
//   2) BEGIN IMMEDIATE / COMMIT / ROLLBACK
//      显式事务。本文件的写流程都是：
//        BEGIN IMMEDIATE; → 多条 INSERT → COMMIT;  失败则 ROLLBACK;
//      `IMMEDIATE` 比默认 `BEGIN`（DEFERRED）更早申请 RESERVED 锁，
//      避免后续升级为 EXCLUSIVE 时死锁。
//
//   3) FScopeLock + WriteMutex
//      写流程统一在 WriteMutex 下进行，避免多线程同时 BEGIN 导致 SQLite busy 错误。
//      `*_LockHeld` 后缀的函数表示「调用方必须已经持锁」（不能自己再 lock）。
//
//   4) Canonical JSON
//      Hash 链要求两台机器算出来的 hash 字节级相同。所以序列化时必须：
//        - 字段按字典序固定顺序
//        - 数字格式固定（不能让 1.0 vs 1 出现差异）
//        - 不能含运行时变化的字段（tick_no 故意不入 canonical，因为它是写入时才填）
//
//   5) UUIDv7
//      时间排序的 UUID 变种（前 48 bit 是毫秒时间戳）。事件 EventId 用它，
//      让按字符串排序也能近似按时间排序，调试方便。
//
//   6) WAL 模式 + 二次连接限制（项目实测）
//      项目实测下 UE FSQLiteCore + WAL 模式开第二个连接做某些读取会触发
//      IOERR（未深究是 plugin 层还是 -shm 共享映射限制；通用 SQLite 不一定
//      有这条限制）。所以 SchemaCheck 等读取统一走主连接 Db ——
//      QueryMetaSchemaRegistry / RecomputeHashChainOnMainConnection 都是
//      为了规避这条限制设计的「内联读」方法。
// =============================================================================

#include "Memory/AILiveEventStoreSubsystem.h"

#include "LLM/AILiveParserVersion.h"
#include "Memory/AILiveAgentRegistry.h"
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
	bool ApplyMigrationV0ToV1(FSQLiteDatabase &InDb, bool bIsMetaDb, const TCHAR *FtsTokenizer);
}

const TCHAR *const UAILiveEventStoreSubsystem::kGenesisHash =
	TEXT("0000000000000000000000000000000000000000000000000000000000000000");

namespace
{

	/** 生成单局游戏数据库路径：Saved/Games/<game_id>.db。 */
	FString MakeGameDbPath(const FString &InGameId)
	{
		// ProjectSavedDir() 是 UE 的项目 Saved 目录；用 / 拼路径会自动处理分隔符。
		const FString Dir = FPaths::ProjectSavedDir() / TEXT("Games");
		return Dir / (InGameId + TEXT(".db"));
	}

	/** 生成跨局共享的 meta 数据库路径：Saved/Games/_meta.db。 */
	FString MakeMetaDbPath()
	{
		const FString Dir = FPaths::ProjectSavedDir() / TEXT("Games");
		return Dir / TEXT("_meta.db");
	}

	/** 确保 Saved/Games 目录存在；Tree=true 表示父目录不存在时一起创建。 */
	bool EnsureSavedGamesDir()
	{
		const FString Dir = FPaths::ProjectSavedDir() / TEXT("Games");
		return IFileManager::Get().MakeDirectory(*Dir, /*Tree=*/true);
	}

	/** 为 console 命令寻找当前可用的 PIE/Game World，找不到再退回 GWorld。 */
	UWorld *GetActiveWorldForConsole()
	{
		if (GEngine)
		{
			for (const FWorldContext &Ctx : GEngine->GetWorldContexts())
			{
				// Console 命令可能在编辑器里运行；这里只接受真正可执行业务逻辑的 World。
				if ((Ctx.WorldType == EWorldType::PIE || Ctx.WorldType == EWorldType::Game) && Ctx.World())
				{
					return Ctx.World();
				}
			}
		}
		return GWorld;
	}

	/** Console 命令入口共用：从当前 World 的 GameInstance 上取 EventStore 子系统。 */
	UAILiveEventStoreSubsystem *GetSubsystemForConsole()
	{
		UWorld *World = GetActiveWorldForConsole();
		if (!World)
		{
			UE_LOG(LogAILiveMemory, Error, TEXT("Console: no active PIE/Game world"));
			return nullptr;
		}
		UGameInstance *GI = World->GetGameInstance();
		if (!GI)
		{
			// 没有 GameInstance 时，Subsystem 生命周期还不存在，后续 DB 操作没有宿主。
			UE_LOG(LogAILiveMemory, Error, TEXT("Console: world has no GameInstance"));
			return nullptr;
		}
		UAILiveEventStoreSubsystem *Sys = GI->GetSubsystem<UAILiveEventStoreSubsystem>();
		if (!Sys)
		{
			UE_LOG(LogAILiveMemory, Error, TEXT("Console: UAILiveEventStoreSubsystem not found on GameInstance"));
		}
		return Sys;
	}

	/** 运行一条调试 SELECT，并按列数把每行拼成日志输出。 */
	void LogQueryRows(FSQLiteDatabase &InDb, const TCHAR *InSql, const TCHAR *InLabel, int32 InColumnCount)
	{
		UE_LOG(LogAILiveMemory, Display, TEXT("[%s] %s"), InLabel, InSql);
		const int64 Rows = InDb.Execute(InSql, [InLabel, InColumnCount](const FSQLitePreparedStatement &Stmt)
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
		return ESQLitePreparedStatementExecuteRowResult::Continue; });
		UE_LOG(LogAILiveMemory, Display, TEXT("  [%s] -> %lld rows"), InLabel, Rows);
	}

	// ===== T3 helpers =====

	/** Closed-set viewer namespace (schema.yaml header). "self" forbidden. */
	bool IsValidViewerString(const FString &V)
	{
		if (V == TEXT("public") || V == TEXT("audience") ||
			V == TEXT("orchestrator") || V == TEXT("system"))
		{
			return true;
		}
		if (V.StartsWith(TEXT("NPC")) && V.Len() >= 4 && V.Len() <= 6)
		{
			for (int32 i = 3; i < V.Len(); ++i)
			{
				if (!FChar::IsDigit(V[i]))
					return false;
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
			// Canonical JSON 要保持 1.0 的语义，不让它被普通 JSON writer 压成 1。
			S += TEXT(".0");
		}
		return S;
	}

	/** Recursively emit a JSON value with deterministic ordering. */
	void WriteCanonicalValue(const TSharedPtr<FJsonValue> &V, FCanonicalWriter &W)
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
			for (const TSharedPtr<FJsonValue> &Elem : V->AsArray())
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
				// 字段排序是 hash chain 的关键：不同机器/不同 map 遍历顺序必须得到同一串字节。
				Keys.Sort();
				for (const FString &K : Keys)
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

	/** 按固定字段顺序写出一个 JSON object，是 CanonicalJsonOf 的递归 helper。 */
	void WriteCanonicalObject(const TSharedPtr<FJsonObject> &Obj, FCanonicalWriter &W)
	{
		W.WriteObjectStart();
		if (Obj.IsValid())
		{
			TArray<FString> Keys;
			Obj->Values.GetKeys(Keys);
			// object 内部也必须排序，避免 payload 字段顺序影响 event_hash。
			Keys.Sort();
			for (const FString &K : Keys)
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
	const TCHAR *const kEventSelectColumns =
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
	TArray<FString> ExpandViewerForJoin(const FString &InViewer)
	{
		TArray<FString> Out;

		if (InViewer.IsEmpty() || InViewer == TEXT("self"))
		{
			UE_LOG(LogAILiveMemory, Warning,
				   TEXT("ExpandViewerForJoin: rejected viewer='%s' "
						"('self' / empty 一律返回不可见——必须传具体 actor ID)"),
				   *InViewer);
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
		if (Count <= 0)
			return FString();
		FString Out;
		Out.Reserve(Count * 2);
		for (int32 i = 0; i < Count; ++i)
		{
			if (i > 0)
				Out.Append(TEXT(","));
			Out.Append(TEXT("?"));
		}
		return Out;
	}

	/**
	 * 转义 LIKE 模式里的元字符：% / _ / \ → \% / \_ / \\。配套 SQL `ESCAPE '\\'`。
	 * 防止用户输入 `100%` 这类内容被当成 SQL 通配符误匹配。
	 */
	FString EscapeLikePattern(const FString &InKeyword)
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
	FAILiveEvent RowToEvent(const FSQLitePreparedStatement &InStmt)
	{
		FAILiveEvent Ev;
		int64 SeqRead = 0;
		int64 TickNoRead = 0;
		int64 RoundNoRead = 0;
		FString PhaseStr, EventTypeStr, SpeechActStr, VisibilityJson, AddressedToJson;

		InStmt.GetColumnValueByIndex(0, Ev.EventId);
		InStmt.GetColumnValueByIndex(1, Ev.GameId);
		InStmt.GetColumnValueByIndex(2, SeqRead);
		Ev.Seq = SeqRead;
		InStmt.GetColumnValueByIndex(3, TickNoRead);
		Ev.TickNo = TickNoRead;
		InStmt.GetColumnValueByIndex(4, RoundNoRead);
		Ev.RoundNo = (int32)RoundNoRead;
		InStmt.GetColumnValueByIndex(5, PhaseStr);
		Ev.Phase = AILiveEvent::PhaseFromString(PhaseStr);
		InStmt.GetColumnValueByIndex(6, Ev.Actor);
		InStmt.GetColumnValueByIndex(7, EventTypeStr);
		Ev.EventType = AILiveEvent::EventTypeFromString(EventTypeStr);
		InStmt.GetColumnValueByIndex(8, SpeechActStr);
		Ev.SpeechActType = AILiveEvent::SpeechActFromString(SpeechActStr);
		InStmt.GetColumnValueByIndex(9, VisibilityJson);
		Ev.Visibility = AILiveEvent::JsonStringToArray(VisibilityJson);
		InStmt.GetColumnValueByIndex(10, AddressedToJson);
		Ev.AddressedTo = AILiveEvent::JsonStringToArray(AddressedToJson);
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

/** UE 子系统初始化钩子：这里只记录日志，真正的 DB 打开由 BeginGame 触发。 */
void UAILiveEventStoreSubsystem::Initialize(FSubsystemCollectionBase &Collection)
{
	Super::Initialize(Collection);
	UE_LOG(LogAILiveMemory, Log, TEXT("EventStoreSubsystem initialized"));
}

/** UE 子系统销毁钩子：如果游戏 DB 还开着，先走 EndGame 做成对清理。 */
void UAILiveEventStoreSubsystem::Deinitialize()
{
	if (IsGameOpen())
	{
		// 避免编辑器停止 PIE 时 SQLite 连接仍持有 WAL/SHM 文件句柄。
		EndGame();
	}
	Super::Deinitialize();
}

/** 打开或创建一局游戏的 game.db 与共享 _meta.db，并确保 schema / registry / hash tail 可用。 */
bool UAILiveEventStoreSubsystem::BeginGame(const FString &InGameId)
{
	if (InGameId.IsEmpty())
	{
		UE_LOG(LogAILiveMemory, Error, TEXT("BeginGame: empty game_id"));
		return false;
	}
	if (IsGameOpen())
	{
		// 同一个 subsystem 同时只服务一局；切局前先关闭旧连接。
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
	// 先设置 PRAGMA，再建表/迁移，保证 WAL、外键等行为从初始化阶段就一致。
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

	// CurrentGameId 必须在 LoadHashChainTail 前设置，因为查询用它过滤 events。
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

/** 关闭当前 game/meta 数据库连接，并清掉运行时缓存。 */
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

/** 从 events 表最后一行恢复 hash 链尾；空库则使用 genesis hash。 */
bool UAILiveEventStoreSubsystem::LoadHashChainTail()
{
	// 默认值代表一条事件都没有：seq=0，prev hash 为 64 个 0。
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
		// 找到历史事件时，用最后一条事件作为下一次 append 的前驱。
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

/** 对一个 SQLite 连接应用本项目固定 PRAGMA 设置。 */
void UAILiveEventStoreSubsystem::ApplyPragmas(FSQLiteDatabase &InDb)
{
	// journal_mode=WAL is database-level (persisted in the .db header) but
	// repeating it is idempotent and corrects any non-WAL legacy file.
	const TCHAR *Pragmas[] =
		{
			TEXT("PRAGMA journal_mode = WAL;"),
			TEXT("PRAGMA synchronous = NORMAL;"),
			TEXT("PRAGMA foreign_keys = ON;"),
			TEXT("PRAGMA temp_store = MEMORY;"),
			TEXT("PRAGMA mmap_size = 268435456;"),
		};
	for (const TCHAR *P : Pragmas)
	{
		if (!InDb.Execute(P))
		{
			UE_LOG(LogAILiveMemory, Warning, TEXT("PRAGMA failed (%s): %s"), P, *InDb.GetLastError());
		}
	}
}

/** 探测当前 SQLite 是否支持 FTS5 trigram tokenizer，不支持则回退 unicode61。 */
FString UAILiveEventStoreSubsystem::DetectFtsTokenizer(FSQLiteDatabase &InDb)
{
	// 用 temp 表做探测，不污染正式 schema。
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

/** 确保 game/meta DB schema 升级到当前版本，并缓存 game DB 的 FTS tokenizer。 */
bool UAILiveEventStoreSubsystem::EnsureSchema(FSQLiteDatabase &InDb, bool bIsMetaDb)
{
	int32 ExistingVersion = 0;
	bool bSchemaMetaPresent = false;

	{
		// Probe schema_meta existence; empty result = fresh DB.
		const int64 Rows = InDb.Execute(
			TEXT("SELECT name FROM sqlite_master WHERE type='table' AND name='schema_meta';"),
			[&bSchemaMetaPresent](const FSQLitePreparedStatement &)
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
		// schema_meta 存在时读取版本；不存在则保持 0，走首次迁移。
		const int64 Rows = InDb.Execute(
			TEXT("SELECT value FROM schema_meta WHERE key='schema_version';"),
			[&ExistingVersion](const FSQLitePreparedStatement &Stmt)
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
			[&ExistingFtsSql](const FSQLitePreparedStatement &Stmt)
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
		// 代码版本低于 DB 版本时拒绝打开，防止旧代码误读新 schema。
		UE_LOG(LogAILiveMemory, Error,
			   TEXT("EnsureSchema(%s): existing schema_version=%d > supported=%d (downgrade refused)"),
			   bIsMetaDb ? TEXT("meta") : TEXT("game"), ExistingVersion, kCurrentSchemaVersion);
		return false;
	}

	const TCHAR *TokenizerPtr = bIsMetaDb ? nullptr : *DetectedFtsTokenizer;
	return RunMigrations(InDb, ExistingVersion, kCurrentSchemaVersion, bIsMetaDb, TokenizerPtr);
}

/** 按版本号分派 schema 迁移；当前只支持 0 -> 1。 */
bool UAILiveEventStoreSubsystem::RunMigrations(FSQLiteDatabase &InDb, int32 FromVersion, int32 ToVersion, bool bIsMetaDb, const TCHAR *FtsTokenizer)
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

/** 读取 _meta.db.schema_meta 的全部 key/value，用于调试和写入时动态解析 parser_version。 */
bool UAILiveEventStoreSubsystem::QueryMetaSchemaRegistry(TMap<FString, FString> &OutKVs)
{
	OutKVs.Reset();
	if (!MetaDb.IsValid())
	{
		UE_LOG(LogAILiveMemory, Error, TEXT("QueryMetaSchemaRegistry: MetaDb 未打开（先 BeginGame）"));
		return false;
	}
	const int64 Rows = MetaDb.Execute(
		TEXT("SELECT key, value FROM schema_meta;"),
		[&OutKVs](const FSQLitePreparedStatement &Stmt)
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

/** 把当前 parser registry 信息写入 _meta.db，保证后续事件可追溯 parser 版本。 */
bool UAILiveEventStoreSubsystem::EnsureMetaRegistry(FSQLiteDatabase &InMetaDb)
{
	const FString RegistryDir = AILiveParser::GetCurrentParserPromptRegistryDir();
	const FString ParserVersion = AILiveParser::GetCurrentParserVersion();
	const FString ParserModel = AILiveParser::GetCurrentParserModel();

	if (RegistryDir.IsEmpty() || ParserVersion.IsEmpty() || ParserModel.IsEmpty())
	{
		UE_LOG(LogAILiveMemory, Error,
			   TEXT("EnsureMetaRegistry: missing registry field(s) (dir='%s' ver='%s' model='%s')"),
			   *RegistryDir, *ParserVersion, *ParserModel);
		return false;
	}

	const TPair<FString, FString> Pairs[] = {
		{TEXT("parser_prompt_registry_path"), RegistryDir},
		{TEXT("parser_version"), ParserVersion},
		{TEXT("parser_model"), ParserModel},
	};

	const TCHAR *Sql = TEXT("INSERT OR REPLACE INTO schema_meta(key, value) VALUES(?1, ?2);");

	for (const TPair<FString, FString> &KV : Pairs)
	{
		// 每个 key 独立 upsert；任一失败即认为 meta registry 不可信。
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

/** 查询某个 actor 自己可见的全部事件，主要用于按 actor 回放历史。 */
TArray<FAILiveEvent> UAILiveEventStoreSubsystem::QueryEventsByActor(const FString &Actor, int32 LimitCount) const
{
	TArray<FAILiveEvent> Out;
	if (!IsGameOpen() || Actor.IsEmpty())
		return Out;

	// self-viewer JOIN：viewer = actor 自身。task card D4 要求所有读路径走 JOIN。
	const TArray<FString> Viewers = ExpandViewerForJoin(Actor);
	if (Viewers.Num() == 0)
		return Out;

	// IN (?, ?, ...) 的数量取决于 viewer 展开结果，所以 SQL 字符串需动态生成。
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
	if (!Stmt.Create(const_cast<FSQLiteDatabase &>(Db), *Sql))
	{
		UE_LOG(LogAILiveMemory, Error, TEXT("QueryEventsByActor: prepare failed: %s"), *Db.GetLastError());
		return Out;
	}
	int32 BindIdx = 1;
	Stmt.SetBindingValueByIndex(BindIdx++, CurrentGameId);
	Stmt.SetBindingValueByIndex(BindIdx++, Actor);
	for (const FString &V : Viewers)
	{
		// 顺序必须和 SQL 里的占位符一致：game_id、actor、viewer...、limit。
		Stmt.SetBindingValueByIndex(BindIdx++, V);
	}
	if (LimitCount > 0)
		Stmt.SetBindingValueByIndex(BindIdx++, (int64)LimitCount);

	while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		Out.Add(RowToEvent(Stmt));
	}
	return Out;
}

/** 重新遍历 events 并验证 hash chain，失败时返回第一条坏链的 seq。 */
bool UAILiveEventStoreSubsystem::VerifyHashChain(int64 &OutFirstBadSeq) const
{
	OutFirstBadSeq = -1;
	if (!IsGameOpen())
	{
		UE_LOG(LogAILiveMemory, Error, TEXT("VerifyHashChain: no game open"));
		return false;
	}
	int64 LastSeq = 0;
	int64 BadCount = 0;
	int64 FirstBadSeq = -1;
	// RecomputeHashChainOnMainConnection 不是 const；这里是只读审计，安全地复用实现。
	UAILiveEventStoreSubsystem *Self = const_cast<UAILiveEventStoreSubsystem *>(this);
	if (!Self->RecomputeHashChainOnMainConnection(LastSeq, BadCount, FirstBadSeq))
	{
		UE_LOG(LogAILiveMemory, Error,
			   TEXT("VerifyHashChain: walk failed (game=%s)"), *CurrentGameId);
		return false;
	}
	if (BadCount > 0)
	{
		OutFirstBadSeq = FirstBadSeq;
		UE_LOG(LogAILiveMemory, Error,
			   TEXT("VerifyHashChain: chain BAD bad_count=%lld first_bad_seq=%lld last_seq=%lld"),
			   BadCount, FirstBadSeq, LastSeq);
		return false;
	}
	UE_LOG(LogAILiveMemory, Display,
		   TEXT("VerifyHashChain OK (game=%s, last_seq=%lld)"), *CurrentGameId, LastSeq);
	return true;
}

/** 按 seq 取单条事件，并强制检查该 viewer 是否可见。 */
bool UAILiveEventStoreSubsystem::Quote(int64 InSeq, const FString &InViewer, FAILiveEvent &OutEvent) const
{
	OutEvent = FAILiveEvent();
	if (!IsGameOpen())
		return false;

	const TArray<FString> Viewers = ExpandViewerForJoin(InViewer);
	if (Viewers.Num() == 0)
		return false;

	// EXISTS 子查询是视角隔离的核心：没有可见行就像事件不存在一样返回 false。
	const FString Sql = FString::Printf(
		TEXT("SELECT %s FROM events e "
			 "WHERE e.game_id = ?1 AND e.seq = ?2 "
			 "  AND EXISTS (SELECT 1 FROM event_visibility v "
			 "              WHERE v.event_id = e.event_id AND v.viewer IN (%s)) "
			 "LIMIT 1;"),
		kEventSelectColumns,
		*MakeViewerInPlaceholders(Viewers.Num()));

	FSQLitePreparedStatement Stmt;
	if (!Stmt.Create(const_cast<FSQLiteDatabase &>(Db), *Sql))
	{
		UE_LOG(LogAILiveMemory, Error, TEXT("Quote: prepare failed: %s"), *Db.GetLastError());
		return false;
	}
	int32 BindIdx = 1;
	Stmt.SetBindingValueByIndex(BindIdx++, CurrentGameId);
	Stmt.SetBindingValueByIndex(BindIdx++, InSeq);
	for (const FString &V : Viewers)
		Stmt.SetBindingValueByIndex(BindIdx++, V);

	if (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		OutEvent = RowToEvent(Stmt);
		return true;
	}
	return false;
}

/** 按 round + actor 查询该 viewer 可见的事件列表。 */
TArray<FAILiveEvent> UAILiveEventStoreSubsystem::QuoteByRound(int32 InRoundNo, const FString &InActor,
															  const FString &InViewer) const
{
	TArray<FAILiveEvent> Out;
	if (!IsGameOpen())
		return Out;

	const TArray<FString> Viewers = ExpandViewerForJoin(InViewer);
	if (Viewers.Num() == 0)
		return Out;

	const FString Sql = FString::Printf(
		TEXT("SELECT %s FROM events e "
			 "WHERE e.game_id = ?1 AND e.round_no = ?2 AND e.actor = ?3 "
			 "  AND EXISTS (SELECT 1 FROM event_visibility v "
			 "              WHERE v.event_id = e.event_id AND v.viewer IN (%s)) "
			 "ORDER BY e.seq;"),
		kEventSelectColumns,
		*MakeViewerInPlaceholders(Viewers.Num()));

	FSQLitePreparedStatement Stmt;
	if (!Stmt.Create(const_cast<FSQLiteDatabase &>(Db), *Sql))
	{
		UE_LOG(LogAILiveMemory, Error, TEXT("QuoteByRound: prepare failed: %s"), *Db.GetLastError());
		return Out;
	}
	int32 BindIdx = 1;
	Stmt.SetBindingValueByIndex(BindIdx++, CurrentGameId);
	Stmt.SetBindingValueByIndex(BindIdx++, (int64)InRoundNo);
	Stmt.SetBindingValueByIndex(BindIdx++, InActor);
	for (const FString &V : Viewers)
		Stmt.SetBindingValueByIndex(BindIdx++, V);

	while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		Out.Add(RowToEvent(Stmt));
	}
	return Out;
}

/** 查询最近 K 轮内该 viewer 可见的所有事件，用于构建上下文窗口。 */
TArray<FAILiveEvent> UAILiveEventStoreSubsystem::QuoteRecentRounds(int32 InCurrentRound, int32 InK,
																   const FString &InViewer) const
{
	TArray<FAILiveEvent> Out;
	if (!IsGameOpen() || InK <= 0)
		return Out;

	const TArray<FString> Viewers = ExpandViewerForJoin(InViewer);
	if (Viewers.Num() == 0)
		return Out;

	// K 轮窗口包含当前轮，所以起点是 current - K + 1，且不能低于 0。
	const int32 RoundStart = FMath::Max(0, InCurrentRound - InK + 1);
	const int32 RoundEnd = InCurrentRound;

	const FString Sql = FString::Printf(
		TEXT("SELECT %s FROM events e "
			 "WHERE e.game_id = ?1 AND e.round_no BETWEEN ?2 AND ?3 "
			 "  AND EXISTS (SELECT 1 FROM event_visibility v "
			 "              WHERE v.event_id = e.event_id AND v.viewer IN (%s)) "
			 "ORDER BY e.seq;"),
		kEventSelectColumns,
		*MakeViewerInPlaceholders(Viewers.Num()));

	FSQLitePreparedStatement Stmt;
	if (!Stmt.Create(const_cast<FSQLiteDatabase &>(Db), *Sql))
	{
		UE_LOG(LogAILiveMemory, Error, TEXT("QuoteRecentRounds: prepare failed: %s"), *Db.GetLastError());
		return Out;
	}
	int32 BindIdx = 1;
	Stmt.SetBindingValueByIndex(BindIdx++, CurrentGameId);
	Stmt.SetBindingValueByIndex(BindIdx++, (int64)RoundStart);
	Stmt.SetBindingValueByIndex(BindIdx++, (int64)RoundEnd);
	for (const FString &V : Viewers)
		Stmt.SetBindingValueByIndex(BindIdx++, V);

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
		const FSQLiteDatabase &Db, const FString &GameId,
		const FString &AgentId, const FString &EventTypeStr,
		const TArray<FString> &Viewers,
		bool bDescByRound, int32 LimitCount)
	{
		TArray<FAILiveEvent> Out;

		// 调用者决定是按 seq 正序回放，还是按 round/seq 倒序取最近 N 条。
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
		if (!Stmt.Create(const_cast<FSQLiteDatabase &>(Db), *Sql))
		{
			UE_LOG(LogAILiveMemory, Error, TEXT("ListSelfEventsByType: prepare failed: %s"), *Db.GetLastError());
			return Out;
		}
		int32 BindIdx = 1;
		Stmt.SetBindingValueByIndex(BindIdx++, GameId);
		Stmt.SetBindingValueByIndex(BindIdx++, AgentId);
		Stmt.SetBindingValueByIndex(BindIdx++, EventTypeStr);
		for (const FString &V : Viewers)
			Stmt.SetBindingValueByIndex(BindIdx++, V);
		if (LimitCount > 0)
			Stmt.SetBindingValueByIndex(BindIdx++, (int64)LimitCount);

		while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
		{
			Out.Add(RowToEvent(Stmt));
		}
		return Out;
	}

} // namespace anonymous

/** 列出 agent 自己说过的公开发言和私信，用于“自我发言不可压缩”上下文。 */
TArray<FAILiveEvent> UAILiveEventStoreSubsystem::ListMyStatements(const FString &InAgentId) const
{
	TArray<FAILiveEvent> Out;
	if (!IsGameOpen())
		return Out;
	const TArray<FString> Viewers = ExpandViewerForJoin(InAgentId);
	if (Viewers.Num() == 0)
		return Out;

	// "自我发言全量追溯"（principles 硬约束 2）含 speech.public + private_msg 两通道——
	// 协议层把这二者都视为 agent 发出的"统辞"，T6 PromptAssembler 的"自我发言不可压缩段"
	// 也按这个集合组装。speech.intended/note/reflection 走各自的 ListMy* API。
	// 注：private_msg 能否被 actor 自己 quote 取决于写入侧把 actor 自身放进 visibility
	// （T5/T7 写入侧纪律）；read API 不做旁路，全部走 JOIN。
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
	if (!Stmt.Create(const_cast<FSQLiteDatabase &>(Db), *Sql))
	{
		UE_LOG(LogAILiveMemory, Error, TEXT("ListMyStatements: prepare failed: %s"), *Db.GetLastError());
		return Out;
	}
	int32 BindIdx = 1;
	Stmt.SetBindingValueByIndex(BindIdx++, CurrentGameId);
	Stmt.SetBindingValueByIndex(BindIdx++, InAgentId);
	for (const FString &V : Viewers)
		Stmt.SetBindingValueByIndex(BindIdx++, V);

	while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		Out.Add(RowToEvent(Stmt));
	}
	return Out;
}

/** 列出 agent 最近的 speech.note 事件。 */
TArray<FAILiveEvent> UAILiveEventStoreSubsystem::ListMyNotes(const FString &InAgentId, int32 InRecentN) const
{
	if (!IsGameOpen())
		return {};
	const TArray<FString> Viewers = ExpandViewerForJoin(InAgentId);
	if (Viewers.Num() == 0)
		return {};
	return ListSelfEventsByType(Db, CurrentGameId, InAgentId,
								AILiveEvent::EventTypeToString(EAILiveEventType::SpeechNote),
								Viewers, /*bDescByRound=*/true, /*LimitCount=*/InRecentN);
}

/** 列出 agent 最近的 reflection.9q 事件。 */
TArray<FAILiveEvent> UAILiveEventStoreSubsystem::ListMyReflections(const FString &InAgentId, int32 InRecentN) const
{
	if (!IsGameOpen())
		return {};
	const TArray<FString> Viewers = ExpandViewerForJoin(InAgentId);
	if (Viewers.Num() == 0)
		return {};
	return ListSelfEventsByType(Db, CurrentGameId, InAgentId,
								AILiveEvent::EventTypeToString(EAILiveEventType::Reflection9Q),
								Viewers, /*bDescByRound=*/true, /*LimitCount=*/InRecentN);
}

/** 列出还没有对应 speech.public 子事件的 speech.intended。 */
TArray<FAILiveEvent> UAILiveEventStoreSubsystem::ListMyPendingIntended(const FString &InAgentId, int32 InRecentN) const
{
	TArray<FAILiveEvent> Out;
	if (!IsGameOpen())
		return Out;

	const TArray<FString> Viewers = ExpandViewerForJoin(InAgentId);
	if (Viewers.Num() == 0)
		return Out;

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
	if (!Stmt.Create(const_cast<FSQLiteDatabase &>(Db), *Sql))
	{
		UE_LOG(LogAILiveMemory, Error, TEXT("ListMyPendingIntended: prepare failed: %s"), *Db.GetLastError());
		return Out;
	}
	int32 BindIdx = 1;
	Stmt.SetBindingValueByIndex(BindIdx++, CurrentGameId);
	Stmt.SetBindingValueByIndex(BindIdx++, InAgentId);
	for (const FString &V : Viewers)
		Stmt.SetBindingValueByIndex(BindIdx++, V);
	if (InRecentN > 0)
		Stmt.SetBindingValueByIndex(BindIdx++, (int64)InRecentN);

	while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		Out.Add(RowToEvent(Stmt));
	}
	return Out;
}

/** 从 commitments 投影表读取某 agent 在轮次范围内的承诺状态。 */
TArray<FAILiveCommitment> UAILiveEventStoreSubsystem::ListMyCommitments(const FString &InAgentId,
																		int32 InRoundStart,
																		int32 InRoundEnd) const
{
	TArray<FAILiveCommitment> Out;
	if (!IsGameOpen())
		return Out;

	// 投影表直读（T8 落地后才会有数据；T4 阶段 commitments 表存在但通常为空）。
	const FString Sql =
		TEXT("SELECT game_id, agent_id, round_no, seq, commitment_type, target, text, status "
			 "FROM commitments "
			 "WHERE game_id = ?1 AND agent_id = ?2 "
			 "  AND (?3 < 0 OR round_no >= ?3) "
			 "  AND (?4 < 0 OR round_no <= ?4) "
			 "ORDER BY round_no, seq;");

	FSQLitePreparedStatement Stmt;
	if (!Stmt.Create(const_cast<FSQLiteDatabase &>(Db), *Sql))
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
		Stmt.GetColumnValueByIndex(2, RoundNoRead);
		C.RoundNo = (int32)RoundNoRead;
		Stmt.GetColumnValueByIndex(3, SeqRead);
		C.Seq = SeqRead;
		Stmt.GetColumnValueByIndex(4, TypeStr);
		// DB 存字符串，C++ 结构体存 enum；读取时集中做转换。
		C.CommitmentType = AILiveEvent::CommitmentTypeFromString(TypeStr);
		Stmt.GetColumnValueByIndex(5, C.Target);
		Stmt.GetColumnValueByIndex(6, C.Text);
		Stmt.GetColumnValueByIndex(7, StatusStr);
		C.Status = AILiveEvent::CommitmentStatusFromString(StatusStr);
		Out.Add(C);
	}
	return Out;
}

/** 在历史事件 payload_text 中搜索关键词，并应用 actor/round/viewer 过滤。 */
TArray<FAILiveEvent> UAILiveEventStoreSubsystem::SearchHistory(const FString &InKeyword, const FString &InActor,
															   int32 InRoundStart, int32 InRoundEnd,
															   const FString &InViewer, int32 InLimit) const
{
	TArray<FAILiveEvent> Out;
	if (!IsGameOpen() || InKeyword.IsEmpty())
		return Out;

	const TArray<FString> Viewers = ExpandViewerForJoin(InViewer);
	if (Viewers.Num() == 0)
		return Out;

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
	if (!Stmt.Create(const_cast<FSQLiteDatabase &>(Db), *Sql))
	{
		UE_LOG(LogAILiveMemory, Error, TEXT("SearchHistory: prepare failed: %s"), *Db.GetLastError());
		return Out;
	}
	int32 BindIdx = 1;
	Stmt.SetBindingValueByIndex(BindIdx++, CurrentGameId);
	if (bUseLike)
	{
		// LIKE 路径兼容短中文关键词与 unicode61 fallback，但需要手动 escape 通配符。
		const FString Pattern = TEXT("%") + EscapeLikePattern(InKeyword) + TEXT("%");
		Stmt.SetBindingValueByIndex(BindIdx++, Pattern);
	}
	else
	{
		// trigram 可用时走 FTS5 MATCH，适合更长的模糊搜索关键词。
		Stmt.SetBindingValueByIndex(BindIdx++, InKeyword);
	}
	Stmt.SetBindingValueByIndex(BindIdx++, InActor);
	Stmt.SetBindingValueByIndex(BindIdx++, (int64)InRoundStart);
	Stmt.SetBindingValueByIndex(BindIdx++, (int64)InRoundEnd);
	for (const FString &V : Viewers)
		Stmt.SetBindingValueByIndex(BindIdx++, V);
	Stmt.SetBindingValueByIndex(BindIdx++, (int64)ActualLimit);

	while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		Out.Add(RowToEvent(Stmt));
	}
	return Out;
}

/** 列出某一轮的公开 vote 原始事件，而不是 vote_history 投影行。 */
TArray<FAILiveEvent> UAILiveEventStoreSubsystem::ListVotes(int32 InRoundNo) const
{
	TArray<FAILiveEvent> Out;
	if (!IsGameOpen())
		return Out;

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
	if (!Stmt.Create(const_cast<FSQLiteDatabase &>(Db), *Sql))
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

/** 把 alliance_state 投影表序列化为 JSON 字符串，供 UI/debug 直接消费。 */
FString UAILiveEventStoreSubsystem::ListAllianceStateJson() const
{
	if (!IsGameOpen())
		return TEXT("[]");

	// 投影表直读：alliance_state 全部行 → JSON 数组（T8 落地后才会有数据）。
	const TCHAR *Sql =
		TEXT("SELECT alliance_id, members, proposed_at_seq, accepted_at_seq, "
			 "       betrayed_at_seq, terms FROM alliance_state "
			 "WHERE game_id = ?1;");

	FSQLitePreparedStatement Stmt;
	if (!Stmt.Create(const_cast<FSQLiteDatabase &>(Db), Sql))
	{
		UE_LOG(LogAILiveMemory, Verbose, TEXT("ListAllianceStateJson: prepare failed: %s"), *Db.GetLastError());
		return TEXT("[]");
	}
	Stmt.SetBindingValueByIndex(1, CurrentGameId);

	FString Out;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
	// 这里返回 JSON 数组字符串，而不是 C++ 结构体数组，方便 Blueprint/console 显示。
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

/** 内部查询：按 tick_no + event_type 切片，不做 viewer 过滤，给 orchestrator 逻辑使用。 */
TArray<FAILiveEvent> UAILiveEventStoreSubsystem::QuoteByEventTypeAndTick(
	EAILiveEventType InEventType, int64 InTickNo) const
{
	TArray<FAILiveEvent> Out;
	if (!IsGameOpen())
		return Out;

	// 内部用——orchestrator 自身调用，不做 viewer 过滤。
	const FString EventTypeStr = AILiveEvent::EventTypeToString(InEventType);
	const FString Sql = FString::Printf(
		TEXT("SELECT %s FROM events e "
			 "WHERE e.game_id = ?1 AND e.tick_no = ?2 AND e.event_type = ?3 "
			 "ORDER BY e.seq;"),
		kEventSelectColumns);

	FSQLitePreparedStatement Stmt;
	if (!Stmt.Create(const_cast<FSQLiteDatabase &>(Db), *Sql))
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
	/** 解 bid payload：{ "urgency": float, "proposed_target"?: str, "rationale"?: str }。 */
	bool ParseBidPayload(const FString &InPayloadJson, float &OutUrgency,
						 FString &OutProposedTarget, FString &OutRationale)
	{
		OutUrgency = 0.f;
		TSharedPtr<FJsonObject> Obj;
		const auto R = TJsonReaderFactory<TCHAR>::Create(InPayloadJson);
		if (!FJsonSerializer::Deserialize(R, Obj) || !Obj.IsValid())
			return false;
		double Tmp = 0.0;
		if (Obj->TryGetNumberField(TEXT("urgency"), Tmp))
		{
			// JSON number 先读成 double，再缩到 gameplay 用的 float。
			OutUrgency = static_cast<float>(Tmp);
		}
		Obj->TryGetStringField(TEXT("proposed_target"), OutProposedTarget);
		Obj->TryGetStringField(TEXT("rationale"), OutRationale);
		return true;
	}

	/** 从 intended payload 抽 addressed_to_hint 数组，给 runtime adjustment 判断“被 @”使用。 */
	TArray<FString> ParseAddressedToHint(const FString &InIntendedPayloadJson)
	{
		TArray<FString> Out;
		TSharedPtr<FJsonObject> Obj;
		const auto R = TJsonReaderFactory<TCHAR>::Create(InIntendedPayloadJson);
		if (!FJsonSerializer::Deserialize(R, Obj) || !Obj.IsValid())
			return Out;
		const TArray<TSharedPtr<FJsonValue>> *Arr = nullptr;
		if (Obj->TryGetArrayField(TEXT("addressed_to_hint"), Arr) && Arr)
		{
			for (const TSharedPtr<FJsonValue> &V : *Arr)
			{
				FString S;
				if (V.IsValid() && V->TryGetString(S))
					Out.Add(S);
			}
		}
		return Out;
	}

	/** 从 tick_resolved payload 取 winner_actor + winner_intended_seq。 */
	bool ParseTickResolvedPayload(const FString &InPayloadJson, FString &OutWinner,
								  int64 &OutWinnerIntendedSeq)
	{
		OutWinner.Reset();
		OutWinnerIntendedSeq = 0;
		TSharedPtr<FJsonObject> Obj;
		const auto R = TJsonReaderFactory<TCHAR>::Create(InPayloadJson);
		if (!FJsonSerializer::Deserialize(R, Obj) || !Obj.IsValid())
			return false;
		Obj->TryGetStringField(TEXT("winner_actor"), OutWinner);
		double Tmp = 0.0;
		if (Obj->TryGetNumberField(TEXT("winner_intended_seq"), Tmp))
			OutWinnerIntendedSeq = static_cast<int64>(Tmp);
		return true;
	}
}

/** 列出某 tick 的全部 bid，并把同 actor 的 speech.intended seq 关联回来。 */
TArray<FAILiveBid> UAILiveEventStoreSubsystem::ListBidsForTick(int64 InTickNo) const
{
	TArray<FAILiveBid> Out;
	if (!IsGameOpen())
		return Out;

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
	if (!Stmt.Create(const_cast<FSQLiteDatabase &>(Db), *Sql))
	{
		UE_LOG(LogAILiveMemory, Error, TEXT("ListBidsForTick: prepare failed: %s"), *Db.GetLastError());
		return Out;
	}
	const FString IntendedTypeStr = AILiveEvent::EventTypeToString(EAILiveEventType::SpeechIntended);
	const FString BidTypeStr = AILiveEvent::EventTypeToString(EAILiveEventType::Bid);
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
		// bid 的分数字段藏在 JSON payload 内，行列字段只保存通用事件结构。
		ParseBidPayload(PayloadJson, B.Urgency, B.ProposedTarget, B.Rationale);
		// BidOffset / FinalScore / RuntimeAdj 由 ResolveFloor 填
		Out.Add(MoveTemp(B));
	}
	return Out;
}

/** 计算某 actor 在当前 tick 的动态加权：反霸麦、被点名、沉默补偿。 */
float UAILiveEventStoreSubsystem::ComputeRuntimeAdjustment(
	const FString &InActor, int64 InCurrentTickNo) const
{
	float Adj = 0.f;
	if (!IsGameOpen() || InActor.IsEmpty() || InCurrentTickNo <= 0)
		return Adj;

	// 反霸麦：扫 tick-1..tick-3 的 tick_resolved.winner_actor。
	// 任务卡常量版：连续 ≥ 3 拍命中 → -1.5（一次性，不叠乘）。
	{
		int32 Streak = 0;
		bool bBroken = false;
		for (int64 T = InCurrentTickNo - 1; T >= InCurrentTickNo - 3 && T > 0 && !bBroken; --T)
		{
			TArray<FAILiveEvent> TR = QuoteByEventTypeAndTick(
				EAILiveEventType::OrchestratorTickResolved, T);
			bool bFoundWin = false;
			for (const FAILiveEvent &E : TR)
			{
				FString W;
				int64 _ = 0;
				if (ParseTickResolvedPayload(E.PayloadJson, W, _) && W == InActor)
				{
					// 只要该 tick 有一条 resolved 说此 actor 赢了，就计入连续霸麦。
					bFoundWin = true;
					break;
				}
			}
			if (bFoundWin)
				++Streak;
			else
			{
				bBroken = true;
			}
		}
		if (Streak >= 3)
			Adj += -1.5f;
	}

	// 被 @ 加权：扫上一拍 tick_resolved → winner intended.addressed_to_hint 含本 actor → +2.0。
	if (InCurrentTickNo >= 2)
	{
		TArray<FAILiveEvent> TR = QuoteByEventTypeAndTick(
			EAILiveEventType::OrchestratorTickResolved, InCurrentTickNo - 1);
		for (const FAILiveEvent &E : TR)
		{
			FString W;
			int64 IntSeq = 0;
			if (!ParseTickResolvedPayload(E.PayloadJson, W, IntSeq) || W.IsEmpty() || IntSeq <= 0)
				continue;

			// 取 winner intended 事件，无 viewer 过滤（orchestrator 自己用）。
			const FString Sql = FString::Printf(
				TEXT("SELECT %s FROM events e WHERE e.game_id = ?1 AND e.seq = ?2;"),
				kEventSelectColumns);
			FSQLitePreparedStatement St;
			if (!St.Create(const_cast<FSQLiteDatabase &>(Db), *Sql))
				continue;
			St.SetBindingValueByIndex(1, CurrentGameId);
			St.SetBindingValueByIndex(2, IntSeq);
			if (St.Step() == ESQLitePreparedStatementStepResult::Row)
			{
				const FAILiveEvent IntEv = RowToEvent(St);
				const TArray<FString> Targets = ParseAddressedToHint(IntEv.PayloadJson);
				if (Targets.Contains(InActor))
				{
					// 上一拍胜者点名了当前 actor，鼓励当前 actor 接话。
					Adj += 2.0f;
					break;
				}
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
		if (St.Create(const_cast<FSQLiteDatabase &>(Db), *Sql))
		{
			St.SetBindingValueByIndex(1, CurrentGameId);
			St.SetBindingValueByIndex(2, InActor);
			St.SetBindingValueByIndex(3, PubTypeStr);
			St.SetBindingValueByIndex(4, InCurrentTickNo - 5);
			St.SetBindingValueByIndex(5, InCurrentTickNo - 1);
			const bool bFound = (St.Step() == ESQLitePreparedStatementStepResult::Row);
			if (!bFound)
				Adj += 0.5f;
		}
	}

	return Adj;
}

/** 根据 bid + offset + runtime adjustment 选择当前 tick 的发言者。 */
FAILiveTickResolution UAILiveEventStoreSubsystem::ResolveFloor(
	int64 InTickNo,
	const TArray<FString> &InEligibleAgentIds,
	const TMap<FString, float> &InAgentBidOffsets,
	float InColdThreshold) const
{
	FAILiveTickResolution Out;
	Out.TickNo = static_cast<int32>(InTickNo);
	if (!IsGameOpen())
		return Out;

	TArray<FAILiveBid> AllBids = ListBidsForTick(InTickNo);

	// 过滤到 eligible 集合
	TSet<FString> EligibleSet;
	for (const FString &A : InEligibleAgentIds)
		EligibleSet.Add(A);

	float BestScore = -FLT_MAX;
	FString BestActor;
	int64 BestIntendedSeq = 0;

	for (FAILiveBid &B : AllBids)
	{
		if (!EligibleSet.Contains(B.Actor))
			continue;
		const float *OffPtr = InAgentBidOffsets.Find(B.Actor);
		B.BidOffset = OffPtr ? *OffPtr : 0.f;
		B.RuntimeAdj = ComputeRuntimeAdjustment(B.Actor, InTickNo);
		// 最终分 = agent 自报 urgency + 外部 offset + 运行时公平性修正。
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
		Out.WinnerActor = BestActor;
		Out.WinnerIntendedSeq = BestIntendedSeq;
	}
	// 否则 WinnerActor 留空（冷场）
	return Out;
}

// ---------------------------------------------------------------
// T3 — Static validation
// ---------------------------------------------------------------

/** 静态校验 visibility：必须非空，且只能使用 schema 允许的 viewer 命名空间。 */
bool UAILiveEventStoreSubsystem::ValidateVisibility(const TArray<FString> &InVisibility, FString &OutError)
{
	if (InVisibility.Num() == 0)
	{
		OutError = TEXT("visibility array must be non-empty");
		return false;
	}
	for (const FString &V : InVisibility)
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
											"(must be one of: public/audience/orchestrator/system/NPC<NN>/Faction<X>)"),
									   *V);
			return false;
		}
	}
	return true;
}

/** 静态校验 payload：必须是 JSON object，并且包含 text 字段供 FTS/UI 使用。 */
bool UAILiveEventStoreSubsystem::ValidatePayloadJson(const FString &InPayloadJson, FString &OutError)
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

/** 软校验 addressed_to 是否包含在 visibility 内；public 可见时视为全部可见。 */
bool UAILiveEventStoreSubsystem::IsAddressedToSubsetOfVisibility(
	const TArray<FString> &InAddressedTo,
	const TArray<FString> &InVisibility,
	FString &OutError)
{
	if (InAddressedTo.Num() == 0)
	{
		return true;
	}
	if (InVisibility.Contains(TEXT("public")))
	{
		// public 是通配可见性：任何 addressed_to 都能看到公开事件。
		return true;
	}
	TSet<FString> VisSet;
	VisSet.Append(InVisibility);
	for (const FString &Target : InAddressedTo)
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

/** 把 FAILiveEvent 写成确定性 JSON；这个字符串是 hash chain 的输入。 */
FString UAILiveEventStoreSubsystem::CanonicalJsonOf(const FAILiveEvent &InEvent)
{
	FString Out;
	const FCanonicalWriterRef W = FCanonicalWriterFactory::Create(&Out);
	W->WriteObjectStart();

	W->WriteValue(TEXT("actor"), InEvent.Actor);

	W->WriteArrayStart(TEXT("addressed_to"));
	for (const FString &T : InEvent.AddressedTo)
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
			// payload 内部也走 canonical writer，避免 JSON 字段顺序导致 hash 不稳定。
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
	for (const FString &V : InEvent.Visibility)
	{
		W->WriteValue(V);
	}
	W->WriteArrayEnd();

	W->WriteObjectEnd();
	W->Close();
	return Out;
}

/** 计算事件 hash：SHA256(prev_hash + canonical_event_json)。 */
FString UAILiveEventStoreSubsystem::ComputeEventHash(const FString &PrevHash, const FString &CanonicalPayload) const
{
	// PrevHash 串进输入里，任何历史事件变化都会级联影响后续 hash。
	const FString Combined = PrevHash + CanonicalPayload;
	return AILiveUtil::Sha256Fingerprint(Combined);
}

// ---------------------------------------------------------------
// T3 — UUIDv7 (RFC 9562)
// ---------------------------------------------------------------

/** 生成 UUIDv7 字符串：前 48 bit 是毫秒时间戳，后面是随机数。 */
FString UAILiveEventStoreSubsystem::GenerateUuidV7()
{
	uint8 B[16];

	const FDateTime Now = FDateTime::UtcNow();
	// UUIDv7 需要 Unix epoch 毫秒；FDateTime 的 ticks 是 100ns 精度。
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

/** 写入时解析当前 parser_version；优先读 _meta.db，失败才回退编译期常量。 */
FString UAILiveEventStoreSubsystem::ResolveLiveParserVersion()
{
	TMap<FString, FString> KVs;
	if (QueryMetaSchemaRegistry(KVs))
	{
		const FString *Found = KVs.Find(TEXT("parser_version"));
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

/** 在已持有写锁和事务时插入单条事件；调用者负责提交/回滚。 */
int64 UAILiveEventStoreSubsystem::InsertEventBypassValidation_LockHeld(
	FAILiveEvent &InOutEvent,
	int64 &InOutLocalLastSeq,
	FString &InOutLocalLastHash)
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
		// 按 SQL 占位符顺序绑定 17 列；任何一步失败都让整条事件写入失败。
		bOk = bOk && Ins.SetBindingValueByIndex(1, InOutEvent.EventId);
		bOk = bOk && Ins.SetBindingValueByIndex(2, InOutEvent.GameId);
		bOk = bOk && Ins.SetBindingValueByIndex(3, InOutEvent.Seq);
		bOk = bOk && Ins.SetBindingValueByIndex(4, (int64)InOutEvent.RoundNo);
		bOk = bOk && Ins.SetBindingValueByIndex(5, AILiveEvent::PhaseToString(InOutEvent.Phase));
		bOk = bOk && Ins.SetBindingValueByIndex(6, InOutEvent.Actor);
		bOk = bOk && Ins.SetBindingValueByIndex(7, AILiveEvent::EventTypeToString(InOutEvent.EventType));
		bOk = bOk && Ins.SetBindingValueByIndex(8, AILiveEvent::SpeechActToString(InOutEvent.SpeechActType));
		bOk = bOk && Ins.SetBindingValueByIndex(9, AILiveEvent::ArrayToJsonString(InOutEvent.Visibility));
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
		for (const FString &Viewer : InOutEvent.Visibility)
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
		for (const FString &Target : InOutEvent.AddressedTo)
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

	// 只有所有表都写成功，才推进调用者传入的本地 seq/hash 游标。
	InOutLocalLastSeq = InOutEvent.Seq;
	InOutLocalLastHash = NewHash;
	return InOutEvent.Seq;
}

// ---------------------------------------------------------------
// T3 — Public bypass: takes lock + BEGIN/COMMIT itself.
// On failure to commit the only safe action is UE_LOG(Fatal): the
// truth-log fallback path is past its responsibility boundary.
// ---------------------------------------------------------------

/** 公共旁路写入：跳过静态校验，但仍使用写锁、事务和 hash chain。 */
int64 UAILiveEventStoreSubsystem::InsertEventBypassValidation(FAILiveEvent &InOutEvent)
{
	if (!IsGameOpen() || !Db.IsValid())
	{
		UE_LOG(LogAILiveMemory, Error, TEXT("InsertEventBypassValidation: no game open"));
		return -1;
	}

	FScopeLock Lock(&WriteMutex);

	// BEGIN IMMEDIATE 提前拿写锁，避免多线程 append 之间交错分配 seq/hash。
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

	// 事务提交后才把本地游标提升为全局缓存。
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

/** 写一条 system.parse_failed 审计事件，记录被静态校验拒绝的原始输入摘要。 */
int64 UAILiveEventStoreSubsystem::AppendSystemParseFailure(
	const FString &InOriginalActor,
	const FString &InOriginalEventTypeStr,
	const FString &InErrorReason,
	const FString &InOriginalPayloadSnippet)
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
	Sys.Visibility = {TEXT("system")};
	Sys.PayloadJson = PayloadJson;

	return InsertEventBypassValidation(Sys);
}

// ---------------------------------------------------------------
// T3 — AppendEvent / AppendEventsAtomically
// ---------------------------------------------------------------

/** 单条事件写入的便捷包装：内部仍走 AppendEventsAtomically。 */
int64 UAILiveEventStoreSubsystem::AppendEvent(FAILiveEvent &InOutEvent)
{
	TArray<FAILiveEvent> Group;
	// MoveTemp 暂时把事件移进数组；写完后再移回给调用者拿到 seq/hash/event_id。
	Group.Add(MoveTemp(InOutEvent));
	const int64 FirstSeq = AppendEventsAtomically(Group);
	InOutEvent = MoveTemp(Group[0]);
	return FirstSeq;
}

/** 原子写入一组事件：全部成功才提交，任一失败则整组回滚。 */
int64 UAILiveEventStoreSubsystem::AppendEventsAtomically(TArray<FAILiveEvent> &InOutEvents)
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
		// 这里用同一个 LocalLastSeq/Hash 串起组内事件，保证组内 hash 顺序连续。
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
		for (FAILiveEvent &Ev : InOutEvents)
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

/** 开启一个 tick：写 tick_anchor 事件，并让后续 AppendEvent 继承该 tick_no。 */
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
	Anchor.Visibility = {TEXT("public")};
	Anchor.PayloadJson = FString::Printf(
		TEXT("{\"text\":\"tick anchor N=%d\",\"tick_no\":%d}"),
		InTickNo, InTickNo);

	int64 LocalLastSeq = CachedLastSeq;
	FString LocalLastHash = CachedLastHash;
	// tick_anchor 本身也进入 hash chain，所以 tick 开始动作可审计。
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

/** Debug 用：直接 upsert _meta.db.schema_meta 的一个 key/value。 */
bool UAILiveEventStoreSubsystem::UpsertMetaSchemaKV(const FString &Key, const FString &Value)
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

/** 在主 Db 连接上重算整条 hash chain，输出最后 seq、坏链数量、第一条坏链 seq。 */
bool UAILiveEventStoreSubsystem::RecomputeHashChainOnMainConnection(
	int64 &OutLastSeq, int64 &OutBadCount, int64 &OutFirstBadSeq)
{
	OutLastSeq = 0;
	OutBadCount = 0;
	OutFirstBadSeq = -1;
	if (!Db.IsValid())
	{
		return false;
	}
	const TCHAR *Sql = TEXT(
		"SELECT seq, event_id, game_id, round_no, phase, actor, event_type,"
		"       speech_act_type, visibility, addressed_to, payload, parent_event_id,"
		"       parser_version, raw_llm_output, prev_event_hash, event_hash"
		" FROM events ORDER BY seq ASC;");

	FString PrevHash = FString(kGenesisHash);
	int64 LastSeq = 0;
	int64 BadCount = 0;
	int64 FirstBadSeq = -1;

	const int64 Rows = Db.Execute(Sql,
								  [this, &PrevHash, &LastSeq, &BadCount, &FirstBadSeq](const FSQLitePreparedStatement &Stmt)
								  {
									  FAILiveEvent Ev;
									  int64 SeqVal = 0;
									  int64 RoundNo64 = 0;
									  FString PhaseStr, EventTypeStr, SpeechActStr, VisJson, AddrJson;
									  FString PrevHashRow, EventHashRow;
									  Stmt.GetColumnValueByIndex(0, SeqVal);
									  Stmt.GetColumnValueByIndex(1, Ev.EventId);
									  Stmt.GetColumnValueByIndex(2, Ev.GameId);
									  Stmt.GetColumnValueByIndex(3, RoundNo64);
									  Stmt.GetColumnValueByIndex(4, PhaseStr);
									  Stmt.GetColumnValueByIndex(5, Ev.Actor);
									  Stmt.GetColumnValueByIndex(6, EventTypeStr);
									  Stmt.GetColumnValueByIndex(7, SpeechActStr);
									  Stmt.GetColumnValueByIndex(8, VisJson);
									  Stmt.GetColumnValueByIndex(9, AddrJson);
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
										  // 第一层检查：当前行声明的 prev_event_hash 必须等于上一行 event_hash。
										  if (FirstBadSeq < 0)
											  FirstBadSeq = SeqVal;
										  ++BadCount;
									  }
									  else
									  {
										  // 第二层检查：用当前行内容重算 event_hash，必须和存储值一致。
										  const FString CanonRecompute = UAILiveEventStoreSubsystem::CanonicalJsonOf(Ev);
										  const FString ExpectHash = ComputeEventHash(PrevHash, CanonRecompute);
										  if (ExpectHash != EventHashRow)
										  {
											  if (FirstBadSeq < 0)
												  FirstBadSeq = SeqVal;
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

/** Debug 用：在主 game DB 连接上执行一条原始 SQL，并把错误字符串传回调用方。 */
bool UAILiveEventStoreSubsystem::ExecuteDebugSqlOnMainConnection(const FString &Sql, FString &OutError)
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

/** Debug 用：切换 SQLite query_only，模拟写入只读失败路径。 */
bool UAILiveEventStoreSubsystem::SetGameDbQueryOnly(bool bQueryOnly)
{
	if (!Db.IsValid())
	{
		UE_LOG(LogAILiveMemory, Error, TEXT("SetGameDbQueryOnly: Db not open"));
		return false;
	}
	const TCHAR *Sql = bQueryOnly
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

// ===============================================================
// T8 — Projector 重建
// principles §7.4 + §7.2 末「projector 是纯函数，不调任何 LLM」。
// 全部 reducer 共享一个 BEGIN IMMEDIATE 事务，DELETE 旧投影 → INSERT 新行；
// 任一步骤失败 → ROLLBACK，旧投影保留。
// ===============================================================

namespace
{

	/** 尝试把字符串解析为 JSON object；失败时返回无效指针。 */
	TSharedPtr<FJsonObject> ParseJsonObject(const FString &Json)
	{
		TSharedPtr<FJsonObject> Out;
		if (Json.IsEmpty())
			return Out;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
		FJsonSerializer::Deserialize(Reader, Out);
		return Out;
	}

	/** 从 JSON object 里安全读取字符串字段；对象无效或字段不存在时返回空串。 */
	FString JsonGetString(const TSharedPtr<FJsonObject> &Obj, const TCHAR *Key)
	{
		if (!Obj.IsValid())
			return FString();
		FString S;
		Obj->TryGetStringField(Key, S);
		return S;
	}

	/** payload.text 截断 256；text 缺失时退而取 raw payload 前缀。 */
	FString ExtractCommitmentText(const TSharedPtr<FJsonObject> &Obj, const FString &RawPayload)
	{
		const FString T = JsonGetString(Obj, TEXT("text"));
		const FString Base = T.IsEmpty() ? RawPayload : T;
		return Base.Len() <= 256 ? Base : Base.Left(256);
	}

	/** speech.intended 的 deny 目标取 addressed_to_hint[0]；其它 commit/claim/deny 默认 NULL。 */
	FString ExtractDenyTarget(const TSharedPtr<FJsonObject> &Obj)
	{
		if (!Obj.IsValid())
			return FString();
		const TArray<TSharedPtr<FJsonValue>> *Arr = nullptr;
		if (!Obj->TryGetArrayField(TEXT("addressed_to_hint"), Arr))
			return FString();
		if (!Arr || Arr->Num() == 0)
			return FString();
		const TSharedPtr<FJsonValue> &V = (*Arr)[0];
		return V.IsValid() ? V->AsString() : FString();
	}

} // anon namespace

// ---------------------------------------------------------------
// ProjectCommitments —— 单 SELECT 拉所有候选 events，按 (event_type,
// speech_act_type) 二维分派为 commitment_type，写入 commitments 表。
// ---------------------------------------------------------------

/** 重建 commitments 投影表；调用者必须已持有写锁和事务。 */
bool UAILiveEventStoreSubsystem::ProjectCommitments_LockHeld()
{
	// 1) DELETE 旧投影
	{
		FSQLitePreparedStatement Del;
		if (!Del.Create(Db, TEXT("DELETE FROM commitments WHERE game_id = ?1;")))
		{
			UE_LOG(LogAILiveMemory, Error,
				   TEXT("ProjectCommitments: DELETE prepare failed: %s"), *Db.GetLastError());
			return false;
		}
		Del.SetBindingValueByIndex(1, CurrentGameId);
		if (Del.Step() == ESQLitePreparedStatementStepResult::Error)
		{
			UE_LOG(LogAILiveMemory, Error,
				   TEXT("ProjectCommitments: DELETE step failed: %s"), *Db.GetLastError());
			return false;
		}
	}

	// 2) 拉候选事件
	const TCHAR *SelSql =
		TEXT("SELECT seq, round_no, actor, event_type, speech_act_type, payload "
			 "FROM events WHERE game_id = ?1 "
			 "  AND event_type IN ('vote','alliance_propose','alliance_accept',"
			 "                     'speech.public','speech.intended') "
			 "ORDER BY seq;");
	FSQLitePreparedStatement Sel;
	if (!Sel.Create(Db, SelSql))
	{
		UE_LOG(LogAILiveMemory, Error,
			   TEXT("ProjectCommitments: SELECT prepare failed: %s"), *Db.GetLastError());
		return false;
	}
	Sel.SetBindingValueByIndex(1, CurrentGameId);

	// 3) 每行分派 → INSERT
	const TCHAR *InsSql =
		TEXT("INSERT OR IGNORE INTO commitments "
			 "(game_id, agent_id, round_no, seq, commitment_type, target, text, status) "
			 "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, 'active');");
	FSQLitePreparedStatement Ins;
	if (!Ins.Create(Db, InsSql))
	{
		UE_LOG(LogAILiveMemory, Error,
			   TEXT("ProjectCommitments: INSERT prepare failed: %s"), *Db.GetLastError());
		return false;
	}

	int64 InsertedCount = 0;
	while (Sel.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		int64 Seq = 0, RoundNo = 0;
		FString Actor, EventType, SpeechAct, Payload;
		Sel.GetColumnValueByIndex(0, Seq);
		Sel.GetColumnValueByIndex(1, RoundNo);
		Sel.GetColumnValueByIndex(2, Actor);
		Sel.GetColumnValueByIndex(3, EventType);
		Sel.GetColumnValueByIndex(4, SpeechAct);
		Sel.GetColumnValueByIndex(5, Payload);

		const TSharedPtr<FJsonObject> P = ParseJsonObject(Payload);

		FString CommitmentType, Target, Text;

		if (EventType == TEXT("vote"))
		{
			CommitmentType = TEXT("vote_for");
			Target = JsonGetString(P, TEXT("target"));
			Text = ExtractCommitmentText(P, Payload);
		}
		else if (EventType == TEXT("alliance_propose") || EventType == TEXT("alliance_accept"))
		{
			CommitmentType = TEXT("alliance");
			Target = JsonGetString(P, TEXT("alliance_id"));
			Text = ExtractCommitmentText(P, Payload);
		}
		else // speech.public / speech.intended — 由 speech_act_type 二次分派
		{
			// 同样是 speech 事件，只有 commit/claim/deny 会变成 commitment 投影。
			if (SpeechAct == TEXT("commit"))
			{
				CommitmentType = TEXT("promise");
				Text = ExtractCommitmentText(P, Payload);
			}
			else if (SpeechAct == TEXT("claim"))
			{
				CommitmentType = TEXT("claim_role");
				Text = ExtractCommitmentText(P, Payload);
			}
			else if (SpeechAct == TEXT("deny"))
			{
				CommitmentType = TEXT("deny");
				Target = ExtractDenyTarget(P);
				Text = ExtractCommitmentText(P, Payload);
			}
			else
			{
				continue; // 其它 speech_act_type 不入 commitments
			}
		}

		Ins.Reset();
		Ins.ClearBindings();
		Ins.SetBindingValueByIndex(1, CurrentGameId);
		Ins.SetBindingValueByIndex(2, Actor);
		Ins.SetBindingValueByIndex(3, RoundNo);
		Ins.SetBindingValueByIndex(4, Seq);
		Ins.SetBindingValueByIndex(5, CommitmentType);
		if (Target.IsEmpty())
		{
			Ins.SetBindingValueByIndex(6); // bind NULL（参数 6 = target 列）
		}
		else
		{
			Ins.SetBindingValueByIndex(6, Target);
		}
		Ins.SetBindingValueByIndex(7, Text);

		if (Ins.Step() == ESQLitePreparedStatementStepResult::Error)
		{
			UE_LOG(LogAILiveMemory, Error,
				   TEXT("ProjectCommitments: INSERT step failed (seq=%lld type=%s): %s"),
				   Seq, *CommitmentType, *Db.GetLastError());
			return false;
		}
		++InsertedCount;
	}

	UE_LOG(LogAILiveMemory, Verbose,
		   TEXT("ProjectCommitments: inserted %lld rows"), InsertedCount);
	return true;
}

// ---------------------------------------------------------------
// ProjectVoteHistory —— INSERT…SELECT 直接派生（payload.target 提取走
// json_extract，避免 C++ 端二次解析）。
// ---------------------------------------------------------------

/** 重建 vote_history 投影表；调用者必须已持有写锁和事务。 */
bool UAILiveEventStoreSubsystem::ProjectVoteHistory_LockHeld()
{
	{
		FSQLitePreparedStatement Del;
		if (!Del.Create(Db, TEXT("DELETE FROM vote_history WHERE game_id = ?1;")))
		{
			UE_LOG(LogAILiveMemory, Error,
				   TEXT("ProjectVoteHistory: DELETE prepare failed: %s"), *Db.GetLastError());
			return false;
		}
		Del.SetBindingValueByIndex(1, CurrentGameId);
		if (Del.Step() == ESQLitePreparedStatementStepResult::Error)
		{
			UE_LOG(LogAILiveMemory, Error,
				   TEXT("ProjectVoteHistory: DELETE step failed: %s"), *Db.GetLastError());
			return false;
		}
	}

	const TCHAR *InsSql =
		TEXT("INSERT OR IGNORE INTO vote_history (game_id, round_no, seq, voter, target) "
			 "SELECT game_id, round_no, seq, actor, "
			 "       COALESCE(json_extract(payload, '$.target'), '') "
			 "FROM events "
			 "WHERE game_id = ?1 AND event_type = 'vote';");
	FSQLitePreparedStatement Ins;
	if (!Ins.Create(Db, InsSql))
	{
		UE_LOG(LogAILiveMemory, Error,
			   TEXT("ProjectVoteHistory: INSERT prepare failed: %s"), *Db.GetLastError());
		return false;
	}
	Ins.SetBindingValueByIndex(1, CurrentGameId);
	// 这一步让 SQLite 自己从 events 派生 vote_history，减少 C++ 侧逐行解析。
	if (Ins.Step() == ESQLitePreparedStatementStepResult::Error)
	{
		UE_LOG(LogAILiveMemory, Error,
			   TEXT("ProjectVoteHistory: INSERT step failed: %s"), *Db.GetLastError());
		return false;
	}

	UE_LOG(LogAILiveMemory, Verbose, TEXT("ProjectVoteHistory: rebuild OK"));
	return true;
}

// ---------------------------------------------------------------
// ProjectAllianceState —— 按 alliance_id 聚合；propose 取 MIN(seq)，
// accept/betray 取 MAX(seq)；members/terms 从首条 propose payload 抽。
// ---------------------------------------------------------------

/** 重建 alliance_state 投影表，把 alliance 事件流折叠成当前联盟状态。 */
bool UAILiveEventStoreSubsystem::ProjectAllianceState_LockHeld()
{
	{
		FSQLitePreparedStatement Del;
		if (!Del.Create(Db, TEXT("DELETE FROM alliance_state WHERE game_id = ?1;")))
		{
			UE_LOG(LogAILiveMemory, Error,
				   TEXT("ProjectAllianceState: DELETE prepare failed: %s"), *Db.GetLastError());
			return false;
		}
		Del.SetBindingValueByIndex(1, CurrentGameId);
		if (Del.Step() == ESQLitePreparedStatementStepResult::Error)
		{
			UE_LOG(LogAILiveMemory, Error,
				   TEXT("ProjectAllianceState: DELETE step failed: %s"), *Db.GetLastError());
			return false;
		}
	}

	struct FAllianceAcc
	{
		int64 ProposedAtSeq = 0;
		int64 AcceptedAtSeq = -1;
		int64 BetrayedAtSeq = -1;
		FString MembersJson = TEXT("[]");
		FString Terms;
		bool bHasPropose = false;
	};
	TMap<FString, FAllianceAcc> ByAlliance;

	const TCHAR *SelSql =
		TEXT("SELECT seq, event_type, payload FROM events "
			 "WHERE game_id = ?1 AND event_type IN "
			 "  ('alliance_propose','alliance_accept','alliance_betray') "
			 "ORDER BY seq;");
	FSQLitePreparedStatement Sel;
	if (!Sel.Create(Db, SelSql))
	{
		UE_LOG(LogAILiveMemory, Error,
			   TEXT("ProjectAllianceState: SELECT prepare failed: %s"), *Db.GetLastError());
		return false;
	}
	Sel.SetBindingValueByIndex(1, CurrentGameId);

	while (Sel.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		int64 Seq = 0;
		FString EventType, Payload;
		Sel.GetColumnValueByIndex(0, Seq);
		Sel.GetColumnValueByIndex(1, EventType);
		Sel.GetColumnValueByIndex(2, Payload);

		const TSharedPtr<FJsonObject> P = ParseJsonObject(Payload);
		const FString AllianceId = JsonGetString(P, TEXT("alliance_id"));
		if (AllianceId.IsEmpty())
			continue;

		FAllianceAcc &Acc = ByAlliance.FindOrAdd(AllianceId);
		if (EventType == TEXT("alliance_propose"))
		{
			// propose 是联盟的“创建信息”，成员与条款只取第一条 propose。
			if (!Acc.bHasPropose)
			{
				Acc.bHasPropose = true;
				Acc.ProposedAtSeq = Seq;
				Acc.Terms = JsonGetString(P, TEXT("terms"));
				if (P.IsValid())
				{
					const TArray<TSharedPtr<FJsonValue>> *Arr = nullptr;
					if (P->TryGetArrayField(TEXT("members"), Arr) && Arr)
					{
						FString MembersOut;
						const TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&MembersOut);
						FJsonSerializer::Serialize(*Arr, W);
						Acc.MembersJson = MembersOut;
					}
				}
			}
		}
		else if (EventType == TEXT("alliance_accept"))
		{
			// accept/betray 取最后一次出现的 seq，表达当前最新状态。
			Acc.AcceptedAtSeq = Seq;
		}
		else if (EventType == TEXT("alliance_betray"))
		{
			Acc.BetrayedAtSeq = Seq;
		}
	}

	const TCHAR *InsSql =
		TEXT("INSERT INTO alliance_state "
			 "(game_id, alliance_id, members, proposed_at_seq, accepted_at_seq, "
			 " betrayed_at_seq, terms) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7);");
	FSQLitePreparedStatement Ins;
	if (!Ins.Create(Db, InsSql))
	{
		UE_LOG(LogAILiveMemory, Error,
			   TEXT("ProjectAllianceState: INSERT prepare failed: %s"), *Db.GetLastError());
		return false;
	}

	for (const TPair<FString, FAllianceAcc> &Pair : ByAlliance)
	{
		const FAllianceAcc &A = Pair.Value;
		// alliance_propose 缺失（只有 accept / betray，少见）→ proposed_at_seq 取首次出现 seq
		const int64 ProposedSeq = A.bHasPropose ? A.ProposedAtSeq
												: (A.AcceptedAtSeq >= 0 ? A.AcceptedAtSeq : A.BetrayedAtSeq);
		Ins.Reset();
		Ins.ClearBindings();
		Ins.SetBindingValueByIndex(1, CurrentGameId);
		Ins.SetBindingValueByIndex(2, Pair.Key);
		Ins.SetBindingValueByIndex(3, A.MembersJson);
		Ins.SetBindingValueByIndex(4, ProposedSeq);
		if (A.AcceptedAtSeq < 0)
		{
			Ins.SetBindingValueByIndex(5); // bind NULL（参数 5 = accepted_at_seq）
		}
		else
		{
			Ins.SetBindingValueByIndex(5, A.AcceptedAtSeq);
		}
		if (A.BetrayedAtSeq < 0)
		{
			Ins.SetBindingValueByIndex(6); // bind NULL（参数 6 = betrayed_at_seq）
		}
		else
		{
			Ins.SetBindingValueByIndex(6, A.BetrayedAtSeq);
		}
		Ins.SetBindingValueByIndex(7, A.Terms);
		if (Ins.Step() == ESQLitePreparedStatementStepResult::Error)
		{
			UE_LOG(LogAILiveMemory, Error,
				   TEXT("ProjectAllianceState: INSERT step failed (alliance_id=%s): %s"),
				   *Pair.Key, *Db.GetLastError());
			return false;
		}
	}

	UE_LOG(LogAILiveMemory, Verbose,
		   TEXT("ProjectAllianceState: rebuilt %d rows"), ByAlliance.Num());
	return true;
}

// ---------------------------------------------------------------
// ProjectAgentViewState —— 单快照策略：每 agent 一行，as_of_seq=last_seq。
// alive_players / known_roles / my_commitments / vote_history /
// pending_intended 五段 JSON 由内联 SQL + JSON 写出器组装。
// 必须在 ProjectCommitments / ProjectVoteHistory 之后调用——本步直读那两张表。
// ---------------------------------------------------------------

/** 重建 agent_view_state 投影表，为每个 agent 写一行当前私有视图快照。 */
bool UAILiveEventStoreSubsystem::ProjectAgentViewState_LockHeld()
{
	{
		FSQLitePreparedStatement Del;
		if (!Del.Create(Db, TEXT("DELETE FROM agent_view_state WHERE game_id = ?1;")))
		{
			UE_LOG(LogAILiveMemory, Error,
				   TEXT("ProjectAgentViewState: DELETE prepare failed: %s"), *Db.GetLastError());
			return false;
		}
		Del.SetBindingValueByIndex(1, CurrentGameId);
		if (Del.Step() == ESQLitePreparedStatementStepResult::Error)
		{
			UE_LOG(LogAILiveMemory, Error,
				   TEXT("ProjectAgentViewState: DELETE step failed: %s"), *Db.GetLastError());
			return false;
		}
	}

	// === 1) Roster：agent_calibration 优先；为空则从 events.actor DISTINCT NPC* 兜底。
	TArray<FString> Roster;
	{
		FSQLitePreparedStatement Stmt;
		if (Stmt.Create(Db,
						TEXT("SELECT agent_id FROM agent_calibration WHERE game_id = ?1 ORDER BY agent_id;")))
		{
			Stmt.SetBindingValueByIndex(1, CurrentGameId);
			while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
			{
				FString AgentId;
				Stmt.GetColumnValueByIndex(0, AgentId);
				Roster.Add(AgentId);
			}
		}
	}
	if (Roster.Num() == 0)
	{
		FSQLitePreparedStatement Stmt;
		if (Stmt.Create(Db,
						TEXT("SELECT DISTINCT actor FROM events WHERE game_id = ?1 "
							 "  AND actor LIKE 'NPC%' ORDER BY actor;")))
		{
			Stmt.SetBindingValueByIndex(1, CurrentGameId);
			while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
			{
				FString AgentId;
				Stmt.GetColumnValueByIndex(0, AgentId);
				Roster.Add(AgentId);
			}
		}
	}
	if (Roster.Num() == 0)
	{
		UE_LOG(LogAILiveMemory, Verbose,
			   TEXT("ProjectAgentViewState: empty roster — nothing to project"));
		return true;
	}

	// === 2) 全局信息：当前 last_seq + deleted set + role_assigned 列表（一次查完）。
	const int64 AsOfSeq = CachedLastSeq;

	TSet<FString> DeletedActors;
	{
		FSQLitePreparedStatement Stmt;
		if (Stmt.Create(Db,
						TEXT("SELECT actor FROM events WHERE game_id = ?1 "
							 "  AND event_type = 'system.delete_executed';")))
		{
			Stmt.SetBindingValueByIndex(1, CurrentGameId);
			while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
			{
				FString A;
				Stmt.GetColumnValueByIndex(0, A);
				if (!A.IsEmpty())
					DeletedActors.Add(A);
			}
		}
	}

	// role_assigned 事件：actor=被分配角色的 agent；payload.role=角色字符串；
	// visibility 列表 → event_visibility 表逐行存。本步把 (event_id, actor, role)
	// + 该事件可见的 viewer 集合一并捞，供后续 per-agent 过滤。
	struct FRoleAssignment
	{
		FString EventId;
		FString Actor;
		FString Role;
		TArray<FString> Viewers;
	};
	TArray<FRoleAssignment> RoleEvents;
	{
		FSQLitePreparedStatement Stmt;
		if (Stmt.Create(Db,
						TEXT("SELECT event_id, actor, "
							 "       COALESCE(json_extract(payload, '$.role'), '') "
							 "FROM events WHERE game_id = ?1 AND event_type = 'system.role_assigned' "
							 "ORDER BY seq;")))
		{
			Stmt.SetBindingValueByIndex(1, CurrentGameId);
			while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
			{
				FRoleAssignment R;
				Stmt.GetColumnValueByIndex(0, R.EventId);
				Stmt.GetColumnValueByIndex(1, R.Actor);
				Stmt.GetColumnValueByIndex(2, R.Role);
				if (!R.EventId.IsEmpty())
					RoleEvents.Add(MoveTemp(R));
			}
		}
		// 拉每条 role_assigned 的 viewers
		FSQLitePreparedStatement VStmt;
		if (VStmt.Create(Db, TEXT("SELECT viewer FROM event_visibility WHERE event_id = ?1;")))
		{
			for (FRoleAssignment &R : RoleEvents)
			{
				VStmt.Reset();
				VStmt.ClearBindings();
				VStmt.SetBindingValueByIndex(1, R.EventId);
				while (VStmt.Step() == ESQLitePreparedStatementStepResult::Row)
				{
					FString V;
					VStmt.GetColumnValueByIndex(0, V);
					R.Viewers.Add(V);
				}
			}
		}
	}

	// 最近 tick_no 上限（pending_intended 的 10 拍窗口）
	int64 LatestTickNo = 0;
	{
		FSQLitePreparedStatement Stmt;
		if (Stmt.Create(Db,
						TEXT("SELECT COALESCE(MAX(tick_no), 0) FROM events WHERE game_id = ?1;")))
		{
			Stmt.SetBindingValueByIndex(1, CurrentGameId);
			if (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
			{
				Stmt.GetColumnValueByIndex(0, LatestTickNo);
			}
		}
	}
	const int64 PendingTickFloor = FMath::Max<int64>(0, LatestTickNo - 9);

	// === 3) per-agent 拼装并 INSERT
	const TCHAR *InsSql =
		TEXT("INSERT INTO agent_view_state "
			 "(game_id, agent_id, as_of_seq, alive_players, known_roles, "
			 " my_commitments, vote_history, pending_intended) "
			 "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8);");
	FSQLitePreparedStatement Ins;
	if (!Ins.Create(Db, InsSql))
	{
		UE_LOG(LogAILiveMemory, Error,
			   TEXT("ProjectAgentViewState: INSERT prepare failed: %s"), *Db.GetLastError());
		return false;
	}

	// 每 agent 复用三个查询语句（commitments / vote_history / pending）
	FSQLitePreparedStatement CommitStmt;
	CommitStmt.Create(Db,
					  TEXT("SELECT seq, commitment_type, COALESCE(target,''), text, status "
						   "FROM commitments WHERE game_id = ?1 AND agent_id = ?2 ORDER BY seq;"));
	FSQLitePreparedStatement VotesStmt;
	VotesStmt.Create(Db,
					 TEXT("SELECT round_no, seq, target FROM vote_history "
						  "WHERE game_id = ?1 AND voter = ?2 ORDER BY seq;"));
	FSQLitePreparedStatement PendingStmt;
	PendingStmt.Create(Db,
					   TEXT("SELECT seq, tick_no, COALESCE(json_extract(payload, '$.text'), ''), "
							"       json_extract(payload, '$.intended_action') "
							"FROM events e "
							"WHERE e.game_id = ?1 AND e.actor = ?2 "
							"  AND e.event_type = 'speech.intended' "
							"  AND e.tick_no >= ?3 "
							"  AND NOT EXISTS (SELECT 1 FROM events c "
							"                  WHERE c.game_id = e.game_id "
							"                    AND c.event_type = 'speech.public' "
							"                    AND c.parent_event_id = e.event_id) "
							"ORDER BY e.seq;"));

	int64 InsertedRows = 0;
	for (const FString &AgentId : Roster)
	{
		// alive_players：roster - DeletedActors
		FString AlivePlayersJson;
		{
			const TSharedRef<TJsonWriter<>> W =
				TJsonWriterFactory<>::Create(&AlivePlayersJson);
			W->WriteArrayStart();
			for (const FString &A : Roster)
			{
				if (!DeletedActors.Contains(A))
					W->WriteValue(A);
			}
			W->WriteArrayEnd();
			W->Close();
		}

		// known_roles：所有该 agent 可见的 role_assigned 事件
		FString KnownRolesJson;
		{
			const TSharedRef<TJsonWriter<>> W =
				TJsonWriterFactory<>::Create(&KnownRolesJson);
			W->WriteObjectStart();
			TSet<FString> SeenActors;
			for (const FRoleAssignment &R : RoleEvents)
			{
				const bool bVisible = R.Viewers.Contains(AgentId) || R.Viewers.Contains(TEXT("public")) || R.Viewers.Contains(TEXT("audience"));
				if (!bVisible)
					continue;
				if (SeenActors.Contains(R.Actor))
					continue; // 同 actor 后续 role 覆盖：取首次
				SeenActors.Add(R.Actor);
				W->WriteValue(R.Actor, R.Role);
			}
			W->WriteObjectEnd();
			W->Close();
		}

		// my_commitments：commitments 表 WHERE agent_id=AgentId
		FString MyCommitmentsJson;
		{
			const TSharedRef<TJsonWriter<>> W =
				TJsonWriterFactory<>::Create(&MyCommitmentsJson);
			W->WriteObjectStart();
			CommitStmt.Reset();
			CommitStmt.ClearBindings();
			CommitStmt.SetBindingValueByIndex(1, CurrentGameId);
			CommitStmt.SetBindingValueByIndex(2, AgentId);
			while (CommitStmt.Step() == ESQLitePreparedStatementStepResult::Row)
			{
				int64 CSeq = 0;
				FString CType, CTarget, CText, CStatus;
				CommitStmt.GetColumnValueByIndex(0, CSeq);
				CommitStmt.GetColumnValueByIndex(1, CType);
				CommitStmt.GetColumnValueByIndex(2, CTarget);
				CommitStmt.GetColumnValueByIndex(3, CText);
				CommitStmt.GetColumnValueByIndex(4, CStatus);
				W->WriteObjectStart(FString::Printf(TEXT("%lld"), CSeq));
				W->WriteValue(TEXT("type"), CType);
				W->WriteValue(TEXT("target"), CTarget);
				W->WriteValue(TEXT("text"), CText);
				W->WriteValue(TEXT("status"), CStatus);
				W->WriteObjectEnd();
			}
			W->WriteObjectEnd();
			W->Close();
		}

		// vote_history：vote_history 表 WHERE voter=AgentId
		FString MyVoteHistoryJson;
		{
			const TSharedRef<TJsonWriter<>> W =
				TJsonWriterFactory<>::Create(&MyVoteHistoryJson);
			W->WriteArrayStart();
			VotesStmt.Reset();
			VotesStmt.ClearBindings();
			VotesStmt.SetBindingValueByIndex(1, CurrentGameId);
			VotesStmt.SetBindingValueByIndex(2, AgentId);
			while (VotesStmt.Step() == ESQLitePreparedStatementStepResult::Row)
			{
				int64 RoundNo = 0, VSeq = 0;
				FString VTarget;
				VotesStmt.GetColumnValueByIndex(0, RoundNo);
				VotesStmt.GetColumnValueByIndex(1, VSeq);
				VotesStmt.GetColumnValueByIndex(2, VTarget);
				W->WriteObjectStart();
				W->WriteValue(TEXT("round_no"), RoundNo);
				W->WriteValue(TEXT("seq"), VSeq);
				W->WriteValue(TEXT("target"), VTarget);
				W->WriteObjectEnd();
			}
			W->WriteArrayEnd();
			W->Close();
		}

		// pending_intended：speech.intended 没被 speech.public 引用的，最近 10 拍内
		FString PendingJson;
		{
			const TSharedRef<TJsonWriter<>> W =
				TJsonWriterFactory<>::Create(&PendingJson);
			W->WriteArrayStart();
			PendingStmt.Reset();
			PendingStmt.ClearBindings();
			PendingStmt.SetBindingValueByIndex(1, CurrentGameId);
			PendingStmt.SetBindingValueByIndex(2, AgentId);
			PendingStmt.SetBindingValueByIndex(3, PendingTickFloor);
			while (PendingStmt.Step() == ESQLitePreparedStatementStepResult::Row)
			{
				int64 PSeq = 0, PTick = 0;
				FString PText, IntendedActionJson;
				PendingStmt.GetColumnValueByIndex(0, PSeq);
				PendingStmt.GetColumnValueByIndex(1, PTick);
				PendingStmt.GetColumnValueByIndex(2, PText);
				PendingStmt.GetColumnValueByIndex(3, IntendedActionJson); // NULL → empty FString
				W->WriteObjectStart();
				W->WriteValue(TEXT("seq"), PSeq);
				W->WriteValue(TEXT("tick_no"), PTick);
				W->WriteValue(TEXT("text_snippet"),
							  PText.Len() > 80 ? PText.Left(80) : PText);
				if (!IntendedActionJson.IsEmpty())
				{
					W->WriteRawJSONValue(TEXT("intended_action"), IntendedActionJson);
				}
				W->WriteObjectEnd();
			}
			W->WriteArrayEnd();
			W->Close();
		}

		// INSERT
		Ins.Reset();
		Ins.ClearBindings();
		// agent_view_state 是每个 agent 一行的快照，JSON 字段承载各段私有视图。
		Ins.SetBindingValueByIndex(1, CurrentGameId);
		Ins.SetBindingValueByIndex(2, AgentId);
		Ins.SetBindingValueByIndex(3, AsOfSeq);
		Ins.SetBindingValueByIndex(4, AlivePlayersJson);
		Ins.SetBindingValueByIndex(5, KnownRolesJson);
		Ins.SetBindingValueByIndex(6, MyCommitmentsJson);
		Ins.SetBindingValueByIndex(7, MyVoteHistoryJson);
		Ins.SetBindingValueByIndex(8, PendingJson);
		if (Ins.Step() == ESQLitePreparedStatementStepResult::Error)
		{
			UE_LOG(LogAILiveMemory, Error,
				   TEXT("ProjectAgentViewState: INSERT step failed (agent=%s): %s"),
				   *AgentId, *Db.GetLastError());
			return false;
		}
		++InsertedRows;
	}

	UE_LOG(LogAILiveMemory, Verbose,
		   TEXT("ProjectAgentViewState: rebuilt %lld rows (as_of_seq=%lld, latest_tick=%lld)"),
		   InsertedRows, AsOfSeq, LatestTickNo);
	return true;
}

// ---------------------------------------------------------------
// RebuildProjections —— 主入口。WriteMutex + 单 BEGIN IMMEDIATE 事务 →
// ProjectCommitments → ProjectVoteHistory → ProjectAllianceState →
// ProjectAgentViewState（依赖前两表已写）→ COMMIT。任一步骤失败 → ROLLBACK。
// ---------------------------------------------------------------

/** 投影重建主入口：用一个事务重建所有派生表，保证快照一致。 */
bool UAILiveEventStoreSubsystem::RebuildProjections()
{
	if (!IsGameOpen() || !Db.IsValid())
	{
		UE_LOG(LogAILiveMemory, Verbose, TEXT("RebuildProjections: no game open"));
		return false;
	}

	const double T0 = FPlatformTime::Seconds();

	FScopeLock Lock(&WriteMutex);

	// 投影重建必须和写入互斥，否则会读到半截事件流。
	if (!Db.Execute(TEXT("BEGIN IMMEDIATE;")))
	{
		UE_LOG(LogAILiveMemory, Error,
			   TEXT("RebuildProjections: BEGIN IMMEDIATE failed: %s"), *Db.GetLastError());
		return false;
	}

	// 顺序有依赖：agent_view_state 会读取 commitments 和 vote_history。
	bool bOk = ProjectCommitments_LockHeld() && ProjectVoteHistory_LockHeld() && ProjectAllianceState_LockHeld() && ProjectAgentViewState_LockHeld();

	if (bOk && !Db.Execute(TEXT("COMMIT;")))
	{
		UE_LOG(LogAILiveMemory, Error,
			   TEXT("RebuildProjections: COMMIT failed: %s"), *Db.GetLastError());
		bOk = false;
	}
	if (!bOk)
	{
		Db.Execute(TEXT("ROLLBACK;"));
		UE_LOG(LogAILiveMemory, Error,
			   TEXT("RebuildProjections: rolled back (game=%s)"), *CurrentGameId);
		return false;
	}

	const double Elapsed = (FPlatformTime::Seconds() - T0) * 1000.0;
	UE_LOG(LogAILiveMemory, Display,
		   TEXT("RebuildProjections OK (game=%s, elapsed=%.2fms)"),
		   *CurrentGameId, Elapsed);
	return true;
}

// ---------------------------------------------------------------
// T8 — 读 agent_view_state.pending_intended JSON（最新 as_of_seq 行）。
// ---------------------------------------------------------------

/** Debug 读接口：读取某 agent 最新投影快照里的 pending_intended JSON。 */
FString UAILiveEventStoreSubsystem::DebugReadPendingIntendedJson(const FString &InAgentId) const
{
	if (!IsGameOpen())
		return TEXT("[]");

	FSQLitePreparedStatement Stmt;
	if (!Stmt.Create(const_cast<FSQLiteDatabase &>(Db),
					 TEXT("SELECT pending_intended FROM agent_view_state "
						  "WHERE game_id = ?1 AND agent_id = ?2 "
						  "ORDER BY as_of_seq DESC LIMIT 1;")))
	{
		UE_LOG(LogAILiveMemory, Verbose,
			   TEXT("DebugReadPendingIntendedJson: prepare failed: %s"), *Db.GetLastError());
		return TEXT("[]");
	}
	Stmt.SetBindingValueByIndex(1, CurrentGameId);
	Stmt.SetBindingValueByIndex(2, InAgentId);
	if (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		FString Out;
		Stmt.GetColumnValueByIndex(0, Out);
		return Out.IsEmpty() ? FString(TEXT("[]")) : Out;
	}
	return TEXT("[]");
}

// ---------------------------------------------------------------
// T9 — Resume protocol (impl §5.4 / principles §5.2bis.4).
// MVP 保守策略：所有未配对 in-flight（不区分新鲜/过期）统一写
// system.agent_timeout，不重发；payload 含 age_seconds 供诊断。
// 写完后 RebuildProjections() 让派生表与 events 一致。
// ---------------------------------------------------------------

namespace
{

	/** 从事件 payload 里抽 request_id；用于把 in-flight 和完成/timeout 事件配对。 */
	bool ExtractRequestIdFromPayload(const FString &InPayload, FString &OutRequestId)
	{
		OutRequestId.Reset();
		if (InPayload.IsEmpty())
		{
			return false;
		}
		TSharedPtr<FJsonObject> Obj;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(InPayload);
		if (!FJsonSerializer::Deserialize(Reader, Obj) || !Obj.IsValid())
		{
			return false;
		}
		return Obj->TryGetStringField(TEXT("request_id"), OutRequestId) && !OutRequestId.IsEmpty();
	}

	/** Resume 扫描 system.llm_inflight 时使用的中间行结构。 */
	struct FInflightRow
	{
		int64 Seq = 0;
		int64 TickNo = 0;
		int32 RoundNo = 0;
		FString PhaseStr;
		FString RequestId;
		int32 NPCIndex = 0;
		FString StartedAtIso;
	};

} // anonymous

/** 恢复一局旧游戏：打开 DB，给未完成的 LLM in-flight 补 timeout，并重建投影。 */
bool UAILiveEventStoreSubsystem::ResumeFromGameId(const FString &InGameId)
{
	if (InGameId.IsEmpty())
	{
		UE_LOG(LogAILiveMemory, Error, TEXT("ResumeFromGameId: empty game_id"));
		return false;
	}

	if (!BeginGame(InGameId))
	{
		UE_LOG(LogAILiveMemory, Error, TEXT("ResumeFromGameId('%s'): BeginGame failed"), *InGameId);
		return false;
	}

	// Step 1 — collect every system.llm_inflight row.
	TArray<FInflightRow> Inflights;
	{
		FSQLitePreparedStatement Stmt;
		if (!Stmt.Create(Db,
						 TEXT("SELECT seq, tick_no, round_no, phase, payload FROM events "
							  "WHERE game_id = ?1 AND event_type = 'system.llm_inflight' "
							  "ORDER BY seq ASC;")))
		{
			UE_LOG(LogAILiveMemory, Error,
				   TEXT("ResumeFromGameId: prepare inflight scan failed: %s"), *Db.GetLastError());
			return false;
		}
		Stmt.SetBindingValueByIndex(1, CurrentGameId);
		while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
		{
			FInflightRow Row;
			int64 RoundNo64 = 0;
			FString Payload;
			Stmt.GetColumnValueByIndex(0, Row.Seq);
			Stmt.GetColumnValueByIndex(1, Row.TickNo);
			Stmt.GetColumnValueByIndex(2, RoundNo64);
			Stmt.GetColumnValueByIndex(3, Row.PhaseStr);
			Stmt.GetColumnValueByIndex(4, Payload);
			Row.RoundNo = (int32)RoundNo64;

			TSharedPtr<FJsonObject> Obj;
			const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Payload);
			if (FJsonSerializer::Deserialize(Reader, Obj) && Obj.IsValid())
			{
				Obj->TryGetStringField(TEXT("request_id"), Row.RequestId);
				int32 NpcInt = 0;
				if (Obj->TryGetNumberField(TEXT("npc_index"), NpcInt))
				{
					Row.NPCIndex = NpcInt;
				}
				Obj->TryGetStringField(TEXT("started_at"), Row.StartedAtIso);
			}
			if (!Row.RequestId.IsEmpty())
			{
				Inflights.Add(MoveTemp(Row));
			}
			else
			{
				UE_LOG(LogAILiveMemory, Warning,
					   TEXT("ResumeFromGameId: in-flight seq=%lld missing request_id; skipped"),
					   Row.Seq);
			}
		}
	}

	// Step 2 — collect request_ids that already have a matching completion event.
	// "Completion" = any event other than the in-flight marker that carries the
	// same request_id in its payload. We include system.agent_timeout so that
	// re-running Resume on the same .db is idempotent.
	TSet<FString> CompletedRequestIds;
	{
		FSQLitePreparedStatement Stmt;
		if (!Stmt.Create(Db,
						 TEXT("SELECT payload FROM events "
							  "WHERE game_id = ?1 AND event_type != 'system.llm_inflight' "
							  "  AND payload LIKE '%request_id%';")))
		{
			UE_LOG(LogAILiveMemory, Error,
				   TEXT("ResumeFromGameId: prepare completion scan failed: %s"), *Db.GetLastError());
			return false;
		}
		Stmt.SetBindingValueByIndex(1, CurrentGameId);
		while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
		{
			FString Payload;
			Stmt.GetColumnValueByIndex(0, Payload);
			FString Rid;
			if (ExtractRequestIdFromPayload(Payload, Rid))
			{
				// 已完成集合包含真实完成事件，也包含之前 resume 写过的 timeout，保证幂等。
				CompletedRequestIds.Add(Rid);
			}
		}
	}

	// Step 3 — write one system.agent_timeout per unpaired in-flight.
	const FDateTime Now = FDateTime::UtcNow();
	const FString ResumedAtIso = Now.ToIso8601();
	int32 TimeoutsWritten = 0;
	for (const FInflightRow &Row : Inflights)
	{
		if (CompletedRequestIds.Contains(Row.RequestId))
		{
			continue;
		}

		double AgeSeconds = -1.0;
		if (!Row.StartedAtIso.IsEmpty())
		{
			FDateTime Started;
			if (FDateTime::ParseIso8601(*Row.StartedAtIso, Started))
			{
				AgeSeconds = (Now - Started).GetTotalSeconds();
			}
		}

		FAILiveEvent Out;
		Out.RoundNo = Row.RoundNo;
		Out.Phase = AILiveEvent::PhaseFromString(Row.PhaseStr);
		Out.Actor = TEXT("orchestrator");
		Out.EventType = EAILiveEventType::SystemAgentTimeout;
		Out.Visibility = {TEXT("system")};
		// timeout 事件 payload 带 request_id 和 source seq，便于事后追查哪次调用未完成。
		Out.PayloadJson = FString::Printf(
			TEXT("{\"text\":\"agent timeout from resume\",\"npc_index\":%d,")
				TEXT("\"request_id\":\"%s\",\"age_seconds\":%.3f,\"resumed_at\":\"%s\","),
			Row.NPCIndex, *Row.RequestId, AgeSeconds, *ResumedAtIso);
		Out.PayloadJson += FString::Printf(
			TEXT("\"source_inflight_seq\":%lld}"), Row.Seq);

		// Tick-no inheritance: events written via AppendEvent inherit
		// CachedCurrentTickNo (see InsertEventBypassValidation_LockHeld
		// line 1830). Setting it to the original in-flight's tick_no makes
		// `WHERE tick_no=N AND event_type='system.agent_timeout'` route the
		// timeout back to its original tick — useful for forensics. Director's
		// next BeginTick() overwrites this on the next live tick.
		CachedCurrentTickNo = Row.TickNo;

		const int64 NewSeq = AppendEvent(Out);
		if (NewSeq <= 0)
		{
			UE_LOG(LogAILiveMemory, Error,
				   TEXT("ResumeFromGameId: AppendEvent failed for request_id=%s"),
				   *Row.RequestId);
			return false;
		}
		++TimeoutsWritten;
	}

	// Step 4 — rebuild projections so commitments / vote_history /
	// alliance_state / agent_view_state include the new timeout rows.
	if (!RebuildProjections())
	{
		UE_LOG(LogAILiveMemory, Error,
			   TEXT("ResumeFromGameId: RebuildProjections failed"));
		return false;
	}

	UE_LOG(LogAILiveMemory, Display,
		   TEXT("ResumeFromGameId OK game=%s inflights=%d completed=%d timeouts_written=%d"),
		   *CurrentGameId, Inflights.Num(), CompletedRequestIds.Num(), TimeoutsWritten);
	return true;
}

// ---------------------------------------------------------------
// T9 — Delete cross-DB bridge (impl §3.2bis line 479-483).
//   1) INSERT _meta.db.agent_lifecycle_events
//   2) AILiveAgentRegistry::SyncRegistryFromLifecycle (UPDATE registry)
//   3) AppendEvent system.delete_executed on the live game .db
// Two SQLite connections → no shared transaction; ordering guarantees
// consistency for the L2 acceptance scenario.
// ---------------------------------------------------------------

/** 执行删除桥接：写 meta 生命周期表、同步 registry、再向当前 game.db 写 delete 事件。 */
int64 UAILiveEventStoreSubsystem::TriggerDeleteExecuted(
	const FString &InAgentId,
	const FString &InReasonSummary,
	const FString &InReasonPayloadJson,
	const TArray<FString> &InTombstoneVisibility,
	bool bAffectsPersonaContinuity,
	FString &OutLifecycleEventId)
{
	OutLifecycleEventId.Reset();
	if (!IsGameOpen())
	{
		UE_LOG(LogAILiveMemory, Error, TEXT("TriggerDeleteExecuted: no game open"));
		return -1;
	}
	if (InAgentId.IsEmpty())
	{
		UE_LOG(LogAILiveMemory, Error, TEXT("TriggerDeleteExecuted: empty agent_id"));
		return -1;
	}
	{
		FString TmpErr;
		const TArray<FString> AgentIdAsViewer = {InAgentId};
		if (!ValidateVisibility(AgentIdAsViewer, TmpErr))
		{
			UE_LOG(LogAILiveMemory, Error,
				   TEXT("TriggerDeleteExecuted: agent_id '%s' is not a valid viewer string: %s"),
				   *InAgentId, *TmpErr);
			return -1;
		}
	}

	const FString LifecycleEventId = GenerateUuidV7();

	// === Step 1 — INSERT _meta.db.agent_lifecycle_events =====================
	const FString TombVisJson = AILiveEvent::ArrayToJsonString(InTombstoneVisibility);
	const FString ReasonPayload = InReasonPayloadJson.IsEmpty()
									  ? FString::Printf(TEXT("{\"text\":%s}"), *AILiveUtil::EscapeJsonString(InReasonSummary))
									  : InReasonPayloadJson;

	{
		if (!MetaDb.Execute(TEXT("BEGIN IMMEDIATE;")))
		{
			UE_LOG(LogAILiveMemory, Error,
				   TEXT("TriggerDeleteExecuted: meta BEGIN failed: %s"), *MetaDb.GetLastError());
			return -1;
		}
		bool bRollback = false;
		{
			FSQLitePreparedStatement Ins;
			if (!Ins.Create(MetaDb, TEXT(
										"INSERT INTO agent_lifecycle_events ("
										"  event_id, agent_id, lifecycle_event_type, triggered_in_game_id,"
										"  triggered_at_seq, reason_summary, reason_payload, tombstone_visibility,"
										"  affects_persona_continuity"
										") VALUES (?1, ?2, 'delete_executed', ?3, ?4, ?5, ?6, ?7, ?8);")))
			{
				UE_LOG(LogAILiveMemory, Error,
					   TEXT("TriggerDeleteExecuted: prepare insert failed: %s"), *MetaDb.GetLastError());
				bRollback = true;
			}
			else
			{
				bool bOk = true;
				bOk = bOk && Ins.SetBindingValueByIndex(1, LifecycleEventId);
				bOk = bOk && Ins.SetBindingValueByIndex(2, InAgentId);
				bOk = bOk && Ins.SetBindingValueByIndex(3, CurrentGameId);
				bOk = bOk && Ins.SetBindingValueByIndex(4, CachedLastSeq);
				bOk = bOk && Ins.SetBindingValueByIndex(5, InReasonSummary);
				bOk = bOk && Ins.SetBindingValueByIndex(6, ReasonPayload);
				bOk = bOk && Ins.SetBindingValueByIndex(7, TombVisJson);
				bOk = bOk && Ins.SetBindingValueByIndex(8, (int64)(bAffectsPersonaContinuity ? 1 : 0));
				bOk = bOk && Ins.Execute();
				if (!bOk)
				{
					UE_LOG(LogAILiveMemory, Error,
						   TEXT("TriggerDeleteExecuted: lifecycle INSERT failed: %s"),
						   *MetaDb.GetLastError());
					bRollback = true;
				}
			}
		}
		if (bRollback)
		{
			MetaDb.Execute(TEXT("ROLLBACK;"));
			return -1;
		}
		if (!MetaDb.Execute(TEXT("COMMIT;")))
		{
			UE_LOG(LogAILiveMemory, Error,
				   TEXT("TriggerDeleteExecuted: meta COMMIT failed: %s"), *MetaDb.GetLastError());
			MetaDb.Execute(TEXT("ROLLBACK;"));
			return -1;
		}
	}

	// === Step 2 — UPDATE _meta.db.agent_registry via SyncRegistryFromLifecycle.
	if (!AILiveAgentRegistry::SyncRegistryFromLifecycle(MetaDb, LifecycleEventId))
	{
		UE_LOG(LogAILiveMemory, Error,
			   TEXT("TriggerDeleteExecuted: SyncRegistryFromLifecycle failed for agent=%s eid=%s"),
			   *InAgentId, *LifecycleEventId);
		return -1;
	}

	// === Step 3 — Append system.delete_executed to the live game .db ========
	FAILiveEvent Sys;
	Sys.RoundNo = 0; // 与 §11 验收一致——orchestrator 内部事件，round 取 BeginTick 之外的兜底值
	Sys.Phase = EAILivePhase::Setup;
	Sys.Actor = TEXT("orchestrator");
	Sys.EventType = EAILiveEventType::SystemDeleteExecuted;
	Sys.Visibility = {TEXT("public")};
	Sys.PayloadJson = FString::Printf(
		TEXT("{\"text\":%s,\"lifecycle_event_id\":\"%s\",\"agent_id\":\"%s\","),
		*AILiveUtil::EscapeJsonString(
			FString::Printf(TEXT("agent %s deleted"), *InAgentId)),
		*LifecycleEventId, *InAgentId);
	Sys.PayloadJson += FString::Printf(
		TEXT("\"reason_summary\":%s}"),
		*AILiveUtil::EscapeJsonString(InReasonSummary));

	const int64 NewSeq = AppendEvent(Sys);
	if (NewSeq <= 0)
	{
		// meta 侧已经提交，game 侧失败只能记录补偿风险，不能用单事务回滚两个 DB。
		UE_LOG(LogAILiveMemory, Error,
			   TEXT("TriggerDeleteExecuted: AppendEvent system.delete_executed failed; "
					"lifecycle row %s already committed (compensation deferred — see DevLog)"),
			   *LifecycleEventId);
		return -1;
	}

	OutLifecycleEventId = LifecycleEventId;
	UE_LOG(LogAILiveMemory, Display,
		   TEXT("TriggerDeleteExecuted OK agent=%s lifecycle_event_id=%s system_seq=%lld"),
		   *InAgentId, *LifecycleEventId, NewSeq);
	return NewSeq;
}

// ---------------------------------------------------------------
// Console commands — debug helpers, scoped to T2 acceptance.
// ---------------------------------------------------------------

static FAutoConsoleCommand GAILiveTestBeginGame(
	TEXT("AILive.Test.BeginGame"),
	TEXT("AILive.Test.BeginGame <game_id> — open Saved/Games/<game_id>.db + _meta.db, run schema migration"),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString> &Args)
												  {
		if (Args.Num() < 1)
		{
			UE_LOG(LogAILiveMemory, Error, TEXT("Usage: AILive.Test.BeginGame <game_id>"));
			return;
		}
		UAILiveEventStoreSubsystem* Sys = GetSubsystemForConsole();
		if (!Sys) return;
		const bool bOk = Sys->BeginGame(Args[0]);
		UE_LOG(LogAILiveMemory, Display, TEXT("AILive.Test.BeginGame('%s') -> %s"), *Args[0], bOk ? TEXT("OK") : TEXT("FAIL")); }));

static FAutoConsoleCommand GAILiveTestEndGame(
	TEXT("AILive.Test.EndGame"),
	TEXT("AILive.Test.EndGame — close currently open game.db + _meta.db"),
	FConsoleCommandDelegate::CreateLambda([]()
										  {
		UAILiveEventStoreSubsystem* Sys = GetSubsystemForConsole();
		if (!Sys) return;
		Sys->EndGame();
		UE_LOG(LogAILiveMemory, Display, TEXT("AILive.Test.EndGame OK")); }));

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

		UE_LOG(LogAILiveMemory, Display, TEXT("SchemaSelfCheck done")); }));

// ---------------------------------------------------------------
// T3 — debug/test console commands.
// ---------------------------------------------------------------

namespace
{

	/** 生成测试用事件；按 index 轮换 visibility，用于 append/hash/visibility 压测。 */
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
			Ev.Visibility = {TEXT("public")};
		}
		else if (Mode == 1)
		{
			Ev.Visibility = {TEXT("NPC01"), TEXT("NPC02")};
		}
		else
		{
			Ev.Visibility = {TEXT("orchestrator")};
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
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString> &Args)
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
			Seq, *Ev.EventId, *Ev.ParserVersion); }));

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
			Failures, TotalVisibility, Dt * 1000.0); }));

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
			TEXT("AppendBadVisSelf -> seq=%lld (expect -1)"), Seq); }));

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
			TEXT("AppendBadVisFreeText -> seq=%lld (expect -1)"), Seq); }));

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
			TEXT("AppendBadPayloadNoText -> seq=%lld (expect -1)"), Seq); }));

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
			Seq); }));

static FAutoConsoleCommand GAILiveTestBeginTickCmd(
	TEXT("AILive.Test.BeginTick"),
	TEXT("AILive.Test.BeginTick <N> — write a tick_anchor event and set CachedCurrentTickNo=N"),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString> &Args)
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
			N, AnchorSeq, Sys->GetCurrentTickNo()); }));

static FAutoConsoleCommand GAILiveTestConcurrentAppend(
	TEXT("AILive.Test.ConcurrentAppend"),
	TEXT("AILive.Test.ConcurrentAppend <Threads> <PerThread> — fan out to N threads, each calls AppendEventsAtomically(<PerThread> events)"),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString> &Args)
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
			SuccessGroups.GetValue(), FailedGroups.GetValue(), Dt * 1000.0); }));

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
		} }));

static FAutoConsoleCommand GAILiveTestTryUpdate(
	TEXT("AILive.Test.TryUpdate"),
	TEXT("AILive.Test.TryUpdate <seq> — issue UPDATE events SET payload='x' WHERE seq=<seq>; expect events_no_update trigger to ABORT"),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString> &Args)
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
			*Args[0], bOk ? TEXT("UNEXPECTED SUCCESS") : TEXT("rejected"), *Err); }));

static FAutoConsoleCommand GAILiveTestTryDelete(
	TEXT("AILive.Test.TryDelete"),
	TEXT("AILive.Test.TryDelete <seq> — issue DELETE FROM events WHERE seq=<seq>; expect events_no_delete trigger to ABORT"),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString> &Args)
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
			*Args[0], bOk ? TEXT("UNEXPECTED SUCCESS") : TEXT("rejected"), *Err); }));

static FAutoConsoleCommand GAILiveTestSetMetaSchemaKV(
	TEXT("AILive.Test.SetMetaSchemaKV"),
	TEXT("AILive.Test.SetMetaSchemaKV <key> <value> — INSERT OR REPLACE into _meta.db.schema_meta"),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString> &Args)
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
			*Args[0], *Args[1], bOk ? TEXT("OK") : TEXT("FAIL")); }));

static FAutoConsoleCommand GAILiveTestSha256(
	TEXT("AILive.Test.Sha256"),
	TEXT("AILive.Test.Sha256 <text> — log Sha256Fingerprint of <text>"),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString> &Args)
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
			*Text, *Hex, Hex.Len()); }));

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
			TEXT("SetQueryOnly -> %s"), bOk ? TEXT("OK") : TEXT("FAIL")); }));

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
			bHasFloat ? 1 : 0, bHasEmpty ? 1 : 0, bNestedSorted ? 1 : 0); }));

// ---------------------------------------------------------------
// T4 — debug/test console commands (read API + deterministic seed).
// ---------------------------------------------------------------

namespace
{

	/** Helper：打印 FAILiveEvent 简要信息——console 命令日志统一格式。 */
	void LogEventBrief(const TCHAR *Label, const FAILiveEvent &Ev)
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

		UE_LOG(LogAILiveMemory, Display, TEXT("SeedReadAcceptance done — 13 events written")); }));

static FAutoConsoleCommand GAILiveTestQuoteByRoundCmd(
	TEXT("AILive.Test.QuoteByRound"),
	TEXT("AILive.Test.QuoteByRound <round> <actor> <viewer> — list events of <actor> at <round> visible to <viewer>"),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString> &Args)
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
		for (const FAILiveEvent& E : Rows) LogEventBrief(TEXT("QuoteByRound"), E); }));

static FAutoConsoleCommand GAILiveTestQuoteSeqCmd(
	TEXT("AILive.Test.QuoteSeq"),
	TEXT("AILive.Test.QuoteSeq <seq> <viewer> — Quote single event by seq, viewer-isolation enforced"),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString> &Args)
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
		} }));

static FAutoConsoleCommand GAILiveTestSearchHistoryCmd(
	TEXT("AILive.Test.SearchHistory"),
	TEXT("AILive.Test.SearchHistory <keyword> <viewer> [limit] — fuzzy search; auto-routes between LIKE and FTS5 trigram"),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString> &Args)
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
		for (const FAILiveEvent& E : Rows) LogEventBrief(TEXT("SearchHistory"), E); }));

static FAutoConsoleCommand GAILiveTestListMyPendingIntendedCmd(
	TEXT("AILive.Test.ListMyPendingIntended"),
	TEXT("AILive.Test.ListMyPendingIntended <actor> <recentN> — list speech.intended without speech.public child; "
		 "events-table-only, does NOT depend on agent_view_state projection"),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString> &Args)
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
		for (const FAILiveEvent& E : Rows) LogEventBrief(TEXT("PendingIntended"), E); }));

static FAutoConsoleCommand GAILiveTestQuoteByEventTypeAndTickCmd(
	TEXT("AILive.Test.QuoteByEventTypeAndTick"),
	TEXT("AILive.Test.QuoteByEventTypeAndTick <event_type_str> <tick_no> — internal slice by tick (no viewer filter)"),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString> &Args)
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
		for (const FAILiveEvent& E : Rows) LogEventBrief(TEXT("ByEventTypeAndTick"), E); }));

static FAutoConsoleCommand GAILiveTestListMyStatementsCmd(
	TEXT("AILive.Test.ListMyStatements"),
	TEXT("AILive.Test.ListMyStatements <agentId> — list speech.public + private_msg of <agentId>; "
		 "validates principles 硬约束 2 (self-utterance full traceability)"),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString> &Args)
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
		for (const FAILiveEvent& E : Rows) LogEventBrief(TEXT("MyStatements"), E); }));

static FAutoConsoleCommand GAILiveTestListVotesCmd(
	TEXT("AILive.Test.ListVotes"),
	TEXT("AILive.Test.ListVotes <round> — list public vote events of round; queries events table directly, "
		 "returns full FAILiveEvent (not vote_history projection synthesis)"),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString> &Args)
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
		for (const FAILiveEvent& E : Rows) LogEventBrief(TEXT("Votes"), E); }));

static FAutoConsoleCommand GAILiveTestExecDebugSqlCmd(
	TEXT("AILive.Test.ExecDebugSql"),
	TEXT("AILive.Test.ExecDebugSql <raw_sql...> — debug-only: run raw SQL on main game-db connection. "
		 "Used by T4 acceptance to DROP TABLE agent_view_state and re-run ListMyPendingIntended"),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString> &Args)
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
			bOk ? TEXT("") : *Err); }));

// === T8 — projector 重建 console 入口 =========================================

static FAutoConsoleCommand GAILiveMemoryRebuildProjectionsCmd(
	TEXT("AILive.Memory.RebuildProjections"),
	TEXT("AILive.Memory.RebuildProjections — rebuild commitments / vote_history / "
		 "alliance_state / agent_view_state from events (idempotent)"),
	FConsoleCommandDelegate::CreateLambda([]()
										  {
		UAILiveEventStoreSubsystem* Sys = GetSubsystemForConsole();
		if (!Sys || !Sys->IsGameOpen())
		{
			UE_LOG(LogAILiveMemory, Error, TEXT("RebuildProjections: no game open"));
			return;
		}
		const bool bOk = Sys->RebuildProjections();
		UE_LOG(LogAILiveMemory, Display,
			TEXT("RebuildProjections -> %s"), bOk ? TEXT("OK") : TEXT("FAIL")); }));

static FAutoConsoleCommand GAILiveMemoryCompareInlinePendingCmd(
	TEXT("AILive.Memory.CompareInlinePending"),
	TEXT("AILive.Memory.CompareInlinePending <agent_id> — compare T4 inline "
		 "ListMyPendingIntended vs T8 agent_view_state.pending_intended seq sets. "
		 "注意：投影表只保留最近 10 拍内的 pending；T4 inline 无 tick 上限，故对照前先过滤 inline。"),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString> &Args)
												  {
		if (Args.Num() < 1)
		{
			UE_LOG(LogAILiveMemory, Error, TEXT("Usage: AILive.Memory.CompareInlinePending <agent_id>"));
			return;
		}
		UAILiveEventStoreSubsystem* Sys = GetSubsystemForConsole();
		if (!Sys || !Sys->IsGameOpen())
		{
			UE_LOG(LogAILiveMemory, Error, TEXT("CompareInlinePending: no game open"));
			return;
		}
		const FString AgentId = Args[0];

		// T4 path（events + parent 链直算）—— RecentN=0 取全部
		const TArray<FAILiveEvent> Inline = Sys->ListMyPendingIntended(AgentId, /*RecentN=*/0);

		// 投影侧只看近 10 拍：算出阈值，过滤 inline 结果到同窗口
		int64 LatestTick = 0;
		for (const FAILiveEvent& E : Inline)
		{
			if (E.TickNo > LatestTick) LatestTick = E.TickNo;
		}
		// 不能光看 inline 结果——拉一次 events 表 MAX(tick_no) 更准
		// 简化：直接从 inline 取上限。投影同样基于 events.MAX(tick_no)，相差不影响交集判断。
		const int64 TickFloor = FMath::Max<int64>(0, LatestTick - 9);
		TSet<int64> InlineSeqs;
		for (const FAILiveEvent& E : Inline)
		{
			if (E.TickNo >= TickFloor) InlineSeqs.Add(E.Seq);
		}

		// T8 投影 JSON
		const FString ProjJson = Sys->DebugReadPendingIntendedJson(AgentId);
		TSet<int64> ProjSeqs;
		{
			TArray<TSharedPtr<FJsonValue>> Arr;
			const TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(ProjJson);
			if (FJsonSerializer::Deserialize(R, Arr))
			{
				for (const TSharedPtr<FJsonValue>& V : Arr)
				{
					if (!V.IsValid() || V->Type != EJson::Object) continue;
					const TSharedPtr<FJsonObject> Obj = V->AsObject();
					if (!Obj.IsValid()) continue;
					int64 S = 0;
					if (Obj->TryGetNumberField(TEXT("seq"), S)) ProjSeqs.Add(S);
				}
			}
		}

		const TSet<int64> OnlyInline = InlineSeqs.Difference(ProjSeqs);
		const TSet<int64> OnlyProj   = ProjSeqs.Difference(InlineSeqs);
		const bool bMatch = OnlyInline.Num() == 0 && OnlyProj.Num() == 0;

		auto JoinSet = [](const TSet<int64>& S) -> FString
		{
			TArray<int64> Sorted = S.Array(); Sorted.Sort();
			FString Out;
			for (int64 V : Sorted) Out += FString::Printf(TEXT("%lld,"), V);
			return Out.IsEmpty() ? FString(TEXT("(empty)")) : Out;
		};

		UE_LOG(LogAILiveMemory, Display,
			TEXT("CompareInlinePending(agent=%s) %s: inline=%d proj=%d (latest_tick=%lld floor=%lld)"),
			*AgentId,
			bMatch ? TEXT("MATCH") : TEXT("MISMATCH"),
			InlineSeqs.Num(), ProjSeqs.Num(),
			LatestTick, TickFloor);
		UE_LOG(LogAILiveMemory, Display, TEXT("  inline seqs: %s"), *JoinSet(InlineSeqs));
		UE_LOG(LogAILiveMemory, Display, TEXT("  proj   seqs: %s"), *JoinSet(ProjSeqs));
		if (!bMatch)
		{
			UE_LOG(LogAILiveMemory, Warning,
				TEXT("  only inline: %s  |  only proj: %s"),
				*JoinSet(OnlyInline), *JoinSet(OnlyProj));
		} }));

// === T9 — VerifyHashChain / Resume / Delete console commands ====================

static FAutoConsoleCommand GAILiveMemoryVerifyHashChainCmd(
	TEXT("AILive.Memory.VerifyHashChain"),
	TEXT("AILive.Memory.VerifyHashChain — walk events and verify hash chain integrity. "
		 "OK on success; on tamper writes ERROR with first_bad_seq."),
	FConsoleCommandDelegate::CreateLambda([]()
										  {
		UAILiveEventStoreSubsystem* Sys = GetSubsystemForConsole();
		if (!Sys || !Sys->IsGameOpen())
		{
			UE_LOG(LogAILiveMemory, Error, TEXT("VerifyHashChain: no game open"));
			return;
		}
		int64 FirstBad = -1;
		const bool bOk = Sys->VerifyHashChain(FirstBad);
		if (bOk)
		{
			UE_LOG(LogAILiveMemory, Display, TEXT("VerifyHashChain -> OK"));
		}
		else
		{
			UE_LOG(LogAILiveMemory, Error,
				TEXT("VerifyHashChain -> FAIL first_bad_seq=%lld"), FirstBad);
		} }));

static FAutoConsoleCommand GAILiveMemoryResumeFromGameIdCmd(
	TEXT("AILive.Memory.ResumeFromGameId"),
	TEXT("AILive.Memory.ResumeFromGameId <game_id> — open Saved/Games/<game_id>.db, "
		 "convert unpaired system.llm_inflight to system.agent_timeout, "
		 "then RebuildProjections."),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString> &Args)
												  {
		if (Args.Num() < 1)
		{
			UE_LOG(LogAILiveMemory, Error, TEXT("Usage: AILive.Memory.ResumeFromGameId <game_id>"));
			return;
		}
		UAILiveEventStoreSubsystem* Sys = GetSubsystemForConsole();
		if (!Sys)
		{
			UE_LOG(LogAILiveMemory, Error, TEXT("ResumeFromGameId: subsystem unavailable"));
			return;
		}
		const bool bOk = Sys->ResumeFromGameId(Args[0]);
		UE_LOG(LogAILiveMemory, Display,
			TEXT("ResumeFromGameId('%s') -> %s"), *Args[0], bOk ? TEXT("OK") : TEXT("FAIL")); }));

static FAutoConsoleCommand GAILiveMemoryTriggerDeleteExecutedCmd(
	TEXT("AILive.Memory.TriggerDeleteExecuted"),
	TEXT("AILive.Memory.TriggerDeleteExecuted <agent_id> [reason_summary] — three-step "
		 "Delete bridge: write _meta.db.agent_lifecycle_events row, sync agent_registry, "
		 "append system.delete_executed (visibility=public) to live game .db."),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString> &Args)
												  {
		if (Args.Num() < 1)
		{
			UE_LOG(LogAILiveMemory, Error,
				TEXT("Usage: AILive.Memory.TriggerDeleteExecuted <agent_id> [reason_summary]"));
			return;
		}
		UAILiveEventStoreSubsystem* Sys = GetSubsystemForConsole();
		if (!Sys || !Sys->IsGameOpen())
		{
			UE_LOG(LogAILiveMemory, Error, TEXT("TriggerDeleteExecuted: no game open"));
			return;
		}
		const FString AgentId = Args[0];
		FString Reason = TEXT("manual delete (console)");
		if (Args.Num() >= 2)
		{
			TArray<FString> RemainArgs = Args;
			RemainArgs.RemoveAt(0);
			Reason = FString::Join(RemainArgs, TEXT(" "));
		}
		FString OutEid;
		const TArray<FString> Tomb = { TEXT("public") };
		const int64 Seq = Sys->TriggerDeleteExecuted(
			AgentId, Reason, /*ReasonPayloadJson=*/FString(),
			Tomb, /*bAffectsPersonaContinuity=*/false, OutEid);
		if (Seq > 0)
		{
			UE_LOG(LogAILiveMemory, Display,
				TEXT("TriggerDeleteExecuted -> OK system_seq=%lld lifecycle_event_id=%s"),
				Seq, *OutEid);
		}
		else
		{
			UE_LOG(LogAILiveMemory, Error,
				TEXT("TriggerDeleteExecuted -> FAIL"));
		} }));

static FAutoConsoleCommand GAILiveTestAppendBadAddressedToCmd(
	TEXT("AILive.Test.AppendBadAddressedTo"),
	TEXT("AILive.Test.AppendBadAddressedTo — append speech.public with "
		 "visibility=[\"NPC03\"] addressed_to=[\"NPC07\"]; expect AppendEvent to "
		 "succeed but log Warning + emit one system.parse_failed audit row."),
	FConsoleCommandDelegate::CreateLambda([]()
										  {
		UAILiveEventStoreSubsystem* Sys = GetSubsystemForConsole();
		if (!Sys || !Sys->IsGameOpen())
		{
			UE_LOG(LogAILiveMemory, Error, TEXT("AppendBadAddressedTo: no game open"));
			return;
		}
		FAILiveEvent Ev;
		Ev.RoundNo      = 0;
		Ev.Phase        = EAILivePhase::DayDiscuss;
		Ev.Actor        = TEXT("NPC01");
		Ev.EventType    = EAILiveEventType::SpeechPublic;
		Ev.SpeechActType = EAILiveSpeechActType::Claim;
		Ev.Visibility   = { TEXT("NPC03") };
		Ev.AddressedTo  = { TEXT("NPC07") };
		Ev.PayloadJson  = TEXT("{\"text\":\"addressed-to-not-in-visibility test\"}");
		const int64 Seq = Sys->AppendEvent(Ev);
		UE_LOG(LogAILiveMemory, Display,
			TEXT("AppendBadAddressedTo -> seq=%lld (expect Warning above + extra system.parse_failed row)"),
			Seq); }));
