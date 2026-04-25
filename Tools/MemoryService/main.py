"""AI Live Memory Service — T04 骨架。

只暴露 /health；写/查记忆留 T08 实现。startup 阶段对 Neo4j 与 ollama
embedding 服务做一次存活探测，结果落到 /health 返回的 bool 字段里。
"""

import os
from pathlib import Path

import httpx
from dotenv import load_dotenv
from fastapi import FastAPI
from neo4j import GraphDatabase

from models import HealthResponse

# Tools/MemoryService/main.py → 项目根（parents[2]）
PROJECT_ROOT = Path(__file__).resolve().parents[2]
load_dotenv(PROJECT_ROOT / ".env")

NEO4J_URI = os.getenv("NEO4J_URI", "bolt://localhost:7687")
NEO4J_USER = os.getenv("NEO4J_USER", "neo4j")
NEO4J_PWD = os.getenv("NEO4J_PASSWORD", "")
EMBED_URL = os.getenv("EMBEDDING_API_BASE", "http://localhost:11434/v1")
EMBED_MODEL = os.getenv("EMBEDDING_MODEL_NAME", "qwen3-embedding:8b")

VERSION = "0.1"

app = FastAPI(title="AI Live Memory Service", version=VERSION)

driver = None
neo4j_ok = False
embed_ok = False


@app.on_event("startup")
async def boot() -> None:
    global driver, neo4j_ok, embed_ok

    try:
        driver = GraphDatabase.driver(NEO4J_URI, auth=(NEO4J_USER, NEO4J_PWD))
        with driver.session() as s:
            s.run("MATCH (n) RETURN count(n) LIMIT 1").single()
        neo4j_ok = True
        print(f"[neo4j] ok @ {NEO4J_URI}")
    except Exception as e:
        print(f"[neo4j] {e}")

    # trust_env=False: 本机有全局 HTTP_PROXY=http://127.0.0.1:10808，
    # 必须 bypass 才能直连 localhost:11434 的 ollama。
    try:
        async with httpx.AsyncClient(trust_env=False, timeout=3.0) as c:
            r = await c.get(f"{EMBED_URL}/models")
            embed_ok = r.status_code == 200
        print(f"[embed] {'ok' if embed_ok else f'http {r.status_code}'} @ {EMBED_URL}")
    except Exception as e:
        print(f"[embed] {e}")


@app.on_event("shutdown")
def teardown() -> None:
    if driver is not None:
        driver.close()


@app.get("/health", response_model=HealthResponse)
def health() -> HealthResponse:
    return HealthResponse(
        status="ok",
        neo4j=neo4j_ok,
        embedding=embed_ok,
        version=VERSION,
    )
