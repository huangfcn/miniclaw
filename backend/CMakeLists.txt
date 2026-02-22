cmake_minimum_required(VERSION 3.10)
project(miniclaw VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find OpenSSL, CURL, and ZLIB
find_package(OpenSSL REQUIRED)
find_package(CURL REQUIRED)
find_package(ZLIB REQUIRED)
if(APPLE)
    set(BOOST_INC "/opt/homebrew/opt/boost/include")
    set(BOOST_LIB "/opt/homebrew/opt/boost/lib")
    include_directories(${BOOST_INC})
    link_directories(${BOOST_LIB})
else()
    find_package(Boost REQUIRED COMPONENTS filesystem iostreams thread date_time regex)
    include_directories(${Boost_INCLUDE_DIRS})
endif()

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
set(THIRDPARTY_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third-party")

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
    ${LIBUV_DIR}/include
)

# Third-party libraries (prebuilt)
set(FAISS_DIR "${THIRDPARTY_DIR}/faiss")
set(LUCENE_DIR "${THIRDPARTY_DIR}/LucenePlusPlus")
set(FIBER_DIR "${THIRDPARTY_DIR}/fiber")
set(LIBUV_DIR "${THIRDPARTY_DIR}/libuv")

set_target_properties(uSockets PROPERTIES LINKER_LANGUAGE C)

add_executable(miniclaw 
    src/main.cpp 
    src/agent.cpp
    src/agent/fiber_pool.cpp
    src/agent/memory_index.cpp
)

target_include_directories(miniclaw PRIVATE 
    src
    ${EXTERNAL_DIR}/spdlog/include
    ${LIBUV_DIR}/include
    ${FIBER_DIR}/include
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


target_include_directories(miniclaw PRIVATE 
    ${FAISS_DIR}/include
    ${LUCENE_DIR}/include
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
    ${LIBUV_DIR}/lib/libuv.a
    ${FIBER_DIR}/lib/libfiber.a
    ${FAISS_DIR}/lib/libfaiss.a
)

if(APPLE)
    target_link_libraries(miniclaw PRIVATE 
        ${LUCENE_DIR}/lib/liblucene++.dylib
        ${LUCENE_DIR}/lib/liblucene++-contrib.dylib
        "-framework Accelerate"
        "/opt/homebrew/opt/libomp/lib/libomp.dylib"
    )
    target_link_libraries(miniclaw PRIVATE boost_filesystem boost_iostreams boost_thread boost_date_time boost_regex)
elseif(WIN32)
    target_link_libraries(miniclaw PRIVATE 
        ${LUCENE_DIR}/lib/liblucene++.a
        ${LUCENE_DIR}/lib/liblucene++-contrib.a
        openblas
        ws2_32
        iphlpapi
        userenv
        dbghelp
        gomp
    )
    target_link_libraries(miniclaw PRIVATE ${Boost_LIBRARIES})
else()
    target_link_libraries(miniclaw PRIVATE 
        ${LUCENE_DIR}/lib/liblucene++.so
        ${LUCENE_DIR}/lib/liblucene++-contrib.so
    )
    target_link_libraries(miniclaw PRIVATE ${Boost_LIBRARIES})
endif()

add_executable(test_fiber tests/test_fiber.cpp)
    target_include_directories(test_fiber PRIVATE 
    ${LIBUV_DIR}/include
    ${FIBER_DIR}/include
)
target_link_libraries(test_fiber PRIVATE 
    pthread
    CURL::libcurl
    simdjson
    ${LIBUV_DIR}/lib/libuv.a
    ${FIBER_DIR}/lib/libfiber.a
)

if(WIN32)
    target_link_libraries(test_fiber PRIVATE 
        ws2_32
        iphlpapi
        userenv
        dbghelp
        gomp
    )
    target_link_libraries(test_fiber PRIVATE ${Boost_LIBRARIES})
endif()

add_executable(test_agent_loop tests/test_agent_loop.cpp src/agent/memory_index.cpp)
target_include_directories(test_agent_loop PRIVATE 
    src
    ${EXTERNAL_DIR}/spdlog/include
    ${LIBUV_DIR}/include
    ${FIBER_DIR}/include
    ${FAISS_DIR}/include
    ${LUCENE_DIR}/include
    ${yaml-cpp_SOURCE_DIR}/include
    ${simdjson_SOURCE_DIR}/include
    ${simdjson_SOURCE_DIR}/src
)
target_link_libraries(test_agent_loop PRIVATE 
    pthread
    CURL::libcurl
    yaml-cpp
    simdjson
    ${LIBUV_DIR}/lib/libuv.a
    ${FIBER_DIR}/lib/libfiber.a
    ${FAISS_DIR}/lib/libfaiss.a
)

if(APPLE)
    target_link_libraries(test_agent_loop PRIVATE 
        ${LUCENE_DIR}/lib/liblucene++.dylib
        ${LUCENE_DIR}/lib/liblucene++-contrib.dylib
        "-framework Accelerate"
        "/opt/homebrew/opt/libomp/lib/libomp.dylib"
    )
    target_link_libraries(test_agent_loop PRIVATE boost_filesystem boost_iostreams boost_thread boost_date_time boost_regex)
elseif(WIN32)
    target_link_libraries(test_agent_loop PRIVATE 
        ${LUCENE_DIR}/lib/liblucene++.a
        ${LUCENE_DIR}/lib/liblucene++-contrib.a
        openblas
        ws2_32
        iphlpapi
        userenv
        dbghelp
        gomp
    )
    target_link_libraries(test_agent_loop PRIVATE ${Boost_LIBRARIES})
endif()

add_executable(test_memory_index tests/test_memory_index.cpp src/agent/memory_index.cpp)
target_include_directories(test_memory_index PRIVATE 
    src
    ${FAISS_DIR}/include
    ${LUCENE_DIR}/include
    ${EXTERNAL_DIR}/spdlog/include
    ${LIBUV_DIR}/include
    ${FIBER_DIR}/include
)
target_link_libraries(test_memory_index PRIVATE 
    pthread
    CURL::libcurl
    yaml-cpp
    simdjson
    ${LIBUV_DIR}/lib/libuv.a
    ${FIBER_DIR}/lib/libfiber.a
    ${FAISS_DIR}/lib/libfaiss.a
)

if(APPLE)
    target_link_libraries(test_memory_index PRIVATE 
        ${LUCENE_DIR}/lib/liblucene++.dylib
        ${LUCENE_DIR}/lib/liblucene++-contrib.dylib
        "-framework Accelerate"
        "/opt/homebrew/opt/libomp/lib/libomp.dylib"
    )
    target_link_libraries(test_memory_index PRIVATE boost_filesystem boost_iostreams boost_thread boost_date_time boost_regex)
elseif(WIN32)
    target_link_libraries(test_memory_index PRIVATE 
        ${LUCENE_DIR}/lib/liblucene++.a
        ${LUCENE_DIR}/lib/liblucene++-contrib.a
        openblas
        ws2_32
        iphlpapi
        userenv
        dbghelp
        gomp
    )
    target_link_libraries(test_memory_index PRIVATE ${Boost_LIBRARIES})
endif()

add_executable(test_distillation tests/test_distillation.cpp src/agent/memory_index.cpp)
target_include_directories(test_distillation PRIVATE 
    src
    ${EXTERNAL_DIR}/spdlog/include
    ${LIBUV_DIR}/include
    ${FIBER_DIR}/include
    ${FAISS_DIR}/include
    ${LUCENE_DIR}/include
    ${yaml-cpp_SOURCE_DIR}/include
    ${simdjson_SOURCE_DIR}/include
    ${simdjson_SOURCE_DIR}/src
)
target_link_libraries(test_distillation PRIVATE 
    pthread
    CURL::libcurl
    yaml-cpp
    simdjson
    ${LIBUV_DIR}/lib/libuv.a
    ${FIBER_DIR}/lib/libfiber.a
    ${FAISS_DIR}/lib/libfaiss.a
)

if(APPLE)
    target_link_libraries(test_distillation PRIVATE 
        ${LUCENE_DIR}/lib/liblucene++.dylib
        ${LUCENE_DIR}/lib/liblucene++-contrib.dylib
        "-framework Accelerate"
        "/opt/homebrew/opt/libomp/lib/libomp.dylib"
    )
    target_link_libraries(test_distillation PRIVATE boost_filesystem boost_iostreams boost_thread boost_date_time boost_regex)
elseif(WIN32)
    target_link_libraries(test_distillation PRIVATE 
        ${LUCENE_DIR}/lib/liblucene++.a
        ${LUCENE_DIR}/lib/liblucene++-contrib.a
        openblas
        ws2_32
        iphlpapi
        userenv
        dbghelp
        gomp
    )
    target_link_libraries(test_distillation PRIVATE ${Boost_LIBRARIES})
endif()
