target_sources(orderProcessorBench
    PRIVATE
        EventProcessingBench.cpp
        OrderMatchingBench.cpp
        StateMachineBench.cpp
        InterlockCacheBench.cpp
)

target_include_directories(orderProcessorBench
    PRIVATE
        ${CMAKE_SOURCE_DIR}/test
)
