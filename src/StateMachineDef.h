/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009-2026 Sergey Mikhailik

 Distributed under the GNU Affero General Public License (AGPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#pragma once

namespace COP
{
struct OrderEntry;

namespace OrdState
{

/// The order state machine's states. A persisted state id is an index below this (#150); StateMachine.cpp checks it,
/// and every state's id, against the machine's transition table at compile time.
constexpr int ORDER_STATE_COUNT = 13;

/// Whether a state id is one of the first region's states, the order's lifecycle: Rcvd_New to Suspended (0 to 7) and
/// Rejected (12). An order's record stores one state per region, and restore refuses a pair whose first isn't one of
/// these (#600); MSM never produces such a pair, and the next event would do whatever its rows did. StateMachine.cpp
/// pins each state's id, and these sets with them.
constexpr bool isZone1StateId(int id)
{
    return ((0 <= id) && (id <= 7)) || (12 == id);
}

/// Whether a state id is one of the second region's states, its cancel and replace: NoCnlReplace to CnclReplaced (8 to
/// 11). See isZone1StateId().
constexpr bool isZone2StateId(int id)
{
    return (8 <= id) && (id <= 11);
}

struct OrderStatePersistence
{
    OrderEntry *orderData_;
    int stateZone1Id_;
    int stateZone2Id_;

    OrderStatePersistence() : orderData_(nullptr), stateZone1Id_(-1), stateZone2Id_(-1) {}

    void serialize(std::string &msg) const;
    char *serialize(char *msg) const;
    const char *restore(const char *buf, size_t size);
    bool compare(const OrderStatePersistence &val) const;
};

} // namespace OrdState
} // namespace COP