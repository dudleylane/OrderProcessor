#pragma once

#ifdef BUILD_FIX

#include <quickfix/MessageStore.h>
#include <quickfix/SessionSettings.h>

#include <memory>

namespace COP::App
{

class FixGateway;

/// The stores the FIX sessions keep what they send in, for a ResendRequest: FileStore's, in FileStorePath. The server
/// makes them here, and so does BM_FixSendExecutionReportStored, which times a report stored by them (#513). gateway
/// is the sessions' application.
std::unique_ptr<FIX::MessageStoreFactory> makeFixStores(const FIX::SessionSettings &settings, FixGateway &gateway);

} // namespace COP::App

#endif // BUILD_FIX
