#ifdef BUILD_FIX

#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

#include "FixTls.h"
#include "TestAux.h"

using namespace COP::App;

namespace
{

/// Settings as config/fix.cfg gives them, plus extra, which goes into [DEFAULT]
FIX::SessionSettings settingsWith(const std::string &extra = "")
{
    std::istringstream text("[DEFAULT]\nConnectionType=acceptor\nBeginString=FIX.4.4\nSenderCompID=ORDER_PROCESSOR\n"
                            "SocketAcceptPort=5001\n" +
                            extra);
    return FIX::SessionSettings(text);
}

/// A file with something in it, removed when the test ends
struct TempFile
{
    explicit TempFile(const std::string &name) : path(test::uniqueTestPath(name))
    {
        std::ofstream(path) << "-----BEGIN-----\n";
    }
    ~TempFile()
    {
        std::remove(path.c_str());
    }
    const std::string path;
};

TEST(FixTlsTest, TheFilesAndTheProtocolsGoIntoDefault)
{
    const TempFile cert("fix_tls_cert");
    const TempFile key("fix_tls_key");
    FIX::SessionSettings settings = settingsWith();
    addFixTls(settings, FixTlsFiles{ cert.path, key.path });

    EXPECT_EQ(cert.path, settings.get().getString(FIX::SERVER_CERTIFICATE_FILE));
    EXPECT_EQ(key.path, settings.get().getString(FIX::SERVER_CERTIFICATE_KEY_FILE));
    // TLS 1.2 and 1.3 only: QuickFIX's default, "all -SSLv2", allows SSLv3, TLS 1.0 and TLS 1.1
    EXPECT_EQ("-all +TLSv1_2 +TLSv1_3", settings.get().getString(FIX::SSL_PROTOCOL));

    // A counterparty's session, made after, as FixServer makes them, carries them too
    const FIX::SessionID sid("FIX.4.4", "ORDER_PROCESSOR", "CLIENT_A");
    settings.set(sid, FIX::Dictionary());
    EXPECT_EQ(cert.path, settings.get(sid).getString(FIX::SERVER_CERTIFICATE_FILE));
}

TEST(FixTlsTest, TheSettingsOwnProtocolsAreKept)
{
    const TempFile cert("fix_tls_cert");
    const TempFile key("fix_tls_key");
    FIX::SessionSettings settings = settingsWith("SSLProtocol=-all +TLSv1_3\n");
    addFixTls(settings, FixTlsFiles{ cert.path, key.path });
    EXPECT_EQ("-all +TLSv1_3", settings.get().getString(FIX::SSL_PROTOCOL));
}

TEST(FixTlsTest, AFileItCantReadIsRefusedByName)
{
    const TempFile cert("fix_tls_cert");
    const std::string missing = test::uniqueTestPath("fix_tls_missing");
    const auto refusal = [](const FixTlsFiles &tls) -> std::string
    {
        FIX::SessionSettings settings = settingsWith();
        try
        {
            addFixTls(settings, tls);
        }
        catch (const std::runtime_error &e)
        {
            return e.what();
        }
        return "accepted";
    };
    EXPECT_NE(std::string::npos,
              refusal(FixTlsFiles{ missing, cert.path }).find("can't read the FIX TLS certificate '" + missing + "'"));
    EXPECT_NE(std::string::npos,
              refusal(FixTlsFiles{ cert.path, missing }).find("can't read the FIX TLS key '" + missing + "'"));
    EXPECT_NE(std::string::npos, refusal(FixTlsFiles{ cert.path, "" }).find("can't read the FIX TLS key ''"));
}

} // namespace

#endif // BUILD_FIX
