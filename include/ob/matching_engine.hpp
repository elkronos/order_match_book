#pragma once

// Single-instrument, single-threaded, price-time priority matching engine.
//
// Hot-path properties:
//   * zero heap allocation after construction (pooled orders, fixed ladder,
//     fixed-capacity hash index);
//   * aggressors match from the stack: an order touches the pool and the id
//     index only if (and when) it comes to rest;
//   * O(1) add / cancel / reduce, near-O(1) best-price maintenance via a
//     two-level occupancy bitmap;
//   * events delivered through a statically-dispatched Listener (template
//     parameter), so a no-op listener compiles away entirely;
//   * fully deterministic output: no clocks, no randomness that reaches the
//     event stream, no iteration in hash order.
//
// Every order state change goes through transition(), which validates it
// against the OrderState table in state_machine.hpp and reports
// (order, from, reason) to the listener. Session changes are validated against
// the SessionState table the same way.
//
// Listener contract:
//   void on_order(const Order&, OrderState from, Reason why);   // order.state is the new state
//   void on_trade(const Trade&);
//   void on_reject(OrderId, OwnerId, Command, Reason);            // command rejected, no state change
//   void on_session(SessionState from, SessionState to);
// Callbacks run synchronously mid-command. They may call const queries (which
// observe the in-progress state) but must not issue commands: re-entry aborts.
// Commands are noexcept, so a throwing listener terminates the process rather
// than leaving a half-updated book (fail-stop).

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#include "ob/detail/level_bitmap.hpp"
#include "ob/detail/order_index.hpp"
#include "ob/detail/order_pool.hpp"
#include "ob/order.hpp"
#include "ob/state_machine.hpp"
#include "ob/types.hpp"

namespace ob {

struct EngineConfig {
  Price min_price = 1;                    // lowest valid limit price (ticks)
  Price max_price = 1'000'000;            // highest valid limit price (ticks); span <= 2^24
  std::uint32_t max_orders = 1u << 20;    // maximum simultaneously resting orders
  Qty max_order_qty = 1'000'000'000;      // per-order cap; keeps all quantity sums overflow-free
  Price market_collar_ticks = 0;          // market orders may trade at most this far through the
                                          // touch seen on arrival; 0 = unlimited
  std::uint64_t hash_seed = 0;            // id-index hash key; 0 = random (never affects output)
};

[[noreturn]] inline void fatal(const char* msg) noexcept {
  std::fprintf(stderr, "orderbook fatal: %s\n", msg);
  std::abort();
}

template <class Listener>
class MatchingEngine {
  static constexpr std::size_t npos = detail::LevelBitmap::npos;

 public:
  MatchingEngine(const EngineConfig& cfg, Listener& listener)
      : cfg_(validated(cfg)),
        listener_(listener),
        bids_(level_span(cfg_)),
        asks_(level_span(cfg_)),
        pool_(cfg_.max_orders),
        index_(cfg_.max_orders, cfg_.hash_seed ? cfg_.hash_seed : random_seed()) {}

  MatchingEngine(const MatchingEngine&) = delete;
  MatchingEngine& operator=(const MatchingEngine&) = delete;

  // ---------------------------------------------------------------- commands

  Reason submit(const NewOrder& req) noexcept {
    const CommandScope scope(*this);
    Order o;
    o.id = req.id;
    o.side = req.side;
    o.type = req.type;
    o.tif = req.tif;
    o.owner = req.owner;
    o.post_only = req.post_only;
    o.price = req.type == OrderType::Market ? 0 : req.price;
    o.leaves = req.qty;
    // A market order can never rest: GTC/Day degrade to IOC.
    if (o.type == OrderType::Market && (o.tif == TimeInForce::GTC || o.tif == TimeInForce::Day))
      o.tif = TimeInForce::IOC;

    if (const Reason why = validate_new(o); why != Reason::Ok) [[unlikely]] {
      transition(o, OrderState::Rejected, why);
      return why;
    }
    transition(o, OrderState::Active, Reason::Accepted);
    work(o);
    return Reason::Ok;
  }

