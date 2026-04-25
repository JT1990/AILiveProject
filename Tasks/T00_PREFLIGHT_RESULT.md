# T00 — 环境前置确认 结果

最后更新：2026-04-26

> **状态**：DoD #1–#7 + #9–#11 全部 ✅；DoD #8 录屏工具用户选定 OBS。**T00 关闭，可进 T01**。秘钥一律 masked。

## DoD 总览

| # | 项 | 状态 | 备注 |
| - | - | --- | ---- |
| 1 | Neo4j 实例 | ✅ | 5.26.22 community（Docker 容器 `story-next-neo4j`）→ T08 走 **native vector index** |
| 2 | qwen3-embedding 服务 | ✅ | ollama 0.18.0 / `/v1/models` 含 `qwen3-embedding:8b` / 单次 embedding HTTP 200 / **dim = 4096** / 首次 cold latency 3436 ms |
| 3 | DeepSeek 账号 | ✅ | HTTP 200 / latency 571 ms / model alias `deepseek-chat` → `deepseek-v4-flash` / 9 tokens 用量 / 账户余额充足 |
| 3b | GLM 账号 | 🟡 keys 在场 | `.env` 含 `GLM_API_BASE` + `GLM_API_KEY`；T18.5 才使用，本卡不发请求 |
| 4 | NPC Pawn / Actor | ✅ | `BP_NPC_MH_Character_*` → `SandboxCharacter_Mover` → `APawn`，无需升级父类 |
| 5 | `.env` 文件健康 | ✅ | 所有必需 key 在场；`.gitignore:79` 命中；`git ls-files .env` 空 |
| 6 | 8 NPC 配置盘点 | ✅ | 详见 `DevLog/2026-04-26_npc_baseline_inventory.md` |
| 7 | UE 项目 baseline 烟测 | ✅ | Build.bat 通过（6.54 s，"Result: Succeeded"）；PIE T 键 TTS+A2F 烟测 OK |
| 8 | 录屏工具（弱前置） | ✅ | OBS（T17 / T26 验收用） |
| 9 | Monolith MCP 可用性 | ✅ | v0.12.0 / port 9316 / UE 5.7.51494982 / 988 actions |
| 10 | NPC speech actor baseline | ✅ | VisualOverride child = `BP_MH_Character_*`；`Face_Archetype_Skeleton_AnimBP` 含 `AnimGraphNode_ApplyACEAnimation` |
| 11 | AIController / AutoPossessAI | ✅ | `AIControllerClass` = 默认 `AAIController`；`AutoPossessAI` = `PlacedInWorld` |

## 1. Neo4j（DoD #1） ✅

`.env`：

```
NEO4J_URI=bolt://localhost:7687
NEO4J_USER=neo4j
NEO4J_PASSWORD=story...t123 (len=12)
```

部署形态：Docker 容器 `story-next-neo4j`（`docker start story-next-neo4j`）。本机 proxy 设置 `HTTP_PROXY=http://127.0.0.1:10808`，所有 localhost 调用必须 `--noproxy localhost,127.0.0.1`。

probe 结果：

```bash
curl --noproxy localhost,127.0.0.1 -I http://localhost:7474   → HTTP 200
curl --noproxy localhost,127.0.0.1 -u neo4j:*** \
  -X POST http://localhost:7474/db/neo4j/tx/commit \
  -d '{"statements":[{"statement":"CALL dbms.components() ..."}]}'
→ {"row":["Neo4j Kernel",["5.26.22"],"community"]}
```

→ Neo4j Kernel **5.26.22 community**。版本 5.x，T08 走 **native vector index**（`db.index.vector.createNodeIndex`），不需 Python 端 cosine fallback。

## 2. qwen3-embedding（DoD #2） ✅

`.env`：

```
EMBEDDING_API_BASE=http://localhost:11434/v1
EMBEDDING_MODEL_NAME=qwen3-embedding:8b
```

部署：ollama 0.18.0（Linux 进程，`ollama serve`）。

probe 结果：

```bash
curl --noproxy localhost,127.0.0.1 http://localhost:11434/v1/models
→ HTTP 200, {"data":[{"id":"qwen3-embedding:8b","object":"model",...}]}

curl --noproxy localhost,127.0.0.1 -X POST http://localhost:11434/v1/embeddings \
  -H "Content-Type: application/json" \
  -d '{"model":"qwen3-embedding:8b","input":"你好"}'
→ HTTP 200, {"data":[{"embedding":[...]}], "model":"qwen3-embedding:8b",
              "usage":{"prompt_tokens":2,"total_tokens":2}}
```

- **embedding 维度 = 4096** → T08 `db.index.vector.createNodeIndex` 时填 dim=4096
- 首次 cold latency = 3436 ms（含模型加载）；后续应明显更快，T08 实测时再记 P50/P95
- 2 tokens for `"你好"`

