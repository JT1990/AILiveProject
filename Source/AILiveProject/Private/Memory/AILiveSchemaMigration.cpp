// =============================================================================
// 中文教学：AILiveSchemaMigration.cpp —— SQLite DDL(数据定义语言) + schema 版本迁移
//
// 这是什么：
//   集中存放所有「建表 / 建索引 / 建触发器 / 建 FTS」的 SQL 字符串常量，以及
//   schema 版本演进的「迁移函数」。
//
// 文件结构：
//   1) GameDb_*  ：当前局 .db 的所有 DDL 语句（events / commitments / projector 表 ...）
//   2) MetaDb_*  ：跨局 _meta.db 的所有 DDL 语句（agent_registry / lifecycle_events ...）
//   3) ApplyMigrationV0ToV1：把 schema 版本从 0 升到 1（首次建库走这个）
//
// 为什么单独成文件：
//   EventStoreSubsystem.cpp 已经 4600 行，把巨大的 DDL 字符串再塞进去会让
//   主流程更难读。把它们抽到本文件，主文件只 #include 这边的函数即可。
//
// 关键概念：
//   1) AppendOnly via TRIGGER … BEFORE UPDATE/DELETE → ABORT
//      events 表绝不允许 UPDATE 或 DELETE。SQLite 触发器在写之前检查，
//      不合规直接 ABORT 整个事务。这是 hash 链一致性的硬保证。
//
//   2) FTS5 + tokenizer
//      SQLite FTS5（全文搜索扩展）支持的中文 tokenizer 取决于 SQLite 编译选项。
//      项目运行时 detect 是「trigram」（短词搜索）还是「unicode61」（拉丁词）。
//      SearchHistory 据此决定走 MATCH 还是 LIKE。
//
//   3) IF NOT EXISTS
//      DDL 加这个保证幂等。第二次 BeginGame 不会因表已存在报错。
// =============================================================================

#include "CoreMinimal.h"
#include "Memory/AILiveEventTypes.h"
#include "SQLiteDatabase.h"

namespace AILiveSchemaMigration
{
	// schema 版本 0 → 1 的迁移函数。中文教学：未来 schema 改动要加 V1ToV2 / V2ToV3 等。
	// 调用方按版本号顺序串起来跑（在 EventStoreSubsystem::RunMigrations 里）。
	bool ApplyMigrationV0ToV1(FSQLiteDatabase &InDb, bool bIsMetaDb, const TCHAR *FtsTokenizer);
}

namespace
{

	// =============================================================
	// Single-game .db DDL (impl §3.2). Order matters: events before
	// events_fts triggers; tables before their indexes.
	// =============================================================

	static const TCHAR *GameDb_CreateSchemaMeta = TEXT(
		"CREATE TABLE IF NOT EXISTS schema_meta ("
		"  key   TEXT PRIMARY KEY,"
		"  value TEXT NOT NULL"
		");");

	static const TCHAR *GameDb_CreateEvents = TEXT(
		"CREATE TABLE IF NOT EXISTS events ("
		"  event_id        TEXT PRIMARY KEY,"
		"  game_id         TEXT NOT NULL,"
		"  seq             INTEGER NOT NULL,"
		"  tick_no         INTEGER NOT NULL DEFAULT 0,"
		"  round_no        INTEGER NOT NULL,"
		"  phase           TEXT NOT NULL,"
		"  actor           TEXT NOT NULL,"
		"  event_type      TEXT NOT NULL,"
		"  speech_act_type TEXT,"
		"  visibility      TEXT NOT NULL,"
		"  addressed_to    TEXT,"
		"  payload         TEXT NOT NULL,"
		"  payload_text    TEXT GENERATED ALWAYS AS (json_extract(payload, '$.text')) STORED,"
		"  parent_event_id TEXT,"
		"  parser_version  TEXT NOT NULL DEFAULT '1',"
		"  raw_llm_output  TEXT,"
		"  prev_event_hash TEXT NOT NULL,"
		"  event_hash      TEXT NOT NULL,"
		"  wall_clock      TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')),"
		"  UNIQUE (game_id, seq)"
		");");

