# SmartNAS 项目流程树与文件输入输出

## 1. 总体流程树

```text
用户 / 浏览器
└── GET http://<host>:8080/
    └── SmartNAS Core（C++ / Workflow）
        ├── 读取 config/config.json
        ├── 提供 web/index.html、CSS、JS 和 hash-wasm
        ├── 认证：注册 / 登录 / JWT 校验
        ├── 文件：上传 / 分片 / 合并 / 下载 / 预览
        ├── 元数据：列表 / 搜索 / 摘要 / 标签 / 目录 / 分享
        ├── 读写 var/db/smartnas.db
        └── 读写 var/data/
    └── 前端按 /api/config 得到 Agent 端口
        └── SmartNAS Agent（Python / FastAPI）
            ├── 携带浏览器 JWT 回调 Core API
            ├── 下载用户文件并转换 Markdown
            ├── 调用 DeepSeek /chat/completions
            ├── 生成摘要、标签、对话和文件问答
            ├── 写回 Core 的 summary / tags
            ├── 读写 var/cache/markdown/
            ├── 读写 var/cache/vector/
            ├── 读写 var/cache/agent_tasks.db
            └── 写入 var/log/agent_audit.jsonl
```

核心边界：Core 是用户、文件和元数据的唯一事实来源；Agent 不直读 Core SQLite，只通过带 JWT 的 HTTP API 访问 Core。

## 2. 启动与运行流程

```text
./scripts/start_smartnas.sh
├── 定位项目根目录
├── SMARTNAS_CONFIG -> config/config.json
├── 解析端口、数据路径和 DeepSeek 配置
├── 创建 var/data 和 var/db
├── cmake -S . -B build -> cmake --build build
│   └── build/bin/SmartNAS
├── 启动 Core
│   ├── src/main.cpp
│   ├── AppConfig::load
│   ├── DatabaseManager::init
│   └── WFHttpServer(Router::process) :8080
├── 启动 Agent
│   └── scripts/agent_service.py -> uvicorn(agent.service:app) :8081
└── trap EXIT/INT/TERM -> 同时停止 Core 和 Agent
```

## 3. 主要业务流程

### 3.1 注册与登录

```text
web/js/core.js
├── POST /register + User/Password headers
│   └── AuthHandlers -> UserRepository -> users 表
└── POST /login + User/Password headers
    └── AuthHandlers -> UserRepository 验证密码
        └── HS256 JWT {issuer: smartnas, user: <username>}
            └── localStorage.token -> 后续 Authorization: Bearer <token>
```

输入：用户名、密码。输出：注册状态，或登录 JWT JSON。

### 3.2 分片上传

```text
浏览器 File
└── upload.js 计算 SHA-256
    ├── 小/中文件：Web Crypto
    └── 大文件：hash-wasm，失败时用内置 JS SHA-256
        └── GET /api/upload/init
            ├── 文件已存在 -> {status:"exists"}
            └── 未完成 -> {missing:[分片序号...]}
                └── POST /api/upload/chunk（并发）
                    └── var/data/<hash>.part<N>
                        └── POST /api/upload/merge
                            ├── 合并时校验 SHA-256 和字节数
                            ├── var/data/<hash>.bin
                            ├── files 表写入用户元数据
                            └── 静默调用 Agent summarize/start
```

输入：文件字节、原文件名、目录、JWT。输出：缺失分片列表、分片状态、合并结果和文件元数据。

### 3.3 列表、预览、下载和变更

```text
files.js / operations.js
├── GET /api/list?dir=...&deleted=0|1 -> 文件+子目录 JSON
├── GET /api/preview?hash=... -> MIME + Range 字节流
├── GET /download?hash=... -> attachment + Range 字节流
├── POST /api/delete|restore|purge -> files 表 / var/data
├── POST /api/rename|move -> files 表
├── GET|POST /api/folders -> folders 表
├── POST /api/folders/rename|move|delete -> folders + files 事务变更
└── POST /api/share/create -> shares 表 -> /share/download?token=...
```

输入：JWT、hash、目录路径、新名称或分享时长。输出：JSON 列表/状态、文件字节流或分享 URL。

### 3.4 摘要、标签与 RAG

