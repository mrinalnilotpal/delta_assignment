#pragma once

#include "oms/ExchangeRegistry.h"   // pulls ExchangeClient.h -> Order.h -> Types.h
#include "oms/Health.h"
#include "oms/Logging.h"
#include "oms/OrderPool.h"          // pulls Order.h / OrderId.h
#include "oms/Position.h"
#include "oms/Router.h"

namespace oms {

// A strategy/algo request; `size` is a magnitude (>0), direction from `side`.
struct OrderRequest {
  InstrumentId instrument{0};
  Side         side{Side::Buy};
  Quantity     size{0};
  Price        price{0};
  OrderType    type{OrderType::Limit};
  StrategyId   strategy_id{0};
  AlgoTag      algo_tag{0};
  int64_t      worker_index{0};   // algo self-fill routing tag (spec 2.7)
  int          worker_offset{0};  // -> Order.int_data[offset]
  bool         reconciliation{false};   // corrective order (2.10); off metrics/netting
};

// Inbound event kinds, for the single shared validation path.
enum class EventKind : uint8_t { Confirm, Fill, Reject, CancelAck, UnsolicitedCancel };

// Result of the shared guard sequence. Anything but Ok is logged and returned;
// the event is never silently applied.
enum class ValidationResult : uint8_t {
  Ok,
  UnknownOrder,     // id not live (stale generation / never existed)
  TerminalState,    // order already Filled/Cancelled/Rejected
  DuplicateTrade,   // trade id already seen for this order
  StaleCumulative,  // cumulative fill <= max already applied
  FieldMismatch,    // price/size/side/overfill sanity failed
};

// Everything a validation call needs.
struct EventContext {
  EventKind kind{EventKind::Confirm};
  TradeId   trade_id{0};
  Quantity  qty{0};
  Price     price{0};
};

// Published order events. Strategies subscribe; the position is ALWAYS updated
// before any subscriber runs (the ordering invariant, spec 2.3).
enum class OrderEventKind : uint8_t {
  Confirmed, PartialFill, Fill, Rejected, Cancelled, UnsolicitedCancel,
};

struct OrderEvent {
  OrderEventKind kind{OrderEventKind::Confirmed};
  OrderIdRaw     internal_id{0};
  InstrumentId   instrument{0};
  Side           side{Side::Buy};
  Quantity       last_qty{0};
  Price          last_price{0};
  Quantity       filled_size{0};
  Quantity       size{0};
  OrderStatus    status{OrderStatus::New};
  RejectReason   reason{RejectReason::Unknown};   // valid on Rejected events
  Timestamp      ts{0};
};

class OrderEventListener {
 public:
  virtual ~OrderEventListener() = default;
  virtual void on_order_event(const OrderEvent&) = 0;
};

// Owns the pool, live-order index, and positions. Implements ExchangeEventSink
// so the client calls it synchronously from poll() (single-writer, lock-free).
class OrderManager : public ExchangeEventSink {
 public:
  OrderManager(ExchangeRegistry& registry, Router& router, HealthModel& health,
               ILogSink* log = nullptr,
               uint32_t pool_capacity = OrderPool::kDefaultCapacity);

  void set_clock(std::function<Timestamp()> clock) { clock_ = std::move(clock); }

  // Subscribers run AFTER the position is updated inside on_fill (spec 2.3).
  void subscribe(OrderEventListener* l) { listeners_.push_back(l); }

  // ---- strategy-facing API ----
  OrderIdRaw submit(const OrderRequest&);
  bool       cancel(OrderIdRaw);

  // ---- ExchangeEventSink (called on the poll/event-loop thread) ----
  void on_confirm(OrderIdRaw, const ExchangeOrderId&, Timestamp) override;
  void on_fill(OrderIdRaw, Quantity, Price, TradeId, Timestamp) override;
  void on_reject(OrderIdRaw, RejectReason, Timestamp) override;
  void on_cancel_ack(OrderIdRaw, Timestamp) override;
  void on_unsolicited_cancel(OrderIdRaw, Timestamp) override;

  // ---- failure-mode hooks (spec 2.11) ----
  // Mode 1: orders unacked past `timeout` are marked, a cancel is attempted, and
  // health is told; the order stays live (possibly-live, never assumed dead).
  void check_ack_timeouts(Timestamp now, Timestamp timeout);
  // Mode 7: on reconnect, request a reconcile instead of assuming orders died.
  void on_venue_disconnect(VenueId);
  void on_venue_reconnect(VenueId);

