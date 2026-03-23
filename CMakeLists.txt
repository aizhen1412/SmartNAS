cmake_minimum_required(VERSION 3.24)
project(SmartNAS LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
include_directories(${CMAKE_SOURCE_DIR}/include)
# 查找依赖库
find_package(spdlog REQUIRED)
set(WORKFLOW_LIBS workflow ssl crypto pthread)

# 编译主程序
add_executable(SmartNAS_Scanner src/main.cpp)
target_link_libraries(SmartNAS_Scanner PRIVATE spdlog::spdlog ${WORKFLOW_LIBS})