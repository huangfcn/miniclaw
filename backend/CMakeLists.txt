cmake_minimum_required(VERSION 3.14)
project(miniclaw_backend VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

# Crow - Web Framework
FetchContent_Declare(
    Crow
    GIT_REPOSITORY https://github.com/CrowCpp/Crow.git
    GIT_TAG        v1.2.0
)
FetchContent_MakeAvailable(Crow)

# nlohmann_json - JSON Library
FetchContent_Declare(
    json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
)
FetchContent_MakeAvailable(json)

# cpr - C++ Requests
FetchContent_Declare(
    cpr
    GIT_REPOSITORY https://github.com/libcpr/cpr.git
    GIT_TAG        1.10.5
)
FetchContent_MakeAvailable(cpr)

# spdlog - Logging
FetchContent_Declare(
    spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG        v1.14.1
)
FetchContent_MakeAvailable(spdlog)

add_executable(miniclaw_backend src/main.cpp src/agent.cpp)

target_include_directories(miniclaw_backend PRIVATE src)

target_link_libraries(miniclaw_backend PRIVATE Crow::Crow nlohmann_json::nlohmann_json cpr::cpr spdlog::spdlog)
