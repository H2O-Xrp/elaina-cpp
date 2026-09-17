cmake_minimum_required(VERSION 3.25)
project(elaina-cpp VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(ELAINA_BUILD_EXAMPLES "Build examples" ON)
option(ELAINA_BUILD_TESTS "Build tests" ON)
option(ELAINA_BUILD_BENCHMARKS "Build benchmarks" ON)
option(ELAINA_WARNINGS_AS_ERRORS "Warnings as errors" OFF)

add_library(elaina_core
  src/router.cpp
  src/http_codec.cpp
  src/middleware.cpp
  src/reactor.cpp
  src/connection.cpp
  src/server.cpp
  src/json_mini.cpp
)
add_library(elaina::core ALIAS elaina_core)
target_include_directories(elaina_core PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/modules>
  $<INSTALL_INTERFACE:include>
)
find_package(Threads REQUIRED)
target_link_libraries(elaina_core PUBLIC Threads::Threads)

target_compile_options(elaina_core PRIVATE -Wall -Wextra -Wpedantic)
if(ELAINA_WARNINGS_AS_ERRORS)
  target_compile_options(elaina_core PRIVATE -Werror)
endif()
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  target_compile_options(elaina_core PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(elaina_core PUBLIC -fsanitize=address,undefined)
endif()

if(ELAINA_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()
if(ELAINA_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
if(ELAINA_BUILD_BENCHMARKS)
  add_subdirectory(benchmarks)
endif()

install(DIRECTORY include/elaina modules/json
  DESTINATION include
  FILES_MATCHING PATTERN "*.hpp"
)
install(TARGETS elaina_core EXPORT elaina-targets)
install(EXPORT elaina-targets NAMESPACE elaina:: DESTINATION lib/cmake/elaina)
