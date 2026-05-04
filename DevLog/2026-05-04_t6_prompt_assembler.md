# 2026-05-04 — T6 AILivePromptAssembler 落地

T6 任务卡完成。把 `Docs/memory_principles.md` §4.3「prompt 拼装优先级」从文档落到代码——新增 `AILivePromptAssembler` namespace 接管 ACT02 reaction phase 的 system + user prompt 拼装，按 7 段必保留顺序（自我发言 / pending intended / notes / commitments / 近场窗口 / 私聊 / challenge prefetch）拼装事件流原文。读侧顺带把 `tick_no` 列纳入 `FAILiveEvent`，不破坏 canonical JSON 哈希链。

## 决策（5 条 Q&A）

施工前与用户确认 4 条范围/方案歧义；施工中协作者审查又触发 1 条 §4.3 必保留段缺失冲突——共 5 条 Q&A 全部对齐后才动代码。

1. **`tick_no` 列归宿**：T4 的 `kEventSelectColumns` 故意不含 `tick_no`（canonical JSON 协议保护）。T6 任务卡 line 43 明说"PromptAssembler 必须查这一列"。决策：**扩 `FAILiveEvent` 加 `int64 TickNo`，`kEventSelectColumns` 改 18 列；CanonicalJsonOf 实现保持不变（tick_no 仍排除）；现有 `AILive.Test.CanonicalEcho` 守护此不变量**。
2. **commitments 段依赖**：`AppendCommitmentsSection` 列在任务卡 helper，但 commitments 投影表是 T8 ProjectCommitments 才落地。决策：**T6 直接 call 已存在的 `ListMyCommitments(viewer, -1, -1)`，stub 行为天然——T8 前返回空数组，T8 落地后自动有内容**。
3. **近场窗口 K 常量**：principles §4.3 line 223 要求"K 必须固定"但没给数值。决策：**K=5**（覆盖 1-2 个 day_discuss 反应链；与 bidding §5.2bis 推荐 N=3 语义不同——后者是抢麦判霸麦阈值）。
4. **Phase 接管范围**：决策：**只接管 Reaction**；Seed 保持原样（开局首拍无历史，9 段中 7 段会空）。
5. **§4.3 必保留 5 (notes) + 必保留 8 (私聊段) 任务卡漏列**：协作者审查指出 §4.3 这两段都要求进 prompt，但任务卡 line 21 helper 清单只列了 5 个。决策：**全部纳入**——加 `AppendNotesSection` + `AppendPrivateChatsSection`，超出任务卡 helper 字面列表 2 项但与 principles §4.3 完全对齐。

## 协作者审查吸收（两轮共 9 条全部接受）

**第一轮 5 条**：
- **High** `QuoteByRound(round, "", viewer)` 命中 0 条（line 842 SQL 强制 `e.actor = ?3`） → 改用 `QuoteRecentRounds(round, 1, Viewer)`（K=1 即单轮窗口）
- **High** system prompt 不能引入"四通道认知"措辞（ACT02 line 632 仍 `bResponseFormatJson=true`，line 729 ParseAnswer 读 legacy `willingness`/`content`）→ 保留 legacy JSON 输出协议；四通道是 T7 范围
- **Medium** notes/private 段 §4.3 vs 任务卡冲突 → AskUserQuestion 后纳入（决策 #5）
- **Medium** `AssembleSystemPrompt(Cfg, Opt)` 偏离任务卡 line 25 `(Store, Cfg, Opt)` → 签名加 Store；`Opt.Viewer` 替代 `AgentId` 命名
- **Low** `LogAILive` 不存在 → 改 `LogAILiveMemory`（声明在 `AILiveEventTypes.h:7`）

**第二轮 4 条**：
- **High** `RowToEvent` 示例 `Stmt.GetColumnValueByIndex(3).GetIntValue()` 编译失败（UE 5.7 是 `bool(Index, OutValue)` 形式）→ 按现有 ref-out 风格写 `int64 TickNoRead = 0; InStmt.GetColumnValueByIndex(3, TickNoRead); Ev.TickNo = TickNoRead;`，原索引 4..16 顺移到 5..17
- **Medium** `InsertEventBypassValidation_LockHeld` line 1562 直接 bind `CachedCurrentTickNo`，但调用方 `InOutEvent.TickNo` 仍是 0 → bind 前补 `InOutEvent.TickNo = CachedCurrentTickNo;`，CanonicalJsonOf 仍排除 tick_no 字段，**哈希链对老库兼容**
- **Low** `check()` 与 `ensureMsgf()` 不等价 → 用 `ensureAlwaysMsgf`（PIE 不 abort，留 callstack + 继续 prompt 组装）；任务卡 line 41「让上层处理」语义对齐
- **Low** "7 段标题 reaction prompt 中可见" 与 `Opt.ChallengeText=""` 矛盾 → 完成定义改为「ACT02 普通 reaction 验前 6 段；`AILive.Test.AssembleChallenge` console 单独验第 7 段」

