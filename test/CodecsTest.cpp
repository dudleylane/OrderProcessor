/**
 Concurrent Order Processor library - Google Test Migration

 Authors: dudleylane, Claude
 Test Migration: 2026

 Copyright (C) 2026 dudleylane

 Distributed under the GNU Affero General Public License (AGPL).
*/

#include <gtest/gtest.h>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "TestAux.h"
#include "InstrumentCodec.h"
#include "StringTCodec.h"
#include "AccountCodec.h"
#include "ClearingCodec.h"
#include "RawDataCodec.h"
#include "OrderCodec.h"
#include "WideDataStorage.h"

using namespace COP;
using namespace COP::Codec;
using namespace COP::Store;

namespace
{

SourceIdT addInstrument(const std::string &name)
{
    auto instr = std::make_unique<InstrumentEntry>();
    instr->symbol_ = name;
    instr->securityId_ = "AAA";
    instr->securityIdSource_ = "AAASrc";
    return WideDataStorage::instance()->add(instr.release());
}

class CodecsTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        // Setup code if needed
    }

    void TearDown() override
    {
        // Cleanup code if needed
    }
};

TEST_F(CodecsTest, InstrumentCodecEmpty)
{
    InstrumentEntry val;
    std::string buf;
    IdT id;
    u32 version = 0;
    InstrumentCodec::encode(val, &buf, &id, &version);

    EXPECT_FALSE(buf.empty());
    EXPECT_EQ(id, val.id_);

    // In the layout encode() wrote: its out-parameter is the storage's version, not the layout's (#144)
    InstrumentEntry decVal;
    InstrumentCodec::decode(id, InstrumentCodec::FORMAT_VERSION, buf.c_str(), buf.size(), &decVal);

    EXPECT_EQ(decVal.id_, val.id_);
    EXPECT_EQ(INVALID_INSTRUMENTTYPE, decVal.instrumentType_);
    EXPECT_EQ(INVALID_CURRENCY, decVal.baseCurrency_);
    EXPECT_EQ(INVALID_CURRENCY, decVal.termCurrency_);
    EXPECT_EQ(decVal.securityId_, val.securityId_);
    EXPECT_TRUE(decVal.securityId_.empty());
    EXPECT_EQ(decVal.securityIdSource_, val.securityIdSource_);
    EXPECT_TRUE(decVal.securityIdSource_.empty());
    EXPECT_EQ(decVal.symbol_, val.symbol_);
    EXPECT_TRUE(decVal.symbol_.empty());
}

TEST_F(CodecsTest, InstrumentCodecFilled)
{
    InstrumentEntry val;
    val.id_ = IdT(1234, 6789);
    val.securityId_ = "securityId_";
    val.securityIdSource_ = "securityIdSource_";
    val.symbol_ = "symbol_";
    val.instrumentType_ = FX_SPOT_INSTRUMENTTYPE;
    val.baseCurrency_ = EUR_CURRENCY;
    val.termCurrency_ = USD_CURRENCY;
    std::string buf;
    IdT id;
    u32 version = 0;
    InstrumentCodec::encode(val, &buf, &id, &version);

    EXPECT_FALSE(buf.empty());
    EXPECT_EQ(id, val.id_);

    InstrumentEntry decVal;
    InstrumentCodec::decode(id, InstrumentCodec::FORMAT_VERSION, buf.c_str(), buf.size(), &decVal);

    EXPECT_EQ(decVal.id_, val.id_);
    EXPECT_EQ(decVal.securityId_, val.securityId_);
    EXPECT_EQ(decVal.securityIdSource_, val.securityIdSource_);
    EXPECT_EQ(decVal.symbol_, val.symbol_);
    EXPECT_EQ(FX_SPOT_INSTRUMENTTYPE, decVal.instrumentType_) << "an instrument's type is persisted (#172)";
    EXPECT_EQ(EUR_CURRENCY, decVal.baseCurrency_);
    EXPECT_EQ(USD_CURRENCY, decVal.termCurrency_);
}

