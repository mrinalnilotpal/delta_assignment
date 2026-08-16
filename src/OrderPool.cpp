#include "oms/OrderPool.h"

namespace oms {

OrderPool::OrderPool(uint32_t capacity)
    : capacity_(capacity),
      slots_(capacity),          // constructs every Order -> commits its pages
      generation_(capacity, 0),
      allocated_(capacity, 0) {
  assert(capacity_ > 0 && capacity_ <= kMaxSlots &&
         "OrderPool capacity must fit the 24-bit slot field");

  // Touch all bookkeeping memory now so the OS commits physical pages here,
  // off the wire path.
  free_list_.reserve(capacity_);
  for (uint32_t i = 0; i < capacity_; ++i) {
    generation_[i] = 0;
    allocated_[i] = 0;
    // Push in descending order so the first acquire() hands out slot 0.
    free_list_.push_back(capacity_ - 1 - i);
  }
}

uint32_t OrderPool::acquire() {
  assert(!free_list_.empty() && "OrderPool exhausted - hard stop");
  const uint32_t slot = free_list_.back();
  free_list_.pop_back();
  allocated_[slot] = 1;
  ++in_use_;

  // Reset reused state without reallocating (clear() keeps string capacity).
  Order& o = slots_[slot];
  o.internal_id = 0;
  o.size = 0;
  o.filled_size = 0;
  o.total_fill_amount = 0;
  o.limit_price = 0;
  o.instrument = 0;
  o.status = OrderStatus::New;
  o.side = Side::Buy;
  o.type = OrderType::Limit;
  o.pending_cancel = false;
  o.false_cancel_reject = false;
  o.exchange_id.clear();
  o.strategy_id = 0;
  o.algo_tag = 0;
  o.ack_timed_out = false;
  o.reconciliation = false;
  o.ts = OrderTimestamps{};
  o.int_data.fill(0);
  return slot;
}

void OrderPool::release(uint32_t slot) {
  assert(slot < capacity_ && "release out of range");
  assert(allocated_[slot] && "double free of order slot");
  allocated_[slot] = 0;
  ++generation_[slot];   // recycle -> any id minted before now is stale
  --in_use_;
  free_list_.push_back(slot);
}

bool OrderPool::is_current(OrderIdRaw id) const {
  const OrderId d = decode_order_id(id);
  if (d.slot >= capacity_) return false;
  if (!allocated_[d.slot]) return false;
  return generation_[d.slot] == d.generation;
}

}  // namespace oms
