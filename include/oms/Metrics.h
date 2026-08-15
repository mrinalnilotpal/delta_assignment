#pragma once

#include <string>
#include <unordered_map>

#include "oms/OrderManager.h"   // pulls MarketData/Logging/Types
#include "oms/RingBuffer.h"

namespace oms {

// Bounded distribution: samples land in a fixed ring; percentiles are computed
// at report time by sorting a copy. With few samples p99.9 collapses onto one
// observation -- stated honestly in the report.
class Distribution {
 public:
  explicit Distribution(std::size_t capacity = 1024) : buf_(capacity) {}
  void add(int64_t v) { buf_.push(v); sum_ += v; ++count_all_; }
  std::size_t count() const { return buf_.size(); }
  int64_t p(double q) const { return buf_.percentile(q); }
  int64_t p50() const { return p(0.50); }
  int64_t p99() const { return p(0.99); }
  int64_t p999() const { return p(0.999); }
  int64_t min() const { return buf_.percentile(0.0); }
  int64_t max() const { return buf_.percentile(1.0); }
  double  mean() const;

 private:
  RingBuffer<int64_t> buf_;
  int64_t             sum_{0};        // running sum over ALL adds (for mean)
  std::size_t         count_all_{0};
};

// Execution-quality metrics (spec 2.12). Subscribes to the OMS event stream and
// records rich samples cheaply; percentiles are aggregated off the hot path at
// report time.
class MetricsCollector : public OrderEventListener {
 public:
  MetricsCollector(OrderManager& om, MarketDataSource* mds = nullptr)
      : om_(om), mds_(mds) {}

  void on_order_event(const OrderEvent&) override;

  // A per-instrument reference VWAP so we can also report VWAP-relative slippage.
  void set_reference_vwap(InstrumentId inst, Price vwap) { ref_vwap_[inst] = vwap; }

  // Per rebalance cycle: fill rate and completion.
  void record_cycle(Quantity target_abs, Quantity filled_abs);

  // Fed by the driver watching health transitions.
  void add_demotion() { ++demotions_; }
  void add_recovery() { ++recoveries_; }

  void set_netting_saved(int64_t n) { netting_saved_ = n; }

  // ---- distributions (bps for slippage; ns for latencies) ----
  const Distribution& slippage_arrival_bps() const { return slip_arrival_; }
  const Distribution& slippage_vwap_bps() const { return slip_vwap_; }
  const Distribution& time_to_fill_ns() const { return ttf_; }
  const Distribution& ack_latency_ns() const { return ack_latency_; }
  const Distribution& fill_rate_bps() const { return fill_rate_; }   // filled/target in bps

  // ---- scalars / rates ----
  uint64_t filled_orders() const { return filled_orders_; }
  uint64_t cycles() const { return cycles_; }
  uint64_t completed_cycles() const { return completed_cycles_; }
  double   rebalance_completion_rate() const {
    return cycles_ == 0 ? 0.0 : static_cast<double>(completed_cycles_) / static_cast<double>(cycles_);
  }
  uint64_t demotions() const { return demotions_; }
  uint64_t recoveries() const { return recoveries_; }
  int64_t  netting_saved() const { return netting_saved_; }

  // Write a machine-readable summary + a per-cycle CSV.
  void write_summary_csv(const std::string& path) const;

 private:
  OrderManager&     om_;
  MarketDataSource* mds_;

  std::unordered_map<OrderIdRaw, Price>    arrival_mid_;
  std::unordered_map<InstrumentId, Price>  ref_vwap_;

  Distribution slip_arrival_;
  Distribution slip_vwap_;
  Distribution ttf_;
  Distribution ack_latency_;
  Distribution fill_rate_{4096};

  uint64_t filled_orders_{0};
  uint64_t cycles_{0};
  uint64_t completed_cycles_{0};
  uint64_t demotions_{0};
  uint64_t recoveries_{0};
  int64_t  netting_saved_{0};
};

}  // namespace oms
