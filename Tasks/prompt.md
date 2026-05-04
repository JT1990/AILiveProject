执行 @Tasks/T7_runtick_bid_parser.md 
 
工作流：
1. 读 Tasks/T7_runtick_bid_parser.md 拿到目标 / 涉及文件 / 依赖 / 验收方式 / 完成定义。如果依赖任务（blockedBy）尚未完成，停下来问我。 
2. 读 Tasks/00_overview.md 了解整体定位与依赖图，注意"文档冲突 / 待澄清"段对本任务的影响。
3. 按任务卡"预检"段做差量：C++ 状态用 Read/Grep；Blueprint/资产用 Monolith MCP（先 mcp__monolith__monolith_status 确认在线）。
4. 按需读真相源文档（不要全文 Read，按 offset/limit 切片, 不要照搬代码块——它是参考实现，不是逐行复制脚本）：
   - 任务卡对应的 principles 章节（参考 Tasks/00_overview.md 的"§11 验收"列）
   - schema.yaml 仅当本任务涉及新字段或 DDL 变更
   - PRD.md 仅当本任务触及 AI 身份 / Delete 协议（T1/T6/T9）
   - CLAUDE.md + AGENTS.md 永远扫一眼
5. 按"涉及文件"逐项实现，每改一个文件用 TaskCreate 跟踪进度。
6. 跑"验收方式"中**全部**用例（不是子集），机械验证：SQL 返回 / DB Browser 看得到 / Hash verify 通过 / PIE 烟测。任一用例失败 = 任务未完成，调试根因。
7. 全部通过后写一篇 DevLog/YYYY-MM-DD_<topic>.md（任务卡末尾"里程碑 DevLog"段给了文件名建议），记录决策与验收实证。

硬约束（违反即停）：
- 不动 .uproject plugin 列表之外的引擎设定 / .Target.cs 的 DefaultBuildSettings = V6 / DefaultEngine.ini 的 bTickPhysicsAsync。
- 既有 GASP / Mover / Visual-override / NPC 父类 BP 链路保持原样，不重写。
- 任务卡之外的代码不顺手清理；只动卡片"涉及文件"列出的内容。

遇到下列情况停下来问我（用 AskUserQuestion），不自行决定：
- 任务卡 / 00_overview.md 的"文档冲突 / 待澄清"段触发的歧义。
- 真相源文档之间矛盾（schema.yaml vs implementation vs principles）。
- 验收用例需要新增没在卡片里的资产 / BP / 数据。
- 发现既有 inventory 与卡片描述不一致（资产被改过、字段已迁移等）。

—

提示：

- T0 → T9 必须按依赖顺序执行（T0 → T1 → T2 → T2.5 → T3 → 之后分支）。新会话执行任务前先 git status 确认上一任务的产出已 commit。
- 如果任务太大想分段交付，告诉新会话「只完成验收方式的前 N 项」即可——卡片本身允许子集递交。
- 总览的"端到端验证"是 T9 完成后的全局检查，不属于任何单个任务。

---

 
特殊任务的强制段落清单（直接告诉新会话读哪些）：

- T2.5：Read memory_principles.md offset=276 limit=120（§5.1 四通道 + §5.2bis Bid + §5.4 双 LLM）+ offset=712 limit=85（§A.1 OUTPUT FORMAT 模板，反向编码 Parser system prompt 用）
- T3：Read memory_principles.md offset=63 limit=90（§二底层不变量）+ offset=587 limit=18（§7.2 单 orchestrator 串行）
- T6：Read memory_principles.md offset=176 limit=70（§4.3 prompt 拼装优先级）+ offset=712 limit=200（附录 A 全部模板）
- T7：Read memory_principles.md offset=270 limit=200（§5 全部协议）+ offset=531 limit=80（§6.1 BEL_EXT 9 项）
- T8：Read memory_principles.md offset=153 limit=20（§4.2 投影语义）+ offset=622 limit=15（§7.4 commitments）
- T9：Read memory_principles.md offset=607 limit=30（§7.3 故障处理）+ offset=661 limit=25（§8.3 Delete）+ Read PRD.md offset=145 limit=25（Delete 协议工程层落点）