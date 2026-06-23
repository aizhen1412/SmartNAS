# SmartNAS Agent 功能说明

## 1. 服务定位

SmartNAS Agent 是独立运行的 Python/FastAPI 服务，默认监听 `8081`。它负责：

- 调用 DeepSeek OpenAI-compatible API；
- 普通对话与 SSE 流式对话；
- 文件转 Markdown、摘要和标签生成；
- 单文件全文问答和文件内 RAG 问答；
- Markdown 分块、向量生成、FAISS 检索和索引持久化；
- 摘要后台任务、取消、重试、去重与状态持久化；
- Agent 操作审计。

NAS Core 仍负责 JWT 鉴权、文件存储、下载、元数据和目录操作。Agent 不直接读取 Core 的 SQLite，而是携带用户 JWT 调用 Core HTTP API。

## 2. 启动与依赖

安装依赖：

```bash
python3 -m venv .venv
source .venv/bin/activate
pip install -r scripts/requirements-agent.txt
```

单独启动 Agent：

```bash
export SMARTNAS_DEEPSEEK_API_KEY=sk-...
python3 scripts/agent_service.py
```

也可以同时启动 Core 和 Agent：

```bash
./scripts/start_smartnas.sh
```

Agent 默认通过 `http://127.0.0.1:8080` 访问 Core。

## 3. 配置项

默认参数统一存放在 `config/config.json`。下列环境变量用于部署时覆盖对应 JSON 值，优先级为：环境变量 > JSON 配置 > 代码默认值。

| 环境变量 | 默认值 | 用途 |
| --- | --- | --- |
| `SMARTNAS_CORE_API` | `http://127.0.0.1:8080` | Core API 地址 |
| `SMARTNAS_DEEPSEEK_API_KEY` | 空 | DeepSeek API Key，也兼容 `DEEPSEEK_API_KEY` |
| `SMARTNAS_DEEPSEEK_API_BASE` | `https://api.deepseek.com` | 模型 API 地址 |
| `SMARTNAS_DEEPSEEK_MODEL` | `deepseek-chat` | 模型名称 |
| `SMARTNAS_DEEPSEEK_TIMEOUT` | `60` | 模型请求超时秒数 |
| `SMARTNAS_GENERATION_TEMPERATURE` | `0.7` | 生成温度 |
| `SMARTNAS_GENERATION_TOP_P` | `0.8` | Top-P |
| `SMARTNAS_MAX_MARKDOWN_CHARS` | `12000` | 单次摘要输入长度 |
| `SMARTNAS_SUMMARY_CHUNK_CHARS` | `7000` | 长文摘要分块长度 |
| `SMARTNAS_FILE_QA_DIRECT_CHARS` | `30000` | 全文问答与 RAG 问答切换阈值 |
| `SMARTNAS_CACHE_DIR` | `var/cache/markdown` | Markdown 缓存目录 |
| `SMARTNAS_VECTOR_DIR` | `var/cache/vector` | 向量数据和 FAISS 索引目录 |
| `SMARTNAS_EMBEDDING_MODEL` | `BAAI/bge-small-zh-v1.5` | Embedding 模型 |
| `SMARTNAS_RAG_CHUNK_CHARS` | `900` | RAG 文本块长度 |
| `SMARTNAS_RAG_CHUNK_OVERLAP` | `120` | RAG 分块重叠长度 |
| `SMARTNAS_RAG_TOP_K` | `6` | 默认召回数量 |
| `SMARTNAS_RAG_MIN_SCORE` | `0.28` | 最低相似度 |
| `SMARTNAS_TASK_DB` | `var/cache/agent_tasks.db` | 摘要任务数据库 |
| `SMARTNAS_AUDIT_LOG` | `var/log/agent_audit.jsonl` | 审计日志路径 |
| `SMARTNAS_AUDIT_LOG_MAX_BYTES` | `20971520` | 单个审计日志最大字节数 |
| `SMARTNAS_AUDIT_LOG_BACKUPS` | `10` | 审计日志保留数量 |
| `SMARTNAS_AUDIT_LOG_FULL_CONTENT` | `1` | 是否记录完整内容 |
| `SMARTNAS_AGENT_HOST` | `0.0.0.0` | Agent 监听地址 |
| `SMARTNAS_AGENT_PORT` | `8081` | Agent 监听端口 |
| `SMARTNAS_SUMMARY_WORKER_COUNT` | `1` | 摘要后台 worker 数 |

## 4. 鉴权与用户隔离

除健康检查外，接口都使用 Core 签发的 JWT：

```http
Authorization: Bearer <token>
```

