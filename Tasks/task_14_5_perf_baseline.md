# T14.5 — M2 性能基线测量

## 目标
M2 通过后跑一次量化测试，作为 M5 验收前的对比基线。**没有数字基线无法判断 M5 8 NPC 是否扛得住**。

## 前置
T14（M2 端到端通过）

## DoD
- [ ] 在 `Level_LiarsBar` 用真 DeepSeek（不是 mock）跑 1 局完整游戏
- [ ] 收集以下数字（编辑器 stat 命令 + Output Log 抓取 + Memory Service 端点查询）：

### 时长 / 速度
- 单局总时长（秒）
- 总回合数
- LLM 调用平均延迟（按 `request → response` 时间差）
- LLM 调用 P95 延迟
- 单 NPC 平均决策完成时间（trigger → action 完成）

### LLM 调用统计
- 总 LLM 调用次数（按 `LogMind: DeepSeek request` 计数）
- recall_retry_count（recall_long_term_memory tool_call 触发次数；M2 期望 0-2）
- HTTP 429 / 5xx 次数
- JSON parse fallback 次数
- Validate reject 次数

### Context / Memory
- `context_tokens_max`（单 NPC 最大 buffer token）
- `compact_count`（Compact 触发次数；128k 模型期望 ≥ 1，200k 模型可能 0）
- `summary_count`（scene_end Summarizer 完成次数；M2 单局期望 = 4 NPC）
- `cache_hit_tokens` 占 input 总 token 比例（DeepSeek `usage.prompt_cache_hit_tokens / prompt_tokens`）
- `fallback_raw_count`（Summarizer 失败 raw_log fallback 次数）
- `actual_to_estimated_token_ratio_p95`（每次 LLM 返回时实际 token / 估算 token，P95）

### 性能 / 资源
- 全局平均 fps（`stat fps` 录屏 / `stat dumpave`）
- GameThread 单帧最大耗时（`stat unit` 看 spike）
- `inflight_http_p95`（in-flight HTTP request 数 P95；> 12 触发 warning）
- `scene_summary_p95_seconds`（scene_end Summarizer P95 完成时间）

- [ ] 写到 `DevLog/2026-04-XX_m2_perf_baseline.md`，按表格列出
- [ ] 推算 M5（8 NPC × 多轮）的预估调用数，与 DeepSeek 账号 ratelimit 对比
- [ ] 如果推算超 ratelimit，**修改 T21 的 GM 唤醒间隔参数**（默认 10s 改更长）+ **修改 LLM Budget 两池容量**（reasoner=4 / summarizer=2 是否需要调）

## 验收信号
- DevLog 文件就位
- 推算结果写明：M5 预估单局 LLM 调用数 / 单 NPC 平均决策延迟 / 是否会触限流 / cache 命中率是否达标 / Compact / Summarizer 排队是否健康
- 给出 T21 的 NegotiateTickTimer 推荐值（基于实测）
- 给出 SafetyMargin 调整建议（如果 actual_to_estimated > 1.3 则升到 70%）

## 不在范围
- 优化任何代码（先测，不改）
- 跨厂商性能对比（T18.5 后才有意义）

## 风险
- M2 单局可能 5-15 分钟，测一次就要这么久
- DeepSeek 实时延迟波动大，跑一局的 P95 可能不准；如果不放心跑 3 局取均值
- token 估算偏差监控如果在 M2 已超 1.3 就要在 M3 之前修估算逻辑（接 cl100k tokenizer），避免 M5 再翻车