## 3. DeepSeek（DoD #3） ✅

`.env`：

```
DEEPSEEK_API_BASE=https://api.deepseek.com/v1
DEEPSEEK_API_KEY=sk-71...3e3e (len=35)
```

实测：

```bash
curl https://api.deepseek.com/v1/chat/completions \
  -H "Authorization: Bearer $DEEPSEEK_API_KEY" \
  -H "Content-Type: application/json" \
  -d '{"model":"deepseek-chat","messages":[{"role":"user","content":"hi"}],"max_tokens":4}'
```

- HTTP 200
- latency 571 ms（单次）
- model 字段返回 `deepseek-v4-flash`（`deepseek-chat` 是 2026 别名，T05 prompt 工程时按需固定具体版本）
- usage：5 prompt + 4 completion = 9 tokens
- 账户余额充足（用户确认）

T05 实测 baseline 时再补 RPM / TPM（账号 dashboard 数字 + 真实 batched ping）。本卡不阻塞。

### 3b. GLM（候选，T18.5 用）

`.env` 已含：

```
GLM_API_BASE=https://open.bigmodel.cn/api/paas/v4
GLM_API_KEY=78673...zkDi (len=49)
```

T18.5 实施日 / 重新核实 model id（候选 `glm-4-plus` / `glm-4`，PRD 笔记里写过 `glm-5.1`，按当时官方文档为准）。本卡不发请求。

## 4. NPC Pawn / Actor（DoD #4） ✅

```
BP_NPC_MH_Character_1..8 → SandboxCharacter_Mover → APawn
```

✅ P0 强约束 B 满足；T18 / T19 / T19.7 直接走 `AAIController::MoveToActor` + Perception 路线，无需父类升级。

## 5. `.env` 健康（DoD #5） ✅

| Key | 状态 | mask |
| --- | --- | --- |
| `minimax` | ✅ | `sk-ap...SCVs (len=126)` |
| `DEEPSEEK_API_BASE` | ✅ | `https://api.deepseek.com/v1` |
| `DEEPSEEK_API_KEY` | ✅ | `sk-71...3e3e (len=35)` |
| `GLM_API_BASE` | ✅ | `https://open.bigmodel.cn/api/paas/v4` |
| `GLM_API_KEY` | ✅ | `78673...zkDi (len=49)` |
| `EMBEDDING_API_BASE` | ✅ | `http://localhost:11434/v1` |
| `EMBEDDING_MODEL_NAME` | ✅ | `qwen3-embedding:8b` |
| `NEO4J_URI` | ✅ | `bolt://localhost:7687` |
| `NEO4J_USER` | ✅ | `neo4j` |
| `NEO4J_PASSWORD` | ✅ | `story...t123 (len=12)` |

`.gitignore` 检查：

```
git ls-files .env       → (空，未被 track)
git check-ignore -v .env → .gitignore:79 命中
```

## 6. 8 NPC 配置盘点（DoD #6） ✅

→ `DevLog/2026-04-26_npc_baseline_inventory.md`

要点：

- 8 只壳 Pawn 同形：单变量 `FixedVisualOverrideClass` 指各自 `BP_MH_Character_N_C`
- BeginPlay 在壳 Pawn = `Super → SetFixedAndApply`；PrewarmA2F 实际在 visual child 上调（CLAUDE.md 描述应在 M0 同步收紧）
- T 键 TTS handler 在 visual child（不是壳 Pawn）

## 7. UE baseline 烟测（DoD #7） 🟡 partial

### 7a. Build.bat ✅

```
"D:\Software\UE_5.7\Engine\Build\BatchFiles\Build.bat" AILiveProjectEditor Win64 Development \
  -Project="D:/Project/Unreal/AILiveProject/AILiveProject.uproject"
```

结果：`Result: Succeeded` / Total execution time 6.54 s / "Target is up to date"

警告（非阻塞）：
- `Plugin 'Monolith' depends on plugin 'StructUtils' which was deprecated in 5.5` —— Monolith 插件链路，本项目不动
- 同警告对 `AILiveProjectEditor` —— 同上
- "Invalidating makefile for AILiveProjectEditor (DefaultEngine.ini modified)" —— 之前编辑器内的 Project Settings 改动，无 cpp 重编

### 7b. PIE T 键 TTS 烟测 ✅

用户手点：PIE `Level_AILive.umap` → 按 T → TTS+A2F 链路 OK（`MinimaxSpeechClient` → `MinimaxACELibrary::TriggerMinimaxSpeech` → `ACEAudioCurveSource` → `ApplyACEAnimation`）。

## 8. 录屏工具（DoD #8，弱前置） ✅

