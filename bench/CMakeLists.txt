target_sources(orderProcessorBench
    PRIVATE
        EventProcessingBench.cpp
        OrderMatchingBench.cpp
        StateMachineBench.cpp
        InterlockCacheBench.cpp
        TransactionScopePoolBench.cpp
        NumaAllocatorBench.cpp
        OrderParamsLayoutBench.cpp
)

target_include_directories(orderProcessorBench
    PRIVATE
        ${CMAKE_SOURCE_DIR}/test
)

if(BUILD_FIX)
    target_sources(orderProcessorBench PRIVATE
        FixGatewayBench.cpp
        ${CMAKE_SOURCE_DIR}/app/FixGateway.cpp
        ${CMAKE_SOURCE_DIR}/app/FixOutQueues.cpp
        ${CMAKE_SOURCE_DIR}/app/MultiOutQueues.cpp)
    target_include_directories(orderProcessorBench PRIVATE
        ${CMAKE_SOURCE_DIR}/app)
endif()
