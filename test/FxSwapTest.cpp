/**
 Concurrent Order Processor library - FX Swap Test Suite

 Tests for FX Swap order matching, execution, validation, and codec round-trip.
*/

#include <gtest/gtest.h>

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

namespace {

class TestDeferedEventContainer : public DeferedEventContainer {
public:
    void addDeferedEvent(DeferedEventBase* evnt) override {
        events_.push_back(evnt);
    }

    size_t deferedEventCount() const override { return events_.size(); }

    void removeDeferedEventsFrom(size_t startIndex) override {
        if (startIndex >= events_.size()) return;
        for (size_t i = startIndex; i < events_.size(); ++i)
            delete events_[i];
        events_.erase(events_.begin() + static_cast<std::ptrdiff_t>(startIndex), events_.end());
    }

    void clear() {
        for (auto* e : events_) delete e;
        events_.clear();
    }

    std::vector<DeferedEventBase*> events_;
};

class FxSwapTest : public ProcessorFixture {
protected:
    void SetUp() override {
        ProcessorFixture::SetUp();

        deferedContainer_ = std::make_unique<TestDeferedEventContainer>();
        matcher_ = std::make_unique<OrderMatcher>();
        matcher_->init(deferedContainer_.get());

        inQueues_ = std::make_unique<IncomingQueues>();
        outQueues_ = std::make_unique<OutgoingQueues>();

        context_ = Context(
            OrderStorage::instance(),
            orderBook_.get(),
            inQueues_.get(),
            outQueues_.get(),
            matcher_.get(),
            IdTGenerator::instance());
    }

    void TearDown() override {
        deferedContainer_->clear();
        deferedContainer_.reset();
        matcher_.reset();
        outQueues_.reset();
        inQueues_.reset();

        ProcessorFixture::TearDown();
    }

