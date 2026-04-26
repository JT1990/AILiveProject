# T17 — 5 局压测 + 策略观察 + DevLog

## 目标
连续跑 5 局骗子酒馆并记录关键观察指标，形成 DevLog 文档，作为 M3 总验收。

## 前置
T15（动画桥）+ T16（跨局 peer_summary 注入 + 多 persona）

## DoD
- [ ] 跑 5 局完整骗子酒馆：每局直到剩 1 NPC alive
- [ ] 录屏（OBS 或编辑器自带）保存到 `DevLog/2026-04-XX_liarsbar_5sessions/`（或合并 1 个长视频）
- [ ] 收集每局指标（手工记录到 DevLog 即可）：
  - 总回合数
  - 获胜者
  - 每 NPC 谎称比例（claim != actual 的次数 / 总出牌次数）
  - 每 NPC 挑战次数 + 命中率
  - 出现的"跨局指控"次数（Speak 内容引用上局事件，验证 peer_summary 注入生效）
- [ ] 收集长会话稳定性指标（5 局累计）：
  - Compact 总触发次数（128k 模型 5 局期望 ≥ 5）
  - peer_summary 节点总数（应等于 4 NPC × 3 peer × 5 局 = 60）
  - 新场启动时 peer_summary 命中率（应 ≥ 90%；不达标说明 Summarizer 还没完成下场就启动了）
  - cache_hit_tokens 占比（5 局后期是否稳定 ≥ 70%）
- [ ] 写 `DevLog/2026-04-XX_liarsbar_polish.md`（按现有 DevLog 风格）：
  - 实施摘要
  - 关键调试坑（如：DeepSeek 偶尔不输出 JSON / Cooldown 与 ChallengeWindow 冲突 / Compact 摘要质量 / peer_summary 跨场命中失败）
  - 5 局指标表
  - 涌现现象观察（联盟、个人风格、欺骗模式、跨局指控具体例子）
  - 已知缺陷 + 下阶段需要修复

## 验收信号
- DevLog 文件就位
- 录屏可回放 5 局完整对局
- DevLog 中至少 3 条"涌现现象"观察
- DevLog 中至少 2 条"跨局指控"具体例子（"第 3 局第 5 回合 NPC_3 引用了第 1 局 NPC_2 的谎言"）+ 对应 peer_summary 文本对照
- DevLog 末尾列出明确的"M4 阻塞项"（少数决开发前必须解决的事）

## 不在范围
- 自动化指标采集脚本（手工记录足够）
- 数据可视化 / 图表
- 跨厂商对比（MVP 只 DeepSeek；T18.5 后才有意义）

## 风险
- 单局耗时长（5-10 分钟），5 局 = 30-50 分钟纯运行 + 观察记录时间
- DeepSeek 账号在密集调用下可能限流——summarizer pool 与 reasoner pool 分开兜底
- 涌现观察主观性高，DevLog 描述要具体到"第 3 局第 5 回合 NPC_3 引用了第 1 局 NPC_2 的谎言"
- peer_summary 命中失败（< 90%）时建议调长 scene 间隔或调小 MaxConcurrentSummarizer 池容量（让单局 summarizer 更快完成）
