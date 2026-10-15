#pragma once

#ifdef BUILD_FIX

// QuickFIX's settings: include this only where QuickFIX's headers may go, as FixServer.cpp and the tests do
#include <quickfix/SessionSettings.h>

#include <fstream>
#include <stdexcept>
#include <string>

#include "FixServer.h"

namespace COP::App
{

/// The protocols the FIX acceptor serves TLS with, unless its settings name others (#228). QuickFIX's own default,
/// "all -SSLv2", allows SSLv3, TLS 1.0 and TLS 1.1.
inline constexpr const char *FIX_TLS_PROTOCOLS = "-all +TLSv1_2 +TLSv1_3";

/// Puts tls's certificate and key into the settings' [DEFAULT], where QuickFIX's SSL acceptor reads them, and
/// FIX_TLS_PROTOCOLS as SSLProtocol unless the settings give one (#228). Throws for a file this process can't read,
/// naming it: QuickFIX's own error, at start, doesn't.
inline void addFixTls(FIX::SessionSettings &settings, const FixTlsFiles &tls)
{
    const auto mustRead = [](const std::string &path, const std::string &what)
    {
        if (path.empty() || !std::ifstream(path))
        {
            throw std::runtime_error("can't read the FIX TLS " + what + " '" + path +
                                     "' (#228): the server must be able to read it");
        }
    };
    mustRead(tls.certificate, "certificate");
    mustRead(tls.key, "key");
    FIX::Dictionary defaults = settings.get();
    defaults.setString(FIX::SERVER_CERTIFICATE_FILE, tls.certificate);
    defaults.setString(FIX::SERVER_CERTIFICATE_KEY_FILE, tls.key);
    if (!defaults.has(FIX::SSL_PROTOCOL))
    {
        defaults.setString(FIX::SSL_PROTOCOL, FIX_TLS_PROTOCOLS);
    }
    settings.set(defaults);
}

} // namespace COP::App

#endif // BUILD_FIX
