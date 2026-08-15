// Benchmark / illustrative run (spec 2.12). Multi-cycle rebalance driven by TWAP
// and POV against the seeded SimulatedExchange with injected failures. Emits
// results/{metrics_summary.csv, cycles.csv, REPORT.md}.
// Run from the repo root:  ./build/sim/oms_bench

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "oms/ExecutionAlgo.h"    // each pulls OrderManager -> the rest
#include "oms/Metrics.h"
#include "oms/Netting.h"
#include "oms/Reconciliation.h"
#include "SimulatedExchange.h"

using namespace oms;

namespace {

// Static book + fixed per-interval volume; enough to exercise POV and to give a
// stable arrival mid for slippage. (A moving market would make arrival- and
// VWAP-relative slippage diverge; here they coincide, which we say in the report.)
class StaticMarketData : public MarketDataSource {
 public:
  std::unordered_map<InstrumentId, TopOfBook> books;
  Quantity vol_per_interval{0};
  TopOfBook top_of_book(InstrumentId i) const override {
    auto it = books.find(i);
    return it == books.end() ? TopOfBook{} : it->second;
  }
  Quantity volume_since(InstrumentId, Timestamp) const override { return vol_per_interval; }
};

struct Rig {
  CapturingLogSink          log;
  ExchangeRegistry          registry;
  CircuitBreakerHealthModel health;
  SimulatedExchange*        sim{nullptr};
  std::unique_ptr<HealthAwareRouter> router;
  std::unique_ptr<OrderManager>      om;
  StaticMarketData          mds;
  std::unique_ptr<AlgoDispatcher>    dispatcher;
  std::unique_ptr<MetricsCollector>  metrics;

  explicit Rig(SimConfig cfg) {
    auto s = std::make_unique<SimulatedExchange>(static_cast<VenueId>(0), cfg);
    sim = s.get();
    registry.add(std::move(s));
    health.register_venue(0);
    health.set_clock([this] { return sim->now(); });
    router = std::make_unique<HealthAwareRouter>(health, registry.venues());
    om = std::make_unique<OrderManager>(registry, *router, health, &log);
    om->set_clock([this] { return sim->now(); });
    sim->set_event_sink(om.get());
    dispatcher = std::make_unique<AlgoDispatcher>(*om);
    metrics = std::make_unique<MetricsCollector>(*om, &mds);
    om->subscribe(metrics.get());     // metrics first: it only reads
    om->subscribe(dispatcher.get());  // then route to algos
  }

  void poll_n(int n) { for (int i = 0; i < n; ++i) sim->poll(); }
};

int64_t next_worker = 1;

// Work a single parent order to completion (or its deadline) with the given algo.
Quantity run_parent(Rig& rig, const std::string& algo_type, InstrumentId inst, Side side,
                    Quantity qty, Timestamp start, Timestamp window, int n_intervals,
                    std::vector<std::unique_ptr<ExecutionAlgo>>& keep) {
  const Quantity before = rig.om->position(inst).net;

  AlgoConfig ac;
  ac.om = rig.om.get();
  ac.mds = &rig.mds;
  ac.log = &rig.log;
  ac.worker_offset = 0;
  ac.worker_index = next_worker++;
  ac.algo_tag = 1;
  ac.slices = n_intervals;
  ac.participation_rate = 0.20;
  ac.min_volume_floor = 1;
  ac.catch_up_threshold = qty;   // large -> POV stays in participation mode here
  ac.residual_twap_slices = n_intervals;

  auto algo = AlgoFactory::create(algo_type, ac);
  rig.dispatcher->register_algo(algo.get(), ac.worker_offset, ac.worker_index);

  const TopOfBook b = rig.mds.top_of_book(inst);
  ParentOrder p;
  p.instrument = inst;
  p.side = side;
  p.qty = qty;
  p.start = start;
  p.end = start + window;
  p.limit = (side == Side::Buy) ? b.ask : b.bid;   // cross the spread -> fills
  algo->start(p);

  const Timestamp step = window / n_intervals;
  for (int i = 0; i <= n_intervals; ++i) {
    algo->on_timer(start + static_cast<Timestamp>(i) * step);
    rig.poll_n(6);
  }
  // Deadline sweep + drain.
  algo->on_timer(p.end + step);
  rig.poll_n(8);

  keep.push_back(std::move(algo));
  return rig.om->position(inst).net - before;
}

std::string fmt_rate(double r) {
  std::ostringstream os;
  os << std::fixed << std::setprecision(3) << r;
  return os.str();
}

}  // namespace