	static const TCHAR *GameDb_IdxEventsGameRound = TEXT(
		"CREATE INDEX IF NOT EXISTS idx_events_game_round ON events (game_id, round_no, seq);");
	static const TCHAR *GameDb_IdxEventsActor = TEXT(
		"CREATE INDEX IF NOT EXISTS idx_events_actor ON events (game_id, actor, seq);");
	static const TCHAR *GameDb_IdxEventsType = TEXT(
		"CREATE INDEX IF NOT EXISTS idx_events_type ON events (game_id, event_type, round_no);");
	static const TCHAR *GameDb_IdxEventsGameTick = TEXT(
		"CREATE INDEX IF NOT EXISTS idx_events_game_tick ON events (game_id, tick_no, seq);");

	static const TCHAR *GameDb_TriggerEventsNoUpdate = TEXT(
		"CREATE TRIGGER IF NOT EXISTS events_no_update BEFORE UPDATE ON events "
		"BEGIN SELECT RAISE(ABORT, 'events table is append-only; UPDATE forbidden'); END;");
	static const TCHAR *GameDb_TriggerEventsNoDelete = TEXT(
		"CREATE TRIGGER IF NOT EXISTS events_no_delete BEFORE DELETE ON events "
		"BEGIN SELECT RAISE(ABORT, 'events table is append-only; DELETE forbidden'); END;");

	static const TCHAR *GameDb_CreateEventVisibility = TEXT(
		"CREATE TABLE IF NOT EXISTS event_visibility ("
		"  event_id TEXT NOT NULL,"
		"  viewer   TEXT NOT NULL,"
		"  PRIMARY KEY (event_id, viewer),"
		"  FOREIGN KEY (event_id) REFERENCES events(event_id) ON DELETE CASCADE"
		");");
	static const TCHAR *GameDb_IdxEventVisibilityViewer = TEXT(
		"CREATE INDEX IF NOT EXISTS idx_event_visibility_viewer ON event_visibility(viewer);");

	static const TCHAR *GameDb_CreateEventAddressedTo = TEXT(
		"CREATE TABLE IF NOT EXISTS event_addressed_to ("
		"  event_id TEXT NOT NULL,"
		"  target   TEXT NOT NULL,"
		"  PRIMARY KEY (event_id, target),"
		"  FOREIGN KEY (event_id) REFERENCES events(event_id) ON DELETE CASCADE"
		");");
	static const TCHAR *GameDb_IdxEventAddressedTarget = TEXT(
		"CREATE INDEX IF NOT EXISTS idx_event_addressed_target ON event_addressed_to(target);");

	static const TCHAR *GameDb_TriggerEventsFtsAi = TEXT(
		"CREATE TRIGGER IF NOT EXISTS events_fts_ai AFTER INSERT ON events BEGIN "
		"  INSERT INTO events_fts(rowid, payload_text) VALUES (new.rowid, new.payload_text); "
		"END;");
	static const TCHAR *GameDb_TriggerEventsFtsAd = TEXT(
		"CREATE TRIGGER IF NOT EXISTS events_fts_ad AFTER DELETE ON events BEGIN "
		"  INSERT INTO events_fts(events_fts, rowid, payload_text) "
		"    VALUES('delete', old.rowid, old.payload_text); "
		"END;");

	static const TCHAR *GameDb_CreateAgentViewState = TEXT(
		"CREATE TABLE IF NOT EXISTS agent_view_state ("
		"  game_id          TEXT NOT NULL,"
		"  agent_id         TEXT NOT NULL,"
		"  as_of_seq        INTEGER NOT NULL,"
		"  alive_players    TEXT NOT NULL,"
		"  known_roles      TEXT NOT NULL,"
		"  my_commitments   TEXT NOT NULL,"
		"  vote_history     TEXT NOT NULL,"
		"  pending_intended TEXT NOT NULL DEFAULT '[]',"
		"  PRIMARY KEY (game_id, agent_id, as_of_seq)"
		");");

	static const TCHAR *GameDb_CreateCommitments = TEXT(
		"CREATE TABLE IF NOT EXISTS commitments ("
		"  game_id         TEXT NOT NULL,"
		"  agent_id        TEXT NOT NULL,"
		"  round_no        INTEGER NOT NULL,"
		"  seq             INTEGER NOT NULL,"
		"  commitment_type TEXT NOT NULL,"
		"  target          TEXT,"
		"  text            TEXT NOT NULL,"
		"  status          TEXT NOT NULL DEFAULT 'active',"
		"  PRIMARY KEY (game_id, seq, commitment_type)"
		");");
	static const TCHAR *GameDb_IdxCommitmentsAgent = TEXT(
		"CREATE INDEX IF NOT EXISTS idx_commitments_agent ON commitments(game_id, agent_id, round_no);");