**OBS**（用户选定）。T17 / T26 验收阶段录视频证据。

## 9. Monolith MCP（DoD #9） ✅

`mcp__monolith__monolith_status`：

```
version=0.12.0, server_running=true, server_port=9316,
total_actions=988, namespaces=13,
engine_version=++UE5+Release-5.7-CL-51494982
```

CLAUDE.md "MCP 一律用 Monolith" 强约束 ✅。

## 10. NPC speech actor baseline（DoD #10） ✅

抽样 `BP_MH_Character_1`，结构 MetaHuman 模板生成，8 只同形：

- VisualOverride child = `BP_MH_Character_*`（Actor 子类）
- 关键组件：`Body > Face` 下挂 `ACEAudioCurveSource`（A2F 角色契约第 1 项 ✅）
- Face 组件 AnimClass = `Face_Archetype_Skeleton_AnimBP_C`（共享）
- AnimGraph 含 `AnimGraphNode_ApplyACEAnimation`（"Apply ACE Face Animations"，A2F 角色契约第 2 项 ✅）

T06 `ResolveSpeechActor` 设计：壳 Pawn 上拿 `AC_VisualOverrideManager.VisualOverride.GetChildActor()` 直接得到 visual child，传给 `TriggerMinimaxSpeech`。

## 11. AIController / AutoPossessAI（DoD #11） ✅

CDO（继承自 `APawn`）：

- `AIControllerClass` = `/Script/AIModule.AIController`（**默认**，未自定义）
- `AutoPossessAI` = `PlacedInWorld`（放置即自动 possess）
- `AutoPossessPlayer` = `Disabled`

T18 Perception 组件标准做法是挂在 AIController 上。本工程 NPC 用默认 `AAIController`；如需挂 Perception，可考虑将 T19.7 的 `AIC_NPC_SmartObject` 扩成统一 NPC AIController，但属于 T18 设计点，**T00 仅记录**，不动。

## 风险 & 决策影响（Step 5 roll-up）

### 已确定的关键决策（数字落地）

1. **Neo4j 5.26.22 community** → T08 走 native vector index（`db.index.vector.createNodeIndex`），不需 Python cosine fallback
2. **embedding dim = 4096**（qwen3-embedding:8b）→ T08 vector index 维度参数 = 4096
3. **NPC 已是 Pawn 子类** → M0 不需要插升 Pawn 卡；T18 / T19 / T19.7 走标准 `AAIController::MoveToActor` + Perception 路线
4. **AIControllerClass 默认 AAIController** → T18 Perception 组件挂哪里需 T18 设计点决定（候选：扩 `AIC_NPC_SmartObject`）

### 工程级发现（非阻塞，记录待跟进）

5. **`HTTP_PROXY=http://127.0.0.1:10808` 全局代理** → 后续所有访问 localhost 服务（Neo4j / ollama / Memory Service）必须 `--noproxy localhost,127.0.0.1` 或在请求层显式跳过代理。Memory Service 客户端（T09 `UMindMemoryClient`）需要在 `HTTPClient` 上显式 bypass proxy for localhost；写到 T04 的 README + T09 实现里。
6. **CLAUDE.md "BeginPlay 时序"两层**：壳 Pawn = `Super → SetFixedAndApply → MindComponent.Initialize`；visual child = `PrewarmA2F → ...`。M0 完成由 T07 卡同步 CLAUDE.md。
7. **T 键 + TTS handler 在 visual child（不是壳 Pawn）** → T06 `ResolveSpeechActor` 走 `VisualOverride.GetChildActor()`。
8. **`AIC_NPC_SmartObject`** 存在但 NPC 未引用 → T19.7 / T18 设计时再决定要不要升格成 NPC 默认 AIController。
9. **`AnimateCharacterFromWavFileAsync` 死分支**留在 visual child EventGraph 里 → T15 Montage 接入 / M3 polish 时清；本卡不动。
10. **StructUtils 5.5 deprecated** 警告（来自 Monolith 插件依赖）→ 非项目自身问题，跟 Monolith 上游升级。

### 后置实测（不阻塞 M0，T05 时补）

11. **DeepSeek RPM / TPM 实测 baseline** → T05 验收阶段做 batched ping，结果记 `Tasks/T05_RATELIMIT_BASELINE.md`
12. **embedding warm latency P50/P95** → T08 实测，作为 NPC 决策延迟预算依据
13. **GLM 候选 model id** → T18.5 实施日按官方最新 model 列表核实

## 输出文件

- `Tasks/T00_PREFLIGHT_RESULT.md`（本文件）
- `DevLog/2026-04-26_npc_baseline_inventory.md`
- `Tools/MemoryService/README.md`（仅框架，T04 填实质）
