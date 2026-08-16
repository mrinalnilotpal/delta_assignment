#pragma once

// Single home for the small internal helpers that previously lived in a per-.cpp
// anonymous `namespace { ... }`. Defined once here (inline / templates) so no
// translation unit re-declares them at file scope. Not part of the public API:
// everything lives in oms::detail.

#include <cstring>

#include "oms/Health.h"        // HealthState, TripReason
#include "oms/OrderManager.h"  // EventKind, ValidationResult, OrderStatus, ExchangeFactoryFn

namespace oms::detail {

// ---- generic math ----------------------------------------------------------
inline Quantity ceil_div(Quantity a, Quantity b) {
  if (b <= 0) return a;
  return (a + b - 1) / b;
}

// Signed slippage in basis points of avg vs a reference price (sign: +1 buy, -1 sell).
inline int64_t bps(Price avg, Price ref, int sign) {
  if (ref == 0) return 0;
  const double diff = static_cast<double>(avg - ref) * static_cast<double>(sign);
  return static_cast<int64_t>(diff / static_cast<double>(ref) * 10'000.0);
}

// splitmix64 step: deterministic PRNG for the seeded demo signal producer.
inline uint64_t next_rand(uint64_t& s) {
  uint64_t z = (s += 0x9E3779B97F4A7C15ull);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  return z ^ (z >> 31);
}

// ---- length-prefixed binary codec helpers ----------------------------------
template <typename T>
void put(std::vector<uint8_t>& b, const T& v) {
  const auto* p = reinterpret_cast<const uint8_t*>(&v);
  b.insert(b.end(), p, p + sizeof(T));
}

template <typename T>
bool get(const std::vector<uint8_t>& b, std::size_t& off, T& v) {
  if (off + sizeof(T) > b.size()) return false;
  std::memcpy(&v, b.data() + off, sizeof(T));
  off += sizeof(T);
  return true;
}

// ---- exchange factory registry (process-wide name -> constructor) ----------
inline std::unordered_map<std::string, ExchangeFactoryFn>& factory_table() {
  static std::unordered_map<std::string, ExchangeFactoryFn> table;
  return table;
}

// ---- enum -> string for diagnostics/logging --------------------------------
inline const char* name(EventKind k) {
  switch (k) {
    case EventKind::Confirm:           return "confirm";
    case EventKind::Fill:              return "fill";
    case EventKind::Reject:            return "reject";
    case EventKind::CancelAck:         return "cancel_ack";
    case EventKind::UnsolicitedCancel: return "unsolicited_cancel";
  }
  return "?";
}

inline const char* name(ValidationResult r) {
  switch (r) {
    case ValidationResult::Ok:              return "ok";
    case ValidationResult::UnknownOrder:    return "unknown_order";
    case ValidationResult::TerminalState:   return "terminal_state";
    case ValidationResult::DuplicateTrade:  return "duplicate_trade";
    case ValidationResult::StaleCumulative: return "stale_cumulative";
    case ValidationResult::FieldMismatch:   return "field_mismatch";
  }
  return "?";
}

inline const char* name(OrderStatus s) {
  switch (s) {
    case OrderStatus::New:             return "New";
    case OrderStatus::Sent:            return "Sent";
    case OrderStatus::Confirmed:       return "Confirmed";
    case OrderStatus::PartiallyFilled: return "PartiallyFilled";
    case OrderStatus::SentCancel:      return "SentCancel";
    case OrderStatus::CancelRejected:  return "CancelRejected";
    case OrderStatus::Filled:          return "Filled";
    case OrderStatus::Cancelled:       return "Cancelled";
    case OrderStatus::Rejected:        return "Rejected";
  }
  return "?";
}

inline const char* name(HealthState s) {
  switch (s) {
    case HealthState::Healthy:  return "Healthy";
    case HealthState::Degraded: return "Degraded";
    case HealthState::Down:     return "Down";
    case HealthState::Probing:  return "Probing";
  }
  return "?";
}

inline const char* name(TripReason r) {
  switch (r) {
    case TripReason::None:          return "None";
    case TripReason::Connectivity:  return "Connectivity";
    case TripReason::RejectionRate: return "RejectionRate";
    case TripReason::AckLatency:    return "AckLatency";
  }
  return "?";
}

}  // namespace oms::detail
