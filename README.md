# SmartNAS

SmartNAS 是一个面向个人 NAS / 家庭服务器的智能网盘实验项目。核心服务使用 C++ 实现文件管理和 HTTP API，Agent 服务使用 Python 接入 DeepSeek API 与 MarkItDown，为网盘文件提供摘要、Markdown 转换和文件问答能力。

项目当前目标不是做一个复杂的企业网盘，而是先把“个人文件存储 + 局域网访问 + AI 理解文件内容”跑通，并逐步补齐安全、检索和自动化能力。

## 当前功能

### 网盘核心

- 用户注册、登录和 JWT 鉴权。
- 文件上传、下载、预览。
- 大文件分片上传、断点续传和 SHA-256 合并校验。
- 文件列表、目录浏览、文件夹创建。
- 文件重命名、移动。
- 回收站：删除、恢复、彻底删除。
- 文件分享链接，支持过期时间。
- 文件统计信息。
- 文件名、摘要、AI 标签的简单搜索和分类。
- SQLite 元数据存储，文件本体按内容 hash 存放在 `var/data`。
- 单页 Web UI，入口由核心服务直接提供。

### Agent / AI 能力

完整配置、接口、缓存与后台任务说明见 [Agent 功能说明](docs/agent-service.md)。
代码边界与目录职责见 [整体架构说明](docs/architecture.md)。
整体架构、目录约定和逐文件职责见 [整体架构说明](docs/architecture.md)。

- 使用 MarkItDown 将文件转换为 Markdown。
- 支持摘要的主要格式：
  - `pdf`
  - `docx`
  - `pptx`
  - `xlsx`
  - `txt`
  - `csv`
  - `json`
  - `html`
- 图片和部分媒体文件支持基础 Markdown/元信息摘要 fallback。
- 单文件同步生成摘要和分类标签。
- 单文件异步摘要和标签任务。
- 批量为缺失摘要或标签的文件创建任务。
- 查看摘要和标签任务状态。
- 查看文件转换后的 Markdown。
- 基于文件内容进行问答。
- 大文件问答自动切换为文件内 RAG，小文件直接使用全文。
- 用户级持久化 FAISS 索引，并在查询时同步删除和重命名状态。
- 摘要任务持久化、按用户隔离，并支持去重、取消和重试。
- 用户操作和完整 Agent 结果写入轮转 JSONL 审计日志。
- Agent 流式聊天，可停止生成，并通过 function calling 搜索文件。
- 通过 DeepSeek API 生成聊天、摘要和问答内容。

## 目录结构

```text
SmartNAS/
├── agent/                   # Python Agent 包
├── config/                  # Core 与 Agent 统一配置
├── include/smartnas/        # C++ 公开头文件
├── src/                     # C++ 核心服务
│   ├── api/                 # HTTP 路由
│   ├── config/              # Core 配置加载
│   ├── core/                # 文件存储逻辑
│   ├── db/                  # SQLite 元数据
│   └── utils/               # Hash 等工具
├── scripts/
│   ├── agent_service.py     # 兼容启动入口
│   ├── requirements-agent.txt
│   └── start_smartnas.sh    # 一键启动核心服务 + Agent
├── web/                     # HTML、CSS、JS 与浏览器依赖
├── tests/                   # 自动化测试
├── docs/                    # 架构与接口文档
├── var/
│   ├── data/                # 文件内容与上传分片
│   ├── db/                  # SQLite 数据库
│   └── cache/markdown       # Markdown 转换缓存
└── third_party/             # C++ 第三方依赖
```

## 运行环境

### C++ 核心服务

需要：

- Linux
- CMake
- C++17 编译器
- OpenSSL
- SQLite3
- Sogou Workflow

### Agent 服务

建议使用 Python 虚拟环境：

```bash
python3 -m venv .venv
source .venv/bin/activate
pip install -r scripts/requirements-agent.txt
```

