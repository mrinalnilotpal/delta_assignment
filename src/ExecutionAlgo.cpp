#include "oms/ExecutionAlgo.h"

#include <algorithm>

namespace oms {

namespace {
Quantity ceil_div(Quantity a, Quantity b) {
  if (b <= 0) return a;
  return (a + b - 1) / b;
}
}  // namespace

// ---- AlgoBase ---------------------------------------------------------------
OrderIdRaw AlgoBase::send_child(Quantity qty, Price price, OrderType type) {
  if (qty <= 0 || config_.om == nullptr) return 0;
  OrderRequest req;
  req.instrument = parent_.instrument;
  req.side = parent_.side;
  req.size = qty;
  req.price = price;
  req.type = type;
  req.strategy_id = parent_.strategy_id;
  req.algo_tag = config_.algo_tag;
  req.worker_index = config_.worker_index;
  req.worker_offset = config_.worker_offset;

  const OrderIdRaw id = config_.om->submit(req);
  if (id != 0) {
    children_[id] = qty;
    sent_total_ += qty;
    ++stats_.child_orders;
  }
  return id;
}

void AlgoBase::on_fill(const Order& o, Quantity qty, Price) {
  filled_ += qty;
  stats_.filled_qty = filled_;
  auto it = children_.find(o.internal_id);
  if (it != children_.end()) {
    if (o.status == OrderStatus::Filled) children_.erase(it);
    else it->second = o.remaining();
  }
  if (filled_ >= parent_.qty) done_ = true;
}

void AlgoBase::on_reject(const Order& o, RejectReason) {
  children_.erase(o.internal_id);
}

void AlgoBase::on_cancel(const Order& o) {
  children_.erase(o.internal_id);
}

void AlgoBase::cancel() {
  if (config_.om == nullptr) return;
  std::vector<OrderIdRaw> ids;
  ids.reserve(children_.size());
  for (const auto& [id, rem] : children_) ids.push_back(id);
  for (OrderIdRaw id : ids) config_.om->cancel(id);
}

Quantity AlgoBase::inflight() const {
  Quantity sum = 0;
  for (const auto& [id, rem] : children_) sum += rem;
  return sum;
}

Quantity AlgoBase::remaining_quantity() const {
  const Quantity r = parent_.qty - filled_ - inflight();
  return r > 0 ? r : 0;
}

Price AlgoBase::cross_price() const {
  if (config_.mds != nullptr) {
    const TopOfBook b = config_.mds->top_of_book(parent_.instrument);
    const Price p = (parent_.side == Side::Buy) ? b.ask : b.bid;
    if (p > 0) return p;
  }
  return parent_.limit;
}

// ---- TWAP -------------------------------------------------------------------
void TwapAlgo::start(const ParentOrder& p) {
  parent_ = p;
  stats_.target_qty = p.qty;
  n_ = std::max(1, config_.slices);
  const Timestamp span = p.end > p.start ? (p.end - p.start) : 1;
  slice_dur_ = std::max<Timestamp>(1, span / n_);
}

void TwapAlgo::on_timer(Timestamp now) {
  if (done_) return;

  if (now >= parent_.end) {
    deadline_cross(now);
    if (inflight() == 0) {
      if (filled_ < parent_.qty) stats_.incomplete_at_deadline = true;
      done_ = true;
    }
    return;
  }

  // Fire every slice boundary that is now due.
  while (slices_done_ < n_ &&
         parent_.start + static_cast<Timestamp>(slices_done_) * slice_dur_ <= now) {
    const int k = slices_done_;
    ++slices_done_;
    const int remaining_slices = n_ - k;
    const Quantity rem = remaining_quantity();
    Quantity slice_qty =
        (remaining_slices <= 1) ? rem : ceil_div(rem, static_cast<Quantity>(remaining_slices));
    if (slice_qty > rem) slice_qty = rem;
    if (slice_qty <= 0) continue;   // skip a zero slice; never send a 0 order
    send_child(slice_qty, parent_.limit, OrderType::Limit);
  }

  if (filled_ >= parent_.qty) done_ = true;
}

void TwapAlgo::deadline_cross(Timestamp) {
  if (deadline_done_) return;
  deadline_done_ = true;
  cancel();
  const Quantity residual = parent_.qty - filled_ - inflight();
  if (residual > 0) {
    if (config_.log) {
      config_.log->log(LogLevel::Warn,
                       "TWAP deadline: crossing residual=" + std::to_string(residual));
    }
    send_child(residual, cross_price(), OrderType::IOC);
  }
}

// ---- POV --------------------------------------------------------------------
void PovAlgo::start(const ParentOrder& p) {
  parent_ = p;
  stats_.target_qty = p.qty;
  last_tick_ = p.start;
}

Quantity PovAlgo::inflight() const {
  // Own pre-degrade children plus any TWAP-managed children after degradation.
  return AlgoBase::inflight() + ((degraded_ && twap_) ? twap_->open_quantity() : 0);
}

void PovAlgo::on_fill(const Order& o, Quantity qty, Price price) {
  filled_ += qty;
  stats_.filled_qty = filled_;
  auto it = children_.find(o.internal_id);
  if (it != children_.end()) {                 // one of POV's own children
    if (o.status == OrderStatus::Filled) children_.erase(it);
    else it->second = o.remaining();
  } else if (degraded_ && twap_) {             // a TWAP-managed child
    twap_->on_fill(o, qty, price);
  }
  if (filled_ >= parent_.qty) done_ = true;
}

void PovAlgo::on_reject(const Order& o, RejectReason r) {
  if (children_.erase(o.internal_id) == 0 && degraded_ && twap_) twap_->on_reject(o, r);
}

void PovAlgo::on_cancel(const Order& o) {
  if (children_.erase(o.internal_id) == 0 && degraded_ && twap_) twap_->on_cancel(o);
}

void PovAlgo::cancel() {
  AlgoBase::cancel();
  if (degraded_ && twap_) twap_->cancel();
}

void PovAlgo::degrade_to_twap(Timestamp now) {
  degraded_ = true;
  stats_.degraded_to_twap = true;
  if (config_.log) config_.log->log(LogLevel::Warn, "POV degrading to TWAP over residual window");

  AlgoConfig tc = config_;
  tc.slices = config_.residual_twap_slices;
  twap_ = std::make_unique<TwapAlgo>(tc);

  ParentOrder resid = parent_;
  resid.qty = remaining_quantity();   // residual to work under the time constraint
  resid.start = now;
  twap_->start(resid);
}

void PovAlgo::deadline_cross(Timestamp now) {
  if (deadline_done_) return;
  deadline_done_ = true;
  if (degraded_ && twap_) {
    twap_->on_timer(now);   // let the held TWAP run its own deadline policy
    return;
  }
  cancel();
  const Quantity residual = parent_.qty - filled_;
  if (residual > 0) {
    if (config_.log) config_.log->log(LogLevel::Warn,
                                      "POV deadline: crossing residual=" + std::to_string(residual));
    send_child(residual, cross_price(), OrderType::IOC);
  }
}

void PovAlgo::on_timer(Timestamp now) {
  if (done_) return;

  if (now >= parent_.end) {
    deadline_cross(now);
    if (inflight() == 0) {
      if (filled_ < parent_.qty) stats_.incomplete_at_deadline = true;
      done_ = true;
    }
    return;
  }

  if (degraded_ && twap_) {
    twap_->on_timer(now);
    if (twap_->is_done()) done_ = true;
    return;
  }

  // Observed volume this interval.
  const Quantity vol = config_.mds ? config_.mds->volume_since(parent_.instrument, last_tick_) : 0;
  last_tick_ = now;
  market_volume_ += vol;
  stats_.market_volume = market_volume_;

  // Stage 1: below the floor, do nothing (record an idle interval).
  if (vol < config_.min_volume_floor || vol <= 0) {
    ++stats_.idle_intervals;
    return;
  }

  // Stage 2: cumulative shortfall too large -> degrade to a TWAP schedule.
  const Quantity expected =
      static_cast<Quantity>(config_.participation_rate * static_cast<double>(market_volume_));
  const Quantity shortfall = expected - filled_;
  if (config_.catch_up_threshold > 0 && shortfall > config_.catch_up_threshold) {
    degrade_to_twap(now);
    twap_->on_timer(now);
    return;
  }

  // Normal: child = rate * observed volume, capped by remaining.
  Quantity child =
      static_cast<Quantity>(config_.participation_rate * static_cast<double>(vol));
  const Quantity rem = remaining_quantity();
  if (child > rem) child = rem;
  if (child <= 0) return;
  send_child(child, parent_.limit, OrderType::Limit);
}

// ---- Factory & dispatcher ---------------------------------------------------
std::unique_ptr<ExecutionAlgo> AlgoFactory::create(const std::string& type,
                                                   const AlgoConfig& cfg) {
  if (type == "twap") return std::make_unique<TwapAlgo>(cfg);
  if (type == "pov")  return std::make_unique<PovAlgo>(cfg);
  return std::make_unique<TwapAlgo>(cfg);   // documented default
}

void AlgoDispatcher::on_order_event(const OrderEvent& e) {
  const Order* o = om_.find(e.internal_id);
  if (o == nullptr) return;
  for (const auto& r : regs_) {
    if (o->retrieve_int_data(r.offset) != r.index) continue;
    switch (e.kind) {
      case OrderEventKind::PartialFill:
      case OrderEventKind::Fill:
        r.algo->on_fill(*o, e.last_qty, e.last_price);
        break;
      case OrderEventKind::Rejected:
        r.algo->on_reject(*o, e.reason);
        break;
      case OrderEventKind::Cancelled:
      case OrderEventKind::UnsolicitedCancel:
        r.algo->on_cancel(*o);
        break;
      case OrderEventKind::Confirmed:
        break;
    }
  }
}

}  // namespace oms
