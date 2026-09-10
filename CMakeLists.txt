cmake_minimum_required(VERSION 3.12)

project(netx LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# 默认使用 Release 构建，若需要调试可自行 -DCMAKE_BUILD_TYPE=Debug
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Choose the type of build." FORCE)
endif()

# 统一输出到 build 根目录
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

# 生成 compile_commands.json 便于 IDE/调试
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ============================================================
# 构建选项
# ============================================================
option(NETX_BUILD_EXAMPLES "Build netx examples" ON)
option(NETX_BUILD_APPS     "Build netx applications" ON)
option(NETX_USE_IOURING    "Use io_uring backend (requires liburing)" OFF)

# LTO
include(CheckIPOSupported)
check_ipo_supported(RESULT ipo_supported OUTPUT ipo_error)
if(ipo_supported)
  set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
endif()

# 依赖
find_package(Threads REQUIRED)
find_library(HIREDIS_LIB hiredis)

# ============================================================
# 子目录
# ============================================================
add_subdirectory(src)

if(NETX_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

if(NETX_BUILD_APPS)
  add_subdirectory(app)
endif()
