/**
 * Concurrent Order Processor library - Google Test
 *
 * Migrated from legacy testEventBenchmark.cpp
 *
 * Author: Sergey Mikhailik
 * Test Implementation: 2026
 *
 * Copyright (C) 2009-2026 Sergey Mikhailik
 *
 * Distributed under the GNU General Public License (GPL).
 */

#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <vector>

#include "SubscrManager.h"
#include "EventDef.h"
#include "OrderFilter.h"
#include "FilterImpl.h"
#include "TypesDef.h"
#include "DataModelDef.h"
#include "WideDataStorage.h"
#include "IdTGenerator.h"

using namespace COP;
using namespace COP::Store;
using namespace COP::SubscrMgr;
using namespace COP::Impl;

// =============================================================================
// Test Fixture
// =============================================================================

class EventBenchmarkTest : public ::testing::Test {
protected:
    static constexpr size_t NUM_INSTRUMENTS = 10000;

    void SetUp() override {
        WideDataStorage::create();
        IdTGenerator::create();
        SubscriptionMgr::create();

        srcId_ = WideDataStorage::instance()->add(new StringT("CLNT"));
        destId_ = WideDataStorage::instance()->add(new StringT("NASDAQ"));

        auto* account = new AccountEntry();
        account->type_ = PRINCIPAL_ACCOUNTTYPE;
        account->firm_ = "ACTFirm";
        account->account_ = "ACT";
        account->id_ = IdT();
        accountId_ = WideDataStorage::instance()->add(account);

        auto* clearing = new ClearingEntry();
        clearing->firm_ = "CLRFirm";
        clearingId_ = WideDataStorage::instance()->add(clearing);

        auto* execLst = new ExecutionsT();
        execListId_ = WideDataStorage::instance()->add(execLst);

        symbols_.resize(NUM_INSTRUMENTS);
        instrumentIds_.resize(NUM_INSTRUMENTS);

        size_t pos = 0;
        std::string symb;
        for (char a = 'A'; a < 'Z' && pos < NUM_INSTRUMENTS; ++a) {
            for (char b = 'A'; b < 'Z' && pos < NUM_INSTRUMENTS; ++b) {
                for (char c = 'A'; c < 'Z' && pos < NUM_INSTRUMENTS; ++c) {
                    auto* instr = new InstrumentEntry();
                    symb.clear();
                    symb += a;
                    symb += b;
                    symb += c;
                    symbols_[pos] = symb;
                    instr->symbol_ = symb;
                    instr->securityId_ = "AAA";
                    instr->securityIdSource_ = "AAASrc";
                    instrumentIds_[pos] = WideDataStorage::instance()->add(instr);
                    ++pos;
                }
            }
        }
    }

    void TearDown() override {
        SubscriptionMgr::destroy();
        IdTGenerator::destroy();
        WideDataStorage::destroy();
    }

    // Helper: create an order with only an instrument set (for simple filter scenarios)
    OrderEntry makeSimpleOrder(size_t index) {
        SourceIdT clOrderId, origClOrderID;
        return OrderEntry(srcId_, destId_, clOrderId, origClOrderID,
                          instrumentIds_[index], accountId_, clearingId_, execListId_);
    }

    // Helper: create a fully-populated order matching complex filter criteria
    OrderEntry makeComplexOrder(size_t index) {
        SourceIdT clOrderId, origClOrderID;
        OrderEntry order(srcId_, destId_, clOrderId, origClOrderID,
                         instrumentIds_[index], accountId_, clearingId_, execListId_);
        order.execInstruct_.insert(NOT_HELD_INSTRUCTION);
        order.creationTime_ = 4;
        order.lastUpdateTime_ = 5;
        order.expireTime_ = 2;
        order.settlDate_ = 3;
        order.price_ = 10.51;
        order.stopPx_ = 11.51;
        order.avgPx_ = 12.51;
        order.dayAvgPx_ = 13.51;
        order.status_ = RECEIVEDNEW_ORDSTATUS;
        order.side_ = BUY_SIDE;
        order.ordType_ = LIMIT_ORDERTYPE;
        order.tif_ = GTD_TIF;
        order.settlType_ = _3_SETTLTYPE;
        order.capacity_ = PRINCIPAL_CAPACITY;
        order.currency_ = USD_CURRENCY;
        order.minQty_ = 101;
        order.orderQty_ = 111;
        order.leavesQty_ = 121;
        order.cumQty_ = 131;
        order.dayOrderQty_ = 141;
        order.dayCumQty_ = 151;
        return order;
    }

