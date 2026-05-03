# T8 — Projector 重建（commitments / pending_intended / alliance / vote）

> 总览：[00_overview.md](00_overview.md)
> 依赖：blockedBy = [T7](T7_runtick_bid_parser.md)（pending_intended 投影需要 RunTick 产生 winner / loser 关系）
> 改动量：**中**（1–4h）
> §11 验收：L2「摘要展开 source_seq 定位」+ 新增「pending_intended 投影表与 T6 内联计算结果一致」

## 预检

纯 C++ + DB，**不需要** Monolith MCP；投影重建用例靠 console command + DB Browser 验证。

## 目标

把 events 表派生的所有投影表 / 投影段实现为纯函数 reducer，可被 `RebuildProjections()` 重放重建。重点是 `agent_view_state.pending_intended` —— T7 的 RunTick 异步触发本步实现。**注**：T6 用的最小化 pending 路径（直接查 events 表 + parent 链）仍可走，本步建立投影表是为了加速大数据量场景。

## 涉及文件

### 修

- `Source/AILiveProject/Public/Memory/AILiveEventStoreSubsystem.h` —— 暴露 `RebuildProjections()`（声明在 T2 已写）
- `Source/AILiveProject/Private/Memory/AILiveEventStoreSubsystem.cpp` —— 实现：
  - `RebuildProjections()` —— 顺序：清表 → 重放 events → 调各 projector
  - `ProjectCommitments()` —— 规则匹配 payload.text + speech_act_type 抽取（promise / claim_role / deny / vote_for / alliance）
  - `ProjectPendingIntended()` —— 找出所有 speech.intended 中 parent_event_id 未被任何 speech.public 引用的（按 actor + 最近 K 拍），写入 `agent_view_state.pending_intended`
  - `ProjectAllianceState()` —— 从 alliance_propose / alliance_accept / alliance_betray 派生
  - `ProjectVoteHistory()` —— 从 vote 事件派生
  - `ProjectAgentViewState()` —— 按 agent + as_of_seq 切片重建 alive_players / known_roles / my_commitments / vote_history JSON

## 依赖

blockedBy = [T7](T7_runtick_bid_parser.md)（pending_intended 投影需要 RunTick 产生 winner / loser 关系）

## 验收方式

对齐 §11 **L2 回归**：

- 跑完 ACT02 一局后调 `RebuildProjections()` → commitments / pending_intended / alliance_state / vote_history 表行数与预期一致
- L2「摘要展开」：`event_log_summaries.source_seq_start/end` 可定位回原文事件（注：摘要功能本身不在 MVP 范围；本项只验证 source_seq 字段完整性）
- 删除 commitments 表行 → `RebuildProjections()` 后行数恢复（投影可重建）
- **新增「pending_intended 投影表与 T6 内联计算结果一致」**：跑完 ACT02 → 同时调 `T4: ListMyPendingIntended("NPC04", 5)`（最小化路径，直查 events）和 `SELECT pending_intended FROM agent_view_state WHERE agent_id='NPC04' AND as_of_seq=<latest>`（投影表）→ 两者 JSON 解析后元素集合相等

## 风险点

- Projector 是纯函数禁止调 LLM（impl §7.4 + principles §四 反规约模式）—— 规则匹配 + 言语行为类型字段抽取，不上 LLM
- `pending_intended` 上限 10 拍 —— 早于 10 拍的 intended 仍在 events 表保留，但不再注入投影（impl §3.2 备注）
- `RebuildProjections()` 是 O(n) 重放——单局 ~600 事件下应在 < 100ms 内完成；性能不达标时调到批量 INSERT
- 异步触发：T7 的 `AsyncTask(BackgroundThread)` 调本方法，需要保证 EventStore 的读锁（如有）—— 当前 EventStore 没有读锁，依赖 SQLite WAL 的并发只读特性

## 完成定义

投影表行数与 events 重放一致 + RebuildProjections 可幂等多次调 + pending_intended 投影与 T4 内联结果一致。**不**意味着 Resume 已就绪（T9）。