  // Orders are addressed by (id, owner); another owner's order is reported as
  // UnknownOrder, indistinguishable from a non-existent one.
  Reason cancel(OrderId id, OwnerId owner = 0) noexcept {
    const CommandScope scope(*this);
    Order* o = id ? index_.find(id, owner) : nullptr;
    if (!o) [[unlikely]] {
      listener_.on_reject(id, owner, Command::Cancel, Reason::UnknownOrder);
      return Reason::UnknownOrder;
    }
    transition(*o, OrderState::Cancelled, Reason::UserCancel);
    unrest(o);
    return Reason::Ok;
  }

  // Cancel/replace with FIX semantics: new_qty is the new *total* order
  // quantity, so open quantity becomes new_qty - cum. This makes a size
  // reduction safe against fills racing the request: if new_qty <= cum the
  // order is simply done (Cancelled, reason Replaced), never over-exposed.
  // Shrinking open quantity at the same price amends in place and keeps time
  // priority; anything else pulls the order and re-runs it like a new one (it
  // may trade, and loses time priority) while keeping its fill history.
  Reason modify(OrderId id, Price new_price, Qty new_qty, OwnerId owner = 0) noexcept {
    const CommandScope scope(*this);
    Order* o = nullptr;
    Reason why = Reason::Ok;
    if (session_ == SessionState::Closed) why = Reason::SessionClosed;
    else if (session_ == SessionState::Halted) why = Reason::SessionHalted;
    else if (!(o = id ? index_.find(id, owner) : nullptr)) why = Reason::UnknownOrder;
    else if (new_qty == 0 || new_qty > cfg_.max_order_qty) why = Reason::InvalidQty;
    else if (!in_range(new_price)) why = Reason::InvalidPrice;
    else if (o->post_only && session_ == SessionState::Open && would_cross(o->side, new_price))
      why = Reason::PostOnlyWouldCross;
    if (why != Reason::Ok) [[unlikely]] {
      listener_.on_reject(id, owner, Command::Modify, why);
      return why;
    }

    if (new_qty <= o->cum) {
      transition(*o, OrderState::Cancelled, Reason::Replaced);
      unrest(o);
      return Reason::Ok;
    }
    const Qty leaves = new_qty - o->cum;
    if (new_price == o->price && leaves <= o->leaves) {
      side(o->side).levels[idx(o->price)].total -= o->leaves - leaves;
      o->leaves = leaves;
      transition(*o, o->state, Reason::Replaced);
      return Reason::Ok;
    }
    Order w = *o;
    unrest(o);  // frees a slot, so re-resting below cannot fail
    w.price = new_price;
    w.leaves = leaves;
    transition(w, w.state, Reason::Replaced);
    work(w);
    return Reason::Ok;
  }

  Reason set_session(SessionState to) noexcept {
    const CommandScope scope(*this);
    if (!can_transition(session_, to)) [[unlikely]] {
      listener_.on_reject(0, 0, Command::Session, Reason::InvalidTransition);
      return Reason::InvalidTransition;
    }
    const SessionState from = session_;
    session_ = to;
    listener_.on_session(from, to);
    if (from == SessionState::PreOpen && to == SessionState::Open) uncross();
    else if (to == SessionState::Closed) expire_day_orders();
    return Reason::Ok;
  }

  // ----------------------------------------------------------------- queries

  SessionState session() const noexcept { return session_; }
  const EngineConfig& config() const noexcept { return cfg_; }
  std::size_t resting_orders() const noexcept { return pool_.used(); }
  const Order* find(OrderId id, OwnerId owner = 0) const noexcept {
    return id ? index_.find(id, owner) : nullptr;
  }

