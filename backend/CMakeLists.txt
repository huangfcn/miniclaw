cmake_minimum_required(VERSION 3.10)
project(miniclaw VERSION 0.1.0 LANGUAGES C CXX)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find OpenSSL, CURL, and ZLIB
find_package(OpenSSL REQUIRED)
find_package(CURL REQUIRED)
find_package(ZLIB REQUIRED)

if(APPLE AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    # Tell CMake where to find OpenMP for Apple Clang
    set(OpenMP_C_FLAGS "-Xpreprocessor -fopenmp -I/opt/homebrew/opt/libomp/include")
    set(OpenMP_C_LIB_NAMES "omp")
    set(OpenMP_omp_LIBRARY "/opt/homebrew/opt/libomp/lib/libomp.dylib")
    
    set(OpenMP_CXX_FLAGS "-Xpreprocessor -fopenmp -I/opt/homebrew/opt/libomp/include")
    set(OpenMP_CXX_LIB_NAMES "omp")
endif()

find_package(OpenMP REQUIRED)
include(FetchContent)


# Define directories early for all targets
set(EXTERNAL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external")
set(THIRDPARTY_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third-party")
# Faiss (build from source)
set(FAISS_ENABLE_GPU OFF CACHE BOOL "" FORCE)
set(FAISS_ENABLE_PYTHON OFF CACHE BOOL "" FORCE)
set(FAISS_OPT_LEVEL "generic" CACHE STRING "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
  faiss
  GIT_REPOSITORY https://github.com/facebookresearch/faiss.git
  GIT_TAG        v1.8.0
  PATCH_COMMAND bash -c "git apply --check ${CMAKE_CURRENT_SOURCE_DIR}/third-party/faiss-msys2.patch 2>/dev/null && git apply ${CMAKE_CURRENT_SOURCE_DIR}/third-party/faiss-msys2.patch || echo 'Patch already applied or failed'"
)
FetchContent_MakeAvailable(faiss)

set(LUCENE_DIR "${THIRDPARTY_DIR}/LucenePlusPlus")


if(APPLE)
    set(BOOST_INC "/opt/homebrew/opt/boost/include")
    set(BOOST_LIB "/opt/homebrew/opt/boost/lib")
    include_directories(${BOOST_INC})
    link_directories(${BOOST_LIB})
else()
    if(WIN32)
        find_package(Boost REQUIRED COMPONENTS filesystem iostreams thread date_time regex fiber context)
    else()
        find_package(Boost REQUIRED COMPONENTS filesystem iostreams thread date_time regex)
    endif()
    include_directories(${Boost_INCLUDE_DIRS})
endif()

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/third-party/fiber/include)

# Platform-specific fiber library
set(FIBER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third-party/fiber")
if(WIN32)
    set(FIBER_LIBS Boost::fiber Boost::context)
else()
    set(FIBER_LIBS ${FIBER_DIR}/lib/libfiber.a)
endif()


# libuv (build from source)
FetchContent_Declare(
  libuv
  GIT_REPOSITORY https://github.com/libuv/libuv.git
  GIT_TAG        v1.52.0
)
set(LIBUV_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LIBUV_BUILD_BENCH OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(libuv)

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
    ${libuv_SOURCE_DIR}/include
)
target_link_libraries(uSockets PRIVATE uv_a)


set_target_properties(uSockets PROPERTIES LINKER_LANGUAGE C)

add_executable(miniclaw 
    src/main.cpp 
    src/agent.cpp
    src/agent/fiber_pool.cpp
    src/agent/memory_index.cpp
    src/agent/shutdown.cpp
)

target_include_directories(miniclaw PRIVATE 
    src
    ${EXTERNAL_DIR}/spdlog/include
    ${libuv_SOURCE_DIR}/include
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
    uv_a
    ${FIBER_LIBS}
    faiss
)

if(APPLE)
    target_link_libraries(miniclaw PRIVATE 
        ${LUCENE_DIR}/lib/liblucene++.a
        ${LUCENE_DIR}/lib/liblucene++-contrib.a
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
        ${LUCENE_DIR}/lib/liblucene++.a
        ${LUCENE_DIR}/lib/liblucene++-contrib.a
        OpenMP::OpenMP_CXX
        blas
        lapack
    )
    target_link_libraries(miniclaw PRIVATE ${Boost_LIBRARIES})
endif()

add_executable(test_fiber tests/test_fiber.cpp)
    target_include_directories(test_fiber PRIVATE 
    src
    ${libuv_SOURCE_DIR}/include
)
target_link_libraries(test_fiber PRIVATE 
    pthread
    CURL::libcurl
    simdjson
    uv_a
    ${FIBER_LIBS}
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
    ${libuv_SOURCE_DIR}/include
    ${LUCENE_DIR}/include
    ${yaml-cpp_SOURCE_DIR}/include
    ${simdjson_SOURCE_DIR}/include
    ${simdjson_SOURCE_DIR}/src
    ${uwebsockets_SOURCE_DIR}/src
    ${usockets_SOURCE_DIR}/src
)
target_link_libraries(test_agent_loop PRIVATE 
    pthread
    CURL::libcurl
    yaml-cpp
    simdjson
    uv_a
    ${FIBER_LIBS}
    faiss
)

if(APPLE)
    target_link_libraries(test_agent_loop PRIVATE 
        ${LUCENE_DIR}/lib/liblucene++.a
        ${LUCENE_DIR}/lib/liblucene++-contrib.a
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
else()
    target_link_libraries(test_agent_loop PRIVATE 
        ${LUCENE_DIR}/lib/liblucene++.a
        ${LUCENE_DIR}/lib/liblucene++-contrib.a
        OpenMP::OpenMP_CXX
        blas
        lapack
    )
    target_link_libraries(test_agent_loop PRIVATE ${Boost_LIBRARIES})
endif()

add_executable(test_memory_index tests/test_memory_index.cpp src/agent/memory_index.cpp)
target_include_directories(test_memory_index PRIVATE 
    src
    ${LUCENE_DIR}/include
    ${EXTERNAL_DIR}/spdlog/include
    ${libuv_SOURCE_DIR}/include
    ${uwebsockets_SOURCE_DIR}/src
    ${usockets_SOURCE_DIR}/src
)
target_link_libraries(test_memory_index PRIVATE 
    CURL::libcurl
    pthread
    yaml-cpp
    simdjson
    uv_a
    ${FIBER_LIBS}
    faiss
)

if(APPLE)
    target_link_libraries(test_memory_index PRIVATE 
        ${LUCENE_DIR}/lib/liblucene++.a
        ${LUCENE_DIR}/lib/liblucene++-contrib.a
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
else()
    target_link_libraries(test_memory_index PRIVATE 
        ${LUCENE_DIR}/lib/liblucene++.a
        ${LUCENE_DIR}/lib/liblucene++-contrib.a
        OpenMP::OpenMP_CXX
        blas
        lapack
    )
    target_link_libraries(test_memory_index PRIVATE ${Boost_LIBRARIES})
endif()

add_executable(test_distillation tests/test_distillation.cpp src/agent/memory_index.cpp)
target_include_directories(test_distillation PRIVATE 
    src
    ${EXTERNAL_DIR}/spdlog/include
    ${libuv_SOURCE_DIR}/include
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
    uv_a
    ${FIBER_LIBS}
    faiss
)

if(APPLE)
    target_link_libraries(test_distillation PRIVATE 
        ${LUCENE_DIR}/lib/liblucene++.a
        ${LUCENE_DIR}/lib/liblucene++-contrib.a
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
else()
    target_link_libraries(test_distillation PRIVATE 
        ${LUCENE_DIR}/lib/liblucene++.a
        ${LUCENE_DIR}/lib/liblucene++-contrib.a
        OpenMP::OpenMP_CXX
        blas
        lapack
    )
    target_link_libraries(test_distillation PRIVATE ${Boost_LIBRARIES})
endif()

add_executable(test_distillation_real tests/test_distillation_real.cpp src/agent.cpp src/agent/fiber_pool.cpp src/agent/memory_index.cpp src/agent/shutdown.cpp)
target_include_directories(test_distillation_real PRIVATE 
    src
    ${EXTERNAL_DIR}/spdlog/include
    ${libuv_SOURCE_DIR}/include
    ${LUCENE_DIR}/include
    ${uwebsockets_SOURCE_DIR}/src
    ${usockets_SOURCE_DIR}/src
    ${yaml-cpp_SOURCE_DIR}/include
    ${simdjson_SOURCE_DIR}/include
    ${simdjson_SOURCE_DIR}/src
)
target_link_libraries(test_distillation_real PRIVATE 
    uSockets
    OpenSSL::SSL 
    OpenSSL::Crypto
    CURL::libcurl
    ZLIB::ZLIB
    z
    pthread
    yaml-cpp
    simdjson
    uv_a
    ${FIBER_LIBS}
    faiss
)

if(APPLE)
    target_link_libraries(test_distillation_real PRIVATE 
        ${LUCENE_DIR}/lib/liblucene++.a
        ${LUCENE_DIR}/lib/liblucene++-contrib.a
        "-framework Accelerate"
        "/opt/homebrew/opt/libomp/lib/libomp.dylib"
    )
    target_link_libraries(test_distillation_real PRIVATE boost_filesystem boost_iostreams boost_thread boost_date_time boost_regex)
elseif(WIN32)
    target_link_libraries(test_distillation_real PRIVATE 
        ${LUCENE_DIR}/lib/liblucene++.a
        ${LUCENE_DIR}/lib/liblucene++-contrib.a
        openblas
        ws2_32
        iphlpapi
        userenv
        dbghelp
        gomp
    )
    target_link_libraries(test_distillation_real PRIVATE ${Boost_LIBRARIES})
else()
    target_link_libraries(test_distillation_real PRIVATE 
        ${LUCENE_DIR}/lib/liblucene++.a
        ${LUCENE_DIR}/lib/liblucene++-contrib.a
        OpenMP::OpenMP_CXX
        blas
        lapack
    )
    target_link_libraries(test_distillation_real PRIVATE ${Boost_LIBRARIES})
endif()

add_executable(test_gmail tests/test_gmail.cpp src/agent.cpp src/agent/fiber_pool.cpp src/agent/memory_index.cpp src/agent/shutdown.cpp)
target_include_directories(test_gmail PRIVATE 
    src
    ${EXTERNAL_DIR}/spdlog/include
    ${libuv_SOURCE_DIR}/include
    ${LUCENE_DIR}/include
    ${uwebsockets_SOURCE_DIR}/src
    ${usockets_SOURCE_DIR}/src
    ${yaml-cpp_SOURCE_DIR}/include
    ${simdjson_SOURCE_DIR}/include
    ${simdjson_SOURCE_DIR}/src
)
target_link_libraries(test_gmail PRIVATE 
    uSockets
    OpenSSL::SSL 
    OpenSSL::Crypto
    CURL::libcurl
    ZLIB::ZLIB
    z
    pthread
    yaml-cpp
    simdjson
    uv_a
    ${FIBER_LIBS}
    faiss
)

if(APPLE)
    target_link_libraries(test_gmail PRIVATE 
        ${LUCENE_DIR}/lib/liblucene++.a
        ${LUCENE_DIR}/lib/liblucene++-contrib.a
        "-framework Accelerate"
        "/opt/homebrew/opt/libomp/lib/libomp.dylib"
    )
    target_link_libraries(test_gmail PRIVATE boost_filesystem boost_iostreams boost_thread boost_date_time boost_regex)
elseif(WIN32)
    target_link_libraries(test_gmail PRIVATE 
        ${LUCENE_DIR}/lib/liblucene++.a
        ${LUCENE_DIR}/lib/liblucene++-contrib.a
        openblas
        ws2_32
        iphlpapi
        userenv
        dbghelp
        gomp
    )
    target_link_libraries(test_gmail PRIVATE ${Boost_LIBRARIES})
else()
    target_link_libraries(test_gmail PRIVATE 
        ${LUCENE_DIR}/lib/liblucene++.a
        ${LUCENE_DIR}/lib/liblucene++-contrib.a
        OpenMP::OpenMP_CXX
        blas
        lapack
    )
    target_link_libraries(test_gmail PRIVATE ${Boost_LIBRARIES})
endif()


add_executable(test_busybox tests/test_busybox.cpp)
target_include_directories(test_busybox PRIVATE 
    src 
    ${EXTERNAL_DIR}/spdlog/include
    ${yaml-cpp_SOURCE_DIR}/include
    ${simdjson_SOURCE_DIR}/include
)
target_link_libraries(test_busybox PRIVATE pthread yaml-cpp simdjson)
if(WIN32)
    target_link_libraries(test_busybox PRIVATE ws2_32 iphlpapi userenv dbghelp)
    target_link_libraries(test_busybox PRIVATE ${Boost_LIBRARIES})
endif()


