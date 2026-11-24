#pragma once

// Hierarchical occupancy bitmap over the price ladder.
//
// Level 0 has one bit per price level; each level above has one bit per
// non-zero word of the level below, up to a single top word. Finding the next
// occupied level in either direction climbs until a word has a candidate bit
// and descends with ctz/clz, so it costs O(depth) word operations no matter
// how sparse the ladder is: depth 4 covers 2^24 ticks. (A flat or two-level
// design degrades to a linear scan when the next level is far away, which a
// client can provoke by quoting at the extremes of a wide ladder.)

#include <bit>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ob::detail {

class LevelBitmap {
 public:
  static constexpr std::size_t npos = static_cast<std::size_t>(-1);

  explicit LevelBitmap(std::size_t n) : n_(n) {
    std::size_t bits = n;
    do {
      layers_.emplace_back((bits + 63) / 64, 0);
      bits = layers_.back().size();
    } while (bits > 1);
  }

  std::size_t size() const noexcept { return n_; }

  bool test(std::size_t i) const noexcept { return (layers_[0][i >> 6] >> (i & 63)) & 1u; }

  void set(std::size_t i) noexcept {
    for (auto& layer : layers_) {
      std::uint64_t& w = layer[i >> 6];
      const bool was_empty = w == 0;
      w |= std::uint64_t{1} << (i & 63);
      if (!was_empty) return;  // parent bit already set
      i >>= 6;
    }
  }

  void clear(std::size_t i) noexcept {
    for (auto& layer : layers_) {
      std::uint64_t& w = layer[i >> 6];
      w &= ~(std::uint64_t{1} << (i & 63));
      if (w != 0) return;  // parent bit stays set
      i >>= 6;
    }
  }

  // Smallest set index >= i, or npos.
  std::size_t next_at_or_after(std::size_t i) const noexcept {
    if (i >= n_) return npos;
    std::size_t depth = 0;
    for (;; ++depth) {  // climb: find a word with a set bit at or after i
      if (depth == layers_.size()) return npos;
      const auto& layer = layers_[depth];
      const std::size_t w = i >> 6;
      if (w >= layer.size()) return npos;
      const std::uint64_t m = layer[w] & (~std::uint64_t{0} << (i & 63));
      if (m) {
        i = (w << 6) + std::size_t(std::countr_zero(m));
        break;
      }
      i = w + 1;  // next word, as a bit index one level up
    }
    while (depth-- > 0) i = (i << 6) + std::size_t(std::countr_zero(layers_[depth][i]));  // descend
    return i;
  }

  // Largest set index <= i, or npos.
  std::size_t prev_at_or_before(std::size_t i) const noexcept {
    if (n_ == 0 || i == npos) return npos;
    if (i >= n_) i = n_ - 1;
    std::size_t depth = 0;
    for (;; ++depth) {
      if (depth == layers_.size()) return npos;
      const std::size_t w = i >> 6;
      const std::uint64_t m = layers_[depth][w] & low_mask_inclusive(unsigned(i & 63));
      if (m) {
        i = (w << 6) + 63 - std::size_t(std::countl_zero(m));
        break;
      }
      if (w == 0) return npos;
      i = w - 1;
    }
    while (depth-- > 0) i = (i << 6) + 63 - std::size_t(std::countl_zero(layers_[depth][i]));
    return i;
  }

 private:
  static constexpr std::uint64_t low_mask_inclusive(unsigned b) noexcept {
    return b == 63 ? ~std::uint64_t{0} : ((std::uint64_t{1} << (b + 1)) - 1);
  }

  std::size_t n_;
  std::vector<std::vector<std::uint64_t>> layers_;  // [0] = one bit per price level
};

}  // namespace ob::detail
