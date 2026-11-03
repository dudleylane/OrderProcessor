#ifdef BUILD_FIX

#include "FixStores.h"

#include <quickfix/FileStore.h>

namespace COP::App
{

std::unique_ptr<FIX::MessageStoreFactory> makeFixStores(const FIX::SessionSettings &settings, FixGateway & /*gateway*/)
{
    return std::make_unique<FIX::FileStoreFactory>(settings);
}

} // namespace COP::App

#endif // BUILD_FIX
