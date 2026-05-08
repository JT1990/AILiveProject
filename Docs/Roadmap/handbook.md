# 执行手册：Memory Principles Roadmap 子任务卡片

> 配套：`Docs/Roadmap/00_total_plan.md`（总计划，已永久化）。
> 用途：把总计划切成 **10 张独立窗口可执行的子任务卡**，避免单个 ClaudeCode 对话过长导致输出质量下降。
> 颗粒度原则：**强内聚的子号合卡，独立模块单卡**。

---

## 0. 工作流总览

每张卡片是 **self-contained launcher**：一个全新的 ClaudeCode 窗口（无任何前序对话上下文）只需打开对应卡片文件、按"启动 prompt"开工即可。

### 启动新窗口的标准操作

1. 在 `D:\Project\Unreal\AILiveProject` 根目录新开 ClaudeCode 窗口（UE 侧任务）；或在 `D:\Project\Unreal\AILiveProject\BrainService` 新开窗口（Brain 侧任务，需要先创建该目录与初始化独立 git repo——见 T1）。
2. 第一句话粘贴卡片里的 **"启动 prompt"** 段。
3. ClaudeCode 按 prompt 内的"必读上下文 / 范围内 / 范围外 / 验收"开始工作。
4. 工作完成后，由人类（不是 ClaudeCode）在本手册的 **进度跟踪表** 把对应卡片标记为 ✅。

### 通用 launcher prompt 模板（任何卡片复制此结构）

```text
你是 AILive 项目 Memory Principles 路线图的 <卡号> 子任务执行者。

**必读上下文**（按顺序读，读完再动）：
1. Docs/Roadmap/00_total_plan.md ——总路线图（重点读"边界""阶段 <X>""关键依赖图"三节）
2. Docs/memory_principles.md §<相关章节>
3. Docs/Roadmap/<卡号>_<name>.md ——本任务的详设卡（如已生成）
4. <其他必读文件，每张卡具体列出>

**任务范围（DO）**：<列出本卡要交付的所有内容>
**范围外（DON'T）**：<明确列出哪些事情不属于本卡，应留给其他卡>

**约束**：
- 严格遵守 CLAUDE.md（项目级与用户级两份）。
- 所有 UE 资产改动用 Monolith MCP（不让用户手点编辑器）。
- 所有 Brain Python 代码遵守 memory_principles 数据层/协议层不变量。
- 完成时按本卡"验收"段自检，给出验收清单逐条勾选状态。

**第一步**：先把上述必读文件全部读完，然后用 1-3 句话给我复述你理解的任务范围与与上下游卡片的边界，等我确认后再动手。
```

> 把上面 `<...>` 的占位换成具体内容即可。**关键约束是"先读必读文件再动手"**——避免新窗口在缺乏上下文的情况下凭直觉做决策。

---

## 1. 子任务卡片清单（10 张）

| 卡号 | 名称 | 合并自原子号 | 主仓库 | 状态 |
|---|---|---|---|---|
| **T1** | 协议骨架定稿 + BrainService 仓库初始化 | 0.1 + 0.1.5 + 0.2 + 0.3 + 0.4 | BrainService + UE Docs | ⬜ |
| **T2** | Brain 数据层骨架（EventStore + Schema + 视角隔离 + 投影 + 召回 + `_meta.db`） | 1.1–1.6 + 2.7 commitments | BrainService | ⬜ |
| **T3** | Brain Reasoner + Validator 主链 | 2.1 + 2.2 + 2.3 | BrainService | ⬜ |
| **T4** | Brain Floor Control + Listener-as-filter | 2.4 + 2.5 | BrainService | ⬜ |
| **T5** | Brain 三级反思 + prompt 拼装 | 2.6 + 2.8 | BrainService | ⬜ |
| **T6** | UE 协议基础设施 + 世界状态采集 | 3.1 + 3.2 + 3.3 + 3.4 | UE C++ | ⬜ |
| **T7** | UE 动作 / 语音 dispatcher + 完成回调 wrapper + IngressValidator + 测试键退役 | 3.5 + 3.6 + 3.7 + 3.8 + 3.9 | UE C++ + BP | ⬜ |
| **T8** | 物品 / Delete / Faction 占位 / 配置整理 | 4.1 + 4.2 + 4.3 + 4.4 + 4.5 | UE C++ + BP + Brain | ⬜ |
| **T9** | 02 病毒游戏规则 spec + Brain 机制 | 5.1 + 5.2 | Brain Docs + BrainService | ⬜ |
| **T10** | UE touch 检测 + 一局完整 PIE 联合验证 | 5.3 + 5.4 | UE C++ + BP + 联调 | ⬜ |

---

## 2. 关键依赖（卡片层级）

```
T1 (协议骨架)
  ├─→ T2 (数据层骨架)
  │     ├─→ T3 (Reasoner+Validator) ─┐
  │     │                               ├─→ T4 (Floor+Filter)
  │     │                               ├─→ T5 (反思+prompt)
  │     │                               └─→ T9 (病毒规则+Brain 机制)
  │     └─→ T8 (物品/Delete) ──→ T10
  ├─→ T6 (UE 基础设施 + 状态采集) ──→ T7 (UE 主体 dispatcher) ──→ T10
  └─→ T9 ──→ T10 (联合验证)
```

