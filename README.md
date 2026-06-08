# SmartNAS

SmartNAS 是一个面向个人 NAS / 家庭服务器的智能网盘实验项目。核心服务使用 C++ 实现文件管理和 HTTP API，Agent 服务使用 Python 接入本地大模型与 MarkItDown，为网盘文件提供摘要、Markdown 转换和文件问答能力。

项目当前目标不是做一个复杂的企业网盘，而是先把“个人文件存储 + 局域网访问 + 本地 AI 理解文件内容”跑通，并逐步补齐安全、检索和自动化能力。

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
- 文件名、摘要的简单搜索。
- SQLite 元数据存储，文件本体按内容 hash 存放在 `var/data`。
- 单页 Web UI，入口由核心服务直接提供。

### Agent / AI 能力

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
- 单文件同步摘要。
- 单文件异步摘要任务。
- 批量为缺失摘要的文件创建任务。
- 查看摘要任务状态。
- 查看文件转换后的 Markdown。
- 基于文件内容进行问答。
- Agent 聊天，可调用核心搜索接口查询网盘文件。
- 支持 GGUF 模型走 `llama.cpp`，也支持 HuggingFace Transformers 目录模型。

## 目录结构

```text
SmartNAS/
├── include/                 # C++ 头文件
├── src/                     # C++ 核心服务
│   ├── api/                 # HTTP 路由
│   ├── core/                # 文件存储逻辑
│   ├── db/                  # SQLite 元数据
│   └── utils/               # Hash 等工具
├── scripts/
│   ├── agent_service.py     # Python Agent 服务
│   ├── requirements-agent.txt
│   └── start_smartnas.sh    # 一键启动核心服务 + Agent
├── web/                     # 前端单页界面
├── var/
│   ├── data/                # 文件内容与上传分片
│   ├── db/                  # SQLite 数据库
│   └── cache/markdown       # Markdown 转换缓存
└── models/                  # 本地大模型目录
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

Agent 依赖包括 FastAPI、Uvicorn、MarkItDown、llama-cpp-python、Transformers、PyTorch、Pillow 等。

## 启动

推荐使用一键脚本：

```bash
./scripts/start_smartnas.sh
```

默认服务地址：

- Web / Core API: `http://127.0.0.1:8080`
- Agent API: `http://127.0.0.1:8081`

默认模型路径：

```bash
models/llm/qwen2.5-7b-instruct-q4_k_m.gguf
```

可以通过环境变量覆盖：

```bash
export SMARTNAS_MODEL_PATH=/path/to/model.gguf
export SMARTNAS_CORE_API=http://127.0.0.1:8080
export SMARTNAS_MAX_NEW_TOKENS=512
export SMARTNAS_MAX_MARKDOWN_CHARS=12000
export SMARTNAS_SUMMARY_CHUNK_CHARS=7000
export SMARTNAS_LLAMA_CONTEXT_SIZE=8192
export SMARTNAS_CACHE_DIR=var/cache/markdown
./scripts/start_smartnas.sh
```

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
- `POST /api/v1/files/summary`：更新文件摘要。

### Agent 服务

- `GET /api/agent/health`：Agent 健康检查和模型状态。
- `POST /api/agent/summarize`：同步生成摘要。
- `POST /api/agent/summarize/start`：异步生成摘要。
- `GET /api/agent/summarize/status/{task_id}`：查询任务状态。
- `GET /api/agent/summarize/tasks`：查询最近任务。
- `POST /api/agent/summarize/missing`：为缺失摘要的文件创建任务。
- `GET /api/agent/markdown/{file_hash}`：查看转换后的 Markdown。
- `POST /api/agent/file_qa`：针对单个文件问答。
- `POST /api/agent/chat`：Agent 聊天。
- `POST /api/agent/clear_history`：清除会话历史。

## 已知限制

- 当前更适合局域网和个人实验环境，尚未按公网服务标准加固。
- JWT secret 仍需要改为外部配置。
- 密码存储需要升级为 bcrypt 或 Argon2。
- 下载、Agent 转换和前端 hash 计算仍有大文件内存压力。
- 分片上传需要更严格的 hash、index、大小和用户会话校验。
- 回收站、分享、Agent 任务还需要进一步完善权限边界。
- 批量摘要当前主要处理列表接口返回的文件，深层目录自动遍历仍需增强。
- 图片摘要目前主要依赖元信息或 fallback，真正的 OCR / 视觉模型理解仍属于后续功能。

## 预期功能

### 近期

- 修复高优先级安全问题：路径校验、上传归属、删除文件访问、JWT/密码安全。
- 分享管理：查看、撤销、设置密码、下载次数限制。
- 大文件流式下载、流式 hash、Agent 流式转换。
- 目录递归操作：递归移动、删除、恢复、统计。
- 摘要任务持久化和按用户隔离。
- UI 继续简化，减少重复按钮，提升移动端体验。

### 中期

- 全文索引和向量检索。
- RAG 文件问答，支持多文件、多目录上下文。
- 图片 OCR、音视频转写和多模态摘要。
- 文件标签、收藏、最近访问。
- 配额、限速、审计日志。
- 管理员页面和系统状态页面。

### 长期

- 更完整的本地智能数据管家能力。
- 自动分类、自动命名、重复文件清理。
- 私有化模型管理和多模型路由。
- 更成熟的插件/任务系统。

## 技术选型

- C++17：核心服务、文件 I/O、路由和元数据操作。
- Sogou Workflow：HTTP 服务和异步网络框架。
- OpenSSL：SHA-256、JWT 签名相关加密能力。
- SQLite：轻量级本地元数据存储。
- jwt-cpp：JWT 编解码。
- FastAPI / Uvicorn：Agent HTTP 服务。
- MarkItDown：Office、PDF、文本类文件到 Markdown 的转换。
- llama.cpp / Transformers：本地大模型推理。

## 项目定位

SmartNAS 的最终方向是一个完全由自己掌控的智能网盘：

- 文件保存在本地。
- 摘要和问答尽量在本地模型完成。
- 网盘能力和 AI 能力解耦，核心服务即使没有模型也能独立运行。
- 优先保证个人 NAS 场景下的可用性，再逐步补齐安全与规模化能力。