Agent 依赖包括 FastAPI、Uvicorn、requests、MarkItDown、Pillow、sentence-transformers、FAISS 等。

## 启动

推荐使用一键脚本：

```bash
./scripts/start_smartnas.sh
```

默认服务地址：

- Web / Core API: `http://127.0.0.1:8080`
- Agent API: `http://127.0.0.1:8081`

可以通过环境变量覆盖：

```bash
export SMARTNAS_DEEPSEEK_API_KEY=sk-...
export SMARTNAS_DEEPSEEK_API_BASE=https://api.deepseek.com
export SMARTNAS_DEEPSEEK_MODEL=deepseek-chat
export SMARTNAS_CORE_API=http://127.0.0.1:8080
export SMARTNAS_GENERATION_TEMPERATURE=0.7
export SMARTNAS_GENERATION_TOP_P=0.8
export SMARTNAS_MAX_MARKDOWN_CHARS=12000
export SMARTNAS_SUMMARY_CHUNK_CHARS=7000
export SMARTNAS_CACHE_DIR=var/cache/markdown
export SMARTNAS_VECTOR_DIR=var/cache/vector
export SMARTNAS_EMBEDDING_MODEL=BAAI/bge-small-zh-v1.5
export SMARTNAS_RAG_CHUNK_CHARS=900
export SMARTNAS_RAG_CHUNK_OVERLAP=120
export SMARTNAS_RAG_TOP_K=6
export SMARTNAS_RAG_MIN_SCORE=0.28
export SMARTNAS_FILE_QA_DIRECT_CHARS=30000
export SMARTNAS_TASK_DB=var/cache/agent_tasks.db
export SMARTNAS_AUDIT_LOG=var/log/agent_audit.jsonl
export SMARTNAS_AUDIT_LOG_MAX_BYTES=20971520
export SMARTNAS_AUDIT_LOG_BACKUPS=10
export SMARTNAS_AUDIT_LOG_FULL_CONTENT=0
export SMARTNAS_JWT_SECRET=change-me-to-a-long-random-secret
./scripts/start_smartnas.sh
```

Agent 通过 DeepSeek 的 OpenAI-compatible `/chat/completions` 接口生成内容。启动前需要设置 `SMARTNAS_DEEPSEEK_API_KEY`，也可以使用 `DEEPSEEK_API_KEY`。

也可以手动构建核心服务：

```bash
cmake -S . -B build
cmake --build build
./build/bin/SmartNAS
```

然后单独启动 Agent：

```bash
python3 scripts/agent_service.py
```

## 常用接口

### 核心服务

- `GET /`：Web UI。
- `GET /ping`：健康检查。
- `POST /register`：注册。
- `POST /login`：登录。
- `POST /upload`：普通上传。
- `GET /api/upload/init`：分片上传初始化。
- `POST /api/upload/chunk`：上传分片。
- `POST /api/upload/merge`：合并分片。
- `GET /api/list`：文件列表。
- `GET /download`：下载文件。
- `GET /api/preview`：预览文件。
- `POST /api/delete`：移入回收站。
- `POST /api/restore`：恢复文件。
- `POST /api/purge`：彻底删除。
- `POST /api/rename`：重命名。
- `POST /api/move`：移动。
- `GET /api/folders`：列出目录。
- `POST /api/folders`：创建目录。
- `GET /api/stats`：统计信息。
- `POST /api/share/create`：创建分享链接。
- `GET /share/{token}`：通过分享链接下载。
- `GET /api/v1/files/search`：搜索文件。
- `GET /api/v1/files/all`：列出当前用户所有未删除文件，供 Agent 递归同步。
- `GET /api/v1/me`：返回当前 JWT 对应的用户身份。
- `POST /api/v1/files/summary`：更新文件摘要。
- `POST /api/v1/files/tags`：更新文件分类标签。

### Agent 服务