TEST_F(CodecsTest, InstrumentCodecRefusesATypeOrCurrencyItDoesNotKnow)
{
    // Format version 1 stores the type and both currencies as their values (#172). One past the last this build knows
    // is a newer build's, or not a record at all, and is refused rather than restored; the last is read.
    InstrumentEntry val;
    val.id_ = IdT(1234, 6789);
    val.symbol_ = "NZD/USD";
    val.securityId_ = "NZDUSD";
    val.securityIdSource_ = "ISO";
    val.instrumentType_ = FX_SWAP_INSTRUMENTTYPE;
    val.baseCurrency_ = NZD_CURRENCY;
    val.termCurrency_ = USD_CURRENCY;
    std::string buf;
    IdT id;
    u32 version = 0;
    InstrumentCodec::encode(val, &buf, &id, &version);

    InstrumentEntry decVal;
    InstrumentCodec::decode(id, InstrumentCodec::FORMAT_VERSION, buf.c_str(), buf.size(), &decVal);
    EXPECT_EQ(FX_SWAP_INSTRUMENTTYPE, decVal.instrumentType_);
    EXPECT_EQ(NZD_CURRENCY, decVal.baseCurrency_);

    // The record ends '.', the type, '.', the base currency, '.', the term currency, each value 4 bytes
    ASSERT_LT(15u, buf.size());
    const size_t term = buf.size() - 4;
    const size_t base = term - 5;
    const size_t type = base - 5;
    const auto refused = [&](size_t at, u32 value)
    {
        std::string bad = buf;
        std::memcpy(bad.data() + at, &value, sizeof(value));
        InstrumentEntry badVal;
        EXPECT_THROW(InstrumentCodec::decode(id, InstrumentCodec::FORMAT_VERSION, bad.c_str(), bad.size(), &badVal),
                     std::runtime_error)
            << "a value of " << value << " at byte " << at;
    };
    refused(type, static_cast<u32>(FX_SWAP_INSTRUMENTTYPE) + 1);
    refused(base, static_cast<u32>(NZD_CURRENCY) + 1);
    refused(term, static_cast<u32>(NZD_CURRENCY) + 1);
}

TEST_F(CodecsTest, StringTCodecEmpty)
{
    StringT val;
    std::string buf;
    StringTCodec::encode(val, &buf);

    EXPECT_FALSE(buf.empty());

    StringT decVal;
    StringTCodec::decode(buf.c_str(), buf.size(), &decVal);

    EXPECT_EQ(decVal, val);
    EXPECT_TRUE(decVal.empty());
}

TEST_F(CodecsTest, StringTCodecFilled)
{
    StringT val = "value_";
    std::string buf;
    StringTCodec::encode(val, &buf);

    EXPECT_FALSE(buf.empty());

    StringT decVal;
    StringTCodec::decode(buf.c_str(), buf.size(), &decVal);

    EXPECT_EQ(decVal, val);
}

TEST_F(CodecsTest, AccountCodecEmpty)
{
    AccountEntry val;
    std::string buf;
    IdT id;
    u32 version = 0;
    val.type_ = INVALID_ACCOUNTTYPE;
    AccountCodec::encode(val, &buf, &id, &version);

    EXPECT_FALSE(buf.empty());
    EXPECT_EQ(id, val.id_);

    AccountEntry decVal;
    AccountCodec::decode(id, version, buf.c_str(), buf.size(), &decVal);

    EXPECT_EQ(decVal.id_, val.id_);
    EXPECT_EQ(decVal.account_, val.account_);
    EXPECT_TRUE(decVal.account_.empty());
    EXPECT_EQ(decVal.firm_, val.firm_);
    EXPECT_TRUE(decVal.firm_.empty());
    EXPECT_EQ(decVal.type_, val.type_);
    EXPECT_EQ(INVALID_ACCOUNTTYPE, decVal.type_);
}

TEST_F(CodecsTest, AccountCodecFilled)
{
    AccountEntry val;
    val.id_ = IdT(1234, 6789);
    val.account_ = "account_";
    val.firm_ = "firm_";
    val.type_ = AGENCY_ACCOUNTTYPE;
    std::string buf;
    IdT id;
    u32 version = 0;
    AccountCodec::encode(val, &buf, &id, &version);

    EXPECT_FALSE(buf.empty());
    EXPECT_EQ(id, val.id_);

    AccountEntry decVal;
    AccountCodec::decode(id, version, buf.c_str(), buf.size(), &decVal);

    EXPECT_EQ(decVal.id_, val.id_);
    EXPECT_EQ(decVal.account_, val.account_);
    EXPECT_EQ(decVal.firm_, val.firm_);
    EXPECT_EQ(decVal.type_, val.type_);
}

