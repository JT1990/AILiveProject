# T3 — 写入路径：AppendEvent + 哈希链 + parse_failed 兜底 + tick_no

> 总览：[00_overview.md](00_overview.md)
> 依赖：blockedBy = [T2.5](T2_5_parser_llm_client.md)
> 改动量：**大**（> 4h）
> §11 验收：L0「100 事件 + tick_no」、L2 全部写入相关项

## 预检

纯 C++ + DB 操作，**不需要** Monolith MCP；100 事件用例靠 console command + DB Browser。

## 目标

实现完整写入路径，把 schema.yaml 全部硬约束（viewer 封闭集合 / payload 含 text / canonical JSON / SHA-256 / append-only DB trigger / 串行互斥 / parse_failed 兜底）一次性落到代码。**额外**：`events.parser_version` 字段不再硬编码 `"1"`，改调 `AILiveParser::GetCurrentParserVersion()`（T2.5 落地）。

## 涉及文件

### 修

- `Source/AILiveProject/Public/Memory/AILiveEventStoreSubsystem.h` —— 暴露 `AppendEvent` / `AppendEventsAtomically` / `BeginTick` / `GetCurrentTickNo`（声明在 T2 已写）
- `Source/AILiveProject/Private/Memory/AILiveEventStoreSubsystem.cpp` —— 实现：
  - `ValidateVisibility`（封闭集合：public/audience/orchestrator/system/NPC<NN>/Faction<X>，**显式拒绝 "self"**）
  - `ValidatePayloadJson`（必须可解析 + 含 `$.text`）
  - `AppendEventsAtomically`（FScopeLock(WriteMutex) + BEGIN IMMEDIATE + 静态校验 + INSERT events + 反规范化 event_visibility / event_addressed_to + COMMIT/ROLLBACK + 失败时调 `AppendSystemParseFailure`）
  - `AppendEvent`（委托给 atomic 版）
  - `InsertEventBypassValidation`（仅供内部兜底，CI grep 业务层零调用）
  - `AppendSystemParseFailure`（payload 由本方法构造 → 永远合法 → 无递归失败可能；snippet 截 256 字符）
  - `BeginTick`（写一条 `OrchestratorTickAnchor` 事件 + 缓存 `CachedCurrentTickNo`；之后所有 INSERT 自动填 `events.tick_no`）
  - `CanonicalJsonOf`（字段 ASCII 升序 + 浮点 IEEE 754 round-trip + 数组保序 + 空串 vs 缺字段可区分 + **显式跳过 `tick_no` 字段**）
  - `ComputeEventHash`（OpenSSL EVP_sha256，**禁用 FSHA1**）
  - `GenerateUuidV7`
  - `Sha256Fingerprint` —— 下沉到 `Source/AILiveProject/Public/Util/`，供 EventStore + Director in-flight prompt 指纹复用
  - `EscapeJsonString` —— 同上

## 依赖

blockedBy = [T2.5](T2_5_parser_llm_client.md)

## 验收方式

对齐 §11 **L0 单元** 第 3-4 项 + 全部 **L2 回归** 写入相关项：

- L0：100 条合成事件 → events 行数 100；event_visibility 行数 = sum(visibility 数组长度)；seq 严格递增；hash chain 可验证；未 BeginTick 的事件 tick_no=0
- L0：BeginTick(N) 后写 M 条事件 → `SELECT * FROM events WHERE tick_no=N` 精确返回 M 行
- L2「visibility self 拒绝」：传 `Visibility = {"self", "NPC03"}` → 返回 -1 + UE_LOG 含 "must be expanded" + events 表新增 1 条 `system.parse_failed`
- L2「visibility 自由文本拒绝」：传 `{"random_string"}` → 同上
- L2「payload text 缺失拒绝」：传 `{"foo":"bar"}` → 同上
- L2「parse_failed 不递归失败」：构造 .db 只读场景 → 触发 `UE_LOG(Fatal)` 而非无限递归
- L2「canonical JSON 不变性」：同一 `FAILiveEvent` 序列化两次 byte-for-byte 完全相同；浮点 `1.0` 稳定
- L2「tick_no 不参与 canonical_json」：同一 event 一次 tick_no=0 一次 tick_no=42，CanonicalJsonOf 输出一致；event_hash 与 tick_no 无关
- L2「SHA-256 哈希链」：events.event_hash 长度 = 64
- L2「Sha256Fingerprint 一致性」：`Sha256Fingerprint("hello")` 与 `openssl dgst -sha256` CLI 输出一致；CI grep `Sha1` / `FSHA1` 工程内零结果
- L2「append-only DB-level 强制」：`UPDATE events SET payload='x' WHERE seq=1` 报错 `events table is append-only`；DELETE 同样
- L2「并发写入互斥」：10 线程同时各调 `AppendEventsAtomically(4 events)` → events 表 40 行；seq 1..40 严格递增；hash chain 连续；零 SQLite misuse
- **新增「parser_version 注册表读取」**：写入一条 event → `SELECT parser_version FROM events WHERE seq=?` 返回 `'1'`（与 schema_meta.parser_version 一致）；后续若手工把 schema_meta.parser_version 改为 `'2'`，新写入事件的 parser_version 也是 `'2'`

## 风险点

- `CanonicalJsonOf` 实现要写 unit test 反复跑——浮点 round-trip / 字段 ASCII 排序两个细节最容易踩坑；CLAUDE.md「Goal-Driven Execution」要求用强验收标准（同一事件序列化 byte-for-byte 相同）
- `bOk = bOk && X` 短路求值方向（impl §5.2 反复强调）：写成 `(X && bOk)` 时 X 仍执行，bOk 起不到防御作用——code review 必查
- OpenSSL `EVP_sha256` 路径在 UE 5.7 是否需要额外的 include path 调整（UE 内置 OpenSSL 但 header 路径偶有版本差异）——T0 已经在最小测试中验证过 include 可用
- `_meta.db` 视为另一个 connection，不能跟单局 db 共用 `WriteMutex`——本步只关心单局；跨库写在 T9 Delete 桥接处理

## 里程碑 DevLog

`DevLog/2026-MM-DD_eventstore_write_path.md`，记录哈希链 / canonical JSON 实现细节 / 并发互斥决策 / 100 事件 + 10 线程并发用例结果

## 完成定义

100 条事件可写、可哈希链验证、违规可被 parse_failed 兜住、并发安全、parser_version 从 registry 读取 + 里程碑 DevLog 入库。**不**意味着能读。
