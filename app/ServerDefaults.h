#pragma once

namespace COP
{
namespace App
{

/// The clearing firm seedData seeds, and the default for orderProcessorServer's --clearing-firm: the server puts it
/// on every order (#34).
inline constexpr const char *DEFAULT_CLEARING_FIRM = "HOUSE-CLEARING";

/// The default for orderProcessorServer's --default-account, used for an order that names no account. It is one of
/// the accounts seedData seeds (#34).
inline constexpr const char *DEFAULT_ACCOUNT = "TRADING-1";

} // namespace App
} // namespace COP
