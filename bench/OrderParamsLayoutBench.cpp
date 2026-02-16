/**
 * Concurrent Order Processor library - Google Benchmark
 *
 * OrderParams hot/cold field layout benchmarks: measures sequential
 * access latency for hot-path fields vs cold fields to validate
 * the cache-line-aware struct reordering.
 */

#include <benchmark/benchmark.h>
#include <vector>
#include <cstring>
#include <memory>

#include "DataModelDef.h"
#include "WideDataStorage.h"
#include "IdTGenerator.h"
#include "Logger.h"

using namespace COP;
using namespace COP::Store;

namespace {

// =============================================================================
// Setup: Singleton lifecycle for OrderEntry construction
// =============================================================================

class LayoutBenchSetup {
public:
    LayoutBenchSetup() {
        aux::ExchLogger::create();
        WideDataStorage::create();
        IdTGenerator::create();
    }

    ~LayoutBenchSetup() {
        IdTGenerator::destroy();
        WideDataStorage::destroy();
        aux::ExchLogger::destroy();
    }

    std::unique_ptr<OrderEntry> createOrder() {
        SourceIdT srcId, destId, accountId, clearingId, clOrdId, origClOrderID, execList, instrId;

        srcId = WideDataStorage::instance()->add(new StringT("CLNT"));
        destId = WideDataStorage::instance()->add(new StringT("NASDAQ"));

        auto* clOrd = new RawDataEntry(STRING_RAWDATATYPE, "BenchClOrd", 10);
        clOrdId = WideDataStorage::instance()->add(clOrd);

        auto* instr = new InstrumentEntry();
        instr->symbol_ = "BENCH";
        instr->securityId_ = "B1";
        instr->securityIdSource_ = "SRC";
        instrId = WideDataStorage::instance()->add(instr);

        auto* acct = new AccountEntry();
        acct->account_ = "ACT";
        acct->firm_ = "FRM";
        acct->type_ = PRINCIPAL_ACCOUNTTYPE;
        accountId = WideDataStorage::instance()->add(acct);

        auto* clr = new ClearingEntry();
        clr->firm_ = "CLR";
        clearingId = WideDataStorage::instance()->add(clr);

        auto* execLst = new ExecutionsT();
        execList = WideDataStorage::instance()->add(execLst);

        auto order = std::make_unique<OrderEntry>(
            srcId, destId, clOrdId, origClOrderID,
            instrId, accountId, clearingId, execList);

        order->price_ = 100.0;
        order->leavesQty_ = 1000;
        order->cumQty_ = 0;
        order->orderQty_ = 1000;
        order->status_ = NEW_ORDSTATUS;
        order->side_ = BUY_SIDE;
        order->ordType_ = LIMIT_ORDERTYPE;
        order->tif_ = DAY_TIF;
        order->avgPx_ = 100.0;
        order->creationTime_ = 1000000;
        order->lastUpdateTime_ = 2000000;

        return order;
    }
};

} // namespace

// =============================================================================
// Hot Field Access (first cache lines: price, qty, status, side)
// =============================================================================

static void BM_HotFieldAccess(benchmark::State& state) {
    LayoutBenchSetup setup;
    const int count = static_cast<int>(state.range(0));

    // Create orders
    std::vector<std::unique_ptr<OrderEntry>> orders;
    orders.reserve(count);
    for (int i = 0; i < count; ++i) {
        orders.push_back(setup.createOrder());
        orders.back()->price_ = 100.0 + i;
        orders.back()->leavesQty_ = 1000 - i;
    }

    for (auto _ : state) {
        double totalPrice = 0;
        int totalLeaves = 0;
        for (int i = 0; i < count; ++i) {
            totalPrice += orders[i]->price_;
            totalLeaves += orders[i]->leavesQty_;
            benchmark::DoNotOptimize(orders[i]->status_);
            benchmark::DoNotOptimize(orders[i]->side_);
        }
        benchmark::DoNotOptimize(totalPrice);
        benchmark::DoNotOptimize(totalLeaves);
    }

    state.SetItemsProcessed(state.iterations() * count);
}
BENCHMARK(BM_HotFieldAccess)->Arg(64)->Arg(256)->Arg(1024);

// =============================================================================
// Warm Field Access (timestamps, settlement, capacity)
// =============================================================================

static void BM_WarmFieldAccess(benchmark::State& state) {
    LayoutBenchSetup setup;
    const int count = static_cast<int>(state.range(0));

    std::vector<std::unique_ptr<OrderEntry>> orders;
    orders.reserve(count);
    for (int i = 0; i < count; ++i) {
        orders.push_back(setup.createOrder());
        orders.back()->avgPx_ = 99.5 + i * 0.01;
        orders.back()->creationTime_ = 1000000 + i;
    }

    for (auto _ : state) {
        double totalAvg = 0;
        long long totalTime = 0;
        for (int i = 0; i < count; ++i) {
            totalAvg += orders[i]->avgPx_;
            totalTime += orders[i]->creationTime_;
            benchmark::DoNotOptimize(orders[i]->lastUpdateTime_);
        }
        benchmark::DoNotOptimize(totalAvg);
        benchmark::DoNotOptimize(totalTime);
    }

    state.SetItemsProcessed(state.iterations() * count);
}
BENCHMARK(BM_WarmFieldAccess)->Arg(64)->Arg(256)->Arg(1024);

// =============================================================================
// Mixed Hot+Warm Access (typical order matching pattern)
// =============================================================================

static void BM_MixedHotWarmAccess(benchmark::State& state) {
    LayoutBenchSetup setup;
    const int count = static_cast<int>(state.range(0));

    std::vector<std::unique_ptr<OrderEntry>> orders;
    orders.reserve(count);
    for (int i = 0; i < count; ++i) {
        orders.push_back(setup.createOrder());
    }

    for (auto _ : state) {
        for (int i = 0; i < count; ++i) {
            // Simulate order matching: read hot + update warm
            double px = orders[i]->price_;
            int leaves = orders[i]->leavesQty_;
            benchmark::DoNotOptimize(px);
            benchmark::DoNotOptimize(leaves);

            orders[i]->cumQty_ += 100;
            orders[i]->leavesQty_ -= 100;
            orders[i]->avgPx_ = (orders[i]->avgPx_ + px) / 2.0;
            orders[i]->lastUpdateTime_ = 2000000;
        }
        benchmark::ClobberMemory();

        // Reset for next iteration
        for (int i = 0; i < count; ++i) {
            orders[i]->cumQty_ = 0;
            orders[i]->leavesQty_ = 1000;
        }
    }

    state.SetItemsProcessed(state.iterations() * count);
}
BENCHMARK(BM_MixedHotWarmAccess)->Arg(64)->Arg(256)->Arg(1024);

// =============================================================================
// OrderParams / OrderEntry Size Verification (informational)
// =============================================================================

static void BM_OrderParamsSizeInfo(benchmark::State& state) {
    for (auto _ : state) {
        size_t sz = sizeof(OrderEntry);
        benchmark::DoNotOptimize(sz);
    }
    state.counters["sizeof_OrderEntry"] = static_cast<double>(sizeof(OrderEntry));
    state.counters["sizeof_OrderParams"] = static_cast<double>(sizeof(OrderParams));
    state.counters["alignof_OrderParams"] = static_cast<double>(alignof(OrderParams));
}
BENCHMARK(BM_OrderParamsSizeInfo);
