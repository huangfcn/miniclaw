cmake_minimum_required(VERSION 3.10)
project(miniclaw VERSION 0.1.0 LANGUAGES C CXX ASM)

# The main artifact is a shared library (libminiclaw_core) on every platform;
# compile all static deps (fiber_lib, libuv, uSockets, faiss, ...) with -fPIC
# so they can be linked into it. Linux enforces this at link time (R_X86_64_TPOFF32
# relocation errors); Windows/Android don't care but it's harmless.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
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
elseif(IOS)
    # The iOS SDK provides zlib (usr/lib/libz.tbd) but NOT curl, and it has no
    # OpenSSL. So: build libcurl from source (FetchContent below) against
    # SecureTransport — Apple's built-in TLS stack, the same one Apple's own
    # libcurl uses — plus SDK zlib; no OpenSSL anywhere.
    #
    # Resolve the SDK to an absolute path. CMake does this during compiler
    # detection (default: "iphoneos"); if it ever comes through as just a
    # name, resolve via xcrun ourselves.
    set(IOS_SDK_ROOT "${CMAKE_OSX_SYSROOT}")
    if(NOT IS_DIRECTORY "${IOS_SDK_ROOT}" AND IOS_SDK_ROOT MATCHES "^[A-Za-z0-9_-]+$")
        # NO_CMAKE_FIND_ROOT_PATH: cross-configures restrict searches to the
        # SDK root, but xcrun is a host tool.
        find_program(XCRUN_EXECUTABLE xcrun NO_CMAKE_FIND_ROOT_PATH)
        if(XCRUN_EXECUTABLE)
            execute_process(
                COMMAND "${XCRUN_EXECUTABLE}" --sdk "${IOS_SDK_ROOT}" --show-sdk-path
                OUTPUT_VARIABLE IOS_SDK_ROOT
                OUTPUT_STRIP_TRAILING_WHITESPACE
                RESULT_VARIABLE _xcrun_rc)
            if(NOT _xcrun_rc EQUAL 0 OR NOT IS_DIRECTORY "${IOS_SDK_ROOT}")
                unset(IOS_SDK_ROOT)
            endif()
        endif()
    endif()
    if(NOT IOS_SDK_ROOT)
        message(FATAL_ERROR "iOS build: CMAKE_OSX_SYSROOT is empty. Pass -DCMAKE_OSX_SYSROOT=$(xcrun --sdk iphoneos --show-sdk-path)")
    endif()
    get_filename_component(IOS_SDK_ROOT "${IOS_SDK_ROOT}" ABSOLUTE)
    if(NOT IS_DIRECTORY "${IOS_SDK_ROOT}")
        message(FATAL_ERROR "iOS build: could not resolve the iOS SDK (CMAKE_OSX_SYSROOT='${CMAKE_OSX_SYSROOT}'). Pass -DCMAKE_OSX_SYSROOT=$(xcrun --sdk iphoneos --show-sdk-path)")
    endif()
    message(STATUS "iOS SDK: ${IOS_SDK_ROOT}")
    set(IOS_SDK_ZLIB_TBD "${IOS_SDK_ROOT}/usr/lib/libz.tbd")
    if(NOT EXISTS "${IOS_SDK_ZLIB_TBD}")
        message(FATAL_ERROR "iOS build: ${IOS_SDK_ZLIB_TBD} not found. Is CMAKE_OSX_SYSROOT an iPhoneOS SDK? (current: ${IOS_SDK_ROOT})")
    endif()
    # UNKNOWN IMPORTED with a real LOCATION (not INTERFACE): curl's .pc-file
    # generator reads the LOCATION property and warns on interface-only targets.
    add_library(ZLIB::ZLIB UNKNOWN IMPORTED)
    set_target_properties(ZLIB::ZLIB PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${IOS_SDK_ROOT}/usr/include"
        IMPORTED_LOCATION "${IOS_SDK_ZLIB_TBD}")
    add_library(OpenSSL::SSL INTERFACE IMPORTED)
    add_library(OpenSSL::Crypto INTERFACE IMPORTED)
else()
    find_package(OpenSSL REQUIRED)
    find_package(CURL REQUIRED)
    find_package(ZLIB REQUIRED)
endif()

if(IOS)
    # iOS has no OpenMP runtime: it is not in the SDK, and Homebrew's libomp/
    # llvm are macOS-only. faiss is patched (faiss-local.patch) to treat
    # OpenMP as optional, so disable the finder and feed faiss a single-
    # threaded stub omp.h (third-party/omp-stub) instead: every #pragma omp
    # compiles to a plain sequential loop, which is fine at miniclaw's memory
    # scale. The top-level find_package below is skipped for the same reason.
    set(CMAKE_DISABLE_FIND_PACKAGE_OpenMP TRUE)
