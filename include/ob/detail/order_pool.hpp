#pragma once

// Fixed-capacity slab of Orders with an intrusive free list. All memory is
// allocated (and touched) up front, so acquire/release are O(1) and never hit
// the system allocator or take a page fault on the hot path.

#include <cstddef>
#include <vector>

#include "ob/order.hpp"

namespace ob::detail {

class OrderPool {
 public:
  explicit OrderPool(std::size_t capacity) : slab_(capacity) {
    for (std::size_t i = capacity; i-- > 0;) {
      slab_[i].next = free_;
      free_ = &slab_[i];
    }
  }

  OrderPool(const OrderPool&) = delete;
  OrderPool& operator=(const OrderPool&) = delete;

  bool full() const noexcept { return free_ == nullptr; }
  std::size_t used() const noexcept { return used_; }
  std::size_t capacity() const noexcept { return slab_.size(); }

  // Precondition: !full().
  Order* acquire() noexcept {
    Order* o = free_;
    free_ = o->next;
    ++used_;
    return o;
  }

  void release(Order* o) noexcept {
    o->prev = nullptr;
    o->next = free_;
    free_ = o;
    --used_;
  }

 private:
  std::vector<Order> slab_;
  Order* free_ = nullptr;
  std::size_t used_ = 0;
};

}  // namespace ob::detail