    // Helper: build a complex filter with many field conditions and an instrument filter
    OrderFilter* makeComplexFilterWithInstrument(InstrumentFilter& instrFlt) {
        auto* filter = new OrderFilter();
        filter->addFilter(new OrderStatusEqualFilter(RECEIVEDNEW_ORDSTATUS));
        filter->addFilter(new SideEqualFilter(BUY_SIDE));
        filter->addFilter(new OrderTypeEqualFilter(LIMIT_ORDERTYPE));
        TIFInFilter::ValuesT tif;
        tif.insert(GTD_TIF);
        tif.insert(GTC_TIF);
        filter->addFilter(new TIFInFilter(tif));
        filter->addFilter(new SettlTypeEqualFilter(_3_SETTLTYPE));
        filter->addFilter(new CapacityEqualFilter(PRINCIPAL_CAPACITY));
        filter->addFilter(new CurrencyEqualFilter(USD_CURRENCY));
        InstructionsInFilter::ValuesT instr;
        instr.insert(STAY_ON_OFFER_SIDE_INSTRUCTION);
        instr.insert(NOT_HELD_INSTRUCTION);
        filter->addFilter(new InstructionsInFilter(instr));
        filter->addFilter(new DestinationFilter(new StringEqualFilter("NASDAQ")));
        filter->addFilter(new SourceFilter(new StringEqualFilter("CLNT")));
        filter->addFilter(new PxFilter(new PriceGreaterFilter(10.50)));
        filter->addFilter(new StopPxFilter(new PriceGreaterFilter(11.50)));
        filter->addFilter(new AvgPxFilter(new PriceGreaterFilter(12.50)));
        filter->addFilter(new DayAvgPxFilter(new PriceGreaterFilter(13.50)));
        filter->addFilter(new MinQtyFilter(new QuantityGreaterFilter(100)));
        filter->addFilter(new OrderQtyFilter(new QuantityGreaterFilter(110)));
        filter->addFilter(new LeavesQtyFilter(new QuantityGreaterFilter(120)));
        filter->addFilter(new CumQtyFilter(new QuantityGreaterFilter(130)));
        filter->addFilter(new DayOrderQtyFilter(new QuantityGreaterFilter(140)));
        filter->addFilter(new DayCumQtyFilter(new QuantityGreaterFilter(150)));
        filter->addFilter(new ExpireTimeFilter(new DateTimeGreaterFilter(1)));
        filter->addFilter(new SettlDateFilter(new DateTimeGreaterFilter(2)));
        filter->addFilter(new CreationTimeFilter(new DateTimeGreaterFilter(3)));
        filter->addFilter(new LastUpdateTimeFilter(new DateTimeGreaterFilter(4)));

        filter->addInstrumentFilter(&instrFlt);

        AccountFilter accFlt;
        accFlt.addFilter(new AccountAccountFilter(new StringEqualFilter("ACT")));
        accFlt.addFilter(new AccountFirmFilter(new StringEqualFilter("ACTFirm")));
        accFlt.addFilter(new AccountTypeEqualFilter(PRINCIPAL_ACCOUNTTYPE));
        filter->addAccountFilter(&accFlt);

        ClearingFilter clrFlt;
        clrFlt.addFilter(new ClearingFirmFilter(new StringEqualFilter("CLRFirm")));
        filter->addClearingFilter(&clrFlt);

        return filter;
    }