	static const TCHAR *GameDb_CreateVoteHistory = TEXT(
		"CREATE TABLE IF NOT EXISTS vote_history ("
		"  game_id  TEXT NOT NULL,"
		"  round_no INTEGER NOT NULL,"
		"  seq      INTEGER NOT NULL,"
		"  voter    TEXT NOT NULL,"
		"  target   TEXT NOT NULL,"
		"  PRIMARY KEY (game_id, seq)"
		");");

	static const TCHAR *GameDb_CreateAllianceState = TEXT(
		"CREATE TABLE IF NOT EXISTS alliance_state ("
		"  game_id          TEXT NOT NULL,"
		"  alliance_id      TEXT NOT NULL,"
		"  members          TEXT NOT NULL,"
		"  proposed_at_seq  INTEGER NOT NULL,"
		"  accepted_at_seq  INTEGER,"
		"  betrayed_at_seq  INTEGER,"
		"  terms            TEXT NOT NULL,"
		"  PRIMARY KEY (game_id, alliance_id)"
		");");

	static const TCHAR *GameDb_CreateEventLogSummaries = TEXT(
		"CREATE TABLE IF NOT EXISTS event_log_summaries ("
		"  game_id          TEXT NOT NULL,"
		"  viewer_agent     TEXT NOT NULL,"
		"  round_start      INTEGER NOT NULL,"
		"  round_end        INTEGER NOT NULL,"
		"  summary_text     TEXT NOT NULL CHECK (length(summary_text) <= 2000),"
		"  summarizer_model TEXT NOT NULL,"
		"  source_seq_start INTEGER NOT NULL,"
		"  source_seq_end   INTEGER NOT NULL,"
		"  created_at       TEXT DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')),"
		"  PRIMARY KEY (game_id, viewer_agent, round_start, round_end),"
		"  CHECK (source_seq_start <= source_seq_end)"
		");");

	static const TCHAR *GameDb_CreateBelViolations = TEXT(
		"CREATE TABLE IF NOT EXISTS bel_violations ("
		"  game_id        TEXT NOT NULL,"
		"  agent_id       TEXT NOT NULL,"
		"  round_no       INTEGER NOT NULL,"
		"  seq            INTEGER NOT NULL,"
		"  violation_type TEXT NOT NULL,"
		"  detail         TEXT NOT NULL,"
		"  action_taken   TEXT NOT NULL,"
		"  PRIMARY KEY (game_id, seq, violation_type)"
		");");

	static const TCHAR *GameDb_CreateLeakageAudits = TEXT(
		"CREATE TABLE IF NOT EXISTS leakage_audits ("
		"  game_id          TEXT NOT NULL,"
		"  agent_id         TEXT NOT NULL,"
		"  round_no         INTEGER NOT NULL,"
		"  seq              INTEGER NOT NULL,"
		"  judge_model      TEXT NOT NULL,"
		"  leakage_type     TEXT,"
		"  leakage_score    REAL,"
		"  judge_rationale  TEXT,"
		"  PRIMARY KEY (game_id, seq)"
		");");

	static const TCHAR *GameDb_CreateAgentCalibration = TEXT(
		"CREATE TABLE IF NOT EXISTS agent_calibration ("
		"  game_id              TEXT NOT NULL,"
		"  agent_id             TEXT NOT NULL,"
		"  model_vendor         TEXT NOT NULL,"
		"  model_name           TEXT NOT NULL,"
		"  bid_offset           REAL DEFAULT 0.0,"
		"  role                 TEXT,"
		"  faction              TEXT,"
		"  private_goal         TEXT,"
		"  alliance_members_json TEXT NOT NULL DEFAULT '[]',"
		"  seq_start            INTEGER NOT NULL DEFAULT 1,"
		"  PRIMARY KEY (game_id, agent_id)"
		");");

	static const TCHAR *GameDb_CreateGameState = TEXT(
		"CREATE TABLE IF NOT EXISTS game_state ("
		"  game_id       TEXT PRIMARY KEY,"
		"  current_round INTEGER NOT NULL,"
		"  current_phase TEXT NOT NULL,"
		"  last_seq      INTEGER NOT NULL,"
		"  state_blob    TEXT NOT NULL"
		");");

	// =============================================================
	// _meta.db DDL (impl §3.2bis)
	// =============================================================

	static const TCHAR *MetaDb_CreateSchemaMeta = TEXT(
		"CREATE TABLE IF NOT EXISTS schema_meta ("
		"  key   TEXT PRIMARY KEY,"
		"  value TEXT NOT NULL"
		");");

