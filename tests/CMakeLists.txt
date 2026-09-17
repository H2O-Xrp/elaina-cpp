add_executable(test_router test_router.cpp)
target_link_libraries(test_router PRIVATE elaina::core)
add_test(NAME router COMMAND test_router)

add_executable(test_http_codec test_http_codec.cpp)
target_link_libraries(test_http_codec PRIVATE elaina::core)
add_test(NAME http_codec COMMAND test_http_codec)

add_executable(test_integration test_integration.cpp)
target_link_libraries(test_integration PRIVATE elaina::core)
add_test(NAME integration COMMAND test_integration)
