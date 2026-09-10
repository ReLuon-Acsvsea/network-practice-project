# add_executable(traffic_push_server traffic_push_server.cc)
# target_link_libraries(traffic_push_server PRIVATE netx)

add_executable(traffic_push_server_v2 traffic_push_server_v2.cc)
target_link_libraries(traffic_push_server_v2 PRIVATE netx)
if(HIREDIS_LIB)
    target_link_libraries(traffic_push_server_v2 PRIVATE ${HIREDIS_LIB})
endif()

add_executable(traffic_push_client traffic_push_client.cc)
target_link_libraries(traffic_push_client PRIVATE Threads::Threads)
