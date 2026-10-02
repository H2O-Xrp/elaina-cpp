add_executable(hello_world hello_world.cpp)
target_link_libraries(hello_world PRIVATE elaina::core)

add_executable(json_example json_example.cpp)
target_link_libraries(json_example PRIVATE elaina::core)

add_executable(rest_example rest_example.cpp)
target_link_libraries(rest_example PRIVATE elaina::core)

add_executable(https_example https_example.cpp)
target_link_libraries(https_example PRIVATE elaina::core)