elseif(APPLE AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    # AppleClang accepts -fopenmp but ships neither omp.h nor the runtime;
    # Homebrew's libomp (brew install libomp) provides both. It is keg-only,
    # so it is NOT in AppleClang's default search path. FindOpenMP's
    # AppleClang branch locates the runtime via find_library(HINTS <implicit
    # link dirs>) and the header via find_path — both of which honor
    # CMAKE_LIBRARY_PATH / CMAKE_PREFIX_PATH. Seed those (as cache vars so the
    # faiss subproject, which calls find_package(OpenMP) itself, sees them
    # too). Setting OpenMP_*_FLAGS beforehand has no effect, which is why
    # faiss used to fail with "'omp.h' file not found".
    list(APPEND CMAKE_LIBRARY_PATH "/opt/homebrew/opt/libomp/lib")
    list(APPEND CMAKE_PREFIX_PATH "/opt/homebrew/opt/libomp")
    set(CMAKE_LIBRARY_PATH "${CMAKE_LIBRARY_PATH}" CACHE PATH "Extra lib search paths" FORCE)
    set(CMAKE_PREFIX_PATH "${CMAKE_PREFIX_PATH}" CACHE STRING "Extra prefix search paths" FORCE)
endif()

if(NOT CMAKE_DISABLE_FIND_PACKAGE_OpenMP)
    find_package(OpenMP REQUIRED)
endif()
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
  PATCH_COMMAND bash -c "git apply --check ${CMAKE_CURRENT_SOURCE_DIR}/third-party/faiss-local.patch 2>/dev/null && git apply ${CMAKE_CURRENT_SOURCE_DIR}/third-party/faiss-local.patch || echo 'Patch already applied or failed'"
)
FetchContent_MakeAvailable(faiss)

