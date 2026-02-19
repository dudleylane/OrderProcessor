/**
 Concurrent Order Processor library

 Authors: dudleylane, Claude

 Copyright (C) 2026 dudleylane

 Distributed under the GNU General Public License (GPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#include "PGWriteBehind.h"

#include <pqxx/pqxx>
#include <spdlog/spdlog.h>
#include <chrono>
#include <algorithm>

namespace COP::PG {

namespace {

const char* UPSERT_INSTRUMENT = "pg_upsert_instrument";
const char* UPSERT_ACCOUNT    = "pg_upsert_account";
const char* UPSERT_CLEARING   = "pg_upsert_clearing";
const char* UPSERT_ORDER      = "pg_upsert_order";

const char* UPSERT_INSTRUMENT_SQL =
    "INSERT INTO instruments (symbol, security_id, security_id_source) "
    "VALUES ($1, $2, $3) "
    "ON CONFLICT (symbol) DO UPDATE SET "
    "security_id = EXCLUDED.security_id, "
    "security_id_source = EXCLUDED.security_id_source";

const char* UPSERT_ACCOUNT_SQL =
    "INSERT INTO accounts (account, firm, type) "
    "VALUES ($1, $2, $3::account_type) "
    "ON CONFLICT (account) DO UPDATE SET "
    "firm = EXCLUDED.firm, "
    "type = EXCLUDED.type";

const char* UPSERT_CLEARING_SQL =
    "INSERT INTO clearing_firms (firm) "
    "VALUES ($1) "
    "ON CONFLICT (firm) DO NOTHING";

const char* UPSERT_ORDER_SQL =
    "INSERT INTO orders ("
    "  order_id, order_date, cl_order_id, orig_cl_order_id, "
    "  source, destination, side, ord_type, price, stop_px, "
    "  order_qty, min_qty, leaves_qty, cum_qty, avg_px, "
    "  day_order_qty, day_cum_qty, day_avg_px, "
    "  status, time_in_force, settl_type, capacity, currency, "
    "  instrument_id, account_id, clearing_firm_id, "
    "  expire_time, settl_date"
    ") VALUES ("
    "  $1, to_date($2::text, 'YYYYMMDD'), $3, $4, "
    "  $5, $6, $7::side, $8::order_type, $9, $10, "
    "  $11, $12, $13, $14, $15, "
    "  $16, $17, $18, "
    "  $19::order_status, $20::time_in_force, $21::settl_type, $22::capacity, $23::currency, "
    "  (SELECT id FROM instruments WHERE symbol = $24), "
    "  (SELECT id FROM accounts WHERE account = $25), "
    "  (SELECT id FROM clearing_firms WHERE firm = $26), "
    "  CASE WHEN $27::bigint = 0 THEN NULL ELSE to_timestamp($27::bigint) END, "
    "  CASE WHEN $28::bigint = 0 THEN NULL ELSE to_date($28::text, 'YYYYMMDD') END"
    ") "
    "ON CONFLICT (order_id, order_date) DO UPDATE SET "
    "  status = EXCLUDED.status, "
    "  leaves_qty = EXCLUDED.leaves_qty, "
    "  cum_qty = EXCLUDED.cum_qty, "
    "  avg_px = EXCLUDED.avg_px, "
    "  day_order_qty = EXCLUDED.day_order_qty, "
    "  day_cum_qty = EXCLUDED.day_cum_qty, "
    "  day_avg_px = EXCLUDED.day_avg_px, "
    "  cl_order_id = EXCLUDED.cl_order_id, "
    "  orig_cl_order_id = EXCLUDED.orig_cl_order_id";

void prepareStatements(pqxx::connection& conn) {
    conn.prepare(UPSERT_INSTRUMENT, UPSERT_INSTRUMENT_SQL);
    conn.prepare(UPSERT_ACCOUNT, UPSERT_ACCOUNT_SQL);
    conn.prepare(UPSERT_CLEARING, UPSERT_CLEARING_SQL);
    conn.prepare(UPSERT_ORDER, UPSERT_ORDER_SQL);
}

void executeWrite(pqxx::connection& conn, const InstrumentWrite& w) {
    pqxx::work txn(conn);
    txn.exec(pqxx::prepped{UPSERT_INSTRUMENT},
        pqxx::params{w.symbol, w.securityId, w.securityIdSource});
    txn.commit();
}

void executeWrite(pqxx::connection& conn, const AccountWrite& w) {
    pqxx::work txn(conn);
    txn.exec(pqxx::prepped{UPSERT_ACCOUNT},
        pqxx::params{w.account, w.firm, w.type});
    txn.commit();
}

void executeWrite(pqxx::connection& conn, const ClearingWrite& w) {
    pqxx::work txn(conn);
    txn.exec(pqxx::prepped{UPSERT_CLEARING}, pqxx::params{w.firm});
    txn.commit();
}

void executeWrite(pqxx::connection& conn, const OrderWrite& w) {
    pqxx::work txn(conn);
    auto settlOpt = w.settlType.empty()
        ? std::optional<std::string>{}
        : std::optional<std::string>{w.settlType};
    txn.exec(pqxx::prepped{UPSERT_ORDER}, pqxx::params{
        static_cast<int64_t>(w.orderId),
        static_cast<int64_t>(w.orderDate),
        w.clOrderId, w.origClOrderId,
        w.source, w.destination,
        w.side, w.ordType,
        w.price, w.stopPx,
        static_cast<int>(w.orderQty), static_cast<int>(w.minQty),
        static_cast<int>(w.leavesQty), static_cast<int>(w.cumQty),
        w.avgPx,
        static_cast<int>(w.dayOrderQty), static_cast<int>(w.dayCumQty),
        w.dayAvgPx,
        w.status, w.tif,
        settlOpt,
        w.capacity, w.currency,
        w.instrumentSymbol, w.accountName, w.clearingFirm,
        static_cast<int64_t>(w.expireTime),
        static_cast<int64_t>(w.settlDate)});
    txn.commit();
}

} // anonymous namespace

PGWriteBehind::PGWriteBehind(const std::string& connString)
    : connString_(connString)
{
    thread_ = std::thread(&PGWriteBehind::run, this);
}

PGWriteBehind::~PGWriteBehind() {
    shutdown();
}

void PGWriteBehind::enqueue(PGWriteRequest&& req) {
    queue_.push(std::move(req));
    totalEnqueued_.fetch_add(1, std::memory_order_relaxed);
}

void PGWriteBehind::shutdown() {
    bool expected = false;
    if (!shutdown_.compare_exchange_strong(expected, true))
        return;
    if (thread_.joinable())
        thread_.join();
}

void PGWriteBehind::run() {
    using namespace std::chrono;

    std::unique_ptr<pqxx::connection> conn;
    auto backoff = milliseconds(1000);
    const auto maxBackoff = milliseconds(30000);

    auto tryConnect = [&]() -> bool {
        try {
            conn = std::make_unique<pqxx::connection>(connString_);
            prepareStatements(*conn);
            backoff = milliseconds(1000);
            spdlog::info("PGWriteBehind: connected to PostgreSQL");
            return true;
        } catch (const std::exception& e) {
            spdlog::error("PGWriteBehind: connection failed: {}", e.what());
            conn.reset();
            return false;
        }
    };

    tryConnect();

    while (true) {
        PGWriteRequest req;
        if (queue_.try_pop(req)) {
            if (!conn && !tryConnect()) {
                // Re-enqueue and back off
                queue_.push(std::move(req));
                std::this_thread::sleep_for(backoff);
                backoff = std::min(backoff * 2, maxBackoff);
                continue;
            }

            try {
                std::visit([&](const auto& w) { executeWrite(*conn, w); }, req);
                totalWritten_.fetch_add(1, std::memory_order_relaxed);
            } catch (const pqxx::broken_connection& e) {
                spdlog::error("PGWriteBehind: broken connection: {}", e.what());
                conn.reset();
                queue_.push(std::move(req));
                std::this_thread::sleep_for(backoff);
                backoff = std::min(backoff * 2, maxBackoff);
            } catch (const pqxx::sql_error& e) {
                spdlog::error("PGWriteBehind: SQL error: {} [query: {}]", e.what(), e.query());
                totalErrors_.fetch_add(1, std::memory_order_relaxed);
            } catch (const std::exception& e) {
                spdlog::error("PGWriteBehind: error: {}", e.what());
                totalErrors_.fetch_add(1, std::memory_order_relaxed);
            }
        } else {
            if (shutdown_.load(std::memory_order_acquire))
                break;
            std::this_thread::sleep_for(milliseconds(1));
        }
    }

    // Drain remaining items
    PGWriteRequest req;
    while (queue_.try_pop(req)) {
        if (!conn && !tryConnect())
            break;
        try {
            std::visit([&](const auto& w) { executeWrite(*conn, w); }, req);
            totalWritten_.fetch_add(1, std::memory_order_relaxed);
        } catch (const std::exception& e) {
            spdlog::error("PGWriteBehind: drain error: {}", e.what());
            totalErrors_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    spdlog::info("PGWriteBehind: shutdown complete (written={}, errors={})",
        totalWritten_.load(), totalErrors_.load());
}

} // namespace COP::PG
