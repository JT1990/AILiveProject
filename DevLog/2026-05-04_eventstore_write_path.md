# T3 — EventStore 写入路径：AppendEvent + 哈希链 + parse_failed 兜底 + tick_no

日期：2026-05-04
依赖：T0（SQLiteCore + OpenSSL）+ T1（Memory/ 类型骨架）+ T2（Schema migration + EventStore 子系统骨架）+ T2.5（Parser LLM client + parser_version registry）。

## 目标

把 schema.yaml + memory_principles.md §二底层不变量 + memory_implementation_ue57.md §5.2 的全部硬约束一次性落代码：viewer 封闭集合校验 / payload `$.text` 校验 / canonical JSON / SHA-256 哈希链 / 串行互斥 / 事务原子 / parse_failed 真相源兜底 / tick_no 自动填充。完成后 100 条事件 + 10 线程并发可入库、违规走 system.parse_failed、哈希链可外部独立重算验证。

T2 留下的 `AppendEvent` stub 直接 return false，本任务把它实化 + 新增 `AppendEventsAtomically` / `BeginTick` / `GetCurrentTickNo` 三条公开 API + 一组私有实现方法。

## 关键决策

| 决策点 | 取值 | 理由 |
| --- | --- | --- |
| `AppendEvent` 签名 | `int64 AppendEvent(FAILiveEvent&)` | T2 的 `bool AppendEvent(const FAILiveEvent&)` 破签：不返 seq 上层无法引用、不能回填 hash。T2 stub 零调用所以零成本破签 |
| 校验位置 | **WriteMutex 之前** | 协作者审查指出：持锁内调 parse_failed → 调公开 `InsertEventBypassValidation` 会嵌套取锁 + BEGIN IMMEDIATE in BEGIN IMMEDIATE。改成校验完全在锁外，任一失败立即调公开 bypass 写 parse_failed（自取锁自事务），然后整组拒绝 |
| `_LockHeld` 缓存策略 | by-ref local seq/hash，**不**写全局 Cached* | 协作者审查指出：多事件 group 中第 N 条失败 ROLLBACK 时，前 N-1 条若已更新全局缓存就被污染。改为调用方维护 local 副本，只在 COMMIT 成功后才把 local 复制到全局 |
| BeginTick 失败回滚 | 保存 `OldTick`、ROLLBACK 路径 restore | 否则 BeginTick(N) 失败后下次 AppendEvent 会错误归到未提交的 N |
| canonical JSON 字段范围 | 14 个语义字段；显式排除 `tick_no` / `prev_event_hash` / `event_hash` / `wall_clock` / `payload_text` | 协作者审查指出：`prev_event_hash` 已通过 `Combined = prev || canonical` 入哈希，再放 canonical 是重复绑定；`wall_clock` 由 DB DEFAULT 生成、in-memory 与行值不一致；`payload_text` 是 GENERATED column |
| canonical JSON float 序列化 | 自实现 `CanonicalDoubleToString`：`%.17g` + integer-valued 时强制 `.0` 后缀 | UE `TJsonWriter::WriteValue(double 1.0)` 输出 `"1"` 而非 `"1.0"`，违反 principles §二#8 IEEE round-trip 要求。改用 `WriteRawJSONValue` 旁路写自定义 token |
| `parser_version` 写入 | 每次 INSERT 前调 `QueryMetaSchemaRegistry` 取 `key='parser_version'`，缺失 fallback `AILiveParser::GetCurrentParserVersion()` | 用户裁决 B 方案：动态读 registry。任务卡 L2 第 11 条「手工改 schema_meta='2' → 新事件即 '2'」只有动态读取能机械验证。T2.5 已落地 QueryMetaSchemaRegistry，零跨步成本 |
| GenerateUuidV7 | 自实现 RFC 9562（48-bit Unix epoch ms + version=7 + random + variant=10 + random） | 不降级 v4。约 30 行不引入新依赖 |
| `Sha256Fingerprint` 实现 | 走 `<openssl/sha.h>` 的 `SHA256()` C 函数，不走 `<openssl/evp.h>` | 后者 include `ossl_typ.h` 含 `typedef struct ui_st UI;` 与 UObject 的 `namespace UI` 冲突；`sha.h` 仅 include `e_os2.h` + `stddef.h`，无冲突 |
| Util 下沉位置 | `Public/Util/AILiveSha256.h` + `Public/Util/AILiveJsonEscape.h` | 复用给 Director in-flight prompt 指纹 + parse_failed payload 转义。CI grep `Sha1` / `FSHA1` 工程内零结果 |

