cmake_minimum_required(VERSION 3.10)
project(miniclaw VERSION 0.1.0 LANGUAGES C CXX ASM)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(ANDROID)
    set(PTHREAD_LIB "")
    set(BLAS_LAPACK_LIBS openblas)
else()
    set(PTHREAD_LIB pthread)
    set(BLAS_LAPACK_LIBS blas lapack)
endif()

if(ANDROID)
    set(ANDROID_LIBS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third-party/libcurl-android/jni/build")
    set(OPENSSL_ROOT_DIR "${ANDROID_LIBS_DIR}/openssl/arm64-v8a")
    set(OPENSSL_INCLUDE_DIR "${OPENSSL_ROOT_DIR}/include")
    set(OPENSSL_CRYPTO_LIBRARY "${OPENSSL_ROOT_DIR}/lib/libcrypto.a")
    set(OPENSSL_SSL_LIBRARY "${OPENSSL_ROOT_DIR}/lib/libssl.a")
    
    set(CURL_INCLUDE_DIR "${ANDROID_LIBS_DIR}/curl/arm64-v8a/include")
    set(CURL_LIBRARY "${ANDROID_LIBS_DIR}/curl/arm64-v8a/lib/libcurl.a")
    set(ZLIB_INCLUDE_DIR "${ANDROID_LIBS_DIR}/zlib/arm64-v8a/include")
    set(ZLIB_LIBRARY "${ANDROID_LIBS_DIR}/zlib/arm64-v8a/lib/libz.a")
    
    # Manually define targets for find_package(CURL) if it fails
    add_library(CURL::libcurl STATIC IMPORTED)
    set_target_properties(CURL::libcurl PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${CURL_INCLUDE_DIR}"
        IMPORTED_LOCATION "${CURL_LIBRARY}"
    )
    
    add_library(OpenSSL::Crypto STATIC IMPORTED)
    set_target_properties(OpenSSL::Crypto PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${OPENSSL_INCLUDE_DIR}"
        IMPORTED_LOCATION "${OPENSSL_CRYPTO_LIBRARY}"
    )
    
    add_library(OpenSSL::SSL STATIC IMPORTED)
    set_target_properties(OpenSSL::SSL PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${OPENSSL_INCLUDE_DIR}"
        IMPORTED_LOCATION "${OPENSSL_SSL_LIBRARY}"
    )
    
    add_library(ZLIB::ZLIB STATIC IMPORTED)
    set_target_properties(ZLIB::ZLIB PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${ZLIB_INCLUDE_DIR}"
        IMPORTED_LOCATION "${ZLIB_LIBRARY}"
    )
else()
    find_package(OpenSSL REQUIRED)
    find_package(CURL REQUIRED)
    find_package(ZLIB REQUIRED)
endif()

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
# OpenBLAS (for Android)
if(ANDROID)
    set(OPENBLAS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third-party/openblas-android")
    # Link to the precompiled OpenBLAS built by build_deps_android.sh
    add_library(openblas STATIC IMPORTED)
    set_target_properties(openblas PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${OPENBLAS_ROOT}/include"
        IMPORTED_LOCATION "${OPENBLAS_ROOT}/lib/libopenblas.a"
    )
    set(BLAS_LIBRARIES openblas CACHE STRING "" FORCE)
    set(LAPACK_LIBRARIES openblas CACHE STRING "" FORCE)
endif()

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

option(USE_SQLITE "Use SQLite FTS5 instead of LucenePlusPlus" OFF)
if(USE_SQLITE)
    add_compile_definitions(USE_SQLITE)
endif()

if(USE_SQLITE)
    # Fetch SQLite amalgamation
    FetchContent_Declare(
      sqlite3
      URL      https://www.sqlite.org/2024/sqlite-amalgamation-3460000.zip
    )
    FetchContent_MakeAvailable(sqlite3)
    
    # Define a static library for SQLite with FTS5 enabled
    add_library(sqlite3_lib STATIC ${sqlite3_SOURCE_DIR}/sqlite3.c)
    target_include_directories(sqlite3_lib PUBLIC ${sqlite3_SOURCE_DIR})
    target_compile_definitions(sqlite3_lib PRIVATE SQLITE_ENABLE_FTS5)
    set(SEARCH_INCLUDES ${sqlite3_SOURCE_DIR})
    set(SEARCH_LIBS sqlite3_lib)
else()
    set(LUCENE_DIR "${THIRDPARTY_DIR}/LucenePlusPlus")
    set(SEARCH_INCLUDES ${LUCENE_DIR}/include)
    set(SEARCH_LIBS 
        ${LUCENE_DIR}/lib/liblucene++.a 
        ${LUCENE_DIR}/lib/liblucene++-contrib.a
    )
endif()


if(NOT ANDROID)
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
endif()

if(WIN32)
    include_directories(${CMAKE_CURRENT_SOURCE_DIR}/third-party/fiber/include-win64)
else()
    include_directories(${CMAKE_CURRENT_SOURCE_DIR}/third-party/fiber/include)
endif()

# Platform-specific fiber library
set(FIBER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third-party/fiber")
if(WIN32)
    set(FIBER_LIBS Boost::fiber Boost::context)
else()
    # Build libfiber from source for all other platforms
    add_library(fiber_lib STATIC 
        ${FIBER_DIR}/fiber.c
        ${FIBER_DIR}/context.S
    )
    target_include_directories(fiber_lib PUBLIC 
        ${FIBER_DIR}/include
        ${FIBER_DIR}
    )
    set(FIBER_LIBS fiber_lib)
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
    src/agent/cron_service.cpp
)

if(ANDROID)
    target_sources(miniclaw PRIVATE src/agent/lapack_android_stub.c)
endif()

target_include_directories(miniclaw PRIVATE 
    src
    ${EXTERNAL_DIR}/spdlog/include
    ${libuv_SOURCE_DIR}/include
    ${uwebsockets_SOURCE_DIR}/src
    ${usockets_SOURCE_DIR}/src
    ${yaml-cpp_SOURCE_DIR}/include
    ${simdjson_SOURCE_DIR}/include
    ${simdjson_SOURCE_DIR}/src
    ${THIRDPARTY_DIR}/croncpp
)

# Definitions for libfib / uWS
target_compile_definitions(miniclaw PRIVATE 
    __1_N_MODEL__
)


target_include_directories(miniclaw PRIVATE ${SEARCH_INCLUDES})

target_link_libraries(miniclaw PRIVATE 
    uSockets
    OpenSSL::SSL 
    OpenSSL::Crypto
    CURL::libcurl
    ZLIB::ZLIB
    uv_a
    ${FIBER_LIBS}
    faiss
    yaml-cpp
    simdjson
)

# (Search libs included in platform blocks)

if(APPLE)
    target_link_libraries(miniclaw PRIVATE 
        ${SEARCH_LIBS}
        "-framework Accelerate"
        "/opt/homebrew/opt/libomp/lib/libomp.dylib"
    )
    target_link_libraries(miniclaw PRIVATE boost_filesystem boost_iostreams boost_thread boost_date_time boost_regex)
elseif(WIN32)
    target_link_libraries(miniclaw PRIVATE 
        ${SEARCH_LIBS}
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
        ${SEARCH_LIBS}
        OpenMP::OpenMP_CXX
    )
    target_link_libraries(miniclaw PRIVATE ${BLAS_LAPACK_LIBS})
    if(NOT ANDROID)
        target_link_libraries(miniclaw PRIVATE ${Boost_LIBRARIES})
    endif()
endif()

if(NOT WIN32)
    add_executable(test_fiber tests/test_fiber.cpp)
    target_include_directories(test_fiber PRIVATE 
        src
        ${libuv_SOURCE_DIR}/include
        ${THIRDPARTY_DIR}/croncpp
    )
    target_link_libraries(test_fiber PRIVATE 
        ${PTHREAD_LIB}
        CURL::libcurl
        simdjson
        uv_a
        ${FIBER_LIBS}
    )
endif()

add_executable(test_agent_loop tests/test_agent_loop.cpp src/agent/memory_index.cpp)
target_include_directories(test_agent_loop PRIVATE 
    src
    ${EXTERNAL_DIR}/spdlog/include
    ${libuv_SOURCE_DIR}/include
    ${SEARCH_INCLUDES}
    ${yaml-cpp_SOURCE_DIR}/include
    ${simdjson_SOURCE_DIR}/include
    ${simdjson_SOURCE_DIR}/src
    ${uwebsockets_SOURCE_DIR}/src
    ${usockets_SOURCE_DIR}/src
    ${THIRDPARTY_DIR}/croncpp
)
target_link_libraries(test_agent_loop PRIVATE 
    ${PTHREAD_LIB}
    CURL::libcurl
    yaml-cpp
    simdjson
    uv_a
    ${FIBER_LIBS}
    faiss
)

if(APPLE)
    target_link_libraries(test_agent_loop PRIVATE 
        ${SEARCH_LIBS}
        "-framework Accelerate"
        "/opt/homebrew/opt/libomp/lib/libomp.dylib"
    )
    target_link_libraries(test_agent_loop PRIVATE boost_filesystem boost_iostreams boost_thread boost_date_time boost_regex)
elseif(WIN32)
    target_link_libraries(test_agent_loop PRIVATE 
        ${SEARCH_LIBS}
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
        ${SEARCH_LIBS}
        OpenMP::OpenMP_CXX
        ${BLAS_LAPACK_LIBS}
    )
    target_link_libraries(test_agent_loop PRIVATE ${Boost_LIBRARIES})
endif()

add_executable(test_memory_index tests/test_memory_index.cpp src/agent/memory_index.cpp)
target_include_directories(test_memory_index PRIVATE 
    src
    ${SEARCH_INCLUDES}
    ${EXTERNAL_DIR}/spdlog/include
    ${libuv_SOURCE_DIR}/include
    ${uwebsockets_SOURCE_DIR}/src
    ${usockets_SOURCE_DIR}/src
    ${THIRDPARTY_DIR}/croncpp
)
target_link_libraries(test_memory_index PRIVATE 
    CURL::libcurl
    ${PTHREAD_LIB}
    yaml-cpp
    simdjson
    uv_a
    ${FIBER_LIBS}
    faiss
)

if(APPLE)
    target_link_libraries(test_memory_index PRIVATE 
        ${SEARCH_LIBS}
        "-framework Accelerate"
        "/opt/homebrew/opt/libomp/lib/libomp.dylib"
    )
    target_link_libraries(test_memory_index PRIVATE boost_filesystem boost_iostreams boost_thread boost_date_time boost_regex)
elseif(WIN32)
    target_link_libraries(test_memory_index PRIVATE 
        ${SEARCH_LIBS}
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
        ${SEARCH_LIBS}
        OpenMP::OpenMP_CXX
        ${BLAS_LAPACK_LIBS}
    )
    target_link_libraries(test_memory_index PRIVATE ${Boost_LIBRARIES})
endif()

add_executable(test_distillation tests/test_distillation.cpp src/agent/memory_index.cpp)
target_include_directories(test_distillation PRIVATE 
    src
    ${EXTERNAL_DIR}/spdlog/include
    ${libuv_SOURCE_DIR}/include
    ${SEARCH_INCLUDES}
    ${yaml-cpp_SOURCE_DIR}/include
    ${simdjson_SOURCE_DIR}/include
    ${simdjson_SOURCE_DIR}/src
    ${THIRDPARTY_DIR}/croncpp
)
target_link_libraries(test_distillation PRIVATE 
    ${PTHREAD_LIB}
    CURL::libcurl
    yaml-cpp
    simdjson
    uv_a
    ${FIBER_LIBS}
    faiss
)

if(APPLE)
    target_link_libraries(test_distillation PRIVATE 
        ${SEARCH_LIBS}
        "-framework Accelerate"
        "/opt/homebrew/opt/libomp/lib/libomp.dylib"
    )
    target_link_libraries(test_distillation PRIVATE boost_filesystem boost_iostreams boost_thread boost_date_time boost_regex)
elseif(WIN32)
    target_link_libraries(test_distillation PRIVATE 
        ${SEARCH_LIBS}
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
        ${SEARCH_LIBS}
        OpenMP::OpenMP_CXX
        ${BLAS_LAPACK_LIBS}
    )
    target_link_libraries(test_distillation PRIVATE ${Boost_LIBRARIES})
endif()

add_executable(test_distillation_real tests/test_distillation_real.cpp src/agent.cpp src/agent/fiber_pool.cpp src/agent/memory_index.cpp src/agent/shutdown.cpp src/agent/cron_service.cpp)
target_include_directories(test_distillation_real PRIVATE 
    src
    ${EXTERNAL_DIR}/spdlog/include
    ${libuv_SOURCE_DIR}/include
    ${SEARCH_INCLUDES}
    ${uwebsockets_SOURCE_DIR}/src
    ${usockets_SOURCE_DIR}/src
    ${yaml-cpp_SOURCE_DIR}/include
    ${simdjson_SOURCE_DIR}/include
    ${simdjson_SOURCE_DIR}/src
    ${THIRDPARTY_DIR}/croncpp
)
target_link_libraries(test_distillation_real PRIVATE 
    uSockets
    OpenSSL::SSL 
    OpenSSL::Crypto
    CURL::libcurl
    ZLIB::ZLIB
    z
    ${PTHREAD_LIB}
    yaml-cpp
    simdjson
    uv_a
    ${FIBER_LIBS}
    faiss
)

if(APPLE)
    target_link_libraries(test_distillation_real PRIVATE 
        ${SEARCH_LIBS}
        "-framework Accelerate"
        "/opt/homebrew/opt/libomp/lib/libomp.dylib"
    )
    target_link_libraries(test_distillation_real PRIVATE boost_filesystem boost_iostreams boost_thread boost_date_time boost_regex)
elseif(WIN32)
    target_link_libraries(test_distillation_real PRIVATE 
        ${SEARCH_LIBS}
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
        ${SEARCH_LIBS}
        OpenMP::OpenMP_CXX
        ${BLAS_LAPACK_LIBS}
    )
    target_link_libraries(test_distillation_real PRIVATE ${Boost_LIBRARIES})
endif()

add_executable(test_gmail tests/test_gmail.cpp src/agent.cpp src/agent/fiber_pool.cpp src/agent/memory_index.cpp src/agent/shutdown.cpp src/agent/cron_service.cpp)
target_include_directories(test_gmail PRIVATE 
    src
    ${EXTERNAL_DIR}/spdlog/include
    ${libuv_SOURCE_DIR}/include
    ${SEARCH_INCLUDES}
    ${uwebsockets_SOURCE_DIR}/src
    ${usockets_SOURCE_DIR}/src
    ${yaml-cpp_SOURCE_DIR}/include
    ${simdjson_SOURCE_DIR}/include
    ${simdjson_SOURCE_DIR}/src
    ${THIRDPARTY_DIR}/croncpp
)
target_link_libraries(test_gmail PRIVATE 
    uSockets
    OpenSSL::SSL 
    OpenSSL::Crypto
    CURL::libcurl
    ZLIB::ZLIB
    z
    ${PTHREAD_LIB}
    yaml-cpp
    simdjson
    uv_a
    ${FIBER_LIBS}
    faiss
)

if(APPLE)
    target_link_libraries(test_gmail PRIVATE 
        ${SEARCH_LIBS}
        "-framework Accelerate"
        "/opt/homebrew/opt/libomp/lib/libomp.dylib"
    )
    target_link_libraries(test_gmail PRIVATE boost_filesystem boost_iostreams boost_thread boost_date_time boost_regex)
elseif(WIN32)
    target_link_libraries(test_gmail PRIVATE 
        ${SEARCH_LIBS}
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
        ${SEARCH_LIBS}
        OpenMP::OpenMP_CXX
        ${BLAS_LAPACK_LIBS}
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
target_link_libraries(test_busybox PRIVATE ${PTHREAD_LIB} yaml-cpp simdjson)
if(WIN32)
    target_link_libraries(test_busybox PRIVATE ws2_32 iphlpapi userenv dbghelp)
    target_link_libraries(test_busybox PRIVATE ${Boost_LIBRARIES})
endif()



