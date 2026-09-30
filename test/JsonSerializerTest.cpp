/**
 Concurrent Order Processor library - WebSocket JSON serializer Tests

 Copyright (C) 2026 dudleylane

 Distributed under the GNU Affero General Public License (AGPL).
*/

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <cstring>
#include <memory>
#include <string>

#include "TestFixtures.h"
#include "TestAux.h"
#include "DataModelDef.h"
#include "JsonSerializer.h"
#include "OrderStorage.h"
#include "WideDataStorage.h"

using namespace COP;
using namespace COP::Store;

namespace
{

class JsonSerializerTest : public test::OrderStorageFixture
{
protected:
    void SetUp() override
    {
        test::OrderStorageFixture::SetUp();
        instrument_ = test::addInstrument("AAPL");
        source_ = WideDataStorage::instance()->add(new StringT("WebSocket"));
        destination_ = WideDataStorage::instance()->add(new StringT("Internal"));
    }

    static SourceIdT addRawData(const char *text)
    {
        return WideDataStorage::instance()->add(
            new RawDataEntry(STRING_RAWDATATYPE, text, static_cast<u32>(std::strlen(text))));
    }

    /// An order as WsSession builds one when the client gives no account: clOrderId, the instrument, the source
    /// and the destination are set, and origClOrderId, the account, the clearing and the executions are not.
    std::unique_ptr<OrderEntry> newOrder(const char *clOrderId, const IdT &orderId)
    {
        std::unique_ptr<OrderEntry> order(new OrderEntry(source_, destination_, addRawData(clOrderId), SourceIdT(),
                                                         instrument_, SourceIdT(), SourceIdT(), SourceIdT()));
        order->orderId_ = orderId;
        order->side_ = BUY_SIDE;
        order->ordType_ = LIMIT_ORDERTYPE;
        order->status_ = RECEIVEDNEW_ORDSTATUS;
        return order;
    }

    SourceIdT instrument_;
    SourceIdT source_;
    SourceIdT destination_;
};

TEST_F(JsonSerializerTest, OrderWithUnsetReferencesSerializes)
{
    // The bug this covers: orderToJson resolved every reference, and a new order's origClOrderId is unset, so
    // serializing any new order threw "rawData not found" (#35). An account the client left out is unset too.
    std::unique_ptr<OrderEntry> order = newOrder("CL-1", IdT(1, 1));

    std::string text;
    ASSERT_NO_THROW(text = App::serializeOrderUpdate(*order));
    nlohmann::json data = nlohmann::json::parse(text)["data"];
    EXPECT_EQ("CL-1", data.value("clOrderId", "?"));
    EXPECT_EQ("", data.value("origClOrderId", "?"));
    EXPECT_EQ("", data.value("account", "?"));
    EXPECT_EQ("AAPL", data.value("symbol", "?"));
    EXPECT_EQ("WebSocket", data.value("source", "?"));
    EXPECT_EQ("Internal", data.value("destination", "?"));
}

TEST_F(JsonSerializerTest, SetReferencesAreSerialized)
{
    // A replacement has an origClOrderId, and an order may have an account: both still come through.
    auto *acct = new AccountEntry();
    acct->account_ = "TRADING-1";
    acct->firm_ = "Apex Capital";
    acct->type_ = PRINCIPAL_ACCOUNTTYPE;
    SourceIdT account = WideDataStorage::instance()->add(acct);
    OrderEntry replacement(source_, destination_, addRawData("CL-2"), addRawData("CL-1"), instrument_, account,
                           SourceIdT(), SourceIdT());

    nlohmann::json data = nlohmann::json::parse(App::serializeOrderUpdate(replacement))["data"];
    EXPECT_EQ("CL-2", data.value("clOrderId", "?"));
    EXPECT_EQ("CL-1", data.value("origClOrderId", "?"));
    EXPECT_EQ("TRADING-1", data.value("account", "?"));
}

TEST_F(JsonSerializerTest, SnapshotIncludesAnOrderWithUnsetReferences)
{
    OrderStorage::instance()->restore(newOrder("CL-3", IdT(3, 1)).release());

    std::string text;
    ASSERT_NO_THROW(text = App::serializeOrderSnapshot(OrderStorage::instance()));
    nlohmann::json msg = nlohmann::json::parse(text);
    ASSERT_EQ(1u, msg["data"].size());
    EXPECT_EQ("CL-3", msg["data"][0].value("clOrderId", "?"));
}

} // namespace
