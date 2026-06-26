# SmartNAS 代码结构审计与整理方案

## 结论

SmartNAS 的服务级边界是合理的：C++ Core 管文件和元数据，Python Agent 管模型、文档理解与向量检索，两者只通过带 JWT 的 HTTP API 通信。这一层已经符合高内聚、低耦合的主要方向。

目前不足主要存在于服务内部：

- Core handler 已按领域拆分，但 handler 内仍混合部分 HTTP 映射与业务判断；
- 数据库实现已按领域拆分，但仍通过单一 `DatabaseManager` facade 和互斥锁共享连接；
- `agent/service.py` 仍同时承担 Core 客户端、转换器、RAG、任务和会话，后续还可继续拆分；
- Web 已拆分 HTML、CSS 和领域脚本，后续重点是减少隐式全局状态；
- 自动化测试较少，限制了大规模安全拆分。

因此当前状态可以概括为：宏观边界良好，微观实现仍偏“巨石文件”。

## 已完成的第一阶段整理

Agent 已先拆出稳定、依赖方向清晰的基础模块：

```text
agent/
├── service.py                   # FastAPI 组装、领域流程和路由
├── config.py                    # JSON 配置与环境变量覆盖
├── schemas.py                   # API 请求模型
├── llm_client.py                # DeepSeek 非流式/流式客户端与输出清理
└── sse.py                       # SSE 事件编码和响应头
```

同时，单文件问答已使用共享的 `prepare_file_qa_context` 和 `record_file_qa_answer`。全文/RAG、流式/非流式只在生成方式上分叉，不再重复维护文件转换、检索和上下文规则。

当前依赖方向是：

```text
agent.service
  ├── config
  ├── schemas -> config
  ├── llm_client -> config
  └── sse
```

基础模块不知道 FastAPI 路由、文件任务或 RAG 存储的存在，主服务负责组合它们。

Core 路由也已完成第一阶段拆分：`Router.cpp` 只保留分发、鉴权入口、健康检查和 404；上传、读取、变更、目录、分享、静态资源和 Agent 元数据分别编译为独立实现文件，公共解析函数收敛到 `RouterInternal`。

数据库实现已完成对应拆分：`DatabaseManager.cpp` 只保留连接生命周期、建表和迁移；用户、文件写入、文件查询、文件变更、文件夹和分享 SQL 分别位于独立 repository 实现文件。公共头文件暂时保持兼容，调用方无需改动。

## 推荐目标结构

### Agent

后续可逐步形成：

```text
agent/
├── config.py
├── schemas.py
├── llm_client.py
├── sse.py
├── core_client.py          # Core 身份、下载、摘要/标签写回
├── converters.py           # MarkItDown 与格式 fallback
├── rag_store.py            # 分块、embedding、FAISS、持久化
├── summary_service.py      # 摘要和标签用例
├── file_qa_service.py      # 单文件上下文与问答用例
├── task_store.py           # SQLite 任务持久化
└── task_service.py         # 排队、取消、重试和工作线程
```

建议保持 `agent/service.py` 为 composition root：创建 FastAPI、注册中间件和路由，不再保存具体算法。

### Core

Core 当前结构已经按 HTTP 领域拆分：

```text
src/api/
├── Router.cpp              # 仅路由分发
├── RouterInternal.h        # API 实现私有声明，不对外暴露
├── RouterInternal.cpp      # 查询参数、JSON、安全路径、JWT 常量
├── AuthHandlers.cpp
├── UploadHandlers.cpp
├── FileReadHandlers.cpp
├── FileMutationHandlers.cpp
├── FolderHandlers.cpp
├── ShareHandlers.cpp
├── StaticHandlers.cpp
├── AgentMetadataHandlers.cpp
```

下一步不再是继续切小 handler 文件，而是提取跨 handler 的文件与目录 use-case，使 HTTP 层只负责请求和响应映射。

数据库实现文件当前已经按领域分开；下一步可从兼容 facade 演进为共享连接/事务对象加独立仓储类：

```text
src/db/
├── DatabaseManager.cpp      # 当前连接、schema 和兼容 facade
├── UserRepository.cpp
├── FileWriteRepository.cpp
├── FileLookupRepository.cpp
├── FileQueryRepository.cpp
├── FileMutationRepository.cpp
├── FolderRepository.cpp
└── ShareRepository.cpp
```

仓储只处理 SQL；上传、移动目录和彻底删除等跨仓储规则应放在 service/use-case 层。这样路由不需要知道 SQL，仓储也不负责 HTTP 状态码。

### Web

Web 已按以下结构拆分，并由 Core 的受限静态资源路由提供：

```text
web/
├── index.html
├── css/app.css
└── js/
    ├── core.js
    ├── files.js
    ├── upload.js
    ├── operations.js
    ├── file-qa.js
    ├── chat.js
    └── bootstrap.js
```

不必立刻引入前端框架。原生 ES modules 已足够降低当前耦合，并保留部署简单的优点。

## 具体耦合风险

### 1. 配置解析边界

工作目录依赖已经消除：Core 启动时从统一配置解析数据库、数据和 Web 的绝对路径。当前 C++ 配置加载器面向受控的扁平 JSON schema；如果未来需要嵌套配置、动态重载或复杂类型，应切换到正式 JSON 库并增加 schema 校验。

### 2. 全局可变状态

Agent 的会话、身份缓存、任务字典和模型实例是进程级全局变量。单进程可用，但多 worker 会产生不同会话和任务视图。建议将会话和任务统一放入持久化 store，并显式注入 service。

### 3. 粗粒度数据库锁

`DatabaseManager` 使用一个 mutex 串行化全部 SQLite 操作。当前个人 NAS 负载可以接受，但上传、列表和 Agent 写回会互相等待。拆仓储后仍应共享事务边界，并评估 WAL 和短连接/连接池，而不是简单地给每个仓储一把独立锁。

### 4. HTTP 与领域逻辑混合

Router handler 当前同时做参数校验、权限、业务判断、数据库调用和响应拼接。建议 handler 只完成 HTTP 映射，领域 service 返回结构化结果，再统一映射状态码和 JSON。

### 5. 缺少回归测试

最优先应覆盖：

- 分片上传、断点续传、合并 hash 校验；
- 文件夹树重命名/移动的事务回滚；
- 空文件夹删除约束；
- 摘要任务去重、取消、重启恢复；
- 全文/RAG 文件问答的流式与非流式一致性；
- 用户之间的会话和向量隔离。

没有这些测试时，不建议一次性拆完 Router、DatabaseManager 和前端。

## 推荐实施顺序

1. 在现有 Agent 模块单测基础上，继续补接口与数据库集成测试。
2. 继续拆 Agent 的 `core_client`、`task_store` 和 `file_qa_service`。
3. 在已拆分的 Core handlers 之下增加文件与目录 use-case 层。
4. 将当前 repository 实现文件进一步变成独立仓储类，保留显式事务 service。
5. 将现有经典脚本逐步升级为显式 import/export 的 ES modules。
6. 最后再考虑依赖注入容器、前端框架或多进程部署。

原则是每一步都保持 API 和行为不变，先建立测试保护，再移动代码。高内聚来自明确职责，不来自文件数量本身。