int main() {
  namespace fs = std::filesystem;
  fs::create_directories("results");

  SimConfig cfg;
  cfg.seed = 42;
  cfg.partial_chunks = 3;              // partial fills to exercise absorption
  cfg.reject_prob = 0.05;             // occasional venue reject
  cfg.duplicate_fill_prob = 0.05;     // duplicate deliveries (must be de-duped)
  cfg.unsolicited_cancel_prob = 0.03; // occasional unsolicited cancel
  Rig rig(cfg);

  // Four instruments; a static book with a 2-tick spread around a 10000 mid.
  const std::vector<InstrumentId> insts = {10, 11, 12, 13};
  for (InstrumentId i : insts) {
    TopOfBook b;
    b.bid = 9'999; b.ask = 10'001; b.bid_size = 1'000; b.ask_size = 1'000; b.ts = 0;
    rig.mds.books[i] = b;
    rig.metrics->set_reference_vwap(i, b.mid());   // static book: VWAP ~= arrival mid
  }
  rig.mds.vol_per_interval = 200;

  // Netting illustration: two strategies with opposing deltas on instrument 10
  // net to a smaller order; record the notional saved.
  {
    NettingEngine ne(&rig.log);
    ne.set_target(1, 10, 250);    // +250
    ne.set_target(2, 10, -250);   // -250 -> net 0: nothing traded, notional saved
    ne.compute_cycle({{10, 10'000}});
    rig.metrics->set_netting_saved(ne.netting_saved_notional());
  }

  PeriodicReconciler reconciler(*rig.om, rig.registry,
                                PeriodicReconConfig{/*threshold=*/0, /*band=*/50, /*persist=*/2},
                                &rig.log);

  // Targets per cycle: alternate direction so we both build and unwind. TWAP for
  // instruments 10/11, POV for 12/13.
  const int cycles = 5;
  Timestamp t = 0;
  const Timestamp window = 20'000'000;   // 20ms logical window per parent

  std::ofstream cyc("results/cycles.csv");
  cyc << "cycle,target_abs,filled_abs,fill_rate,completed\n";

  uint64_t trips_before = rig.health.trips(0);

  for (int c = 0; c < cycles; ++c) {
    // A rotating set of targets (units).
    std::unordered_map<InstrumentId, Quantity> targets;
    targets[10] = (c % 2 == 0) ? 120 : 0;
    targets[11] = (c % 2 == 0) ? 90 : 30;
    targets[12] = (c % 2 == 0) ? 150 : 50;
    targets[13] = (c % 2 == 0) ? 60 : 100;

    Quantity cycle_target_abs = 0;
    Quantity cycle_filled_abs = 0;
    std::vector<std::unique_ptr<ExecutionAlgo>> keep;

    for (InstrumentId inst : insts) {
      const Quantity delta = rig.om->execution_delta(inst, targets[inst]);
      if (delta == 0) continue;
      const Side side = delta > 0 ? Side::Buy : Side::Sell;
      const Quantity qty = delta > 0 ? delta : -delta;
      cycle_target_abs += qty;

      const std::string algo_type = (inst <= 11) ? "twap" : "pov";
      const Quantity moved = run_parent(rig, algo_type, inst, side, qty, t, window, 8, keep);
      cycle_filled_abs += (moved < 0 ? -moved : moved);
    }

    rig.metrics->record_cycle(cycle_target_abs, cycle_filled_abs);

    // Steady-state reconcile runs on its own timer between cycles, independent of
    // the execution path above.
    reconciler.reconcile(rig.sim->now());

    const double rate = cycle_target_abs == 0 ? 1.0
                        : static_cast<double>(cycle_filled_abs) / static_cast<double>(cycle_target_abs);
    cyc << c << "," << cycle_target_abs << "," << cycle_filled_abs << "," << fmt_rate(rate)
        << "," << (cycle_filled_abs >= cycle_target_abs ? 1 : 0) << "\n";

    t += window * 2;
  }
  cyc.close();

  const uint64_t demotions = rig.health.trips(0) - trips_before;
  const uint64_t unresolved = rig.health.unresolved(0);
  const uint64_t recoveries = demotions > unresolved ? demotions - unresolved : 0;
  for (uint64_t i = 0; i < demotions; ++i) rig.metrics->add_demotion();
  for (uint64_t i = 0; i < recoveries; ++i) rig.metrics->add_recovery();

  rig.metrics->write_summary_csv("results/metrics_summary.csv");

  // ---- REPORT.md (interpretation, not just a table) ----
  const auto& slipA = rig.metrics->slippage_arrival_bps();
  const auto& ttf = rig.metrics->time_to_fill_ns();
  const auto& ackl = rig.metrics->ack_latency_ns();

  std::ofstream rep("results/REPORT.md");
  rep << "# Execution Metrics Report\n\n";
  rep << "Deterministic run: seed " << cfg.seed << ", " << cycles
      << " rebalance cycles across " << insts.size()
      << " instruments, TWAP on {10,11} and POV on {12,13}, with injected partial "
         "fills, rejects, duplicate fills, and unsolicited cancels. All numbers are "
         "reproducible.\n\n";

  rep << "## Headline numbers\n\n";
  rep << "| Metric | p50 | p99 | p99.9 | count |\n|---|---|---|---|---|\n";
  auto line = [&](const char* name, const Distribution& d) {
    rep << "| " << name << " | " << d.p50() << " | " << d.p99() << " | " << d.p999()
        << " | " << d.count() << " |\n";
  };
  line("Slippage arrival (bps)", slipA);
  line("Slippage VWAP (bps)", rig.metrics->slippage_vwap_bps());
  line("Time to fill (ns)", ttf);
  line("Ack latency (ns)", ackl);
  line("Fill rate (bps of target)", rig.metrics->fill_rate_bps());
  rep << "\n";

  rep << "## Counters\n\n";
  rep << "- Filled orders: " << rig.metrics->filled_orders() << "\n";
  rep << "- Rebalance completion rate: "
      << fmt_rate(rig.metrics->rebalance_completion_rate()) << " ("
      << rig.metrics->completed_cycles() << "/" << rig.metrics->cycles() << ")\n";
  rep << "- Rejections: " << rig.om->reject_count() << "\n";
  rep << "- Ack timeouts: " << rig.om->ack_timeout_count() << "\n";
  rep << "- Unsolicited cancels: " << rig.om->unsolicited_cancel_count() << "\n";
  rep << "- Out-of-sequence events dropped: " << rig.om->out_of_sequence_dropped() << "\n";
  rep << "- False cancel-rejects (excluded from health): "
      << rig.om->false_cancel_reject_count() << "\n";
  rep << "- Venue demotions: " << rig.metrics->demotions() << "\n";
  rep << "- Netting saved notional: " << rig.metrics->netting_saved() << "\n\n";

  rep << "## Interpretation\n\n";
  rep << "- **Slippage reference.** We report slippage against the **arrival mid** "
         "(mid at the instant the OMS accepted the parent), because that captures the "
         "full cost of the delay the execution layer is responsible for. We also report "
         "VWAP-relative slippage, which isolates algo skill from market drift. In this "
         "run the book is static, so the two coincide; in a moving market arrival-relative "
         "would exceed VWAP-relative whenever the market trended against us during the "
         "window. Reporting only one hides where the cost came from.\n";
  rep << "- **What p99 tells you.** p50 is the typical child order; p99 is the tail that "
         "sets risk limits and SLAs. Here the ack-latency p99 of " << ackl.p99()
      << "ns reflects the sim's fixed ack latency; a real venue's p99 blowing out is the "
         "early warning of the reject/latency circuit breaker about to trip.\n";
  rep << "- **Why POV's fill rate differs from TWAP's.** TWAP sizes off the clock and will "
         "cross at the deadline, so it completes the target whenever liquidity is present. "
         "POV sizes off observed volume and deliberately *idles* below the volume floor, so "
         "when volume is thin POV under-fills within the window and only its deadline IOC "
         "closes the gap. POV trades completion certainty for footprint control.\n";
  rep << "- **What the demotion count implies.** " << rig.metrics->demotions()
      << " demotion(s): with a single venue, any demotion means the all-down policy engaged "
         "and a cycle was marked incomplete. With multiple venues it would instead show "
         "routing shifting away from the sick venue.\n";
  rep << "- **What I would alert on in production.** Rebalance completion rate below target "
         "and a rising reject/ack-latency p99 — those lead the circuit breaker. Raw fill "
         "counts are lagging indicators.\n";
  rep << "- **Honest limitation.** Distributions use a fixed-size ring reservoir (last N "
         "samples), so with few observations p99.9 collapses onto a single sample and is not "
         "statistically meaningful here; it is reported for shape, not precision.\n";
  rep.close();

  return 0;
}
