#include <catch2/catch_test_macros.hpp>

#include "oms/Order.h"

using namespace oms;

TEST_CASE("terminal-state helper matches spec", "[order]") {
  CHECK(is_terminal(OrderStatus::Filled));
  CHECK(is_terminal(OrderStatus::Cancelled));
  CHECK(is_terminal(OrderStatus::Rejected));
  CHECK_FALSE(is_terminal(OrderStatus::New));
  CHECK_FALSE(is_terminal(OrderStatus::PartiallyFilled));
  CHECK_FALSE(is_terminal(OrderStatus::SentCancel));
}

TEST_CASE("remaining is derived, never stored", "[order]") {
  Order o;
  o.size = 100;
  o.filled_size = 30;
  CHECK(o.remaining() == 70);
  o.filled_size = 100;
  CHECK(o.remaining() == 0);
}

TEST_CASE("avg fill price divides accumulated notional on demand", "[order]") {
  Order o;
  CHECK(o.avg_fill_price() == 0);          // unfilled -> 0, no divide-by-zero

  // Two fills: 40 @ 100 ticks, 60 @ 110 ticks -> notional 4000 + 6600 = 10600.
  o.filled_size = 100;
  o.total_fill_amount = 10600;
  CHECK(o.avg_fill_price() == 106);        // 10600 / 100
}

TEST_CASE("pending flags are orthogonal to status", "[order]") {
  Order o;
  o.status = OrderStatus::PartiallyFilled;  // a fill can arrive...
  o.pending_cancel = true;                  // ...while a cancel is in flight
  CHECK(o.status == OrderStatus::PartiallyFilled);
  CHECK(o.pending_cancel);
  CHECK_FALSE(is_terminal(o.status));
}