## 改动清单

### 新增

| 文件 | 内容摘要 |
| --- | --- |
| `Source/AILiveProject/Public/Util/AILiveSha256.h` + `.cpp` | `AILiveUtil::Sha256Fingerprint(const FString&) → 64 hex lowercase` |
| `Source/AILiveProject/Public/Util/AILiveJsonEscape.h` + `.cpp` | `AILiveUtil::EscapeJsonString(const FString&) → JSON quoted literal` |

### 修

| 文件 | 改动 |
| --- | --- |
| `Source/AILiveProject/Public/Memory/AILiveEventStoreSubsystem.h` | 改 `AppendEvent` 返 `int64`；新增 `AppendEventsAtomically` / `BeginTick(int32)` / `GetCurrentTickNo` / `ValidateVisibility` / `ValidatePayloadJson` / `CanonicalJsonOf` / `ComputeEventHash` / `UpsertMetaSchemaKV` / `SetGameDbQueryOnly` / `ExecuteDebugSqlOnMainConnection` / `RecomputeHashChainOnMainConnection`；私有 `InsertEventBypassValidation` / `_LockHeld(三参数)` / `AppendSystemParseFailure` / `ResolveLiveParserVersion` / `GenerateUuidV7` / `LoadHashChainTail`；新增 `WriteMutex` / `CachedLastSeq` / `CachedLastHash` / `CachedCurrentTickNo` 成员 |
| `Source/AILiveProject/Private/Memory/AILiveEventStoreSubsystem.cpp` | 同 .h 全部实现；BeginGame 末尾载 `LoadHashChainTail`；EndGame 末尾 reset cache；anonymous namespace 内 `IsValidViewerString` / `WriteCanonicalValue` / `WriteCanonicalObject` / `CanonicalDoubleToString`；新增 13 条 console 测试命令（`AppendOne` / `Append100` / `AppendBadVisSelf` / `AppendBadVisFreeText` / `AppendBadPayloadNoText` / `BeginTick` / `ConcurrentAppend` / `RecomputeAndVerifyChain` / `TryUpdate` / `TryDelete` / `SetMetaSchemaKV` / `Sha256` / `SetQueryOnly` / `CanonicalEcho`） |

## 路径调试 / 协作者审查反馈链

协作者三轮审查触发的关键修正（已全部纳入实现）：

1. **Round 1 高优先级**
   - **canonical JSON 字段范围**：原方案纳入 `prev_event_hash` / `wall_clock`；修正为只覆盖语义字段
   - **BeginTick 签名**：`bool BeginTick(int64, int32, EAILivePhase)` 改为 `int64 BeginTick(int32)` 返 anchor seq；round/phase 在 T3 范围用默认值
   - **fatal 用例机械化**：原方案降级为 code review；改回机械验证用 PRAGMA query_only 注入失败点

2. **Round 2 高优先级**
   - **parser_version B 方案**：用户选 A 后我发现 `QueryMetaSchemaRegistry` 已存在且动态读零成本，重新询问后用户改选 B；写入路径每次 INSERT 前实时查 registry
   - **`_LockHeld` 缓存原子性**：原方案让 `_LockHeld` 直接写 `CachedLastSeq` / `CachedLastHash` → 多事件 group 第 N 条失败时前 N-1 条已污染全局；改为 by-ref local + COMMIT 后才晋升

3. **Round 3 关键修正**
   - **校验在锁外**：原方案让校验在持锁内执行，但 parse_failed 路径调公开 `InsertEventBypassValidation` 自己取锁 + BEGIN IMMEDIATE → 嵌套事务报错。改为静态校验完全在 WriteMutex 之前，rejection 整组直接 return -1
   - **fatal 失败注入**：原方案用 `attrib +R`，但 BeginGame 阶段就因只读失败到不了 fatal 路径。改用 `PRAGMA query_only=1` 在主连接上注入：BeginGame 已成功 → SetQueryOnly → AppendBadVisSelf → 校验 fail → AppendSystemParseFailure → InsertEventBypassValidation → BEGIN IMMEDIATE 失败 → Fatal

