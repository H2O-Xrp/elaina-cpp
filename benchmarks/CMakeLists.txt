add_executable(bench_micro bench_micro.cpp)
target_link_libraries(bench_micro PRIVATE elaina::core)

add_executable(http_load http_load.cpp)
target_link_libraries(http_load PRIVATE Threads::Threads)

add_executable(elaina_server comparison/elaina/server.cpp)
target_link_libraries(elaina_server PRIVATE elaina::core)
set_target_properties(elaina_server PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/benchmarks/comparison)
