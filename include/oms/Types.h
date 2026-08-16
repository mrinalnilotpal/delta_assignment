#pragma once

#include "oms/Common.h"

namespace oms {

// Core domain scalar types (spec 2.1). Byte-width enums pack into the hot Order.
enum class Side : uint8_t { Buy, Sell };
enum class OrderType : uint8_t { Limit, Market, IOC, PostOnly };

using InstrumentId = uint16_t;

// Integer ticks, never double: a tick grid must reconcile to the cent exactly.
using Price = int64_t;

// Signed; the sign carries direction for targets and deltas.
using Quantity = int64_t;

using Timestamp = int64_t;   // nanoseconds since an arbitrary steady epoch
using VenueId   = uint8_t;   // matches the 8-bit venue field of the packed id
using LatencyNs = int64_t;
using TradeId   = uint64_t;              // for per-order fill de-duplication
using ExchangeOrderId = std::string;     // opaque id from the venue

// Shared across order / signal / netting.
using StrategyId = uint16_t;
using AlgoTag    = uint8_t;

// Signed magnitude: Buy -> +, Sell -> -.
constexpr Quantity signed_qty(Side side, Quantity magnitude) {
  return side == Side::Buy ? magnitude : -magnitude;
}

}  // namespace oms
