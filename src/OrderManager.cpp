#include "oms/OrderManager.h"

#include <chrono>

namespace oms {

namespace {
Timestamp steady_now_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

const char* kind_name(EventKind k) {
  switch (k) {
    case EventKind::Confirm:           return "confirm";
    case EventKind::Fill:              return "fill";
    case EventKind::Reject:            return "reject";
    case EventKind::CancelAck:         return "cancel_ack";
    case EventKind::UnsolicitedCancel: return "unsolicited_cancel";
  }
  return "?";
}

const char* result_name(ValidationResult r) {
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

const char* status_name(OrderStatus s) {
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
}  // namespace

OrderManager::OrderManager(ExchangeRegistry& registry, Router& router,
                           HealthModel& health, ILogSink* log,
                           uint32_t pool_capacity)
    : pool_(pool_capacity),
      registry_(registry),
      router_(router),
      health_(health),
      log_(log),
      clock_(steady_now_ns) {}

// ---- validation: the single shared guard sequence ---------------------------
ValidationResult OrderManager::validate(OrderIdRaw id, const EventContext& ev) const {
  // (1) Unknown order: stale generation or never existed.
  if (!pool_.is_current(id)) return ValidationResult::UnknownOrder;

  const Order& o = order_ref(id);

  // (2) Terminal state: once final, no event may mutate it.
  if (is_terminal(o.status)) return ValidationResult::TerminalState;

  if (ev.kind == EventKind::Fill) {
    // (3) Duplicate trade id.
    auto it = trade_ids_.find(id);
    if (it != trade_ids_.end() && it->second.contains(ev.trade_id)) {
      return ValidationResult::DuplicateTrade;
    }
    // (5) Field sanity: positive qty/price, and never over-fill. The overfill
    // guard also implements (4): a cumulative report that adds nothing (delta
    // <= 0) or exceeds the order is rejected rather than applied.
    if (ev.qty <= 0 || ev.price <= 0) return ValidationResult::FieldMismatch;
    if (ev.qty > o.remaining())        return ValidationResult::FieldMismatch;
  }

  return ValidationResult::Ok;
}

void OrderManager::log_rejection(OrderIdRaw id, EventKind kind,
                                 ValidationResult vr, Timestamp ts) const {
  if (!log_) return;
  const OrderId d = decode_order_id(id);
  std::string exch;
  OrderStatus status = OrderStatus::New;
  if (pool_.is_current(id)) {
    const Order& o = order_ref(id);
    exch = o.exchange_id;
    status = o.status;
  }
  log_->log(LogLevel::Error,
            std::string("event rejected: kind=") + kind_name(kind) +
                " reason=" + result_name(vr) +
                " internal_id=" + std::to_string(id) +
                " venue=" + std::to_string(d.venue) +
                " slot=" + std::to_string(d.slot) +
                " gen=" + std::to_string(d.generation) +
                " exchange_id=" + (exch.empty() ? "-" : exch) +
                " status=" + status_name(status) +
                " event_ts=" + std::to_string(ts));
}

void OrderManager::publish(const OrderEvent& e) {
  for (auto* l : listeners_) l->on_order_event(e);
}

void OrderManager::trip_kill_switch(const std::string& reason) {
  if (!killed_) {
    killed_ = true;
    kill_reason_ = reason;
    if (log_) log_->log(LogLevel::Error, "KILL SWITCH tripped: " + reason);
  }
}

// Retire a terminal order: leave the live index but free the slot only after a
// grace FIFO, so recent late/duplicate events drop as TerminalState. The oldest
// entry beyond the window is released (generation bumps -> stragglers go stale).
void OrderManager::retire(OrderIdRaw id, InstrumentId inst,
                          Quantity remaining_to_cancel, Side side) {
  if (remaining_to_cancel > 0) {
    pending_signed_[inst] -= signed_qty(side, remaining_to_cancel);
  }
  live_.erase(id);
  trade_ids_.erase(id);
  retired_fifo_.push_back(id);

  if (retired_fifo_.size() > retired_grace_) {
    const OrderIdRaw old = retired_fifo_.front();
    retired_fifo_.pop_front();
    if (pool_.is_current(old)) pool_.release(slot_of(old));
    expected_false_cancel_rejects_.erase(old);
  }
}

void OrderManager::cancel_working_orders() {
  // All-down policy: cancel working orders where the transport still permits.
  std::vector<OrderIdRaw> snapshot(live_.begin(), live_.end());
  for (OrderIdRaw id : snapshot) {
    if (!pool_.is_current(id)) continue;
    Order& o = order_ref(id);
    if (is_terminal(o.status) || o.pending_cancel) continue;
    ExchangeClient* ex = registry_.get(decode_order_id(id).venue);
    if (ex && ex->is_connected()) {
      if (ex->cancel_order(id, o.exchange_id) == SendResult::Ok) {
        o.pending_cancel = true;
        o.status = OrderStatus::SentCancel;
      }
    }
  }
}

// ---- submit / cancel --------------------------------------------------------
OrderIdRaw OrderManager::submit(const OrderRequest& req) {
  if (killed_) {
    if (log_) log_->log(LogLevel::Error, "submit rejected: kill switch active");
    return 0;
  }

  const auto venue_opt = router_.select(req.instrument, req.side, req.size);
  if (!venue_opt) {
    // All-down: don't buffer (a delayed order trades a moved market); cancel
    // working orders, mark the cycle incomplete, and alert.
    rebalance_incomplete_ = true;
    if (log_) {
      log_->log(LogLevel::Error,
                "ALL-DOWN: no tradeable venue for instrument=" +
                    std::to_string(req.instrument) +
                    "; not submitting, cancelling working orders, cycle marked incomplete");
    }
    cancel_working_orders();
    return 0;
  }

  const VenueId venue = *venue_opt;
  ExchangeClient* ex = registry_.get(venue);
  if (!ex) {
    if (log_) log_->log(LogLevel::Error,
                        "submit rejected: no client for venue=" + std::to_string(venue));
    return 0;
  }

  // Mode 10: pool exhaustion hard-stops with a diagnostic before we touch the
  // pool (its own assert is the backstop).
  if (pool_.in_use() >= pool_.capacity()) {
    trip_kill_switch("ORDER_POOL_EXHAUSTED in_use=" + std::to_string(pool_.in_use()) +
                     " capacity=" + std::to_string(pool_.capacity()));
    return 0;
  }

  const Timestamp now = clock_();
  const uint32_t slot = pool_.acquire();
  const uint16_t gen = pool_.generation(slot);
  const uint64_t seq = ++venue_seq_[venue];   // starts at 1 -> id is never 0
  const OrderIdRaw id =
      encode_order_id(venue, gen, static_cast<uint32_t>(slot), static_cast<uint16_t>(seq));

  Order& o = pool_[slot];
  o.internal_id = id;
  o.instrument = req.instrument;
  o.side = req.side;
  o.type = req.type;
  o.size = req.size;
  o.filled_size = 0;
  o.total_fill_amount = 0;
  o.limit_price = req.price;
  o.status = OrderStatus::New;
  o.strategy_id = req.strategy_id;
  o.algo_tag = req.algo_tag;
  o.reconciliation = req.reconciliation;
  o.ts.created = now;
  o.store_int_data(req.worker_index, req.worker_offset);   // algo self-fill routing tag

  positions_.try_emplace(req.instrument);
  pending_signed_[req.instrument] += signed_qty(req.side, req.size);
  live_.insert(id);   // dedup set is created lazily on the first fill

  const SendResult r = ex->place_order(o);
  health_.on_order_sent(venue, now);

  if (r != SendResult::Ok) {
    // Local rejection before the wire: unwind the reservation.
    o.status = OrderStatus::Rejected;
    ++reject_count_;
    health_.on_reject(venue, /*is_false_cancel_reject=*/false);
    if (log_) log_->log(LogLevel::Warn,
                        "place_order local reject: internal_id=" + std::to_string(id));
    retire(id, req.instrument, req.size, req.side);
    return 0;
  }

  o.status = OrderStatus::Sent;
  o.ts.sent = now;
  return id;
}

bool OrderManager::cancel(OrderIdRaw id) {
  if (!pool_.is_current(id)) {
    if (log_) log_->log(LogLevel::Warn, "cancel of unknown/stale order: id=" + std::to_string(id));
    return false;
  }
  Order& o = order_ref(id);
  if (is_terminal(o.status)) {
    if (log_) log_->log(LogLevel::Warn, "cancel of terminal order: id=" + std::to_string(id));
    return false;
  }
  ExchangeClient* ex = registry_.get(decode_order_id(id).venue);
  if (!ex) return false;

  const SendResult r = ex->cancel_order(id, o.exchange_id);
  if (r != SendResult::Ok) {
    if (log_) log_->log(LogLevel::Warn, "cancel send failed: id=" + std::to_string(id));
    return false;
  }
  // pending_cancel is orthogonal to status: a later fill may set status to
  // PartiallyFilled while pending_cancel still records the cancel in flight.
  o.pending_cancel = true;
  o.status = OrderStatus::SentCancel;
  return true;
}

// ---- inbound handlers (poll/event-loop thread) ------------------------------
void OrderManager::on_confirm(OrderIdRaw id, const ExchangeOrderId& exch_id, Timestamp ts) {
  const ValidationResult vr = validate(id, {EventKind::Confirm, 0, 0, 0});
  if (vr != ValidationResult::Ok) {
    log_rejection(id, EventKind::Confirm, vr, ts);
    ++oos_dropped_count_;
    return;
  }

  Order& o = order_ref(id);
  o.exchange_id = exch_id;
  if (o.status == OrderStatus::New || o.status == OrderStatus::Sent) {
    o.status = OrderStatus::Confirmed;
  }
  o.ts.confirmed = ts;

  // Confirm always follows a send, so ts.sent is set (and may legitimately be 0
  // at logical time 0); record the ack latency unconditionally.
  const VenueId venue = decode_order_id(id).venue;
  health_.on_ack(venue, ts - o.ts.sent);

  publish(OrderEvent{OrderEventKind::Confirmed, id, o.instrument, o.side, 0, 0,
                     o.filled_size, o.size, o.status, RejectReason::Unknown, ts});
}

void OrderManager::on_fill(OrderIdRaw id, Quantity qty, Price price, TradeId trade,
                           Timestamp ts) {
  const ValidationResult vr = validate(id, {EventKind::Fill, trade, qty, price});
  if (vr != ValidationResult::Ok) {
    log_rejection(id, EventKind::Fill, vr, ts);
    ++oos_dropped_count_;
    // Mode 4: an unknown fill means our position is wrong. Don't apply it; log,
    // request a reconcile, and hard-stop only if it recurs (a storm = corruption).
    if (vr == ValidationResult::UnknownOrder) {
      ++unknown_fill_count_;
      reconcile_requested_ = true;
      if (unknown_fill_count_ > unknown_fill_hardstop_threshold_) {
        trip_kill_switch("ORDER_FILLED_NOT_IN_SYSTEM recurred id=" + std::to_string(id));
      }
    }
    return;
  }

  Order& o = order_ref(id);
  const InstrumentId inst = o.instrument;
  const VenueId venue = decode_order_id(id).venue;

  trade_ids_[id].insert(trade);

  // Fill accounting.
  o.filled_size += qty;
  o.total_fill_amount += static_cast<TotalFillAmount>(qty) * price;

  // Atomic position update (ordering invariant): position updates inline here,
  // before any event is published, so no subscriber sees a fill before it lands.
  positions_[inst].apply(signed_qty(o.side, qty), price, ts);
  pending_signed_[inst] -= signed_qty(o.side, qty);

  if (o.ts.first_fill == 0) o.ts.first_fill = ts;
  o.ts.last_fill = ts;

  const bool final = (o.filled_size == o.size);
  o.status = final ? OrderStatus::Filled : OrderStatus::PartiallyFilled;
  if (final) o.ts.terminal = ts;   // set before publish so subscribers see time-to-fill
  health_.on_fill(venue);

  publish(OrderEvent{final ? OrderEventKind::Fill : OrderEventKind::PartialFill,
                     id, inst, o.side, qty, price, o.filled_size, o.size, o.status,
                     RejectReason::Unknown, ts});

  if (final) {
    if (o.pending_cancel) {
      // Filled with a cancel in flight: the coming cancel-reject is benign.
      expected_false_cancel_rejects_.insert(id);
      ++false_cancel_reject_count_;
    }
    retire(id, inst, /*remaining_to_cancel=*/0, o.side);
  }
}

void OrderManager::on_reject(OrderIdRaw id, RejectReason reason, Timestamp ts) {
  const VenueId venue = decode_order_id(id).venue;

  if (reason == RejectReason::CancelRejected) {
    // Benign false cancel-reject (order already completed with cancel in flight).
    if (expected_false_cancel_rejects_.erase(id) > 0) {
      health_.on_reject(venue, /*is_false_cancel_reject=*/true);
      if (log_) log_->log(LogLevel::Info,
                          "benign false cancel-reject id=" + std::to_string(id));
      return;
    }
    // Real cancel-reject for a live order: the cancel did not apply; the order
    // remains working.
    const ValidationResult vr = validate(id, {EventKind::Reject, 0, 0, 0});
    if (vr != ValidationResult::Ok) {
      log_rejection(id, EventKind::Reject, vr, ts);
      ++oos_dropped_count_;
      return;
    }
    Order& o = order_ref(id);
    o.pending_cancel = false;
    o.status = (o.filled_size > 0) ? OrderStatus::PartiallyFilled : OrderStatus::Confirmed;
    ++reject_count_;
    health_.on_reject(venue, /*is_false_cancel_reject=*/false);
    return;
  }

  // Order (place) reject -> terminal.
  const ValidationResult vr = validate(id, {EventKind::Reject, 0, 0, 0});
  if (vr != ValidationResult::Ok) {
    log_rejection(id, EventKind::Reject, vr, ts);
    ++oos_dropped_count_;
    return;
  }

  Order& o = order_ref(id);
  const InstrumentId inst = o.instrument;
  const Quantity rem = o.remaining();
  const Side side = o.side;
  o.status = OrderStatus::Rejected;
  o.ts.terminal = ts;
  ++reject_count_;
  health_.on_reject(venue, /*is_false_cancel_reject=*/false);

  publish(OrderEvent{OrderEventKind::Rejected, id, inst, side, 0, 0,
                     o.filled_size, o.size, o.status, reason, ts});
  retire(id, inst, rem, side);
}

void OrderManager::finish_cancel(OrderIdRaw id, Timestamp ts, EventKind kind,
                                 OrderEventKind pub, bool unsolicited) {
  const ValidationResult vr = validate(id, {kind, 0, 0, 0});
  if (vr != ValidationResult::Ok) {
    log_rejection(id, kind, vr, ts);
    ++oos_dropped_count_;
    return;
  }

  Order& o = order_ref(id);
  const InstrumentId inst = o.instrument;
  const Quantity rem = o.remaining();
  const Side side = o.side;
  o.pending_cancel = false;
  o.status = OrderStatus::Cancelled;
  o.ts.terminal = ts;
  if (unsolicited) {
    ++unsolicited_cancel_count_;
    if (log_) log_->log(LogLevel::Warn, "unsolicited cancel id=" + std::to_string(id));
  }

  publish(OrderEvent{pub, id, inst, side, 0, 0, o.filled_size, o.size, o.status,
                     RejectReason::Unknown, ts});
  retire(id, inst, rem, side);
}

void OrderManager::on_cancel_ack(OrderIdRaw id, Timestamp ts) {
  finish_cancel(id, ts, EventKind::CancelAck, OrderEventKind::Cancelled, false);
}

void OrderManager::on_unsolicited_cancel(OrderIdRaw id, Timestamp ts) {
  finish_cancel(id, ts, EventKind::UnsolicitedCancel, OrderEventKind::UnsolicitedCancel, true);
}

// ---- failure-mode hooks (spec 2.11) -----------------------------------------
void OrderManager::check_ack_timeouts(Timestamp now, Timestamp timeout) {
  // Mode 1: confirmation never arrives. Scan live orders still awaiting an ack.
  std::vector<OrderIdRaw> snapshot(live_.begin(), live_.end());
  for (OrderIdRaw id : snapshot) {
    if (!pool_.is_current(id)) continue;
    Order& o = order_ref(id);
    if (o.ack_timed_out) continue;
    if (o.status != OrderStatus::Sent) continue;   // still awaiting the ack
    if (now - o.ts.sent < timeout) continue;

    o.ack_timed_out = true;
    ++ack_timeout_count_;
    const VenueId venue = decode_order_id(id).venue;
    health_.on_timeout(venue);
    if (log_) log_->log(LogLevel::Warn,
                        "ACK TIMEOUT id=" + std::to_string(id) +
                            " (possibly live; attempting cancel)");
    // Attempt a cancel but NEVER assume the order is dead: leave it live so a
    // late confirm/fill is still routed correctly.
    ExchangeClient* ex = registry_.get(venue);
    if (ex && ex->is_connected() && !o.pending_cancel) {
      if (ex->cancel_order(id, o.exchange_id) == SendResult::Ok) {
        o.pending_cancel = true;
        o.status = OrderStatus::SentCancel;
      }
    }
  }
}

void OrderManager::on_venue_disconnect(VenueId venue) {
  health_.on_disconnect(venue);
  if (log_) log_->log(LogLevel::Warn, "venue disconnect venue=" + std::to_string(venue) +
                                          "; in-flight orders kept (not assumed dead)");
}

void OrderManager::on_venue_reconnect(VenueId venue) {
  health_.on_reconnect(venue);
  // Mode 7: do not assume in-flight orders died. Request a reconcile so live
  // positions and open orders are re-fetched before we trust local state.
  reconcile_requested_ = true;
  if (log_) log_->log(LogLevel::Info, "venue reconnect venue=" + std::to_string(venue) +
                                          "; reconcile requested");
}

// ---- queries ----------------------------------------------------------------
const Position& OrderManager::position(InstrumentId inst) const {
  auto it = positions_.find(inst);
  return it == positions_.end() ? empty_position_ : it->second;
}

Quantity OrderManager::pending_quantity(InstrumentId inst) const {
  auto it = pending_signed_.find(inst);
  return it == pending_signed_.end() ? 0 : it->second;
}

const Order* OrderManager::find(OrderIdRaw id) const {
  if (!pool_.is_current(id)) return nullptr;
  return &order_ref(id);
}

std::vector<InstrumentId> OrderManager::instruments() const {
  std::vector<InstrumentId> out;
  out.reserve(positions_.size());
  for (const auto& [inst, pos] : positions_) out.push_back(inst);
  return out;
}

}  // namespace oms
