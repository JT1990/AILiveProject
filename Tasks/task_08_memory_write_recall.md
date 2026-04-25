# T08 — Memory Service `/memory/write` + `/memory/recall` + `/memory/by_tag`

## 目标
在 Memory Service 上实现 3 个核心端点：写入、向量检索、tag 精确检索。**`/memory/by_tag` 提前到本卡**（v1 设计在 T24 才加，导致 T16 跨游戏关系 prompt 时序倒挂）。对接 Neo4j 存储 + qwen3-embedding 向量化。

## 前置
T04（health 端点 + Neo4j/embedding 探测就位）

## DoD
- [ ] `POST /memory/write`：
  - body: `{agent_id, content, tags?: dict, ts?: epoch_ms}`
  - 流程：调 embedding 服务拿 vector → Neo4j MERGE Agent → CREATE MemoryNode 关联
  - 返回 `{id, ok}`
- [ ] `POST /memory/recall`：
  - body: `{agent_id, query, top_k}`
  - 流程：query → embedding → Neo4j 内向量相似度（cosine）按 agent_id 过滤 → 返回 top_k
  - 返回 `[{id, content, score, ts, tags}]`
- [ ] **新增 `POST /memory/by_tag`**：
  - body: `{agent_id, tag_key, tag_value, top_n: int=5}`
  - Cypher: `MATCH (a:Agent {id:$id})-[:REMEMBERS]->(m) WHERE m.tags[$key] = $val RETURN ... ORDER BY m.ts DESC LIMIT $n`
  - 返回 `[{id, content, score=1.0, ts, tags}]`
- [ ] Neo4j schema：
  - `(:Agent {id})` 节点
  - `(:Memory {id, content, embedding: list<float>, ts, tags: map})`
  - 关系 `(Agent)-[:REMEMBERS]->(Memory)`
- [ ] **vector 检索 MVP 路径收敛**：
  - 默认实现：**Python 端 cosine fallback**——按 agent_id 拉取该 agent 的全部 Memory，numpy 计算 cosine 排序 top_k
  - 如果 T00 确认 Neo4j 5.x 有 native vector index，再升级为 `db.index.vector.queryNodes`
  - 不引入 `gds.similarity.cosine`（GDS 是企业版功能）
- [ ] T08 验收时实测一次 embedding 长度（通常 4096），写到 `Tasks/T00_PREFLIGHT_RESULT.md` 的 embedding dim 字段

## 关键文件
- 修改 `Tools/MemoryService/main.py`
- 新增 `Tools/MemoryService/neo4j_client.py`
- 新增 `Tools/MemoryService/embedding_client.py`
- 新增 `Tools/MemoryService/models.py`

## 关键 API / 伪代码

```python
# models.py
class WriteReq(BaseModel):
    agent_id: str
    content: str
    tags: dict[str, str] = {}
    ts: int | None = None

class RecallReq(BaseModel):
    agent_id: str
    query: str
    top_k: int = 5

class MemoryItem(BaseModel):
    id: str
    content: str
    score: float
    ts: int
    tags: dict[str, str]
```

```python
# embedding_client.py（**ollama OpenAI-compat 接口**，路径 /v1/embeddings）
async def embed(text: str) -> list[float]:
    async with httpx.AsyncClient() as c:
        r = await c.post(f"{EMBED_URL}/embeddings",   # EMBED_URL 已含 /v1
                         json={"model": EMBED_MODEL, "input": text},
                         timeout=30)
    r.raise_for_status()
    # OpenAI 格式: {"data":[{"embedding":[...]}], ...}
    return r.json()["data"][0]["embedding"]
```

```python
# neo4j_client.py
def write_memory(tx, agent_id, content, embedding, ts, tags):
    tx.run("""
        MERGE (a:Agent {id: $aid})
        CREATE (m:Memory {id: randomUUID(), content: $c, embedding: $e, ts: $t, tags: $tags})
        CREATE (a)-[:REMEMBERS]->(m)
        RETURN m.id AS id
    """, aid=agent_id, c=content, e=embedding, t=ts, tags=tags)

def recall_memory(tx, agent_id, query_vec, top_k):
    return tx.run("""
        MATCH (a:Agent {id: $aid})-[:REMEMBERS]->(m:Memory)
        WITH m, gds.similarity.cosine(m.embedding, $q) AS score
        ORDER BY score DESC LIMIT $k
        RETURN m.id AS id, m.content AS content, score, m.ts AS ts, m.tags AS tags
    """, aid=agent_id, q=query_vec, k=top_k)
```

```python
# main.py
@app.post("/memory/write")
async def write(req: WriteReq):
    vec = await embed(req.content)
    ts = req.ts or int(time.time()*1000)
    with driver.session() as s:
        rid = s.execute_write(write_memory, req.agent_id, req.content, vec, ts, req.tags)
    return {"id": rid, "ok": True}

@app.post("/memory/recall", response_model=list[MemoryItem])
async def recall(req: RecallReq):
    vec = await embed(req.query)
    with driver.session() as s:
        rows = s.execute_read(recall_memory, req.agent_id, vec, req.top_k)
    return [MemoryItem(**r) for r in rows]
```

## 验收信号
- 启动 service，curl 写入：
  ```
  curl -X POST http://127.0.0.1:8765/memory/write \
       -H "Content-Type: application/json" \
       -d '{"agent_id":"npc_1","content":"我在第三轮投了 yes"}'
  ```
  返回 `{"id":"...","ok":true}`
- 然后查询：
  ```
  curl -X POST http://127.0.0.1:8765/memory/recall \
       -d '{"agent_id":"npc_1","query":"我之前投票","top_k":3}'
  ```
  返回上一条记录，score > 0.5

## 不在范围
- `/memory/recent`（按时间排）— 留给 T24（by_tag 已能覆盖大部分场景）
- 重要性衰减、记忆压缩、清理

## 风险
- **已确认 ollama OpenAI-compat**：endpoint `http://localhost:11434/v1/embeddings`；request 格式 `{"model":"qwen3-embedding:8b","input":"text"}`；response `{"data":[{"embedding":[...]}]}`
- Neo4j Community 版的 `gds.similarity.cosine` 可能不可用（GDS 是企业版功能）。备选：用 Neo4j 5.x **native vector index**（推荐）—`CREATE VECTOR INDEX agent_mem_vec FOR (m:Memory) ON (m.embedding) OPTIONS ...`，然后用 `db.index.vector.queryNodes`
- 如果 native vector index 也不可用（4.x 版本），降级为 Python 端 numpy cosine（先全量取 agent 记忆然后排序）
- qwen3-embedding:8b 的向量维度需要在 Neo4j vector index 里指定（通常 4096）—— T08 实现时实测一次确认