    OrderEntry* createAndSaveSwapOrder(Side side, PriceT nearPrice, PriceT farPrice,
                                        QuantityT qty, DateTimeT settlDate, DateTimeT farSettlDate) {
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

    OrderEntry* createAndSaveLimitOrder(Side side, PriceT price, QuantityT qty) {
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

TEST_F(FxSwapTest, SwapValidation_FarSettlDateMustBeAfterNear) {
    auto order = createCorrectOrder(instrumentId1_);
    assignClOrderId(order.get());
    order->ordType_ = FXSWAP_ORDERTYPE;
    order->price_ = 1.0850;
    order->farPrice_ = 1.0872;
    order->settlDate_ = 1000;
    order->farSettlDate_ = 500;  // BEFORE near — invalid
    order->status_ = RECEIVEDNEW_ORDSTATUS;

    std::string reason;
    EXPECT_FALSE(order->isValid(&reason));
    EXPECT_NE(std::string::npos, reason.find("far settlement date"));
}

TEST_F(FxSwapTest, SwapValidation_FarPriceMustBePositive) {
    auto order = createCorrectOrder(instrumentId1_);
    assignClOrderId(order.get());
    order->ordType_ = FXSWAP_ORDERTYPE;
    order->price_ = 1.0850;
    order->farPrice_ = 0.0;  // zero — invalid
    order->settlDate_ = 1000;
    order->farSettlDate_ = 2000;
    order->status_ = RECEIVEDNEW_ORDSTATUS;

    std::string reason;
    EXPECT_FALSE(order->isValid(&reason));
    EXPECT_NE(std::string::npos, reason.find("far leg price"));
}

TEST_F(FxSwapTest, SwapValidation_ValidSwapOrder) {
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

TEST_F(FxSwapTest, SwapNoMatch_RestsOnBook) {
    OrderEntry* buySwap = createAndSaveSwapOrder(BUY_SIDE, 1.0850, 1.0872, 100, 1000, 2000);
    buySwap->status_ = NEW_ORDSTATUS;
    orderBook_->add(*buySwap);

    // Match with no contra on book
    matcher_->match(buySwap, context_);

    // No deferred events — order rests
    EXPECT_EQ(0u, deferedContainer_->events_.size());
}

TEST_F(FxSwapTest, SwapDoesNotMatchLimitOrder) {
    // Add a regular LIMIT sell order
    OrderEntry* limitSell = createAndSaveLimitOrder(SELL_SIDE, 1.0850, 100);
    limitSell->status_ = NEW_ORDSTATUS;
    orderBook_->add(*limitSell);

    // Try to match a swap buy against it
    OrderEntry* buySwap = createAndSaveSwapOrder(BUY_SIDE, 1.0850, 1.0872, 100, 1000, 2000);
    buySwap->status_ = NEW_ORDSTATUS;

    matcher_->match(buySwap, context_);

    // No match — swap requires contra swap
    EXPECT_EQ(0u, deferedContainer_->events_.size());
}

TEST_F(FxSwapTest, SwapMatchExact) {
    // Contra sell swap on book
    OrderEntry* sellSwap = createAndSaveSwapOrder(SELL_SIDE, 1.0850, 1.0872, 100, 1000, 2000);
    sellSwap->status_ = NEW_ORDSTATUS;
    orderBook_->add(*sellSwap);

    // Aggressor buy swap
    OrderEntry* buySwap = createAndSaveSwapOrder(BUY_SIDE, 1.0850, 1.0872, 100, 1000, 2000);
    buySwap->status_ = NEW_ORDSTATUS;

    matcher_->match(buySwap, context_);

    // Should produce exactly 1 SwapExecutionDeferedEvent
    EXPECT_EQ(1u, deferedContainer_->events_.size());
}

TEST_F(FxSwapTest, SwapMatchPartial_ChainsMatchEvent) {
    // Contra sell swap on book with qty 50
    OrderEntry* sellSwap = createAndSaveSwapOrder(SELL_SIDE, 1.0850, 1.0872, 50, 1000, 2000);
    sellSwap->status_ = NEW_ORDSTATUS;
    orderBook_->add(*sellSwap);

    // Aggressor buy swap with qty 100
    OrderEntry* buySwap = createAndSaveSwapOrder(BUY_SIDE, 1.0850, 1.0872, 100, 1000, 2000);
    buySwap->status_ = NEW_ORDSTATUS;

    matcher_->match(buySwap, context_);

    // Should produce SwapExecutionDeferedEvent + MatchSwapOrderDeferedEvent
    EXPECT_EQ(2u, deferedContainer_->events_.size());
}

TEST_F(FxSwapTest, SwapSettlDateMismatch_NoMatch) {
    // Contra with different far settlement date
    OrderEntry* sellSwap = createAndSaveSwapOrder(SELL_SIDE, 1.0850, 1.0872, 100, 1000, 3000);
    sellSwap->status_ = NEW_ORDSTATUS;
    orderBook_->add(*sellSwap);

    // Aggressor with different far settlement date
    OrderEntry* buySwap = createAndSaveSwapOrder(BUY_SIDE, 1.0850, 1.0872, 100, 1000, 2000);
    buySwap->status_ = NEW_ORDSTATUS;

    matcher_->match(buySwap, context_);

    EXPECT_EQ(0u, deferedContainer_->events_.size());
}

TEST_F(FxSwapTest, SwapFarPriceIncompatible_NoMatch) {
    // Contra sell swap wants 1.09 on far leg
    OrderEntry* sellSwap = createAndSaveSwapOrder(SELL_SIDE, 1.0850, 1.0900, 100, 1000, 2000);
    sellSwap->status_ = NEW_ORDSTATUS;
    orderBook_->add(*sellSwap);

    // Aggressor buy: near=BUY far=SELL. Far price 1.0872 < contra 1.0900 → compatible
    // Wait — BUY near means SELL far. Aggressor sells far at 1.0872.
    // Contra buys far at 1.0900. Aggressor ask (1.0872) < contra bid (1.0900) → MATCHES.
    // Let me make incompatible: aggressor far price 1.0950 > contra 1.0900
    OrderEntry* buySwap = createAndSaveSwapOrder(BUY_SIDE, 1.0850, 1.0950, 100, 1000, 2000);
    buySwap->status_ = NEW_ORDSTATUS;

    matcher_->match(buySwap, context_);

    // aggressor far (sell) at 1.0950 > contra far (buy) at 1.0900 → incompatible
    EXPECT_EQ(0u, deferedContainer_->events_.size());
}

// =============================================================================
// Codec Round-Trip Tests
// =============================================================================

TEST_F(FxSwapTest, SwapCodecRoundTrip) {
    auto order = createCorrectOrder(instrumentId1_);
    assignClOrderId(order.get());
    OrderEntry* saved = OrderStorage::instance()->save(*order, IdTGenerator::instance());

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

    // Decode
    OrderEntry* decoded = codec.decode(id, version, buf.data(), buf.size());
    ASSERT_NE(nullptr, decoded);

    EXPECT_EQ(FXSWAP_ORDERTYPE, decoded->ordType_);
    EXPECT_DOUBLE_EQ(1.0850, decoded->price_);
    EXPECT_DOUBLE_EQ(1.0872, decoded->farPrice_);
    EXPECT_EQ(1714003200000ULL, decoded->settlDate_);
    EXPECT_EQ(1716595200000ULL, decoded->farSettlDate_);

    delete decoded;
}

TEST_F(FxSwapTest, LegacyCodecCompat_V0DefaultsSwapFields) {
    auto order = createCorrectOrder(instrumentId1_);
    assignClOrderId(order.get());
    OrderEntry* saved = OrderStorage::instance()->save(*order, IdTGenerator::instance());
    saved->ordType_ = LIMIT_ORDERTYPE;  // non-swap

    // Encode as v0
    std::string buf;
    IdT id;
    u32 version;
    COP::Codec::OrderCodec codec;
    codec.encode(*saved, &buf, &id, &version);

    EXPECT_EQ(0u, version);

    // Decode — swap fields should be defaults
    OrderEntry* decoded = codec.decode(id, version, buf.data(), buf.size());
    ASSERT_NE(nullptr, decoded);

    EXPECT_DOUBLE_EQ(0.0, decoded->farPrice_);
    EXPECT_EQ(0u, decoded->farSettlDate_);

    delete decoded;
}

// =============================================================================
// Regression Tests
// =============================================================================

TEST_F(FxSwapTest, NonSwapOrderUnchanged_LimitMatch) {
    // Standard LIMIT order matching should still work
    OrderEntry* sellLimit = createAndSaveLimitOrder(SELL_SIDE, 10.0, 100);
    sellLimit->status_ = NEW_ORDSTATUS;
    orderBook_->add(*sellLimit);

    OrderEntry* buyLimit = createAndSaveLimitOrder(BUY_SIDE, 10.0, 100);
    buyLimit->status_ = NEW_ORDSTATUS;

    matcher_->match(buyLimit, context_);

    // Should produce standard ExecutionDeferedEvent
    EXPECT_EQ(1u, deferedContainer_->events_.size());
}

} // namespace
