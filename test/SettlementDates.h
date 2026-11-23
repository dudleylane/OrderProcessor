/**
 Concurrent Order Processor library - a swap's settlement dates for the tests and the benchmarks

 Copyright (C) 2026 dudleylane

 Distributed under the GNU Affero General Public License (AGPL).
*/

#pragma once

#include <chrono>
#include <cstdio>
#include <string>

#include "ExpiryScheduler.h"
#include "TypesDef.h"

namespace test
{

/**
 * A settlement date the server takes today for a swap's leg: daysAfter days after the current business day's date, as
 * the engine keeps one, milliseconds since the epoch at midnight UTC (#198). The entries refuse a near date before the
 * business day (#551), so a test's or a benchmark's swap takes its dates from here: a fixed date passes, and the swap
 * is then refused, and a baseline binary saved with one fails.
 *
 * Not in TestAux.h, which includes GoogleTest: the benchmarks use it too, and they don't build with GoogleTest (#530).
 */
inline COP::DateTimeT swapSettlementDate(int daysAfter)
{
    using namespace std::chrono;
    const COP::DateTimeT now =
        static_cast<COP::DateTimeT>(duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
    const COP::u32 business = COP::Proc::ExpiryScheduler::tradeDateOf(COP::Proc::ExpiryScheduler::dayStartOf(now));
    const sys_days businessDay(
        year_month_day(year(static_cast<int>(business / 10000)), month(business / 100 % 100), day(business % 100)));
    return static_cast<COP::DateTimeT>(
        duration_cast<milliseconds>((businessDay + days(daysAfter)).time_since_epoch()).count());
}

/// swapSettlementDate()'s date as FIX's LocalMktDate, YYYYMMDD
inline std::string swapLocalMktDate(int daysAfter)
{
    using namespace std::chrono;
    const year_month_day date(floor<days>(sys_time<milliseconds>(milliseconds(swapSettlementDate(daysAfter)))));
    char text[16];
    std::snprintf(text, sizeof(text), "%04d%02u%02u", static_cast<int>(date.year()),
                  static_cast<unsigned>(date.month()), static_cast<unsigned>(date.day()));
    return text;
}

} // namespace test
