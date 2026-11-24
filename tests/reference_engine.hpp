#pragma once

// Deliberately naive re-implementation of the engine's specification:
// a flat vector of resting orders with explicit sequence numbers, linear scans
// for everything, and a brute-force auction over every integer price. It
// shares no data structures or algorithms with MatchingEngine, so identical
// event streams over millions of random commands are strong evidence that the
// optimised engine implements the spec.

#include <algorithm>
#include <cstdlib>
#include <optional>
#include <vector>

#include "ob/listeners.hpp"
#include "ob/matching_engine.hpp"

namespace ob::testing {

class ReferenceEngine {
 public:
  ReferenceEngine(const EngineConfig& cfg, RecordingListener& l) : cfg_(cfg), l_(l) {}

  Reason submit(const NewOrder& r) {
    Order o;
    o.id = r.id;
    o.side = r.side;
    o.type = r.type;
    o.tif = r.tif;
    o.owner = r.owner;
    o.post_only = r.post_only;
    o.price = r.type == OrderType::Market ? 0 : r.price;
    o.leaves = r.qty;
    if (o.type == OrderType::Market && (o.tif == TimeInForce::GTC || o.tif == TimeInForce::Day))
      o.tif = TimeInForce::IOC;
    const bool imm = immediate(o);

    Reason why = Reason::Ok;
    if (session_ == SessionState::Closed) why = Reason::SessionClosed;
    else if (session_ == SessionState::Halted) why = Reason::SessionHalted;
    else if (o.id == 0) why = Reason::InvalidId;
    else if (o.leaves == 0 || o.leaves > cfg_.max_order_qty) why = Reason::InvalidQty;
    else if (o.type == OrderType::Limit && !in_range(o.price)) why = Reason::InvalidPrice;
    else if (o.post_only && imm) why = Reason::InvalidFlags;
    else if (session_ == SessionState::PreOpen && imm) why = Reason::NotAllowedInAuction;
    else if (find(o.id, o.owner) >= 0) why = Reason::DuplicateId;
    else if (!imm && book_.size() >= cfg_.max_orders &&
             !(session_ == SessionState::Open && fillable(o, o.price)))
      why = Reason::BookFull;
    else if (o.post_only && session_ == SessionState::Open && would_cross(o.side, o.price))
      why = Reason::PostOnlyWouldCross;
    if (why != Reason::Ok) {
      emit(o, OrderState::Rejected, why);
      return why;
    }
    emit(o, OrderState::Active, Reason::Accepted);
    work(o);
    return Reason::Ok;
  }

  Reason cancel(OrderId id, OwnerId owner) {
    const long i = id ? find(id, owner) : -1;
    if (i < 0) {
      l_.on_reject(id, owner, Command::Cancel, Reason::UnknownOrder);
      return Reason::UnknownOrder;
    }
    Order o = take(i);
    emit(o, OrderState::Cancelled, Reason::UserCancel);
    return Reason::Ok;
  }

  Reason modify(OrderId id, Price np, Qty total, OwnerId owner) {
    const long i = id ? find(id, owner) : -1;
    Reason why = Reason::Ok;
    if (session_ == SessionState::Closed) why = Reason::SessionClosed;
    else if (session_ == SessionState::Halted) why = Reason::SessionHalted;
    else if (i < 0) why = Reason::UnknownOrder;
    else if (total == 0 || total > cfg_.max_order_qty) why = Reason::InvalidQty;
    else if (!in_range(np)) why = Reason::InvalidPrice;
    else if (at(i).post_only && session_ == SessionState::Open && would_cross(at(i).side, np))
      why = Reason::PostOnlyWouldCross;
    if (why != Reason::Ok) {
      l_.on_reject(id, owner, Command::Modify, why);
      return why;
    }
    if (total <= at(i).cum) {
      Order o = take(i);
      emit(o, OrderState::Cancelled, Reason::Replaced);
      return Reason::Ok;
    }
    const Qty leaves = total - at(i).cum;
    if (np == at(i).price && leaves <= at(i).leaves) {
      at(i).leaves = leaves;
      emit(at(i), at(i).state, Reason::Replaced);
      return Reason::Ok;
    }
    Order o = take(i);
    o.price = np;
    o.leaves = leaves;
    emit(o, o.state, Reason::Replaced);
    work(o);
    return Reason::Ok;
  }

