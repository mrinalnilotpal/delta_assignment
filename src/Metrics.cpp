#include "oms/Metrics.h"

#include <fstream>

#include "oms/Detail.h"

namespace oms {

using detail::bps;

double Distribution::mean() const {
  if (count_all_ == 0) return 0.0;
  return static_cast<double>(sum_) / static_cast<double>(count_all_);
}

void MetricsCollector::on_order_event(const OrderEvent& e) {
  const Order* o = om_.find(e.internal_id);
  if (o == nullptr) return;
  if (o->reconciliation) return;   // corrective orders are excluded (spec 2.10)

  switch (e.kind) {
    case OrderEventKind::Confirmed: {
      // Capture the arrival mid once, at confirm time.
      if (mds_ != nullptr && !arrival_mid_.contains(e.internal_id)) {
        arrival_mid_[e.internal_id] = mds_->top_of_book(e.instrument).mid();
      }
      ack_latency_.add(o->ts.confirmed - o->ts.sent);   // both may be 0 at t0
      break;
    }
    case OrderEventKind::Fill: {   // terminal (fully filled)
      ++filled_orders_;
      const int sign = (o->side == Side::Buy) ? 1 : -1;
      const Price avg = o->avg_fill_price();

      Price arrival = 0;
      if (auto it = arrival_mid_.find(e.internal_id); it != arrival_mid_.end()) arrival = it->second;
      if (arrival > 0) slip_arrival_.add(bps(avg, arrival, sign));

      if (auto it = ref_vwap_.find(e.instrument); it != ref_vwap_.end() && it->second > 0) {
        slip_vwap_.add(bps(avg, it->second, sign));
      }

      if (o->ts.terminal >= o->ts.created) ttf_.add(o->ts.terminal - o->ts.created);
      arrival_mid_.erase(e.internal_id);
      break;
    }
    default:
      break;
  }
}

void MetricsCollector::record_cycle(Quantity target_abs, Quantity filled_abs) {
  ++cycles_;
  if (target_abs <= 0) return;
  const int64_t rate_bps =
      static_cast<int64_t>(static_cast<double>(filled_abs) / static_cast<double>(target_abs) * 10'000.0);
  fill_rate_.add(rate_bps);
  if (filled_abs >= target_abs) ++completed_cycles_;
}

void MetricsCollector::write_summary_csv(const std::string& path) const {
  std::ofstream f(path);
  if (!f) return;
  f << "metric,p50,p99,p999,min,max,count\n";
  auto row = [&](const char* name, const Distribution& d) {
    f << name << "," << d.p50() << "," << d.p99() << "," << d.p999() << ","
      << d.min() << "," << d.max() << "," << d.count() << "\n";
  };
  row("slippage_arrival_bps", slip_arrival_);
  row("slippage_vwap_bps", slip_vwap_);
  row("time_to_fill_ns", ttf_);
  row("ack_latency_ns", ack_latency_);
  row("fill_rate_bps", fill_rate_);

  f << "\ncounter,value\n";
  f << "filled_orders," << filled_orders_ << "\n";
  f << "cycles," << cycles_ << "\n";
  f << "completed_cycles," << completed_cycles_ << "\n";
  f << "rebalance_completion_rate," << rebalance_completion_rate() << "\n";
  f << "rejections," << om_.reject_count() << "\n";
  f << "ack_timeouts," << om_.ack_timeout_count() << "\n";
  f << "unsolicited_cancels," << om_.unsolicited_cancel_count() << "\n";
  f << "out_of_sequence_dropped," << om_.out_of_sequence_dropped() << "\n";
  f << "false_cancel_rejects," << om_.false_cancel_reject_count() << "\n";
  f << "venue_demotions," << demotions_ << "\n";
  f << "venue_recoveries," << recoveries_ << "\n";
  f << "netting_saved_notional," << netting_saved_ << "\n";
}

}  // namespace oms
