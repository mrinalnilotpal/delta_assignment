#include "SimulatedExchange.h"

#include <memory>

namespace oms {

SimulatedExchange::SimulatedExchange(VenueId venue, SimConfig cfg)
    : venue_(venue), cfg_(cfg), rng_(cfg.seed), connected_(cfg.start_connected) {}

void SimulatedExchange::schedule(SimEvent ev) {
  ev.seq = seq_counter_++;
  queue_.push(ev);
}

void SimulatedExchange::set_book(InstrumentId inst, const TopOfBook& b) {
  books_[inst] = b;
}

TopOfBook SimulatedExchange::get_order_book(InstrumentId inst) {
  auto it = books_.find(inst);
  return it == books_.end() ? TopOfBook{} : it->second;
}

SendResult SimulatedExchange::place_order(const Order& o) {
  if (!connected_) return SendResult::TransportDown;

  Working w;
  w.instrument = o.instrument;
  w.side = o.side;
  w.size = o.size;
  w.remaining = o.size;
  w.price = o.limit_price;
  w.exchange_id = "SIM-" + std::to_string(++exch_counter_);
  working_[o.internal_id] = std::move(w);

  std::bernoulli_distribution reject_d(cfg_.reject_prob);
  std::bernoulli_distribution unsol_d(cfg_.unsolicited_cancel_prob);
  std::bernoulli_distribution dup_d(cfg_.duplicate_fill_prob);

  // Venue rejects asynchronously.
  if (reject_d(rng_)) {
    schedule(SimEvent{now_ + cfg_.ack_latency_ns, 0, EvType::Reject, o.internal_id,
                      0, 0, 0, false, RejectReason::RiskLimit});
    return SendResult::Ok;
  }

  const Timestamp confirm_at = now_ + cfg_.ack_latency_ns;
  schedule(SimEvent{confirm_at, 0, EvType::Confirm, o.internal_id, 0, 0, 0, false,
                    RejectReason::Unknown});

  // Unsolicited cancel instead of fills.
  if (unsol_d(rng_)) {
    schedule(SimEvent{confirm_at + cfg_.fill_latency_ns, 0, EvType::Unsolicited,
                      o.internal_id, 0, 0, 0, false, RejectReason::Unknown});
    return SendResult::Ok;
  }

  // Partial fills that sum exactly to size.
  const int chunks = cfg_.partial_chunks < 1 ? 1 : cfg_.partial_chunks;
  const Quantity base = w.size / chunks;
  Quantity assigned = 0;
  for (int i = 0; i < chunks; ++i) {
    const bool last = (i == chunks - 1);
    const Quantity q = last ? (w.size - assigned) : base;
    assigned += q;
    if (q <= 0) continue;

    Timestamp at = now_ + cfg_.fill_latency_ns + static_cast<Timestamp>(i) * cfg_.fill_gap_ns;
    if (cfg_.reorder_fill_before_ack && i == 0) {
      at = confirm_at - 1;   // deliver first fill before the confirm
    }
    const TradeId trade = ++trade_counter_;
    schedule(SimEvent{at, 0, EvType::Fill, o.internal_id, q, w.price, trade, last,
                      RejectReason::Unknown});

    if (dup_d(rng_)) {
      // Duplicate delivery of the same trade id: the OMS must de-dup it.
      schedule(SimEvent{at + 1, 0, EvType::Fill, o.internal_id, q, w.price, trade, last,
                        RejectReason::Unknown});
    }
  }
  return SendResult::Ok;
}

SendResult SimulatedExchange::cancel_order(OrderIdRaw id, const ExchangeOrderId&) {
  if (!connected_) return SendResult::TransportDown;
  auto it = working_.find(id);
  if (it == working_.end() || !it->second.alive) {
    // Nothing to cancel (already gone): the venue will cancel-reject.
    schedule(SimEvent{now_ + cfg_.ack_latency_ns, 0, EvType::CancelReject, id, 0, 0, 0,
                      false, RejectReason::CancelRejected});
    return SendResult::Ok;
  }
  schedule(SimEvent{now_ + cfg_.ack_latency_ns, 0, EvType::CancelAck, id, 0, 0, 0, false,
                    RejectReason::Unknown});
  return SendResult::Ok;
}

SendResult SimulatedExchange::mass_cancel(InstrumentId inst) {
  if (!connected_) return SendResult::TransportDown;
  for (auto& [id, w] : working_) {
    if (w.alive && w.instrument == inst) {
      schedule(SimEvent{now_ + cfg_.ack_latency_ns, 0, EvType::CancelAck, id, 0, 0, 0,
                        false, RejectReason::Unknown});
    }
  }
  return SendResult::Ok;
}

void SimulatedExchange::deliver(const SimEvent& ev) {
  if (!sink_) return;
  auto it = working_.find(ev.id);

  switch (ev.type) {
    case EvType::Confirm:
      sink_->on_confirm(ev.id, it != working_.end() ? it->second.exchange_id : "", ev.at);
      break;
    case EvType::Fill: {
      // Update the sim's own book only for the first sighting of a trade id, so
      // injected duplicates do not corrupt the sim position or remaining size.
      if (it != working_.end()) {
        Working& w = it->second;
        const bool dup = !w.seen_trades.insert(ev.trade).second;
        if (!dup && w.alive) {
          w.remaining -= ev.qty;
          positions_[w.instrument] += signed_qty(w.side, ev.qty);
          if (ev.final || w.remaining <= 0) w.alive = false;
        }
      }
      // Always deliver to the sink (including duplicates) so the OMS de-dup and
      // out-of-sequence guards are actually exercised.
      sink_->on_fill(ev.id, ev.qty, ev.price, ev.trade, ev.at);
      break;
    }
    case EvType::Reject:
      if (it != working_.end()) it->second.alive = false;
      sink_->on_reject(ev.id, ev.reason, ev.at);
      break;
    case EvType::CancelAck:
      if (it != working_.end()) it->second.alive = false;
      sink_->on_cancel_ack(ev.id, ev.at);
      break;
    case EvType::CancelReject:
      sink_->on_reject(ev.id, RejectReason::CancelRejected, ev.at);
      break;
    case EvType::Unsolicited:
      if (it != working_.end()) it->second.alive = false;
      sink_->on_unsolicited_cancel(ev.id, ev.at);
      break;
  }
}

void SimulatedExchange::poll() {
  now_ += cfg_.step_ns;
  while (!queue_.empty() && queue_.top().at <= now_) {
    const SimEvent ev = queue_.top();
    queue_.pop();
    deliver(ev);
  }
}

// ---- injectors: delivered on the next poll() --------------------------------
void SimulatedExchange::inject_confirm(OrderIdRaw id) {
  schedule(SimEvent{now_ + 1, 0, EvType::Confirm, id, 0, 0, 0, false, RejectReason::Unknown});
}
void SimulatedExchange::inject_fill(OrderIdRaw id, Quantity q, Price p, TradeId t, bool final) {
  schedule(SimEvent{now_ + 1, 0, EvType::Fill, id, q, p, t, final, RejectReason::Unknown});
}
void SimulatedExchange::inject_reject(OrderIdRaw id, RejectReason r) {
  schedule(SimEvent{now_ + 1, 0, EvType::Reject, id, 0, 0, 0, false, r});
}
void SimulatedExchange::inject_cancel_ack(OrderIdRaw id) {
  schedule(SimEvent{now_ + 1, 0, EvType::CancelAck, id, 0, 0, 0, false, RejectReason::Unknown});
}
void SimulatedExchange::inject_cancel_reject(OrderIdRaw id) {
  schedule(SimEvent{now_ + 1, 0, EvType::CancelReject, id, 0, 0, 0, false,
                    RejectReason::CancelRejected});
}
void SimulatedExchange::inject_unsolicited_cancel(OrderIdRaw id) {
  schedule(SimEvent{now_ + 1, 0, EvType::Unsolicited, id, 0, 0, 0, false, RejectReason::Unknown});
}

void register_simulated_exchange_factory() {
  ExchangeRegistry::register_factory(
      "sim", [](const ExchangeConfig& cfg) -> std::unique_ptr<ExchangeClient> {
        SimConfig sc;
        sc.seed = cfg.seed;
        return std::make_unique<SimulatedExchange>(cfg.venue_id, sc);
      });
}

}  // namespace oms
