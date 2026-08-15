#pragma once

#include "oms/Types.h"

namespace oms {

// Per-instrument position (spec 2.10). Net-only by default; long/short leg
// splitting is a venue-rules concern (see README). Updated atomically inside
// OrderManager::on_fill.
struct Position {
  Quantity  net{0};          // signed net position
  Quantity  overnight{0};    // carry-forward from prior session
  Quantity  volume{0};       // today's traded volume (magnitude)
  int64_t   amount{0};       // signed notional (ticks * qty)
  int64_t   fees{0};
  Timestamp last_update{0};

  void apply(Quantity signed_fill_qty, Price fill_price, Timestamp ts, int64_t fee = 0) {
    net    += signed_fill_qty;
    volume += (signed_fill_qty < 0 ? -signed_fill_qty : signed_fill_qty);
    amount += signed_fill_qty * fill_price;
    fees   += fee;
    last_update = ts;
  }

  // VWAP of the open position (0 when flat).
  Price avg_price() const { return net == 0 ? 0 : amount / net; }
};

}  // namespace oms