    SourceIdT srcId_, destId_, accountId_, clearingId_, execListId_;
    std::vector<std::string> symbols_;
    std::vector<SourceIdT> instrumentIds_;
};

// =============================================================================
// Simple Equal Filter Scenario (one subscription per symbol)
// =============================================================================

TEST_F(EventBenchmarkTest, SimpleEqualFilter_AddSubscriptions) {
    SubscriberIdT handlerId(1, 0);

    for (size_t i = 0; i < symbols_.size(); ++i) {
        auto* filter = new OrderFilter();
        InstrumentFilter instrFlt;
        auto* eqSymblFlt = new StringEqualFilter(symbols_[i]);
        auto* symblFlt = new InstrumentSymbolFilter(eqSymblFlt);
        instrFlt.addFilter(symblFlt);
        filter->addInstrumentFilter(&instrFlt);
        SubscriptionMgr::instance()->addSubscription("aaa", filter, handlerId);
    }

    // Verify lookup finds a subscriber for the first instrument
    MatchedSubscribersT subscribers;
    OrderEntry order = makeSimpleOrder(0);
    SubscriptionMgr::instance()->getSubscribers(order, &subscribers);
    EXPECT_FALSE(subscribers.empty());

    SubscriptionMgr::instance()->removeSubscriptions(handlerId);
}

TEST_F(EventBenchmarkTest, SimpleEqualFilter_LookupAll) {
    SubscriberIdT handlerId(1, 0);

    for (size_t i = 0; i < symbols_.size(); ++i) {
        auto* filter = new OrderFilter();
        InstrumentFilter instrFlt;
        auto* eqSymblFlt = new StringEqualFilter(symbols_[i]);
        auto* symblFlt = new InstrumentSymbolFilter(eqSymblFlt);
        instrFlt.addFilter(symblFlt);
        filter->addInstrumentFilter(&instrFlt);
        SubscriptionMgr::instance()->addSubscription("aaa", filter, handlerId);
    }

    MatchedSubscribersT subscribers;
    size_t matchCount = 0;
    for (size_t i = 0; i < instrumentIds_.size(); ++i) {
        subscribers.clear();
        OrderEntry order = makeSimpleOrder(i);
        SubscriptionMgr::instance()->getSubscribers(order, &subscribers);
        if (!subscribers.empty()) {
            ++matchCount;
        }
    }

    EXPECT_GT(matchCount, 0u);

    SubscriptionMgr::instance()->removeSubscriptions(handlerId);
}

TEST_F(EventBenchmarkTest, SimpleEqualFilter_LookupFirst) {
    SubscriberIdT handlerId(1, 0);

    for (size_t i = 0; i < symbols_.size(); ++i) {
        auto* filter = new OrderFilter();
        InstrumentFilter instrFlt;
        auto* eqSymblFlt = new StringEqualFilter(symbols_[i]);
        auto* symblFlt = new InstrumentSymbolFilter(eqSymblFlt);
        instrFlt.addFilter(symblFlt);
        filter->addInstrumentFilter(&instrFlt);
        SubscriptionMgr::instance()->addSubscription("aaa", filter, handlerId);
    }

    MatchedSubscribersT subscribers;
    OrderEntry order = makeSimpleOrder(0);
    SubscriptionMgr::instance()->getSubscribers(order, &subscribers);
    EXPECT_FALSE(subscribers.empty());

    SubscriptionMgr::instance()->removeSubscriptions(handlerId);
}

