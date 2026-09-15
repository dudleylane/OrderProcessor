# Fuzz targets (BUILD_FUZZ=ON).
#
# The client-message parser is built from app/ClientMessageParser.cpp alone: it
# depends on the engine's enum headers and nlohmann::json, not on liborderEngine,
# so this target links no engine code (and no spdlog, which Clang cannot compile
# against the el10 spdlog 1.14 / fmt 11 pair).
#
# With Clang the target links libFuzzer (-fsanitize=fuzzer) for a coverage-guided
# campaign:
#     ./build-fuzz/test/fuzz/fuzzClientMessage test/fuzz/corpus/client_message
# With any other compiler it links standalone_main.cpp instead and replays the
# files given on the command line. Sanitizer flags are not added here; pass them
# through CMAKE_CXX_FLAGS exactly as the CI sanitizer jobs do.

add_executable(fuzzClientMessage
    fuzz_client_message.cpp
    ${CMAKE_SOURCE_DIR}/app/ClientMessageParser.cpp)

target_include_directories(fuzzClientMessage PRIVATE
    ${CMAKE_SOURCE_DIR}/app
    ${CMAKE_SOURCE_DIR}/src
    ${Boost_INCLUDE_DIRS})
target_link_libraries(fuzzClientMessage PRIVATE nlohmann_json::nlohmann_json)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(fuzzClientMessage PRIVATE -fsanitize=fuzzer)
    target_link_options(fuzzClientMessage PRIVATE -fsanitize=fuzzer)
else()
    target_sources(fuzzClientMessage PRIVATE standalone_main.cpp)
endif()

# Corpus replay as a ctest entry, so the seeds run in either mode. Files are passed
# individually because libFuzzer treats a directory argument as a corpus to fuzz
# from (runs until stopped) but file arguments as a one-shot replay. This GLOB is
# over corpus data, not sources; CONFIGURE_DEPENDS makes new seeds reconfigure.
file(GLOB CLIENT_MESSAGE_CORPUS CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/corpus/client_message/*)
add_test(NAME FuzzCorpus.ClientMessage
         COMMAND fuzzClientMessage ${CLIENT_MESSAGE_CORPUS})
