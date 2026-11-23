#pragma once

#include <chrono>
#include <optional>
#include <string>

#include "TypesDef.h"

namespace COP
{
namespace App
{

/// A settlement date as the engine keeps one, milliseconds since the epoch at midnight UTC of its day (#198), as the
/// number YYYYMMDD
inline u32 settlementDay(DateTimeT date)
{
    using namespace std::chrono;
    const year_month_day day(floor<days>(sys_time<milliseconds>(milliseconds(date))));
    return static_cast<u32>(static_cast<int>(day.year()) * 10000 +
                            static_cast<int>(static_cast<unsigned>(day.month())) * 100 +
                            static_cast<int>(static_cast<unsigned>(day.day())));
}

/// Why a swap's settlement dates are refused at entry, or nothing (#551), as ruled on #551:
/// - each must be a date as the engine keeps one, midnight UTC of its day (#198), up to 9999-12-31, the last day
///   YYYYMMDD names;
/// - the far one must be after the near one;
/// - the near one must not be before businessDate, the business day's date as YYYYMMDD
///   (ExpiryScheduler::tradeDateOf()), which each entry takes from its own read of the clock.
/// There's no horizon: how far ahead a date may be is the maintainer's to set (#551). The FIX gateway and the WebSocket
/// session ask this of a new swap; nothing asks it of an order already stored, and a replace gives no dates.
inline std::optional<std::string> swapDatesRefusal(DateTimeT nearDate, DateTimeT farDate, u32 businessDate)
{
    constexpr DateTimeT DAY = 86400000;
    constexpr DateTimeT AFTER_THE_LAST_DAY = 253402300800000; // 10000-01-01T00:00Z
    const auto isDate = [](DateTimeT date)
    {
        return (0 == date % DAY) && (date < AFTER_THE_LAST_DAY);
    };
    if (!isDate(nearDate))
    {
        return std::string("the near leg's settlement date must be a date: midnight UTC of its day, up to 9999-12-31");
    }
    if (!isDate(farDate))
    {
        return std::string("the far leg's settlement date must be a date: midnight UTC of its day, up to 9999-12-31");
    }
    if (farDate <= nearDate)
    {
        return std::string("the far leg's settlement date must be after the near leg's");
    }
    const u32 nearDay = settlementDay(nearDate);
    if (nearDay < businessDate)
    {
        return "the near leg's settlement date, " + std::to_string(nearDay) + ", is before the business day, " +
               std::to_string(businessDate);
    }
    return std::nullopt;
}

} // namespace App
} // namespace COP