- `GET /api/agent/health`：Agent 健康检查和 DeepSeek/RAG 状态。
- `POST /api/agent/summarize`：同步生成摘要。
- `POST /api/agent/summarize/start`：异步生成摘要。
- `GET /api/agent/summarize/status/{task_id}`：查询任务状态。
- `GET /api/agent/summarize/tasks`：查询最近任务。
- `POST /api/agent/summarize/cancel/{task_id}`：取消任务。
- `POST /api/agent/summarize/retry/{task_id}`：重试失败或已取消任务。
- `POST /api/agent/summarize/missing`：为缺失摘要的文件创建任务。
- `GET /api/agent/markdown/{file_hash}`：查看转换后的 Markdown。
- `POST /api/agent/file_qa`：针对单个文件问答。
- `POST /api/agent/rag/query`：基于已索引原文片段进行 RAG 检索和回答。
- `POST /api/agent/chat`：Agent 聊天。
- `POST /api/agent/chat/stream`：SSE 流式 Agent 聊天。
- `POST /api/agent/clear_history`：清除会话历史。

## 已知限制

- 当前更适合局域网和个人实验环境，尚未按公网服务标准加固。
- JWT secret 优先从 `SMARTNAS_JWT_SECRET` 读取；未配置时会生成进程临时 secret，重启后旧 token 失效。
- 密码存储已从裸 SHA-256 升级为带 salt 的 PBKDF2-HMAC-SHA256；旧账号会在成功登录后自动迁移。
- 下载、Agent 转换和前端 hash 计算仍有大文件内存压力。
- 分片上传已有 hash、index、大小和用户会话校验；后续仍可补持久化上传会话与限速。
- 回收站和分享仍需要进一步完善权限边界。
- Agent 聊天会优先使用 RAG 原文片段检索；如果向量依赖、embedding 模型或索引不可用，则回退到摘要关键词搜索。
- 图片摘要目前主要依赖元信息或 fallback，真正的 OCR / 视觉模型理解仍属于后续功能。
- 默认审计日志会截断长内容，并尝试使用 `0600` 权限；如果显式设置 `SMARTNAS_AUDIT_LOG_FULL_CONTENT=1`，会保存完整问答、Markdown 提取结果和模型输出。

## 预期功能

### 近期

- 修复高优先级安全问题：路径校验、上传归属、删除文件访问、JWT/密码安全。
- 分享管理：查看、撤销、设置密码、下载次数限制。
- 大文件流式下载、流式 hash、Agent 流式转换。
- 目录递归操作：递归移动、删除、恢复、统计。
- UI 继续简化，减少重复按钮，提升移动端体验。

### 中期

- 图片 OCR、音视频转写和多模态摘要。
- 文件收藏和最近访问。
- 配额、限速、审计日志。
- 管理员页面和系统状态页面。

### 长期

- 更完整的本地智能数据管家能力。
- 自动分类、自动命名、重复文件清理。
- 多模型 API 路由和更细的任务策略。
- 更成熟的插件/任务系统。

## 技术选型

- C++17：核心服务、文件 I/O、路由和元数据操作。
- Sogou Workflow：HTTP 服务和异步网络框架。
- OpenSSL：SHA-256、JWT 签名相关加密能力。
- SQLite：轻量级本地元数据存储。
- jwt-cpp：JWT 编解码。
- FastAPI / Uvicorn：Agent HTTP 服务。
- MarkItDown：Office、PDF、文本类文件到 Markdown 的转换。
- DeepSeek API：聊天、摘要和文件问答生成。

## 项目定位

SmartNAS 的最终方向是一个自己掌控数据和服务边界的智能网盘：

- 文件保存在本地。
- 摘要和问答由独立 Agent 服务完成，当前接入 DeepSeek API。
- 网盘能力和 AI 能力解耦，核心服务即使没有 Agent 也能独立运行。
- 优先保证个人 NAS 场景下的可用性，再逐步补齐安全与规模化能力。
