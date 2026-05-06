#pragma once

// =============================================================================
// 中文教学：AILiveEventStoreSubsystem.h —— AILive 的「事件存储中枢」
//
// 这是整个系统最核心的类。一个 GameInstance 一份单例，挂着两个 SQLite 连接：
//   - Db     ：当前局的事件流 `Saved/Games/<GameId>.db`
//   - MetaDb ：跨局共享的 agent registry / lifecycle  `_meta.db`
//
// 架构总览（参考 PRD memory_principles.md）：
//
//   ┌──────────────────────────────────────────────────────────────────┐
//   │                   UAILiveEventStoreSubsystem                      │
//   │  ┌─────────────────┐    ┌──────────────────┐    ┌──────────────┐│
//   │  │  Append* API    │    │   读 API (T4)    │    │ Projector  ││
//   │  │  (写入 +        │    │   Quote*/List*   │    │ (T8 投影)  ││
//   │  │   hash 链)      │    │   带视角隔离      │    │            ││
//   │  └────────┬────────┘    └────────┬─────────┘    └──────┬─────┘│
//   │           │                       │                      │      │
//   │           ▼                       ▼                      ▼      │
//   │  ┌────────────────────────────────────────────────────────────┐│
//   │  │              SQLite (events / projections)                  ││
//   │  │  events / commitments / vote_history / alliance_state /     ││
//   │  │  agent_view_state / events_fts (FTS5)                       ││
//   │  └────────────────────────────────────────────────────────────┘│
//   └──────────────────────────────────────────────────────────────────┘
//
// 三个核心承诺（任一违反都是协议 bug）：
//
//   1) Append-only + Hash 链
//      events 表绝不允许 UPDATE / DELETE（SQL trigger 强制 ABORT），每条事件
//      hash = SHA-256(prev_hash || canonical_json(event))。VerifyHashChain()
//      可校验从头到尾未篡改。
//
//   2) 视角隔离
//      所有读 API 都接 InViewer 参数，SQL 层用 EXISTS(visibility[]) 强制过滤；
//      传 "self" 字面量一律返回不可见——self 只是 prompt 模板的相对语义。
//      调用方必须传具体 agent_id（"NPC03" 等）或特殊值 "public" / "system" / "orchestrator"。
//
//   3) Projector 是纯函数
//      RebuildProjections() 重放 events 重建所有派生表（commitments / vote_history /
//      alliance_state / agent_view_state）。任意时刻调用都得到相同结果（幂等）。
//      事故恢复就靠它 —— 投影表损坏可以从源 events 重建。
//
// 重要 UE / C++ 概念：
//
//   1) UGameInstanceSubsystem
//      UE 的一种 Subsystem 模式（编辑器 / GameInstance / World / LocalPlayer 各
//      一种）。生命周期：UGameInstance 创建/销毁时同步。GameInstance 在游戏
//      启动到关闭期间一直存在；多 PIE 多 GameInstance 互不干扰。
//      访问方式：`World->GetGameInstance()->GetSubsystem<UAILiveEventStoreSubsystem>()`
//      不需要在 BP 里手动 spawn。
//
//   2) virtual Initialize / Deinitialize
//      Subsystem 的两个生命周期钩子。Initialize 时连 SQLite，Deinitialize 时断开。
//      与 Actor 的 BeginPlay/EndPlay 不同——Subsystem 不依赖 World。
//
//   3) FCriticalSection WriteMutex (mutable)
//      多线程并发 Append 时保护 SQLite + hash 链 cache。`mutable` 让它能在
//      const 方法里被锁（读 API 也要在某些场景下短暂持锁，如重新加载 cache）。
//
//   4) BEGIN IMMEDIATE / COMMIT / ROLLBACK
//      每次 AppendEventsAtomically 包一个 SQL 事务，部分失败整体回滚。
//      `BEGIN IMMEDIATE` 比 `BEGIN` 更激进——立刻申请 RESERVED 锁，避免后续
//      升级到 EXCLUSIVE 时与读连接冲突。详见实现。
//
//   5) UFUNCTION(BlueprintCallable, Category = "AILive|Memory")
//      读 API 主要给 BP 调试 / Console 命令用。写 API 都是 C++ 直调，不需要
//      暴露给 BP（且写入要严格走预校验，BP 的随手调用会绕过 ValidateVisibility）。
//
// 阅读建议：
//   1) 先看 BeginGame / EndGame 理解 SQLite 文件挂接和卸载
//   2) 看 AppendEvent / AppendEventsAtomically + InsertEventBypassValidation_LockHeld
//      理解写入流水线：校验 → 锁 → BEGIN → 计算 hash → INSERT → COMMIT
//   3) 看 Quote / QuoteByRound / QuoteRecentRounds 理解视角隔离 SQL 模式
//   4) 看 VerifyHashChain / RecomputeHashChainOnMainConnection 理解一致性校验
//   5) 看 RebuildProjections + Project*_LockHeld 理解四张投影表怎么重建
// =============================================================================

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
	// UE Subsystem 生命周期钩子
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;  // GameInstance 启动时
	virtual void Deinitialize() override;                                     // GameInstance 关闭时

	// 局生命周期。InGameId 决定 Saved/Games/<InGameId>.db 文件名。
	// 中文教学：BeginGame 会创建/打开数据库 + ApplyPragmas + EnsureSchema +
	// LoadHashChainTail（恢复 cache 末尾 seq+hash）。EndGame 关闭连接。
	bool BeginGame(const FString& InGameId);
	void EndGame();

	bool IsGameOpen() const { return Db.IsValid(); }                   // inline 内联 getter
	const FString& GetCurrentGameId() const { return CurrentGameId; }  // 当前局 ID

	/**
	 * T9 — Resume protocol (impl §5.4 / principles §5.2bis.4 / §7.3).
	 *
	 * Open `Saved/Games/<InGameId>.db` (creates an empty one if missing — same
	 * as BeginGame). Walk events to pair every system.llm_inflight against any
	 * non-system event carrying the same payload.request_id; the unpaired set
	 * is then projected into one system.agent_timeout event each (visibility
	 * = ["system"], payload contains npc_index / request_id / age_seconds /
	 * resumed_at). Finally calls RebuildProjections() so derived tables match
	 * the new tail.
	 *
	 * MVP scope: stale (≥60s) and fresh (<60s) in-flights are treated the same
	 * — always abstain via system.agent_timeout. Re-issuing fresh requests via
	 * prompt cache is deferred (impl §5.4 line 1772).
	 *
	 * Returns true on success. On any step failure the function logs Error
	 * and returns false; callers should not assume a partial recovery is
	 * usable.
	 */
	bool ResumeFromGameId(const FString& InGameId);

	/**
	 * T9 — Delete cross-DB bridge (impl §3.2bis line 479-483).
	 *
	 * Three steps in order:
	 *   1) INSERT one row into `_meta.db.agent_lifecycle_events`
	 *      (lifecycle_event_type='delete_executed') and capture the new
	 *      event_id.
	 *   2) Call AILiveAgentRegistry::SyncRegistryFromLifecycle(MetaDb, eventId)
	 *      so `_meta.db.agent_registry.status='deleted', deleted_at=...`.
	 *   3) Append one `system.delete_executed` event to the live game .db
	 *      with visibility=["public"], payload includes lifecycle_event_id +
	 *      agent_id + reason_summary.
	 *
	 * The two writes span two SQLite connections so they cannot share a
	 * transaction. If the process crashes between (1)+(2) and (3), the
	 * lifecycle row remains and a follow-up Resume should reconcile by
	 * appending the missing system event — that compensation is *not* in
	 * the MVP and is documented in DevLog.
	 *
	 * @param OutLifecycleEventId Receives the `_meta.db` event_id (UUIDv7).
	 * @return seq of the appended `system.delete_executed` event, or -1 on
	 *         any failure.
	 */
	int64 TriggerDeleteExecuted(
		const FString& InAgentId,
		const FString& InReasonSummary,
		const FString& InReasonPayloadJson,
		const TArray<FString>& InTombstoneVisibility,
		bool bAffectsPersonaContinuity,
		FString& OutLifecycleEventId);

	/**
	 * Append one event. Delegates to AppendEventsAtomically (single-element group).
	 * On success, fills InOutEvent.{Seq,EventId,PrevEventHash,EventHash} and returns Seq.
	 * On rejection (visibility / payload validation failed) writes one
	 * system.parse_failed event to the truth log and returns -1.
	 */
	// 写入单事件。中文教学：调用方填好 InOutEvent 业务字段后传入；本函数会
	// 回填 Seq/EventId/PrevEventHash/EventHash。失败返回 -1（同时会写一条
	// system.parse_failed 留痕）。线程安全（内部加 WriteMutex）。
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
	// 批量原子写入。中文教学：一次写多条事件作为一个事务——要么全成功要么
	// 全失败回滚。这是写跨事件不变量（如 commit + corresponding intended）
	// 的唯一安全方式。返回值是组内第一条事件的 Seq。
	int64 AppendEventsAtomically(TArray<FAILiveEvent>& InOutEvents);

	/**
	 * Open a new tick. Writes one orchestrator.tick_anchor event and updates
	 * CachedCurrentTickNo so subsequent inserts auto-fill events.tick_no.
	 * On failure restores the previous tick_no cache. Returns the anchor seq.
	 */
	// 中文教学：开始一个新 tick（一拍）。每个 tick 是一组 NPC 同步思考的边界。
	// BeginTick 会写一条 orchestrator.tick_anchor 事件标记拍号，并更新本 Subsystem
	// 内部缓存 CachedCurrentTickNo。后续 Append* 会自动用这个 tick_no 填 events.tick_no 列。
	int64 BeginTick(int32 InTickNo);

	/**
	 * Read the current tick number used by AppendEvent / AppendEventsAtomically
	 * to fill events.tick_no. 0 before any BeginTick call.
	 */
	int64 GetCurrentTickNo() const { return CachedCurrentTickNo; }

	// 调试 / debug 用。直接按 actor 拉事件，无视角隔离。生产代码不要用。
	TArray<FAILiveEvent> QueryEventsByActor(const FString& Actor, int32 LimitCount) const;

	// 中文教学：从头到尾校验 hash 链未被篡改。OutFirstBadSeq 会写入第一个出错的
	// seq（用于定位损坏起点）。返回 true = 完整无损。这是事故诊断的兜底工具。
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

	// === T8 — Projector 重建 =========================================================
	// principles §7.4 + §7.2 末「projector 是纯函数，不调任何 LLM」。
	// 任一时刻调用都得到与 events 当前快照一致的派生表内容（幂等）。
	// 单事务包裹 DELETE → INSERT；事务在 WriteMutex 下持有，与 AppendEvent 互斥。
	// 失败 → 整体 ROLLBACK，旧投影保留。

	/**
	 * 重放 events，重建 commitments / vote_history / alliance_state /
	 * agent_view_state 四张投影表（pending_intended 写入 agent_view_state JSON 列）。
	 * 返回 true 表示事务提交；false 表示未开局或事务失败。
	 */
	UFUNCTION(BlueprintCallable, Category = "AILive|Memory")
	bool RebuildProjections();

	/**
	 * 读 agent_view_state.pending_intended JSON 列（最新 as_of_seq 行）。
	 * 用于 T8 验收 V4 与 T4 ListMyPendingIntended 做集合比对。
	 * 不存在或读失败返回 "[]"。
	 */
	UFUNCTION(BlueprintCallable, Category = "AILive|Memory")
	FString DebugReadPendingIntendedJson(const FString& InAgentId) const;

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
	// ── 私有辅助。生命周期 / schema 维护 ──────────────────────────────
	// 中文教学：private 区是「实现细节」，外部模块看不到。这里把 SQLite
	// pragma 配置、schema 创建、版本迁移、Meta DB 注册表初始化都封装起来。
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

	// === T8 — Projector reducers（私有；RebuildProjections 内部按顺序调用） ==========
	// 全部要求 caller 已持有 WriteMutex 且 BEGIN IMMEDIATE 事务在飞。
	// 失败返回 false → caller ROLLBACK；不直接抛错或修改全局状态。

	bool ProjectCommitments_LockHeld();
	bool ProjectVoteHistory_LockHeld();
	bool ProjectAllianceState_LockHeld();
	/**
	 * 单快照策略：每个 agent 写一行，as_of_seq = 当前 last_seq。
	 * 内部组装 alive_players / known_roles / my_commitments / vote_history /
	 * pending_intended 五个 JSON 字段。
	 */
	bool ProjectAgentViewState_LockHeld();

	static FString GenerateUuidV7();

	// ── 实例数据。所有运行时状态都在这里 ──────────────────────────────
	FSQLiteDatabase Db;                      // 当前局连接 Saved/Games/<GameId>.db
	FSQLiteDatabase MetaDb;                  // 跨局共享连接 _meta.db
	FString CurrentGameId;                   // 当前局 ID（与 Db 文件名同步）

	// 中文教学：mutable + const 方法
	//   const 方法理论上不能修改成员，但同步原语（mutex）需要在 const 方法里加锁。
	//   `mutable` 关键字给 WriteMutex 开个豁免：const 方法也能 lock 它。
	mutable FCriticalSection WriteMutex;
	int64 CachedLastSeq = 0;                 // hash 链尾 seq（避免每次 Append 都查盘）
	FString CachedLastHash;                  // hash 链尾值
	int64 CachedCurrentTickNo = 0;           // 当前 tick_no（BeginTick 写入）

	/** events_fts 表实际使用的 tokenizer（"trigram" / "unicode61"），EnsureSchema 时缓存。
	 *  SearchHistory 据此决定 LIKE 还是 MATCH 路径。 */
	FString DetectedFtsTokenizer;
};
