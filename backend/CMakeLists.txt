cmake_minimum_required(VERSION 3.14)
project(miniclaw VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find OpenSSL for httplib
find_package(OpenSSL REQUIRED)

# Define include directories for external libraries
set(EXTERNAL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external")

add_executable(miniclaw 
    src/main.cpp 
    src/agent.cpp
)

target_include_directories(miniclaw PRIVATE 
    src
    ${EXTERNAL_DIR}/json
    ${EXTERNAL_DIR}/cpp-httplib
    ${EXTERNAL_DIR}/spdlog/include
    ${EXTERNAL_DIR}/Crow/include
    ${EXTERNAL_DIR}/asio/include
    ${EXTERNAL_DIR}/libuv/include
    ${CMAKE_CURRENT_SOURCE_DIR}/include/fiber
)

# Definitions for Crow/httplib/fiber
target_compile_definitions(miniclaw PRIVATE 
    CPPHTTPLIB_OPENSSL_SUPPORT
    ASIO_STANDALONE
    __1_N_MODEL__
)

target_link_libraries(miniclaw PRIVATE 
    OpenSSL::SSL 
    OpenSSL::Crypto
    pthread
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
    ${EXTERNAL_DIR}/libuv/lib/libuv.a
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/libfiber.a
)