  std::optional<Price> best_bid() const noexcept { return best_price(bids_); }
  std::optional<Price> best_ask() const noexcept { return best_price(asks_); }
  std::optional<Price> last_trade_price() const noexcept {
    return has_last_trade_ ? std::optional<Price>(last_trade_) : std::nullopt;
  }

  Qty level_qty(Side s, Price p) const noexcept {
    return in_range(p) ? side(s).levels[idx(p)].total : 0;
  }
  std::uint32_t level_count(Side s, Price p) const noexcept {
    return in_range(p) ? side(s).levels[idx(p)].count : 0;
  }

  // fn(Price, Qty total, uint32_t count), best price first.
  template <class Fn>
  void for_each_level(Side s, std::size_t max_levels, Fn&& fn) const {
    const BookSide& bs = side(s);
    std::size_t n = 0;
    for (std::size_t i = bs.best; i != npos && n < max_levels; i = next_worse(s, i), ++n)
      fn(price_of(i), bs.levels[i].total, bs.levels[i].count);
  }

  // fn(const Order&), in time priority.
  template <class Fn>
  void for_each_order_at(Side s, Price p, Fn&& fn) const {
    if (!in_range(p)) return;
    for (const Order* o = side(s).levels[idx(p)].head; o; o = o->next) fn(*o);
  }

  // Full O(levels + orders) structural audit. Returns an empty string if the
  // book is consistent, otherwise a description of the first violation.
  std::string check_invariants() const {
    std::size_t seen = 0;
    for (Side s : {Side::Buy, Side::Sell}) {
      const BookSide& bs = side(s);
      std::size_t expect_best = npos;
      for (std::size_t i = 0; i < bs.levels.size(); ++i) {
        const Level& lv = bs.levels[i];
        const std::string where =
            std::string(to_string(s)) + " level " + std::to_string(price_of(i)) + ": ";
        if (bs.occupied.test(i) != (lv.count != 0)) return where + "bitmap/count mismatch";
        if (lv.count == 0) {
          if (lv.head || lv.tail || lv.total) return where + "empty level has residue";
          continue;
        }
        if (s == Side::Buy || expect_best == npos) expect_best = i;
        Qty total = 0;
        std::uint32_t count = 0;
        const Order* prev = nullptr;
        for (const Order* o = lv.head; o; prev = o, o = o->next) {
          if (o->prev != prev) return where + "broken prev link";
          if (o->side != s || o->price != price_of(i)) return where + "order on wrong level";
          if (o->state != OrderState::Active && o->state != OrderState::PartiallyFilled)
            return where + "resting order in state " + std::string(to_string(o->state));
          if (o->leaves == 0 || o->leaves > cfg_.max_order_qty) return where + "resting leaves out of range";
          if ((o->state == OrderState::PartiallyFilled) != (o->cum != 0))
            return where + "state disagrees with fill history";
          if (o->type != OrderType::Limit || o->tif == TimeInForce::IOC || o->tif == TimeInForce::FOK)
            return where + "non-restable order resting";
          if (index_.find(o->id, o->owner) != o) return where + "order missing from index";
          total += o->leaves;
          if (++count > lv.count) return where + "list longer than count";
        }
        if (prev != lv.tail) return where + "tail mismatch";
        if (count != lv.count) return where + "count mismatch";
        if (total != lv.total) return where + "total mismatch";
        seen += count;
      }
      if (bs.best != expect_best) return std::string(to_string(s)) + ": stale best level";
    }
    if (seen != pool_.used()) return "pool usage != resting orders";
    if (seen != index_.size()) return "index size != resting orders";
    // A crossed book is legal only while no matching can occur and the next
    // route to Open passes through an auction: PreOpen, or Closed reached
    // from PreOpen (Closed -> Open is not a legal session transition).
    const bool may_cross = session_ == SessionState::PreOpen || session_ == SessionState::Closed;
    if (!may_cross && bids_.best != npos && asks_.best != npos && bids_.best >= asks_.best)
      return "book crossed during continuous trading";
    return {};
  }

