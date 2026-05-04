-- T7 验收 SQL — PIE 跑完一局 ACT02 后对最新 .db 套用
-- 用法：sqlite3 -readonly Saved/Games/<latest>.db < Tasks/T7_acceptance.sql

.headers on
.mode column
.width 60 12

-- ====== 概览 ======
.print "=== 概览：事件类型分布 ==="
SELECT event_type, COUNT(*) AS n
FROM events
GROUP BY event_type
ORDER BY 2 DESC;

.print ""
.print "=== 概览：tick_no 分布 ==="
SELECT tick_no, COUNT(*) AS n
FROM events
GROUP BY tick_no
ORDER BY tick_no;

.print ""
.print "=== L1 #1 一拍单一 winner（speech.public per tick，过滤 legacy_pre_bid） ==="
SELECT tick_no,
       COUNT(*) AS public_count,
       GROUP_CONCAT(actor) AS actors
FROM events
WHERE event_type='speech.public'
  AND json_extract(payload,'$.legacy_pre_bid') IS NULL
GROUP BY tick_no
ORDER BY tick_no;

.print ""
.print "=== L1 #1 配套：tick_resolved.derived_public_seq 与 speech.public.seq 匹配 ==="
SELECT
    json_extract(tr.payload,'$.tick_no')              AS tick_no,
    json_extract(tr.payload,'$.winner_actor')         AS winner,
    json_extract(tr.payload,'$.derived_public_seq')   AS dps,
    sp.seq                                            AS public_seq,
    CASE WHEN json_extract(tr.payload,'$.derived_public_seq') = sp.seq THEN 'OK' ELSE 'MISMATCH' END AS match
FROM events tr
LEFT JOIN events sp
    ON sp.tick_no = json_extract(tr.payload,'$.tick_no')
   AND sp.event_type = 'speech.public'
   AND json_extract(sp.payload,'$.legacy_pre_bid') IS NULL
WHERE tr.event_type = 'orchestrator.tick_resolved'
ORDER BY tick_no;

.print ""
.print "=== L1 #2 衍生关系：speech.public.parent_event_id → speech.intended ==="
SELECT
    e.tick_no,
    e.actor   AS pub_actor,
    i.actor   AS int_actor,
    e.round_no AS pub_round,
    i.round_no AS int_round,
    CASE WHEN e.actor=i.actor AND e.round_no=i.round_no AND i.event_type='speech.intended'
         THEN 'OK' ELSE 'MISMATCH' END AS match
FROM events e
LEFT JOIN events i ON e.parent_event_id = i.event_id
WHERE e.event_type='speech.public'
  AND json_extract(e.payload,'$.legacy_pre_bid') IS NULL
ORDER BY e.tick_no;

.print ""
.print "=== L1 #3 冷场推进：tick_resolved.winner_actor='' 且无 speech.public ==="
SELECT
    json_extract(payload,'$.tick_no')      AS tick_no,
    json_extract(payload,'$.winner_actor') AS winner,
    json_extract(payload,'$.derived_public_seq') AS dps,
    (SELECT COUNT(*) FROM events sp WHERE sp.tick_no=json_extract(events.payload,'$.tick_no')
       AND sp.event_type='speech.public'
       AND json_extract(sp.payload,'$.legacy_pre_bid') IS NULL) AS pub_count
FROM events
WHERE event_type='orchestrator.tick_resolved'
  AND (json_extract(payload,'$.winner_actor') IS NULL OR json_extract(payload,'$.winner_actor')='');

.print ""
.print "=== L1 #4 bid 隔离 — 所有 bid 事件 visibility 应只有 orchestrator ==="
SELECT v.viewer, COUNT(*) AS n
FROM event_visibility v
JOIN events e ON e.event_id=v.event_id
WHERE e.event_type='bid'
GROUP BY v.viewer;

.print ""
.print "=== L2 #5 反霸麦：tick_audit.all_bids[].runtime_adj 含 -1.5 项（连续 ≥3 拍 winner） ==="
SELECT
    json_extract(payload,'$.tick_no') AS tick_no,
    payload AS audit_payload
FROM events
WHERE event_type='orchestrator.tick_audit'
ORDER BY tick_no;

.print ""
.print "=== L2 #6 被 @ 加权：tick_audit.runtime_adj 含 +2.0 项（上一拍 winner addressed_to） ==="
.print "    （查看 audit_payload 列里的 runtime_adj=2.0 出现）"

.print ""
.print "=== L2 #8 bid 阶段超时：abstain agent 写 SystemParseFailed (parser_llm) ==="
SELECT tick_no, actor,
       json_extract(payload,'$.failure_source') AS failure_source,
       json_extract(payload,'$.parser_error_reason') AS reason,
       json_extract(payload,'$.failed_stage') AS stage
