cmake_minimum_required(VERSION 3.21)

# Handle removed FindBoost module in CMake 3.31+
if(POLICY CMP0167)
    cmake_policy(SET CMP0167 NEW)
endif()

project(OrderProcessor VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Build options
option(BUILD_TESTS "Build unit tests" ON)
option(BUILD_BENCHMARKS "Build performance benchmarks" ON)
option(BUILD_APP "Build production WebSocket server" ON)

# Compiler flags
add_compile_options(-msse2 -fexceptions)

# Ultra low-latency optimization flags for Release builds
if(CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
    add_compile_options(
        -O3                    # Aggressive optimization
        -march=native          # CPU-specific optimizations
        -mtune=native          # CPU-specific tuning
        -flto                  # Link-time optimization
        -ffast-math            # Fast floating-point (safe for price comparisons)
        -funroll-loops         # Loop unrolling
        -fomit-frame-pointer   # Free up register for hot paths
    )
    add_link_options(-flto)    # Enable LTO at link time
endif()

add_compile_definitions(
    BOOST_MPL_CFG_NO_PREPROCESSED_HEADERS
    BOOST_MPL_LIMIT_VECTOR_SIZE=50
)

# Core dependencies
find_package(TBB REQUIRED)
find_package(spdlog REQUIRED)
find_package(Boost 1.70 REQUIRED CONFIG)
find_package(PkgConfig REQUIRED)
pkg_check_modules(LMDB REQUIRED IMPORTED_TARGET lmdb)

option(BUILD_PG "Build with PostgreSQL write-behind (requires libpqxx)" OFF)
if(BUILD_PG)
    pkg_check_modules(LIBPQXX REQUIRED IMPORTED_TARGET libpqxx)
endif()

message(STATUS "Boost_INCLUDE_DIRS = ${Boost_INCLUDE_DIRS}")
message(STATUS "Boost_LIBRARY_DIRS = ${Boost_LIBRARY_DIRS}")

# Main library
add_library(orderEngine STATIC)
add_subdirectory(src)

target_link_libraries(orderEngine
    PUBLIC
        TBB::tbb
        TBB::tbbmalloc
        spdlog::spdlog
        PkgConfig::LMDB
)

target_include_directories(orderEngine
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
        $<INSTALL_INTERFACE:include>
        ${Boost_INCLUDE_DIRS}
)

if(BUILD_PG)
    target_compile_definitions(orderEngine PUBLIC BUILD_PG)
    target_link_libraries(orderEngine PUBLIC PkgConfig::LIBPQXX)
endif()

# FetchContent for external dependencies
include(FetchContent)

if(BUILD_TESTS)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.14.0
    )
    FetchContent_MakeAvailable(googletest)

    enable_testing()

    add_executable(orderProcessorTest)
    add_subdirectory(test)

    target_link_libraries(orderProcessorTest
        PRIVATE
            orderEngine
            GTest::gtest
            GTest::gmock
    )

    include(GoogleTest)
    gtest_discover_tests(orderProcessorTest)
endif()

if(BUILD_BENCHMARKS)
    FetchContent_Declare(
        benchmark
        GIT_REPOSITORY https://github.com/google/benchmark.git
        GIT_TAG v1.8.3
    )
    set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(benchmark)

    add_executable(orderProcessorBench)
    add_subdirectory(bench)

    target_link_libraries(orderProcessorBench
        PRIVATE
            orderEngine
            benchmark::benchmark
            benchmark::benchmark_main
    )
endif()

if(BUILD_APP)
    FetchContent_Declare(json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG v3.11.3)
    FetchContent_MakeAvailable(json)

    add_executable(orderProcessorServer)
    add_subdirectory(app)
    target_link_libraries(orderProcessorServer PRIVATE
        orderEngine nlohmann_json::nlohmann_json pthread)
endif()

# Installation
install(TARGETS orderEngine
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
)

install(DIRECTORY src/
    DESTINATION include
    FILES_MATCHING PATTERN "*.h"
)
