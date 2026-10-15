/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009-2026 Sergey Mikhailik

 Distributed under the GNU Affero General Public License (AGPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#include <cstring>
#include <stdexcept>
#include "OrderCodec.h"
#include "StringTCodec.h"
#include "CodecChecks.h"

using namespace std;
using namespace COP;
using namespace COP::Codec;

namespace
{
const int BUFFER_SIZE = 256;
// The record holds each of these as its in-memory bytes, so a type that changes size misreads every order already
// written (#149): narrowing an enum's underlying type, or a wider QuantityT, has to come with a new FORMAT_VERSION.
static_assert(sizeof(OrderStatus) == 4, "an order's status is stored as 4 bytes");
static_assert(sizeof(Side) == 4, "an order's side is stored as 4 bytes");
static_assert(sizeof(OrderType) == 4, "an order's type is stored as 4 bytes");
static_assert(sizeof(TimeInForce) == 4, "an order's time in force is stored as 4 bytes");
static_assert(sizeof(SettlTypeBase) == 4, "an order's settlement type is stored as 4 bytes");
static_assert(sizeof(Capacity) == 4, "an order's capacity is stored as 4 bytes");
static_assert(sizeof(Currency) == 4, "an order's currency is stored as 4 bytes");
static_assert(sizeof(QuantityT) == 4, "quantities are stored as 4 bytes");
static_assert(sizeof(PriceT) == 8, "prices are stored as 8 bytes");
static_assert(sizeof(DateTimeT) == 8, "times and dates are stored as 8 bytes");
static_assert(sizeof(OrdState::OrderStatePersistence::stateZone1Id_) == 4, "state ids are stored as 4 bytes");
static_assert(sizeof(OrdState::OrderStatePersistence::stateZone2Id_) == 4, "state ids are stored as 4 bytes");
// The record holds each enum's value, so the values are pinned too (#150): renumbering or reordering an enumerator
// misreads every order already written. A new enumerator goes at the end, with its value added here.
static_assert((INVALID_INSTRUCTION == 0) && (STAY_ON_OFFER_SIDE_INSTRUCTION == 1) && (NOT_HELD_INSTRUCTION == 2) &&
                  (WORK_INSTRUCTION == 3) && (GO_ALONG_INSTRUCTION == 4) && (_INSTRUCTION == 5),
              "Instructions values are persisted, as the bits of a 4-byte mask (#172)");
static_assert(_INSTRUCTION < 32, "each execution instruction needs a bit of the 4-byte mask");
static_assert((INVALID_ORDSTATUS == 0) && (RECEIVEDNEW_ORDSTATUS == 1) && (REJECTED_ORDSTATUS == 2) &&
                  (PENDINGNEW_ORDSTATUS == 3) && (PENDINGREPLACE_ORDSTATUS == 4) && (NEW_ORDSTATUS == 5) &&
                  (PARTFILL_ORDSTATUS == 6) && (FILLED_ORDSTATUS == 7) && (EXPIRED_ORDSTATUS == 8) &&
                  (DFD_ORDSTATUS == 9) && (SUSPENDED_ORDSTATUS == 10) && (REPLACED_ORDSTATUS == 11) &&
                  (CANCELED_ORDSTATUS == 12),
              "OrderStatus values are persisted");
static_assert((INVALID_SIDE == 0) && (BUY_SIDE == 1) && (SELL_SIDE == 2) && (BUY_MINUS_SIDE == 3) &&
                  (SELL_PLUS_SIDE == 4) && (SELL_SHORT_SIDE == 5) && (CROSS_SIDE == 6),
              "Side values are persisted");
static_assert((INVALID_ORDERTYPE == 0) && (MARKET_ORDERTYPE == 1) && (LIMIT_ORDERTYPE == 2) && (STOP_ORDERTYPE == 3) &&
                  (STOPLIMIT_ORDERTYPE == 4) && (FXSWAP_ORDERTYPE == 5),
              "OrderType values are persisted");
static_assert((INVALID_TIF == 0) && (DAY_TIF == 1) && (GTD_TIF == 2) && (GTC_TIF == 3) && (FOK_TIF == 4) &&
                  (IOC_TIF == 5) && (OPG_TIF == 6) && (ATCLOSE_TIF == 7),
              "TimeInForce values are persisted");
static_assert((INVALID_SETTLTYPE == 0) && (_0_SETTLTYPE == 1) && (_1_SETTLTYPE == 2) && (_2_SETTLTYPE == 3) &&
                  (_3_SETTLTYPE == 4) && (_4_SETTLTYPE == 5) && (_5_SETTLTYPE == 6) && (_6_SETTLTYPE == 7) &&
                  (_7_SETTLTYPE == 8) && (_8_SETTLTYPE == 9) && (_9_SETTLTYPE == 10) && (_B_SETTLTYPE == 11) &&
                  (_C_SETTLTYPE == 12) && (TENOR_SETTLTYPE == 13),
              "SettlTypeBase values are persisted");
static_assert((INVALID_CAPACITY == 0) && (AGENCY_CAPACITY == 1) && (PRINCIPAL_CAPACITY == 2) &&
                  (PROPRIETARY_CAPACITY == 3) && (INDIVIDUAL_CAPACITY == 4) && (RISKLESS_PRINCIPAL_CAPACITY == 5) &&
                  (AGENT_FOR_ANOTHER_MEMBER_CAPACITY == 6),
              "Capacity values are persisted");
static_assert((INVALID_CURRENCY == 0) && (USD_CURRENCY == 1) && (EUR_CURRENCY == 2) && (GBP_CURRENCY == 3) &&
                  (JPY_CURRENCY == 4) && (CHF_CURRENCY == 5) && (AUD_CURRENCY == 6) && (CAD_CURRENCY == 7) &&
                  (NZD_CURRENCY == 8),
              "Currency values are persisted");

/// The execution instructions as stored: bit n set for the instruction whose value is n (#172)
u32 instructionMask(const InstructionSetT &instructions)
{
    u32 mask = 0;
    for (const Instructions instruction : instructions)
    {
        mask |= 1u << static_cast<u32>(instruction);
    }
    return mask;
}

/// The execution instructions a stored mask names. A bit no instruction has is refused, not dropped.
InstructionSetT instructionsFromMask(u32 mask)
{
    if (0 != (mask >> (static_cast<u32>(_INSTRUCTION) + 1)))
    {
        throw std::runtime_error("Invalid format of the encoded OrderEntry - unknown execution instruction!");
    }
    InstructionSetT instructions;
    for (u32 value = 0; value <= static_cast<u32>(_INSTRUCTION); ++value)
    {
        if (0 != (mask & (1u << value)))
        {
            instructions.insert(static_cast<Instructions>(value));
        }
    }
    return instructions;
}
} // namespace

OrderCodec::OrderCodec(void) {}

OrderCodec::~OrderCodec(void) {}

void OrderCodec::encode(const OrderEntry &val, std::string *buf, IdT *id, u32 *version)
{
    assert(nullptr != buf);
    assert(nullptr != id);
    assert(nullptr != version);

    *id = val.orderId_;
    // Nothing stores this version: the dispatcher keys records with the storage's own, and decode() finds the far
    // leg by its marker (#144).
    *version = (FXSWAP_ORDERTYPE == val.ordType_) ? 1u : 0u;

    string strbuf;
    buf->reserve(buf->size() + BUFFER_SIZE);
    val.instrument_.getId().serialize(*buf);
    buf->append(1, '.');

    val.account_.getId().serialize(*buf);
    buf->append(1, '.');

    val.clearing_.getId().serialize(*buf);
    buf->append(1, '.');

    val.destination_.getId().serialize(*buf);
    buf->append(1, '.');

    val.clOrderId_.getId().serialize(*buf);
    buf->append(1, '.');

    val.origClOrderId_.getId().serialize(*buf);
    buf->append(1, '.');

    val.source_.getId().serialize(*buf);
    buf->append(1, '.');

    val.executions_.getId().serialize(*buf);
    buf->append(1, '.');

    val.origOrderId_.serialize(*buf);
    buf->append(1, '.');

    char typebuf[36];
    memcpy(typebuf, &(val.creationTime_), sizeof(val.creationTime_));
    buf->append(typebuf, sizeof(val.creationTime_));
    buf->append(1, '.');

    memcpy(typebuf, &(val.lastUpdateTime_), sizeof(val.lastUpdateTime_));
    buf->append(typebuf, sizeof(val.lastUpdateTime_));
    buf->append(1, '.');

    memcpy(typebuf, &(val.expireTime_), sizeof(val.expireTime_));
    buf->append(typebuf, sizeof(val.expireTime_));
    buf->append(1, '.');

    memcpy(typebuf, &(val.settlDate_), sizeof(val.settlDate_));
    buf->append(typebuf, sizeof(val.settlDate_));
    buf->append(1, '.');

    memcpy(typebuf, &(val.price_), sizeof(val.price_));
    buf->append(typebuf, sizeof(val.price_));
    buf->append(1, '.');

    memcpy(typebuf, &(val.stopPx_), sizeof(val.stopPx_));
    buf->append(typebuf, sizeof(val.stopPx_));
    buf->append(1, '.');

    memcpy(typebuf, &(val.avgPx_), sizeof(val.avgPx_));
    buf->append(typebuf, sizeof(val.avgPx_));
    buf->append(1, '.');

    memcpy(typebuf, &(val.dayAvgPx_), sizeof(val.dayAvgPx_));
    buf->append(typebuf, sizeof(val.dayAvgPx_));
    buf->append(1, '.');

    memcpy(typebuf, &(val.status_), sizeof(val.status_));
    buf->append(typebuf, sizeof(val.status_));
    buf->append(1, '.');

    memcpy(typebuf, &(val.side_), sizeof(val.side_));
    buf->append(typebuf, sizeof(val.side_));
    buf->append(1, '.');

    memcpy(typebuf, &(val.ordType_), sizeof(val.ordType_));
    buf->append(typebuf, sizeof(val.ordType_));
    buf->append(1, '.');

    memcpy(typebuf, &(val.tif_), sizeof(val.tif_));
    buf->append(typebuf, sizeof(val.tif_));
    buf->append(1, '.');

    memcpy(typebuf, &(val.settlType_), sizeof(val.settlType_));
    buf->append(typebuf, sizeof(val.settlType_));
    buf->append(1, '.');

    memcpy(typebuf, &(val.capacity_), sizeof(val.capacity_));
    buf->append(typebuf, sizeof(val.capacity_));
    buf->append(1, '.');

    memcpy(typebuf, &(val.currency_), sizeof(val.currency_));
    buf->append(typebuf, sizeof(val.currency_));
    buf->append(1, '.');

    memcpy(typebuf, &(val.minQty_), sizeof(val.minQty_));
    buf->append(typebuf, sizeof(val.minQty_));
    buf->append(1, '.');

    memcpy(typebuf, &(val.orderQty_), sizeof(val.orderQty_));
    buf->append(typebuf, sizeof(val.orderQty_));
    buf->append(1, '.');

    memcpy(typebuf, &(val.leavesQty_), sizeof(val.leavesQty_));
    buf->append(typebuf, sizeof(val.leavesQty_));
    buf->append(1, '.');

    memcpy(typebuf, &(val.cumQty_), sizeof(val.cumQty_));
    buf->append(typebuf, sizeof(val.cumQty_));
    buf->append(1, '.');

    memcpy(typebuf, &(val.dayOrderQty_), sizeof(val.dayOrderQty_));
    buf->append(typebuf, sizeof(val.dayOrderQty_));
    buf->append(1, '.');

    memcpy(typebuf, &(val.dayCumQty_), sizeof(val.dayCumQty_));
    buf->append(typebuf, sizeof(val.dayCumQty_));
    buf->append(1, '.');

    val.stateMachinePersistance_.serialize(*buf);
    buf->append(1, '.');

    // Format version 1 (#172)
    const u32 instructions = instructionMask(val.execInstruct_);
    memcpy(typebuf, &instructions, sizeof(instructions));
    buf->append(typebuf, sizeof(instructions));
    buf->append(1, '.');

    // Format version 2 (#223)
    buf->append(1, val.stopTriggered_ ? '\x01' : '\x00');

    // A swap's far leg (#144), after the execution instructions and the stop's mark (#223)
    if (FXSWAP_ORDERTYPE == val.ordType_)
    {
        buf->append(1, '.');
        memcpy(typebuf, &(val.farPrice_), sizeof(val.farPrice_));
        buf->append(typebuf, sizeof(val.farPrice_));
        buf->append(1, '.');
        memcpy(typebuf, &(val.farSettlDate_), sizeof(val.farSettlDate_));
        buf->append(typebuf, sizeof(val.farSettlDate_));
    }
}

char *OrderCodec::encode(const OrderEntry &val, char *buf, IdT *id, u32 *version)
{
    assert(nullptr != buf);
    assert(nullptr != id);
    assert(nullptr != version);

    *id = val.orderId_;
    // Nothing stores this version: the dispatcher keys records with the storage's own, and decode() finds the far
    // leg by its marker (#144).
    *version = (FXSWAP_ORDERTYPE == val.ordType_) ? 1u : 0u;

    char *p = buf;
    p = val.instrument_.getId().serialize(p);
    *p = '.';
    ++p;

    p = val.account_.getId().serialize(p);
    *p = '.';
    ++p;

    p = val.clearing_.getId().serialize(p);
    *p = '.';
    ++p;

    p = val.destination_.getId().serialize(p);
    *p = '.';
    ++p;

    p = val.clOrderId_.getId().serialize(p);
    *p = '.';
    ++p;

    p = val.origClOrderId_.getId().serialize(p);
    *p = '.';
    ++p;

    p = val.source_.getId().serialize(p);
    *p = '.';
    ++p;

    p = val.executions_.getId().serialize(p);
    *p = '.';
    ++p;

    p = val.origOrderId_.serialize(p);
    *p = '.';
    ++p;

    memcpy(p, &(val.creationTime_), sizeof(val.creationTime_));
    p += sizeof(val.creationTime_);
    *p = '.';
    ++p;

    memcpy(p, &(val.lastUpdateTime_), sizeof(val.lastUpdateTime_));
    p += sizeof(val.lastUpdateTime_);
    *p = '.';
    ++p;

    memcpy(p, &(val.expireTime_), sizeof(val.expireTime_));
    p += sizeof(val.expireTime_);
    *p = '.';
    ++p;

    memcpy(p, &(val.settlDate_), sizeof(val.settlDate_));
    p += sizeof(val.settlDate_);
    *p = '.';
    ++p;

    memcpy(p, &(val.price_), sizeof(val.price_));
    p += sizeof(val.price_);
    *p = '.';
    ++p;

    memcpy(p, &(val.stopPx_), sizeof(val.stopPx_));
    p += sizeof(val.stopPx_);
    *p = '.';
    ++p;

    memcpy(p, &(val.avgPx_), sizeof(val.avgPx_));
    p += sizeof(val.avgPx_);
    *p = '.';
    ++p;

    memcpy(p, &(val.dayAvgPx_), sizeof(val.dayAvgPx_));
    p += sizeof(val.dayAvgPx_);
    *p = '.';
    ++p;

    memcpy(p, &(val.status_), sizeof(val.status_));
    p += sizeof(val.status_);
    *p = '.';
    ++p;

    memcpy(p, &(val.side_), sizeof(val.side_));
    p += sizeof(val.side_);
    *p = '.';
    ++p;

    memcpy(p, &(val.ordType_), sizeof(val.ordType_));
    p += sizeof(val.ordType_);
    *p = '.';
    ++p;

    memcpy(p, &(val.tif_), sizeof(val.tif_));
    p += sizeof(val.tif_);
    *p = '.';
    ++p;

    memcpy(p, &(val.settlType_), sizeof(val.settlType_));
    p += sizeof(val.settlType_);
    *p = '.';
    ++p;

    memcpy(p, &(val.capacity_), sizeof(val.capacity_));
    p += sizeof(val.capacity_);
    *p = '.';
    ++p;

    memcpy(p, &(val.currency_), sizeof(val.currency_));
    p += sizeof(val.currency_);
    *p = '.';
    ++p;

    memcpy(p, &(val.minQty_), sizeof(val.minQty_));
    p += sizeof(val.minQty_);
    *p = '.';
    ++p;

    memcpy(p, &(val.orderQty_), sizeof(val.orderQty_));
    p += sizeof(val.orderQty_);
    *p = '.';
    ++p;

    memcpy(p, &(val.leavesQty_), sizeof(val.leavesQty_));
    p += sizeof(val.leavesQty_);
    *p = '.';
    ++p;

    memcpy(p, &(val.cumQty_), sizeof(val.cumQty_));
    p += sizeof(val.cumQty_);
    *p = '.';
    ++p;

    memcpy(p, &(val.dayOrderQty_), sizeof(val.dayOrderQty_));
    p += sizeof(val.dayOrderQty_);
    *p = '.';
    ++p;

    memcpy(p, &(val.dayCumQty_), sizeof(val.dayCumQty_));
    p += sizeof(val.dayCumQty_);
    *p = '.';
    ++p;

    p = val.stateMachinePersistance_.serialize(p);
    *p = '.';
    ++p;

    // Format version 1 (#172)
    const u32 instructions = instructionMask(val.execInstruct_);
    memcpy(p, &instructions, sizeof(instructions));
    p += sizeof(instructions);
    *p = '.';
    ++p;

    // Format version 2 (#223)
    *p = val.stopTriggered_ ? '\x01' : '\x00';
    ++p;

    // A swap's far leg (#144), after the execution instructions and the stop's mark (#223)
    if (FXSWAP_ORDERTYPE == val.ordType_)
    {
        *p = '.';
        ++p;
        memcpy(p, &(val.farPrice_), sizeof(val.farPrice_));
        p += sizeof(val.farPrice_);
        *p = '.';
        ++p;
        memcpy(p, &(val.farSettlDate_), sizeof(val.farSettlDate_));
        p += sizeof(val.farSettlDate_);
    }

    return p;
}

OrderEntry *OrderCodec::decode(const IdT &id, u32 formatVersion, const char *buf, size_t size)
{
    assert(nullptr != buf);
    expectNotEmpty(size, "OrderEntry");
    const char *const end = buf + size;

    SourceIdT sourceId, destId, clOrderId, origClOrderID, instrumentId, accountId, clearingId, executionsId;

    const char *p = instrumentId.restore(buf, size);
    p = skipSeparator(p, end, "Invalid format of the encoded OrderEntry - missed '.' after instrumentId!");

    p = accountId.restore(p, size - (p - buf));
    p = skipSeparator(p, end, "Invalid format of the encoded OrderEntry - missed '.' after accountId!");

    p = clearingId.restore(p, size - (p - buf));
    p = skipSeparator(p, end, "Invalid format of the encoded OrderEntry - missed '.' after cleaingId!");

    p = destId.restore(p, size - (p - buf));
    p = skipSeparator(p, end, "Invalid format of the encoded OrderEntry - missed '.' after destinationId!");

    p = clOrderId.restore(p, size - (p - buf));
    p = skipSeparator(p, end, "Invalid format of the encoded OrderEntry - missed '.' after clOrderId_!");

    p = origClOrderID.restore(p, size - (p - buf));
    p = skipSeparator(p, end, "Invalid format of the encoded OrderEntry - missed '.' after origClOrderId_!");

    p = sourceId.restore(p, size - (p - buf));
    p = skipSeparator(p, end, "Invalid format of the encoded OrderEntry - missed '.' after source_!");

    p = executionsId.restore(p, size - (p - buf));
    p = skipSeparator(p, end, "Invalid format of the encoded OrderEntry - missed '.' after executions_!");

    std::unique_ptr<OrderEntry> val(
        new OrderEntry(sourceId, destId, clOrderId, origClOrderID, instrumentId, accountId, clearingId, executionsId));
    val->orderId_ = id;

    p = val->origOrderId_.restore(p, size - (p - buf));
    p = skipSeparator(p, end, "Invalid format of the encoded OrderEntry - missed '.' after origOrderId_!");

    if (size < (p - buf) + sizeof(val->creationTime_))
    {
        throw std::runtime_error(
            "Invalid format of the encoded OrderEntry - size less than required, unable decode creationTime!");
    }
    memcpy(&(val->creationTime_), p, sizeof(val->creationTime_));
    p += sizeof(val->creationTime_);
    p = skipSeparator(p, end, "Invalid format of the encoded OrderEntry - missed '.' after creationTime!");

    if (size < (p - buf) + sizeof(val->lastUpdateTime_))
    {
        throw std::runtime_error(
            "Invalid format of the encoded OrderEntry - size less than required, unable decode lastUpdateTime_!");
    }
    memcpy(&(val->lastUpdateTime_), p, sizeof(val->lastUpdateTime_));
    p += sizeof(val->lastUpdateTime_);
    p = skipSeparator(p, end, "Invalid format of the encoded OrderEntry - missed '.' after lastUpdateTime_!");

    if (size < (p - buf) + sizeof(val->expireTime_))
    {
        throw std::runtime_error(
            "Invalid format of the encoded OrderEntry - size less than required, unable decode expireTime_!");
    }
    memcpy(&(val->expireTime_), p, sizeof(val->expireTime_));
    p += sizeof(val->expireTime_);
    p = skipSeparator(p, end, "Invalid format of the encoded OrderEntry - missed '.' after expireTime_!");

    if (size < (p - buf) + sizeof(val->settlDate_))
    {
        throw std::runtime_error(
            "Invalid format of the encoded OrderEntry - size less than required, unable decode settlDate_!");
    }
    memcpy(&(val->settlDate_), p, sizeof(val->settlDate_));
    p += sizeof(val->settlDate_);
    p = skipSeparator(p, end, "Invalid format of the encoded OrderEntry - missed '.' after settlDate_!");

    if (size < (p - buf) + sizeof(val->price_))
    {
        throw std::runtime_error(
            "Invalid format of the encoded OrderEntry - size less than required, unable decode price_!");
    }
    memcpy(&(val->price_), p, sizeof(val->price_));
    p += sizeof(val->price_);
    p = skipSeparator(p, end, "Invalid format of the encoded OrderEntry - missed '.' after price_!");

    if (size < (p - buf) + sizeof(val->stopPx_))
    {
        throw std::runtime_error(
            "Invalid format of the encoded OrderEntry - size less than required, unable decode stopPx_!");
    }
    memcpy(&(val->stopPx_), p, sizeof(val->stopPx_));
    p += sizeof(val->stopPx_);
    p = skipSeparator(p, end, "Invalid format of the encoded OrderEntry - missed '.' after stopPx_!");

    if (size < (p - buf) + sizeof(val->avgPx_))
    {
        throw std::runtime_error(
            "Invalid format of the encoded OrderEntry - size less than required, unable decode avgPx_!");
    }
    memcpy(&(val->avgPx_), p, sizeof(val->avgPx_));
    p += sizeof(val->avgPx_);
    p = skipSeparator(p, end, "Invalid format of the encoded OrderEntry - missed '.' after avgPx_!");

    if (size < (p - buf) + sizeof(val->dayAvgPx_))
    {
        throw std::runtime_error(
            "Invalid format of the encoded OrderEntry - size less than required, unable decode dayAvgPx_!");
    }
    memcpy(&(val->dayAvgPx_), p, sizeof(val->dayAvgPx_));
    p += sizeof(val->dayAvgPx_);
    p = skipSeparator(p, end, "Invalid format of the encoded OrderEntry - missed '.' after dayAvgPx_!");

    if (size < (p - buf) + sizeof(val->status_))
    {
        throw std::runtime_error(
            "Invalid format of the encoded OrderEntry - size less than required, unable decode status_!");
    }
    memcpy(&(val->status_), p, sizeof(val->status_));
    p += sizeof(val->status_);
    p = skipSeparator(p, end, "Invalid format of the encoded OrderEntry - missed '.' after status_!");

    if (size < (p - buf) + sizeof(val->side_))
    {
        throw std::runtime_error(
            "Invalid format of the encoded OrderEntry - size less than required, unable decode side_!");
    }
    memcpy(&(val->side_), p, sizeof(val->side_));
    p += sizeof(val->side_);
    p = skipSeparator(p, end, "Invalid format of the encoded OrderEntry - missed '.' after side_!");

    if (size < (p - buf) + sizeof(val->ordType_))
    {
        throw std::runtime_error(
            "Invalid format of the encoded OrderEntry - size less than required, unable decode ordType_!");
    }
    memcpy(&(val->ordType_), p, sizeof(val->ordType_));
    p += sizeof(val->ordType_);
    p = skipSeparator(p, end, "Invalid format of the encoded OrderEntry - missed '.' after ordType_!");

    if (size < (p - buf) + sizeof(val->tif_))
    {
        throw std::runtime_error(
            "Invalid format of the encoded OrderEntry - size less than required, unable decode tif_!");
    }
    memcpy(&(val->tif_), p, sizeof(val->tif_));
    p += sizeof(val->tif_);
    p = skipSeparator(p, end, "Invalid format of the encoded OrderEntry - missed '.' after tif_!");

    if (size < (p - buf) + sizeof(val->settlType_))
    {
        throw std::runtime_error(
            "Invalid format of the encoded OrderEntry - size less than required, unable decode settlType_!");
    }
    memcpy(&(val->settlType_), p, sizeof(val->settlType_));
    p += sizeof(val->settlType_);
    p = skipSeparator(p, end, "Invalid format of the encoded OrderEntry - missed '.' after settlType_!");

    if (size < (p - buf) + sizeof(val->capacity_))
    {
        throw std::runtime_error(
            "Invalid format of the encoded OrderEntry - size less than required, unable decode capacity_!");
    }
    memcpy(&(val->capacity_), p, sizeof(val->capacity_));
    p += sizeof(val->capacity_);
    p = skipSeparator(p, end, "Invalid format of the encoded OrderEntry - missed '.' after capacity_!");

    if (size < (p - buf) + sizeof(val->currency_))
    {
        throw std::runtime_error(
            "Invalid format of the encoded OrderEntry - size less than required, unable decode currency_!");
    }
    memcpy(&(val->currency_), p, sizeof(val->currency_));
    p += sizeof(val->currency_);
    p = skipSeparator(p, end, "Invalid format of the encoded OrderEntry - missed '.' after currency_!");

    if (size < (p - buf) + sizeof(val->minQty_))
    {
        throw std::runtime_error(
            "Invalid format of the encoded OrderEntry - size less than required, unable decode minQty_!");
    }
    memcpy(&(val->minQty_), p, sizeof(val->minQty_));
    p += sizeof(val->minQty_);
    p = skipSeparator(p, end, "Invalid format of the encoded OrderEntry - missed '.' after minQty_!");

    if (size < (p - buf) + sizeof(val->orderQty_))
    {
        throw std::runtime_error(
            "Invalid format of the encoded OrderEntry - size less than required, unable decode orderQty_!");
    }
    memcpy(&(val->orderQty_), p, sizeof(val->orderQty_));
    p += sizeof(val->orderQty_);
    p = skipSeparator(p, end, "Invalid format of the encoded OrderEntry - missed '.' after orderQty_!");

    if (size < (p - buf) + sizeof(val->leavesQty_))
    {
        throw std::runtime_error(
            "Invalid format of the encoded OrderEntry - size less than required, unable decode leavesQty_!");
    }
    memcpy(&(val->leavesQty_), p, sizeof(val->leavesQty_));
    p += sizeof(val->leavesQty_);
    p = skipSeparator(p, end, "Invalid format of the encoded OrderEntry - missed '.' after leavesQty_!");

    if (size < (p - buf) + sizeof(val->cumQty_))
    {
        throw std::runtime_error(
            "Invalid format of the encoded OrderEntry - size less than required, unable decode cumQty_!");
    }
    memcpy(&(val->cumQty_), p, sizeof(val->cumQty_));
    p += sizeof(val->cumQty_);
    p = skipSeparator(p, end, "Invalid format of the encoded OrderEntry - missed '.' after cumQty_!");

    if (size < (p - buf) + sizeof(val->dayOrderQty_))
    {
        throw std::runtime_error(
            "Invalid format of the encoded OrderEntry - size less than required, unable decode dayOrderQty_!");
    }
    memcpy(&(val->dayOrderQty_), p, sizeof(val->dayOrderQty_));
    p += sizeof(val->dayOrderQty_);
    p = skipSeparator(p, end, "Invalid format of the encoded OrderEntry - missed '.' after dayOrderQty_!");

    if (size < (p - buf) + sizeof(val->dayCumQty_))
    {
        throw std::runtime_error(
            "Invalid format of the encoded OrderEntry - size less than required, unable decode dayCumQty_!");
    }
    memcpy(&(val->dayCumQty_), p, sizeof(val->dayCumQty_));
    p += sizeof(val->dayCumQty_);
    p = skipSeparator(p, end, "Invalid format of the encoded OrderEntry - missed '.' after dayCumQty_!");

    p = val->stateMachinePersistance_.restore(p, size - (p - buf));
    if (1 <= formatVersion)
    {
        // Format version 1 (#172): the execution instructions. A version 0 record has none.
        p = skipSeparator(p, end, "Invalid format of the encoded OrderEntry - missed '.' after its state ids!");
        u32 instructions = 0;
        if (static_cast<size_t>(end - p) < sizeof(instructions))
        {
            throw std::runtime_error(
                "Invalid format of the encoded OrderEntry - size less than required, unable decode execInstruct_!");
        }
        memcpy(&instructions, p, sizeof(instructions));
        p += sizeof(instructions);
        val->execInstruct_ = instructionsFromMask(instructions);
    }
    // Format version 2 (#223): whether a stop order's stop was reached. Not reached from an older version, said here
    // rather than left to the field's initialiser: a stop order from before #223 is held, where a released STOP would
    // trade at once as a market order.
    val->stopTriggered_ = false;
    if (2 <= formatVersion)
    {
        p = skipSeparator(p, end, "Invalid format of the encoded OrderEntry - missed '.' after execInstruct_!");
        if (end == p)
        {
            throw std::runtime_error(
                "Invalid format of the encoded OrderEntry - size less than required, unable decode stopTriggered_!");
        }
        if ((0 != *p) && (1 != *p))
        {
            throw std::runtime_error("Invalid format of the encoded OrderEntry - stopTriggered_ is neither 0 nor 1!");
        }
        val->stopTriggered_ = (1 == *p);
        ++p;
    }

    // A swap's far leg follows its own '.' marker, after format version 1's execution instructions (#172) and version
    // 2's stop mark (#223), and every other record ends here. Decide by content, not by version: every format version
    // has the far leg, and when swaps came in, the dispatcher stored a record under the storage's own write counter,
    // not the version encode() returned, so a swap written once came back as version 0 (#144). The content is the
    // order's type: encode() has written the far leg for every swap and for nothing else since swaps came in (e297f98),
    // so a swap's record without one is cut short, and is refused rather than restored with a far leg of 0 (#149).
    if (FXSWAP_ORDERTYPE == val->ordType_)
    {
        p = skipSeparator(p, end, "Invalid format of the encoded OrderEntry - missed '.' before farPrice_!");
        if (size < static_cast<size_t>(p - buf) + sizeof(val->farPrice_))
        {
            throw std::runtime_error(
                "Invalid format of the encoded OrderEntry - size less than required, unable decode farPrice_!");
        }
        memcpy(&(val->farPrice_), p, sizeof(val->farPrice_));
        p += sizeof(val->farPrice_);
        p = skipSeparator(p, end, "Invalid format of the encoded OrderEntry - missed '.' after farPrice_!");
        if (size < static_cast<size_t>(p - buf) + sizeof(val->farSettlDate_))
        {
            throw std::runtime_error(
                "Invalid format of the encoded OrderEntry - size less than required, unable decode farSettlDate_!");
        }
        memcpy(&(val->farSettlDate_), p, sizeof(val->farSettlDate_));
        p += sizeof(val->farSettlDate_);
    }
    // After the far leg, not before it: a swap's record goes on past the state machine's persistence (#149)
    expectConsumed(p, end, "OrderEntry");

    return val.release();
}