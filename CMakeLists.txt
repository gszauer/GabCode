cmake_minimum_required(VERSION 3.20)
project(gabcode VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(GABCODE_BUILD_CLI "Build the CLI shell" ON)
option(GABCODE_BUILD_TESTS "Build tests" ON)

add_subdirectory(deps)
add_subdirectory(core)

if(GABCODE_BUILD_CLI)
    add_subdirectory(cli)
endif()

if(GABCODE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
