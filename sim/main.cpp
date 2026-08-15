#include <iostream>

#include "oms/OrderManager.h"   // pulls ExchangeRegistry/Health/Router/Logging
#include "SimulatedExchange.h"

// Minimal end-to-end wiring: one venue, submit an order, drive the event loop
// via poll(), and show the position update. Full scenarios/benchmarks come with
// the later spec sections.
int main() {
  using namespace oms;

  StderrLogSink log;

  auto sim = std::make_unique<SimulatedExchange>(/*venue=*/0, SimConfig{});
  SimulatedExchange* sim_raw = sim.get();

  ExchangeRegistry registry;
  registry.add(std::move(sim));

  HealthConfig hcfg;
  CircuitBreakerHealthModel health(hcfg, &log);
  health.register_venue(0);
  // Deterministic clock tied to the sim's logical time.
  health.set_clock([sim_raw] { return sim_raw->now(); });

  HealthAwareRouter router(health, registry.venues());

  OrderManager oms(registry, router, health, &log);
  oms.set_clock([sim_raw] { return sim_raw->now(); });
  sim_raw->set_event_sink(&oms);

  const OrderIdRaw id = oms.submit(OrderRequest{/*inst=*/42, Side::Buy, /*size=*/100,
                                                /*price=*/10'000, OrderType::Limit});
  std::cout << "submitted id=" << id << "\n";

  for (int i = 0; i < 10 && oms.find(id); ++i) sim_raw->poll();

  std::cout << "position(42)=" << oms.position(42).net
            << " pending(42)=" << oms.pending_quantity(42) << "\n";
  return 0;
}
