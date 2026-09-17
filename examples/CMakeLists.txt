add_executable(hello_world hello_world.cpp)
target_link_libraries(hello_world PRIVATE elaina::core)

add_executable(json_example json_example.cpp)
target_link_libraries(json_example PRIVATE elaina::core)