## 落地的 7 个 prompt 段

| 优先级 | 段标题 | helper | 数据源 |
| --- | --- | --- | --- |
| 必保留 3 | `[YOUR OWN COMPLETE STATEMENT HISTORY]` | `AppendOwnHistorySection` | `ListMyStatements(Viewer)` |
| 必保留 4 | `[YOUR RECENT INTENDED-BUT-NOT-SAID]` | `AppendPendingIntendedSection` | `ListMyPendingIntended(Viewer, K=5)` |
| 必保留 5 | `[YOUR NOTES TO FUTURE SELF]` | `AppendNotesSection` | `ListMyNotes(Viewer, K=5)` |
| 必保留 6 | `[YOUR COMMITMENTS]` | `AppendCommitmentsSection` | `ListMyCommitments(Viewer, -1, -1)`（T8 前空） |
| 必保留 7 | `[RECENT NEAR-WINDOW PUBLIC EVENTS]` | `AppendNearWindowSection` | `QuoteRecentRounds` 过滤 visibility 含 'public' 且 actor != Viewer |
| 必保留 8 | `[PRIVATE / NON-PUBLIC EVENTS ADDRESSED TO YOU]` | `AppendPrivateChatsSection` | `QuoteRecentRounds` 过滤 visibility 含 Viewer 但不含 'public' 且 actor != Viewer |
| §4.4 prefetch | `[PREFETCHED EVIDENCE FROM REFERENCED ROUND {N}]` | `AppendChallengePrefetchSection` | `ExtractRoundRefs("第\\s*(\\d+)\\s*轮")` → `QuoteRecentRounds(round, 1, Viewer)` |

每行格式（principles §7 模板）：`Tick {tick_no:03d} round_no={round_no:02d} {phase} seq={seq}: "{text}"`，`{text}` 由内部 `ExtractText(PayloadJson)` 用 `FJsonObject::TryGetStringField("text")` 提取。

## 降级硬规则

`kPromptCtxLimit = 64000` 字符（中文 ≈21K tokens；DeepSeek/GLM/Qwen3 均 ≥32K context）。超限按 §4.3 line 223-228 顺序丢段：
1. 先丢 PrivateChats
2. 再丢 Notes
3. 再丢 Commitments
4. **永远不丢** OwnHistory / PendingIntended / NearWindow（公开发言）

降级后仍超限：`UE_LOG(LogAILiveMemory, Error)` + `ensureAlwaysMsgf(...)` 留 callstack 但继续返回 prompt（不 abort PIE，让上层判断长度后实施摘要或拒绝发起 LLM 请求）。

## 文件改动

### 新增

- `Source/AILiveProject/Public/Memory/AILivePromptAssembler.h` —— `namespace AILivePromptAssembler` + `FAssembleOptions{ Viewer, CurrentRound, NearWindowK, GameRule, ChallengeText }` + `AssembleSystemPrompt(Store, Cfg, Opt)` / `AssembleUserPrompt(Store, Cfg, Opt)`
- `Source/AILiveProject/Private/Memory/AILivePromptAssembler.cpp` —— 7 个 `Append*Section` helper + `ExtractText` + `ExtractRoundRefs` + `FormatEventLine` + 降级 + ensureAlwaysMsgf 长度断言 + 2 个 `AILive.Test.Assemble*` console commands

### 修

