#ifdef BUILD_FIX

// QuickFIX headers isolated in this TU via PIMPL (FixServer::Impl).
#include <quickfix/ThreadedSocketAcceptor.h>
#include <quickfix/FileStore.h>
#include <quickfix/SessionSettings.h>

#include <set>
#include <stdexcept>

#include "FixServer.h"
#include "FixCredentials.h"
#include "FixGateway.h"
#include "FixOutQueues.h"
#include "FixRotatingLog.h"
#include "Logger.h"

using namespace COP;
using namespace COP::App;

namespace
{
/// A session for each counterparty the credentials name, unless the settings have one, and none for anyone else (#147).
/// The image's config/fix.cfg names no session since #147: the counterparties are the deployment's, not the image's.
void addCounterpartySessions(FIX::SessionSettings &settings, const FixCredentials &credentials,
                             const std::string &credentialsFile)
{
    const FIX::Dictionary &defaults = settings.get();
    if (!defaults.has(FIX::BEGINSTRING) || !defaults.has(FIX::SENDERCOMPID))
    {
        throw std::runtime_error("the FIX settings' [DEFAULT] needs BeginString and SenderCompID, for the sessions " +
                                 credentialsFile + " makes");
    }
    const std::string beginString = defaults.getString(FIX::BEGINSTRING);
    const std::string senderCompId = defaults.getString(FIX::SENDERCOMPID);
    for (const FixCounterparty &counterparty : credentials.counterparties())
    {
        const FIX::SessionID sid(beginString, senderCompId, counterparty.compId);
        if (!settings.has(sid))
        {
            settings.set(sid, FIX::Dictionary()); // the rest from [DEFAULT]
        }
    }
    const std::set<FIX::SessionID> sessions = settings.getSessions();
    if (sessions.empty())
    {
        throw std::runtime_error(credentialsFile + " names no FIX counterparty");
    }
    for (const FIX::SessionID &sid : sessions)
    {
        if (nullptr == credentials.find(sid.getTargetCompID().getString()))
        {
            throw std::runtime_error("the FIX session " + sid.toString() + " has no line in " + credentialsFile +
                                     ", so nobody could log on to it");
        }
    }
}
} // namespace

struct FixServer::Impl
{
    std::unique_ptr<FixCredentials> credentials; // who may log on, and what each may trade (#147)
    std::unique_ptr<FixGateway> gateway;
    std::unique_ptr<FixOutQueues> fixOutQueues;
    std::unique_ptr<FIX::SessionSettings> settings;
    std::unique_ptr<FIX::FileStoreFactory> storeFactory;
    std::unique_ptr<FixRotatingLogFactory> logFactory; // FileLog's files, bounded (#160)
    std::unique_ptr<FIX::ThreadedSocketAcceptor> acceptor;
};

FixServer::FixServer(Queues::InQueues *inQueues, Store::WideParamsDataStorage *wideData,
                     Store::OrderDataStorage *orderStorage, SourceIdT defaultClearingId, SourceIdT defaultAccountId)
    : impl_(std::make_unique<Impl>())
{
    impl_->gateway =
        std::make_unique<FixGateway>(inQueues, wideData, orderStorage, defaultClearingId, defaultAccountId);
    impl_->fixOutQueues = std::make_unique<FixOutQueues>(impl_->gateway.get(), orderStorage);
}

FixServer::~FixServer()
{
    stop();
}

void FixServer::start(const std::string &configFile, const std::string &credentialsFile)
{
    impl_->settings = std::make_unique<FIX::SessionSettings>(configFile);
    impl_->credentials = std::make_unique<FixCredentials>(FixCredentials::load(credentialsFile));
    addCounterpartySessions(*impl_->settings, *impl_->credentials, credentialsFile);
    impl_->gateway->setCredentials(impl_->credentials.get());
    std::string compIds;
    for (const FixCounterparty &counterparty : impl_->credentials->counterparties())
    {
        compIds += (compIds.empty() ? "" : ", ") + counterparty.compId;
        const unsigned iterations = FixCredentials::iterationsOf(counterparty.passwordHash);
        if (iterations < FixCredentials::MIN_RECOMMENDED_ITERATIONS)
        {
            aux::ExchLogger::instance()->warn(credentialsFile + " hashes " + counterparty.compId + "'s password with " +
                                              std::to_string(iterations) + " iterations: fewer than " +
                                              std::to_string(FixCredentials::MIN_RECOMMENDED_ITERATIONS) +
                                              " is weak against a stolen file (#147)");
        }
    }
    aux::ExchLogger::instance()->note("FIX counterparties, from " + credentialsFile + ": " + compIds);
    impl_->storeFactory = std::make_unique<FIX::FileStoreFactory>(*impl_->settings);
    impl_->logFactory = std::make_unique<FixRotatingLogFactory>(*impl_->settings);
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
