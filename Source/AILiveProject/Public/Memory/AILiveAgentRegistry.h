#pragma once

#include "CoreMinimal.h"

class FSQLiteDatabase;

namespace AILiveAgentRegistry
{
	/**
	 * Read the indicated row from `_meta.db.agent_lifecycle_events` and project
	 * its lifecycle_event_type onto `_meta.db.agent_registry`:
	 *   delete_executed → status='deleted', deleted_at=<wall_clock>
	 *   created / revived → status='active', deleted_at=NULL
	 *   archived → status='archived'
	 *   delete_proposed / delete_vetoed → status untouched (only last_updated_at bumped)
	 *
	 * Caller must already have MetaDb open (BeginGame opens it). This function
	 * manages its own BEGIN IMMEDIATE / COMMIT / ROLLBACK on MetaDb.
	 *
	 * Business code MUST NOT issue `UPDATE agent_registry` directly — this is
	 * the only authorized writer (CI grep guard documented in DevLog).
	 */
	AILIVEPROJECT_API bool SyncRegistryFromLifecycle(FSQLiteDatabase& MetaDb,
		const FString& LifecycleEventId);
}