TEST_F(EventBenchmarkTest, SimpleEqualFilter_LookupLast) {
    SubscriberIdT handlerId(1, 0);

    for (size_t i = 0; i < symbols_.size(); ++i) {
        auto* filter = new OrderFilter();
        InstrumentFilter instrFlt;
        auto* eqSymblFlt = new StringEqualFilter(symbols_[i]);
        auto* symblFlt = new InstrumentSymbolFilter(eqSymblFlt);
        instrFlt.addFilter(symblFlt);
        filter->addInstrumentFilter(&instrFlt);
        SubscriptionMgr::instance()->addSubscription("aaa", filter, handlerId);
    }

    MatchedSubscribersT subscribers;
    OrderEntry order = makeSimpleOrder(instrumentIds_.size() - 1);
    SubscriptionMgr::instance()->getSubscribers(order, &subscribers);
    EXPECT_FALSE(subscribers.empty());

    SubscriptionMgr::instance()->removeSubscriptions(handlerId);
}

TEST_F(EventBenchmarkTest, SimpleEqualFilter_RemoveAll) {
    SubscriberIdT handlerId(1, 0);

    for (size_t i = 0; i < symbols_.size(); ++i) {
        auto* filter = new OrderFilter();
        InstrumentFilter instrFlt;
        auto* eqSymblFlt = new StringEqualFilter(symbols_[i]);
        auto* symblFlt = new InstrumentSymbolFilter(eqSymblFlt);
        instrFlt.addFilter(symblFlt);
        filter->addInstrumentFilter(&instrFlt);
        SubscriptionMgr::instance()->addSubscription("aaa", filter, handlerId);
    }

    SubscriptionMgr::instance()->removeSubscriptions(handlerId);

    // After removal, lookup should find nothing
    MatchedSubscribersT subscribers;
    OrderEntry order = makeSimpleOrder(0);
    SubscriptionMgr::instance()->getSubscribers(order, &subscribers);
    EXPECT_TRUE(subscribers.empty());
}

// =============================================================================
// Simple Match (regex) Filter Scenario (deduped by 2-char prefix)
// =============================================================================

TEST_F(EventBenchmarkTest, SimpleMatchFilter_AddSubscriptions) {
    SubscriberIdT handlerId(1, 0);
    size_t subscrCount = 0;
    std::string prevPattern;

    for (size_t i = 0; i < symbols_.size(); ++i) {
        std::string part(symbols_[i].c_str(), 2);
        if (prevPattern != part) {
            auto* filter = new OrderFilter();
            InstrumentFilter instrFlt;
            auto* eqSymblFlt = new StringMatchFilter(part + ".?");
            auto* symblFlt = new InstrumentSymbolFilter(eqSymblFlt);
            instrFlt.addFilter(symblFlt);
            filter->addInstrumentFilter(&instrFlt);
            SubscriptionMgr::instance()->addSubscription("aaa", filter, handlerId);
            ++subscrCount;
            prevPattern = part;
        }
    }

    // Should have far fewer subscriptions than instruments (one per 2-char prefix)
    EXPECT_GT(subscrCount, 0u);
    EXPECT_LT(subscrCount, NUM_INSTRUMENTS);

    SubscriptionMgr::instance()->removeSubscriptions(handlerId);
}

TEST_F(EventBenchmarkTest, SimpleMatchFilter_LookupAll) {
    SubscriberIdT handlerId(1, 0);
    std::string prevPattern;

    for (size_t i = 0; i < symbols_.size(); ++i) {
        std::string part(symbols_[i].c_str(), 2);
        if (prevPattern != part) {
            auto* filter = new OrderFilter();
            InstrumentFilter instrFlt;
            auto* eqSymblFlt = new StringMatchFilter(part + ".?");
            auto* symblFlt = new InstrumentSymbolFilter(eqSymblFlt);
            instrFlt.addFilter(symblFlt);
            filter->addInstrumentFilter(&instrFlt);
            SubscriptionMgr::instance()->addSubscription("aaa", filter, handlerId);
            prevPattern = part;
        }
    }

    MatchedSubscribersT subscribers;
    size_t matchCount = 0;
    for (size_t i = 0; i < instrumentIds_.size(); ++i) {
        subscribers.clear();
        OrderEntry order = makeSimpleOrder(i);
        SubscriptionMgr::instance()->getSubscribers(order, &subscribers);
        if (!subscribers.empty()) {
            ++matchCount;
        }
    }

    EXPECT_GT(matchCount, 0u);

    SubscriptionMgr::instance()->removeSubscriptions(handlerId);
}

