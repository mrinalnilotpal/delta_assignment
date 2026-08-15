#pragma once

#include <cstdint>

namespace oms {

// Hash-free order-id encoding (spec 2.2). Fields pack into 64 bits so inbound
// events decode by arithmetic, no lookup:
//   [venue:8][generation:16][slot:24][sequence:16]
// - venue      routes the event to the right ExchangeClient/book.
// - generation bumps on slot reuse, so a stale event for a recycled slot is
//   rejected instead of mutating the live order now in that slot.
// - slot       indexes the preallocated OrderPool directly (O(1)).
// - sequence   is monotonic per venue, for ordering diagnostics.
namespace id_bits {
inline constexpr unsigned kVenue      = 8;
inline constexpr unsigned kGeneration = 16;
inline constexpr unsigned kSlot       = 24;
inline constexpr unsigned kSequence   = 16;
static_assert(kVenue + kGeneration + kSlot + kSequence == 64,
              "order id fields must pack into exactly 64 bits");

inline constexpr unsigned kSequenceShift   = 0;
inline constexpr unsigned kSlotShift       = kSequence;                       // 16
inline constexpr unsigned kGenerationShift = kSequence + kSlot;              // 40
inline constexpr unsigned kVenueShift      = kSequence + kSlot + kGeneration; // 56

inline constexpr uint64_t kVenueMask      = (uint64_t{1} << kVenue) - 1;
inline constexpr uint64_t kGenerationMask = (uint64_t{1} << kGeneration) - 1;
inline constexpr uint64_t kSlotMask       = (uint64_t{1} << kSlot) - 1;
inline constexpr uint64_t kSequenceMask   = (uint64_t{1} << kSequence) - 1;
}  // namespace id_bits

using OrderIdRaw = uint64_t;

// Decoded view of the fields packed into an OrderIdRaw.
struct OrderId {
  uint8_t  venue{0};
  uint16_t generation{0};
  uint32_t slot{0};      // only the low 24 bits are meaningful
  uint16_t sequence{0};
};

constexpr OrderIdRaw encode_order_id(uint8_t venue, uint16_t generation,
                                     uint32_t slot, uint16_t sequence) {
  return (static_cast<uint64_t>(venue)      << id_bits::kVenueShift)
       | (static_cast<uint64_t>(generation) << id_bits::kGenerationShift)
       | ((static_cast<uint64_t>(slot) & id_bits::kSlotMask) << id_bits::kSlotShift)
       | (static_cast<uint64_t>(sequence)   << id_bits::kSequenceShift);
}

constexpr OrderId decode_order_id(OrderIdRaw id) {
  OrderId out;
  out.venue      = static_cast<uint8_t>((id >> id_bits::kVenueShift) & id_bits::kVenueMask);
  out.generation = static_cast<uint16_t>((id >> id_bits::kGenerationShift) & id_bits::kGenerationMask);
  out.slot       = static_cast<uint32_t>((id >> id_bits::kSlotShift) & id_bits::kSlotMask);
  out.sequence   = static_cast<uint16_t>((id >> id_bits::kSequenceShift) & id_bits::kSequenceMask);
  return out;
}

// Largest addressable slot given the 24-bit slot field.
inline constexpr uint32_t kMaxSlots = static_cast<uint32_t>(id_bits::kSlotMask) + 1;

}  // namespace oms
