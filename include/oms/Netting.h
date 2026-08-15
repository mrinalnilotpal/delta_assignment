#pragma once

#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "oms/Logging.h"
#include "oms/Types.h"

namespace oms {

// Aggregates per-strategy deltas per cycle into one net order per instrument
// (spec 2.9) so opposing signals don't pay slippage twice. Invariant (asserted):
// global_position == Σ sub-positions. Fills attribute pro-rata by contribution,
// with the rounding residual going to the largest contributor.
class NettingEngine {
 public:
  explicit NettingEngine(ILogSink* log = nullptr) : log_(log) {}

  struct NetOrder {
    InstrumentId instrument{0};
    Quantity     delta{0};   // signed net quantity to trade
  };

  // Set a strategy's desired target for this cycle.
  void set_target(StrategyId s, InstrumentId inst, Quantity target) {
    targets_[key(s, inst)] = target;
    instruments_.insert(inst);
    strategies_.insert(s);
  }

  // Seed / inspect attributed sub-positions. Keeps global == Σ sub consistent.
  void set_attributed(StrategyId s, InstrumentId inst, Quantity q) {
    const Quantity old = attributed(s, inst);
    attributed_[key(s, inst)] = q;
    global_[inst] += (q - old);
    instruments_.insert(inst);
    strategies_.insert(s);
  }
  Quantity attributed(StrategyId s, InstrumentId inst) const {
    auto it = attributed_.find(key(s, inst));
    return it == attributed_.end() ? 0 : it->second;
  }
  Quantity global_position(InstrumentId inst) const {
    auto it = global_.find(inst);
    return it == global_.end() ? 0 : it->second;
  }

  // Collapse this cycle's targets into net orders. Zero-net instruments emit no
  // order but still update attributed positions (internal transfer book entry)
  // and accumulate saved notional.
  std::vector<NetOrder> compute_cycle(const std::unordered_map<InstrumentId, Price>& prices);

  // Attribute a fill on a net order back to the contributing strategies.
  void attribute_fill(InstrumentId inst, Quantity signed_qty, Price fill_price);

  bool    check_invariant() const;
  int64_t netting_saved_notional() const { return saved_notional_; }

 private:
  static uint64_t key(StrategyId s, InstrumentId i) {
    return (static_cast<uint64_t>(s) << 16) | static_cast<uint64_t>(i);
  }

  ILogSink* log_;
  std::unordered_map<uint64_t, Quantity> targets_;
  std::unordered_map<uint64_t, Quantity> attributed_;
  std::unordered_map<InstrumentId, Quantity> global_;
  std::unordered_map<InstrumentId, std::vector<std::pair<StrategyId, Quantity>>> contrib_;
  std::unordered_set<InstrumentId> instruments_;
  std::unordered_set<StrategyId>   strategies_;
  int64_t saved_notional_{0};
};

}  // namespace oms
