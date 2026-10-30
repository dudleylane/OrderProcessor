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
        ExpirySchedulerTest.cpp
        TransactionMgrTest.cpp
        WideDataStorageTest.cpp

        # Phase 4.2-4.3: Remaining new tests
        DeferedEventsTest.cpp
        FiltersTest.cpp
        IdTGeneratorTest.cpp
        LoggerTest.cpp
        QueuesManagerTest.cpp
        SubscriptionTest.cpp

        # Transaction rollback tests
        TransactionScopeTest.cpp
        TrOperationsTest.cpp

        # Subscription event benchmark tests
        EventBenchmarkTest.cpp

        # C++23 ultra low-latency infrastructure tests
        TransactionScopePoolTest.cpp
        NumaAllocatorTest.cpp
        CacheAlignedAtomicTest.cpp
        CpuAffinityHugePagesTest.cpp
        # A std::map's nodes in a pool of their own (#467)
        NodePoolTest.cpp

        # LMDB storage backend tests
        LMDBStorageTest.cpp

        # PostgreSQL write-behind tests (unit tests always, integration gated by env var)
        PGEnumStringsTest.cpp
        PGRequestBuilderTest.cpp
        # The schema's enum types against the strings the write-behind sends (#151)
        PGSchemaTest.cpp

        # Doc-code consistency enforcement
        DocConsistencyTest.cpp

        # WebSocket client-message parser (built from app/, which the test target does not link)
        ClientMessageParserTest.cpp
        ${CMAKE_SOURCE_DIR}/app/ClientMessageParser.cpp

        # WebSocket session, served on loopback (also built from app/)
        WsSessionTest.cpp
        WsServerLimitsTest.cpp
        JsonSerializerTest.cpp

        # seedData's argument parsing (header-only, in app/)
        SeedDataArgsTest.cpp
        ServerArgsTest.cpp
        WsAuthTest.cpp
        # Identities: each sees and trades only its own accounts (#225)
        WsCredentialsTest.cpp
        WsAuthorizationTest.cpp
        ${CMAKE_SOURCE_DIR}/app/WsCredentials.cpp
        ${CMAKE_SOURCE_DIR}/app/WsOutQueues.cpp

        # The server's load of its data directory (#165)
        ServerDataTest.cpp
        ${CMAKE_SOURCE_DIR}/app/ServerData.cpp
        ${CMAKE_SOURCE_DIR}/app/WsSession.cpp
        ${CMAKE_SOURCE_DIR}/app/WsServer.cpp
        ${CMAKE_SOURCE_DIR}/app/SessionManager.cpp
        ${CMAKE_SOURCE_DIR}/app/JsonSerializer.cpp
)

# The repository the tests read files from, such as the docs DocConsistencyTest checks: test::sourceRoot(). They looked
# for it above the working directory, which ctest sets to the build directory, and a build directory outside the
# checkout has no repository above it (#374)
target_compile_definitions(orderProcessorTest
    PRIVATE
        OP_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

target_include_directories(orderProcessorTest
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/mocks
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/app
)

if(BUILD_PG)
    target_sources(orderProcessorTest PRIVATE
        PGWriteBehindTest.cpp
    )
endif()
