#ifdef BUILD_FIX

// QuickFIX headers FIRST — Config23.h installs boost::container::flat_map as
// std::flat_map polyfill. This TU is isolated via PIMPL so the polyfill
// doesn't leak to other TUs that use GCC 15's native std::flat_map.
#include <quickfix/ThreadedSocketAcceptor.h>
#include <quickfix/FileStore.h>
#include <quickfix/FileLog.h>
#include <quickfix/SessionSettings.h>

#include "FixServer.h"
#include "FixGateway.h"
#include "FixOutQueues.h"
#include "Logger.h"

using namespace COP;
using namespace COP::App;

struct FixServer::Impl
{
    std::unique_ptr<FixGateway> gateway;
    std::unique_ptr<FixOutQueues> fixOutQueues;
    std::unique_ptr<FIX::SessionSettings> settings;
    std::unique_ptr<FIX::FileStoreFactory> storeFactory;
    std::unique_ptr<FIX::FileLogFactory> logFactory;
    std::unique_ptr<FIX::ThreadedSocketAcceptor> acceptor;
};

FixServer::FixServer(Queues::InQueues *inQueues, Store::WideParamsDataStorage *wideData,
                     Store::OrderDataStorage *orderStorage, SourceIdT defaultClearingId)
    : impl_(std::make_unique<Impl>())
{
    impl_->gateway = std::make_unique<FixGateway>(inQueues, wideData, orderStorage, defaultClearingId);
    impl_->fixOutQueues = std::make_unique<FixOutQueues>(impl_->gateway.get(), orderStorage);
}

FixServer::~FixServer()
{
    stop();
}

void FixServer::start(const std::string &configFile)
{
    impl_->settings = std::make_unique<FIX::SessionSettings>(configFile);
    impl_->storeFactory = std::make_unique<FIX::FileStoreFactory>(*impl_->settings);
    impl_->logFactory = std::make_unique<FIX::FileLogFactory>(*impl_->settings);
    impl_->acceptor = std::make_unique<FIX::ThreadedSocketAcceptor>(*impl_->gateway, *impl_->storeFactory,
                                                                    *impl_->settings, *impl_->logFactory);
    impl_->acceptor->start();
    aux::ExchLogger::instance()->note("FIX gateway started (ThreadedSocketAcceptor)");
}

void FixServer::stop()
{
    if (impl_->acceptor)
    {
        impl_->acceptor->stop();
        impl_->acceptor.reset();
        impl_->settings.reset();
        impl_->logFactory.reset();
        impl_->storeFactory.reset();
        aux::ExchLogger::instance()->note("FIX gateway stopped");
    }
}

Queues::OutQueues *FixServer::outQueues()
{
    return impl_->fixOutQueues.get();
}

#endif // BUILD_FIX
