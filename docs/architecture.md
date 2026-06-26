# SmartNAS 整体架构与文件说明

## 1. 总体架构

SmartNAS 由三个运行层组成：

```text
Browser
  ├── Core HTTP : 文件、目录、认证、分享、静态页面
  └── Agent HTTP: 对话、摘要、文件问答、RAG

Core (C++ / Workflow / SQLite)
  ├── API handlers
  ├── 文件存储
  └── 元数据仓储

Agent (Python / FastAPI)
  ├── DeepSeek client
  ├── 文档转换与摘要任务
  ├── 文件问答与会话
  └── Embedding / FAISS
```

Core 是数据和权限的唯一事实来源。Agent 不直接读取 Core 数据库，而是携带浏览器传入的 JWT 调用 Core API。前端静态资源由 Core 提供，运行时通过 `/api/config` 获取 Agent 端口和上传参数。

## 2. 目录约定

- `include/smartnas/`：可被多个 C++ 模块引用的公开头文件。
- `src/`：C++ 实现；只服务于单个实现域的私有头文件与对应源码放在一起，例如 `src/api/RouterInternal.h`。
- `agent/`：可导入的 Python Agent 包。
- `scripts/`：兼容启动入口、依赖清单和运维脚本，不存放领域实现。
- `web/`：HTML、CSS、JavaScript 与第三方浏览器资源。
- `config/`：统一运行配置。
- `docs/`：架构和服务说明。
- `tests/`：自动化测试。
- `var/`：运行时数据库、文件、缓存和日志，不属于源码。
- `third_party/`：外部源码依赖。

## 3. Core 文件说明

### 根目录与构建

- `CMakeLists.txt`：声明 Core 全部源码、C++17、OpenSSL、SQLite、Workflow 和输出目录。
- `README.md`：项目功能、依赖和快速启动入口。
- `config/config.json`：Core、Agent、上传、RAG、缓存与审计的统一默认配置。

### 入口与配置

- `src/main.cpp`：加载统一配置、创建数据目录、初始化数据库并启动 Core HTTP 服务。
- `include/smartnas/config/AppConfig.h`：Core 配置只读接口。
- `src/config/AppConfig.cpp`：读取 `config.json`，解析端口、路径、JWT 和上传参数，并将相对路径解析为项目根目录下的绝对路径。

### HTTP API

- `include/smartnas/api/Router.h`：Core 路由 facade 和 handler 声明。
- `src/api/Router.cpp`：CORS、JWT 身份解析和 URI 分发。
- `src/api/RouterInternal.h`：API 层私有工具声明，不暴露给其他模块。
- `src/api/RouterInternal.cpp`：查询参数、JSON 校验、目录规范化、随机 token 和动态 JWT secret。
- `src/api/AuthHandlers.cpp`：注册与登录。
- `src/api/UploadHandlers.cpp`：普通上传、分片初始化、分片写入、合并与 hash 校验。
- `src/api/FileReadHandlers.cpp`：列表、下载、预览、Range 响应和 Agent 全量文件接口。
- `src/api/FileMutationHandlers.cpp`：删除、恢复、永久删除、重命名、移动和统计。
- `src/api/FolderHandlers.cpp`：文件夹创建、重命名、移动、删除和列表。
- `src/api/ShareHandlers.cpp`：分享链接创建和公共下载。
- `src/api/AgentMetadataHandlers.cpp`：供 Agent 使用的文件搜索、摘要和标签写回接口。
- `src/api/StaticHandlers.cpp`：首页、前端 CSS/JS、hash-wasm 和浏览器运行时配置。

### 文件存储

- `include/smartnas/core/FileManager.h`：文件与分片存储接口。
- `src/core/FileManager.cpp`：配置驱动的数据路径、文件读写、分片合并、合并期 SHA-256、删除和范围读取。
- `include/smartnas/core/FileMetadata.h`：文件、文件夹和分享元数据结构。
- `include/smartnas/core/User.h`：用户数据结构。
- `include/smartnas/utils/HashUtil.h`：hash 与 URL 解码接口。
- `src/utils/HashUtil.cpp`：OpenSSL SHA-256 和 URL 解码实现。

### SQLite

- `include/smartnas/db/DatabaseManager.h`：兼容 facade；调用方不依赖具体 repository 文件。
- `src/db/DatabaseManager.cpp`：连接生命周期、schema 创建和迁移。
- `src/db/UserRepository.cpp`：注册和密码认证 SQL。
- `src/db/FileWriteRepository.cpp`：文件元数据、摘要和标签写入。
- `src/db/FileLookupRepository.cpp`：单文件、归属、存在性和引用计数查询。
- `src/db/FileQueryRepository.cpp`：目录列表、全量列表、回收站、搜索和容量统计。
- `src/db/FileMutationRepository.cpp`：文件删除、恢复、重命名和移动。
- `src/db/FolderRepository.cpp`：文件夹写入、空目录删除和目录树事务移动。
- `src/db/ShareRepository.cpp`：分享 token 写入和查询。

