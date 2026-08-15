#pragma once

#include <queue>
#include <random>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "oms/ExchangeRegistry.h"   // pulls ExchangeClient/MarketData/Types

namespace oms {

// Deterministic, seeded control over the failure surface (spec 2.4). The entire
// test suite and the results/ benchmark run on this. Time is logical: each
// poll() advances an internal clock by step_ns and delivers all due events to
// the sink, synchronously, in a fixed total order -> fully reproducible.
struct SimConfig {
  uint64_t  seed{0};
  Timestamp step_ns{1'000'000};          // logical time advanced per poll()
  Timestamp ack_latency_ns{1'000'000};   // send -> confirm
  Timestamp fill_latency_ns{2'000'000};  // send -> first fill
  Timestamp fill_gap_ns{1'000'000};      // between partial fills
  int       partial_chunks{1};           // split each order into N fills
  double    reject_prob{0.0};            // venue rejects the order (async)
  double    unsolicited_cancel_prob{0.0};
  double    duplicate_fill_prob{0.0};    // re-send a fill with the same trade id
  bool      reorder_fill_before_ack{false};  // deliver first fill before confirm
  bool      start_connected{true};
};

class SimulatedExchange : public ExchangeClient {
 public:
  SimulatedExchange(VenueId venue, SimConfig cfg);

  // ---- ExchangeClient ----
  VenueId venue_id() const override { return venue_; }
  SendResult place_order(const Order&) override;
  SendResult cancel_order(OrderIdRaw, const ExchangeOrderId&) override;
  std::unordered_map<InstrumentId, Quantity> get_positions() override { return positions_; }
  TopOfBook get_order_book(InstrumentId) override;
  void set_event_sink(ExchangeEventSink* sink) override { sink_ = sink; }
  void poll() override;
  bool is_connected() const override { return connected_; }
  bool supports_mass_cancel() const override { return true; }
  SendResult mass_cancel(InstrumentId) override;

  // ---- test / scenario controls ----
  Timestamp now() const { return now_; }
  void set_connected(bool c) { connected_ = c; }
  void set_book(InstrumentId, const TopOfBook&);
  // Fabricate an exchange-side position (e.g. a fill the OMS missed while down),
  // used to exercise reconciliation drift.
  void set_position(InstrumentId inst, Quantity q) { positions_[inst] = q; }

  // Precise injectors for race scenarios (delivered on the next poll()).
  void inject_confirm(OrderIdRaw);
  void inject_fill(OrderIdRaw, Quantity, Price, TradeId, bool final);
  void inject_reject(OrderIdRaw, RejectReason);
  void inject_cancel_ack(OrderIdRaw);
  void inject_cancel_reject(OrderIdRaw);
  void inject_unsolicited_cancel(OrderIdRaw);

 private:
  enum class EvType : uint8_t { Confirm, Fill, Reject, CancelAck, CancelReject, Unsolicited };

  struct SimEvent {
    Timestamp    at{0};
    uint64_t     seq{0};       // FIFO tie-break for a fully deterministic order
    EvType       type{EvType::Confirm};
    OrderIdRaw   id{0};
    Quantity     qty{0};
    Price        price{0};
    TradeId      trade{0};
    bool         final{false};
    RejectReason reason{RejectReason::Unknown};
  };

  struct EventGreater {
    bool operator()(const SimEvent& a, const SimEvent& b) const {
      if (a.at != b.at) return a.at > b.at;
      return a.seq > b.seq;   // earlier seq first
    }
  };

  struct Working {
    InstrumentId              instrument{0};
    Side                      side{Side::Buy};
    Quantity                  size{0};
    Quantity                  remaining{0};
    Price                     price{0};
    ExchangeOrderId           exchange_id;
    bool                      alive{true};
    std::unordered_set<TradeId> seen_trades;   // sim-side de-dup for its own book
  };

  void schedule(SimEvent ev);
  void deliver(const SimEvent&);

  VenueId       venue_;
  SimConfig     cfg_;
  std::mt19937_64 rng_;
  ExchangeEventSink* sink_{nullptr};
  bool          connected_{true};
  Timestamp     now_{0};
  uint64_t      seq_counter_{0};
  uint64_t      exch_counter_{0};
  TradeId       trade_counter_{0};

  std::priority_queue<SimEvent, std::vector<SimEvent>, EventGreater> queue_;
  std::unordered_map<OrderIdRaw, Working>    working_;
  std::unordered_map<InstrumentId, Quantity> positions_;
  std::unordered_map<InstrumentId, TopOfBook> books_;
};

// Registers a "sim" factory with ExchangeRegistry (name -> constructor).
void register_simulated_exchange_factory();

}  // namespace oms