TEST_F(CodecsTest, ClearingCodecEmpty)
{
    ClearingEntry val;
    std::string buf;
    IdT id;
    u32 version = 0;
    ClearingCodec::encode(val, &buf, &id, &version);

    EXPECT_FALSE(buf.empty());
    EXPECT_EQ(id, val.id_);

    ClearingEntry decVal;
    ClearingCodec::decode(id, version, buf.c_str(), buf.size(), &decVal);

    EXPECT_EQ(decVal.id_, val.id_);
    EXPECT_EQ(decVal.firm_, val.firm_);
    EXPECT_TRUE(decVal.firm_.empty());
}

TEST_F(CodecsTest, ClearingCodecFilled)
{
    ClearingEntry val;
    val.id_ = IdT(1234, 6789);
    val.firm_ = "firm_";
    std::string buf;
    IdT id;
    u32 version = 0;
    ClearingCodec::encode(val, &buf, &id, &version);

    EXPECT_FALSE(buf.empty());
    EXPECT_EQ(id, val.id_);

    ClearingEntry decVal;
    ClearingCodec::decode(id, version, buf.c_str(), buf.size(), &decVal);

    EXPECT_EQ(decVal.id_, val.id_);
    EXPECT_EQ(decVal.firm_, val.firm_);
}

TEST_F(CodecsTest, RawDataCodecEmpty)
{
    RawDataEntry val;
    std::string buf;
    IdT id;
    u32 version = 0;
    val.type_ = INVALID_RAWDATATYPE;
    val.data_ = nullptr;
    val.length_ = 0;
    RawDataCodec::encode(val, &buf, &id, &version);

    EXPECT_FALSE(buf.empty());
    EXPECT_EQ(id, val.id_);

    RawDataEntry decVal;
    RawDataCodec::decode(id, version, buf.c_str(), buf.size(), &decVal);

    EXPECT_EQ(decVal.id_, val.id_);
    EXPECT_EQ(decVal.type_, val.type_);
    EXPECT_EQ(decVal.data_, val.data_);
    EXPECT_EQ(decVal.length_, val.length_);
}

TEST_F(CodecsTest, RawDataCodecFilled)
{
    RawDataEntry val(STRING_RAWDATATYPE, "data_", 5);
    val.id_ = IdT(1234, 6789);
    std::string buf;
    IdT id;
    u32 version = 0;
    RawDataCodec::encode(val, &buf, &id, &version);

    EXPECT_FALSE(buf.empty());
    EXPECT_EQ(id, val.id_);

    RawDataEntry decVal;
    RawDataCodec::decode(id, version, buf.c_str(), buf.size(), &decVal);

    EXPECT_EQ(decVal.id_, val.id_);
    EXPECT_EQ(decVal.type_, val.type_);
    EXPECT_EQ(decVal.length_, val.length_);
    EXPECT_EQ(0, memcmp(decVal.data_, val.data_, val.length_));
}

class OrderCodecTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        WideDataStorage::create();
    }

    void TearDown() override
    {
        WideDataStorage::destroy();
    }
};

TEST_F(OrderCodecTest, OrderCodecEmpty)
{
    SourceIdT instrId = addInstrument("aaa");
    SourceIdT v;
    OrderEntry val(v, v, v, v, instrId, v, v, v);
    std::string buf;
    IdT id;
    u32 version = 0;
    val.creationTime_ = 0;
    val.lastUpdateTime_ = 0;
    val.expireTime_ = 0;
    val.settlDate_ = 0;
    val.price_ = 0;
    val.stopPx_ = 0;
    val.avgPx_ = 0;
    val.dayAvgPx_ = 0;
    val.status_ = INVALID_ORDSTATUS;
    val.side_ = INVALID_SIDE;
    val.ordType_ = INVALID_ORDERTYPE;
    val.tif_ = INVALID_TIF;
    val.settlType_ = INVALID_SETTLTYPE;
    val.capacity_ = INVALID_CAPACITY;
    val.currency_ = INVALID_CURRENCY;
    val.minQty_ = 0;
    val.orderQty_ = 0;
    val.leavesQty_ = 0;
    val.cumQty_ = 0;
    val.dayOrderQty_ = 0;
    val.dayCumQty_ = 0;
    val.stateMachinePersistance_.orderData_ = nullptr;
    val.stateMachinePersistance_.stateZone1Id_ = 0;
    val.stateMachinePersistance_.stateZone2Id_ = 0;
    OrderCodec::encode(val, &buf, &id, &version);

    EXPECT_FALSE(buf.empty());
    EXPECT_EQ(id, val.orderId_);

    auto decVal =
        std::unique_ptr<OrderEntry>(OrderCodec::decode(id, OrderCodec::FORMAT_VERSION, buf.c_str(), buf.size()));

    ASSERT_NE(nullptr, decVal.get());
    EXPECT_EQ(decVal->instrument_.getId(), val.instrument_.getId());
    EXPECT_EQ(decVal->status_, val.status_);
    EXPECT_EQ(decVal->orderId_, val.orderId_);
}