TEST_F(EventBenchmarkTest, SimpleMatchFilter_LookupFirstAndLast) {
    SubscriberIdT handlerId(1, 0);
    std::string prevPattern;

    for (size_t i = 0; i < symbols_.size(); ++i) {
        std::string part(symbols_[i].c_str(), 2);
        if (prevPattern != part) {
            auto* filter = new OrderFilter();
            InstrumentFilter instrFlt;
            auto* eqSymblFlt = new StringMatchFilter(part + ".?");
            auto* symblFlt = new InstrumentSymbolFilter(eqSymblFlt);
            instrFlt.addFilter(symblFlt);
            filter->addInstrumentFilter(&instrFlt);
            SubscriptionMgr::instance()->addSubscription("aaa", filter, handlerId);
            prevPattern = part;
        }
    }

    {
        MatchedSubscribersT subscribers;
        OrderEntry order = makeSimpleOrder(0);
        SubscriptionMgr::instance()->getSubscribers(order, &subscribers);
        EXPECT_FALSE(subscribers.empty());
    }

    {
        MatchedSubscribersT subscribers;
        OrderEntry order = makeSimpleOrder(instrumentIds_.size() - 1);
        SubscriptionMgr::instance()->getSubscribers(order, &subscribers);
        EXPECT_FALSE(subscribers.empty());
    }

    SubscriptionMgr::instance()->removeSubscriptions(handlerId);
}

// =============================================================================
// Complex Equal Filter Scenario (many filter fields, exact symbol match)
// =============================================================================

TEST_F(EventBenchmarkTest, ComplexEqualFilter_AddSubscriptions) {
    SubscriberIdT handlerId(1, 0);

    for (size_t i = 0; i < symbols_.size(); ++i) {
        InstrumentFilter instrFlt;
        auto* eqSymblFlt = new StringEqualFilter(symbols_[i]);
        auto* symblFlt = new InstrumentSymbolFilter(eqSymblFlt);
        instrFlt.addFilter(symblFlt);
        instrFlt.addFilter(new InstrumentSecurityIdFilter(new StringEqualFilter("AAA")));
        instrFlt.addFilter(new InstrumentSecurityIdSourceFilter(new StringEqualFilter("AAASrc")));

        OrderFilter* filter = makeComplexFilterWithInstrument(instrFlt);
        SubscriptionMgr::instance()->addSubscription("aaa", filter, handlerId);
    }

    MatchedSubscribersT subscribers;
    OrderEntry order = makeComplexOrder(0);
    SubscriptionMgr::instance()->getSubscribers(order, &subscribers);
    EXPECT_FALSE(subscribers.empty());

    SubscriptionMgr::instance()->removeSubscriptions(handlerId);
}

TEST_F(EventBenchmarkTest, ComplexEqualFilter_LookupAll) {
    SubscriberIdT handlerId(1, 0);

    for (size_t i = 0; i < symbols_.size(); ++i) {
        InstrumentFilter instrFlt;
        auto* eqSymblFlt = new StringEqualFilter(symbols_[i]);
        auto* symblFlt = new InstrumentSymbolFilter(eqSymblFlt);
        instrFlt.addFilter(symblFlt);
        instrFlt.addFilter(new InstrumentSecurityIdFilter(new StringEqualFilter("AAA")));
        instrFlt.addFilter(new InstrumentSecurityIdSourceFilter(new StringEqualFilter("AAASrc")));

        OrderFilter* filter = makeComplexFilterWithInstrument(instrFlt);
        SubscriptionMgr::instance()->addSubscription("aaa", filter, handlerId);
    }

    MatchedSubscribersT subscribers;
    size_t matchCount = 0;
    for (size_t i = 0; i < instrumentIds_.size(); ++i) {
        subscribers.clear();
        OrderEntry order = makeComplexOrder(i);
        SubscriptionMgr::instance()->getSubscribers(order, &subscribers);
        if (!subscribers.empty()) {
            ++matchCount;
        }
    }

    EXPECT_GT(matchCount, 0u);

    SubscriptionMgr::instance()->removeSubscriptions(handlerId);
}

