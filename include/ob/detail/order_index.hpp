#pragma once

// Open-addressing (owner, OrderId) -> Order* map.
//
// Ids are scoped per owner: two participants may use the same client order
// id, one owner can never address (cancel, modify, or probe for the existence
// of) another owner's orders, and the engine never reveals another owner's
// ids through DuplicateId.
//
// Linear probing with backward-shift deletion (no tombstones, so probe lengths
// never degrade under churn). Capacity is fixed at construction to >= 2x the
// maximum live order count: load factor <= 0.5, no allocation or rehash on the
// hot path. Slots are 16 bytes ({id, Order*}); the owner half of the key is
// read through the pointer, which is only dereferenced on an id match, so the
// common miss path never touches the order.
//
// Keys are client-chosen, so the hash is keyed with a secret seed and
// finished with the murmur3 mixer. An unkeyed multiplicative hash is
// invertible, which lets a hostile client craft ids that share one home slot
// and turn every lookup into a linear scan. Slot id 0 marks "empty"
// (OrderId 0 is rejected by the engine).

#include <cstddef>
#include <cstdint>
#include <vector>

#include "ob/order.hpp"

namespace ob::detail {

class OrderIndex {
 public:
  OrderIndex(std::size_t max_entries, std::uint64_t seed) : seed_(seed) {
    std::size_t cap = 16;
    while (cap < max_entries * 2) cap <<= 1;
    slots_.assign(cap, Slot{});
    mask_ = cap - 1;
  }

  std::size_t size() const noexcept { return size_; }

  Order* find(OrderId id, OwnerId owner) const noexcept {
    for (std::size_t i = home(id, owner);; i = (i + 1) & mask_) {
      const Slot& s = slots_[i];
      if (s.id == id && s.value->owner == owner) return s.value;
      if (s.id == 0) return nullptr;
    }
  }

  // Precondition: o->id != 0, key not present, size() < max_entries.
  void insert(Order* o) noexcept {
    std::size_t i = home(o->id, o->owner);
    while (slots_[i].id != 0) i = (i + 1) & mask_;
    slots_[i] = Slot{o->id, o};
    ++size_;
  }

  bool erase(OrderId id, OwnerId owner) noexcept {
    std::size_t i = home(id, owner);
    for (;; i = (i + 1) & mask_) {
      if (slots_[i].id == id && slots_[i].value->owner == owner) break;
      if (slots_[i].id == 0) return false;
    }
    // Backward shift: pull later members of the probe chain into the hole
    // unless their home slot lies cyclically within (hole, j].
    std::size_t j = i;
    for (;;) {
      j = (j + 1) & mask_;
      if (slots_[j].id == 0) break;
      const std::size_t h = home(slots_[j].id, slots_[j].value->owner);
      const bool stays = (i <= j) ? (i < h && h <= j) : (i < h || h <= j);
      if (stays) continue;
      slots_[i] = slots_[j];
      i = j;
    }
    slots_[i] = Slot{};
    --size_;
    return true;
  }

 private:
  struct Slot {
    OrderId id = 0;
    Order* value = nullptr;
  };
  static_assert(sizeof(Slot) == 16);

  std::size_t home(OrderId id, OwnerId owner) const noexcept {
    std::uint64_t x = (id ^ seed_) + owner * 0x9E3779B97F4A7C15ull;
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdull;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ull;
    x ^= x >> 33;
    return std::size_t(x) & mask_;
  }

  std::vector<Slot> slots_;
  std::size_t mask_ = 0;
  std::uint64_t seed_;
  std::size_t size_ = 0;
};

}  // namespace ob::detail