	static const TCHAR *MetaDb_CreateAgentLifecycleEvents = TEXT(
		"CREATE TABLE IF NOT EXISTS agent_lifecycle_events ("
		"  event_id                       TEXT PRIMARY KEY,"
		"  agent_id                       TEXT NOT NULL,"
		"  lifecycle_event_type           TEXT NOT NULL,"
		"  triggered_in_game_id           TEXT,"
		"  triggered_at_seq               INTEGER,"
		"  reason_summary                 TEXT NOT NULL,"
		"  reason_payload                 TEXT NOT NULL,"
		"  tombstone_visibility           TEXT NOT NULL,"
		"  affects_persona_continuity     INTEGER NOT NULL DEFAULT 0,"
		"  wall_clock                     TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now'))"
		");");
	static const TCHAR *MetaDb_IdxLifecycleAgent = TEXT(
		"CREATE INDEX IF NOT EXISTS idx_lifecycle_agent ON agent_lifecycle_events (agent_id, wall_clock);");
	static const TCHAR *MetaDb_IdxLifecycleGame = TEXT(
		"CREATE INDEX IF NOT EXISTS idx_lifecycle_game ON agent_lifecycle_events (triggered_in_game_id);");
	static const TCHAR *MetaDb_TriggerLifecycleNoUpdate = TEXT(
		"CREATE TRIGGER IF NOT EXISTS lifecycle_no_update BEFORE UPDATE ON agent_lifecycle_events "
		"BEGIN SELECT RAISE(ABORT, 'agent_lifecycle_events is append-only'); END;");
	static const TCHAR *MetaDb_TriggerLifecycleNoDelete = TEXT(
		"CREATE TRIGGER IF NOT EXISTS lifecycle_no_delete BEFORE DELETE ON agent_lifecycle_events "
		"BEGIN SELECT RAISE(ABORT, 'agent_lifecycle_events is append-only'); END;");

	static const TCHAR *MetaDb_CreateAgentRegistry = TEXT(
		"CREATE TABLE IF NOT EXISTS agent_registry ("
		"  agent_id           TEXT PRIMARY KEY,"
		"  persona_version    INTEGER NOT NULL DEFAULT 1,"
		"  status             TEXT NOT NULL DEFAULT 'active',"
		"  created_at         TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')),"
		"  deleted_at         TEXT,"
		"  last_seen_game_id  TEXT,"
		"  last_updated_at    TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now'))"
		");");
	static const TCHAR *MetaDb_IdxRegistryStatus = TEXT(
		"CREATE INDEX IF NOT EXISTS idx_registry_status ON agent_registry (status);");

	static bool ExecOrFail(FSQLiteDatabase &InDb, const TCHAR *InSql, const TCHAR *InContext)
	{
		if (!InDb.Execute(InSql))
		{
			UE_LOG(LogAILiveMemory, Error, TEXT("DDL failed [%s]: %s"), InContext, *InDb.GetLastError());
			return false;
		}
		return true;
	}

} // namespace anonymous

namespace AILiveSchemaMigration
{

