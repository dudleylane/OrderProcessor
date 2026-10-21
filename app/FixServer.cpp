#ifdef BUILD_FIX

// QuickFIX headers isolated in this TU via PIMPL (FixServer::Impl).
#include <quickfix/ThreadedSSLSocketAcceptor.h>
#include <quickfix/ThreadedSocketAcceptor.h>
#include <quickfix/FileStore.h>
#include <quickfix/FixValues.h>
#include <quickfix/Message.h>
#include <quickfix/SessionSettings.h>

#include <set>
#include <stdexcept>

#include "FixServer.h"
#include "FixCredentials.h"
#include "FixGateway.h"
#include "FixOutQueues.h"
#include "FixRotatingLog.h"
#include "FixTls.h"
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

/// The gateway speaks FIX 5.0 SP2 over FIXT.1.1 alone (#377), so a session of anything else is refused before the
/// acceptor starts. A FIXT session checks a message against the dictionary of its version, and against none for a
/// version that has none: AppDataDictionary is its DefaultApplVerID's.
void checkSessionsSpeakFix50Sp2(const FIX::SessionSettings &settings)
{
    for (const FIX::SessionID &sid : settings.getSessions())
    {
        const FIX::Dictionary &session = settings.get(sid);
        std::string problem;
        if (!sid.isFIXT())
        {
            problem = "its BeginString is " + sid.getBeginString().getString() + ", not FIXT.1.1";
        }
        else if (!session.has(FIX::DEFAULT_APPLVERID) ||
                 (FIX::ApplVerID_FIX50_SP2 !=
                  FIX::Message::toApplVerID(session.getString(FIX::DEFAULT_APPLVERID)).getValue()))
        {
            problem = "its DefaultApplVerID isn't FIX.5.0SP2";
        }
        else if (!session.has(FIX::TRANSPORT_DATA_DICTIONARY) || !session.has(FIX::APP_DATA_DICTIONARY))
        {
            problem = "it has no TransportDataDictionary or no AppDataDictionary";
        }
        if (!problem.empty())
        {
            throw std::runtime_error("the FIX session " + sid.toString() + " can't be served: " + problem +
                                     "; the gateway speaks FIX 5.0 SP2 over FIXT.1.1 alone (#377)");
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
    std::unique_ptr<FIX::Acceptor> acceptor;           // ThreadedSSLSocketAcceptor with TLS (#228)
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

void FixServer::start(const std::string &configFile, const std::string &credentialsFile, const FixTlsFiles &tls)
{
    impl_->settings = std::make_unique<FIX::SessionSettings>(configFile);
    if (tls.enabled())
    {
        // Into [DEFAULT] before the counterparties' sessions are made from it, though QuickFIX reads them from there
        addFixTls(*impl_->settings, tls);
    }
    impl_->credentials = std::make_unique<FixCredentials>(FixCredentials::load(credentialsFile));
    addCounterpartySessions(*impl_->settings, *impl_->credentials, credentialsFile);
    checkSessionsSpeakFix50Sp2(*impl_->settings);
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
    if (tls.enabled())
    {
        impl_->acceptor = std::make_unique<FIX::ThreadedSSLSocketAcceptor>(*impl_->gateway, *impl_->storeFactory,
                                                                           *impl_->settings, *impl_->logFactory);
        impl_->acceptor->start();
        aux::ExchLogger::instance()->note("FIX gateway started (ThreadedSSLSocketAcceptor): TLS only, " +
                                          impl_->settings->get().getString(FIX::SSL_PROTOCOL) + ", with " +
                                          tls.certificate + " (#228)");
        return;
    }
    impl_->acceptor = std::make_unique<FIX::ThreadedSocketAcceptor>(*impl_->gateway, *impl_->storeFactory,
                                                                    *impl_->settings, *impl_->logFactory);
    impl_->acceptor->start();
    aux::ExchLogger::instance()->note("FIX gateway started (ThreadedSocketAcceptor)");
    aux::ExchLogger::instance()->warn("FIX runs over plain TCP: a counterparty's Logon password, and every order and "
                                      "report, cross the network in the clear. --fix-tls-cert and --fix-tls-key "
                                      "serve TLS (#228)");
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
