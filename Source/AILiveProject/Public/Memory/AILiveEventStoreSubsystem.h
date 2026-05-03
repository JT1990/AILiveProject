#pragma once

#include "CoreMinimal.h"
#include "HAL/CriticalSection.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "SQLiteDatabase.h"
#include "Memory/AILiveEventTypes.h"
#include "AILiveEventStoreSubsystem.generated.h"

UCLASS()
class AILIVEPROJECT_API UAILiveEventStoreSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	bool BeginGame(const FString& InGameId);
	void EndGame();

	bool IsGameOpen() const { return Db.IsValid(); }
	const FString& GetCurrentGameId() const { return CurrentGameId; }

	/**
	 * Append one event. Delegates to AppendEventsAtomically (single-element group).
	 * On success, fills InOutEvent.{Seq,EventId,PrevEventHash,EventHash} and returns Seq.
	 * On rejection (visibility / payload validation failed) writes one
	 * system.parse_failed event to the truth log and returns -1.
	 */
	int64 AppendEvent(FAILiveEvent& InOutEvent);

	/**
	 * Append a group of events as a single SQLite transaction.
	 * Either every event lands or the whole group rolls back. Returns the seq
	 * of the first event in the group on success, -1 on any failure.
	 *
	 * Static validation runs *before* the WriteMutex is taken; rejected events
	 * trigger a system.parse_failed write through the regular bypass path
	 * (which takes the lock itself), so there is no nested-lock / nested-BEGIN
	 * risk. The actual writes happen under WriteMutex with BEGIN IMMEDIATE.
	 */
	int64 AppendEventsAtomically(TArray<FAILiveEvent>& InOutEvents);

	/**
	 * Open a new tick. Writes one orchestrator.tick_anchor event and updates
	 * CachedCurrentTickNo so subsequent inserts auto-fill events.tick_no.
	 * On failure restores the previous tick_no cache. Returns the anchor seq.
	 */
	int64 BeginTick(int32 InTickNo);

	/**
	 * Read the current tick number used by AppendEvent / AppendEventsAtomically
	 * to fill events.tick_no. 0 before any BeginTick call.
	 */
	int64 GetCurrentTickNo() const { return CachedCurrentTickNo; }

	TArray<FAILiveEvent> QueryEventsByActor(const FString& Actor, int32 LimitCount) const;
	bool VerifyHashChain(int64& OutFirstBadSeq) const;

	// 通过主连接读 _meta.db.schema_meta 全表。WAL 同进程二次连接会拿不到 -shm
	// 共享映射（实测 ReadOnly / ReadWrite 都回 SQLITE_IOERR），故不开新连接，
	// 走已 open 的 MetaDb。仅在 IsGameOpen() 时可调。
	bool QueryMetaSchemaRegistry(TMap<FString, FString>& OutKVs);

	/**
	 * Write a key/value pair into _meta.db.schema_meta (INSERT OR REPLACE).
	 * Used by debug console commands; the parser_version dynamic-read flow
	 * relies on this to test that runtime mutations propagate to new events.
	 */
	bool UpsertMetaSchemaKV(const FString& Key, const FString& Value);

	/** Set PRAGMA query_only on the live game-db connection (debug only). */
	bool SetGameDbQueryOnly(bool bQueryOnly);

	/**
	 * Execute raw SQL on the main game-db connection (debug only).
	 * Used by acceptance tests to verify events_no_update / events_no_delete
	 * triggers ABORT — secondary connections within the same process can't
	 * acquire the WAL writer lock, so the trigger has to fire on the main
	 * connection. Returns false + writes the error to OutError on failure.
	 */
	bool ExecuteDebugSqlOnMainConnection(const FString& Sql, FString& OutError);

	/**
	 * Walk all events on the main connection, recompute event_hash from
	 * (prev_event_hash || canonical_json(event)), and compare against the
	 * stored hash. Returns false + populates OutFirstBadSeq when any row
	 * fails. Used by AILive.Test.RecomputeAndVerifyChain (in-process variant
	 * that avoids the WAL secondary-reader IOERR).
	 */
	bool RecomputeHashChainOnMainConnection(
		int64& OutLastSeq,
		int64& OutBadCount,
		int64& OutFirstBadSeq);

	// === Static validation (used by AppendEventsAtomically; exposed for tests).
	static bool ValidateVisibility(const TArray<FString>& InVisibility, FString& OutError);
	static bool ValidatePayloadJson(const FString& InPayloadJson, FString& OutError);

	// === Canonical JSON / hash (exposed for debug console commands).
	static FString CanonicalJsonOf(const FAILiveEvent& InEvent);
	FString ComputeEventHash(const FString& PrevHash, const FString& CanonicalPayload) const;

	static constexpr int32 kCurrentSchemaVersion = 1;
	static const TCHAR* const kGenesisHash;

private:
	void ApplyPragmas(FSQLiteDatabase& InDb);
	bool EnsureSchema(FSQLiteDatabase& InDb, bool bIsMetaDb);
	bool RunMigrations(FSQLiteDatabase& InDb, int32 FromVersion, int32 ToVersion, bool bIsMetaDb, const TCHAR* FtsTokenizer);
	bool EnsureMetaRegistry(FSQLiteDatabase& InMetaDb);

	FString DetectFtsTokenizer(FSQLiteDatabase& InDb);

	bool LoadHashChainTail();

	/**
	 * Internal "lock-held" insert. Caller must already hold WriteMutex AND have
	 * BEGIN IMMEDIATE in flight on Db. Updates the by-ref local seq/hash
	 * trackers; **never** mutates the global Cached* members so a subsequent
	 * ROLLBACK leaves global state untouched.
	 */
	int64 InsertEventBypassValidation_LockHeld(
		FAILiveEvent& InOutEvent,
		int64& InOutLocalLastSeq,
		FString& InOutLocalLastHash);

	/** Public-style bypass: takes the lock + BEGIN/COMMIT itself. */
	int64 InsertEventBypassValidation(FAILiveEvent& InOutEvent);

	/** Truth-log fallback when AppendEventsAtomically rejects an event. */
	int64 AppendSystemParseFailure(
		const FString& InOriginalActor,
		const FString& InOriginalEventTypeStr,
		const FString& InErrorReason,
		const FString& InOriginalPayloadSnippet);

	/** Resolve parser_version to write: dynamic schema_meta read with constant fallback. */
	FString ResolveLiveParserVersion();

	static FString GenerateUuidV7();

	FSQLiteDatabase Db;
	FSQLiteDatabase MetaDb;
	FString CurrentGameId;

	mutable FCriticalSection WriteMutex;
	int64 CachedLastSeq = 0;
	FString CachedLastHash;
	int64 CachedCurrentTickNo = 0;
};
