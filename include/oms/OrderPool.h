#pragma once

#include "oms/Order.h"   // pulls OrderId.h + Types.h

namespace oms {

// Fixed-capacity Order pool with a freelist and per-slot generation (spec 2.2).
// Slots and their pages are committed up front (no hot-path allocation/faults);
// releasing a slot bumps its generation so stale ids are rejected; exhaustion is
// a hard stop, never a silent drop.
class OrderPool {
 public:
  static constexpr uint32_t kDefaultCapacity = 100'000;

  explicit OrderPool(uint32_t capacity = kDefaultCapacity);

  // Acquire a free slot; asserts (hard stop) if the pool is exhausted.
  uint32_t acquire();

  // Return a slot to the freelist and bump its generation.
  void release(uint32_t slot);

  Order&       operator[](uint32_t slot)       { return slots_[slot]; }
  const Order& operator[](uint32_t slot) const { return slots_[slot]; }

  uint16_t generation(uint32_t slot) const { return generation_[slot]; }
  bool     slot_live(uint32_t slot)  const { return slot < capacity_ && allocated_[slot]; }

  // Fresh event: id points at a live slot whose generation still matches.
  bool is_current(OrderIdRaw id) const;

  uint32_t capacity() const { return capacity_; }
  uint32_t in_use()   const { return in_use_; }

 private:
  uint32_t              capacity_{0};
  uint32_t              in_use_{0};
  std::vector<Order>    slots_;
  std::vector<uint16_t> generation_;
  std::vector<uint8_t>  allocated_;   // 1 = live, 0 = free
  std::vector<uint32_t> free_list_;
};

}  // namespace oms
