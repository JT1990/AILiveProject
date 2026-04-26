# T08 — Memory Service 端点（event + peer_summary + scene_log + recall + by_tag debug）

## 目标
在 Memory Service 上实现 5 个端点：`event`（fire-and-forget 事件归档）+ `peer_summary`（跨场 per-peer 认知）+ `scene_log`（全 buffer JSONL replay）+ `recall`（仅 Provider tool_call 消费）+ `by_tag`（debug only）。对接 Neo4j 存储 + qwen3-embedding 向量化（仅 recall 用）。

## 前置
T04（health 端点 + Neo4j/embedding 探测就位）

## DoD
- [ ] `POST /memory/event`：
  - body: `{scene_id, round, phase, agent_id, action, params_digest, public_text?, target_agent_id?, result_summary, tags, ts?}`
  - 流程：Neo4j MERGE Agent → CREATE EventNode 关联（**不**调 embedding，纯结构化写入）
  - 返回 `{id, ok}` —— UE 端 fire-and-forget 不等响应
- [ ] `POST /memory/peer_summary`：
  - body: `{agent_id, peer_id, scene_id, summary_text, ts?}`
  - 流程：MERGE (Agent)-[:KNOWS]->(Peer)；CREATE PeerSummaryNode（按 ts 累积，最新的优先）
  - 返回 `{id, ok}`
- [ ] `GET /memory/peer_summary?agent_id=X&peer_ids=A,B,C`：
  - 返回每个 peer 的最新 summary：`[{peer_id, summary_text, scene_id, ts}]`
  - 用于新场启动时 BuildLayer0bFrame 注入
- [ ] `POST /memory/scene_log`：
  - body: `{scene_id, agent_id, messages: [FMindMessage JSONL]}`
  - 流程：写入文件 `<MemoryServicePath>/scene_logs/{scene_id}_{agent_id}.jsonl`（debug + replay 用，不进 Neo4j）
  - 返回 `{path, lines_written}`
- [ ] `POST /memory/recall`：
  - body: `{agent_id, query, top_k}`
  - 流程：query → embedding → Neo4j 内 cosine 相似度按 agent_id 过滤 → 返回 top_k
  - 返回 `[{id, content, score, ts, tags}]`
  - **5s timeout**（Provider tool_call 同步等待，不能拖死）
- [ ] `POST /memory/by_tag`（**debug only**）：
  - 文档明确标注 "**不参与决策路径**，仅 debug HUD（T25 联盟可视化）用"
  - body: `{agent_id, tag_key, tag_value, top_n: int=5}`
  - Cypher 按 tags map 过滤 + ts DESC LIMIT
- [ ] Neo4j schema：
  - `(:Agent {id})`
  - `(:Event {id, scene_id, round, phase, action, params_digest, public_text?, target_agent_id?, result_summary, tags, ts})` —— 无 embedding 字段
  - `(:PeerSummary {id, peer_id, scene_id, summary_text, ts})`
  - `(:Memory {id, content, embedding, ts, tags})` —— recall 用，embedding 字段存在
  - 关系 `(Agent)-[:LOGS]->(Event)`、`(Agent)-[:KNOWS_VIEW]->(PeerSummary)`、`(Agent)-[:REMEMBERS]->(Memory)`
- [ ] vector 检索 MVP：Python 端 cosine fallback（按 agent_id 拉全量 → numpy 排序 top_k）；如 Neo4j 5.x native vector index 可用则升级

## 关键文件
- 修改 `Tools/MemoryService/main.py`
- 新增 `Tools/MemoryService/neo4j_client.py`（如 T04 没建则新增；已有则扩 5 个端点对应方法）
- 新增 `Tools/MemoryService/embedding_client.py`（仅 recall 用）
- 修改 `Tools/MemoryService/models.py`（新增 EventReq / PeerSummaryWriteReq / PeerSummaryFetchResp / SceneLogReq / RecallReq / ByTagReq）

## 验收信号
1. 启动 service，curl 测试 5 个端点：
   - POST /memory/event 写入 → 返回 `{id, ok:true}`
   - POST + GET /memory/peer_summary 写入读取闭环
   - POST /memory/scene_log → 文件落盘
   - POST /memory/recall 写入 + 查询，score > 0.5
   - POST /memory/by_tag 按 tag 过滤返回
2. T08 验收时实测一次 embedding 长度（通常 4096），写到 `Tasks/T00_PREFLIGHT_RESULT.md` 的 embedding dim 字段
3. event 写入 1000 条 → P95 写入延迟 < 50ms（fire-and-forget 友好）

## 不在范围
- 重要性衰减、记忆压缩、清理（peer_summary 累积，不删除旧版本）
- 跨 agent 记忆共享（每 agent 独立视角）

## 风险
- ollama OpenAI-compat 端点 `http://localhost:11434/v1/embeddings`，请求格式 `{"model":"qwen3-embedding:8b","input":"text"}`
- Neo4j Community 版无 GDS；用 native vector index 或 Python cosine fallback
- scene_log JSONL 文件累积，需要文档化清理策略（M5 之后再加）