**硬约束**：
- T1 必须最先（所有人共用同一份 protocol）。
- T2 必须先于 T3（schema 决定 Validator 校验内容）。
- T3 必须先于 T4 / T5 / T9（机制都依赖 Validator 通过的事件）。
- T6 必须先于 T7（Settings + HTTP + Roster 是基础设施）。
- T2 + T3 + T4 + T5 + T6 全部就绪后才启 T7（UE 端 dispatcher 需要 brain 派出真实事件）。
- T8 依赖 T2（Delete 需 `_meta.db`）+ T6（UE 基础设施）。
- T10 依赖 T7 + T8 + T9 同时就位。

**并行波次建议**（人类调度）：

- **波 1（T1 完成后）**：T2、T6 并行起步；T9 的"5.1 规则 spec"也可写文档（不依赖任何 brain 代码）。**T3 不进波 1**——T3 依赖 T2 的 schema/事件枚举/EventStore，必须等 T2 完成。
- **波 2（T2 完成后）**：启 T3。T6 也可在此波继续推进（T6 不依赖 T2，从波 1 继续即可）。
- **波 3（T3 完成后）**：T4 / T5 并行；T9 的"5.2 Brain 端机制"在此波启动。
- **波 4（T6 完成 + T3/T4/T5 全部 ✅）**：启 T7；T8 与 T7 可并行（T8 依赖 T2 + T6，不依赖 T7）。
- **波 5（T7 + T8 + T9 全部 ✅）**：启 T10 联调。

---

## 3. 卡片详设文件按需生成（不预先全建）

为避免 Docs/ 下堆一堆"还没开工就过时"的卡片，**详设文件仅在该卡即将开工前生成**。生成时机由人类调度：

- 卡 N 即将开工前 1–2 天 → 在主对话窗（这次对话或新一次普通模式对话）说"准备生成 T<N> 详设卡"，ClaudeCode 按本手册第 1 节模板 + 总计划相关章节产出 `Docs/Roadmap/T<NN>_<name>.md`。
- 详设卡确认后再开新窗口启动 T<N> 子任务。

每张详设卡的统一结构（生成时遵守）：

1. **目标**（1 句）
2. **必读上下文锚点**（具体文件 + 行号或章节）
3. **范围内 / 范围外**（明确边界）
4. **交付清单**（要新建 / 改的文件 + 一句话职责）
5. **验收**（怎么算做完，最好可机器化检查）
6. **上下游交接**（前置卡输出什么 / 本卡输出谁会消费）
7. **启动 prompt**（可粘贴到新窗口的开场提示，已把必读 + 任务边界写齐）
8. **风险与已知坑**（DevLog / memory_principles 已记的相关踩坑）

---

## 4. 已收敛的项目级决策（所有卡共享）

来自总计划：

- Brain Service 仓库 = `D:\Project\Unreal\AILiveProject\BrainService\`，独立 git，已 gitignored。
- 传输 = HTTP polling（`FHttpModule`），不引入 SSE / WebSockets。
- MVP 游戏 = 02 病毒游戏。
- ontology v1 = `move_to / sit / wait`（3 个 intent）；`speak` 走 `speech.public` 独立通道，不进 ontology。
- ontology v2 = 在 T8 阶段引入 `pickup / use_item / inspect / follow / flee_from`。
- actor_id 命名 = `NPC01..NPC10`，与 `BP_NPC_MH_Character_1..10` 1:1 映射；viewer 封闭集合 `{public, audience, orchestrator, system, NPC<NN>, Faction<X>}`。
- 协议真相源 = `BrainService/protocol/`（protocol.md + JSON Schema + examples）。UE 仓 `Docs/protocol_pointer.md` 记 commit hash。
- UE 永远只 POST 上报，事件流写入由 brain 协议层完成。
- `speech.public` 与 `action.intent` 在 schema / Dispatcher / 路由层全程分离。

---

## 5. 进度跟踪

子任务完成时由人类（不是 ClaudeCode）维护。把上面表格中对应行的"状态"列从 ⬜ 改为 ✅，并在下面追加完成记录：

| 日期 | 卡号 | 完成人 | 备注 |
|---|---|---|---|
| | | | |

---

## 6. 下一步

T1 详设卡已生成（`Docs/Roadmap/T01_protocol_skeleton.md`）。建议下一步：

1. 评审 T1 详设卡——重点核对 §6 验收清单是否覆盖所有硬边界，以及 §3 范围内的交付项与 §4 范围外是否互斥不交叉。
2. 评审通过后，新开 ClaudeCode 窗口，cwd 设为 `D:\Project\Unreal\AILiveProject\BrainService\`（允许它创建该目录并在其内 `git init`）。
3. 把 T1 §9 的"启动 prompt"原样粘贴到新窗口开工。
4. T1 完成后，由人类（不是 ClaudeCode）在本手册 §1 表格把 T1 状态从 ⬜ 改为 ✅，并在 §5 进度跟踪表追加一行登记。
