cmake_minimum_required(VERSION 3.12)

project(netx LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# 默认使用 Release 构建，若需要调试可自行 -DCMAKE_BUILD_TYPE=Debug
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Choose the type of build." FORCE)
endif()

# 生成 compile_commands.json 便于 IDE/调试
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ============================================================
# 构建选项
# ============================================================
option(NETX_BUILD_EXAMPLES "Build netx examples" ON)
option(NETX_BUILD_APPS     "Build netx applications" ON)
option(NETX_USE_IOURING    "Use io_uring backend (requires liburing)" OFF)

# ============================================================
# 核心库
# ============================================================
add_library(netx STATIC
    src/logging.cc
    src/inet_address.cc
    src/socket.cc
    src/epoll_poller.cc
    src/channel.cc
    src/event_loop.cc
    src/buffer.cc
    src/acceptor.cc
    src/event_loop_thread.cc
    src/io_thread_pool.cc
    src/tcp_connection.cc
    src/tcp_server.cc
    src/iouring_poller.cc
    src/worker_thread_pool.cc
    src/http_request.cc
    src/http_response.cc
    src/http_parser.cc
    src/http_server.cc
    src/websocket_server.cc
)

# 头文件路径（PUBLIC = 使用 netx 的目标自动继承此 include 路径）
target_include_directories(netx PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# 编译选项
target_compile_options(netx PRIVATE
    $<$<CONFIG:Debug>:-O0 -g3 -ggdb3 -fno-omit-frame-pointer>
    $<$<CONFIG:Release>:-O3 -DNDEBUG -march=native>
    -Wall -Wextra -Wpedantic -Wno-unused-parameter
)

# 开启 Release 下的 LTO（若编译器支持）
include(CheckIPOSupported)
check_ipo_supported(RESULT ipo_supported OUTPUT ipo_error)
if(ipo_supported)
  set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
endif()

# 线程库
find_package(Threads REQUIRED)
target_link_libraries(netx PRIVATE Threads::Threads)

# io_uring 后端（可选）
if(NETX_USE_IOURING)
  find_package(PkgConfig)
  if(PKG_CONFIG_FOUND)
    pkg_check_modules(LIBURING liburing)
  endif()
  if(LIBURING_FOUND)
    target_compile_definitions(netx PUBLIC NETX_USE_IOURING)
    target_include_directories(netx PRIVATE ${LIBURING_INCLUDE_DIRS})
    target_link_libraries(netx PRIVATE ${LIBURING_LIBRARIES})
  else()
    message(WARNING "NETX_USE_IOURING=ON but liburing not found; falling back to epoll backend")
  endif()
endif()

# ============================================================
# 示例程序
# ============================================================
if(NETX_BUILD_EXAMPLES)
  add_executable(echo_server examples/echo_server.cc)
  target_link_libraries(echo_server PRIVATE netx)

  add_executable(echo_perf_client examples/echo_perf_client.cc)
  target_link_libraries(echo_perf_client PRIVATE Threads::Threads)

  add_executable(http_server examples/http_server.cc)
  target_link_libraries(http_server PRIVATE netx)

  add_executable(http_file_server examples/http_file_server.cc)
  target_link_libraries(http_file_server PRIVATE netx)

  add_executable(logging_bench examples/logging_bench.cc)
  target_link_libraries(logging_bench PRIVATE netx)
endif()

# ============================================================
# 应用程序
# ============================================================
if(NETX_BUILD_APPS)
  # 高并发红绿灯推送服务器与客户端
  add_executable(traffic_push_server app/traffic_push/traffic_push_server.cc)
  target_link_libraries(traffic_push_server PRIVATE netx)

  add_executable(traffic_push_client app/traffic_push/traffic_push_client.cc)
  target_link_libraries(traffic_push_client PRIVATE Threads::Threads)

  # 股票推送服务器与客户端
  add_executable(stock_push_server app/stock_push/stock_push_server.cc)
  target_link_libraries(stock_push_server PRIVATE netx)

  add_executable(stock_push_client app/stock_push/stock_push_client.cc)
  target_link_libraries(stock_push_client PRIVATE Threads::Threads)
endif()
