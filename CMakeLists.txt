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

# Compiler flags
add_compile_options(-msse2 -fexceptions)
add_compile_definitions(
    BOOST_MPL_CFG_NO_PREPROCESSED_HEADERS
    BOOST_MPL_LIMIT_VECTOR_SIZE=50
)

# Core dependencies
find_package(TBB REQUIRED)
find_package(spdlog REQUIRED)
find_package(Boost 1.70 REQUIRED CONFIG)
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
)

target_include_directories(orderEngine
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
        $<INSTALL_INTERFACE:include>
        ${Boost_INCLUDE_DIRS}
)

# Testing dependencies (Google Test, Google Mock, Google Benchmark)
if(BUILD_TESTS OR BUILD_BENCHMARKS)
    include(FetchContent)
endif()

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

# Installation
install(TARGETS orderEngine
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
)

install(DIRECTORY src/
    DESTINATION include
    FILES_MATCHING PATTERN "*.h"
)
