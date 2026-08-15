#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "oms/OrderManager.h"   // pulls ExchangeRegistry/Health/Router/Logging
#include "SimulatedExchange.h"

namespace oms::test {

// Health model with directly-settable outputs, for deterministic router tests.
class StubHealthModel : public HealthModel {
 public:
  std::unordered_map<VenueId, bool>   tradeable;
  std::unordered_map<VenueId, double> weight;

  void on_order_sent(VenueId, Timestamp) override {}
  void on_ack(VenueId, LatencyNs) override {}
  void on_reject(VenueId, bool) override {}
  void on_fill(VenueId) override {}
  void on_timeout(VenueId) override {}
  void on_disconnect(VenueId) override {}
  void on_reconnect(VenueId) override {}

  HealthScore score(VenueId v) const override {
    HealthScore s;
    auto it = weight.find(v);
    s.weight = (it == weight.end()) ? 1.0 : it->second;
    return s;
  }
  bool is_tradeable(VenueId v) const override {
    auto it = tradeable.find(v);
    return it == tradeable.end() ? true : it->second;
  }
};

// Captures inbound events as strings so tests can assert on order/determinism.
class RecordingSink : public ExchangeEventSink {
 public:
  std::vector<std::string> events;
  void on_confirm(OrderIdRaw id, const ExchangeOrderId&, Timestamp) override {
    events.push_back("confirm " + std::to_string(id));
  }
  void on_fill(OrderIdRaw id, Quantity q, Price p, TradeId t, Timestamp) override {
    events.push_back("fill " + std::to_string(id) + " q=" + std::to_string(q) +
                     " p=" + std::to_string(p) + " t=" + std::to_string(t));
  }
  void on_reject(OrderIdRaw id, RejectReason, Timestamp) override {
    events.push_back("reject " + std::to_string(id));
  }
  void on_cancel_ack(OrderIdRaw id, Timestamp) override {
    events.push_back("cancel_ack " + std::to_string(id));
  }
  void on_unsolicited_cancel(OrderIdRaw id, Timestamp) override {
    events.push_back("unsolicited " + std::to_string(id));
  }
};

// One venue wired end-to-end: sim <-> OrderManager, health-aware router.
struct SimHarness {
  CapturingLogSink          log;
  ExchangeRegistry          registry;
  CircuitBreakerHealthModel health;
  SimulatedExchange*        sim{nullptr};
  std::unique_ptr<HealthAwareRouter> router;
  std::unique_ptr<OrderManager>      om;

  explicit SimHarness(SimConfig cfg = SimConfig{}, HealthConfig hc = HealthConfig{})
      : health(hc, &log) {
    auto s = std::make_unique<SimulatedExchange>(static_cast<VenueId>(0), cfg);
    sim = s.get();
    registry.add(std::move(s));
    health.register_venue(0);
    health.set_clock([this] { return sim->now(); });
    router = std::make_unique<HealthAwareRouter>(health, registry.venues());
    om = std::make_unique<OrderManager>(registry, *router, health, &log);
    om->set_clock([this] { return sim->now(); });
    sim->set_event_sink(om.get());
  }

  void poll_n(int n) { for (int i = 0; i < n; ++i) sim->poll(); }
};

}  // namespace oms::test