实施时遇到的 4 个非平凡坑：

1. **OpenSSL `<openssl/evp.h>` UI 命名空间冲突**：UE5.7 的 `ossl_typ.h` 第 144 行 `typedef struct ui_st UI;` 与 `UObject/ObjectMacros.h` 第 923 行 `namespace UI` 冲突，`THIRD_PARTY_INCLUDES_START` 宏不解决命名空间冲突。改用 `<openssl/sha.h>`（仅依赖 `e_os2.h` + `stddef.h`）+ `SHA256()` C 函数。
2. **Live Coding 不可靠 patch console 命令**：`AILive.Test.Sha256` 经 Live Coding 加新 FAutoConsoleCommand 后，运行时输入 Sha256 触发了 TryDelete 的 lambda body。Live Coding 对静态初始化 + lambda 捕获不可靠 patch；UBT 全量重建后命令路由恢复正常。教训：新增 `static FAutoConsoleCommand` 必须 UBT，不能 Live Coding。
3. **WAL 同进程二次连接 IOERR**：`RecomputeAndVerifyChain` 初版用 secondary readonly 连接读 events，跑 `Failed to open database: disk I/O error`。这与 T2.5 `QueryMetaSchemaRegistry` 同根因。改为 `RecomputeHashChainOnMainConnection` 走主连接 SELECT。
4. **append-only trigger 与 GENERATED column 的执行顺序**：初版 `TryUpdate` 用 `payload='x'` 触发，报错 `malformed JSON` 而不是 `events table is append-only`——SQLite 对 `payload_text GENERATED ALWAYS AS json_extract(payload,'$.text')` 的求值发生在 BEFORE UPDATE trigger 之前。改用 `round_no=99` 避开 GENERATED 列依赖，触发器正确 fire `events table is append-only; UPDATE forbidden`。

## 验收实证

按"验收方式"21 项机械验证清单跑完，全部通过：

### L0 — 100 事件 + tick_no（PIE log 实证）

```
Cmd: AILive.Test.BeginGame test_t3
LogAILiveMemory: BeginGame('test_t3') OK ... (last_seq=0, last_hash_prefix=00000000)
Cmd: AILive.Test.Append100
LogAILiveMemory: Append100 done: failures=0, total_visibility_entries=133, elapsed_ms=149.8
Cmd: AILive.Test.RecomputeAndVerifyChain
LogAILiveMemory: chain ok, last_seq=100
```

SQL 复核：
```
sqlite> SELECT COUNT(*), COUNT(DISTINCT seq), MIN(seq)||'..'||MAX(seq) FROM events;
148|148|1..148
sqlite> SELECT COUNT(*) FROM events WHERE tick_no=0;
100
sqlite> SELECT COUNT(*) FROM events WHERE tick_no=42;
48     -- 1 anchor + 3 AppendOne + 3 parse_failed + 40 ConcurrentAppend + 1 AppendOne after bump
sqlite> SELECT COUNT(*) FROM events WHERE length(event_hash)=64;
148    -- 全部 SHA-256 64 hex
```

### L0 — BeginTick 后写 M 条

```
Cmd: AILive.Test.BeginTick 42
LogAILiveMemory: BeginTick(42) -> anchor_seq=101, GetCurrentTickNo=42
Cmd: AILive.Test.AppendOne hello42-a
LogAILiveMemory: AppendOne -> seq=102 ... parser_version=1
Cmd: AILive.Test.AppendOne hello42-b
LogAILiveMemory: AppendOne -> seq=103
Cmd: AILive.Test.AppendOne hello42-c
LogAILiveMemory: AppendOne -> seq=104
```

### L2 — visibility / payload 拒绝（3 条全 → seq=-1 + parse_failed）

```
LogAILiveMemory: rejected event[0] (actor=NPC03): visibility contains 'self' — must be expanded ...
LogAILiveMemory: rejected event[0] (actor=NPC03): invalid viewer string: 'random_string' ...
LogAILiveMemory: rejected event[0] (actor=orchestrator): payload must contain 'text' field ...
sqlite> SELECT COUNT(*) FROM events WHERE event_type='system.parse_failed';
3
```

