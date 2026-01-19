# Google Test based test files
target_sources(orderProcessorTest
    PRIVATE
        # Custom main with global Logger initialization
        TestMain.cpp

        # Test utilities
        TestAux.cpp
        StateMachineHelper.cpp

        # Google Test converted tests
        CodecsTest.cpp
        FileStorageTest.cpp
        IncomingQueuesTest.cpp
        InterlockCacheTest.cpp
        NLinkTreeTest.cpp
        OrderBookTest.cpp
        ProcessorTest.cpp
        StateMachineTest.cpp
        StatesTest.cpp
        StorageRecordDispatcherTest.cpp
        TaskManagerTest.cpp
        IntegrationTest.cpp
)

target_include_directories(orderProcessorTest
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/src
)
