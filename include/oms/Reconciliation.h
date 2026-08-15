#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "oms/OrderManager.h"   // pulls ExchangeRegistry/Logging/Types

namespace oms {

// Startup reconciliation (spec 2.10). Source of truth: persisted local file
// seeds positions, exchange is authoritative for executed reality. Drift beyond
// the auto-heal band is a HARD STOP (a human is present at boot).
enum class ReconAction : uint8_t {
  Proceed,          // local == exchange
  AdoptColdStart,   // no persisted state: adopt exchange, log loudly
  AdoptAutoHeal,    // small drift: adopt exchange, log, emit metric
  HardStop,         // drift too large OR exchange unreachable
};

struct ReconLine {
  InstrumentId instrument{0};
  Quantity     local{0};
  Quantity     exchange{0};
  ReconAction  action{ReconAction::Proceed};
  std::string  why;
};

struct StartupReport {
  bool                                        can_start{false};   // false => stay DNT / hard stop
  std::vector<ReconLine>                      lines;
  std::unordered_map<InstrumentId, Quantity>  adopted;            // seed positions if can_start
};

struct StartupReconConfig {
  Quantity startup_auto_heal_threshold{0};
};

class StartupReconciler {
 public:
  explicit StartupReconciler(StartupReconConfig cfg = {}, ILogSink* log = nullptr)
      : cfg_(cfg), log_(log) {}

  StartupReport reconcile(const std::unordered_map<InstrumentId, Quantity>& local,
                          bool has_persisted_state,
                          bool exchange_reachable,
                          const std::unordered_map<InstrumentId, Quantity>& exchange) const;

 private:
  StartupReconConfig cfg_;
  ILogSink*          log_;
};

// Periodic (steady-state) reconciliation (spec 2.10 / brief §7). Runs on its own
// timer, independent of the execution path: it reads positions and (within the
// band) submits `reconciliation`-tagged corrective orders.
enum class DriftAction : uint8_t {
  InSync,          // |drift| <= reconcile_threshold: ignored, logged as resolved
  PendingConfirm,  // drift seen but not yet persisted across enough cycles
  AutoHealed,      // within band: corrective order emitted
  Halted,          // beyond band: sticky HALT, manual re-arm required
};

struct DriftRecord {
  InstrumentId inst{0};
  Quantity     exchange{0};
  Quantity     oms{0};       // filled-only OMS position (in-flight excluded)
  Quantity     drift{0};
  DriftAction  action{DriftAction::InSync};
  Timestamp    ts{0};
};

struct PeriodicReconConfig {
  Quantity reconcile_threshold{0};   // |drift| <= this: ignore
  Quantity auto_heal_band{0};        // |drift| <= this (and > threshold): correct
  int      persist_cycles{2};        // drift must persist this many cycles before acting
};

class PeriodicReconciler {
 public:
  PeriodicReconciler(OrderManager& om, ExchangeRegistry& registry,
                     PeriodicReconConfig cfg = {}, ILogSink* log = nullptr)
      : om_(om), registry_(registry), cfg_(cfg), log_(log) {}

  // One reconcile cycle. Returns the per-instrument drift records.
  std::vector<DriftRecord> reconcile(Timestamp now);

  bool     halted() const { return halted_; }
  void     rearm() { halted_ = false; }        // manual, sticky-halt re-arm entry point
  uint64_t corrective_orders() const { return corrective_orders_; }
  uint64_t halts() const { return halts_; }

 private:
  OrderManager&       om_;
  ExchangeRegistry&   registry_;
  PeriodicReconConfig cfg_;
  ILogSink*           log_;
  bool                halted_{false};
  uint64_t            corrective_orders_{0};
  uint64_t            halts_{0};
  std::unordered_map<InstrumentId, int> persist_;
};

}  // namespace oms
