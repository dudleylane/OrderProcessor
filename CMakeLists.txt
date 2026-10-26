cmake_minimum_required(VERSION 3.21)

# Handle removed FindBoost module in CMake 3.31+
if(POLICY CMP0167)
    cmake_policy(SET CMP0167 NEW)
endif()

project(OrderProcessor VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# The build directory's compile_commands.json, which clang-tidy and docs/DEBUGGING.md's cppcheck recipe read (#174)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Build options
option(BUILD_TESTS "Build unit tests" ON)
option(BUILD_BENCHMARKS "Build performance benchmarks" ON)
option(BUILD_APP "Build production WebSocket server" ON)
option(BUILD_FIX "Build with QuickFIX/C++ FIX gateway (requires quickfix)" OFF)
if(BUILD_FIX)
    # QuickFIX changes its SONAME at every major release, and code built against one major must not use
    # another's library (22.0 changed an inline comparator in message_order), so ask for the major this
    # branch is verified with. The package's version check accepts the same major only, and its imported
    # target brings the headers and the library from the same installation.
    find_package(quickfix 22 CONFIG REQUIRED)
    # The FIX credentials file's password hashes, PBKDF2-HMAC-SHA256 (#147)
    find_package(OpenSSL REQUIRED COMPONENTS Crypto)
endif()
option(BUILD_FUZZ "Build fuzz targets in test/fuzz (libFuzzer with Clang, corpus replay otherwise)" OFF)
option(WARNINGS_AS_ERRORS "Fail the build on a warning in this project's own code; CI's Release job and the pre-push hook turn it on (#167)" OFF)
set(MARCH "native" CACHE STRING
    "The CPU a Release build targets (-march): native, this machine's, or an ISA level such as x86-64-v3 for a binary that runs on other machines; the server image uses x86-64-v3 (#170)")

# Warnings for this project's own targets, which the FetchContent dependencies don't get: -Wall -Wextra, and -Werror with
# WARNINGS_AS_ERRORS (#167)
function(op_warnings target)
    target_compile_options(${target} PRIVATE -Wall -Wextra $<$<BOOL:${WARNINGS_AS_ERRORS}>:-Werror>)
endfunction()

# Compiler flags
add_compile_options(-msse2 -fexceptions)

# Ultra low-latency optimization flags for Release builds
if(CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
    # Tune for this machine when building for it, and for no CPU in particular otherwise, as GCC does by default (#170)
    if(MARCH STREQUAL "native")
        set(MTUNE native)
    else()
        set(MTUNE generic)
    endif()
    add_compile_options(
        -O3                    # Aggressive optimization
        -march=${MARCH}        # The target CPU's instructions: native by default (#170)
        -mtune=${MTUNE}        # Its scheduling
        -flto=auto             # Link-time optimization, its LTRANS jobs in parallel: plain -flto warned of serial ones (#167)
        -ffast-math            # Fast floating-point, but its -ffinite-math-only lets the compiler delete every
        -fno-finite-math-only  # inf/NaN check (#142: a price of 1e400 parsed as inf), so turn that part back off
        -funroll-loops         # Loop unrolling
        -fomit-frame-pointer   # Free up register for hot paths
    )
    add_link_options(-flto=auto) # Enable LTO at link time
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
# SHA-256 from OpenSSL's libcrypto, for the WebSocket server's token hashes (#225)
if(BUILD_APP OR BUILD_TESTS OR BUILD_BENCHMARKS)
    find_package(OpenSSL REQUIRED COMPONENTS Crypto)
endif()

option(BUILD_PG "Build with PostgreSQL write-behind (requires libpqxx)" OFF)
if(BUILD_PG)
    pkg_check_modules(LIBPQXX REQUIRED IMPORTED_TARGET libpqxx)
endif()

message(STATUS "Boost_INCLUDE_DIRS = ${Boost_INCLUDE_DIRS}")
message(STATUS "Boost_LIBRARY_DIRS = ${Boost_LIBRARY_DIRS}")

# Main library
add_library(orderEngine STATIC)
add_subdirectory(src)
op_warnings(orderEngine)

target_link_libraries(orderEngine
    PUBLIC
        TBB::tbb
        TBB::tbbmalloc
        spdlog::spdlog
        PkgConfig::LMDB
        numa
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

# nlohmann/json is needed by the app, the fuzz targets, the parser unit tests, and the WebSocket benchmarks (#159), so it
# is fetched
# before the test target is created.
if(BUILD_APP OR BUILD_FUZZ OR BUILD_TESTS OR BUILD_BENCHMARKS)
    FetchContent_Declare(json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG v3.11.3)
    FetchContent_MakeAvailable(json)
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
    op_warnings(orderProcessorTest)

    target_link_libraries(orderProcessorTest
        PRIVATE
            orderEngine
            GTest::gtest
            GTest::gmock
            nlohmann_json::nlohmann_json
            OpenSSL::Crypto
    )

    if(BUILD_FIX)
        target_link_libraries(orderProcessorTest PRIVATE quickfix::quickfix OpenSSL::Crypto)
        target_compile_definitions(orderProcessorTest PRIVATE BUILD_FIX)
        target_include_directories(orderProcessorTest PRIVATE ${CMAKE_SOURCE_DIR}/app)
    endif()

    include(GoogleTest)
    gtest_discover_tests(orderProcessorTest)

    # The runner's job-started hook against the payloads it decides on (#143, #168). It needs jq, as the hook does: without
    # it the test is skipped.
    add_test(NAME JobStartedHookDecides
             COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/test/JobStartedHookTest.sh
                     ${CMAKE_CURRENT_SOURCE_DIR}/scripts/runner/job-started.sh)
    set_tests_properties(JobStartedHookDecides PROPERTIES SKIP_RETURN_CODE 77)

    # Each workflow's tested-tree job against canned API answers (#314): when the push of a merge may skip the jobs
    # its pull request's run passed. It needs jq and git, as the script does: without them the test is skipped.
    add_test(NAME CiTestedTreeDecides
             COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/test/CiTestedTreeTest.sh
                     ${CMAKE_CURRENT_SOURCE_DIR}/scripts/ci-tested-tree.sh)
    set_tests_properties(CiTestedTreeDecides PROPERTIES SKIP_RETURN_CODE 77)

    # The CPUs a pinned benchmark comparison watches, and the busiest of them (#406), against a made-up topology and
    # canned /proc/stat snapshots
    add_test(NAME CpuBusyDecides
             COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/test/CpuBusyTest.sh
                     ${CMAKE_CURRENT_SOURCE_DIR}/scripts/cpu-busy.sh)

    # A benchmark comparison's baseline and the config it reads (#415), with stub benchmark binaries. It needs python3,
    # as the script does: without it the test is skipped.
    add_test(NAME BenchmarkBaselineKeepsItsConfig
             COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/test/BenchmarkBaselineTest.sh
                     ${CMAKE_CURRENT_SOURCE_DIR}/scripts/benchmark-regression.sh)
    set_tests_properties(BenchmarkBaselineKeepsItsConfig PROPERTIES SKIP_RETURN_CODE 77)
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
    op_warnings(orderProcessorBench)

    target_link_libraries(orderProcessorBench
        PRIVATE
            orderEngine
            benchmark::benchmark
            benchmark::benchmark_main
            nlohmann_json::nlohmann_json
            OpenSSL::Crypto
    )

    if(BUILD_FIX)
        target_link_libraries(orderProcessorBench PRIVATE quickfix::quickfix OpenSSL::Crypto)
        target_compile_definitions(orderProcessorBench PRIVATE BUILD_FIX)
        if(BUILD_TESTS)
            # The FIX session benchmarks read their config from OP_BENCH_CONFIG_DIR when it is set, and from this tree's
            # config/ otherwise (#415). It needs python3: without it the test is skipped.
            add_test(NAME FixBenchmarksFindTheirConfig
                     COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/test/FixBenchConfigTest.sh
                             $<TARGET_FILE:orderProcessorBench> ${CMAKE_CURRENT_SOURCE_DIR}/config)
            set_tests_properties(FixBenchmarksFindTheirConfig PROPERTIES SKIP_RETURN_CODE 77)
        endif()
    endif()
endif()

if(BUILD_APP)
    add_executable(orderProcessorServer)
    add_subdirectory(app)
    op_warnings(orderProcessorServer)
    target_link_libraries(orderProcessorServer PRIVATE
        orderEngine nlohmann_json::nlohmann_json OpenSSL::Crypto pthread)

    if(BUILD_FIX)
        target_link_libraries(orderProcessorServer PRIVATE quickfix::quickfix OpenSSL::Crypto)
        target_compile_definitions(orderProcessorServer PRIVATE BUILD_FIX)
    endif()

    add_executable(seedData app/seed_data.cpp)
    op_warnings(seedData)
    target_link_libraries(seedData PRIVATE orderEngine pthread)

    if(BUILD_TESTS)
        # Run seedData itself, so the Debug and sanitizer CI jobs see an abort in its own code (#41): on a new directory,
        # then again on the one it seeded, where it skips what exists. Both runs are in every ctest: CI's checkout removes
        # the build tree, so a directory left by an earlier run never covered the second path there (#175).
        add_test(NAME SeedDataStartsFromANewDirectory
                 COMMAND ${CMAKE_COMMAND} -E remove_directory ${CMAKE_CURRENT_BINARY_DIR}/seed-data-test)
        set_tests_properties(SeedDataStartsFromANewDirectory PROPERTIES FIXTURES_SETUP SeedDataNewDirectory)
        add_test(NAME SeedDataRunsCleanly COMMAND seedData ${CMAKE_CURRENT_BINARY_DIR}/seed-data-test)
        set_tests_properties(SeedDataRunsCleanly PROPERTIES FIXTURES_REQUIRED SeedDataNewDirectory
                                                            FIXTURES_SETUP SeedDataSeededDirectory)
        add_test(NAME SeedDataRunsCleanlyOnASeededDirectory COMMAND seedData ${CMAKE_CURRENT_BINARY_DIR}/seed-data-test)
        set_tests_properties(SeedDataRunsCleanlyOnASeededDirectory PROPERTIES FIXTURES_REQUIRED SeedDataSeededDirectory)
        # seedData --if-empty seeds a directory with no instrument, and leaves one with instruments alone (#161)
        add_test(NAME SeedDataIfEmptySeedsOnlyADirectoryWithoutInstruments
                 COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/test/SeedDataIfEmptyTest.sh $<TARGET_FILE:seedData>
                         $<TARGET_FILE:orderProcessorServer>)
        # Run the server so that it can't start: it must say why and exit, not abort (#203)
        add_test(NAME ServerThatCannotStartExits
                 COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/test/ServerStartupTest.sh $<TARGET_FILE:orderProcessorServer>)
        if(BUILD_FIX)
            # A FIX client that reconnects gets back the reports it missed: the server, its settings, two sessions (#160)
            find_package(Python3 REQUIRED COMPONENTS Interpreter)
            add_test(NAME FixClientRecoversMissedReports
                     COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/test/FixResendTest.py
                             $<TARGET_FILE:orderProcessorServer> $<TARGET_FILE:seedData>
                             ${CMAKE_CURRENT_SOURCE_DIR}/config/fix.cfg ${CMAKE_CURRENT_SOURCE_DIR}/config/spec)
            # Only a counterparty with its Username and Password logs on, and it trades only its accounts (#147)
            add_test(NAME FixLogonNeedsCredentials
                     COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/test/FixLogonTest.py
                             $<TARGET_FILE:orderProcessorServer> $<TARGET_FILE:seedData>
                             ${CMAKE_CURRENT_SOURCE_DIR}/config/fix.cfg ${CMAKE_CURRENT_SOURCE_DIR}/config/spec)
            # An order the gateway refuses is answered to its sender, and to nobody else (#189)
            add_test(NAME FixRefusalsReachOnlyTheirSender
                     COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/test/FixRefusalTest.py
                             $<TARGET_FILE:orderProcessorServer> $<TARGET_FILE:seedData>
                             ${CMAKE_CURRENT_SOURCE_DIR}/config/fix.cfg ${CMAKE_CURRENT_SOURCE_DIR}/config/spec)
            # With --fix-tls-cert and --fix-tls-key, FIX serves TLS only; without them it warns (#228)
            add_test(NAME FixServesTlsOnly
                     COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/test/FixTlsTest.py
                             $<TARGET_FILE:orderProcessorServer> $<TARGET_FILE:seedData>
                             ${CMAKE_CURRENT_SOURCE_DIR}/config/fix.cfg ${CMAKE_CURRENT_SOURCE_DIR}/config/spec)
        endif()
    endif()
endif()

if(BUILD_FUZZ)
    # enable_testing() otherwise runs only under BUILD_TESTS; without it the
    # corpus-replay add_test registers nothing and ctest passes vacuously.
    enable_testing()
    add_subdirectory(test/fuzz)
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
