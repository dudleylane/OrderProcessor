target_sources(orderProcessorServer PRIVATE
    main.cpp
    WsServer.cpp
    WsSession.cpp
    JsonSerializer.cpp
    WsOutQueues.cpp
    SessionManager.cpp)