 private:
  struct Level {
    Order* head = nullptr;
    Order* tail = nullptr;
    Qty total = 0;
    std::uint32_t count = 0;
  };

  struct BookSide {
    explicit BookSide(std::size_t n) : levels(n), occupied(n) {}
    std::vector<Level> levels;
    detail::LevelBitmap occupied;
    std::size_t best = npos;  // bids: highest occupied index; asks: lowest
  };

  // Fail-stop guard against a listener issuing commands from a callback.
  struct CommandScope {
    explicit CommandScope(MatchingEngine& e) noexcept : engine(e) {
      if (engine.in_command_) [[unlikely]] fatal("re-entrant command issued from a listener callback");
      engine.in_command_ = true;
    }
    ~CommandScope() { engine.in_command_ = false; }
    CommandScope(const CommandScope&) = delete;
    CommandScope& operator=(const CommandScope&) = delete;
    MatchingEngine& engine;
  };

  static constexpr std::uint64_t kMaxLevels = std::uint64_t{1} << 24;

  // Rejects configurations whose arithmetic could overflow: the price span is
  // computed without signed overflow, and the largest possible level total
  // (every resting order at max quantity on one level) must fit in 63 bits,
  // which also bounds every auction volume and FOK liquidity sum.
  static EngineConfig validated(const EngineConfig& c) {
    if (c.max_price < c.min_price) throw std::invalid_argument("max_price < min_price");
    if (std::uint64_t(c.max_price) - std::uint64_t(c.min_price) >= kMaxLevels)
      throw std::invalid_argument("price ladder wider than 2^24 ticks");
    if (c.max_orders == 0 || c.max_order_qty == 0)
      throw std::invalid_argument("max_orders and max_order_qty must be > 0");
    if (c.max_order_qty > (std::uint64_t{1} << 63) / c.max_orders)
      throw std::invalid_argument("max_orders * max_order_qty must stay below 2^63");
    if (c.market_collar_ticks < 0 || std::uint64_t(c.market_collar_ticks) > kMaxLevels)
      throw std::invalid_argument("market_collar_ticks must be in [0, 2^24]");
    return c;
  }
  static std::size_t level_span(const EngineConfig& c) {
    return std::size_t(std::uint64_t(c.max_price) - std::uint64_t(c.min_price)) + 1;
  }
  static std::uint64_t random_seed() {
    std::random_device rd;
    return (std::uint64_t(rd()) << 32 | rd()) | 1;
  }

  BookSide& side(Side s) noexcept { return s == Side::Buy ? bids_ : asks_; }
  const BookSide& side(Side s) const noexcept { return s == Side::Buy ? bids_ : asks_; }

  bool in_range(Price p) const noexcept { return p >= cfg_.min_price && p <= cfg_.max_price; }
  std::size_t idx(Price p) const noexcept { return std::size_t(p - cfg_.min_price); }
  Price price_of(std::size_t i) const noexcept { return cfg_.min_price + Price(i); }

  std::optional<Price> best_price(const BookSide& bs) const noexcept {
    return bs.best == npos ? std::nullopt : std::optional<Price>(price_of(bs.best));
  }

  // Next occupied level strictly worse than i on side s.
  std::size_t next_worse(Side s, std::size_t i) const noexcept {
    const BookSide& bs = side(s);
    if (s == Side::Buy) return i == 0 ? npos : bs.occupied.prev_at_or_before(i - 1);
    return bs.occupied.next_at_or_after(i + 1);
  }

  static bool crosses(Side taker, Price level_price, Price limit) noexcept {
    return taker == Side::Buy ? level_price <= limit : level_price >= limit;
  }

  bool would_cross(Side s, Price p) const noexcept {
    const BookSide& opp = side(opposite(s));
    return opp.best != npos && crosses(s, price_of(opp.best), p);
  }

