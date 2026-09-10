add_executable(echo_server echo_server.cc)
target_link_libraries(echo_server PRIVATE netx)

add_executable(echo_perf_client echo_perf_client.cc)
target_link_libraries(echo_perf_client PRIVATE Threads::Threads)

add_executable(http_server http_server.cc)
target_link_libraries(http_server PRIVATE netx)

add_executable(http_file_server http_file_server.cc)
target_link_libraries(http_file_server PRIVATE netx)

add_executable(logging_bench logging_bench.cc)
target_link_libraries(logging_bench PRIVATE netx)

add_executable(timer_test timer_test.cc)
target_link_libraries(timer_test PRIVATE netx)

add_executable(connection_timeout_test connection_timeout_test.cc)
target_link_libraries(connection_timeout_test PRIVATE netx)

add_executable(data_source_test data_source_test.cc)
target_link_libraries(data_source_test PRIVATE netx)

add_executable(connection_pool_test connection_pool_test.cc)
target_link_libraries(connection_pool_test PRIVATE netx)

# Redis 相关
if(HIREDIS_LIB)
    add_executable(redis_simulator redis_simulator.cc)
    target_link_libraries(redis_simulator PRIVATE netx ${HIREDIS_LIB})

    add_executable(redis_data_source_test redis_data_source_test.cc)
    target_link_libraries(redis_data_source_test PRIVATE netx ${HIREDIS_LIB})

    add_executable(redis_pipeline_test redis_pipeline_test.cc)
    target_link_libraries(redis_pipeline_test PRIVATE netx ${HIREDIS_LIB})
endif()
