#pragma once

#include "oms/Types.h"

namespace oms {

// Market data consumption (spec 2.5): how the system consumes data, not how it
// is sourced. Push-primary (sinks) + pull-secondary (source queries for POV).
struct TopOfBook {
  Price     bid{0};
  Price     ask{0};
  Quantity  bid_size{0};
  Quantity  ask_size{0};
  Timestamp ts{0};

  constexpr Price mid() const { return (bid + ask) / 2; }
};

struct Trade {
  InstrumentId inst{0};
  Price        price{0};
  Quantity     size{0};
  Timestamp    ts{0};
};

// Push side: subscribers react to book/trade events.
class MarketDataSink {
 public:
  virtual ~MarketDataSink() = default;
  virtual void on_book_update(InstrumentId, const TopOfBook&) = 0;
  virtual void on_trade(const Trade&) = 0;
};

// Pull side: anyone can query current state at any time.
class MarketDataSource {
 public:
  virtual ~MarketDataSource() = default;
  virtual TopOfBook top_of_book(InstrumentId) const = 0;
  // Traded volume in (since, now]. POV sizes child orders from this.
  virtual Quantity volume_since(InstrumentId, Timestamp since) const = 0;
};

}  // namespace oms