- Agent 通过 `/api/v1/me` 向 Core 验证身份；
- 普通聊天上下文按用户隔离；
- 单文件问答上下文按“用户 + 文件 hash”隔离，并保留最近三轮问答；
- 向量缓存按用户命名空间隔离；
- 摘要任务只能由任务所属用户查看、取消或重试。

聊天和单文件问答上下文目前保存在 Agent 内存中，服务重启后会清空。摘要任务与向量索引会持久化。

## 5. HTTP API

### 健康检查

`GET /api/agent/health`

返回模型配置、依赖状态、缓存路径、RAG 参数和支持格式。

### 普通聊天

- `POST /api/agent/chat`：非流式聊天，请求体为 `{"prompt":"..."}`。
- `POST /api/agent/chat/stream`：SSE 流式聊天，请求体相同。
- `POST /api/agent/clear_history`：清空当前用户普通聊天上下文。

流式接口使用以下事件：

```text
data: {"type":"delta","content":"增量文本"}

data: {"type":"done"}

data: {"type":"error","message":"错误信息"}
```

上游 SSE 使用小块读取，并关闭代理转换和缓冲。模型输出中的 Markdown 星号会在 Agent 输出层统一移除。

### 文件摘要与标签

- `POST /api/agent/summarize`：同步摘要，请求体为 `{"hash":"...","force":false}`。
- `POST /api/agent/summarize/start`：创建后台摘要任务。
- `GET /api/agent/summarize/status/{task_id}`：查询任务。
- `GET /api/agent/summarize/tasks`：列出当前用户任务。
- `POST /api/agent/summarize/cancel/{task_id}`：取消任务。
- `POST /api/agent/summarize/retry/{task_id}`：重试失败或取消的任务。
- `POST /api/agent/summarize/missing`：为缺少摘要的支持文件批量排队，保留作管理 API，页面不再提供手动入口。

Web UI 在文件上传、恢复、重命名和移动成功后，会静默调用 `summarize/start`。任务在后台完成 Markdown 转换、摘要、标签和向量索引，不阻塞文件操作。

### Markdown 与单文件问答

- `GET /api/agent/markdown/{file_hash}`：返回转换后的 Markdown。
- `POST /api/agent/file_qa`：非流式单文件问答，请求体为 `{"hash":"...","question":"..."}`。
- `POST /api/agent/file_qa/stream`：SSE 流式单文件问答，请求体相同。

文件长度不超过 `SMARTNAS_FILE_QA_DIRECT_CHARS` 时，模型接收全文；超过阈值时只召回该文件相关片段。后续问题会携带该文件最近三轮问答，大文件检索还会结合上一个问题处理“它、这个结论”等指代。

`file_qa/stream` 的事件格式与普通流式聊天一致，`done` 事件额外带有 `mode`，值为 `full` 或 `rag`。

### RAG 查询

`POST /api/agent/rag/query`

请求体：

```json
{"query":"检索问题","top_k":6}
```

返回相似片段、文件名、hash、块序号、相似度和基于片段生成的回答。

## 6. 文档处理与缓存

- MarkItDown 用于 PDF、Office、文本、网页、EPUB、ZIP、Notebook 等格式转换；
- 图片使用尺寸、EXIF、嵌入元数据和 SVG 文本生成 fallback Markdown；
- 音视频在无法提取转录时使用媒体元数据；
- Markdown 按文件 hash 缓存在 `var/cache/markdown`；
- RAG 块、embedding 和 FAISS 索引缓存在 `var/cache/vector`；
- 摘要成功后会同时写回 Core 摘要与标签，并更新该用户向量索引；
- 查询前会对照 Core 当前文件列表，移除已删除文件的块并同步重命名后的文件名。

## 7. 后台任务与审计

摘要任务使用单工作线程执行，避免多个文档转换和本地 embedding 同时挤占内存。相同用户、相同文件的待处理任务会去重。任务状态包括：

- `pending`
- `running`
- `cancel_requested`
- `cancelled`
- `success`
- `failed`

任务记录保存在 `var/cache/agent_tasks.db`。Agent 重启时，未完成任务会标记为失败，可通过重试接口重新排队。

审计日志采用轮转 JSONL，记录用户、操作、状态、耗时、线程及结果或错误。默认路径为 `var/log/agent_audit.jsonl`，文件权限会尝试设置为 `0600`。

## 8. 主要代码位置

- Agent 服务与全部 API：`agent/service.py`
- Agent 配置、请求模型、模型客户端和 SSE：`agent/`
- 兼容启动入口：`scripts/agent_service.py`
- Python 依赖：`scripts/requirements-agent.txt`
- Web 调用与流式渲染：`web/index.html`
- Core/Agent 边界：`docs/agent-decoupling.md`
