# SmartNAS —— 技术选型详细说明（Architecture Rationale）

本章节详细阐述 SmartNAS 各核心技术的**选择依据、替代方案对比与架构价值**。

---

# 1️⃣ 编程语言：C++20

## 为什么选择 C++20？

SmartNAS 的核心目标是：

- 极致性能
- 可控资源管理
- 高并发 I/O
- GPU 推理集成
- 系统级优化能力

C++ 是少数能够：

- 直接操作系统调用（epoll / sendfile）
- 深度集成 CUDA
- 与高性能 AI 推理库无缝结合
- 同时实现零运行时开销抽象

的语言。

C++20 相比旧标准的优势：

### ① Concepts

- 在编译期约束模板参数
- 替代 SFINAE 黑魔法
- 提高泛型代码可读性与安全性
- 减少难以理解的编译报错

### ② Ranges

- 构建声明式数据流
- 避免中间容器拷贝
- 提升代码表达力与性能

### ③ 协程（预留扩展）

虽然当前核心使用异步框架，但 C++20 协程为未来网络层重构提供可能。

### ④ RAII + 智能指针

- deterministic resource management
- 避免 GC 停顿
- 适合高吞吐服务器场景

---

# 2️⃣ 网络调度核心：Sogou Workflow

## 为什么选择 Workflow？

这是一个工业级异步调度框架，核心优势在于：

- 纯回调式异步模型
- 计算与网络线程分离
- 内置 MySQL / Redis 异步客户端
- 基于 epoll 的高性能事件循环

## 架构核心优势

### ① Compute-Network 分离模型

AI 推理（GPU 矩阵运算）耗时较长。

如果使用传统线程池模型：

- 可能阻塞 I/O 线程
- 导致吞吐下降

Workflow 将：

- 网络 I/O 任务
- 计算任务（AI 推理）

分离调度，避免互相影响。

## 为什么不选 Boost.Asio / libuv？

| 方案 | 问题 |
|------|------|
| Boost.Asio | 需要自行构建大量基础设施 |
| libuv | 更适合 Node.js 生态 |
| 自研 Reactor | 开发成本高，稳定性风险大 |

Workflow 已提供成熟工程能力。

---

# 3️⃣ 多模态推理：ONNX Runtime（GPU 版）

## 选择依据

SmartNAS 的核心能力是：

> 语义向量生成

模型为 CLIP。

ONNX Runtime 优势：

- 支持 CUDA 加速
- 支持 Tensor Core
- 支持模型量化
- 跨平台推理引擎

## 为什么不用 PyTorch C++ API？

| 方案 | 问题 |
|------|------|
| PyTorch | 依赖重，部署复杂 |
| TensorRT | 对模型转换要求高 |
| ONNX Runtime | 轻量、通用、生产级 |

## 性能收益

- RTX 3060 Tensor Core 加速
- 相比 CPU 提升 20 倍以上

---

# 4️⃣ 向量数据库：Faiss

## 为什么选择 Faiss？

语义搜索本质是：

> 高维向量近似最近邻（ANN）

Faiss 优势：

- 成熟 ANN 实现
- 支持 IVF-PQ
- 支持 GPU 加速
- 亿级数据支持

## 为什么不选 Milvus？

Milvus 是分布式系统，SmartNAS 是：

> 单机边缘系统

Faiss 更轻量。

## IVF-PQ 的意义

- 倒排索引减少候选集
- 乘积量化降低内存占用
- 在性能与精度之间取得平衡

---

# 5️⃣ 高性能文件 I/O：Linux sendfile

## 选择依据

传统文件下载流程：

用户态缓冲区中转

造成：

- CPU 拷贝开销
- 多次上下文切换

sendfile 实现：

> 零拷贝（Zero-Copy）

数据路径：

Page Cache → Socket Buffer

无需进入用户空间。

## 为什么不使用 read/write？

read/write 会：

- 产生两次内存拷贝
- 增加 CPU 负担

sendfile 更适合大文件传输。

---

# 6️⃣ 存储层：MySQL 8.0 + Redis

## 分层设计理念

- MySQL：元数据持久化
- Redis：高频状态缓存

## 为什么不是 PostgreSQL？

MySQL 生态成熟：

- 运维成本低
- 社区支持强
- Workflow 原生支持

## Redis 的作用

- 分片上传状态机
- Bitmap 记录块完成情况
- Set 维护分片集合

支持秒级断点续传。

---

# 7️⃣ JSON 解析：simdjson

## 选择依据

API 层 JSON 解析是高频操作。

simdjson：

- 使用 AVX2 / AVX-512
- 单核 GB/s 解析能力

## 为什么不用 nlohmann/json？

| 方案 | 问题 |
|------|------|
| nlohmann | 易用但性能低 |
| RapidJSON | 性能好但不如 SIMD |
| simdjson | 极致性能 |

在高 QPS 场景下降低 CPU 占用。

---

# 8️⃣ 日志系统：spdlog

## 选择依据

高频日志会成为性能瓶颈。

spdlog：

- header-only
- 异步写盘
- 高性能格式化

## 为什么不用 glog？

glog：

- Google 风格
- 但同步写盘为主
- 性能不如 spdlog

---

# 9️⃣ 二期规划：llama.cpp

## 为什么选择 llama.cpp？

目标：

> 本地部署轻量大模型

优势：

- 支持 4-bit 量化
- 支持 CPU / GPU 混合推理
- 单机部署简单

规划模型：

Qwen2.5 1.5B

## 未来能力

- 文档摘要
- 本地问答
- RAG 增强检索
- 语义重排序

---

# 🧠 总体架构哲学

SmartNAS 技术选型遵循三条原则：

1. 单机极致性能优先
2. 零冗余抽象
3. 边缘算力最大化利用

---

# 🎯 最终目标

在个人 PC / NAS 上实现：

- 高并发
- 毫秒级语义检索
- 本地 AI 推理
- 零隐私泄露

构建真正属于个人的数据智能系统。