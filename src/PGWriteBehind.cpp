/**
 Concurrent Order Processor library

 Authors: dudleylane, Claude

 Copyright (C) 2026 dudleylane

 Distributed under the GNU Affero General Public License (AGPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#include "PGWriteBehind.h"

#include <pqxx/pqxx>

#include <algorithm>
#include <chrono>
#include <memory>
#include <optional>
#include <vector>

#include "Logger.h"

namespace COP::PG
{

namespace
{

const char *UPSERT_INSTRUMENT = "pg_upsert_instrument";
const char *UPSERT_ACCOUNT = "pg_upsert_account";
const char *UPSERT_CLEARING = "pg_upsert_clearing";
const char *UPSERT_ORDER = "pg_upsert_order";
const char *DELETE_ORDER = "pg_delete_order";

const char *UPSERT_INSTRUMENT_SQL = "INSERT INTO instruments (symbol, security_id, security_id_source) "
                                    "VALUES ($1, $2, $3) "
                                    "ON CONFLICT (symbol) DO UPDATE SET "
                                    "security_id = EXCLUDED.security_id, "
                                    "security_id_source = EXCLUDED.security_id_source";

const char *UPSERT_ACCOUNT_SQL = "INSERT INTO accounts (account, firm, type) "
                                 "VALUES ($1, $2, $3::account_type) "
                                 "ON CONFLICT (account) DO UPDATE SET "
                                 "firm = EXCLUDED.firm, "
                                 "type = EXCLUDED.type";

const char *UPSERT_CLEARING_SQL = "INSERT INTO clearing_firms (firm) "
                                  "VALUES ($1) "
                                  "ON CONFLICT (firm) DO NOTHING";

// The day an order's id was drawn, in UTC: IdT::date_ is Unix seconds. It was read as YYYYMMDD, and the times as
// seconds and YYYYMMDD, where the engine keeps milliseconds (#151). The update covers every column but the key, since
// a rollback can take an order back to any earlier version.
const char *ORDER_DATE_SQL = "(to_timestamp($2::bigint) AT TIME ZONE 'UTC')::date";
const char *UPSERT_ORDER_SQL =
    "INSERT INTO orders ("
    "  order_id, order_date, cl_order_id, orig_cl_order_id, "
    "  source, destination, side, ord_type, price, stop_px, "
    "  order_qty, min_qty, leaves_qty, cum_qty, avg_px, "
    "  day_order_qty, day_cum_qty, day_avg_px, "
    "  status, time_in_force, settl_type, capacity, currency, "
    "  instrument_id, account_id, clearing_firm_id, "
    "  expire_time, settl_date, stop_triggered, day_start"
    ") VALUES ("
    "  $1, (to_timestamp($2::bigint) AT TIME ZONE 'UTC')::date, $3, $4, "
    "  $5, $6, $7::side, $8::order_type, $9, $10, "
    "  $11, $12, $13, $14, $15, "
    "  $16, $17, $18, "
    "  $19::order_status, $20::time_in_force, $21::settl_type, $22::capacity, $23::currency, "
    "  (SELECT id FROM instruments WHERE symbol = $24), "
    "  (SELECT id FROM accounts WHERE account = $25), "
    "  (SELECT id FROM clearing_firms WHERE firm = $26), "
    "  CASE WHEN $27::bigint = 0 THEN NULL ELSE to_timestamp($27::bigint::float8 / 1000) END, "
    "  CASE WHEN $28::bigint = 0 THEN NULL ELSE (to_timestamp($28::bigint::float8 / 1000) AT TIME ZONE 'UTC')::date END"
    ", $29"
    ", CASE WHEN $30::bigint = 0 THEN NULL ELSE to_timestamp($30::bigint::float8 / 1000) END"
    ") "
    "ON CONFLICT (order_id, order_date) DO UPDATE SET "
    "  cl_order_id = EXCLUDED.cl_order_id, orig_cl_order_id = EXCLUDED.orig_cl_order_id, "
    "  source = EXCLUDED.source, destination = EXCLUDED.destination, "
    "  side = EXCLUDED.side, ord_type = EXCLUDED.ord_type, price = EXCLUDED.price, stop_px = EXCLUDED.stop_px, "
    "  order_qty = EXCLUDED.order_qty, min_qty = EXCLUDED.min_qty, leaves_qty = EXCLUDED.leaves_qty, "
    "  cum_qty = EXCLUDED.cum_qty, avg_px = EXCLUDED.avg_px, "
    "  day_order_qty = EXCLUDED.day_order_qty, day_cum_qty = EXCLUDED.day_cum_qty, day_avg_px = EXCLUDED.day_avg_px, "
    "  status = EXCLUDED.status, time_in_force = EXCLUDED.time_in_force, settl_type = EXCLUDED.settl_type, "
    "  capacity = EXCLUDED.capacity, currency = EXCLUDED.currency, "
    "  instrument_id = EXCLUDED.instrument_id, account_id = EXCLUDED.account_id, "
    "  clearing_firm_id = EXCLUDED.clearing_firm_id, "
    "  expire_time = EXCLUDED.expire_time, settl_date = EXCLUDED.settl_date, stop_triggered = EXCLUDED.stop_triggered, "
    "  day_start = EXCLUDED.day_start";
const std::string DELETE_ORDER_SQL =
    std::string("DELETE FROM orders WHERE order_id = $1 AND order_date = ") + ORDER_DATE_SQL;

void prepareStatements(pqxx::connection &conn)
{
    conn.prepare(UPSERT_INSTRUMENT, UPSERT_INSTRUMENT_SQL);
    conn.prepare(UPSERT_ACCOUNT, UPSERT_ACCOUNT_SQL);
    conn.prepare(UPSERT_CLEARING, UPSERT_CLEARING_SQL);
    conn.prepare(UPSERT_ORDER, UPSERT_ORDER_SQL);
    conn.prepare(DELETE_ORDER, DELETE_ORDER_SQL);
}

void execute(pqxx::work &txn, const InstrumentWrite &w)
{
    txn.exec(pqxx::prepped{ UPSERT_INSTRUMENT }, pqxx::params{ w.symbol, w.securityId, w.securityIdSource });
}

void execute(pqxx::work &txn, const AccountWrite &w)
{
    txn.exec(pqxx::prepped{ UPSERT_ACCOUNT }, pqxx::params{ w.account, w.firm, w.type });
}

void execute(pqxx::work &txn, const ClearingWrite &w)
{
    txn.exec(pqxx::prepped{ UPSERT_CLEARING }, pqxx::params{ w.firm });
}

void execute(pqxx::work &txn, const OrderWrite &w)
{
    const auto settlType =
        w.settlType.empty() ? std::optional<std::string>{} : std::optional<std::string>{ w.settlType };
    // Quantities are BIGINT: an unsigned QuantityT doesn't fit INTEGER, and was cast to int (#151)
    txn.exec(pqxx::prepped{ UPSERT_ORDER }, pqxx::params{ static_cast<int64_t>(w.orderId),
                                                          static_cast<int64_t>(w.orderDate),
                                                          w.clOrderId,
                                                          w.origClOrderId,
                                                          w.source,
                                                          w.destination,
                                                          w.side,
                                                          w.ordType,
                                                          w.price,
                                                          w.stopPx,
                                                          static_cast<int64_t>(w.orderQty),
                                                          static_cast<int64_t>(w.minQty),
                                                          static_cast<int64_t>(w.leavesQty),
                                                          static_cast<int64_t>(w.cumQty),
                                                          w.avgPx,
                                                          static_cast<int64_t>(w.dayOrderQty),
                                                          static_cast<int64_t>(w.dayCumQty),
                                                          w.dayAvgPx,
                                                          w.status,
                                                          w.tif,
                                                          settlType,
                                                          w.capacity,
                                                          w.currency,
                                                          w.instrumentSymbol,
                                                          w.accountName,
                                                          w.clearingFirm,
                                                          static_cast<int64_t>(w.expireTime),
                                                          static_cast<int64_t>(w.settlDate),
                                                          w.stopTriggered,
                                                          static_cast<int64_t>(w.dayStart) });
}

void execute(pqxx::work &txn, const DeleteOrderWrite &w)
{
    txn.exec(pqxx::prepped{ DELETE_ORDER },
             pqxx::params{ static_cast<int64_t>(w.orderId), static_cast<int64_t>(w.orderDate) });
}

/// The row a request writes, or deletes (#231): an order's row is its (order_id, order_date), the table's key
std::string rowOf(const PGWriteRequest &request)
{
    struct Row
    {
        std::string operator()(const InstrumentWrite &w) const
        {
            return "instrument " + w.symbol;
        }
        std::string operator()(const AccountWrite &w) const
        {
            return "account " + w.account;
        }
        std::string operator()(const ClearingWrite &w) const
        {
            return "clearing " + w.firm;
        }
        std::string operator()(const OrderWrite &w) const
        {
            return "order " + std::to_string(w.orderId) + ' ' + std::to_string(w.orderDate);
        }
        std::string operator()(const DeleteOrderWrite &w) const
        {
            return "order " + std::to_string(w.orderId) + ' ' + std::to_string(w.orderDate);
        }
    };
    return std::visit(Row{}, request);
}

/// Writes requests [first, last) in one transaction
void writeAll(pqxx::connection &conn, std::vector<PGWriteRequest>::const_iterator first,
              std::vector<PGWriteRequest>::const_iterator last)
{
    pqxx::work txn(conn);
    for (auto it = first; it != last; ++it)
    {
        std::visit(
            [&txn](const auto &w)
            {
                execute(txn, w);
            },
            *it);
    }
    txn.commit();
}

} // anonymous namespace

std::string PGWriteBehind::withConnectTimeout(const std::string &connString)
{
    if (std::string::npos != connString.find("connect_timeout"))
    {
        return connString;
    }
    // A URI takes it as a query parameter, the keyword form as one more keyword
    const bool uri = (0 == connString.rfind("postgresql://", 0)) || (0 == connString.rfind("postgres://", 0));
    if (uri)
    {
        return connString + ((std::string::npos == connString.find('?')) ? "?" : "&") + "connect_timeout=5";
    }
    return connString + (connString.empty() ? "" : " ") + "connect_timeout=5";
}

PGWriteBehind::PGWriteBehind(const std::string &connString) : connString_(withConnectTimeout(connString))
{
    thread_ = std::thread(&PGWriteBehind::run, this);
}

PGWriteBehind::~PGWriteBehind()
{
    shutdown();
}

void PGWriteBehind::enqueue(PGWriteRequest &&req)
{
    std::string row = rowOf(req);
    bool replaced = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto queued = rows_.find(row);
        if (rows_.end() != queued)
        {
            queue_[queued->second - front_].request = std::move(req);
            replaced = true;
        }
        else
        {
            rows_.emplace(row, front_ + queue_.size());
            queue_.push_back(Queued{ std::move(row), std::move(req) });
        }
    }
    totalEnqueued_.fetch_add(1, std::memory_order_relaxed);
    if (replaced)
    {
        totalCoalesced_.fetch_add(1, std::memory_order_relaxed);
        return; // the writer already has that row to write
    }
    wake_.notify_one();
}

std::vector<std::string> PGWriteBehind::queuedRows() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> rows;
    rows.reserve(queue_.size());
    for (const Queued &queued : queue_)
    {
        rows.push_back(queued.row);
    }
    return rows;
}

void PGWriteBehind::shutdown()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    wake_.notify_one();
    if (thread_.joinable())
    {
        thread_.join();
    }
}

void PGWriteBehind::run()
{
    using namespace std::chrono;
    auto *log = aux::ExchLogger::instance();
    std::unique_ptr<pqxx::connection> conn;
    const auto firstBackoff = milliseconds(1000);
    const auto maxBackoff = milliseconds(30000);
    auto backoff = firstBackoff;
    bool everConnected = false;
    // Connections lost since a write last landed. The first is replaced at once, as one a firewall dropped while it sat
    // idle would be; after that the writer waits between attempts, as it does for a database it can't reach.
    unsigned lostInARow = 0;
    std::vector<PGWriteRequest> batch; // what is being written; kept until it lands, so nothing is reordered

    // Before connecting again: waits, twice as long each time. False when stopping, with what is left dropped and
    // counted: shutdown doesn't wait for a database that isn't there.
    const auto retryLater = [&]
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (stopping_)
        {
            const u64 dropped = batch.size() + queue_.size();
            front_ += queue_.size();
            queue_.clear();
            rows_.clear();
            batch.clear();
            totalDropped_.fetch_add(dropped, std::memory_order_relaxed);
            log->error("PGWriteBehind: " + std::to_string(dropped) +
                       " write(s) dropped at shutdown: PostgreSQL can't be reached");
            return false;
        }
        wake_.wait_for(lock, backoff,
                       [this]
                       {
                           return stopping_;
                       });
        backoff = std::min(backoff * 2, maxBackoff);
        return true;
    };
    const auto landed = [&](std::size_t requests)
    {
        totalWritten_.fetch_add(requests, std::memory_order_relaxed);
        lostInARow = 0;
        backoff = firstBackoff; // only once writes land: a database can accept connections and still drop each one
    };
    // Whether a write failed because the connection is gone, not because Postgres refused it: libpqxx's broken_connection,
    // or any error on a connection that no longer reads as open, such as in_doubt_error, a commit cut off. Its requests
    // are written again, which PGWriteRequest allows.
    const auto connectionLost = [&](const std::exception &e)
    {
        return (nullptr != dynamic_cast<const pqxx::broken_connection *>(&e)) || !conn->is_open();
    };

    while (true)
    {
        if (batch.empty())
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock,
                       [this]
                       {
                           return stopping_ || !queue_.empty();
                       });
            if (queue_.empty())
            {
                break; // stopping, and everything is written
            }
            // Taken rows leave rows_: a later request for one is queued again, and written after this batch
            const std::size_t n = std::min(queue_.size(), BATCH_SIZE);
            for (std::size_t i = 0; i < n; ++i)
            {
                rows_.erase(queue_.front().row);
                batch.push_back(std::move(queue_.front().request));
                queue_.pop_front();
                ++front_;
            }
        }

        if (!conn)
        {
            try
            {
                conn = std::make_unique<pqxx::connection>(connString_);
                prepareStatements(*conn);
                log->note(std::string("PGWriteBehind: connected to PostgreSQL") + (everConnected ? " again" : ""));
                everConnected = true;
            }
            catch (const std::exception &e)
            {
                conn.reset();
                log->error(std::string("PGWriteBehind: can't connect to PostgreSQL: ") + e.what());
                if (!retryLater())
                {
                    break;
                }
                continue;
            }
        }

        bool lost = false;
        try
        {
            writeAll(*conn, batch.cbegin(), batch.cend());
            landed(batch.size());
            batch.clear();
        }
        catch (const std::exception &e)
        {
            if (connectionLost(e))
            {
                // Written again, from the start of the batch, on the next connection
                log->error(std::string("PGWriteBehind: lost the connection: ") + e.what());
                lost = true;
            }
            else
            {
                // Postgres refused a request: write the batch one at a time, so the rest land and each refusal is
                // counted
                log->warn(std::string("PGWriteBehind: a batch was refused, writing it a request at a time: ") +
                          e.what());
                std::size_t done = 0;
                for (; done < batch.size(); ++done)
                {
                    try
                    {
                        writeAll(*conn, batch.cbegin() + done, batch.cbegin() + done + 1);
                        landed(1);
                    }
                    catch (const std::exception &failed)
                    {
                        if (connectionLost(failed))
                        {
                            log->error(std::string("PGWriteBehind: lost the connection: ") + failed.what());
                            lost = true;
                            break;
                        }
                        totalErrors_.fetch_add(1, std::memory_order_relaxed);
                        log->error(std::string("PGWriteBehind: PostgreSQL refused a write: ") + failed.what());
                    }
                }
                batch.erase(batch.begin(), batch.begin() + done);
            }
        }
        if (lost)
        {
            conn.reset();
            if ((1 < ++lostInARow) && !retryLater())
            {
                break;
            }
        }
    }

    log->note("PGWriteBehind: stopped, " + std::to_string(totalWritten_.load()) + " written, " +
              std::to_string(totalErrors_.load()) + " refused, " + std::to_string(totalDropped_.load()) + " dropped, " +
              std::to_string(totalCoalesced_.load()) + " replaced by a later write of their row while queued");
}

} // namespace COP::PG
