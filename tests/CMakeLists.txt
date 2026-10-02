add_executable(test_router test_router.cpp)
target_link_libraries(test_router PRIVATE elaina::core)
add_test(NAME router COMMAND test_router)

add_executable(test_http_codec test_http_codec.cpp)
target_link_libraries(test_http_codec PRIVATE elaina::core)
add_test(NAME http_codec COMMAND test_http_codec)

add_executable(test_integration test_integration.cpp)
target_link_libraries(test_integration PRIVATE elaina::core)
add_test(NAME integration COMMAND test_integration)

add_executable(test_rest test_rest.cpp)
target_link_libraries(test_rest PRIVATE elaina::core)
add_test(NAME rest COMMAND test_rest)

add_executable(test_tls test_tls.cpp)
target_link_libraries(test_tls PRIVATE elaina::core)
add_test(NAME tls COMMAND test_tls)
