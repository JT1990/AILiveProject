# T00 — 环境前置确认

## 目标
所有 26 张实施卡都假设若干外部环境就绪。任何一项缺位都会让某个里程碑当场翻车。这张卡把所有假设变成显式 checklist，全过才进 T01。**不写任何代码，只验证现状 + 收集事实**。

## 前置
无（最早的卡）

## DoD（每项都要 ✅ 或明确替代方案）

### 1. Neo4j 实例
**已确认**（`.env`）：
```
NEO4J_URI=bolt://localhost:7687
NEO4J_USER=neo4j
NEO4J_PASSWORD=storynext123
```
- [ ] 验证 Neo4j 已启动：浏览器打开 `http://localhost:7474` 能登录
- [ ] 验证版本是 5.x（用 vector index）：`call dbms.components()` 看 version
  - 若 4.x：T08 用 Python 端 cosine 排序回退方案

### 2. qwen3-embedding 服务
**已确认**：ollama OpenAI-compatible 接口。`.env` 中：
```
EMBEDDING_API_BASE=http://localhost:11434/v1
EMBEDDING_MODEL_NAME=qwen3-embedding:8b
```
- [ ] 验证 ollama 已运行：`curl http://localhost:11434/v1/models` 返回 200
- [ ] 验证 model 已 pull：`ollama list` 看到 `qwen3-embedding:8b`
- [ ] 实测一次：
  ```
  curl -X POST http://localhost:11434/v1/embeddings \
       -H "Content-Type: application/json" \
       -d '{"model":"qwen3-embedding:8b","input":"你好"}'
  ```
  返回 `{"data":[{"embedding":[...]}]}`，记下 embedding 长度（通常 4096，用于 T08 vector index 维度）

### 3. DeepSeek 账号
**已确认**（`.env`）：
```
DEEPSEEK_API_BASE=https://api.deepseek.com/v1
DEEPSEEK_API_KEY=sk-...
```
- [ ] 测试连通：
  ```bash
  curl https://api.deepseek.com/v1/chat/completions \
    -H "Authorization: Bearer sk-..." \
    -H "Content-Type: application/json" \
    -d '{"model":"deepseek-chat","messages":[{"role":"user","content":"hi"}]}'
  ```
  返回正常 chat completion
- [ ] 账号余额 / tier 确认（影响 ratelimit）
- [ ] Ratelimit 估算（账号 dashboard 看 RPM/TPM 限额）

### 3b. GLM 账号（T18.5 用）
**待用户提供**：
- 申请 GLM (智谱) 账号 + API key
- 候选 endpoint: `https://open.bigmodel.cn/api/paas/v4`（OpenAI-compatible）
- 候选 model: `glm-4-plus` 或 `glm-4`
- 加入 `.env`：
  ```
  GLM_API_BASE=https://open.bigmodel.cn/api/paas/v4
  GLM_API_KEY=...
  ```

### 4. `BP_NPC_MH_Character` 类型链确认
用 Monolith MCP：
- [ ] `monolith_status` 在线
- [ ] `get_class_info(BP_NPC_MH_Character)` 拿父类链
- [ ] 确认是 `APawn` 子类还是 `AActor` 子类（影响 T19 MoveTo / T18 Perception 全部）
- [ ] 如果是 Pawn，确认 `AIControllerClass` 配置是什么（默认还是自定义）
- [ ] **结果记到** `Tasks/T00_PREFLIGHT_RESULT.md`

**判断后果**：
- 如果是 `APawn` 子类 → T19 MoveTo 用 `AAIController::MoveToActor` 路线
- 如果是 `AActor` 子类 → T19 改为手撸位移（用 `SetActorLocation` + 自己实现 NavMesh path follow），或在 M4 之前升级父类为 Pawn（更大改动）

### 5. `.env` 文件健康
- [ ] `D:/Project/Unreal/AILiveProject/.env` 存在
- [ ] 包含 `minimax=sk-...`（M0 之前已有）
- [ ] 计划添加 `deepseek=sk-...`（T01 实施时）
- [ ] **`.env` 在 `.gitignore` 中**（必须验证；如果不在就加）
- [ ] `git status` 确认 `.env` 不在 staged/tracked

### 6. 现有 8 NPC 配置盘点
用 Monolith MCP 对 `BP_NPC_MH_Character_1..8` 各自跑：
- [ ] `get_variables` 拿 `FixedVisualOverrideClass` 当前指向（有/无）
- [ ] `get_components` 拿当前组件列表（确认没有遗留的 MindComponent 类似变量）
- [ ] `get_execution_flow(BeginPlay)` 拿当前 BeginPlay 调用链（确认 `PrewarmA2F` + `SetFixedAndApply` 顺序）
- [ ] `get_execution_flow(T 键事件)` 拿当前 T 键的硬编码 TTS 逻辑（M2/M4 改造前作为 baseline）
- [ ] **结果记到** `DevLog/2026-04-XX_npc_baseline_inventory.md`

### 7. UE 项目 baseline 烟测
- [ ] 当前主分支能 `Build.bat AILiveProjectEditor Win64 Development` 通过
- [ ] 加载 `Level_AILive.umap` PIE 不报 Error
- [ ] 按 T 键 NPC 能正常说话（验证现有 TTS+A2F 链路 OK）

### 8. 录屏工具（弱前置）
- [ ] 你有什么录屏工具（OBS / 引擎自带 / ShareX / 其他）—— T17 / T26 验收要录像

## 关键操作（Monolith MCP 调用清单）

```
monolith_status
get_class_info(BP_NPC_MH_Character)         # 类型确认
get_components(BP_NPC_MH_Character_1)        # 组件盘点
get_components(BP_NPC_MH_Character_2..8)
get_variables(BP_NPC_MH_Character_1..8)
get_execution_flow(BP_NPC_MH_Character.BeginPlay)
get_execution_flow(BP_NPC_MH_Character.OnT)  # T 键事件
```

## 验收信号

填一份 `Tasks/T00_PREFLIGHT_RESULT.md`，覆盖上面 1-7 各项，每项要么 ✅ 要么写明替代方案。8 项不强求。

## 不在范围
- 任何代码改动
- 任何资产创建

## 风险

- 如果 NPC 是 Actor 而非 Pawn，T19 改造量大，建议 M4 之前回到 BP 父类升级
- qwen3-embedding 部署形式不明会卡死 T08（Memory Service write/recall），必须本卡里彻底确认
- DeepSeek 账号 ratelimit 不够会让 M5 不可执行（一局 200+ 调用 / 60s）

## 输出文件

- `Tasks/T00_PREFLIGHT_RESULT.md`（你手填的环境清单）
- `DevLog/2026-04-XX_npc_baseline_inventory.md`（NPC baseline 盘点）
- `Tools/MemoryService/README.md` 框架（含环境变量段，但服务本身在 T04 才建）