if(IOS)
    # faiss includes <omp.h> unconditionally; give it the single-threaded
    # stub (see third-party/omp-stub/omp.h). PRIVATE so it never leaks into
    # miniclaw_core's consumers.
    target_include_directories(faiss SYSTEM PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/third-party/omp-stub")
endif()

option(USE_SQLITE "Use SQLite FTS5 instead of LucenePlusPlus" OFF)
if(USE_SQLITE)
    add_compile_definitions(USE_SQLITE)
endif()

# Force the mobile code paths (sandboxed shell + file sandbox) on a desktop
# build, for testing without an Android/iOS device:
#   cmake -B build-mobile -DFORCE_MOBILE=ON && cmake --build build-mobile
option(FORCE_MOBILE "Build with MC_MOBILE even on desktop (testing)" OFF)

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
    if(WIN32)
        # Windows fiber_pool uses Boost.Fiber (compiled libs); Lucene++ (when
        # not USE_SQLITE) needs the rest.
        find_package(Boost REQUIRED COMPONENTS filesystem iostreams thread date_time regex fiber context)
        include_directories(${Boost_INCLUDE_DIRS})
    elseif(NOT USE_SQLITE)
        # Non-Windows: Boost is needed only by Lucene++. fiber_pool uses the
        # vendored C fiber runtime everywhere except Windows, and with
        # USE_SQLITE=ON there is no Lucene++ either — so e.g. iOS needs no
        # Boost at all.
        if(APPLE)
            set(BOOST_INC "/opt/homebrew/opt/boost/include")
            include_directories(${BOOST_INC})
            set(BOOST_LIB "/opt/homebrew/opt/boost/lib")
            link_directories(${BOOST_LIB})
        endif()
        find_package(Boost REQUIRED COMPONENTS filesystem iostreams thread date_time regex)
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

# libcurl — iOS only: the iPhoneOS SDK does not ship curl (unlike macOS), so
# build it from source, statically, against SecureTransport (Apple's built-in
# TLS stack — no OpenSSL exists on iOS) and the SDK's zlib. The ZLIB::ZLIB /
# OpenSSL::* targets were pre-created in the platform block above; CMake's
# Find modules skip target creation when they already exist.
if(IOS)
    set(CURL_USE_SECTRANSP ON CACHE BOOL "" FORCE)
    # libpsl (Public Suffix List) is only used for cookie-domain handling;
    # we never use cookies. Not available on iOS, so turn it off — its
    # find_package is REQUIRED when enabled.
    set(CURL_USE_LIBPSL OFF CACHE BOOL "" FORCE)
    # curl defaults to BUILD_SHARED_LIBS=ON (would produce a separate
    # libcurl dylib + a CURL::libcurl_shared alias); we want it static,
    # linked into libminiclaw_core.dylib like every other dependency.
    # No side effects elsewhere: this project never enables shared libs and
    # all later targets specify their type explicitly.
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(BUILD_STATIC_LIBS ON CACHE BOOL "" FORCE)
    FetchContent_Declare(
      curl
      GIT_REPOSITORY https://github.com/curl/curl.git
      GIT_TAG        curl-8_12_1
    )
    # curl's project() is named CURL, so its own alias target is
    # CURL::libcurl — exactly the name used by the link lines below.
    FetchContent_MakeAvailable(curl)
endif()


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

# ── Core sources (compiled into the embeddable shared library; every entry
#    point — desktop console wrapper, Android/iOS app — drives it through the
#    mc_* C ABI) ─────────────────────────────────────────────────────────────
set(MINICLAW_CORE_SOURCES
    src/agent.cpp
    src/agent/fiber_pool.cpp
    src/agent/memory_index.cpp
    src/agent/shutdown.cpp
    src/agent/cron_service.cpp
)

if(ANDROID)
    list(APPEND MINICLAW_CORE_SOURCES src/agent/lapack_android_stub.c)
endif()

# Helper: apply the core include dirs / definitions / link libraries to a
# target (used by miniclaw_core and the optional mc_e2e test driver).
function(miniclaw_apply_core_target tgt)
    target_include_directories(${tgt} PRIVATE 
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
    target_compile_definitions(${tgt} PRIVATE 
        __1_N_MODEL__
    )
    target_include_directories(${tgt} PRIVATE ${SEARCH_INCLUDES})
    target_link_libraries(${tgt} PRIVATE 
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
    # Platform-specific search/BLAS libs
    if(APPLE)
        if(IOS)
            # No OpenMP runtime on iOS — faiss uses the single-threaded stub
            # (see third-party/omp-stub), so there is no libomp to link.
            target_link_libraries(${tgt} PRIVATE
                ${SEARCH_LIBS}
                "-framework Accelerate"
            )
        else()
            target_link_libraries(${tgt} PRIVATE 
                ${SEARCH_LIBS}
                "-framework Accelerate"
                "/opt/homebrew/opt/libomp/lib/libomp.dylib"
            )
            if(NOT USE_SQLITE)
                target_link_libraries(${tgt} PRIVATE boost_filesystem boost_iostreams boost_thread boost_date_time boost_regex)
            endif()
        endif()
    elseif(WIN32)
        target_link_libraries(${tgt} PRIVATE 
            ${SEARCH_LIBS}
            openblas
            ws2_32
            iphlpapi
            userenv
            dbghelp
            gomp
        )
        target_link_libraries(${tgt} PRIVATE ${Boost_LIBRARIES})
    else()
        target_link_libraries(${tgt} PRIVATE 
            ${SEARCH_LIBS}
            OpenMP::OpenMP_CXX
        )
        target_link_libraries(${tgt} PRIVATE ${BLAS_LAPACK_LIBS})
        if(NOT ANDROID)
            target_link_libraries(${tgt} PRIVATE ${Boost_LIBRARIES})
        endif()
    endif()
endfunction()

# ── Embeddable shared library (Tauri mobile links this) ────────────────────
add_library(miniclaw_core SHARED ${MINICLAW_CORE_SOURCES} src/mobile/agent_api.cpp)
set_target_properties(miniclaw_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

# MinGW (MSYS2) binutils 2.41+ no longer exports all global symbols from a
# DLL by default, so the mc_* C ABI and shutdown helpers would be missing
# from the import library and the desktop `miniclaw` wrapper couldn't
# link against them. Request explicit export of all symbols on MinGW; ELF and
# Mach-O already export globals by default.
if(MINGW)
    target_link_options(miniclaw_core PRIVATE -Wl,--export-all-symbols)
endif()

if(ANDROID OR IOS OR (FORCE_MOBILE AND NOT ANDROID AND NOT IOS))
    # Mobile builds: no raw shell, file tools sandboxed to the workspace.
    target_compile_definitions(miniclaw_core PRIVATE MC_MOBILE)
endif()

miniclaw_apply_core_target(miniclaw_core)

# ── Desktop console server (sidecar) ────────────────────────────────────────
# Standalone console app that drives the engine through the same C ABI
# (mc_engine_*) as mobile, linking the shared libminiclaw_core instead of
# compiling the core sources in. It waits on a port with the classic
# HTTP/SSE endpoints (see src/main.cpp). Desktop-only: mobile embeds the
# library in-process and never spawns this.
if(NOT ANDROID AND NOT IOS)
    add_executable(miniclaw src/main.cpp)
    target_include_directories(miniclaw PRIVATE src)
    # mc_* symbols + the embedded HTTP server resolve from libminiclaw_core
    # at runtime (same shared library Android/iOS embed).
    target_link_libraries(miniclaw PRIVATE miniclaw_core)
endif()

# Desktop end-to-end test for the mobile C ABI (see MOBILE.md).
# Build with: cmake -B build -DFORCE_MOBILE=ON -DMC_E2E_TEST=ON
option(MC_E2E_TEST "Build the mobile C-ABI end-to-end test driver" OFF)
if(MC_E2E_TEST AND NOT ANDROID AND NOT IOS)
    add_executable(mc_e2e test/mobile_e2e_test.cpp ${MINICLAW_CORE_SOURCES}
                     src/mobile/agent_api.cpp)
    # Always mobile mode: this driver exists to test the phone code path.
    target_compile_definitions(mc_e2e PRIVATE MC_MOBILE)
    miniclaw_apply_core_target(mc_e2e)
endif()

# ── JNI bridge (native Android app, see android/) ───────────────────────────
# Exposes the C ABI to Java/Kotlin as com.miniclaw.core.NativeEngine.
# On Android this is built by default; on desktop it can be enabled with
# -DMC_JNI=ON (requires JAVA_HOME) purely as a compile check.
if(ANDROID)
    set(_mc_jni_default ON)
else()
    set(_mc_jni_default OFF)
endif()
option(MC_JNI "Build the JNI bridge (libminiclaw_jni)" ${_mc_jni_default})
if(MC_JNI)
    add_library(miniclaw_jni SHARED src/mobile/jni_bridge.cpp)
    set_target_properties(miniclaw_jni PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_include_directories(miniclaw_jni PRIVATE src)
    # mc_* symbols resolve from libminiclaw_core.so at runtime.
    target_link_libraries(miniclaw_jni PRIVATE miniclaw_core)
    if(ANDROID)
        # jni.h ships in the NDK sysroot; nothing else needed.
    else()
        # Desktop compile check: use JDK headers from JAVA_HOME.
        find_package(JNI REQUIRED)
        target_link_libraries(miniclaw_jni PRIVATE JNI::JNI)
    endif()
endif()

# ── Desktop test executables ────────────────────────────────────────────────
# Host-only: they build plain executables that link boost/libomp from the
# desktop toolchain, so they are skipped for Android/iOS device builds.
if(NOT ANDROID AND NOT IOS)
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
    if(ANDROID)
        target_sources(test_agent_loop PRIVATE src/agent/lapack_android_stub.c)
    else()
        target_link_libraries(test_agent_loop PRIVATE ${Boost_LIBRARIES})
    endif()
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
    if(NOT ANDROID)
        target_link_libraries(test_memory_index PRIVATE ${Boost_LIBRARIES})
    endif()
endif()

if(ANDROID)
    target_sources(test_memory_index PRIVATE src/agent/lapack_android_stub.c)
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
    if(NOT ANDROID)
        target_link_libraries(test_distillation PRIVATE ${Boost_LIBRARIES})
    endif()
endif()

if(ANDROID)
    target_sources(test_distillation PRIVATE src/agent/lapack_android_stub.c)
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
    if(NOT ANDROID)
        target_link_libraries(test_distillation_real PRIVATE ${Boost_LIBRARIES})
    endif()
endif()

if(ANDROID)
    target_sources(test_distillation_real PRIVATE src/agent/lapack_android_stub.c)
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
    if(NOT ANDROID)
        target_link_libraries(test_gmail PRIVATE ${Boost_LIBRARIES})
    endif()
endif()

if(ANDROID)
    target_sources(test_gmail PRIVATE src/agent/lapack_android_stub.c)
endif()


if(WIN32)
    add_executable(test_busybox tests/test_busybox.cpp)
    target_include_directories(test_busybox PRIVATE 
        src 
        ${EXTERNAL_DIR}/spdlog/include
        ${yaml-cpp_SOURCE_DIR}/include
        ${simdjson_SOURCE_DIR}/include
    )
    target_link_libraries(test_busybox PRIVATE ${PTHREAD_LIB} yaml-cpp simdjson)
    target_link_libraries(test_busybox PRIVATE ws2_32 iphlpapi userenv dbghelp)
    target_link_libraries(test_busybox PRIVATE ${Boost_LIBRARIES})
endif()
endif()



