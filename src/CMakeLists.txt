set(SRC_LIST
    logging.cc
    inet_address.cc
    socket.cc
    epoll_poller.cc
    channel.cc
    event_loop.cc
    buffer.cc
    acceptor.cc
    event_loop_thread.cc
    io_thread_pool.cc
    tcp_connection.cc
    tcp_server.cc
    iouring_poller.cc
    worker_thread_pool.cc
    http_request.cc
    http_response.cc
    http_parser.cc
    http_server.cc
    websocket_server.cc
    timer.cc
    timer_wheel.cc
    simulator_data_source.cc
)

add_library(netx STATIC ${SRC_LIST})

target_include_directories(netx PUBLIC ${PROJECT_SOURCE_DIR}/include)

target_compile_options(netx PRIVATE
    $<$<CONFIG:Debug>:-O0 -g3 -ggdb3 -fno-omit-frame-pointer>
    $<$<CONFIG:Release>:-O3 -DNDEBUG -march=native>
    -Wall -Wextra -Wpedantic -Wno-unused-parameter
)

target_link_libraries(netx PRIVATE Threads::Threads)

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
