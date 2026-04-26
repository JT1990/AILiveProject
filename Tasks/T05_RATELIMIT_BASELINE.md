# T05 — DeepSeek Ratelimit Baseline

`UMindLLMProvider_DeepSeek::BatchPing(WorldCtx, Prompt, N=10, IntervalMs=500)` 实测结果。
数字作为 T21（Negotiate 阶段 GM 唤醒间隔）和 T26（M5 多轮验收）的实际可行性依据。

## 测试上下文

- 测试日期：2026-04-26
- 网络环境：N/A（个人项目，未记录）
- DeepSeek endpoint：`https://api.deepseek.com/v1`
- 测试 prompt：`用一句话介绍你自己`（21 chars）
- N（请求总数）：10
- IntervalMs（请求间隔）：500
- 测试 cadence：2 QPS（串行，间隔触发）

## 实测数据

- 账号 tier：N/A（个人项目，未记录）
- 实测 QPS 上限：≥ 2/秒（测试 cadence 上限，未触发 429；更高 QPS 未测）
- 平均延迟：887ms
- P95 延迟：954ms
- 最小延迟：689ms
- 最大延迟：1327ms（第 0 次冷启动）
- 触发 429：no
- 全部成功：10 / 10

### 单次明细（ms）

| # | 延迟 | HTTP |
| - | ---: | :--- |
| 0 | 1327 | 200 |
| 1 |  828 | 200 |
| 2 |  765 | 200 |
| 3 |  717 | 200 |
| 4 |  827 | 200 |
| 5 |  689 | 200 |
| 6 |  918 | 200 |
| 7 |  954 | 200 |
| 8 |  889 | 200 |
| 9 |  952 | 200 |

## 结论与影响

- 冷启动后稳定在 700–950ms，分布紧（avg 与 p95 仅差 67ms）。
- 2 QPS 完全无 ratelimit 压力；T21 多 NPC 并发场景下，按 8 NPC × cooldown 2s ≈ 4 QPS 仍远低于实测上限。
- M5 跨轮场景需要更激进 QPS 时再补一次 BatchPing（建议 IntervalMs=100 / N=20）。

## 备注

- BatchPing 串行触发（IntervalMs 间隔），不是并发猛冲；并发上限要在 T18.5 / T21 单独测。
- 若 P95 > 5000ms 或 429 比例 > 10%，需要在 T21 把 GM 唤醒间隔放大、或在 `Engine.ini` 调大 `[HTTP] HttpReceiveTimeout`。
- 第 0 次 1327ms 是 HTTPS TLS 握手 + 服务端首次冷加载；后续连接复用降到 ~800ms 是合理水位。