  static bool immediate(const Order& o) noexcept {
    return o.type == OrderType::Market || o.tif == TimeInForce::IOC || o.tif == TimeInForce::FOK;
  }

  Reason validate_new(const Order& o) const noexcept {
    if (session_ == SessionState::Closed) return Reason::SessionClosed;
    if (session_ == SessionState::Halted) return Reason::SessionHalted;
    if (o.id == 0) return Reason::InvalidId;
    if (o.leaves == 0 || o.leaves > cfg_.max_order_qty) return Reason::InvalidQty;
    if (o.type == OrderType::Limit && !in_range(o.price)) return Reason::InvalidPrice;
    if (o.post_only && immediate(o)) return Reason::InvalidFlags;
    if (session_ == SessionState::PreOpen && immediate(o)) return Reason::NotAllowedInAuction;
    if (index_.find(o.id, o.owner)) return Reason::DuplicateId;
    // Only an order that will come to rest needs a slot. IOC/FOK/market orders
    // never do, and on a full book a limit order is still accepted if it is
    // certain to fill completely (rare path), so liquidity can always be taken.
    if (!immediate(o) && pool_.full() &&
        !(session_ == SessionState::Open && fok_fillable(o, o.price)))
      return Reason::BookFull;
    if (o.post_only && session_ == SessionState::Open && would_cross(o.side, o.price))
      return Reason::PostOnlyWouldCross;
    return Reason::Ok;
  }

  void transition(Order& o, OrderState to, Reason why) noexcept {
    if (!can_transition(o.state, to)) [[unlikely]] fatal("illegal order state transition");
    const OrderState from = o.state;
    o.state = to;
    listener_.on_order(o, from, why);
  }

  // Run a working (Active / PartiallyFilled) order that is not in the book:
  // a new order, or one pulled by a cancel/replace.
  void work(Order& o) noexcept {
    if (session_ == SessionState::PreOpen) return rest(o);
    const Price limit = o.type == OrderType::Limit ? o.price : market_limit(o.side);
    if (o.tif == TimeInForce::FOK && !fok_fillable(o, limit))
      return transition(o, OrderState::Cancelled, Reason::FokUnfillable);
    const bool stopped_by_stp = match(o, limit);
    if (o.leaves == 0) return;  // Filled
    if (stopped_by_stp) return transition(o, OrderState::Cancelled, Reason::SelfTradePrevented);
    if (immediate(o)) return transition(o, OrderState::Cancelled, Reason::IocRemainder);
    rest(o);
  }

  // Effective limit of a market order: the far end of the ladder, or with a
  // collar, a fixed distance through the opposite touch at arrival.
  Price market_limit(Side s) const noexcept {
    const bool buy = s == Side::Buy;
    const BookSide& opp = side(opposite(s));
    if (cfg_.market_collar_ticks == 0 || opp.best == npos) return buy ? cfg_.max_price : cfg_.min_price;
    const Price touch = price_of(opp.best);
    return buy ? std::min(cfg_.max_price, touch + cfg_.market_collar_ticks)
               : std::max(cfg_.min_price, touch - cfg_.market_collar_ticks);
  }

  // Copy into a pool slot and queue at the back of its level. A slot is
  // guaranteed: validate_new checked capacity, and matching only frees slots.
  void rest(const Order& src) noexcept {
    Order* o = pool_.acquire();
    *o = src;
    index_.insert(o);
    BookSide& bs = side(o->side);
    const std::size_t i = idx(o->price);
    Level& lv = bs.levels[i];
    o->next = nullptr;
    o->prev = lv.tail;
    (lv.tail ? lv.tail->next : lv.head) = o;
    lv.tail = o;
    lv.total += o->leaves;
    if (++lv.count == 1) {
      bs.occupied.set(i);
      if (bs.best == npos || (o->side == Side::Buy ? i > bs.best : i < bs.best)) bs.best = i;
    }
  }

