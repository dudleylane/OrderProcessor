/**
 Concurrent Order Processor library - A WebSocket session's pending request ids Tests

 Copyright (C) 2026 dudleylane

 Distributed under the GNU Affero General Public License (AGPL).
*/

#include <gtest/gtest.h>
#include <optional>
#include <string>

#include "PendingRequests.h"

using namespace COP::App;

// What a session keeps of its new orders' requests until each order's first update names its request (#790)

TEST(PendingRequestsTest, AnOrdersFirstUpdateTakesItsRequestsIdOnce)
{
    PendingRequests pending(4);
    pending.remember("WS-1", "r-1");
    pending.remember("WS-2", "r-2");
    EXPECT_EQ(2u, pending.size());

    EXPECT_EQ(std::optional<std::string>("r-2"), pending.take("WS-2"));
    EXPECT_EQ(std::nullopt, pending.take("WS-2")) << "an order's later update named its request again";
    EXPECT_EQ(std::nullopt, pending.take("WS-3")) << "an order the session kept nothing for named a request";
    EXPECT_EQ(std::optional<std::string>("r-1"), pending.take("WS-1"));
    EXPECT_TRUE(pending.empty());
}

TEST(PendingRequestsTest, AnUnstoredOrdersRequestIsForgotten)
{
    PendingRequests pending(4);
    pending.remember("WS-1", "r-1");
    pending.remember("WS-2", "r-2");
    pending.forget("WS-1");
    pending.forget("WS-9"); // kept nothing: nothing changes
    EXPECT_EQ(1u, pending.size());
    EXPECT_EQ(std::nullopt, pending.take("WS-1"));
    EXPECT_EQ(std::optional<std::string>("r-2"), pending.take("WS-2"));
}

TEST(PendingRequestsTest, PastItsCapacityTheOldestIsDroppedFirstAndCounted)
{
    PendingRequests pending(3);
    for (int i = 1; i <= 5; ++i)
    {
        pending.remember("WS-" + std::to_string(i), "r-" + std::to_string(i));
    }
    EXPECT_EQ(3u, pending.size());
    EXPECT_EQ(2u, pending.dropped());
    EXPECT_EQ(std::nullopt, pending.take("WS-1"));
    EXPECT_EQ(std::nullopt, pending.take("WS-2"));
    EXPECT_EQ(std::optional<std::string>("r-3"), pending.take("WS-3"));
    EXPECT_EQ(std::optional<std::string>("r-5"), pending.take("WS-5"));

    // A taken one frees its place: the next is kept with nothing dropped
    pending.remember("WS-6", "r-6");
    pending.remember("WS-7", "r-7");
    EXPECT_EQ(3u, pending.size());
    EXPECT_EQ(2u, pending.dropped());
    EXPECT_EQ(std::optional<std::string>("r-4"), pending.take("WS-4"));
}

TEST(PendingRequestsTest, AClOrdIdKeptAlreadyKeepsItsFirstRequest)
{
    // The session makes each ClOrdID, so none comes twice; were one to, the first request would keep its place
    PendingRequests pending(4);
    pending.remember("WS-1", "r-1");
    pending.remember("WS-1", "r-other");
    EXPECT_EQ(1u, pending.size());
    EXPECT_EQ(std::optional<std::string>("r-1"), pending.take("WS-1"));
}
