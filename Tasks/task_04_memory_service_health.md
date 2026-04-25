# T04 — Memory Service Python 项目 + `/health`

## 目标
搭建 `Tools/MemoryService/` 的 Python 项目骨架，仅实现 `/health` 端点；连接 Neo4j 与 qwen3-embedding 暂不实现，但要在 startup 里做存活探测并打 log。

## 前置
无（与 UE 侧解耦，可与 T01-T03 并行做）

## DoD
- [ ] `Tools/MemoryService/` 目录建立，包含 `main.py` / `models.py` / `requirements.txt` / `start.bat` / `README.md`
- [ ] `pip install -r requirements.txt` 通过（fastapi / uvicorn / neo4j / httpx / pydantic）
- [ ] `start.bat` 一键起服务在 `127.0.0.1:8765`
- [ ] `curl http://127.0.0.1:8765/health` 返回 `{"status":"ok","neo4j":bool,"embedding":bool,"version":"0.1"}`
- [ ] startup 阶段尝试 `MATCH (n) RETURN count(n) LIMIT 1` 探测 Neo4j；尝试 embedding 服务的 health（如有）
- [ ] README 里写明启动方法 + 端口选择 + Neo4j / embedding 配置位置（环境变量或 `.env`）

## 关键文件
全部新建。

## 关键 API / 伪代码

**配置来源**：直接读项目 `.env` 文件（与 UE 端共用，避免双写）。.env 中已有：
- `NEO4J_URI=bolt://localhost:7687`
- `NEO4J_USER=neo4j`
- `NEO4J_PASSWORD=storynext123`
- `EMBEDDING_API_BASE=http://localhost:11434/v1`（**ollama OpenAI-compat 接口**）
- `EMBEDDING_MODEL_NAME=qwen3-embedding:8b`

```python
# main.py
from fastapi import FastAPI
from dotenv import load_dotenv
import os
from pathlib import Path
from neo4j import GraphDatabase
import httpx

# 加载 ../../.env （从 Tools/MemoryService/main.py 看回项目根）
PROJECT_ROOT = Path(__file__).resolve().parents[2]
load_dotenv(PROJECT_ROOT / ".env")

app = FastAPI(title="AI Live Memory Service", version="0.1")

NEO4J_URI = os.getenv("NEO4J_URI", "bolt://localhost:7687")
NEO4J_USER = os.getenv("NEO4J_USER", "neo4j")
NEO4J_PWD = os.getenv("NEO4J_PASSWORD", "")
EMBED_URL = os.getenv("EMBEDDING_API_BASE", "http://localhost:11434/v1")  # OpenAI-compat
EMBED_MODEL = os.getenv("EMBEDDING_MODEL_NAME", "qwen3-embedding:8b")

driver = None
neo4j_ok = False
embed_ok = False

@app.on_event("startup")
async def boot():
    global driver, neo4j_ok, embed_ok
    try:
        driver = GraphDatabase.driver(NEO4J_URI, auth=(NEO4J_USER, NEO4J_PWD))
        with driver.session() as s:
            s.run("MATCH (n) RETURN count(n) LIMIT 1").single()
        neo4j_ok = True
    except Exception as e:
        print(f"[neo4j] {e}")
    try:
        # OpenAI-compat 健康探测：列模型
        async with httpx.AsyncClient() as c:
            r = await c.get(f"{EMBED_URL}/models", timeout=3)
            embed_ok = r.status_code == 200
    except Exception as e:
        print(f"[embed] {e}")

@app.get("/health")
def health():
    return {"status":"ok","neo4j":neo4j_ok,"embedding":embed_ok,"version":"0.1"}
```

```bat
:: start.bat
@echo off
python -m uvicorn main:app --host 127.0.0.1 --port 8765 --reload
```

```
# requirements.txt
fastapi>=0.110
uvicorn[standard]>=0.27
neo4j>=5.18
httpx>=0.27
pydantic>=2.0
python-dotenv>=1.0
```

## 验收信号
- `cd Tools/MemoryService && start.bat`，控制台看到 `Uvicorn running on http://127.0.0.1:8765`
- 浏览器或 curl 访问 `/health`，返回上面 JSON
- 即使 Neo4j 没起（`neo4j: false`），服务本身也能跑（不阻塞）

## 不在范围
- 实际写/查记忆（T08）
- Embedding 实际调用（T08）
- Docker 化（不做）

## 风险
- **已确认**：qwen3-embedding 通过 ollama OpenAI-compatible 接口（`http://localhost:11434/v1/embeddings`）；T08 调用走此路径
- **已确认**：Neo4j 在 `bolt://localhost:7687`，凭据 `neo4j / storynext123`
- 启动 ollama 需要先 `ollama pull qwen3-embedding:8b` + `ollama serve`，本前置在 T00 已检查
