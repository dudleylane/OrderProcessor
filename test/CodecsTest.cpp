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

    InstrumentEntry decVal;
    InstrumentCodec::decode(id, version, buf.c_str(), buf.size(), &decVal);

    EXPECT_EQ(decVal.id_, val.id_);
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
    std::string buf;
    IdT id;
    u32 version = 0;
    InstrumentCodec::encode(val, &buf, &id, &version);

    EXPECT_FALSE(buf.empty());
    EXPECT_EQ(id, val.id_);

    InstrumentEntry decVal;
    InstrumentCodec::decode(id, version, buf.c_str(), buf.size(), &decVal);

    EXPECT_EQ(decVal.id_, val.id_);
    EXPECT_EQ(decVal.securityId_, val.securityId_);
    EXPECT_EQ(decVal.securityIdSource_, val.securityIdSource_);
    EXPECT_EQ(decVal.symbol_, val.symbol_);
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
    val.ordType_ = LIMIT_ORDERTYPE;
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

    // The mask is the record's last 4 bytes
    const u32 unknown = 1u << (static_cast<u32>(_INSTRUCTION) + 1);
    std::memcpy(&buf[buf.size() - sizeof(unknown)], &unknown, sizeof(unknown));
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
