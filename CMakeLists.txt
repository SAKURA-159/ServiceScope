cmake_minimum_required(VERSION 3.16)
project(ServiceScope VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Fetch header-only dependencies
include(FetchContent)

FetchContent_Declare(
    cpp-httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG v0.18.1
)
FetchContent_GetProperties(cpp-httplib)
if(NOT cpp-httplib_POPULATED)
    FetchContent_Populate(cpp-httplib)
endif()

FetchContent_Declare(
    json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
)
FetchContent_GetProperties(json)
if(NOT json_POPULATED)
    FetchContent_Populate(json)
endif()

add_executable(servicescope
    src/main.cpp
)

target_include_directories(servicescope PRIVATE
    ${cpp-httplib_SOURCE_DIR}
    ${json_SOURCE_DIR}/include
    src
)

# Platform-specific settings
if(WIN32)
    target_compile_definitions(servicescope PRIVATE _WIN32_WINNT=0x0A00)
    target_link_libraries(servicescope PRIVATE ws2_32)
endif()

if(MSVC)
    target_compile_options(servicescope PRIVATE /W4 /O2)
else()
    target_compile_options(servicescope PRIVATE -Wall -Wextra -O3 -march=native)
    target_link_libraries(servicescope PRIVATE pthread)
endif()
