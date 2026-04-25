# T26 — M5 验证 + 背叛涌现观察 + DevLog + CLAUDE.md 更新

## 目标
跑多局完整少数决（多轮淘汰到分胜负），观察"自发联盟形成 / 背叛事件 / 跨轮指控"等涌现现象，写 DevLog 总结，更新 CLAUDE.md 把 Mind + GameMaster 子系统的关键约定补充进去。**M5 总验收，整个 plan 收尾**。

## 前置
T25（联盟 HUD）+ T24（多轮循环）

## DoD
- [ ] 跑 3 局完整少数决，每局至少 3 轮投票
- [ ] 录屏保存
- [ ] 收集每局指标：
  - 总轮数
  - 最终赢家
  - 自发结盟次数（ProposeAlliance + AcceptAlliance 都成功）
  - 背叛事件次数（一个 agent 在 intent 里说投 yes 但实际投 no，且事后被另一方记忆引用）
  - 跨轮指控次数（NPC 在 Negotiate 中引用上轮事件）
- [ ] 写 `DevLog/2026-04-XX_minorityrule_full.md`（M5 完结报告）：
  - 实施摘要
  - 3 局完整指标表
  - 涌现现象详细案例（具体到 round / agent / 引用文本）
  - 跨厂商行为差异观察（DeepSeek vs GLM，依赖 T18.5 已完成）
  - 后续扩展建议（更多厂商 / 其他游戏卡）
- [ ] 同步更新 `CLAUDE.md` **和 `AGENTS.md`**（仓库内同时存在两份协作约定文档）：
  - 在"架构"章节加新段落 "Mind + GameMaster 子系统"，描述：
    - Mind 决策层与 GameMaster 规则层的职责边界
    - per-agent view 与"客观真实"的分离
    - Action 派发协议（通用 + 游戏专属）
    - 决策触发的混合模式（GM 唤醒 + Perception 节流）
    - 关键文件入口（`Source/AILiveProject/Public/Mind/`）
    - 失败模式：LLM JSON 失败容错 / Action 验证失败的回退 / 私聊 channel 的语音 attenuation 局限
- [ ] 更新 `Tasks/README.md`：标记 M5 完成，可选增加"扩展项"段（如下一步可加哪些游戏卡 / 哪些厂商）

## 验收信号

**强制项**：
- ✅ 3 局完整跑到分胜负
- ✅ DevLog 文件就位 + 录屏
- ✅ CLAUDE.md 加了 Mind + GameMaster 子系统说明

**涌现观察（至少满足 2 项）**：
- ✅ 至少 1 局出现稳定联盟（≥3 NPC 互相 AcceptAlliance 形成偶数派系）
- ✅ 至少 1 次明显背叛（agent 公开承诺 yes 但实际 no，事后该背叛被另一方在后续轮次中引用，被引用记忆有 round-N 标记）
- ✅ 至少 1 次跨轮指控（如"上轮 NPC_3 也是这么说然后骗了我们"）
- ✅ 看完整 3 局有清晰的"局内冲突戏剧化"感（不是各 NPC 自说自话）

**协议入口指标（必收集）**：
- 跨 3 局 LLM 总数 / JSON parse fallback / Validate reject / Wait 续命 / HTTP 429（DeepSeek 与 GLM 分别统计）

**性能**：
- 整局过程游戏线程稳定 30+ fps（少数决场景较空，应该 60+）

## 多游戏会话验证

完成上面 3 局少数决后，做一次"骗子酒馆 → 少数决"跨关卡验证：

- [ ] 同一 PIE 内：先加载 `Level_LiarsBar` 跑 1 局完整骗子酒馆 → 中途不退出 PIE → `OpenLevel("Level_MinorityRule")` 加载少数决关卡 → 跑 1 轮少数决
- [ ] **关键观察**：少数决里某 NPC 的 prompt 中能看到来自骗子酒馆的记忆（"X 在骗子酒馆里第 3 回合骗了我"）
- [ ] 在 DevLog 记录："跨游戏记忆迁移工作 / 失败原因"
- [ ] 如果失败，可能是 agent_id 不稳定或 Memory Service 在关卡切换时被意外重启

## 不在范围
- 多厂商 LLM 混搭（已由 T18.5 处理；本卡只观察现有混搭表现）
- 其他 22 张游戏卡片（M6+）
- 直播 / 推流功能
- 跨 PIE session 持久化（重启 PIE 即清记忆是接受的）

## 风险
- 涌现现象不一定每局都出现 — 跑 3 局期望覆盖到关键现象，如未达成观察项 ≥2 项，DevLog 应记录"未涌现"作为已知不足
- DeepSeek 在密集少数决调用下可能 ratelimit — 提前看 dashboard，必要时插入 cooldown 调长
- LLM 不输出严格 JSON 的容错占比应在可接受范围（< 20%），如频繁需要回手 T05 上 function calling
- CLAUDE.md 改动需要保持现有"关键规则 / 命令 / Monolith MCP"等章节结构不被破坏