TEST_F(OrderCodecTest, OrderCodecFilled)
{
    SourceIdT instrId = addInstrument("instrument");
    OrderEntry val(SourceIdT(1, 1), SourceIdT(2, 2), SourceIdT(3, 3), SourceIdT(4, 4), instrId, SourceIdT(6, 6),
                   SourceIdT(7, 7), SourceIdT(8, 8));
    std::string buf;
    IdT id;
    u32 version = 0;

    val.creationTime_ = 1111;
    val.lastUpdateTime_ = 1112;
    val.price_ = 22.22;
    val.status_ = REJECTED_ORDSTATUS;
    val.side_ = BUY_SIDE;
    val.ordType_ = LIMIT_ORDERTYPE;
    val.orderId_ = IdT(4444, 4445);
    // Rejected, and no cancel or replace: a persisted order's states are always states of the machine (#150)
    val.stateMachinePersistance_.stateZone1Id_ = 12;
    val.stateMachinePersistance_.stateZone2Id_ = 8;
    // Lost on every restart until #172
    val.execInstruct_ = { STAY_ON_OFFER_SIDE_INSTRUCTION, NOT_HELD_INSTRUCTION, _INSTRUCTION };

    OrderCodec::encode(val, &buf, &id, &version);

    EXPECT_FALSE(buf.empty());
    EXPECT_EQ(id, val.orderId_);

    auto decVal =
        std::unique_ptr<OrderEntry>(OrderCodec::decode(id, OrderCodec::FORMAT_VERSION, buf.c_str(), buf.size()));

    ASSERT_NE(nullptr, decVal.get());
    EXPECT_EQ(decVal->instrument_.getId(), val.instrument_.getId());
    EXPECT_EQ(decVal->creationTime_, val.creationTime_);
    EXPECT_EQ(decVal->price_, val.price_);
    EXPECT_EQ(decVal->status_, val.status_);
    EXPECT_EQ(decVal->side_, val.side_);
    EXPECT_EQ(decVal->ordType_, val.ordType_);
    EXPECT_EQ(decVal->orderId_, val.orderId_);
    EXPECT_EQ(decVal->execInstruct_, val.execInstruct_);
}

// Nothing in the server writes through the encoder that fills a buffer it's given, so this is what keeps it writing the
// record the other one writes (#172)
TEST_F(OrderCodecTest, BothEncodersWriteTheSameRecord)
{
    SourceIdT instrId = addInstrument("instrument");
    OrderEntry val(SourceIdT(1, 1), SourceIdT(2, 2), SourceIdT(3, 3), SourceIdT(4, 4), instrId, SourceIdT(6, 6),
                   SourceIdT(7, 7), SourceIdT(8, 8));
    val.orderId_ = IdT(4444, 4445);
    val.price_ = 22.22;
    val.status_ = NEW_ORDSTATUS;
    val.side_ = SELL_SIDE;
    val.ordType_ = STOPLIMIT_ORDERTYPE;
    val.stopPx_ = 22.0;
    val.stopTriggered_ = true; // set, so that an encoder that drops it writes a different record (#223)
    val.orderQty_ = 100;
    val.leavesQty_ = 100;
    val.stateMachinePersistance_.stateZone1Id_ = 2;
    val.stateMachinePersistance_.stateZone2Id_ = 8;
    val.execInstruct_ = { NOT_HELD_INSTRUCTION, GO_ALONG_INSTRUCTION };

    std::string written;
    IdT id;
    u32 version = 0;
    OrderCodec::encode(val, &written, &id, &version);

    std::vector<char> buf(4096);
    const char *end = OrderCodec::encode(val, buf.data(), &id, &version);
    EXPECT_EQ(written, std::string(buf.data(), static_cast<size_t>(end - buf.data())));

    // On this branch a swap's far leg follows its execution instructions, in both encoders
    val.ordType_ = FXSWAP_ORDERTYPE;
    val.farPrice_ = 22.5;
    val.settlDate_ = 1000;
    val.farSettlDate_ = 2000;
    written.clear();
    OrderCodec::encode(val, &written, &id, &version);
    end = OrderCodec::encode(val, buf.data(), &id, &version);
    EXPECT_EQ(written, std::string(buf.data(), static_cast<size_t>(end - buf.data())));
}

