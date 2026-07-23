# 1. 声明 CMake 最低版本要求
cmake_minimum_required(VERSION 3.10)

# 2. 声明项目名称和使用的语言 (CXX 代表 C++)
project(SmartNAS CXX)

include(CTest)

# 3. 设置 C++ 标准为 C++17 (符合你的规划)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 4. 指定编译输出目录，让生成的可执行文件留在 build 目录内
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# 5. 添加头文件搜索路径 (让你代码里的 #include "smartnas/..." 能找到)
include_directories(
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/third_party/jwt-cpp/include
)

# 6. 查找系统中安装的 pthread 和 OpenSSL (Workflow 的底层依赖)
find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)
find_library(SQLITE3_LIB sqlite3)

# 7. 定义核心服务及按领域拆分的 HTTP handlers
add_executable(SmartNAS
    src/main.cpp
    src/config/AppConfig.cpp
    src/api/Router.cpp
    src/api/RouterInternal.cpp
    src/api/AuthHandlers.cpp
    src/api/UploadHandlers.cpp
    src/api/FileMutationHandlers.cpp
    src/api/FolderHandlers.cpp
    src/api/ShareHandlers.cpp
    src/api/StaticHandlers.cpp
    src/api/FileReadHandlers.cpp
    src/api/AgentMetadataHandlers.cpp
    src/core/FileManager.cpp
    src/utils/HashUtil.cpp
    src/db/DatabaseManager.cpp
    src/db/UserRepository.cpp
    src/db/FileWriteRepository.cpp
    src/db/FileLookupRepository.cpp
    src/db/FileQueryRepository.cpp
    src/db/FileMutationRepository.cpp
    src/db/FolderRepository.cpp
    src/db/ShareRepository.cpp
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
    ${SQLITE3_LIB}
)

if(BUILD_TESTING)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_test(
        NAME core_api_integration
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/test_core_api.py $<TARGET_FILE:SmartNAS>
    )
    set_tests_properties(core_api_integration PROPERTIES TIMEOUT 30)
endif()
