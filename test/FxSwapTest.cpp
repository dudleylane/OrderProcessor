/**
 Concurrent Order Processor library - FX Swap Test Suite

 Tests for FX Swap order matching, execution, validation, and codec round-trip.
*/

#include <gtest/gtest.h>

#include <limits>

#include "TestFixtures.h"
#include "TestAux.h"
#include "MockDefered.h"
#include "MockQueues.h"
#include "OrderMatcher.h"
#include "IncomingQueues.h"
#include "OutgoingQueues.h"
#include "OrderCodec.h"
#include "DeferedEvents.h"

using namespace COP;
using namespace COP::Proc;
using namespace COP::ACID;
using namespace COP::Store;
using namespace COP::Queues;
using namespace test;

namespace
{

class TestDeferedEventContainer : public DeferedEventContainer
{
public:
    void addDeferedEvent(DeferedEventBase *evnt) override
    {
        events_.push_back(evnt);
    }

    size_t deferedEventCount() const override
    {
        return events_.size();
    }

    void removeDeferedEventsFrom(size_t startIndex) override
    {
        if (startIndex >= events_.size())
        {
            return;
        }
        for (size_t i = startIndex; i < events_.size(); ++i)
        {
            delete events_[i];
        }
        events_.erase(events_.begin() + static_cast<std::ptrdiff_t>(startIndex), events_.end());
    }

    void clear()
    {
        for (auto *e : events_)
        {
            delete e;
        }
        events_.clear();
    }

    std::vector<DeferedEventBase *> events_;
};

class FxSwapTest : public ProcessorFixture
{
protected:
    void SetUp() override
    {
        ProcessorFixture::SetUp();

        deferedContainer_ = std::make_unique<TestDeferedEventContainer>();
        matcher_ = std::make_unique<OrderMatcher>();
        matcher_->init(deferedContainer_.get());

        inQueues_ = std::make_unique<IncomingQueues>();
        outQueues_ = std::make_unique<OutgoingQueues>();

        context_ = Context(OrderStorage::instance(), orderBook_.get(), inQueues_.get(), outQueues_.get(),
                           matcher_.get(), IdTGenerator::instance());
    }

    void TearDown() override
    {
        deferedContainer_->clear();
        deferedContainer_.reset();
        matcher_.reset();
        outQueues_.reset();
        inQueues_.reset();

        ProcessorFixture::TearDown();
    }

    OrderEntry *createAndSaveSwapOrder(Side side, PriceT nearPrice, PriceT farPrice, QuantityT qty, DateTimeT settlDate,
                                       DateTimeT farSettlDate)
    {
        auto order = createCorrectOrder(instrumentId1_);
        assignClOrderId(order.get());
        order->side_ = side;
        order->ordType_ = FXSWAP_ORDERTYPE;
        order->price_ = nearPrice;
        order->farPrice_ = farPrice;
        order->orderQty_ = qty;
        order->leavesQty_ = qty;
        order->settlDate_ = settlDate;
        order->farSettlDate_ = farSettlDate;

        return OrderStorage::instance()->save(*order, IdTGenerator::instance());
    }