```text
operations.js -> POST Agent /api/agent/summarize/start
└── service.py 持久化 pending 任务
    └── ThreadPoolExecutor worker
        ├── GET Core /download?hash=... + JWT
        ├── MarkItDown / 图片、文本、媒体 fallback -> Markdown
        ├── var/cache/markdown/<hash>.v2.md
        ├── DeepSeek -> 摘要 -> POST Core /api/v1/files/summary
        ├── DeepSeek -> 标签 -> POST Core /api/v1/files/tags
        └── Markdown 分块 -> embedding -> FAISS / numpy 相似度
            └── var/cache/vector/<user-namespace>/
```

输入：JWT、文件 hash、force 标志。输出：任务状态、Markdown、摘要、JSON 标签和向量索引。

### 3.5 对话与单文件问答

```text
chat.js
└── POST /api/agent/chat/stream {prompt}
    ├── 按用户取内存对话历史
    ├── DeepSeek function calling -> Core 文件搜索（可选）
    └── SSE delta* -> done | error

file-qa.js
└── POST /api/agent/file_qa/stream {hash, question}
    ├── 短文：完整 Markdown -> DeepSeek
    └── 长文：该文件 RAG top-k -> DeepSeek
        └── SSE delta* -> done {mode:"full"|"rag"}
```

输入：JWT、问题，单文件问答另需 hash。输出：SSE 增量文本、完成或错误事件。

## 4. Core HTTP 入口与输出

| 入口 | 主要输入 | 主要输出 | 实现文件 |
| --- | --- | --- | --- |
| `GET /`, `/index.html`, `/assets/*`, `/vendor/*` | 路径 | HTML/CSS/JS 字节 | `StaticHandlers.cpp` |
| `GET /api/config` | 无 | Agent 端口和上传参数 JSON | `StaticHandlers.cpp` |
| `GET /ping` | 无 | 健康状态 JSON | `Router.cpp` |
| `POST /register`, `/login` | `User`, `Password` header | 文本状态 / JWT JSON | `AuthHandlers.cpp` |
| `POST /upload` | JWT，文件 headers，body | 普通上传结果 JSON | `UploadHandlers.cpp` |
| `GET /api/upload/init` | JWT，hash/total/size/chunkSize query | `exists` 或 `missing[]` | `UploadHandlers.cpp` |
| `POST /api/upload/chunk` | JWT，hash/index headers，body | `{"status":"ok"}` | `UploadHandlers.cpp` |
| `POST /api/upload/merge` | JWT，name/hash/count/size/directory headers | 合并和元数据写入结果 | `UploadHandlers.cpp` |
| `GET /api/list`, `/api/v1/files/all`, `/api/v1/me` | JWT，可选目录/回收站 | 文件 JSON 或当前用户 JSON | `FileReadHandlers.cpp` |
| `GET /download`, `/api/preview` | JWT 或 query token，hash，Range | 全量/范围文件字节 | `FileReadHandlers.cpp` |
| `POST /api/delete`, `/restore`, `/purge`, `/rename`, `/move` | JWT，hash，可选 name/dir | 变更状态 JSON | `FileMutationHandlers.cpp` |
| `GET /api/stats` | JWT | 使用容量 JSON | `FileMutationHandlers.cpp` |
| `GET|POST /api/folders` 及子路由 | JWT，path/name/dir | 目录列表或变更状态 | `FolderHandlers.cpp` |
| `POST /api/share/create`, `GET /share/download` | JWT+hash+hours / share token | 分享 URL JSON / 文件字节 | `ShareHandlers.cpp` |
| `GET /api/v1/files/search` | JWT，keyword | 匹配文件 JSON | `AgentMetadataHandlers.cpp` |
| `POST /api/v1/files/summary`, `/tags` | JWT，File-Hash，body | 元数据写回状态 | `AgentMetadataHandlers.cpp` |

## 5. Agent HTTP 入口与输出

