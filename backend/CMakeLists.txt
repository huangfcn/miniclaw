cmake_minimum_required(VERSION 3.14)
project(miniclaw VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find OpenSSL, CURL, and ZLIB
find_package(OpenSSL REQUIRED)
find_package(CURL REQUIRED)
find_package(ZLIB REQUIRED)

include(FetchContent)

# uSockets
FetchContent_Declare(
  uSockets
  GIT_REPOSITORY https://github.com/uNetworking/uSockets.git
  GIT_TAG        v0.8.8
)
FetchContent_MakeAvailable(uSockets)

# uWebSockets
FetchContent_Declare(
  uWebSockets
  GIT_REPOSITORY https://github.com/uNetworking/uWebSockets.git
  GIT_TAG        v20.70.0
)
FetchContent_MakeAvailable(uWebSockets)

# yaml-cpp
FetchContent_Declare(
  yaml-cpp
  GIT_REPOSITORY https://github.com/jbeder/yaml-cpp.git
  GIT_TAG        0.8.0
)
FetchContent_MakeAvailable(yaml-cpp)

# simdjson
FetchContent_Declare(
  simdjson
  GIT_REPOSITORY https://github.com/simdjson/simdjson.git
  GIT_TAG        v3.10.1
)
FetchContent_MakeAvailable(simdjson)

# Define include directories for external libraries
set(EXTERNAL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external")

# Define uSockets as a static library
add_library(uSockets STATIC 
    ${usockets_SOURCE_DIR}/src/bsd.c
    ${usockets_SOURCE_DIR}/src/context.c
    ${usockets_SOURCE_DIR}/src/loop.c
    ${usockets_SOURCE_DIR}/src/socket.c
    ${usockets_SOURCE_DIR}/src/udp.c
    ${usockets_SOURCE_DIR}/src/eventing/libuv.c
)

target_compile_definitions(uSockets PRIVATE 
    LIBUS_USE_LIBUV
    LIBUS_NO_SSL
)

target_include_directories(uSockets PRIVATE 
    ${usockets_SOURCE_DIR}/src
    ${EXTERNAL_DIR}/libuv/include
)

set_target_properties(uSockets PROPERTIES LINKER_LANGUAGE C)

add_executable(miniclaw 
    src/main.cpp 
    src/agent.cpp
    src/agent/fiber_pool.cpp
)

target_include_directories(miniclaw PRIVATE 
    src
    ${EXTERNAL_DIR}/json
    ${EXTERNAL_DIR}/spdlog/include
    ${EXTERNAL_DIR}/libuv/include
    ${CMAKE_CURRENT_SOURCE_DIR}/include/fiber
    ${uwebsockets_SOURCE_DIR}/src
    ${usockets_SOURCE_DIR}/src
    ${yaml-cpp_SOURCE_DIR}/include
    ${simdjson_SOURCE_DIR}/include
    ${simdjson_SOURCE_DIR}/src
)

# Definitions for libfib / uWS
target_compile_definitions(miniclaw PRIVATE 
    __1_N_MODEL__
)

target_link_libraries(miniclaw PRIVATE 
    uSockets
    OpenSSL::SSL 
    OpenSSL::Crypto
    CURL::libcurl
    ZLIB::ZLIB
    z
    pthread
    yaml-cpp
    simdjson
    ${EXTERNAL_DIR}/libuv/lib/libuv.a
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/libfiber.a
)

add_executable(test_fiber tests/test_fiber.cpp)
target_include_directories(test_fiber PRIVATE 
    ${EXTERNAL_DIR}/libuv/include
    ${CMAKE_CURRENT_SOURCE_DIR}/include/fiber
)
target_link_libraries(test_fiber PRIVATE 
    pthread
    CURL::libcurl
    simdjson
    ${EXTERNAL_DIR}/libuv/lib/libuv.a
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/libfiber.a
)