  // Unlink from the book and return the slot; `o` is dead afterwards.
  void unrest(Order* o) noexcept {
    BookSide& bs = side(o->side);
    const std::size_t i = idx(o->price);
    Level& lv = bs.levels[i];
    (o->prev ? o->prev->next : lv.head) = o->next;
    (o->next ? o->next->prev : lv.tail) = o->prev;
    lv.total -= o->leaves;
    if (--lv.count == 0) {
      bs.occupied.clear(i);
      if (bs.best == i) bs.best = next_worse(o->side, i);
    }
    index_.erase(o->id, o->owner);
    pool_.release(o);
  }

  // Apply a fill to a resting order (level total already adjusted).
  void fill_resting(Order& m) noexcept {
    if (m.leaves != 0) return transition(m, OrderState::PartiallyFilled, Reason::Fill);
    transition(m, OrderState::Filled, Reason::Fill);
    unrest(&m);
  }

  void print(const Order& buy, const Order& sell, Price px, Qty qty, Aggressor aggr) noexcept {
    last_trade_ = px;
    has_last_trade_ = true;
    listener_.on_trade(Trade{++trade_seq_, buy.id, sell.id, buy.owner, sell.owner, px, qty, aggr});
  }

  // Continuous matching of an aggressor against the opposite side.
  // Returns true if matching stopped because of self-trade prevention.
  bool match(Order& t, Price limit) noexcept {
    BookSide& opp = side(opposite(t.side));
    const bool buy = t.side == Side::Buy;
    while (t.leaves != 0 && opp.best != npos) {
      const Price lp = price_of(opp.best);
      if (!crosses(t.side, lp, limit)) break;
      Level& lv = opp.levels[opp.best];
      do {
        Order& m = *lv.head;
        if (t.owner != 0 && m.owner == t.owner) return true;  // STP: cancel newest
        const Qty q = std::min(t.leaves, m.leaves);
        lv.total -= q;
        m.leaves -= q;
        m.cum += q;
        t.leaves -= q;
        t.cum += q;
        print(buy ? t : m, buy ? m : t, lp, q, buy ? Aggressor::Buy : Aggressor::Sell);
        fill_resting(m);
        transition(t, t.leaves == 0 ? OrderState::Filled : OrderState::PartiallyFilled, Reason::Fill);
      } while (t.leaves != 0 && lv.head);
    }
    return false;
  }

  // Can the order be filled completely right now, honouring STP?
  // Pass 1 sums level totals (O(levels)) and fails fast on thin liquidity.
  // Only if enough liquidity exists, and STP applies, does pass 2 walk the
  // individual orders that would be consumed, looking for one of the owner's
  // own. A rejected FOK therefore never costs more than the orders a
  // successful one would have filled.
  bool fok_fillable(const Order& t, Price limit) const noexcept {
    const Side os = opposite(t.side);
    const BookSide& opp = side(os);
    Qty need = t.leaves;
    std::size_t last = npos;  // deepest level the fill would reach
    for (std::size_t i = opp.best; i != npos; i = next_worse(os, i)) {
      if (!crosses(t.side, price_of(i), limit)) return false;
      if (opp.levels[i].total >= need) {
        last = i;
        break;
      }
      need -= opp.levels[i].total;
    }
    if (last == npos) return false;
    if (t.owner == 0) return true;
    need = t.leaves;
    for (std::size_t i = opp.best;; i = next_worse(os, i)) {
      for (const Order* m = opp.levels[i].head; m; m = m->next) {
        if (m->owner == t.owner) return false;  // matching would stop here
        if (m->leaves >= need) return true;
        need -= m->leaves;
      }
      if (i == last) return false;  // unreachable: pass 1 proved sufficiency
    }
  }

