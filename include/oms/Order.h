#pragma once

#include "oms/OrderId.h"
#include "oms/Types.h"

namespace oms {

// 128-bit signed fill notional (price * qty), divided on demand for avg price;
// int64 can overflow for large tick-price * size, so 128 bits is used.
using TotalFillAmount = __int128;

enum class OrderStatus : uint8_t {
  New,             // created locally, not on the wire
  Sent,            // written to transport, no ack yet
  Confirmed,       // live on book
  PartiallyFilled,
  SentCancel,      // cancel in flight
  CancelRejected,
  Filled,          // TERMINAL
  Cancelled,       // TERMINAL
  Rejected,        // TERMINAL
};

constexpr bool is_terminal(OrderStatus s) {
  return s == OrderStatus::Filled
      || s == OrderStatus::Cancelled
      || s == OrderStatus::Rejected;
}

// Key timestamps for latency / time-to-fill metrics.
struct OrderTimestamps {
  Timestamp created{0};
  Timestamp sent{0};
  Timestamp confirmed{0};
  Timestamp first_fill{0};
  Timestamp last_fill{0};
  Timestamp terminal{0};
};

// One order through its full lifecycle. The cache-line HOT BLOCK groups every
// field touched on an ack/fill; DO NOT REORDER it.
struct alignas(64) Order {
  // ===== HOT BLOCK (do not reorder) =====
  OrderIdRaw      internal_id{0};                 // packed venue|gen|slot|seq
  Quantity        size{0};
  Quantity        filled_size{0};
  TotalFillAmount total_fill_amount{0};           // running notional; /qty on demand
  Price           limit_price{0};
  InstrumentId    instrument{0};
  OrderStatus     status{OrderStatus::New};
  Side            side{Side::Buy};
  OrderType       type{OrderType::Limit};
  bool            pending_cancel{false};          // cancel in flight (orthogonal to status)
  bool            false_cancel_reject{false};     // benign cancel-reject expected
  // ===== end HOT BLOCK =====

  std::string     exchange_id;                    // assigned on confirmation
  StrategyId      strategy_id{0};
  AlgoTag         algo_tag{0};
  bool            ack_timed_out{false};           // ack timer expired (mode 1)
  bool            reconciliation{false};          // corrective order (2.10); off metrics/netting
  OrderTimestamps ts{};

  // Scratch tag (spec 2.7): an algo stamps its index here and filters the global
  // event stream by it, so the OMS needs no algo registry.
  static constexpr int kScratchSlots = 4;
  std::array<int64_t, kScratchSlots> int_data{};

  void store_int_data(int64_t value, int offset) { int_data[static_cast<std::size_t>(offset)] = value; }
  int64_t retrieve_int_data(int offset) const { return int_data[static_cast<std::size_t>(offset)]; }

  constexpr Quantity remaining() const { return size - filled_size; }

  // Average fill price in ticks (0 if unfilled).
  constexpr Price avg_fill_price() const {
    if (filled_size == 0) return 0;
    return static_cast<Price>(total_fill_amount / static_cast<TotalFillAmount>(filled_size));
  }
};

}  // namespace oms
