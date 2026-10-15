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
