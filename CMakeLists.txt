# 1. 声明 CMake 最低版本要求
cmake_minimum_required(VERSION 3.10)

# 2. 声明项目名称和使用的语言 (CXX 代表 C++)
project(SmartNAS CXX)

# 3. 设置 C++ 标准为 C++17 (符合你的规划)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 4. 指定编译输出目录，让生成的可执行文件直接进入 /bin 目录
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)

# 5. 添加头文件搜索路径 (让你代码里的 #include "smartnas/..." 能找到)
include_directories(${CMAKE_SOURCE_DIR}/include)

# 6. 查找系统中安装的 pthread 和 OpenSSL (Workflow 的底层依赖)
find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)

# 7. 定义你的可执行文件，目前只包含 main.cpp
add_executable(SmartNAS
    src/main.cpp
    src/api/Router.cpp
    src/core/FileManager.cpp
    src/utils/HashUtil.cpp
    src/db/DatabaseManager.cpp # 【新增】
)
# 8. 链接第三方库
# workflow: 搜狗网络引擎
# OpenSSL::SSL / OpenSSL::Crypto: 加密库
# Threads::Threads: 线程库
target_link_libraries(SmartNAS PRIVATE
    workflow
    OpenSSL::SSL
    OpenSSL::Crypto
    Threads::Threads
)