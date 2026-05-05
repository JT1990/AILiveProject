#include "Memory/AILiveAgentRegistry.h"

#include "Memory/AILiveEventTypes.h" // LogAILiveMemory
#include "SQLiteDatabase.h"
#include "SQLitePreparedStatement.h"

namespace AILiveAgentRegistry
{

bool SyncRegistryFromLifecycle(FSQLiteDatabase& MetaDb, const FString& LifecycleEventId)
{
	if (!MetaDb.IsValid())
	{
		UE_LOG(LogAILiveMemory, Error, TEXT("SyncRegistryFromLifecycle: MetaDb not open"));
		return false;
	}
	if (LifecycleEventId.IsEmpty())
	{
		UE_LOG(LogAILiveMemory, Error, TEXT("SyncRegistryFromLifecycle: empty LifecycleEventId"));
		return false;
	}

	FString AgentId;
	FString LifecycleType;
	FString WallClock;
	FString TriggeredInGameId;
	{
		FSQLitePreparedStatement Stmt;
		if (!Stmt.Create(MetaDb,
			TEXT("SELECT agent_id, lifecycle_event_type, wall_clock, "
			     "       COALESCE(triggered_in_game_id, '') "
			     "FROM agent_lifecycle_events WHERE event_id = ?1 LIMIT 1;")))
		{
			UE_LOG(LogAILiveMemory, Error,
				TEXT("SyncRegistryFromLifecycle: prepare select failed: %s"),
				*MetaDb.GetLastError());
			return false;
		}
		Stmt.SetBindingValueByIndex(1, LifecycleEventId);
		const ESQLitePreparedStatementStepResult Step = Stmt.Step();
		if (Step != ESQLitePreparedStatementStepResult::Row)
		{
			UE_LOG(LogAILiveMemory, Error,
				TEXT("SyncRegistryFromLifecycle: lifecycle row not found event_id=%s"),
				*LifecycleEventId);
			return false;
		}
		Stmt.GetColumnValueByIndex(0, AgentId);
		Stmt.GetColumnValueByIndex(1, LifecycleType);
		Stmt.GetColumnValueByIndex(2, WallClock);
		Stmt.GetColumnValueByIndex(3, TriggeredInGameId);
	}

	if (AgentId.IsEmpty())
	{
		UE_LOG(LogAILiveMemory, Error,
			TEXT("SyncRegistryFromLifecycle: lifecycle row has empty agent_id"));
		return false;
	}

	if (!MetaDb.Execute(TEXT("BEGIN IMMEDIATE;")))
	{
		UE_LOG(LogAILiveMemory, Error,
			TEXT("SyncRegistryFromLifecycle: BEGIN failed: %s"), *MetaDb.GetLastError());
		return false;
	}

	auto Rollback = [&MetaDb]() -> bool
	{
		MetaDb.Execute(TEXT("ROLLBACK;"));
		return false;
	};

	{
		FSQLitePreparedStatement IfMissing;
		if (!IfMissing.Create(MetaDb,
			TEXT("INSERT OR IGNORE INTO agent_registry (agent_id, last_seen_game_id) "
			     "VALUES (?1, NULLIF(?2, ''));")))
		{
			UE_LOG(LogAILiveMemory, Error,
				TEXT("SyncRegistryFromLifecycle: prepare insert failed: %s"),
				*MetaDb.GetLastError());
			return Rollback();
		}
		IfMissing.SetBindingValueByIndex(1, AgentId);
		IfMissing.SetBindingValueByIndex(2, TriggeredInGameId);
		if (IfMissing.Step() != ESQLitePreparedStatementStepResult::Done)
		{
			UE_LOG(LogAILiveMemory, Error,
				TEXT("SyncRegistryFromLifecycle: insert step failed: %s"),
				*MetaDb.GetLastError());
			return Rollback();
		}
	}

	const TCHAR* UpdateSql = nullptr;
	if (LifecycleType == TEXT("delete_executed"))
	{
		UpdateSql = TEXT(
			"UPDATE agent_registry SET status='deleted', deleted_at=?1, "
			"  last_seen_game_id=COALESCE(NULLIF(?2, ''), last_seen_game_id), "
			"  last_updated_at=?1 "
			"WHERE agent_id=?3;");
	}
	else if (LifecycleType == TEXT("created") || LifecycleType == TEXT("revived"))
	{
		UpdateSql = TEXT(
			"UPDATE agent_registry SET status='active', deleted_at=NULL, "
			"  last_seen_game_id=COALESCE(NULLIF(?2, ''), last_seen_game_id), "
			"  last_updated_at=?1 "
			"WHERE agent_id=?3;");
	}
	else if (LifecycleType == TEXT("archived"))
	{
		UpdateSql = TEXT(
			"UPDATE agent_registry SET status='archived', "
			"  last_seen_game_id=COALESCE(NULLIF(?2, ''), last_seen_game_id), "
			"  last_updated_at=?1 "
			"WHERE agent_id=?3;");
	}
	else
	{
		// delete_proposed / delete_vetoed / unknown → only bump heartbeat
		UpdateSql = TEXT(
			"UPDATE agent_registry SET "
			"  last_seen_game_id=COALESCE(NULLIF(?2, ''), last_seen_game_id), "
			"  last_updated_at=?1 "
			"WHERE agent_id=?3;");
	}

	{
		FSQLitePreparedStatement Upd;
		if (!Upd.Create(MetaDb, UpdateSql))
		{
			UE_LOG(LogAILiveMemory, Error,
				TEXT("SyncRegistryFromLifecycle: prepare update failed: %s"),
				*MetaDb.GetLastError());
			return Rollback();
		}
		Upd.SetBindingValueByIndex(1, WallClock);
		Upd.SetBindingValueByIndex(2, TriggeredInGameId);
		Upd.SetBindingValueByIndex(3, AgentId);
		if (Upd.Step() != ESQLitePreparedStatementStepResult::Done)
		{
			UE_LOG(LogAILiveMemory, Error,
				TEXT("SyncRegistryFromLifecycle: update step failed: %s"),
				*MetaDb.GetLastError());
			return Rollback();
		}
	}

	if (!MetaDb.Execute(TEXT("COMMIT;")))
	{
		UE_LOG(LogAILiveMemory, Error,
			TEXT("SyncRegistryFromLifecycle: COMMIT failed: %s"), *MetaDb.GetLastError());
		return Rollback();
	}

	UE_LOG(LogAILiveMemory, Display,
		TEXT("SyncRegistryFromLifecycle OK agent=%s type=%s wall_clock=%s"),
		*AgentId, *LifecycleType, *WallClock);
	return true;
}

} // namespace AILiveAgentRegistry
