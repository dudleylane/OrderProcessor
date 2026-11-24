target_sources(orderProcessorServer PRIVATE
    main.cpp
    ServerData.cpp
    WsServer.cpp
    WsSession.cpp
    InFlightGauge.cpp
    WsCredentials.cpp
    JsonSerializer.cpp
    ClientMessageParser.cpp
    WsOutQueues.cpp
    BookPublisher.cpp
    SessionManager.cpp
    MetricsPublisher.cpp)
