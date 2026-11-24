#pragma once

// The two finite-state machines that govern the engine:
//
//   * OrderState   – lifecycle of a single order.
//   * SessionState – trading phase of the whole book.
//
// Both are expressed as constexpr transition tables. The engine routes every
// state change through can_transition(); an illegal transition is a logic bug
// and aborts the process (it can never be caused by client input).

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace ob {

// ---------------------------------------------------------------------------
// Order lifecycle
//
//   PendingNew ──► Rejected                          (validation failed)
//       │
//       ▼
//     Active ──► PartiallyFilled ──► Filled
//       │  ▲          │  ▲
//       │  └─replace  │  └─fill / replace
//       ├──► Filled   │
//       ├──► Cancelled◄┤   (user cancel, IOC remainder, FOK, self-trade)
//       └──► Expired ◄─┘   (Day order at session close)
//
// Filled, Cancelled, Rejected and Expired are terminal.
// ---------------------------------------------------------------------------
enum class OrderState : std::uint8_t {
  PendingNew,
  Active,
  PartiallyFilled,
  Filled,
  Cancelled,
  Rejected,
  Expired,
};
inline constexpr std::size_t kOrderStateCount = 7;

enum class SessionState : std::uint8_t {
  Closed,   // no order entry; cancels allowed; GTC orders persist
  PreOpen,  // orders accumulate without matching (book may cross)
  Open,     // continuous matching
  Halted,   // cancels only
};
inline constexpr std::size_t kSessionStateCount = 4;

namespace detail {
constexpr std::uint8_t bit(OrderState s) noexcept { return std::uint8_t(1u << unsigned(s)); }
constexpr std::uint8_t bit(SessionState s) noexcept { return std::uint8_t(1u << unsigned(s)); }
}  // namespace detail

inline constexpr std::array<std::uint8_t, kOrderStateCount> kOrderTransitions = [] {
  using S = OrderState;
  using detail::bit;
  std::array<std::uint8_t, kOrderStateCount> t{};
  t[std::size_t(S::PendingNew)] = bit(S::Active) | bit(S::Rejected);
  t[std::size_t(S::Active)] = bit(S::Active) | bit(S::PartiallyFilled) | bit(S::Filled) |
                              bit(S::Cancelled) | bit(S::Expired);
  t[std::size_t(S::PartiallyFilled)] =
      bit(S::PartiallyFilled) | bit(S::Filled) | bit(S::Cancelled) | bit(S::Expired);
  // Filled, Cancelled, Rejected, Expired: terminal (0).
  return t;
}();

//   Closed ──► PreOpen ──► Open ◄──► Halted
//     ▲          │  ▲       │          │
//     │          │  └───────┼──────────┘ (re-opening auction)
//     └──────────┴──────────┴──────────┘
inline constexpr std::array<std::uint8_t, kSessionStateCount> kSessionTransitions = [] {
  using S = SessionState;
  using detail::bit;
  std::array<std::uint8_t, kSessionStateCount> t{};
  t[std::size_t(S::Closed)] = bit(S::PreOpen);
  t[std::size_t(S::PreOpen)] = bit(S::Open) | bit(S::Closed);
  t[std::size_t(S::Open)] = bit(S::Halted) | bit(S::Closed);
  t[std::size_t(S::Halted)] = bit(S::Open) | bit(S::PreOpen) | bit(S::Closed);
  return t;
}();

constexpr bool can_transition(OrderState from, OrderState to) noexcept {
  return (kOrderTransitions[std::size_t(from)] & detail::bit(to)) != 0;
}
constexpr bool can_transition(SessionState from, SessionState to) noexcept {
  return (kSessionTransitions[std::size_t(from)] & detail::bit(to)) != 0;
}
constexpr bool is_terminal(OrderState s) noexcept { return kOrderTransitions[std::size_t(s)] == 0; }

// Compile-time spec checks.
static_assert(is_terminal(OrderState::Filled) && is_terminal(OrderState::Cancelled) &&
              is_terminal(OrderState::Rejected) && is_terminal(OrderState::Expired));
static_assert(!is_terminal(OrderState::PendingNew) && !is_terminal(OrderState::Active) &&
              !is_terminal(OrderState::PartiallyFilled));
static_assert(!can_transition(OrderState::PendingNew, OrderState::Filled),
              "orders must be acknowledged before they can trade");
static_assert(!can_transition(OrderState::PartiallyFilled, OrderState::Active),
              "fills are irreversible");
static_assert(!can_transition(SessionState::Closed, SessionState::Open),
              "the book must pass through an auction before opening");

constexpr std::string_view to_string(OrderState s) noexcept {
  switch (s) {
    case OrderState::PendingNew: return "PendingNew";
    case OrderState::Active: return "Active";
    case OrderState::PartiallyFilled: return "PartiallyFilled";
    case OrderState::Filled: return "Filled";
    case OrderState::Cancelled: return "Cancelled";
    case OrderState::Rejected: return "Rejected";
    case OrderState::Expired: return "Expired";
  }
  return "?";
}

constexpr std::string_view to_string(SessionState s) noexcept {
  switch (s) {
    case SessionState::Closed: return "Closed";
    case SessionState::PreOpen: return "PreOpen";
    case SessionState::Open: return "Open";
    case SessionState::Halted: return "Halted";
  }
  return "?";
}

}  // namespace ob
