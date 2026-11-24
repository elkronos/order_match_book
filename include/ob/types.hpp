#pragma once

#include <cstdint>
#include <string_view>

namespace ob {

using OrderId = std::uint64_t;  // 0 is reserved as "invalid"
using Price = std::int64_t;     // integer ticks; may be negative (spreads, etc.)
using Qty = std::uint64_t;
using OwnerId = std::uint32_t;  // 0 = anonymous (self-trade prevention disabled)

enum class Side : std::uint8_t { Buy, Sell };
enum class OrderType : std::uint8_t { Limit, Market };
enum class TimeInForce : std::uint8_t { GTC, Day, IOC, FOK };
enum class Aggressor : std::uint8_t { Buy, Sell, None };  // None = auction print
enum class Command : std::uint8_t { New, Cancel, Modify, Session };

// Single vocabulary for both "why did this order change state" and
// "why was this command rejected". Reason::Ok is the success return code.
enum class Reason : std::uint8_t {
  Ok,
  // Lifecycle causes (attached to order state transitions).
  Accepted,
  Fill,
  UserCancel,
  Replaced,
  IocRemainder,
  FokUnfillable,
  SelfTradePrevented,
  SessionEnd,
  // Rejections.
  InvalidId,
  DuplicateId,
  InvalidQty,
  InvalidPrice,
  InvalidFlags,
  PostOnlyWouldCross,
  BookFull,
  UnknownOrder,
  SessionClosed,
  SessionHalted,
  NotAllowedInAuction,
  InvalidTransition,
};

constexpr Side opposite(Side s) noexcept { return s == Side::Buy ? Side::Sell : Side::Buy; }

constexpr std::string_view to_string(Side s) noexcept { return s == Side::Buy ? "Buy" : "Sell"; }

constexpr std::string_view to_string(OrderType t) noexcept {
  return t == OrderType::Limit ? "Limit" : "Market";
}

constexpr std::string_view to_string(TimeInForce t) noexcept {
  switch (t) {
    case TimeInForce::GTC: return "GTC";
    case TimeInForce::Day: return "Day";
    case TimeInForce::IOC: return "IOC";
    case TimeInForce::FOK: return "FOK";
  }
  return "?";
}

constexpr std::string_view to_string(Aggressor a) noexcept {
  switch (a) {
    case Aggressor::Buy: return "Buy";
    case Aggressor::Sell: return "Sell";
    case Aggressor::None: return "None";
  }
  return "?";
}

constexpr std::string_view to_string(Command c) noexcept {
  switch (c) {
    case Command::New: return "New";
    case Command::Cancel: return "Cancel";
    case Command::Modify: return "Modify";
    case Command::Session: return "Session";
  }
  return "?";
}

constexpr std::string_view to_string(Reason r) noexcept {
  switch (r) {
    case Reason::Ok: return "Ok";
    case Reason::Accepted: return "Accepted";
    case Reason::Fill: return "Fill";
    case Reason::UserCancel: return "UserCancel";
    case Reason::Replaced: return "Replaced";
    case Reason::IocRemainder: return "IocRemainder";
    case Reason::FokUnfillable: return "FokUnfillable";
    case Reason::SelfTradePrevented: return "SelfTradePrevented";
    case Reason::SessionEnd: return "SessionEnd";
    case Reason::InvalidId: return "InvalidId";
    case Reason::DuplicateId: return "DuplicateId";
    case Reason::InvalidQty: return "InvalidQty";
    case Reason::InvalidPrice: return "InvalidPrice";
    case Reason::InvalidFlags: return "InvalidFlags";
    case Reason::PostOnlyWouldCross: return "PostOnlyWouldCross";
    case Reason::BookFull: return "BookFull";
    case Reason::UnknownOrder: return "UnknownOrder";
    case Reason::SessionClosed: return "SessionClosed";
    case Reason::SessionHalted: return "SessionHalted";
    case Reason::NotAllowedInAuction: return "NotAllowedInAuction";
    case Reason::InvalidTransition: return "InvalidTransition";
  }
  return "?";
}

}  // namespace ob
