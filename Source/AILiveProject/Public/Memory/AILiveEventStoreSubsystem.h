#pragma once

#include "CoreMinimal.h"
#include "HAL/CriticalSection.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "SQLiteDatabase.h"
#include "Memory/AILiveEventTypes.h"
#include "Memory/AILiveBidTypes.h"
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

	// === T4 — 读 API（视角隔离 JOIN 强制）。
	// viewer 永远是具体 actor ID 字符串（如 "NPC03" / "orchestrator" / "system"）；
	// 传 "self" 字面量一律返回不可见——self 是 prompt 模板的相对语义，禁止入读路径。
	// 详见 memory_principles.md 硬约束 5、6。

	/** 单点取事件。返回 false 表示不可见或不存在（OutEvent 仅在 true 时填充）。 */
	UFUNCTION(BlueprintCallable, Category = "AILive|Memory")
	bool Quote(int64 InSeq, const FString& InViewer, FAILiveEvent& OutEvent) const;

	/** 取 round_no 内某 actor 的全部可见事件，seq 升序。 */
	UFUNCTION(BlueprintCallable, Category = "AILive|Memory")
	TArray<FAILiveEvent> QuoteByRound(int32 InRoundNo, const FString& InActor,
	                                   const FString& InViewer) const;

	/** 取 [max(0, currentRound-K+1), currentRound] 范围内全部可见事件，seq 升序。 */
	UFUNCTION(BlueprintCallable, Category = "AILive|Memory")
	TArray<FAILiveEvent> QuoteRecentRounds(int32 InCurrentRound, int32 InK,
	                                        const FString& InViewer) const;

	/**
	 * 取 agent 自己的全部公开发言 + 私聊（speech.public ∪ private_msg）——principles
	 * 硬约束 2 "自我发言全量追溯" 含全部发言通道。视角隔离仍走 JOIN，viewer 传自己。
	 * private_msg 能被命中要求写入侧把 actor 自身放进 visibility（T5/T7 写入纪律）。
	 */
	UFUNCTION(BlueprintCallable, Category = "AILive|Memory")
	TArray<FAILiveEvent> ListMyStatements(const FString& InAgentId) const;

	/** 取 agent 自己最近 N 条 speech.note，seq 降序。 */
	UFUNCTION(BlueprintCallable, Category = "AILive|Memory")
	TArray<FAILiveEvent> ListMyNotes(const FString& InAgentId, int32 InRecentN) const;

	/** 取 agent 自己最近 N 条 reflection.9q，round_no 降序后 seq 降序。 */
	UFUNCTION(BlueprintCallable, Category = "AILive|Memory")
	TArray<FAILiveEvent> ListMyReflections(const FString& InAgentId, int32 InRecentN) const;

	/**
	 * 取 agent 最近 N 条 "已写 intended 但未衍生 speech.public" 的 intended，seq 降序。
	 * 走 events 表 + parent 链直算，**不**依赖 agent_view_state 投影表（即使该表被 DROP 仍工作）。
	 */
	UFUNCTION(BlueprintCallable, Category = "AILive|Memory")
	TArray<FAILiveEvent> ListMyPendingIntended(const FString& InAgentId, int32 InRecentN) const;

	/**
	 * 投影表直读：commitments 表（T8 落地后才会有数据）。
	 * roundStart/roundEnd 传 -1 表示无界。
	 */
	UFUNCTION(BlueprintCallable, Category = "AILive|Memory")
	TArray<FAILiveCommitment> ListMyCommitments(const FString& InAgentId,
	                                              int32 InRoundStart = -1,
	                                              int32 InRoundEnd = -1) const;

	/**
	 * FTS5 trigram 模糊搜索 + 视角隔离。keyword 长度 < 3 或 trigram 不可用时
	 * 自动降级到 payload_text LIKE '%kw%'（仍套同一 visibility EXISTS）。
	 */
	UFUNCTION(BlueprintCallable, Category = "AILive|Memory")
	TArray<FAILiveEvent> SearchHistory(const FString& InKeyword, const FString& InActor,
	                                    int32 InRoundStart, int32 InRoundEnd,
	                                    const FString& InViewer, int32 InLimit) const;

	/**
	 * 取 round 内全部 event_type='vote' 的公开原文（直接查 events 表，**不**读
	 * vote_history 投影表——投影表只用作索引/计数；提取原文必须走 events，否则
	 * EventId/PayloadJson/EventHash 等关键字段缺失，调用方误以为拿到了可 quote 的事件）。
	 * 视角隔离 hard-code viewer='public'。
	 */
	UFUNCTION(BlueprintCallable, Category = "AILive|Memory")
	TArray<FAILiveEvent> ListVotes(int32 InRoundNo) const;

	/** 投影表直读：alliance_state 表全部行序列化为 JSON 数组（T8 落地后才会有数据）。 */
	UFUNCTION(BlueprintCallable, Category = "AILive|Memory")
	FString ListAllianceStateJson() const;

	/**
	 * 内部用（T7 派生 action.intent 时按 tick_no 切片）。无 viewer 过滤——调用方
	 * 是 orchestrator 自己。**不**加 BlueprintCallable，避免 BP 误用。
	 */
	TArray<FAILiveEvent> QuoteByEventTypeAndTick(EAILiveEventType InEventType, int64 InTickNo) const;

	// === T7 — Bid 协议 + Floor control ============================================
	// principles §5.2bis / §5.4。orchestrator 内部用，**不**加 BlueprintCallable。

	/**
	 * 取本拍全部 bid 事件并解析为 FAILiveBid 数组。
	 * IntendedSeq 由 (game_id, tick_no, actor) JOIN events.speech.intended 反解
	 * （协议不变量：每 actor 每拍最多 1 条 intended + 1 条 bid）。
	 * BidOffset 留 0；Director 在 ResolveFloor 调用前从 Roster 注入。
	 */
	TArray<FAILiveBid> ListBidsForTick(int64 InTickNo) const;

	/**
	 * 收齐 bid → 对每 eligible bid 计算 RuntimeAdj → FinalScore = Urgency + BidOffset + RuntimeAdj
	 * → 取最大；最大 < ColdThreshold → WinnerActor=""（冷场）；同分按 lexicographic actor_id 取小。
	 * AllBids 含全部 eligible（未参与 bid 的 abstain agent 不在此列表——caller 通过 EligibleAgentIds 过滤）。
	 * DerivedPublicSeq 留 0：由 caller 写完 speech.public 后回填到 tick_resolved.payload。
	 */
	FAILiveTickResolution ResolveFloor(int64 InTickNo,
	                                   const TArray<FString>& InEligibleAgentIds,
	                                   const TMap<FString, float>& InAgentBidOffsets,
	                                   float InColdThreshold = 3.0f) const;

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

	/**
	 * Soft check: every addressed_to target should be in the expanded visibility
	 * set. Per schema.yaml the violation must NOT reject the write (some
	 * legitimate "暗中点名" cases exist) — caller logs Warning + writes a
	 * system.parse_failed audit row, then proceeds with the original write.
	 *
	 * Limitation: visibility containing a Faction<X> tag is treated as opaque
	 * (no member lookup in T3); 'public' viewer is treated as a wildcard.
	 */
	static bool IsAddressedToSubsetOfVisibility(
		const TArray<FString>& InAddressedTo,
		const TArray<FString>& InVisibility,
		FString& OutError);

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

	/**
	 * T7 — RuntimeAdj 计算（任务卡常量版，DevLog 调参基线）：
	 *   反霸麦：扫 tick-1..tick-3 的 tick_resolved.winner_actor，连续 ≥ 3 拍命中本 actor → -1.5（一次性）
	 *   被 @ 加权：扫上一拍 tick_resolved → winner intended payload.addressed_to_hint 含本 actor → +2.0
	 *   沉默加权：本 actor 最近 5 拍无 actor=本 NPC 的 speech.public → +0.5
	 *   三项独立累加返回。
	 */
	float ComputeRuntimeAdjustment(const FString& InActor, int64 InCurrentTickNo) const;

	static FString GenerateUuidV7();

	FSQLiteDatabase Db;
	FSQLiteDatabase MetaDb;
	FString CurrentGameId;

	mutable FCriticalSection WriteMutex;
	int64 CachedLastSeq = 0;
	FString CachedLastHash;
	int64 CachedCurrentTickNo = 0;

	/** events_fts 表实际使用的 tokenizer（"trigram" / "unicode61"），EnsureSchema 时缓存。
	 *  SearchHistory 据此决定 LIKE 还是 MATCH 路径。 */
	FString DetectedFtsTokenizer;
};