| 入口 | 主要输入 | 主要输出 |
| --- | --- | --- |
| `GET /api/agent/health` | 无 | 模型、缓存、RAG、格式支持状态 JSON |
| `POST /api/agent/summarize` | JWT，`{hash, force}` | 同步摘要、标签、索引结果 |
| `POST /api/agent/summarize/start` | JWT，`{hash, force}` | 后台任务 JSON |
| `GET .../status/{id}`, `GET .../tasks` | JWT，task id | 任务/任务列表 JSON |
| `POST .../cancel/{id}`, `POST .../retry/{id}` | JWT，task id | 取消/重试后任务 JSON |
| `POST .../summarize/missing` | JWT | 缺摘要文件的批量排队结果 |
| `GET /api/agent/markdown/{hash}` | JWT，hash | 文件名和 Markdown JSON |
| `POST /api/agent/file_qa` | JWT，`{hash, question}` | 回答与 `full/rag` 模式 JSON |
| `POST /api/agent/file_qa/stream` | 同上 | SSE `delta/done/error` |
| `POST /api/agent/rag/query` | JWT，`{query, top_k}` | 相似块、分数和生成回答 |
| `POST /api/agent/chat` | JWT，`{prompt}` | 对话回答 JSON |
| `POST /api/agent/chat/stream` | 同上 | SSE `delta/done/error` |
| `POST /api/agent/clear_history` | JWT | 当前用户历史清理状态 |

## 6. 逐文件作用、输入与输出

### 6.1 根目录、配置和文档

| 文件 | 作用 | 输入 | 输出 |
| --- | --- | --- | --- |
| `.gitignore` | 排除构建、Python 缓存、运行数据、模型和 IDE 文件 | Git 工作树路径 | Git 忽略规则 |
| `CMakeLists.txt` | 编译 Core，配置 C++17、include 路径和链接库 | C++ 源码、Workflow、OpenSSL、SQLite3、pthread | `build/bin/SmartNAS` |
| `README.md` | 面向使用者的功能、依赖、启动和 API 概览 | 项目行为 | 使用说明 |
| `config/config.json` | Core 和 Agent 的统一默认配置 | 部署参数 | 端口、路径、上传、LLM、RAG、审计参数 |
| `server-env-setup` | 在 WSL/Linux 中从默认路由推导代理地址 | `ip route`，10808 代理端口 | `winip`、`all_proxy`、`ALL_PROXY` 环境变量 |
| `docs/architecture.md` | 当前系统分层、目录约定和数据流 | 当前实现 | 整体架构文档 |
| `docs/code-architecture.md` | 代码结构审计、耦合风险和演进顺序 | 源码分层 | 整理建议 |
| `docs/agent-decoupling.md` | Core/Agent 解耦边界和迁移说明 | Core 与 Agent 调用关系 | 边界约束文档 |
| `docs/agent-service.md` | Agent 配置、API、缓存、任务和审计说明 | Agent 实现 | Agent 运维/接口文档 |
| `docs/project-flow-tree.md` | 本文档，联系运行流程与每个文件 | 全仓库结构与调用链 | 流程树和文件 I/O 索引 |

### 6.2 C++ 公开头文件

| 文件 | 作用 | 输入 | 输出 |
| --- | --- | --- | --- |
| `include/smartnas/api/Router.h` | 声明 Router facade 及各 HTTP handler | Workflow HTTP 类型、`FileMetadata` | Core API 编译期接口 |
| `include/smartnas/config/AppConfig.h` | 声明单例配置和只读 getter | 配置文件路径 | 端口、目录、JWT、上传参数 |
| `include/smartnas/core/FileManager.h` | 声明文件、分片、合并和 Range 读取接口 | 文件名/hash/字节/偏移 | 成功状态、字节、合并 hash/大小 |
| `include/smartnas/core/FileMetadata.h` | 定义文件、目录和分享 DTO | DB/API 字段 | `FileMetadata`、`FolderMetadata`、`ShareMetadata` |
| `include/smartnas/core/User.h` | 定义用户 DTO | 用户表字段 | `User` 结构 |
| `include/smartnas/db/DatabaseManager.h` | 声明 SQLite facade 的用户、文件、目录、分享操作 | DTO、hash、owner、path | bool/计数/DTO/列表查询接口 |
| `include/smartnas/utils/HashUtil.h` | 声明 SHA-256 和 URL 解码 | 字节、文件路径或 URL 文本 | hash 或解码文本 |

### 6.3 C++ Core 实现