FROM events
WHERE event_type='system.parse_failed'
  AND json_extract(payload,'$.failure_source')='parser_llm'
ORDER BY tick_no, actor;

.print ""
.print "=== L2 #9 同拍 tick_no 切片：每拍 bid 数（理论 = 10 - abstain 数） ==="
SELECT tick_no, COUNT(*) AS bid_count
FROM events
WHERE event_type='bid'
GROUP BY tick_no
ORDER BY tick_no;

.print ""
.print "=== L2 #10 tick_resolved 可见 / tick_audit 不可见 — visibility 验证 ==="
SELECT e.event_type, v.viewer, COUNT(*) AS n
FROM event_visibility v
JOIN events e ON e.event_id=v.event_id
WHERE e.event_type IN ('orchestrator.tick_resolved','orchestrator.tick_audit')
GROUP BY e.event_type, v.viewer
ORDER BY e.event_type, v.viewer;

.print ""
.print "=== L2 #10 配套：tick_resolved payload 不含 all_bids ==="
SELECT tick_no,
       CASE WHEN json_extract(payload,'$.all_bids') IS NULL THEN 'OK_NO_ALLBIDS' ELSE 'LEAK!' END AS check_status
FROM events
WHERE event_type='orchestrator.tick_resolved'
ORDER BY tick_no;

.print ""
.print "=== L2 #11 action.intent 派生：actor / visibility / intent ==="
SELECT
    e.tick_no,
    e.actor,
    e.parent_event_id,
    json_extract(e.payload,'$.intent')                          AS intent,
    json_extract(e.payload,'$.derived_from_intended_seq')        AS from_intended,
    GROUP_CONCAT(v.viewer) AS visibility
FROM events e
LEFT JOIN event_visibility v ON v.event_id=e.event_id
WHERE e.event_type='action.intent'
GROUP BY e.event_id
ORDER BY e.tick_no, e.actor;

.print ""
.print "=== L2 #11 配套：action.intent visibility 中无 'self' 字面量 ==="
SELECT COUNT(*) AS self_leak
FROM event_visibility v
JOIN events e ON v.event_id=e.event_id
WHERE e.event_type='action.intent' AND v.viewer='self';

.print ""
.print "=== L2 #12 payload redaction fuzz — tick_audit visibility 全是 orchestrator ==="
SELECT v.viewer, COUNT(*) AS n
FROM event_visibility v
JOIN events e ON v.event_id=e.event_id
WHERE e.event_type='orchestrator.tick_audit'
GROUP BY v.viewer;

.print ""
.print "=== Parser #13 配套：abstain agent 不写 4 通道事件 ==="
SELECT
    pf.tick_no,
    pf.actor AS abstain_actor,
    (SELECT COUNT(*) FROM events x
       WHERE x.tick_no=pf.tick_no AND x.actor=pf.actor
         AND x.event_type IN ('speech.scratchpad','speech.intended','bid','speech.note')) AS four_channel_count
FROM events pf
WHERE pf.event_type='system.parse_failed'
  AND json_extract(pf.payload,'$.failure_source')='parser_llm';

.print ""
.print "=== Parser #14 100% 解析率烟测：parser_llm 失败次数（理想=0） ==="
SELECT COUNT(*) AS parser_llm_failures
FROM events
WHERE event_type='system.parse_failed'
  AND json_extract(payload,'$.failure_source')='parser_llm';

.print ""
.print "=== 决策 #13 验：parser_llm SystemParseFailed visibility = {orchestrator, NPCxx} ==="
SELECT
    e.tick_no, e.actor,
    GROUP_CONCAT(v.viewer ORDER BY v.viewer) AS visibility
FROM events e
LEFT JOIN event_visibility v ON v.event_id=e.event_id
WHERE e.event_type='system.parse_failed'
  AND json_extract(e.payload,'$.failure_source')='parser_llm'
GROUP BY e.event_id;

.print ""
.print "=== 哈希链验证：last seq + first bad seq ==="
SELECT MAX(seq) AS last_seq FROM events;

.print ""
.print "=== 4 通道完整性：每 (tick_no, actor) 应有 4 条 (scratchpad/intended/bid/note) 或 0 条 (abstain) ==="
SELECT
    tick_no, actor,
    SUM(CASE WHEN event_type='speech.scratchpad' THEN 1 ELSE 0 END) AS sp,
    SUM(CASE WHEN event_type='speech.intended'   THEN 1 ELSE 0 END) AS in_,
    SUM(CASE WHEN event_type='bid'               THEN 1 ELSE 0 END) AS bid,
    SUM(CASE WHEN event_type='speech.note'       THEN 1 ELSE 0 END) AS note
FROM events
WHERE actor LIKE 'NPC%' AND tick_no > 0
GROUP BY tick_no, actor
ORDER BY tick_no, actor;
