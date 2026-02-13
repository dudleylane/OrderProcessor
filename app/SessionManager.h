#pragma once

#include <memory>
#include <vector>
#include <string>
#include <oneapi/tbb/spin_rw_mutex.h>

namespace COP::App {

class WsSession;

class SessionManager {
public:
    void addSession(std::shared_ptr<WsSession> session);
    void removeSession(std::shared_ptr<WsSession> session);
    void broadcast(const std::string& json);
    void broadcastBookUpdate(const std::string& symbol, const std::string& json);

private:
    mutable oneapi::tbb::spin_rw_mutex rwLock_;
    std::vector<std::shared_ptr<WsSession>> sessions_;
};

}
