target_sources(orderProcessorServer PRIVATE
    main.cpp
    ServerData.cpp
    WsServer.cpp
    WsSession.cpp
    WsCredentials.cpp
    JsonSerializer.cpp
    ClientMessageParser.cpp
    WsOutQueues.cpp
    SessionManager.cpp
    MetricsPublisher.cpp)

if(BUILD_FIX)
    target_sources(orderProcessorServer PRIVATE
        FixServer.cpp
        FixCredentials.cpp
        FixGateway.cpp
        FixOutQueues.cpp
        FixRotatingLog.cpp
        MultiOutQueues.cpp)
endif()
