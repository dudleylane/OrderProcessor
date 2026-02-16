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
