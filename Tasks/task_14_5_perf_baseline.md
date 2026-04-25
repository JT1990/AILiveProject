# T14.5 — M2 性能基线测量

## 目标
M2 通过后跑一次量化测试，作为 M5 验收前的对比基线。**没有数字基线无法判断 M5 8 NPC 是否扛得住**。

## 前置
T14（M2 端到端通过）

## DoD
- [ ] 在 `Level_LiarsBar` 用真 DeepSeek（不是 mock）跑 1 局完整游戏
- [ ] 收集以下数字（编辑器 stat 命令 + Output Log 抓取）：
  - 单局总时长（秒）
  - 总回合数
  - 总 LLM 调用次数（按 `LogMind: DeepSeek request` 计数）
  - LLM 调用平均延迟（按 `request → response` 时间差）
  - LLM 调用 P95 延迟
  - 单 NPC 平均决策完成时间（trigger → action 完成）
  - 全局平均 fps（`stat fps` 录屏 / `stat dumpave`）
  - GameThread 单帧最大耗时（`stat unit` 看 spike）
  - 总 Memory.Write 调用次数
  - 总 Memory.Recall 调用次数
  - HTTP error / retry 次数（如果 DeepSeek 限流）
- [ ] 写到 `DevLog/2026-04-XX_m2_perf_baseline.md`，按表格列出
- [ ] 推算 M5（8 NPC × 多轮）的预估调用数，与 DeepSeek 账号 ratelimit 对比
- [ ] 如果推算超 ratelimit，**修改 T21 的 GM 唤醒间隔参数**（默认 10s 改更长）

## 验收信号
- DevLog 文件就位
- 推算结果写明：M5 预估单局 LLM 调用数 / 单 NPC 平均决策延迟 / 是否会触限流
- 给出 T21 的 NegotiateTickTimer 推荐值（基于实测）

## 不在范围
- 优化任何代码（先测，不改）
- 跨厂商性能对比（T18.5 后才有意义）

## 风险
- M2 单局可能 5-15 分钟，测一次就要这么久
- DeepSeek 实时延迟波动大，跑一局的 P95 可能不准；如果不放心跑 3 局取均值
