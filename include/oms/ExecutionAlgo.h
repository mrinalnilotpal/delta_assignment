#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "oms/OrderManager.h"   // pulls Order/Types/Logging/MarketData

namespace oms {

// A parent execution request handed to an algo.
struct ParentOrder {
  InstrumentId instrument{0};
  Side         side{Side::Buy};
  Quantity     qty{0};        // total magnitude to execute
  Timestamp    start{0};
  Timestamp    end{0};
  Price        limit{0};      // limit / max price for child orders
  StrategyId   strategy_id{0};
};

struct AlgoStats {
  Quantity target_qty{0};
  Quantity filled_qty{0};
  Quantity market_volume{0};        // POV: cumulative observed volume
  uint64_t child_orders{0};
  uint64_t idle_intervals{0};       // POV: intervals skipped under the floor
  bool     degraded_to_twap{false}; // POV: fell back to a TWAP schedule
  bool     incomplete_at_deadline{false};
};

// Everything an algo needs, injected so algos stay decoupled from transport.
struct AlgoConfig {
  OrderManager*     om{nullptr};
  MarketDataSource* mds{nullptr};
  ILogSink*         log{nullptr};

  // Self-fill routing tag (spec 2.7): stamped on each child order.
  int     worker_offset{0};
  int64_t worker_index{0};
  AlgoTag algo_tag{0};

  // TWAP.
  int slices{10};

  // POV.
  double   participation_rate{0.10};
  Quantity min_volume_floor{0};       // below this observed volume -> idle
  Quantity catch_up_threshold{0};     // shortfall beyond this -> degrade to TWAP
  int      residual_twap_slices{5};
};

// General execution-algo interface (spec 2.7). TWAP and POV both implement it;
// switching between them requires no OrderManager changes.
class ExecutionAlgo {
 public:
  virtual ~ExecutionAlgo() = default;
  virtual void start(const ParentOrder&) = 0;
  virtual void on_timer(Timestamp) = 0;
  virtual void on_fill(const Order&, Quantity fill_qty, Price fill_price) = 0;
  virtual void on_reject(const Order&, RejectReason) = 0;
  virtual void on_cancel(const Order&) = 0;
  virtual void cancel() = 0;
  virtual bool is_done() const = 0;
  virtual AlgoStats stats() const = 0;
};

// Shared child-order bookkeeping: submission, fill/terminal accounting, and the
// in-flight / remaining computation used by both algos.
class AlgoBase : public ExecutionAlgo {
 public:
  explicit AlgoBase(AlgoConfig cfg) : config_(cfg) {}

  void on_fill(const Order&, Quantity, Price) override;
  void on_reject(const Order&, RejectReason) override;
  void on_cancel(const Order&) override;
  void cancel() override;
  bool is_done() const override { return done_; }
  AlgoStats stats() const override { return stats_; }

  // Open (unfilled, in-flight) child quantity. Public so a composing algo
  // (POV holding a TWAP) can include the child's in-flight in its own.
  Quantity open_quantity() const { return inflight(); }

 protected:
  OrderIdRaw send_child(Quantity qty, Price price, OrderType type);
  virtual Quantity inflight() const;
  Quantity remaining_quantity() const;   // target - filled - in-flight
  Price    cross_price() const;          // aggressive price for a deadline IOC

  AlgoConfig                              config_;
  ParentOrder                             parent_{};
  Quantity                                filled_{0};
  Quantity                                sent_total_{0};
  std::unordered_map<OrderIdRaw, Quantity> children_;   // id -> remaining
  AlgoStats                               stats_{};
  bool                                    done_{false};
};

// TWAP: divide [start, end] into N equal slices; each slice sizes itself as
// ceil(remaining / remaining_slices) so prior partial fills are absorbed.
class TwapAlgo : public AlgoBase {
 public:
  explicit TwapAlgo(AlgoConfig cfg) : AlgoBase(cfg) {}
  void start(const ParentOrder&) override;
  void on_timer(Timestamp) override;

 private:
  void deadline_cross(Timestamp);
  Timestamp slice_dur_{1};
  int       n_{1};
  int       slices_done_{0};
  bool      deadline_done_{false};
};

// POV: each interval sizes child = rate * observed volume. Falls back through a
// three-stage policy when volume dries up; stage 2 *holds a TWAP instance*.
class PovAlgo : public AlgoBase {
 public:
  explicit PovAlgo(AlgoConfig cfg) : AlgoBase(cfg) {}
  void start(const ParentOrder&) override;
  void on_timer(Timestamp) override;
  void on_fill(const Order&, Quantity, Price) override;
  void on_reject(const Order&, RejectReason) override;
  void on_cancel(const Order&) override;
  void cancel() override;

 protected:
  Quantity inflight() const override;

 private:
  void degrade_to_twap(Timestamp now);
  void deadline_cross(Timestamp);

  Timestamp last_tick_{0};
  Quantity  market_volume_{0};
  bool      degraded_{false};
  bool      deadline_done_{false};
  std::unique_ptr<TwapAlgo> twap_;   // stage-2 fallback (shared interface payoff)
};

// Name -> constructor factory. The OrderManager contains ZERO references to any
// concrete algo type; only this factory names them.
class AlgoFactory {
 public:
  static std::unique_ptr<ExecutionAlgo> create(const std::string& type, const AlgoConfig&);
};

// Routes global order events to the owning algo by the scratch tag on the order
// (spec 2.7). Keeps the OMS free of any algo registry / ownership map.
class AlgoDispatcher : public OrderEventListener {
 public:
  explicit AlgoDispatcher(OrderManager& om) : om_(om) {}
  void register_algo(ExecutionAlgo* algo, int worker_offset, int64_t worker_index) {
    regs_.push_back({algo, worker_offset, worker_index});
  }
  void on_order_event(const OrderEvent&) override;

 private:
  struct Reg { ExecutionAlgo* algo; int offset; int64_t index; };
  OrderManager&     om_;
  std::vector<Reg>  regs_;
};

}  // namespace oms