// A record is written with what the decision it persists left the order with (#411): a fill's transaction can run after
// a later fill of the order was applied, and the fill's record must not count it. Both encoders write the figures given.
TEST_F(OrderCodecTest, AnOrderIsWrittenWithTheFiguresGiven)
{
    SourceIdT instrId = addInstrument("instrument");
    OrderEntry val(SourceIdT(1, 1), SourceIdT(2, 2), SourceIdT(3, 3), SourceIdT(4, 4), instrId, SourceIdT(6, 6),
                   SourceIdT(7, 7), SourceIdT(8, 8));
    val.orderId_ = IdT(4444, 4445);
    val.price_ = 22.22;
    val.side_ = SELL_SIDE;
    val.ordType_ = STOPLIMIT_ORDERTYPE;
    val.stopPx_ = 22.0;
    val.orderQty_ = 100;
    // The first fill: 30 of 100, once the stop was reached
    val.status_ = PARTFILL_ORDSTATUS;
    val.leavesQty_ = 70;
    val.cumQty_ = 30;
    val.avgPx_ = 22.25;
    val.dayOrderQty_ = 100;
    val.dayCumQty_ = 30;
    val.dayAvgPx_ = 22.25;
    val.dayStart_ = 1792098000000ULL;
    val.stopTriggered_ = true;
    val.stateMachinePersistance_.stateZone1Id_ = 3; // PartFill
    val.stateMachinePersistance_.stateZone2Id_ = 8; // NoCnlReplace
    const OrderFigures firstFill(val);
    // The second, which takes the rest, applied to the order before the first fill's transaction runs
    val.status_ = FILLED_ORDSTATUS;
    val.leavesQty_ = 0;
    val.cumQty_ = 100;
    val.avgPx_ = 22.4;
    val.dayCumQty_ = 100;
    val.dayAvgPx_ = 22.4;
    val.stateMachinePersistance_.stateZone1Id_ = 4; // Filled

    std::string written;
    IdT id;
    u32 version = 0;
    OrderCodec::encode(val, firstFill, &written, &id, &version);
    EXPECT_EQ(id, val.orderId_);
    std::vector<char> buf(4096);
    const char *end = OrderCodec::encode(val, firstFill, buf.data(), &id, &version);
    EXPECT_EQ(written, std::string(buf.data(), static_cast<size_t>(end - buf.data())));

    auto decVal =
        std::unique_ptr<OrderEntry>(OrderCodec::decode(id, OrderCodec::FORMAT_VERSION, written.data(), written.size()));
    ASSERT_NE(nullptr, decVal.get());
    EXPECT_EQ(PARTFILL_ORDSTATUS, decVal->status_);
    EXPECT_EQ(70u, decVal->leavesQty_);
    EXPECT_EQ(30u, decVal->cumQty_);
    EXPECT_DOUBLE_EQ(22.25, decVal->avgPx_);
    EXPECT_EQ(100u, decVal->dayOrderQty_);
    EXPECT_EQ(30u, decVal->dayCumQty_);
    EXPECT_DOUBLE_EQ(22.25, decVal->dayAvgPx_);
    EXPECT_EQ(1792098000000ULL, decVal->dayStart_);
    EXPECT_TRUE(decVal->stopTriggered_);
    EXPECT_EQ(3, decVal->stateMachinePersistance_.stateZone1Id_);
    EXPECT_EQ(8, decVal->stateMachinePersistance_.stateZone2Id_);
    // What no decision changes comes from the order
    EXPECT_EQ(100u, decVal->orderQty_);
    EXPECT_DOUBLE_EQ(22.22, decVal->price_);
    EXPECT_DOUBLE_EQ(22.0, decVal->stopPx_);
    EXPECT_EQ(SELL_SIDE, decVal->side_);

    // Without figures, the order as it is: the record its own figures write
    std::string own;
    OrderCodec::encode(val, &own, &id, &version);
    std::string withOwn;
    OrderCodec::encode(val, OrderFigures(val), &withOwn, &id, &version);
    EXPECT_EQ(own, withOwn);
    EXPECT_NE(own, written);
}