| 文件 | 作用 | 输入 | 输出/副作用 |
| --- | --- | --- | --- |
| `src/main.cpp` | Core composition root：加载配置、创建目录、初始化 DB、启动 HTTP | argv/`SMARTNAS_CONFIG` | 长驻 Core 进程或错误退出码 |
| `src/config/AppConfig.cpp` | 解析受控的扁平 JSON，将相对路径解析到项目根 | JSON 文本 | `AppConfig` 单例状态 |
| `src/api/Router.cpp` | CORS、OPTIONS、JWT 解析、URI 分发、ping/404 | `WFHttpTask` | 转发到 handler 或直接 HTTP 响应 |
| `src/api/RouterInternal.h` | API 实现域私有辅助声明 | `std::string` | detail 编译期接口，不对外暴露 |
| `src/api/RouterInternal.cpp` | JWT secret、hash/JSON 验证、query 解码、路径规范化、token 生成 | URI/文本/配置 | 规范化字符串、bool、随机 token |
| `src/api/AuthHandlers.cpp` | 注册、登录和 JWT 签发 | User/Password headers | 注册文本或 `{token}` JSON |
| `src/api/UploadHandlers.cpp` | 普通/分片上传、断点续传、秒传、合并校验 | JWT，query/headers，文件 body | `.partN`/`.bin`、files 元数据、状态 JSON |
| `src/api/FileReadHandlers.cpp` | 列表、当前用户、下载、预览和 HTTP Range | JWT，hash/dir/deleted/Range | JSON 或 200/206 文件字节流 |
| `src/api/FileMutationHandlers.cpp` | 软删除、恢复、彻底删除、重命名、移动、容量统计 | JWT，hash/name/dir | files 表变更、可选物理删除、JSON |
| `src/api/FolderHandlers.cpp` | 列表、创建、重命名、移动、删除目录 | JWT，path/name/dir | folders/files 表变更或 JSON |
| `src/api/ShareHandlers.cpp` | 创建有效期分享并公开下载 | JWT+hash+hours，或 token | shares 记录、share URL 或文件字节 |
| `src/api/StaticHandlers.cpp` | 受限静态资源和浏览器运行配置 | URL、`web_dir`、上传配置 | HTML/CSS/JS/WASM 脚本或 JSON |
| `src/api/AgentMetadataHandlers.cpp` | 为 Agent 提供文件搜索、摘要和标签写回 | JWT，keyword/File-Hash/body | 搜索 JSON 或 DB 写回状态 |
| `src/core/FileManager.cpp` | `var/data` 中的文件 I/O、分片合并、EVP SHA-256、`pread` | 文件名/hash/分片/偏移 | 物理文件、读取字节、hash/大小、bool |
| `src/utils/HashUtil.cpp` | 实现内存/文件 SHA-256 与 percent-decoding | 字节或编码字符串 | 64 位 hex hash 或解码文本 |
| `src/db/DatabaseManager.cpp` | 打开 SQLite，创建/migrate users、files、folders、shares | DB 路径 | schema 与共享 sqlite 连接 |
| `src/db/UserRepository.cpp` | 注册和密码校验 | username/password | users 行、bool |
| `src/db/FileWriteRepository.cpp` | 写入文件元数据、摘要和标签 | `FileMetadata` 或 owner/hash/value | files 行变更、bool |
| `src/db/FileLookupRepository.cpp` | 按 hash/owner 查文件、存在性、引用计数 | hash、username | `FileMetadata`、bool 或 count |
| `src/db/FileQueryRepository.cpp` | 查目录/全量/回收站/摘要搜索/容量 | owner、directory、keyword | `vector<FileMetadata>` 或容量字节数 |
| `src/db/FileMutationRepository.cpp` | 删除元数据、软删除、恢复、重命名、移动 | owner、hash、name/directory | files 行变更、bool |
| `src/db/FolderRepository.cpp` | 目录 CRUD 与目录树移动事务 | owner、path、new_path | folders/files 行变更或目录列表 |
| `src/db/ShareRepository.cpp` | 写入和查询分享 token | token、owner、hash、过期时间 | shares 行或 `ShareMetadata` |

### 6.4 Python Agent

| 文件 | 作用 | 输入 | 输出/副作用 |
| --- | --- | --- | --- |
| `agent/__init__.py` | 标记 `agent` 为 Python package | Python import | package namespace |
| `agent/config.py` | 加载 JSON，应用环境变量覆盖，解析绝对路径 | `SMARTNAS_CONFIG`、配置 JSON、环境变量 | Agent 模块级配置常量 |
| `agent/schemas.py` | 定义 Pydantic HTTP body | JSON body | `ChatRequest`、`SummarizeRequest`、`FileQuestionRequest`、`RagQueryRequest` |
| `agent/llm_client.py` | DeepSeek 非流式/流式客户端、tool calling 消息和文本清理 | messages、tools、LLM 配置 | assistant message、完整文本或 token iterator |
| `agent/sse.py` | 统一 SSE 事件 JSON 和防缓冲 header | event type/payload 或事件 iterable | `data: {...}\n\n` 或 `StreamingResponse` |
| `agent/service.py` | FastAPI composition root；Core client、转换、摘要、标签、RAG、任务、对话、审计 | Agent HTTP、JWT、Core HTTP、文件字节、DeepSeek | JSON/SSE，Markdown/向量/任务 DB/审计日志，Core 元数据写回 |