## 4. Agent 文件说明

- `agent/__init__.py`：Python 包标记。
- `agent/config.py`：读取统一 JSON 配置，并提供环境变量覆盖。
- `agent/schemas.py`：FastAPI/Pydantic 请求模型。
- `agent/llm_client.py`：DeepSeek 非流式、SSE 流式请求和模型输出清理。
- `agent/sse.py`：Agent 对浏览器的 SSE 编码与防缓冲响应头。
- `agent/service.py`：FastAPI 应用、Core 客户端、文档转换、RAG、摘要任务、文件问答、会话和审计。
- `scripts/agent_service.py`：兼容旧启动命令的轻量包装器。
- `scripts/requirements-agent.txt`：Agent Python 依赖。
- `scripts/start_smartnas.sh`：读取统一配置、构建并同时管理 Core 与 Agent 生命周期。

Agent 更详细的接口和缓存说明见 [agent-service.md](agent-service.md)。

## 5. Web 文件说明

- `web/index.html`：页面语义结构和模态框，不包含业务实现。
- `web/css/app.css`：SmartNAS 页面样式与响应式布局。
- `web/js/core.js`：运行时配置、认证、全局状态和通用 UI。
- `web/js/files.js`：文件列表、排序、面包屑、图标和操作菜单渲染。
- `web/js/upload.js`：登录注册提交、SHA-256、分片和并发上传。
- `web/js/operations.js`：下载、预览、摘要任务和文件/文件夹变更。
- `web/js/file-qa.js`：Markdown 查看和单文件流式问答。
- `web/js/chat.js`：右侧 Agent 对话、停止生成和 SSE 消费。
- `web/js/bootstrap.js`：等待运行时配置后初始化页面。
- `web/vendor/hash-wasm-sha256.umd.min.js`：大文件增量 SHA-256 第三方实现。
- `web/vendor/hash-wasm-LICENSE`：第三方许可证。

## 6. 配置

统一配置文件为 `config/config.json`。参数分为：

- Core：监听地址、端口、数据库、数据目录、Web 目录、JWT secret；
- 上传：分片大小、并发数、浏览器原生 hash 内存阈值；
- Agent：监听地址、Core API、DeepSeek、生成采样参数；
- 文档任务：Markdown 长度、摘要分块、任务数据库和 worker 数；
- RAG：向量目录、embedding 模型、分块、召回数和最低分数；
- 审计：日志路径、轮转大小、备份数和内容策略。

Agent 配置优先级为：环境变量 > `config/config.json` > 代码默认值。API Key 推荐通过 `SMARTNAS_DEEPSEEK_API_KEY` 提供，不建议提交到配置文件。Core 使用 `SMARTNAS_CONFIG` 或启动参数选择配置文件。

## 7. 主要运行流程

### 上传

浏览器读取 `/api/config` → 计算 SHA-256 → 查询缺失分片 → 并发上传 → Core 合并时同步校验 → 写入 SQLite → 浏览器静默提交 Agent 摘要任务。

### 单文件问答

浏览器请求 Agent SSE → Agent 通过 Core 下载文件或命中 Markdown 缓存 → 小文件全文问答，大文件文件内 RAG → 增量返回 → 保存“用户 + 文件 hash”上下文。

### 目录移动

浏览器提交目录目标 → Core 校验目标关系 → SQLite 事务更新文件夹树和内部文件目录 → 任一步失败则整体回滚。

## 8. 测试与构建

- `CMakeLists.txt`：声明所有 Core 分域源码和链接依赖。
- `tests/test_agent_modules.py`：Agent schema、SSE 和流式模型客户端基础单测。

文档文件：

- `docs/architecture.md`：当前整体架构、目录和逐文件职责。
- `docs/project-flow-tree.md`：端到端流程树，以及每个文件的作用、输入和输出。
- `docs/agent-service.md`：Agent 配置、API、缓存和后台任务。
- `docs/agent-decoupling.md`：Core 与 Agent 的服务边界。
- `docs/code-architecture.md`：结构审计、遗留风险与后续演进路线。

```bash
cmake -S . -B build
cmake --build build
python3 -m unittest tests/test_agent_modules.py
./scripts/start_smartnas.sh
```