### L2 — canonical JSON 不变性

```
LogAILiveMemory: CanonicalEcho A={"actor":"NPC07",...
                                 ...,"payload":{"empty":"","nested":{"a":1.0,"b":2.0},"text":"x","willingness_score":1.0},...
LogAILiveMemory: byte_for_byte_match=1, tick_no_invariance_match=1,
                 no_prev_event_hash=1, no_event_hash=1, no_wall_clock=1,
                 float_round_trip=1, empty_string_kept=1, nested_keys_sorted=1
```

### L2 — Sha256 一致性

```
LogAILiveMemory: Sha256("hello") = 2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824 (len=64)
```

与 OpenSSL CLI `echo -n hello | openssl dgst -sha256` 输出一致。CI grep：

```
$ grep -ri 'Sha1\|FSHA1' Source/  →  零结果
```

### L2 — append-only DB 强制（trigger 触发）

```
Cmd: AILive.Test.TryUpdate 1
LogAILiveMemory: TryUpdate seq=1 -> rejected (err='events table is append-only; UPDATE forbidden'; ...)
Cmd: AILive.Test.TryDelete 1
LogAILiveMemory: TryDelete seq=1 -> rejected (err='events table is append-only; DELETE forbidden'; ...)
```

### L2 — 并发写入互斥

```
Cmd: AILive.Test.ConcurrentAppend 10 4
LogAILiveMemory: ConcurrentAppend(10 x 4) done: success=10, failed=0, elapsed_ms=40.7
Cmd: AILive.Test.RecomputeAndVerifyChain
LogAILiveMemory: chain ok, last_seq=147
```

10 线程 × 4 事件 = 40 条新行，seq 严格递增、hash chain 完整、UE_LOG 无 SQLite misuse。

### L2 — parser_version 注册表动态读取（B 方案）

```
Cmd: AILive.Test.SetMetaSchemaKV parser_version 2
LogAILiveMemory: SetMetaSchemaKV('parser_version'='2') -> OK
Cmd: AILive.Test.AppendOne hello-after-version-bump
LogAILiveMemory: AppendOne -> seq=148 ... parser_version=2     ← 动态读取生效
Cmd: AILive.Test.SetMetaSchemaKV parser_version 1
LogAILiveMemory: SetMetaSchemaKV('parser_version'='1') -> OK
```

SQL 复核：
```
sqlite> SELECT parser_version, COUNT(*) FROM events GROUP BY parser_version;
1|147
2|1
```

### L2 — parse_failed 不递归失败（fatal 注入）

```
Cmd: AILive.Test.SetQueryOnly
LogAILiveMemory: Db query_only=1 — subsequent INSERTs will fail with SQLITE_READONLY
Cmd: AILive.Test.AppendBadVisSelf
LogAILiveMemory: rejected event[0] (actor=NPC03): visibility contains 'self' ...
LogWindows: Error: Fatal error: [...]:894
            InsertEventBypassValidation BEGIN IMMEDIATE failed: attempt to write
            a readonly database — EventStore is past its responsibility boundary.
[Callstack] UAILiveEventStoreSubsystem::InsertEventBypassValidation()
```

callstack 单帧、`AppendSystemParseFailure` 计数 = 1（不递归）、PIE 崩溃后无再次进入。

### 外部 Python 哈希链独立复算

`tools/verify_chain.py`（一次性脚本）独立用 `hashlib.sha256` + 复刻的 canonical 算法（含 `.0` 浮点规则、ASCII 升序、空串/缺字段区分）跑完 148 行：

```
$ python /tmp/verify_chain.py
rows=148 bad=0 first_bad=None
```

跨实现一致 → canonical 算法是可移植 spec、不是 UE 内部黑盒。

## 风险点 / 已知遗留

