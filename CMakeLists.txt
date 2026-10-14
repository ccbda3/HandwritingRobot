cmake_minimum_required(VERSION 3.21)

project(
    HandwritingRobot
    VERSION 0.1.0
    DESCRIPTION "学习个人笔迹风格，生成带书写时序的笔画轨迹并导出 GRBL G 代码"
    HOMEPAGE_URL "https://github.com/Whaoy/HandwritingRobot"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(HR_BUILD_APP "构建 Qt 桌面前端" ON)
option(HR_BUILD_TESTS "构建 C++ 单元测试" ON)
option(HR_BUILD_BENCH "构建性能基准" ON)

if(HR_BUILD_TESTS)
    enable_testing()
endif()

# C++ 核心库：轨迹数据结构、运动约束优化、G 代码生成。
# 不依赖 Qt，也不依赖 Python，可独立构建与测试。
add_subdirectory(core)

if(HR_BUILD_BENCH)
    add_subdirectory(benchmarks/cpp)
endif()

if(HR_BUILD_APP)
    add_subdirectory(app)
endif()

message(STATUS "HandwritingRobot ${PROJECT_VERSION}")
message(STATUS "  Qt 前端: ${HR_BUILD_APP} | C++ 测试: ${HR_BUILD_TESTS} | 基准: ${HR_BUILD_BENCH}")