// Since format version 3 a record carries the order's strings, each under its field's id (#192). A string given for a
// field that names nothing could never be restored: the encoder refuses it, and so does the decoder, in a record.
TEST_F(OrderCodecTest, AStringForAFieldThatNamesNothingIsRefused)
{
    SourceIdT instrId = addInstrument("instrument");
    // source, destination, clOrderId, origClOrderId (unset), instrument, account, clearing, executions
    OrderEntry val(SourceIdT(1, 0), SourceIdT(2, 0), SourceIdT(3, 1), SourceIdT(), instrId, SourceIdT(6, 1),
                   SourceIdT(7, 1), SourceIdT(8, 1));
    val.orderId_ = IdT(4444, 4445);
    val.stateMachinePersistance_.stateZone1Id_ = 2;
    val.stateMachinePersistance_.stateZone2Id_ = 8;
    OrderStrings strings;
    strings.origClOrderId.emplace(STRING_RAWDATATYPE, "CL-0", 4);
    std::string buf;
    IdT id;
    u32 version = 0;
    EXPECT_THROW(OrderCodec::encode(val, &buf, &id, &version, strings), std::invalid_argument);

    // The record without it, then the same with the original's ClOrdID marked carried: its presence byte is the third
    // from the end of the four ('.', 0) pairs that close the strings of a record carrying none, which version 4's '.'
    // and day start follow (#244)
    buf.clear();
    OrderCodec::encode(val, &buf, &id, &version);
    const size_t stringsEnd = buf.size() - (1 + sizeof(DateTimeT));
    ASSERT_EQ('\0', buf[stringsEnd - 5]);
    std::string carried = buf.substr(0, stringsEnd - 5) + std::string("\x01", 1) +
                          std::string("\x01\0\0\0\x04\0\0\0CL-0", 12) + buf.substr(stringsEnd - 4);
    try
    {
        std::unique_ptr<OrderEntry>(OrderCodec::decode(id, OrderCodec::FORMAT_VERSION, carried.data(), carried.size()));
        ADD_FAILURE() << "a record carrying a string for a field that names nothing was decoded";
    }
    catch (const std::runtime_error &ex)
    {
        EXPECT_NE(std::string::npos, std::string(ex.what()).find("for an unset id")) << ex.what();
    }

    // A string's id has date 0, which IdT::isValid() refuses, and it is carried all the same
    buf.clear();
    OrderStrings named;
    named.source = "desk-a";
    named.destination = "Internal";
    OrderCodec::encode(val, &buf, &id, &version, named);
    OrderStrings decoded;
    std::unique_ptr<OrderEntry> order(
        OrderCodec::decode(id, OrderCodec::FORMAT_VERSION, buf.data(), buf.size(), &decoded));
    ASSERT_TRUE(decoded.source && decoded.destination);
    EXPECT_EQ("desk-a", *decoded.source);
    EXPECT_EQ("Internal", *decoded.destination);
    EXPECT_FALSE(decoded.clOrderId || decoded.origClOrderId);
}

// A mask bit that no execution instruction has is a layout this build doesn't know (#172)
TEST_F(OrderCodecTest, AnUnknownExecutionInstructionIsRefused)
{
    SourceIdT instrId = addInstrument("instrument");
    OrderEntry val(SourceIdT(1, 1), SourceIdT(2, 2), SourceIdT(3, 3), SourceIdT(4, 4), instrId, SourceIdT(6, 6),
                   SourceIdT(7, 7), SourceIdT(8, 8));
    val.orderId_ = IdT(4444, 4445);
    val.stateMachinePersistance_.stateZone1Id_ = 12;
    val.stateMachinePersistance_.stateZone2Id_ = 8;
    std::string buf;
    IdT id;
    u32 version = 0;
    OrderCodec::encode(val, &buf, &id, &version);

    // The mask comes 19 bytes before the record's end: '.' and the stop flag (#223), the four strings this order
    // doesn't carry, each '.' and a 0 (#192), then '.' and the day its figures are for (#244)
    const u32 unknown = 1u << (static_cast<u32>(_INSTRUCTION) + 1);
    std::memcpy(&buf[buf.size() - 19 - sizeof(unknown)], &unknown, sizeof(unknown));
    try
    {
        std::unique_ptr<OrderEntry>(OrderCodec::decode(id, OrderCodec::FORMAT_VERSION, buf.c_str(), buf.size()));
        ADD_FAILURE() << "an order with an unknown execution instruction was decoded";
    }
    catch (const std::runtime_error &ex)
    {
        EXPECT_NE(std::string::npos, std::string(ex.what()).find("unknown execution instruction")) << ex.what();
    }
}

} // namespace
