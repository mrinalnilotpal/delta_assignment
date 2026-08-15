#include "oms/Reconciliation.h"

#include <algorithm>
#include <set>
#include <string>

namespace oms {

StartupReport StartupReconciler::reconcile(
    const std::unordered_map<InstrumentId, Quantity>& local,
    bool has_persisted_state,
    bool exchange_reachable,
    const std::unordered_map<InstrumentId, Quantity>& exchange) const {
  StartupReport report;

  // Exchange unreachable => cannot verify => HARD STOP. Trading blind is worse
  // than not trading.
  if (!exchange_reachable) {
    if (log_) log_->log(LogLevel::Error, "startup recon: exchange unreachable -> HARD STOP");
    report.can_start = false;
    report.lines.push_back({0, 0, 0, ReconAction::HardStop, "exchange unreachable"});
    return report;
  }

  std::set<InstrumentId> insts;
  for (const auto& [i, q] : local) insts.insert(i);
  for (const auto& [i, q] : exchange) insts.insert(i);

  bool hard_stop = false;
  for (InstrumentId inst : insts) {
    Quantity l = 0, e = 0;
    if (auto it = local.find(inst); it != local.end()) l = it->second;
    if (auto it = exchange.find(inst); it != exchange.end()) e = it->second;

    ReconLine line{inst, l, e, ReconAction::Proceed, {}};

    if (!has_persisted_state) {
      line.action = ReconAction::AdoptColdStart;
      line.why = "cold start: exchange is the only truth available";
      report.adopted[inst] = e;
      if (log_) log_->log(LogLevel::Warn, "startup recon COLD START inst=" +
                                              std::to_string(inst) + " adopt exchange=" +
                                              std::to_string(e));
    } else if (l == e) {
      line.action = ReconAction::Proceed;
      line.why = "match";
      report.adopted[inst] = l;
    } else {
      const Quantity drift = (l - e) < 0 ? (e - l) : (l - e);
      if (drift <= cfg_.startup_auto_heal_threshold) {
        line.action = ReconAction::AdoptAutoHeal;
        line.why = "drift<=threshold: missed fill while down; exchange authoritative";
        report.adopted[inst] = e;
        if (log_) log_->log(LogLevel::Warn, "startup recon AUTO-HEAL inst=" +
                                                std::to_string(inst) + " local=" +
                                                std::to_string(l) + " exch=" +
                                                std::to_string(e) + " drift=" +
                                                std::to_string(drift));
      } else {
        line.action = ReconAction::HardStop;
        line.why = "drift>threshold: cannot distinguish missed fill from corruption";
        hard_stop = true;
        if (log_) log_->log(LogLevel::Error, "startup recon HARD STOP inst=" +
                                                 std::to_string(inst) + " local=" +
                                                 std::to_string(l) + " exch=" +
                                                 std::to_string(e) + " drift=" +
                                                 std::to_string(drift));
      }
    }
    report.lines.push_back(line);
  }

  report.can_start = !hard_stop;
  if (hard_stop) report.adopted.clear();
  return report;
}

// ---- Periodic reconciliation ------------------------------------------------
std::vector<DriftRecord> PeriodicReconciler::reconcile(Timestamp now) {
  std::vector<DriftRecord> out;
  if (halted_) return out;   // sticky: stays halted until rearm()

  // Snapshot exchange positions across all venues (may be slow; this runs on the
  // reconcile timer, NOT the execution path).
  std::unordered_map<InstrumentId, Quantity> ex_pos;
  for (VenueId v : registry_.venues()) {
    ExchangeClient* ex = registry_.get(v);
    if (ex == nullptr || !ex->is_connected()) continue;
    for (const auto& [inst, q] : ex->get_positions()) ex_pos[inst] += q;
  }

  // Union of instruments the exchange reports and the OMS tracks.
  std::set<InstrumentId> insts;
  for (const auto& [inst, q] : ex_pos) insts.insert(inst);
  for (InstrumentId inst : om_.instruments()) insts.insert(inst);

  for (InstrumentId inst : insts) {
    // Compare exchange position against the OMS FILLED-ONLY position; in-flight
    // quantity lives in pending_quantity and is deliberately excluded, or every
    // open order would look like drift.
    const Quantity oms_pos = om_.position(inst).net;
    Quantity ex = 0;
    if (auto it = ex_pos.find(inst); it != ex_pos.end()) ex = it->second;
    const Quantity drift = ex - oms_pos;
    const Quantity adrift = drift < 0 ? -drift : drift;

    DriftRecord rec{inst, ex, oms_pos, drift, DriftAction::InSync, now};

    if (adrift <= cfg_.reconcile_threshold) {
      persist_[inst] = 0;
      rec.action = DriftAction::InSync;
      if (log_ && adrift == 0) log_->log(LogLevel::Info,
                                         "[[RESOLVED]] reconcile inst=" + std::to_string(inst));
      out.push_back(rec);
      continue;
    }

    // Require the drift to persist across consecutive cycles before acting, so a
    // transient snapshot skew (a fill in flight during the read) is not chased.
    const int c = ++persist_[inst];
    if (c < cfg_.persist_cycles) {
      rec.action = DriftAction::PendingConfirm;
      if (log_) log_->log(LogLevel::Warn,
                          "reconcile drift pending confirm inst=" + std::to_string(inst) +
                              " exch=" + std::to_string(ex) + " oms=" + std::to_string(oms_pos) +
                              " drift=" + std::to_string(drift) + " cycle=" + std::to_string(c));
      out.push_back(rec);
      continue;
    }
    persist_[inst] = 0;

    if (adrift <= cfg_.auto_heal_band) {
      // Emit a corrective order tagged reconciliation (excluded from metrics /
      // netting) to move the OMS position toward exchange truth.
      OrderRequest req;
      req.instrument = inst;
      req.side = drift > 0 ? Side::Buy : Side::Sell;
      req.size = adrift;
      req.type = OrderType::IOC;
      req.reconciliation = true;
      if (ExchangeClient* ex_c = registry_.get(0)) req.price = ex_c->get_order_book(inst).mid();
      if (req.price <= 0) req.price = 1;
      om_.submit(req);
      ++corrective_orders_;
      rec.action = DriftAction::AutoHealed;
      if (log_) log_->log(LogLevel::Warn,
                          "reconcile AUTO-HEAL inst=" + std::to_string(inst) +
                              " exch=" + std::to_string(ex) + " oms=" + std::to_string(oms_pos) +
                              " corrective=" + std::to_string(signed_qty(req.side, req.size)));
      out.push_back(rec);
    } else {
      // Beyond the band: sticky HALT, alert, require manual re-arm.
      halted_ = true;
      ++halts_;
      rec.action = DriftAction::Halted;
      if (log_) log_->log(LogLevel::Error,
                          "reconcile HALT inst=" + std::to_string(inst) +
                              " exch=" + std::to_string(ex) + " oms=" + std::to_string(oms_pos) +
                              " drift=" + std::to_string(drift) + " (manual re-arm required)");
      out.push_back(rec);
      return out;   // stop this cycle; system is halted
    }
  }
  return out;
}

}  // namespace oms
