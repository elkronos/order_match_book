#pragma once

#include "ob/state_machine.hpp"
#include "ob/types.hpp"

namespace ob {

// Resting/working order. One cache line; lives in a pre-allocated pool and is
// threaded into its price level's FIFO through the intrusive prev/next links.
struct alignas(64) Order {
  OrderId id = 0;
  Price price = 0;  // 0 for market orders
  Qty leaves = 0;   // open (unfilled) quantity
  Qty cum = 0;      // cumulative filled quantity
  Order* prev = nullptr;
  Order* next = nullptr;
  OwnerId owner = 0;
  Side side = Side::Buy;
  OrderType type = OrderType::Limit;
  TimeInForce tif = TimeInForce::GTC;
  OrderState state = OrderState::PendingNew;
  bool post_only = false;
};
static_assert(sizeof(Order) == 64);

struct NewOrder {
  OrderId id = 0;
  Side side = Side::Buy;
  OrderType type = OrderType::Limit;
  TimeInForce tif = TimeInForce::GTC;
  Price price = 0;  // ignored for market orders
  Qty qty = 0;
  OwnerId owner = 0;
  bool post_only = false;
};

struct Trade {
  std::uint64_t seq = 0;  // monotonically increasing match id
  OrderId buy_id = 0;
  OrderId sell_id = 0;
  OwnerId buy_owner = 0;   // ids are only unique per owner
  OwnerId sell_owner = 0;
  Price price = 0;
  Qty qty = 0;
  Aggressor aggressor = Aggressor::None;
};

inline NewOrder limit_order(OrderId id, Side side, Price price, Qty qty,
                            TimeInForce tif = TimeInForce::GTC, OwnerId owner = 0,
                            bool post_only = false) {
  return NewOrder{id, side, OrderType::Limit, tif, price, qty, owner, post_only};
}

inline NewOrder market_order(OrderId id, Side side, Qty qty, TimeInForce tif = TimeInForce::IOC,
                             OwnerId owner = 0) {
  return NewOrder{id, side, OrderType::Market, tif, 0, qty, owner, false};
}

}  // namespace ob
