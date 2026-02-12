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

        # New test files for previously untested modules (Phase 4.1)
        OrderMatcherTest.cpp
        OrderStorageTest.cpp
        OutgoingQueuesTest.cpp
        TransactionMgrTest.cpp
        WideDataStorageTest.cpp

        # Phase 4.2-4.3: Remaining new tests
        DeferedEventsTest.cpp
        FiltersTest.cpp
        IdTGeneratorTest.cpp
        QueuesManagerTest.cpp
        SubscriptionTest.cpp

        # Transaction rollback tests
        TransactionScopeTest.cpp
        TrOperationsTest.cpp

        # LMDB storage backend tests
        LMDBStorageTest.cpp
)

target_include_directories(orderProcessorTest
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/mocks
        ${CMAKE_SOURCE_DIR}/src
)