    OrderEntry *createAndSaveLimitOrder(Side side, PriceT price, QuantityT qty)
    {
        auto order = createCorrectOrder(instrumentId1_);
        assignClOrderId(order.get());
        order->side_ = side;
        order->ordType_ = LIMIT_ORDERTYPE;
        order->price_ = price;
        order->orderQty_ = qty;
        order->leavesQty_ = qty;

        return OrderStorage::instance()->save(*order, IdTGenerator::instance());
    }

protected:
    std::unique_ptr<TestDeferedEventContainer> deferedContainer_;
    std::unique_ptr<OrderMatcher> matcher_;
    std::unique_ptr<IncomingQueues> inQueues_;
    std::unique_ptr<OutgoingQueues> outQueues_;
    Context context_;
};

// =============================================================================
// Validation Tests
// =============================================================================

TEST_F(FxSwapTest, SwapValidation_FarSettlDateMustBeAfterNear)
{
    auto order = createCorrectOrder(instrumentId1_);
    assignClOrderId(order.get());
    order->ordType_ = FXSWAP_ORDERTYPE;
    order->price_ = 1.0850;
    order->farPrice_ = 1.0872;
    order->settlDate_ = 1000;
    order->farSettlDate_ = 500; // BEFORE near — invalid
    order->status_ = RECEIVEDNEW_ORDSTATUS;

    std::string reason;
    EXPECT_FALSE(order->isValid(&reason));
    EXPECT_NE(std::string::npos, reason.find("far settlement date"));
}

TEST_F(FxSwapTest, SwapValidation_FarPriceMustBePositive)
{
    auto order = createCorrectOrder(instrumentId1_);
    assignClOrderId(order.get());
    order->ordType_ = FXSWAP_ORDERTYPE;
    order->price_ = 1.0850;
    order->farPrice_ = 0.0; // zero — invalid
    order->settlDate_ = 1000;
    order->farSettlDate_ = 2000;
    order->status_ = RECEIVEDNEW_ORDSTATUS;

    std::string reason;
    EXPECT_FALSE(order->isValid(&reason));
    EXPECT_NE(std::string::npos, reason.find("far leg price"));
}

TEST_F(FxSwapTest, SwapValidation_BothLegPricesMustBeAboveZero)
{
    // The near leg is held to the far leg's rule, as a limit order's price is (#188): at 0 a swap sold its near leg at
    // any bid. A NaN is refused on either leg
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const struct
    {
        double nearPrice, farPrice;
        const char *reason;
    } cases[] = { { 0.0, 1.0872, "near leg price" },
                  { -1.5, 1.0872, "near leg price" },
                  { nan, 1.0872, "near leg price" },
                  { 1.0850, nan, "far leg price" } };
    for (const auto &c : cases)
    {
        auto order = createCorrectOrder(instrumentId1_);
        assignClOrderId(order.get());
        order->ordType_ = FXSWAP_ORDERTYPE;
        order->price_ = c.nearPrice;
        order->farPrice_ = c.farPrice;
        order->settlDate_ = 1000;
        order->farSettlDate_ = 2000;
        order->status_ = RECEIVEDNEW_ORDSTATUS;

        std::string reason;
        EXPECT_FALSE(order->isValid(&reason)) << c.nearPrice << " / " << c.farPrice;
        EXPECT_NE(std::string::npos, reason.find(c.reason)) << reason;
    }
}

TEST_F(FxSwapTest, SwapValidation_ValidSwapOrder)
{
    auto order = createCorrectOrder(instrumentId1_);
    assignClOrderId(order.get());
    order->ordType_ = FXSWAP_ORDERTYPE;
    order->price_ = 1.0850;
    order->farPrice_ = 1.0872;
    order->settlDate_ = 1000;
    order->farSettlDate_ = 2000;
    order->status_ = RECEIVEDNEW_ORDSTATUS;

    std::string reason;
    EXPECT_TRUE(order->isValid(&reason));
}

// =============================================================================
// Matching Tests
// =============================================================================

TEST_F(FxSwapTest, SwapNoMatch_RestsOnBook)
{
    OrderEntry *buySwap = createAndSaveSwapOrder(BUY_SIDE, 1.0850, 1.0872, 100, 1000, 2000);
    buySwap->status_ = NEW_ORDSTATUS;
    orderBook_->add(*buySwap);

    // Match with no contra on book
    matcher_->match(buySwap, context_);

    // No deferred events — order rests
    EXPECT_EQ(0u, deferedContainer_->events_.size());
}

TEST_F(FxSwapTest, SwapDoesNotMatchLimitOrder)
{
    // Add a regular LIMIT sell order
    OrderEntry *limitSell = createAndSaveLimitOrder(SELL_SIDE, 1.0850, 100);
    limitSell->status_ = NEW_ORDSTATUS;
    orderBook_->add(*limitSell);

    // Try to match a swap buy against it
    OrderEntry *buySwap = createAndSaveSwapOrder(BUY_SIDE, 1.0850, 1.0872, 100, 1000, 2000);
    buySwap->status_ = NEW_ORDSTATUS;

    matcher_->match(buySwap, context_);

    // No match — swap requires contra swap
    EXPECT_EQ(0u, deferedContainer_->events_.size());
}

TEST_F(FxSwapTest, SwapMatchExact)
{
    // Contra sell swap on book
    OrderEntry *sellSwap = createAndSaveSwapOrder(SELL_SIDE, 1.0850, 1.0872, 100, 1000, 2000);
    sellSwap->status_ = NEW_ORDSTATUS;
    orderBook_->add(*sellSwap);

    // Aggressor buy swap
    OrderEntry *buySwap = createAndSaveSwapOrder(BUY_SIDE, 1.0850, 1.0872, 100, 1000, 2000);
    buySwap->status_ = NEW_ORDSTATUS;

    matcher_->match(buySwap, context_);

    // Should produce exactly 1 SwapExecutionDeferedEvent
    EXPECT_EQ(1u, deferedContainer_->events_.size());
}

TEST_F(FxSwapTest, SwapMatchPartial_ChainsMatchEvent)
{
    // Contra sell swap on book with qty 50
    OrderEntry *sellSwap = createAndSaveSwapOrder(SELL_SIDE, 1.0850, 1.0872, 50, 1000, 2000);
    sellSwap->status_ = NEW_ORDSTATUS;
    orderBook_->add(*sellSwap);

    // Aggressor buy swap with qty 100
    OrderEntry *buySwap = createAndSaveSwapOrder(BUY_SIDE, 1.0850, 1.0872, 100, 1000, 2000);
    buySwap->status_ = NEW_ORDSTATUS;

    matcher_->match(buySwap, context_);

    // Should produce SwapExecutionDeferedEvent + MatchSwapOrderDeferedEvent
    EXPECT_EQ(2u, deferedContainer_->events_.size());
}

TEST_F(FxSwapTest, SwapSettlDateMismatch_NoMatch)
{
    // Contra with different far settlement date
    OrderEntry *sellSwap = createAndSaveSwapOrder(SELL_SIDE, 1.0850, 1.0872, 100, 1000, 3000);
    sellSwap->status_ = NEW_ORDSTATUS;
    orderBook_->add(*sellSwap);

    // Aggressor with different far settlement date
    OrderEntry *buySwap = createAndSaveSwapOrder(BUY_SIDE, 1.0850, 1.0872, 100, 1000, 2000);
    buySwap->status_ = NEW_ORDSTATUS;

    matcher_->match(buySwap, context_);

    EXPECT_EQ(0u, deferedContainer_->events_.size());
}

TEST_F(FxSwapTest, SwapFarPriceIncompatible_NoMatch)
{
    // Contra sell swap wants 1.09 on far leg
    OrderEntry *sellSwap = createAndSaveSwapOrder(SELL_SIDE, 1.0850, 1.0900, 100, 1000, 2000);
    sellSwap->status_ = NEW_ORDSTATUS;
    orderBook_->add(*sellSwap);

    // Aggressor buy: near=BUY far=SELL. Far price 1.0872 < contra 1.0900 → compatible
    // Wait — BUY near means SELL far. Aggressor sells far at 1.0872.
    // Contra buys far at 1.0900. Aggressor ask (1.0872) < contra bid (1.0900) → MATCHES.
    // Let me make incompatible: aggressor far price 1.0950 > contra 1.0900
    OrderEntry *buySwap = createAndSaveSwapOrder(BUY_SIDE, 1.0850, 1.0950, 100, 1000, 2000);
    buySwap->status_ = NEW_ORDSTATUS;

    matcher_->match(buySwap, context_);

    // aggressor far (sell) at 1.0950 > contra far (buy) at 1.0900 → incompatible
    EXPECT_EQ(0u, deferedContainer_->events_.size());
}

// =============================================================================
// Codec Round-Trip Tests
// =============================================================================

TEST_F(FxSwapTest, SwapCodecRoundTrip)
{
    auto order = createCorrectOrder(instrumentId1_);
    assignClOrderId(order.get());
    OrderEntry *saved = OrderStorage::instance()->save(*order, IdTGenerator::instance());
    // the states Processor gives a stored order before it's persisted: New, and no cancel or replace (#150)
    saved->stateMachinePersistance_.stateZone1Id_ = 2;
    saved->stateMachinePersistance_.stateZone2Id_ = 8;

    saved->ordType_ = FXSWAP_ORDERTYPE;
    saved->price_ = 1.0850;
    saved->farPrice_ = 1.0872;
    saved->settlDate_ = 1714003200000ULL;
    saved->farSettlDate_ = 1716595200000ULL;

    // Encode
    std::string buf;
    IdT id;
    u32 version;
    COP::Codec::OrderCodec codec;
    codec.encode(*saved, &buf, &id, &version);

    EXPECT_EQ(1u, version);

    // Decode, in the format version a record's word carries (#149, #172), not the version encode() returns
    OrderEntry *decoded = codec.decode(id, COP::Codec::OrderCodec::FORMAT_VERSION, buf.data(), buf.size());
    ASSERT_NE(nullptr, decoded);

    EXPECT_EQ(FXSWAP_ORDERTYPE, decoded->ordType_);
    EXPECT_DOUBLE_EQ(1.0850, decoded->price_);
    EXPECT_DOUBLE_EQ(1.0872, decoded->farPrice_);
    EXPECT_EQ(1714003200000ULL, decoded->settlDate_);
    EXPECT_EQ(1716595200000ULL, decoded->farSettlDate_);

    delete decoded;
}

TEST_F(FxSwapTest, LegacyCodecCompat_V0DefaultsSwapFields)
{
    auto order = createCorrectOrder(instrumentId1_);
    assignClOrderId(order.get());
    OrderEntry *saved = OrderStorage::instance()->save(*order, IdTGenerator::instance());
    // the states Processor gives a stored order before it's persisted: New, and no cancel or replace (#150)
    saved->stateMachinePersistance_.stateZone1Id_ = 2;
    saved->stateMachinePersistance_.stateZone2Id_ = 8;
    saved->ordType_ = LIMIT_ORDERTYPE; // non-swap

    // A non-swap order: its record has no far leg
    std::string buf;
    IdT id;
    u32 version;
    COP::Codec::OrderCodec codec;
    codec.encode(*saved, &buf, &id, &version);

    EXPECT_EQ(0u, version);

    // Decode — swap fields should be defaults
    OrderEntry *decoded = codec.decode(id, COP::Codec::OrderCodec::FORMAT_VERSION, buf.data(), buf.size());
    ASSERT_NE(nullptr, decoded);

    EXPECT_DOUBLE_EQ(0.0, decoded->farPrice_);
    EXPECT_EQ(0u, decoded->farSettlDate_);

    delete decoded;
}

TEST_F(FxSwapTest, SwapCodecReadsTheFarLegWhateverTheVersion)
{
    // Every format version has a swap's far leg, and decode reads it by content. Records were once stored under the
    // storage's own version, 0 on an order's first write, and decode read the far leg only from version 1 on, so a swap
    // written once lost it (#144). Since #172 the far leg follows the execution instructions, in format version 1.
    auto order = createCorrectOrder(instrumentId1_);
    assignClOrderId(order.get());
    OrderEntry *saved = OrderStorage::instance()->save(*order, IdTGenerator::instance());
    // the states Processor gives a stored order before it's persisted: New, and no cancel or replace (#150)
    saved->stateMachinePersistance_.stateZone1Id_ = 2;
    saved->stateMachinePersistance_.stateZone2Id_ = 8;
    saved->ordType_ = FXSWAP_ORDERTYPE;
    saved->farPrice_ = 1.0872;
    saved->farSettlDate_ = 1716595200000ULL;

    std::string buf;
    IdT id;
    u32 version;
    COP::Codec::OrderCodec codec;
    codec.encode(*saved, &buf, &id, &version);

    // Format version 1, as encode() writes it; and version 0, the same record without the execution instructions ('.'
    // and their 4-byte mask), which come before the far leg ('.', farPrice_, '.', farSettlDate_)
    const size_t farLeg = 2 + sizeof(saved->farPrice_) + sizeof(saved->farSettlDate_);
    const size_t instructions = 1 + sizeof(u32);
    ASSERT_LT(farLeg + instructions, buf.size());
    const std::string v0 = buf.substr(0, buf.size() - farLeg - instructions) + buf.substr(buf.size() - farLeg);
    const std::pair<u32, std::string> records[] = { { 1u, buf }, { 0u, v0 } };
    for (const auto &[formatVersion, record] : records)
    {
        std::unique_ptr<OrderEntry> decoded(codec.decode(id, formatVersion, record.data(), record.size()));
        ASSERT_NE(nullptr, decoded) << "format version " << formatVersion;
        EXPECT_DOUBLE_EQ(1.0872, decoded->farPrice_) << "format version " << formatVersion;
        EXPECT_EQ(1716595200000ULL, decoded->farSettlDate_) << "format version " << formatVersion;
    }
}

TEST_F(FxSwapTest, SwapRecordCutAfterTheFarPriceIsRejected)
{
    // A record that ends right after farPrice_ must be rejected without reading past its end: decode used to read the
    // byte after it to look for the next '.' (#144). The copy is exactly as long as the record, so a sanitizer build
    // sees any read beyond it.
    auto order = createCorrectOrder(instrumentId1_);
    assignClOrderId(order.get());
    OrderEntry *saved = OrderStorage::instance()->save(*order, IdTGenerator::instance());
    // the states Processor gives a stored order before it's persisted: New, and no cancel or replace (#150)
    saved->stateMachinePersistance_.stateZone1Id_ = 2;
    saved->stateMachinePersistance_.stateZone2Id_ = 8;
    saved->ordType_ = FXSWAP_ORDERTYPE;
    saved->farPrice_ = 1.0872;
    saved->farSettlDate_ = 1716595200000ULL;

    std::string buf;
    IdT id;
    u32 version;
    COP::Codec::OrderCodec codec;
    codec.encode(*saved, &buf, &id, &version);

    const size_t cut = buf.size() - 1 - sizeof(saved->farSettlDate_); // drop the last '.' and farSettlDate_
    std::unique_ptr<char[]> exact(new char[cut]);
    memcpy(exact.get(), buf.data(), cut);
    EXPECT_THROW(delete codec.decode(id, COP::Codec::OrderCodec::FORMAT_VERSION, exact.get(), cut), std::runtime_error);
}

// =============================================================================
// Regression Tests
// =============================================================================

TEST_F(FxSwapTest, NonSwapOrderUnchanged_LimitMatch)
{
    // Standard LIMIT order matching should still work
    OrderEntry *sellLimit = createAndSaveLimitOrder(SELL_SIDE, 10.0, 100);
    sellLimit->status_ = NEW_ORDSTATUS;
    orderBook_->add(*sellLimit);

    OrderEntry *buyLimit = createAndSaveLimitOrder(BUY_SIDE, 10.0, 100);
    buyLimit->status_ = NEW_ORDSTATUS;

    matcher_->match(buyLimit, context_);

    // Should produce standard ExecutionDeferedEvent
    EXPECT_EQ(1u, deferedContainer_->events_.size());
}

// An outright order and a resting swap (#212): the swap trades only with a swap, so an outright order passes over it
TEST_F(FxSwapTest, LimitOrderDoesNotMatchASwap)
{
    OrderEntry *sellSwap = createAndSaveSwapOrder(SELL_SIDE, 1.0850, 1.0872, 100, 1000, 2000);
    sellSwap->status_ = NEW_ORDSTATUS;
    orderBook_->add(*sellSwap);

    OrderEntry *buyLimit = createAndSaveLimitOrder(BUY_SIDE, 1.0850, 100);
    buyLimit->status_ = NEW_ORDSTATUS;
    matcher_->match(buyLimit, context_);

    EXPECT_EQ(0u, deferedContainer_->events_.size()) << "an outright order traded a swap's near leg";
}

TEST_F(FxSwapTest, LimitOrderPassesOverASwapToTheOutrightOrderBehindIt)
{
    // The swap has the best price, and the outright sell behind it still trades
    OrderEntry *sellSwap = createAndSaveSwapOrder(SELL_SIDE, 1.0840, 1.0862, 100, 1000, 2000);
    sellSwap->status_ = NEW_ORDSTATUS;
    orderBook_->add(*sellSwap);
    OrderEntry *sellLimit = createAndSaveLimitOrder(SELL_SIDE, 1.0850, 100);
    sellLimit->status_ = NEW_ORDSTATUS;
    orderBook_->add(*sellLimit);

    OrderEntry *buyLimit = createAndSaveLimitOrder(BUY_SIDE, 1.0850, 100);
    buyLimit->status_ = NEW_ORDSTATUS;
    matcher_->match(buyLimit, context_);

    ASSERT_EQ(1u, deferedContainer_->events_.size());
    auto *trade = dynamic_cast<ExecutionDeferedEvent *>(deferedContainer_->events_.front());
    ASSERT_NE(nullptr, trade);
    ASSERT_EQ(1u, trade->trades_.size());
    EXPECT_EQ(sellLimit, trade->trades_.front().order_);
    EXPECT_DOUBLE_EQ(1.0850, trade->trades_.front().lastPx_);
}

TEST_F(FxSwapTest, MarketOrderDoesNotMatchASwap)
{
    // With only a swap on the other side, a market order finds nothing to trade, and is canceled
    OrderEntry *sellSwap = createAndSaveSwapOrder(SELL_SIDE, 1.0850, 1.0872, 100, 1000, 2000);
    sellSwap->status_ = NEW_ORDSTATUS;
    orderBook_->add(*sellSwap);

    OrderEntry *buyMarket = createAndSaveLimitOrder(BUY_SIDE, 0.0, 100);
    buyMarket->ordType_ = MARKET_ORDERTYPE;
    buyMarket->status_ = NEW_ORDSTATUS;
    matcher_->match(buyMarket, context_);

    ASSERT_EQ(1u, deferedContainer_->events_.size());
    EXPECT_NE(nullptr, dynamic_cast<CancelOrderDeferedEvent *>(deferedContainer_->events_.front()))
        << "a market order traded a swap's near leg";
}

} // namespace