  Reason set_session(SessionState to) {
    if (!can_transition(session_, to)) {
      l_.on_reject(0, 0, Command::Session, Reason::InvalidTransition);
      return Reason::InvalidTransition;
    }
    const SessionState from = session_;
    session_ = to;
    l_.on_session(from, to);
    if (from == SessionState::PreOpen && to == SessionState::Open) uncross();
    else if (to == SessionState::Closed) expire();
    return Reason::Ok;
  }

  std::optional<Price> best_bid() const { return best_px(Side::Buy); }
  std::optional<Price> best_ask() const { return best_px(Side::Sell); }

  // Resting orders of one side in priority order.
  std::vector<Order> side_in_priority(Side s) const {
    std::vector<const Resting*> v;
    for (const Resting& r : book_)
      if (r.o.side == s) v.push_back(&r);
    std::sort(v.begin(), v.end(), [](const Resting* a, const Resting* b) { return better(*a, *b); });
    std::vector<Order> out;
    for (const Resting* r : v) out.push_back(r->o);
    return out;
  }

 private:
  struct Resting {
    Order o;
    std::uint64_t seq;
  };

  static bool better(const Resting& a, const Resting& b) {
    if (a.o.price != b.o.price) return a.o.side == Side::Buy ? a.o.price > b.o.price : a.o.price < b.o.price;
    return a.seq < b.seq;
  }
  static bool immediate(const Order& o) {
    return o.type == OrderType::Market || o.tif == TimeInForce::IOC || o.tif == TimeInForce::FOK;
  }
  static bool crosses(Side taker, Price lp, Price limit) { return taker == Side::Buy ? lp <= limit : lp >= limit; }

  bool in_range(Price p) const { return p >= cfg_.min_price && p <= cfg_.max_price; }
  Order& at(long i) { return book_[std::size_t(i)].o; }

  long find(OrderId id, OwnerId owner) const {
    for (std::size_t i = 0; i < book_.size(); ++i)
      if (book_[i].o.id == id && book_[i].o.owner == owner) return long(i);
    return -1;
  }
  Order take(long i) {
    Order o = at(i);
    book_.erase(book_.begin() + i);
    return o;
  }

  long best_of(Side s) const {
    long best = -1;
    for (std::size_t i = 0; i < book_.size(); ++i)
      if (book_[i].o.side == s && (best < 0 || better(book_[i], book_[std::size_t(best)]))) best = long(i);
    return best;
  }
  std::optional<Price> best_px(Side s) const {
    const long i = best_of(s);
    return i < 0 ? std::nullopt : std::optional<Price>(book_[std::size_t(i)].o.price);
  }
  bool would_cross(Side s, Price p) const {
    const auto b = best_px(opposite(s));
    return b && crosses(s, *b, p);
  }

  void emit(Order& o, OrderState to, Reason why) {
    if (!can_transition(o.state, to)) std::abort();
    const OrderState from = o.state;
    o.state = to;
    l_.on_order(o, from, why);
  }
  void rest(const Order& o) { book_.push_back({o, seq_++}); }
  void trade(const Order& b, const Order& s, Price px, Qty q, Aggressor a) {
    l_.on_trade(Trade{++trade_seq_, b.id, s.id, b.owner, s.owner, px, q, a});
    last_ = px;
  }

  // Simulates matching without side effects.
  bool fillable(const Order& t, Price limit) const {
    Qty need = t.leaves;
    for (const Order& m : side_in_priority(opposite(t.side))) {
      if (!crosses(t.side, m.price, limit)) return false;
      if (t.owner != 0 && m.owner == t.owner) return false;
      if (m.leaves >= need) return true;
      need -= m.leaves;
    }
    return false;
  }

  Price limit_of(const Order& o) const {
    if (o.type == OrderType::Limit) return o.price;
    const bool buy = o.side == Side::Buy;
    const auto touch = best_px(opposite(o.side));
    if (cfg_.market_collar_ticks == 0 || !touch) return buy ? cfg_.max_price : cfg_.min_price;
    return buy ? std::min(cfg_.max_price, *touch + cfg_.market_collar_ticks)
               : std::max(cfg_.min_price, *touch - cfg_.market_collar_ticks);
  }

