#include <catch2/catch_test_macros.hpp>

#include "oms/Signal.h"

using namespace oms;

namespace {
SignalGateConfig small_cfg() {
  SignalGateConfig c;
  c.max_signal_age = 100;
  c.seq_modulo = 256;
  c.miss_threshold = 5;
  c.hb_interval = 100;
  c.hb_threshold = 3;
  c.max_abs_target = 1000;
  return c;
}

SignalMessage make_msg(StrategyId s, uint64_t seq, Timestamp gen_at,
                       std::vector<TargetPosition> t = {{5, 100}}) {
  SignalMessage m;
  m.schema_version = 1;
  m.sequence = seq;
  m.generated_at = gen_at;
  m.strategy_id = s;
  m.targets = std::move(t);
  return m;
}

// A gate already past startup reconciliation (DNT cleared), for staleness tests.
SignalGate armed_gate(SignalGateConfig c = small_cfg()) {
  SignalGate g(c);
  g.enable_trading(true);
  return g;
}
}  // namespace

TEST_CASE("signal gate boots in do-not-trade until startup clears it", "[signal][dnt]") {
  SignalGate gate(small_cfg());
  // Boots in DNT: every signal is rejected before startup reconciliation clears.
  CHECK(gate.check(make_msg(1, 1, 0), 0) == SignalVerdict::RejectDNT);
  gate.enable_trading(true);   // wired from StartupReport.can_start
  CHECK(gate.check(make_msg(1, 1, 0), 0) == SignalVerdict::Accept);
}

TEST_CASE("signal codec round-trips", "[signal]") {
  SignalMessage m = make_msg(7, 42, 123, {{1, -50}, {2, 300}, {3, 0}});
  const auto bytes = serialize_signal(m);
  SignalMessage out;
  REQUIRE(deserialize_signal(bytes, out));
  CHECK(out.sequence == 42);
  CHECK(out.strategy_id == 7);
  REQUIRE(out.targets.size() == 3);
  CHECK(out.targets[0].instrument == 1);
  CHECK(out.targets[0].signed_target == -50);
  CHECK(out.targets[1].signed_target == 300);
}

TEST_CASE("signal codec rejects a truncated buffer", "[signal]") {
  auto bytes = serialize_signal(make_msg(1, 1, 0));
  bytes.pop_back();
  SignalMessage out;
  CHECK_FALSE(deserialize_signal(bytes, out));
}

TEST_CASE("age check rejects a stale signal", "[signal][staleness]") {
  SignalGate gate = armed_gate();
  CHECK(gate.check(make_msg(1, 1, 0), 200) == SignalVerdict::RejectAge);   // age 200 > 100
  CHECK(gate.check(make_msg(1, 1, 200), 200) == SignalVerdict::Accept);
}

TEST_CASE("absurd target values are rejected at ingest", "[signal][staleness]") {
  SignalGate gate = armed_gate();
  CHECK(gate.check(make_msg(1, 1, 0, {{5, 2000}}), 0) == SignalVerdict::RejectAbsurd);
}

TEST_CASE("sequence gap is wrap-aware", "[signal][staleness]") {
  SignalGate gate = armed_gate();
  CHECK(gate.check(make_msg(1, 10, 0), 0) == SignalVerdict::Accept);       // first
  CHECK(gate.check(make_msg(1, 11, 0), 0) == SignalVerdict::Accept);       // in order
  CHECK(gate.check(make_msg(1, 14, 0), 0) == SignalVerdict::GapAcceptedLogged);   // fwd gap
  CHECK(gate.check(make_msg(1, 13, 0), 0) == SignalVerdict::DuplicateDropped);    // backward
  // Recovers on the next in-order sequence after the forward gap.
  CHECK(gate.check(make_msg(1, 15, 0), 0) == SignalVerdict::Accept);
}

TEST_CASE("heartbeat goes dead after N missed intervals", "[signal][staleness]") {
  SignalGate gate = armed_gate();
  CHECK(gate.check(make_msg(1, 1, 200), 200) == SignalVerdict::Accept);
  CHECK(gate.heartbeat_alive(1, 200));
  CHECK(gate.heartbeat_alive(1, 500));       // exactly at the 300ns deadline
  CHECK_FALSE(gate.heartbeat_alive(1, 501));  // breach
  CHECK_FALSE(gate.heartbeat_alive(99, 200)); // never seen -> dead
}
