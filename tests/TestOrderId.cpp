#include <catch2/catch_test_macros.hpp>

#include "oms/OrderId.h"

using namespace oms;

// Compile-time guarantee that encode/decode are constexpr and round-trip.
static_assert([] {
  auto d = decode_order_id(encode_order_id(3, 7, 12345, 42));
  return d.venue == 3 && d.generation == 7 && d.slot == 12345 && d.sequence == 42;
}(), "encode/decode must round-trip at compile time");

TEST_CASE("order id round-trips typical values", "[order_id]") {
  const OrderIdRaw raw = encode_order_id(3, 7, 12345, 42);
  const OrderId d = decode_order_id(raw);
  CHECK(d.venue == 3);
  CHECK(d.generation == 7);
  CHECK(d.slot == 12345u);
  CHECK(d.sequence == 42);
}

TEST_CASE("order id round-trips field maxima", "[order_id]") {
  const uint8_t  venue = 0xFF;
  const uint16_t gen   = 0xFFFF;
  const uint32_t slot  = 0xFFFFFF;   // 24 bits
  const uint16_t seq   = 0xFFFF;

  const OrderId d = decode_order_id(encode_order_id(venue, gen, slot, seq));
  CHECK(d.venue == venue);
  CHECK(d.generation == gen);
  CHECK(d.slot == slot);
  CHECK(d.sequence == seq);
}

TEST_CASE("fields do not bleed into each other", "[order_id]") {
  // Set only one field; all others must decode to zero.
  CHECK(decode_order_id(encode_order_id(0xFF, 0, 0, 0)).venue == 0xFF);
  CHECK(decode_order_id(encode_order_id(0xFF, 0, 0, 0)).generation == 0);
  CHECK(decode_order_id(encode_order_id(0xFF, 0, 0, 0)).slot == 0u);
  CHECK(decode_order_id(encode_order_id(0xFF, 0, 0, 0)).sequence == 0);

  const OrderId only_slot = decode_order_id(encode_order_id(0, 0, 0xFFFFFF, 0));
  CHECK(only_slot.venue == 0);
  CHECK(only_slot.generation == 0);
  CHECK(only_slot.slot == 0xFFFFFFu);
  CHECK(only_slot.sequence == 0);
}

TEST_CASE("slot beyond 24 bits is masked, not overflowed", "[order_id]") {
  // Bit 24 set -> must be dropped, leaving slot 0 and not corrupting generation.
  const uint32_t slot = 0x1000000;   // 2^24
  const OrderId d = decode_order_id(encode_order_id(0, 0, slot, 0));
  CHECK(d.slot == 0u);
  CHECK(d.generation == 0);
}
