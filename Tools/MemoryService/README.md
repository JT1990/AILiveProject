# Memory Service (framework, T00)

> **状态**：T00 留下的目录占位与环境变量段。服务本体在 T04 卡（`Tasks/task_04_memory_service_health.md`）实现，包含 FastAPI app + `/health` + Neo4j 客户端 + ollama embedding 客户端。本文件只在 T04 起补充 install / run 实质内容。

## 用途

CLAUDE.md "AI 心智决策系统" 的记忆层。NPC 通过 `UMindMemoryClient`（C++）走本地 HTTP REST 与本服务通信：

- `POST /memory/write` —— 写入一条记忆（含 embedding via ollama）
- `POST /memory/recall` —— 语义检索（Neo4j 5.x native vector index 或 Python 端 cosine fallback）
- `POST /memory/by_tag` —— 按 tag/speaker 维度检索（跨局关系 / 跨游戏会话）

存储后端：Neo4j 5.x（开发期 bolt://localhost:7687）。Embedding：ollama OpenAI-compat 接口（默认 qwen3-embedding:8b）。

## 部署

- 开发期：`uvicorn main:app --reload` 手动起；与 UE 编辑器同主机即可。
- 持久化：同进程内跨关卡 / 跨局；不要求 Neo4j 重启后恢复（参见 PRD "持久化范围"）。
- 不做：认证 / TLS / 多用户隔离（本地端口任意进程可访问，明确写在不在 MVP 范围）。

## 环境变量（来源：项目根 `.env`）

服务启动时读取以下 keys（**不在本文件回显 value**；查看 / 修改实际值请直接编辑 `.env`）：

| Key | 用途 | 备注 |
| --- | --- | --- |
| `NEO4J_URI` | Bolt 连接串 | 默认 `bolt://localhost:7687` |
| `NEO4J_USER` | 用户名 | 默认 `neo4j` |
| `NEO4J_PASSWORD` | 密码 | masked |
| `EMBEDDING_API_BASE` | ollama OpenAI-compat 基地址 | 默认 `http://localhost:11434/v1` |
| `EMBEDDING_MODEL_NAME` | embedding 模型名 | 默认 `qwen3-embedding:8b` |

`.env` 本身受 `.gitignore` 保护，禁止 commit；服务读取后仅在内存中保留，不要写日志或 metrics 中泄露。

## T00 已确定的数字（T04 实现时直接用）

- **Neo4j 版本 5.26.22 community** → 走 native vector index（`db.index.vector.createNodeIndex`），不需 Python cosine fallback
- **embedding 维度 = 4096**（实测 `qwen3-embedding:8b`，输入 `"你好"`）→ vector index `dim = 4096`
- **localhost 代理坑**：本机有全局 `HTTP_PROXY=http://127.0.0.1:10808`，所有访问 `localhost:7687` / `localhost:11434` 必须显式 bypass。FastAPI 服务进程内：
  - `httpx`：传 `proxies=None` 或在 `Trust env=False` 下创建 client
  - `requests`：`session.trust_env = False` 或 `proxies={"http": "", "https": ""}`
  - `neo4j` 官方 driver：bolt 协议不走 HTTP proxy，但若用 HTTP REST 接口同样要 bypass
- **Neo4j 部署**：Docker 容器名 `story-next-neo4j`，启动方式 `docker start story-next-neo4j`

## 待 T04 填充

- 目录结构（`main.py` / `routes/` / `clients/` / `tests/`）
- 启动 / 停止命令
- `/health` 检查项（Neo4j ping + ollama models 可达 + embedding 维度断言 = 4096）
- vector index 建表脚本（5.x native）
- warm-up 路径：首次 cold latency = 3436 ms（T00 实测，含模型加载），warm latency 在 T08 实测后回填

## 不在范围

- 长期生产部署（Docker / systemd / 进程监控）
- 跨主机部署 / 鉴权
- 备份 / 灾恢