TEST_F(EventBenchmarkTest, ComplexEqualFilter_LookupFirstAndLast) {
    SubscriberIdT handlerId(1, 0);

    for (size_t i = 0; i < symbols_.size(); ++i) {
        InstrumentFilter instrFlt;
        auto* eqSymblFlt = new StringEqualFilter(symbols_[i]);
        auto* symblFlt = new InstrumentSymbolFilter(eqSymblFlt);
        instrFlt.addFilter(symblFlt);
        instrFlt.addFilter(new InstrumentSecurityIdFilter(new StringEqualFilter("AAA")));
        instrFlt.addFilter(new InstrumentSecurityIdSourceFilter(new StringEqualFilter("AAASrc")));

        OrderFilter* filter = makeComplexFilterWithInstrument(instrFlt);
        SubscriptionMgr::instance()->addSubscription("aaa", filter, handlerId);
    }

    {
        MatchedSubscribersT subscribers;
        OrderEntry order = makeComplexOrder(0);
        SubscriptionMgr::instance()->getSubscribers(order, &subscribers);
        EXPECT_FALSE(subscribers.empty());
    }

    {
        MatchedSubscribersT subscribers;
        OrderEntry order = makeComplexOrder(instrumentIds_.size() - 1);
        SubscriptionMgr::instance()->getSubscribers(order, &subscribers);
        EXPECT_FALSE(subscribers.empty());
    }

    SubscriptionMgr::instance()->removeSubscriptions(handlerId);
}

// =============================================================================
// Complex Match (regex) Filter Scenario (many fields, regex symbol match)
// =============================================================================

TEST_F(EventBenchmarkTest, ComplexMatchFilter_AddSubscriptions) {
    SubscriberIdT handlerId(1, 0);
    size_t subscrCount = 0;
    std::string prevPattern;

    for (size_t i = 0; i < symbols_.size(); ++i) {
        std::string part(symbols_[i].c_str(), 2);
        if (prevPattern != part) {
            InstrumentFilter instrFlt;
            auto* eqSymblFlt = new StringMatchFilter(part + ".?");
            auto* symblFlt = new InstrumentSymbolFilter(eqSymblFlt);
            instrFlt.addFilter(symblFlt);
            instrFlt.addFilter(new InstrumentSecurityIdFilter(new StringEqualFilter("AAA")));
            instrFlt.addFilter(new InstrumentSecurityIdSourceFilter(new StringEqualFilter("AAASrc")));

            OrderFilter* filter = makeComplexFilterWithInstrument(instrFlt);
            SubscriptionMgr::instance()->addSubscription("aaa", filter, handlerId);
            ++subscrCount;
            prevPattern = part;
        }
    }

    EXPECT_GT(subscrCount, 0u);
    EXPECT_LT(subscrCount, NUM_INSTRUMENTS);

    SubscriptionMgr::instance()->removeSubscriptions(handlerId);
}