1. **Live Coding 与新增 FAutoConsoleCommand**：本次实施被 Live Coding patch 不可靠耽误两轮（Sha256 路由到 TryDelete handler、check 字符串字面量未刷新）。后续涉及新增 console 命令的任务**必须** UBT 全量重建，不能用 Live Coding 验证 console 路由。
2. **Editor crash 后 PIE 状态丢失**：fatal 用例触发 `UE_LOG(Fatal)` 让 PIE 进程整体退出，下一次跑测试需要重启编辑器。这是设计预期（principle §二#8 + §7.2 要求 EventStore 在跨过责任边界时硬退出）。
3. **`RecomputeHashChainOnMainConnection` 仅供调试**：T9 的真正 `VerifyHashChain` 公开 API 仍是 stub；本步只为 T3 自测用。T9 落地时应当：(a) 不使用主写连接（避免占 BEGIN IMMEDIATE 锁）、(b) 支持 partial chain verification、(c) 输出结构化结果而不是 UE_LOG。
4. **UUIDv7 时间戳分辨率**：当前 ms 级精度，同 ms 内多事件 random 部分区分。T9 复核时若发现密集写入下 collision 风险可升级到 µs 级时间戳。
5. **canonical JSON 整数 → 浮点强制 `.0`**：payload 中的 `{"index":42}` 被 canonicalize 为 `{"index":42.0}`，因为 FJsonValueNumber 始终是 double。这与「integer 字段在 USTRUCT 上是 int64 → 不加 .0」不一致——struct level 整数字段（seq / round_no）保持整数格式。该不一致是 JSON 类型系统局限的合理后果，文档化即可。

## 完成定义

- 21 项机械验收全部通过（含 L2 #12 fatal 注入、L2 #11 parser_version 动态读取） ✓
- 外部 Python 哈希链独立复算 148/148 通过 ✓
- 工程内 `Sha1` / `FSHA1` grep 零引用 ✓
- DevLog 入库 ✓

**不**意味着读 API（T4）/ Director 改造（T5）/ bid 协议（T7）—— 那是后续任务。

---

## 提交后修订（吸收协作者第 4 轮审查）

3 项修正全部纳入：

| 严重度 | 修正 | 位置 |
| --- | --- | --- |
| **High** | `AppendSystemParseFailure` 改用 `TJsonWriter` 构造整个 payload。原实现把 `InOriginalActor` / `InOriginalEventTypeStr` 直接拼进 `"text"` 段没转义——若 Actor 含 `"` / 换行 / `\` 会生成非法 JSON → `events.payload_text` GENERATED column 解析失败 → INSERT 失败 → `UE_LOG(Fatal)` 崩溃。改用 writer 后所有字段（含 text 摘要）都自动转义 | `EventStoreSubsystem.cpp` |
| **Medium** | 新增 `IsAddressedToSubsetOfVisibility` 静态校验 + `AppendEventsAtomically` 的软警告分支。`addressed_to` 不在 `visibility` 集合时记 `UE_LOG(Warning)` + 写一条 `system.parse_failed` 审计行，但**不**拒绝原事件写入（schema.yaml line 308 + impl §5.2 line 2338 的"暗中点名"用例需要保留）。`'public'` 视为通配；Faction 在 T3 范围视为不透明（不展开成员） | `EventStoreSubsystem.h/.cpp` |
| **Medium** | 删 `AILiveSha256.h:10` 注释里的 `FSHA1` 字样——它让任务卡的 "CI grep `Sha1`/`FSHA1` 零结果" 验收形式上失败。改写为"SHA-1 helpers are forbidden project-wide"。重新跑 grep 确认零匹配 | `AILiveSha256.h` |

**侧记 UUIDv7 同 ms 递增**（拒绝）：principles 的"事件标识符严格递增"约束指 schema.yaml line 111 的 `seq: integer # 单局内全局递增`，不是 line 113 的 `event_id: string # 唯一事件 UUID`。我们用 seq 严格单调（`CachedLastSeq + 1`），event_id 只要求"唯一字符串"——UUIDv7 ms-precision + 80-bit random 满足唯一性。

新增 console 命令 `AILive.Test.AppendAddressedToOutsideVis` 验收 `addressed_to` 软警告路径（visibility=`["NPC03"]` + addressed_to=`["NPC07"]` → 期望 seq>0 + 多一条 parse_failed 审计行）。

修订后 UBT 全量重建通过、`grep -ri 'Sha1\|FSHA1' Source/` 零结果。
