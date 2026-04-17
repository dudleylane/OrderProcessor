target_sources(orderProcessorServer PRIVATE
    main.cpp
    WsServer.cpp
    WsSession.cpp
    JsonSerializer.cpp
    WsOutQueues.cpp
    SessionManager.cpp
    MetricsPublisher.cpp)

if(BUILD_FIX)
    target_sources(orderProcessorServer PRIVATE
        FixServer.cpp
        FixGateway.cpp
        FixOutQueues.cpp
        MultiOutQueues.cpp)
endif()