- `Source/AILiveProject/Public/Memory/AILiveEventTypes.h` —— `FAILiveEvent` struct 加 `int64 TickNo = 0`（BlueprintReadOnly + 注释说明 EventStore 写入时回填，canonical JSON 不参与）
- `Source/AILiveProject/Private/Memory/AILiveEventStoreSubsystem.cpp` —— `kEventSelectColumns` 加 `e.tick_no` 至第 4 列（17→18）；`RowToEvent` 加 `TickNoRead` 顺移；3 处注释更新（line 243-244 / 745 / 1389）；`InsertEventBypassValidation_LockHeld` 在 hash 计算前补 `InOutEvent.TickNo = CachedCurrentTickNo;` 给所有写入路径回填 TickNo
- `Source/AILiveProject/Public/Acts/Act02RuleReceiveDirector.h` —— 删除 `BuildReactionSystemPrompt` / `BuildReactionUserPrompt` 声明（被 PromptAssembler 替代后 orphan）
- `Source/AILiveProject/Private/Acts/Act02RuleReceiveDirector.cpp` —— include `Memory/AILivePromptAssembler.h`；`DispatchLLMs` reaction 分支改调 `AILivePromptAssembler::AssembleSystemPrompt/AssembleUserPrompt`；删除两个 helper 函数体（共 25 行）。**保留** `BuildSeedSystemPrompt/UserPrompt`、`LastSpeakerIndex`、`SpeakerName`、`N.UnspokenContent`（其中 SpeakerName 在 TTS dispatch line 976 仍用；其余字段 T7 主循环重写时再清理）

## 验收实证

UBT 全量重建：12.17s，零错误零警告（`Result: Succeeded`），4 个改动 .cpp 全部编译过：AILiveEventTypes.cpp / AILivePromptAssembler.cpp / AILiveEventStoreSubsystem.cpp / Act02RuleReceiveDirector.cpp。

PIE 跑 ACT01 → ACT02 后 console 验收（DB: `Saved/Games/<ts>.db`，包含 ACT01 setup 阶段 24 条事件）：

| 验收项 | 命令 | 实际结果 |
| --- | --- | --- |
| **§11 L1 pending_intended 注入** | `AILive.Test.AssemblePendingIntended NPC04` | seq=25 len=1569 `header_present=1 text_present=1` ✓ — 输出含 `[YOUR RECENT INTENDED-BUT-NOT-SAID]` 段 + 该条 intended 原文「我想质疑 NPC03 关于联盟的说法」 |
| **§4.4 challenge prefetch** | `AILive.Test.AssembleChallenge NPC04 3` | seq=26 len=1816 `header_present=1 text_present=1` ✓ — 输出含 `[PREFETCHED EVIDENCE FROM REFERENCED ROUND 3]` 段 + 第 3 轮 NPC03 公开发言原文 |
| **canonical JSON 不退化（tick_no 仍排除）** | `AILive.Test.CanonicalEcho` | `byte_for_byte_match=1, tick_no_invariance_match=1, no_prev_event_hash=1, no_event_hash=1, no_wall_clock=1, float_round_trip=1, empty_string_kept=1, nested_keys_sorted=1` ✓ |
| **哈希链对老库兼容** | `AILive.Test.RecomputeAndVerifyChain` | `chain ok, last_seq=24` ✓ — TickNo 字段加入后哈希链零损坏 |
| **7 段全部按模板渲染** | console 输出 prompt 字符串 | 7 段标题全部出现：OwnHistory / PendingIntended / Notes / Commitments / NearWindow / PrivateChats / ChallengePrefetch；每行 `Tick %03d round_no=%02d %s seq=%lld: "%s"` 格式正确 |
| **NearWindow 视角隔离** | console 输出 | NPC04 viewer 视角看到其他 NPC 的 setup 阶段 11 条公开发言；自己的 seq=17 在 OwnHistory 段而非 NearWindow（`actor != Viewer` 过滤生效） |

## 后续工作（不在 T6 范围）

- **Reflection 9 问段（可裁剪 1）** —— 与 T7 四通道认知输出 schema 一并落地
- **ProjectCommitments 规则匹配抽取** —— T8 范围；T6 directly call `ListMyCommitments`，T8 落地前空数组
- **ChallengeText 实际来源** —— T7 主循环或后续 hostile prompt 入口；T6 仅暴露 `Opt.ChallengeText` 入口，Act02 集成时传空串
- **NearWindow oldest tick 截断** —— 当前降级把 NearWindow 整段保留；超限场景下应支持按 tick 截断（principles §4.3 line 226 第 3 步），T6 未实现
- **四通道认知 schema 切换** —— T7 主循环重写时把 `OUTPUT SCHEMA` 段从 legacy `{ want_to_speak, willingness, content }` 切到四通道；T6 system prompt 暂保留 ACT02 现有协议
- **OwnHistory 空 text 行抑制** —— PIE 验收发现 NPC04 一条 setup phase speech.public payload.text 为空，输出 `seq=17: ""` 不优雅；polish 项留待后续