  // Opening / re-opening auction. Every price in the crossed range
  // [best ask, best bid] is a candidate. Executable volume is constant on each
  // occupied level and on each gap between consecutive occupied levels, so the
  // range is scanned as those segments (no allocation, O(levels in range)).
  // Selection: max executable volume, then min imbalance, then closest to the
  // reference price (last trade, else mid of the crossed range), then lowest.
  void uncross() noexcept {
    if (bids_.best == npos || asks_.best == npos || bids_.best < asks_.best) return;
    const std::size_t lo = asks_.best, hi = bids_.best;
    const Price ref = has_last_trade_ ? last_trade_ : price_of(lo) + (price_of(hi) - price_of(lo)) / 2;

    Qty bid_total = 0;  // all bids priced inside [lo, hi]
    for (std::size_t i = bids_.occupied.next_at_or_after(lo); i <= hi; i = bids_.occupied.next_at_or_after(i + 1))
      bid_total += bids_.levels[i].total;

    struct Choice {
      Qty exec = 0, imbalance = 0;
      Price dist = 0, price = 0;
    } best;
    auto consider = [&](Price a, Price b, Qty bid_vol, Qty ask_vol) {  // segment [a, b]
      const Price p = std::clamp(ref, a, b);
      const Choice c{std::min(bid_vol, ask_vol), bid_vol > ask_vol ? bid_vol - ask_vol : ask_vol - bid_vol,
                     p > ref ? p - ref : ref - p, p};
      if (c.exec > best.exec ||
          (c.exec == best.exec && (c.imbalance < best.imbalance ||
                                   (c.imbalance == best.imbalance && c.dist < best.dist))))
        best = c;
    };

    Qty asks_at_or_below = 0, bids_below = 0;
    for (std::size_t i = lo; i <= hi;) {
      asks_at_or_below += asks_.levels[i].total;
      consider(price_of(i), price_of(i), bid_total - bids_below, asks_at_or_below);
      bids_below += bids_.levels[i].total;
      const std::size_t next =
          std::min(bids_.occupied.next_at_or_after(i + 1), asks_.occupied.next_at_or_after(i + 1));
      if (i < hi && next > i + 1)  // open gap (i, next): bids >= next vs asks <= i
        consider(price_of(i + 1), price_of(next - 1), bid_total - bids_below, asks_at_or_below);
      i = next;
    }
    if (best.exec == 0) return;

    for (Qty remaining = best.exec; remaining != 0;) {
      Level& bl = bids_.levels[bids_.best];
      Level& al = asks_.levels[asks_.best];
      Order& b = *bl.head;
      Order& a = *al.head;
      const Qty q = std::min({remaining, b.leaves, a.leaves});
      bl.total -= q;
      al.total -= q;
      b.leaves -= q;
      b.cum += q;
      a.leaves -= q;
      a.cum += q;
      remaining -= q;
      print(b, a, best.price, q, Aggressor::None);
      fill_resting(b);
      fill_resting(a);
    }
  }

  // Session close: Day orders expire (bids best→worst, then asks), GTC stay.
  void expire_day_orders() noexcept {
    for (Side s : {Side::Buy, Side::Sell}) {
      BookSide& bs = side(s);
      for (std::size_t i = bs.best; i != npos;) {
        const std::size_t next = next_worse(s, i);
        for (Order* o = bs.levels[i].head; o;) {
          Order* following = o->next;
          if (o->tif == TimeInForce::Day) {
            transition(*o, OrderState::Expired, Reason::SessionEnd);
            unrest(o);
          }
          o = following;
        }
        i = next;
      }
    }
  }

  EngineConfig cfg_;
  Listener& listener_;
  SessionState session_ = SessionState::Closed;
  bool in_command_ = false;
  bool has_last_trade_ = false;
  BookSide bids_;
  BookSide asks_;
  detail::OrderPool pool_;
  detail::OrderIndex index_;
  std::uint64_t trade_seq_ = 0;
  Price last_trade_ = 0;
};

}  // namespace ob
