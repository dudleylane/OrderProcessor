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
endif()
