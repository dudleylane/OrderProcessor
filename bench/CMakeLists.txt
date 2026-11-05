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
        ClientMessageParserBench.cpp
        WsSessionBench.cpp

        # The WebSocket broadcast path a transaction worker takes through WsOutQueues (built from app/, #159), the
        # parser the IO thread runs on each client message (#516), and a served session's read of each (#519)
        ${CMAKE_SOURCE_DIR}/app/SessionManager.cpp
        ${CMAKE_SOURCE_DIR}/app/WsServer.cpp
        ${CMAKE_SOURCE_DIR}/app/WsSession.cpp
        ${CMAKE_SOURCE_DIR}/app/InFlightGauge.cpp
        ${CMAKE_SOURCE_DIR}/app/WsCredentials.cpp
        ${CMAKE_SOURCE_DIR}/app/JsonSerializer.cpp
        ${CMAKE_SOURCE_DIR}/app/ClientMessageParser.cpp
        ${CMAKE_SOURCE_DIR}/app/WsOutQueues.cpp
)

target_include_directories(orderProcessorBench
    PRIVATE
        ${CMAKE_SOURCE_DIR}/test
        ${CMAKE_SOURCE_DIR}/app
)