TEST_F(EventBenchmarkTest, ComplexMatchFilter_LookupAll) {
    SubscriberIdT handlerId(1, 0);
    std::string prevPattern;

    for (size_t i = 0; i < symbols_.size(); ++i) {
        std::string part(symbols_[i].c_str(), 2);
        if (prevPattern != part) {
            InstrumentFilter instrFlt;
            auto* eqSymblFlt = new StringMatchFilter(part + ".?");
            auto* symblFlt = new InstrumentSymbolFilter(eqSymblFlt);
            instrFlt.addFilter(symblFlt);
            instrFlt.addFilter(new InstrumentSecurityIdFilter(new StringEqualFilter("AAA")));
            instrFlt.addFilter(new InstrumentSecurityIdSourceFilter(new StringEqualFilter("AAASrc")));

            OrderFilter* filter = makeComplexFilterWithInstrument(instrFlt);
            SubscriptionMgr::instance()->addSubscription("aaa", filter, handlerId);
            prevPattern = part;
        }
    }

    MatchedSubscribersT subscribers;
    size_t matchCount = 0;
    for (size_t i = 0; i < instrumentIds_.size(); ++i) {
        subscribers.clear();
        OrderEntry order = makeComplexOrder(i);
        SubscriptionMgr::instance()->getSubscribers(order, &subscribers);
        if (!subscribers.empty()) {
            ++matchCount;
        }
    }

    EXPECT_GT(matchCount, 0u);

    SubscriptionMgr::instance()->removeSubscriptions(handlerId);
}

TEST_F(EventBenchmarkTest, ComplexMatchFilter_LookupFirstAndLast) {
    SubscriberIdT handlerId(1, 0);
    std::string prevPattern;

    for (size_t i = 0; i < symbols_.size(); ++i) {
        std::string part(symbols_[i].c_str(), 2);
        if (prevPattern != part) {
            InstrumentFilter instrFlt;
            auto* eqSymblFlt = new StringMatchFilter(part + ".?");
            auto* symblFlt = new InstrumentSymbolFilter(eqSymblFlt);
            instrFlt.addFilter(symblFlt);
            instrFlt.addFilter(new InstrumentSecurityIdFilter(new StringEqualFilter("AAA")));
            instrFlt.addFilter(new InstrumentSecurityIdSourceFilter(new StringEqualFilter("AAASrc")));

            OrderFilter* filter = makeComplexFilterWithInstrument(instrFlt);
            SubscriptionMgr::instance()->addSubscription("aaa", filter, handlerId);
            prevPattern = part;
        }
    }

    {
        MatchedSubscribersT subscribers;
        OrderEntry order = makeComplexOrder(0);
        SubscriptionMgr::instance()->getSubscribers(order, &subscribers);
        EXPECT_FALSE(subscribers.empty());
    }

    {
        MatchedSubscribersT subscribers;
        OrderEntry order = makeComplexOrder(instrumentIds_.size() - 1);
        SubscriptionMgr::instance()->getSubscribers(order, &subscribers);
        EXPECT_FALSE(subscribers.empty());
    }

    SubscriptionMgr::instance()->removeSubscriptions(handlerId);
}

// =============================================================================
// Spot-check lookups at specific positions across the instrument space
// =============================================================================

TEST_F(EventBenchmarkTest, SimpleEqualFilter_LookupAtMidpoints) {
    SubscriberIdT handlerId(1, 0);

    for (size_t i = 0; i < symbols_.size(); ++i) {
        auto* filter = new OrderFilter();
        InstrumentFilter instrFlt;
        auto* eqSymblFlt = new StringEqualFilter(symbols_[i]);
        auto* symblFlt = new InstrumentSymbolFilter(eqSymblFlt);
        instrFlt.addFilter(symblFlt);
        filter->addInstrumentFilter(&instrFlt);
        SubscriptionMgr::instance()->addSubscription("aaa", filter, handlerId);
    }

    // Spot-check at positions matching the legacy test's specific indices
    const size_t indices[] = {0, 4998, 4999, 5000, 5001, instrumentIds_.size() - 1};
    for (size_t idx : indices) {
        MatchedSubscribersT subscribers;
        OrderEntry order = makeSimpleOrder(idx);
        SubscriptionMgr::instance()->getSubscribers(order, &subscribers);
        EXPECT_FALSE(subscribers.empty()) << "No subscriber found at index " << idx;
    }

    SubscriptionMgr::instance()->removeSubscriptions(handlerId);
}