### 6.5 脚本与测试

| 文件 | 作用 | 输入 | 输出 |
| --- | --- | --- | --- |
| `scripts/start_smartnas.sh` | 一键配置、构建、启动和联动停止 | 配置、CMake、Python 环境 | Core/Agent 进程和启动信息 |
| `scripts/agent_service.py` | 向后兼容的 Agent 启动入口 | Agent host/port、`agent.service.app` | Uvicorn HTTP 服务 |
| `scripts/requirements-agent.txt` | 声明 Agent Python 依赖 | pip | FastAPI、MarkItDown、Pillow、embedding、FAISS 等安装集 |
| `tests/test_agent_modules.py` | 验证 schema、SSE 和 LLM 流式解析 | unittest、mock HTTP stream | 测试通过/失败结果 |

### 6.6 Web 前端

| 文件 | 作用 | 输入 | 输出/副作用 |
| --- | --- | --- | --- |
| `web/index.html` | 页面 DOM、登录/注册/文件/任务/对话模态框，按顺序加载脚本 | Core 静态资源请求、用户交互 | DOM 和脚本全局函数调用 |
| `web/css/app.css` | 页面布局、响应式、模态框、文件列表和对话视觉 | HTML class/id | 渲染样式 |
| `web/js/core.js` | 运行配置、全局状态、认证、toast 和模态框 | `/api/config`、登录输入、localStorage | `API_BASE/AGENT_BASE`、JWT、全局 UI 状态 |
| `web/js/files.js` | 文件/目录列表获取、排序、过滤、面包屑和菜单渲染 | `/api/list` JSON，搜索/排序/当前目录 | `globalFiles/globalFolders`、列表 DOM |
| `web/js/upload.js` | SHA-256、分片并发、断点续传、合并和上传错误处理 | Browser `File`、上传配置、JWT | hash、Core 上传请求、进度/UI 状态 |
| `web/js/operations.js` | 下载、预览、摘要任务、删除/恢复/移动/重命名/分享/建目录 | 菜单事件、hash/path、JWT | Core/Agent 请求、轮询状态、刷新后 UI |
| `web/js/file-qa.js` | Markdown 查看和单文件 SSE 问答 | hash、question、JWT | Markdown DOM 或增量回答 DOM |
| `web/js/chat.js` | 普通 SSE 对话、中止生成和清空历史 | prompt、JWT、AbortController | 增量对话 DOM 和 Agent 历史变更 |
| `web/js/bootstrap.js` | 页面启动顺序 | `window.onload` | 先 `loadRuntimeConfig`，再 `checkAuth` |
| `web/vendor/hash-wasm-sha256.umd.min.js` | 大文件增量 SHA-256 UMD 依赖 | 分块字节 | `globalThis.hashwasm.createSHA256` |
| `web/vendor/hash-wasm-LICENSE` | hash-wasm 第三方许可文本 | 上游许可条款 | 分发合规声明 |

### 6.7 vendored jwt-cpp

这些文件是上游第三方库，Core 当前直接使用默认 `picojson` traits；其他 traits 是可选 JSON 后端适配。