	bool ApplyMigrationV0ToV1(FSQLiteDatabase &InDb, bool bIsMetaDb, const TCHAR *FtsTokenizer)
	{
		// 开启一次显式事务：下面所有建表/建索引/建触发器要么全部成功，要么失败后全部撤销。
		if (!InDb.Execute(TEXT("BEGIN IMMEDIATE;")))
		{
			UE_LOG(LogAILiveMemory, Error, TEXT("BEGIN failed: %s"), *InDb.GetLastError());
			return false;
		}

		// 本函数内部复用的小回滚函数。任何一步 DDL 失败，就 ROLLBACK 并把整体迁移标记为失败。
		auto Rollback = [&InDb]() -> bool
		{
			InDb.Execute(TEXT("ROLLBACK;"));
			return false;
		};

		// bIsMetaDb=true：初始化跨局 _meta.db，只创建 agent 注册表/生命周期相关 schema。
		if (bIsMetaDb)
		{
			// 把要执行的 SQL 常量排成数组，保证按固定顺序执行。
			const TCHAR *Statements[] =
				{
					MetaDb_CreateSchemaMeta,
					MetaDb_CreateAgentLifecycleEvents,
					MetaDb_IdxLifecycleAgent,
					MetaDb_IdxLifecycleGame,
					MetaDb_TriggerLifecycleNoUpdate,
					MetaDb_TriggerLifecycleNoDelete,
					MetaDb_CreateAgentRegistry,
					MetaDb_IdxRegistryStatus,
				};
			for (const TCHAR *Sql : Statements)
			{
				// 逐条执行 DDL。ExecOrFail 会在失败时输出具体 SQLite 错误。
				if (!ExecOrFail(InDb, Sql, TEXT("_meta.db DDL")))
				{
					return Rollback();
				}
			}
		}
		else
		{
			// bIsMetaDb=false：初始化单局 game.db。game.db 需要 FTS 全文搜索 tokenizer。
			check(FtsTokenizer != nullptr);

			// FTS 表依赖 events 表，所以先创建普通表、索引、append-only 触发器等基础结构。
			const TCHAR *PreFtsStatements[] =
				{
					GameDb_CreateSchemaMeta,
					GameDb_CreateEvents,
					GameDb_IdxEventsGameRound,
					GameDb_IdxEventsActor,
					GameDb_IdxEventsType,
					GameDb_IdxEventsGameTick,
					GameDb_TriggerEventsNoUpdate,
					GameDb_TriggerEventsNoDelete,
					GameDb_CreateEventVisibility,
					GameDb_IdxEventVisibilityViewer,
					GameDb_CreateEventAddressedTo,
					GameDb_IdxEventAddressedTarget,
				};
			for (const TCHAR *Sql : PreFtsStatements)
			{
				// 执行 FTS 之前的基础 DDL；任意失败则回滚整个 migration。
				if (!ExecOrFail(InDb, Sql, TEXT("game.db DDL")))
				{
					return Rollback();
				}
			}

			// FTS tokenizer 运行时探测得到，不能完全写死成 static const TCHAR*，所以这里动态拼 SQL。
			const FString FtsCreateSql = FString::Printf(
				TEXT("CREATE VIRTUAL TABLE IF NOT EXISTS events_fts USING fts5("
					 "payload_text, content='events', content_rowid='rowid', tokenize='%s');"),
				FtsTokenizer);
			// 创建 events_fts 虚拟表：它用于对 events.payload_text 做全文搜索。
			if (!ExecOrFail(InDb, *FtsCreateSql, TEXT("events_fts virtual table")))
			{
				return Rollback();
			}
			// events 插入后，同步把 payload_text 写入 FTS 索引。
			if (!ExecOrFail(InDb, GameDb_TriggerEventsFtsAi, TEXT("events_fts_ai")))
			{
				return Rollback();
			}
			// events 删除后，同步从 FTS 索引删除对应内容；虽然 events 本身禁止 DELETE，但触发器仍保持 schema 完整。
			if (!ExecOrFail(InDb, GameDb_TriggerEventsFtsAd, TEXT("events_fts_ad")))
			{
				return Rollback();
			}

			// FTS 建好后，再创建不依赖 FTS 的 projector/派生状态表。
			const TCHAR *PostFtsStatements[] =
				{
					GameDb_CreateAgentViewState,
					GameDb_CreateCommitments,
					GameDb_IdxCommitmentsAgent,
					GameDb_CreateVoteHistory,
					GameDb_CreateAllianceState,
					GameDb_CreateEventLogSummaries,
					GameDb_CreateBelViolations,
					GameDb_CreateLeakageAudits,
					GameDb_CreateAgentCalibration,
					GameDb_CreateGameState,
				};
			for (const TCHAR *Sql : PostFtsStatements)
			{
				// 执行剩余 game.db DDL；仍然处在同一个事务里。
				if (!ExecOrFail(InDb, Sql, TEXT("game.db DDL")))
				{
					return Rollback();
				}
			}
		}

		// 所有 DDL 成功后，把 schema_meta.schema_version 写成 1，表示 V0->V1 迁移已经完成。
		if (!InDb.Execute(TEXT("INSERT OR REPLACE INTO schema_meta(key, value) VALUES('schema_version', '1');")))
		{
			UE_LOG(LogAILiveMemory, Error, TEXT("schema_version write failed: %s"), *InDb.GetLastError());
			return Rollback();
		}

		// 提交事务。提交成功后，前面的建表/建索引/建触发器才真正落地。
		if (!InDb.Execute(TEXT("COMMIT;")))
		{
			UE_LOG(LogAILiveMemory, Error, TEXT("COMMIT failed: %s"), *InDb.GetLastError());
			return Rollback();
		}

		// 返回 true 告诉调用方：本次 schema migration 成功。
		return true;
	}

} // namespace AILiveSchemaMigration
