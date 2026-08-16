#pragma once

#include "oms/MarketData.h"
#include "oms/Order.h"   // pulls OrderId.h + Types.h

namespace oms {

// Result of a send/cancel attempt (spec 2.4).
enum class SendResult : uint8_t {
  Ok,
  RejectedLocally,   // failed pre-transport validation / risk
  TransportDown,     // not connected
  RateLimited,
  Duplicate,
};

// Why an order (or a cancel) was rejected by the venue.
enum class RejectReason : uint8_t {
  Unknown,
  RiskLimit,
  InvalidPrice,
  InvalidSize,
  PostOnlyWouldCross,
  RateLimited,
  TransportError,
  CancelRejected,    // a cancel could not be applied (may be a benign fill race)
};

// Async event delivery (spec 2.4): the client calls the sink SYNCHRONOUSLY from
// poll(), on the polling thread, with no queue between exchange and OMS -- so
// order and position state can never be observed inconsistently.
class ExchangeEventSink {
 public:
  virtual ~ExchangeEventSink() = default;

  virtual void on_confirm(OrderIdRaw, const ExchangeOrderId&, Timestamp) = 0;
  virtual void on_fill(OrderIdRaw, Quantity fill_qty, Price fill_price,
                       TradeId, Timestamp) = 0;
  virtual void on_reject(OrderIdRaw, RejectReason, Timestamp) = 0;
  virtual void on_cancel_ack(OrderIdRaw, Timestamp) = 0;
  virtual void on_unsolicited_cancel(OrderIdRaw, Timestamp) = 0;
};

// The exchange interface the whole system depends on (spec 2.4). A "fat" base:
// capability methods default to "not supported". SimulatedExchange implements
// the same interface as a real venue.
class ExchangeClient {
 public:
  virtual ~ExchangeClient() = default;

  virtual VenueId venue_id() const = 0;

  virtual SendResult place_order(const Order&) = 0;
  virtual SendResult cancel_order(OrderIdRaw, const ExchangeOrderId&) = 0;

  virtual std::unordered_map<InstrumentId, Quantity> get_positions() = 0;
  virtual TopOfBook get_order_book(InstrumentId) = 0;

  // Register the sink and drain the transport, invoking the sink inline.
  virtual void set_event_sink(ExchangeEventSink*) = 0;
  virtual void poll() = 0;

  virtual bool is_connected() const = 0;

  // ---- capability defaults ----
  virtual bool supports_mass_cancel() const { return false; }
  virtual SendResult mass_cancel(InstrumentId) { return SendResult::RejectedLocally; }
};

}  // namespace oms