| 文件 | 作用 | 输入 | 输出 |
| --- | --- | --- | --- |
| `third_party/jwt-cpp/LICENSE` | jwt-cpp 许可条款 | 上游项目 | 分发合规声明 |
| `third_party/jwt-cpp/README.md` | 上游用法和兼容性说明 | jwt-cpp API | 第三方使用文档 |
| `third_party/jwt-cpp/include/jwt-cpp/base.h` | Base64/Base64URL 编解码基础实现 | JWT 字节/文本 | 编码/解码结果 |
| `third_party/jwt-cpp/include/jwt-cpp/jwt.h` | JWT 创建、解析、claim、算法和验签主头 | token、key、claims | 签名 token、decoded JWT、验证结果 |
| `third_party/jwt-cpp/include/picojson/picojson.h` | 内置轻量 JSON parser/serializer | JSON 文本/值 | JSON DOM/序列化文本 |
| `third_party/jwt-cpp/include/jwt-cpp/traits/defaults.h.in` | 构建时默认 traits 模板 | CMake/选定 JSON backend | 生成的默认 traits header |
| `third_party/jwt-cpp/include/jwt-cpp/traits/boost-json/defaults.h` | Boost.JSON 默认类型别名 | Boost.JSON 类型 | jwt-cpp traits defaults |
| `third_party/jwt-cpp/include/jwt-cpp/traits/boost-json/traits.h` | Boost.JSON 适配 | JWT JSON 操作 | parser/serializer traits |
| `third_party/jwt-cpp/include/jwt-cpp/traits/danielaparker-jsoncons/defaults.h` | jsoncons 默认类型别名 | jsoncons 类型 | jwt-cpp traits defaults |
| `third_party/jwt-cpp/include/jwt-cpp/traits/danielaparker-jsoncons/traits.h` | jsoncons 适配 | JWT JSON 操作 | parser/serializer traits |
| `third_party/jwt-cpp/include/jwt-cpp/traits/glaze-json/defaults.h` | Glaze 默认类型别名 | Glaze 类型 | jwt-cpp traits defaults |
| `third_party/jwt-cpp/include/jwt-cpp/traits/glaze-json/traits.h` | Glaze 适配 | JWT JSON 操作 | parser/serializer traits |
| `third_party/jwt-cpp/include/jwt-cpp/traits/kazuho-picojson/defaults.h` | picojson 默认类型别名 | picojson 类型 | Core 当前 JWT 默认 traits |
| `third_party/jwt-cpp/include/jwt-cpp/traits/kazuho-picojson/traits.h` | picojson 适配 | JWT JSON 操作 | parser/serializer traits |
| `third_party/jwt-cpp/include/jwt-cpp/traits/nlohmann-json/defaults.h` | nlohmann/json 默认类型别名 | nlohmann JSON 类型 | jwt-cpp traits defaults |
| `third_party/jwt-cpp/include/jwt-cpp/traits/nlohmann-json/traits.h` | nlohmann/json 适配 | JWT JSON 操作 | parser/serializer traits |
| `third_party/jwt-cpp/include/jwt-cpp/traits/open-source-parsers-jsoncpp/defaults.h` | JsonCpp 默认类型别名 | JsonCpp 类型 | jwt-cpp traits defaults |
| `third_party/jwt-cpp/include/jwt-cpp/traits/open-source-parsers-jsoncpp/traits.h` | JsonCpp 适配 | JWT JSON 操作 | parser/serializer traits |
| `third_party/jwt-cpp/include/jwt-cpp/traits/reflectcpp-json/defaults.h` | reflect-cpp 默认类型别名 | reflect-cpp 类型 | jwt-cpp traits defaults |
| `third_party/jwt-cpp/include/jwt-cpp/traits/reflectcpp-json/traits.h` | reflect-cpp 适配 | JWT JSON 操作 | parser/serializer traits |

## 7. 运行时数据树

```text
var/                              # 全部由程序重建，不进 Git
├── db/smartnas.db                # Core SQLite：users/files/folders/shares
├── data/
│   ├── <sha256>.bin              # 合并后文件本体
│   └── <sha256>.part<N>          # 未合并上传分片
├── cache/
│   ├── markdown/<hash>.v2.md    # Agent Markdown 缓存
│   ├── vector/<namespace>/      # chunks.json / index.faiss
│   └── agent_tasks.db           # 摘要任务 SQLite
└── log/agent_audit.jsonl          # Agent 轮转审计日志
```

## 8. 依赖方向

```text
web -> Core HTTP
web -> Agent HTTP
Agent -> Core HTTP
Agent -> DeepSeek HTTP

src/main
├── config
├── api -> core + db + utils + config
├── core -> config
└── db -> core DTO + utils

agent.service
├── agent.config
├── agent.schemas -> agent.config
├── agent.llm_client -> agent.config
└── agent.sse
```

不应出现的反向依赖：Core 不 import/call Agent 实现；Agent 不 include C++ Core 代码，也不直接读取 `smartnas.db`。