  // ---- queries ----
  const Position& position(InstrumentId) const;
  Quantity        pending_quantity(InstrumentId) const;   // signed sum of remaining()
  const Order*    find(OrderIdRaw) const;                  // nullptr if not live
  std::vector<InstrumentId> instruments() const;           // instruments with a tracked position

  // The execution delta the brief defines: target - (position + pending).
  Quantity execution_delta(InstrumentId inst, Quantity target) const {
    return target - (position(inst).net + pending_quantity(inst));
  }

  // Single shared guard sequence, exposed so tests can hit it directly.
  ValidationResult validate(OrderIdRaw, const EventContext&) const;

  // Unknown fills to tolerate before the kill switch trips (mode 4).
  void set_unknown_fill_hardstop_threshold(uint64_t n) { unknown_fill_hardstop_threshold_ = n; }

  // ---- diagnostics / policy state ----
  bool        killed() const { return killed_; }
  const std::string& kill_reason() const { return kill_reason_; }
  bool        rebalance_incomplete() const { return rebalance_incomplete_; }
  void        reset_rebalance_cycle() { rebalance_incomplete_ = false; }
  uint64_t    false_cancel_reject_count() const { return false_cancel_reject_count_; }
  std::size_t live_order_count() const { return live_.size(); }

  // Reconcile request flag (set on unknown fill / reconnect); drained by caller.
  bool     reconcile_requested() const { return reconcile_requested_; }
  void     clear_reconcile_request() { reconcile_requested_ = false; }

  // ---- error / recovery counters (metrics, spec 2.12) ----
  uint64_t reject_count() const { return reject_count_; }
  uint64_t ack_timeout_count() const { return ack_timeout_count_; }
  uint64_t unsolicited_cancel_count() const { return unsolicited_cancel_count_; }
  uint64_t out_of_sequence_dropped() const { return oos_dropped_count_; }
  uint64_t unknown_fill_count() const { return unknown_fill_count_; }

 private:
  uint32_t slot_of(OrderIdRaw id) const { return decode_order_id(id).slot; }
  Order&   order_ref(OrderIdRaw id) { return pool_[slot_of(id)]; }
  const Order& order_ref(OrderIdRaw id) const { return pool_[slot_of(id)]; }

  void publish(const OrderEvent&);
  void log_rejection(OrderIdRaw, EventKind, ValidationResult, Timestamp) const;
  void retire(OrderIdRaw, InstrumentId, Quantity remaining_to_cancel, Side);
  // Shared body for cancel-ack and unsolicited-cancel: validate, go terminal,
  // publish `pub`, retire. `unsolicited` adds the counter + warning log.
  void finish_cancel(OrderIdRaw, Timestamp, EventKind, OrderEventKind pub, bool unsolicited);
  void trip_kill_switch(const std::string& reason);
  void cancel_working_orders();

  OrderPool        pool_;
  ExchangeRegistry& registry_;
  Router&          router_;
  HealthModel&     health_;
  ILogSink*        log_;
  std::function<Timestamp()> clock_;

  std::unordered_map<InstrumentId, Position> positions_;
  std::unordered_map<InstrumentId, Quantity> pending_signed_;
  std::unordered_map<VenueId, uint64_t>      venue_seq_;
  std::unordered_map<OrderIdRaw, std::unordered_set<TradeId>> trade_ids_;
  std::unordered_set<OrderIdRaw>             live_;
  std::unordered_set<OrderIdRaw>             expected_false_cancel_rejects_;

  // Terminal orders leave the live index but their slot is freed only after a
  // grace window, so recent duplicate/late events resolve to TerminalState
  // (dropped) rather than UnknownOrder (kill switch); older ones become stale.
  std::deque<OrderIdRaw>                     retired_fifo_;
  std::size_t                               retired_grace_{4096};

  std::vector<OrderEventListener*> listeners_;

  bool        killed_{false};
  std::string kill_reason_;
  bool        rebalance_incomplete_{false};
  uint64_t    false_cancel_reject_count_{0};

  bool        reconcile_requested_{false};
  uint64_t    unknown_fill_count_{0};
  uint64_t    unknown_fill_hardstop_threshold_{3};
  uint64_t    reject_count_{0};
  uint64_t    ack_timeout_count_{0};
  uint64_t    unsolicited_cancel_count_{0};
  uint64_t    oos_dropped_count_{0};

  Position    empty_position_{};
};

}  // namespace oms
