target_sources(orderProcessorBench
    PRIVATE
        EventProcessingBench.cpp
        ProcessorBench.cpp
        OrderMatchingBench.cpp
        StateMachineBench.cpp
        TransactionScopePoolBench.cpp
        NumaAllocatorBench.cpp
        OrderParamsLayoutBench.cpp
        SessionManagerBench.cpp

        # The WebSocket broadcast path a transaction worker takes through WsOutQueues (built from app/, #159)
        ${CMAKE_SOURCE_DIR}/app/SessionManager.cpp
        ${CMAKE_SOURCE_DIR}/app/WsSession.cpp
        ${CMAKE_SOURCE_DIR}/app/WsCredentials.cpp
        ${CMAKE_SOURCE_DIR}/app/JsonSerializer.cpp
        ${CMAKE_SOURCE_DIR}/app/ClientMessageParser.cpp
)

target_include_directories(orderProcessorBench
    PRIVATE
        ${CMAKE_SOURCE_DIR}/test
        ${CMAKE_SOURCE_DIR}/app
)

if(BUILD_FIX)
    target_sources(orderProcessorBench PRIVATE
        FixGatewayBench.cpp
        ${CMAKE_SOURCE_DIR}/app/FixCredentials.cpp
        ${CMAKE_SOURCE_DIR}/app/FixGateway.cpp
        ${CMAKE_SOURCE_DIR}/app/FixOutQueues.cpp
        ${CMAKE_SOURCE_DIR}/app/MultiOutQueues.cpp)
    target_include_directories(orderProcessorBench PRIVATE
        ${CMAKE_SOURCE_DIR}/app)
    # The session BM_FixSendExecutionReport and BM_FixOrderAndAcknowledgement send through reads config/fix.cfg and its
    # dictionaries from this tree, as FixtSessionTest's does: the benchmarks run in a scratch directory (#395). A
    # comparison hands a baseline the copy saved with it instead, in OP_BENCH_CONFIG_DIR (#415)
    target_compile_definitions(orderProcessorBench PRIVATE
        OP_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
endif()
