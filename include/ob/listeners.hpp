#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "ob/order.hpp"
#include "ob/state_machine.hpp"
#include "ob/types.hpp"

namespace ob {

// Discards everything; compiles to nothing. Use for benchmarks.
struct NullListener {
  void on_order(const Order&, OrderState, Reason) noexcept {}
  void on_trade(const Trade&) noexcept {}
  void on_reject(OrderId, OwnerId, Command, Reason) noexcept {}
  void on_session(SessionState, SessionState) noexcept {}
};

// Flattened, comparable snapshot of one engine output event.
struct Event {
  enum class Kind : std::uint8_t { Order, Trade, Reject, Session };
  Kind kind = Kind::Order;
  // Order and reject events
  OrderId id = 0;
  OwnerId owner = 0;
  OrderState from = OrderState::PendingNew;
  OrderState to = OrderState::PendingNew;
  Reason reason = Reason::Ok;
  Price price = 0;
  Qty leaves = 0;
  Qty cum = 0;
  // Trade events
  Trade trade{};
  // Reject events (id + reason above)
  Command command = Command::New;
  // Session events
  SessionState session_from = SessionState::Closed;
  SessionState session_to = SessionState::Closed;

  bool operator==(const Event& o) const {
    if (kind != o.kind) return false;
    switch (kind) {
      case Kind::Order:
        return id == o.id && owner == o.owner && from == o.from && to == o.to && reason == o.reason &&
               price == o.price && leaves == o.leaves && cum == o.cum;
      case Kind::Trade:
        return trade.seq == o.trade.seq && trade.buy_id == o.trade.buy_id &&
               trade.sell_id == o.trade.sell_id && trade.buy_owner == o.trade.buy_owner &&
               trade.sell_owner == o.trade.sell_owner && trade.price == o.trade.price &&
               trade.qty == o.trade.qty && trade.aggressor == o.trade.aggressor;
      case Kind::Reject: return id == o.id && owner == o.owner && command == o.command && reason == o.reason;
      case Kind::Session: return session_from == o.session_from && session_to == o.session_to;
    }
    return false;
  }

  std::string to_string() const {
    using std::to_string;
    using ob::to_string;
    switch (kind) {
      case Kind::Order:
        return "ORDER   id=" + to_string(id) + (owner ? "/" + to_string(owner) : "") + " " + std::string(to_string(from)) + " -> " +
               std::string(to_string(to)) + " (" + std::string(to_string(reason)) +
               ") px=" + to_string(price) + " leaves=" + to_string(leaves) + " cum=" + to_string(cum);
      case Kind::Trade:
        return "TRADE   #" + to_string(trade.seq) + " buy=" + to_string(trade.buy_id) +
               " sell=" + to_string(trade.sell_id) + " px=" + to_string(trade.price) +
               " qty=" + to_string(trade.qty) + " aggressor=" + std::string(to_string(trade.aggressor));
      case Kind::Reject:
        return "REJECT  " + std::string(to_string(command)) + " id=" + to_string(id) +
               (owner ? "/" + to_string(owner) : "") + " (" +
               std::string(to_string(reason)) + ")";
      case Kind::Session:
        return "SESSION " + std::string(to_string(session_from)) + " -> " +
               std::string(to_string(session_to));
    }
    return "?";
  }
};

// Records every event; used by tests and the CLI.
struct RecordingListener {
  std::vector<Event> events;

  void on_order(const Order& o, OrderState from, Reason why) {
    Event e;
    e.kind = Event::Kind::Order;
    e.id = o.id;
    e.owner = o.owner;
    e.from = from;
    e.to = o.state;
    e.reason = why;
    e.price = o.price;
    e.leaves = o.leaves;
    e.cum = o.cum;
    events.push_back(e);
  }
  void on_trade(const Trade& t) {
    Event e;
    e.kind = Event::Kind::Trade;
    e.trade = t;
    events.push_back(e);
  }
  void on_reject(OrderId id, OwnerId owner, Command c, Reason why) {
    Event e;
    e.kind = Event::Kind::Reject;
    e.id = id;
    e.owner = owner;
    e.command = c;
    e.reason = why;
    events.push_back(e);
  }
  void on_session(SessionState from, SessionState to) {
    Event e;
    e.kind = Event::Kind::Session;
    e.session_from = from;
    e.session_to = to;
    events.push_back(e);
  }
};

}  // namespace ob
