#include <catch2/catch_test_macros.hpp>

#include "oms/OrderId.h"
#include "oms/OrderPool.h"

using namespace oms;

TEST_CASE("pool hands out slots and tracks usage", "[pool]") {
  OrderPool pool(8);
  CHECK(pool.capacity() == 8u);
  CHECK(pool.in_use() == 0u);

  const uint32_t a = pool.acquire();
  const uint32_t b = pool.acquire();
  CHECK(a == 0u);          // first acquire hands out slot 0
  CHECK(b == 1u);
  CHECK(pool.in_use() == 2u);
  CHECK(pool.slot_live(a));
  CHECK(pool.slot_live(b));

  pool.release(a);
  CHECK_FALSE(pool.slot_live(a));
  CHECK(pool.in_use() == 1u);
}

TEST_CASE("releasing a slot bumps its generation", "[pool][generation]") {
  OrderPool pool(4);
  const uint32_t slot = pool.acquire();
  const uint16_t g0 = pool.generation(slot);

  pool.release(slot);
  CHECK(pool.generation(slot) == static_cast<uint16_t>(g0 + 1));
}

TEST_CASE("stale event for a recycled slot is rejected", "[pool][generation]") {
  OrderPool pool(4);

  // Order A occupies a slot; mint its id from the live generation.
  const uint32_t slot = pool.acquire();
  const OrderIdRaw id_a =
      encode_order_id(/*venue=*/1, pool.generation(slot), slot, /*seq=*/100);
  REQUIRE(pool.is_current(id_a));

  // A dies; the slot is recycled to a new order B (same slot, new generation).
  pool.release(slot);
  const uint32_t slot_b = pool.acquire();
  REQUIRE(slot_b == slot);               // LIFO freelist reuses the slot
  const OrderIdRaw id_b =
      encode_order_id(1, pool.generation(slot_b), slot_b, 200);

  // The live order's id validates; the stale id for the dead order does not.
  CHECK(pool.is_current(id_b));
  CHECK_FALSE(pool.is_current(id_a));    // late fill for A must be rejected
}

TEST_CASE("id for a freed slot is not current", "[pool][generation]") {
  OrderPool pool(4);
  const uint32_t slot = pool.acquire();
  const OrderIdRaw id = encode_order_id(0, pool.generation(slot), slot, 1);
  REQUIRE(pool.is_current(id));
  pool.release(slot);
  CHECK_FALSE(pool.is_current(id));      // slot no longer allocated
}

TEST_CASE("out-of-range slot is never current", "[pool]") {
  OrderPool pool(4);
  const OrderIdRaw id = encode_order_id(0, 0, /*slot=*/9999, 0);
  CHECK_FALSE(pool.is_current(id));
}