  void work(Order& o) {
    if (session_ == SessionState::PreOpen) return rest(o);
    const Price limit = limit_of(o);
    if (o.tif == TimeInForce::FOK && !fillable(o, limit))
      return emit(o, OrderState::Cancelled, Reason::FokUnfillable);
    bool stp = false;
    while (o.leaves > 0) {
      const long bi = best_of(opposite(o.side));
      if (bi < 0 || !crosses(o.side, at(bi).price, limit)) break;
      if (o.owner != 0 && at(bi).owner == o.owner) {
        stp = true;
        break;
      }
      Order& m = at(bi);
      const Qty q = std::min(o.leaves, m.leaves);
      m.leaves -= q;
      m.cum += q;
      o.leaves -= q;
      o.cum += q;
      if (o.side == Side::Buy) trade(o, m, m.price, q, Aggressor::Buy);
      else trade(m, o, m.price, q, Aggressor::Sell);
      settle(bi);
      emit(o, o.leaves == 0 ? OrderState::Filled : OrderState::PartiallyFilled, Reason::Fill);
    }
    if (o.leaves == 0) return;
    if (stp) emit(o, OrderState::Cancelled, Reason::SelfTradePrevented);
    else if (immediate(o)) emit(o, OrderState::Cancelled, Reason::IocRemainder);
    else rest(o);
  }

  void settle(long i) {
    if (at(i).leaves != 0) return emit(at(i), OrderState::PartiallyFilled, Reason::Fill);
    emit(at(i), OrderState::Filled, Reason::Fill);
    take(i);
  }

  void uncross() {
    const auto bb = best_bid(), ba = best_ask();
    if (!bb || !ba || *bb < *ba) return;
    const Price ref = last_ ? *last_ : *ba + (*bb - *ba) / 2;
    bool found = false;
    Qty best_exec = 0, best_imb = 0;
    Price best_dist = 0, px = 0;
    for (Price p = *ba; p <= *bb; ++p) {
      Qty bv = 0, av = 0;
      for (const Resting& r : book_) {
        if (r.o.side == Side::Buy && r.o.price >= p) bv += r.o.leaves;
        if (r.o.side == Side::Sell && r.o.price <= p) av += r.o.leaves;
      }
      const Qty exec = std::min(bv, av);
      const Qty imb = bv > av ? bv - av : av - bv;
      const Price dist = std::abs(p - ref);
      if (!found || exec > best_exec ||
          (exec == best_exec && (imb < best_imb || (imb == best_imb && dist < best_dist)))) {
        found = true;
        best_exec = exec;
        best_imb = imb;
        best_dist = dist;
        px = p;
      }
    }
    for (Qty rem = best_exec; rem > 0;) {
      const long bi = best_of(Side::Buy), ai = best_of(Side::Sell);
      const Qty q = std::min({rem, at(bi).leaves, at(ai).leaves});
      for (long i : {bi, ai}) {
        at(i).leaves -= q;
        at(i).cum += q;
      }
      rem -= q;
      trade(at(bi), at(ai), px, q, Aggressor::None);
      const OrderId aid = at(ai).id;
      const OwnerId aown = at(ai).owner;
      settle(bi);  // may erase, shifting indices
      settle(find(aid, aown));
    }
  }

  void expire() {
    std::vector<Order> doomed;
    for (Side s : {Side::Buy, Side::Sell})
      for (const Order& o : side_in_priority(s))
        if (o.tif == TimeInForce::Day) doomed.push_back(o);
    for (const Order& d : doomed) {
      Order o = take(find(d.id, d.owner));
      emit(o, OrderState::Expired, Reason::SessionEnd);
    }
  }

  EngineConfig cfg_;
  RecordingListener& l_;
  SessionState session_ = SessionState::Closed;
  std::vector<Resting> book_;
  std::uint64_t seq_ = 0;
  std::uint64_t trade_seq_ = 0;
  std::optional<Price> last_;
};

}  // namespace ob::testing
