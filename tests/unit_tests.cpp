#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <csignal>
#include <random>
#include <map>

#include "ob/listeners.hpp"
#include "ob/matching_engine.hpp"
#include "test_framework.hpp"

using namespace ob;
using Engine = MatchingEngine<RecordingListener>;

namespace {

EngineConfig cfg(Price lo, Price hi, std::uint32_t max_orders) {
  EngineConfig c;
  c.min_price = lo;
  c.max_price = hi;
  c.max_orders = max_orders;
  return c;
}

// Runs fn in a forked child; true if the child died from SIGABRT.
template <class Fn>
bool aborts(Fn fn) {
  const pid_t pid = fork();
  if (pid == 0) {
    [[maybe_unused]] FILE* quiet = std::freopen("/dev/null", "w", stderr);
    fn();
    _exit(0);
  }
  int status = 0;
  waitpid(pid, &status, 0);
  return WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT;
}

struct Fx {
  RecordingListener rec;
  Engine eng;
  explicit Fx(EngineConfig c = cfg(1, 1000, 1024), bool open = true) : eng(c, rec) {
    if (open) {
      eng.set_session(SessionState::PreOpen);
      eng.set_session(SessionState::Open);
    }
    rec.events.clear();
  }
  std::vector<Trade> trades() const {
    std::vector<Trade> t;
    for (const Event& e : rec.events)
      if (e.kind == Event::Kind::Trade) t.push_back(e.trade);
    return t;
  }
  // State transitions recorded for one order id, as "from->to" pairs.
  std::vector<std::pair<OrderState, OrderState>> path(OrderId id) const {
    std::vector<std::pair<OrderState, OrderState>> p;
    for (const Event& e : rec.events)
      if (e.kind == Event::Kind::Order && e.id == id) p.emplace_back(e.from, e.to);
    return p;
  }
  OrderState final_state(OrderId id) const {
    OrderState s = OrderState::PendingNew;
    for (const Event& e : rec.events)
      if (e.kind == Event::Kind::Order && e.id == id) s = e.to;
    return s;
  }
  Reason last_reason(OrderId id) const {
    Reason r = Reason::Ok;
    for (const Event& e : rec.events)
      if ((e.kind == Event::Kind::Order || e.kind == Event::Kind::Reject) && e.id == id) r = e.reason;
    return r;
  }
  bool ok() const { return eng.check_invariants().empty(); }
};

using S = OrderState;
constexpr auto Buy = Side::Buy;
constexpr auto Sell = Side::Sell;

}  // namespace

// --------------------------------------------------------------- state tables

TEST(order_state_table_matches_lifecycle) {
  CHECK(can_transition(S::PendingNew, S::Active));
  CHECK(can_transition(S::PendingNew, S::Rejected));
  CHECK(!can_transition(S::PendingNew, S::Filled));
  CHECK(can_transition(S::Active, S::PartiallyFilled));
  CHECK(can_transition(S::PartiallyFilled, S::PartiallyFilled));
  CHECK(!can_transition(S::PartiallyFilled, S::Active));
  for (S t : {S::Filled, S::Cancelled, S::Rejected, S::Expired}) {
    CHECK(is_terminal(t));
    for (int to = 0; to < int(kOrderStateCount); ++to) CHECK(!can_transition(t, S(to)));
  }
}

TEST(session_state_table) {
  using SS = SessionState;
  CHECK(can_transition(SS::Closed, SS::PreOpen));
  CHECK(!can_transition(SS::Closed, SS::Open));
  CHECK(can_transition(SS::PreOpen, SS::Open));
  CHECK(can_transition(SS::Open, SS::Halted));
  CHECK(can_transition(SS::Halted, SS::PreOpen));
  CHECK(!can_transition(SS::Open, SS::PreOpen));
  CHECK(!can_transition(SS::Open, SS::Open));
}

// ------------------------------------------------------------ basic matching

TEST(resting_orders_set_best_prices) {
  Fx f;
  CHECK_EQ(f.eng.submit(limit_order(1, Buy, 99, 10)), Reason::Ok);
  CHECK_EQ(f.eng.submit(limit_order(2, Buy, 98, 5)), Reason::Ok);
  CHECK_EQ(f.eng.submit(limit_order(3, Sell, 101, 7)), Reason::Ok);
  CHECK_EQ(f.eng.best_bid(), std::optional<Price>(99));
  CHECK_EQ(f.eng.best_ask(), std::optional<Price>(101));
  CHECK_EQ(f.eng.resting_orders(), std::size_t(3));
  CHECK_EQ(f.eng.level_qty(Buy, 99), Qty(10));
  CHECK(f.trades().empty());
  CHECK(f.ok());
}

TEST(price_then_time_priority_and_maker_price) {
  Fx f;
  f.eng.submit(limit_order(1, Sell, 101, 5));
  f.eng.submit(limit_order(2, Sell, 101, 5));
  f.eng.submit(limit_order(3, Sell, 100, 5));
  f.eng.submit(limit_order(4, Buy, 101, 12));
  const auto t = f.trades();
  CHECK_EQ(t.size(), std::size_t(3));
  CHECK_EQ(t[0].sell_id, OrderId(3));
  CHECK_EQ(t[0].price, Price(100));  // price improvement for the taker
  CHECK_EQ(t[1].sell_id, OrderId(1));
  CHECK_EQ(t[2].sell_id, OrderId(2));
  CHECK_EQ(t[2].qty, Qty(2));
  CHECK_EQ(t[0].aggressor, Aggressor::Buy);
  CHECK_EQ(f.final_state(4), S::Filled);
  CHECK_EQ(f.final_state(2), S::PartiallyFilled);
  CHECK_EQ(f.eng.find(2)->leaves, Qty(3));
  CHECK(f.ok());
}

TEST(partial_fill_state_path) {
  Fx f;
  f.eng.submit(limit_order(1, Sell, 100, 10));
  f.eng.submit(limit_order(2, Buy, 100, 4));
  f.eng.submit(limit_order(3, Buy, 100, 6));
  const auto p = f.path(1);
  CHECK_EQ(p.size(), std::size_t(3));
  CHECK(p[0] == std::make_pair(S::PendingNew, S::Active));
  CHECK(p[1] == std::make_pair(S::Active, S::PartiallyFilled));
  CHECK(p[2] == std::make_pair(S::PartiallyFilled, S::Filled));
  CHECK_EQ(f.eng.resting_orders(), std::size_t(0));
  CHECK_EQ(f.eng.best_ask(), std::optional<Price>());
  CHECK(f.ok());
}

TEST(aggressor_rests_remainder) {
  Fx f;
  f.eng.submit(limit_order(1, Sell, 100, 3));
  f.eng.submit(limit_order(2, Buy, 101, 10));
  CHECK_EQ(f.final_state(2), S::PartiallyFilled);
  CHECK_EQ(f.eng.best_bid(), std::optional<Price>(101));
  CHECK_EQ(f.eng.level_qty(Buy, 101), Qty(7));
  CHECK(f.ok());
}

// ------------------------------------------------------------- order types

TEST(ioc_remainder_is_cancelled) {
  Fx f;
  f.eng.submit(limit_order(1, Sell, 100, 3));
  f.eng.submit(limit_order(2, Buy, 100, 10, TimeInForce::IOC));
  CHECK_EQ(f.trades().size(), std::size_t(1));
  CHECK_EQ(f.final_state(2), S::Cancelled);
  CHECK_EQ(f.last_reason(2), Reason::IocRemainder);
  CHECK_EQ(f.eng.resting_orders(), std::size_t(0));
  CHECK(f.ok());
}

TEST(fok_is_all_or_nothing) {
  Fx f;
  f.eng.submit(limit_order(1, Sell, 100, 5));
  f.eng.submit(limit_order(2, Sell, 101, 5));
  f.eng.submit(limit_order(3, Sell, 102, 5));
  f.eng.submit(limit_order(4, Buy, 101, 11, TimeInForce::FOK));  // only 10 available <= 101
  CHECK(f.trades().empty());
  CHECK_EQ(f.final_state(4), S::Cancelled);
  CHECK_EQ(f.last_reason(4), Reason::FokUnfillable);
  CHECK_EQ(f.eng.resting_orders(), std::size_t(3));
  f.eng.submit(limit_order(5, Buy, 102, 11, TimeInForce::FOK));
  CHECK_EQ(f.trades().size(), std::size_t(3));
  CHECK_EQ(f.final_state(5), S::Filled);
  CHECK_EQ(f.eng.level_qty(Sell, 102), Qty(4));
  CHECK(f.ok());
}

TEST(market_order_sweeps_and_never_rests) {
  Fx f;
  f.eng.submit(limit_order(1, Buy, 99, 5));
  f.eng.submit(limit_order(2, Buy, 97, 5));
  f.eng.submit(market_order(3, Sell, 20, TimeInForce::GTC));  // GTC degrades to IOC
  const auto t = f.trades();
  CHECK_EQ(t.size(), std::size_t(2));
  CHECK_EQ(t[0].price, Price(99));
  CHECK_EQ(t[1].price, Price(97));
  CHECK_EQ(t[1].aggressor, Aggressor::Sell);
  CHECK_EQ(f.final_state(3), S::Cancelled);
  CHECK_EQ(f.last_reason(3), Reason::IocRemainder);
  CHECK_EQ(f.eng.best_bid(), std::optional<Price>());
  CHECK(f.ok());
}

TEST(market_order_on_empty_book_is_cancelled) {
  Fx f;
  f.eng.submit(market_order(1, Buy, 5));
  CHECK_EQ(f.final_state(1), S::Cancelled);
  CHECK(f.ok());
}

TEST(post_only_rejected_if_crossing_else_rests) {
  Fx f;
  f.eng.submit(limit_order(1, Sell, 100, 5));
  CHECK_EQ(f.eng.submit(limit_order(2, Buy, 100, 5, TimeInForce::GTC, 0, true)), Reason::PostOnlyWouldCross);
  CHECK_EQ(f.final_state(2), S::Rejected);
  CHECK_EQ(f.eng.submit(limit_order(3, Buy, 99, 5, TimeInForce::GTC, 0, true)), Reason::Ok);
  CHECK_EQ(f.eng.best_bid(), std::optional<Price>(99));
  CHECK_EQ(f.eng.submit(limit_order(4, Buy, 99, 5, TimeInForce::IOC, 0, true)), Reason::InvalidFlags);
  CHECK(f.trades().empty());
  CHECK(f.ok());
}

TEST(self_trade_prevention_cancels_aggressor) {
  Fx f;
  f.eng.submit(limit_order(1, Sell, 100, 5, TimeInForce::GTC, /*owner*/ 9));
  f.eng.submit(limit_order(2, Sell, 100, 5, TimeInForce::GTC, /*owner*/ 7));
  f.eng.submit(limit_order(3, Buy, 100, 8, TimeInForce::GTC, /*owner*/ 7));
  const auto t = f.trades();
  CHECK_EQ(t.size(), std::size_t(1));  // trades with owner 9, then hits own order
  CHECK_EQ(t[0].sell_id, OrderId(1));
  CHECK_EQ(f.final_state(3), S::Cancelled);
  CHECK_EQ(f.last_reason(3), Reason::SelfTradePrevented);
  CHECK_EQ(f.eng.find(2, 7)->leaves, Qty(5));  // resting own order untouched
  // Anonymous orders (owner 0) never trigger STP: this one trades with owner 7.
  f.eng.submit(limit_order(5, Buy, 100, 1));
  CHECK_EQ(f.final_state(5), S::Filled);
  CHECK_EQ(f.trades().back().sell_id, OrderId(2));
  CHECK(f.ok());
}

TEST(fok_respects_self_trade_prevention) {
  Fx f;
  f.eng.submit(limit_order(1, Sell, 100, 5, TimeInForce::GTC, 7));
  f.eng.submit(limit_order(2, Sell, 100, 5, TimeInForce::GTC, 8));
  f.eng.submit(limit_order(3, Buy, 100, 3, TimeInForce::FOK, 7));
  CHECK(f.trades().empty());
  CHECK_EQ(f.last_reason(3), Reason::FokUnfillable);
  CHECK(f.ok());
}

// ------------------------------------------------------- cancel and modify

TEST(cancel_and_unknown_cancel) {
  Fx f;
  f.eng.submit(limit_order(1, Buy, 100, 5));
  CHECK_EQ(f.eng.cancel(1), Reason::Ok);
  CHECK_EQ(f.final_state(1), S::Cancelled);
  CHECK_EQ(f.eng.cancel(1), Reason::UnknownOrder);
  CHECK(f.rec.events.back().kind == Event::Kind::Reject);
  CHECK_EQ(f.eng.best_bid(), std::optional<Price>());
  CHECK(f.ok());
}

TEST(modify_reduce_keeps_priority) {
  Fx f;
  f.eng.submit(limit_order(1, Buy, 100, 10));
  f.eng.submit(limit_order(2, Buy, 100, 10));
  CHECK_EQ(f.eng.modify(1, 100, 4), Reason::Ok);
  CHECK_EQ(f.eng.level_qty(Buy, 100), Qty(14));
  f.eng.submit(limit_order(3, Sell, 100, 4));
  CHECK_EQ(f.trades()[0].buy_id, OrderId(1));
  CHECK(f.ok());
}

TEST(modify_increase_loses_priority) {
  Fx f;
  f.eng.submit(limit_order(1, Buy, 100, 10));
  f.eng.submit(limit_order(2, Buy, 100, 10));
  CHECK_EQ(f.eng.modify(1, 100, 11), Reason::Ok);
  f.eng.submit(limit_order(3, Sell, 100, 4));
  CHECK_EQ(f.trades()[0].buy_id, OrderId(2));
  CHECK(f.ok());
}

TEST(modify_price_can_cross_and_trade) {
  Fx f;
  f.eng.submit(limit_order(1, Sell, 102, 5));
  f.eng.submit(limit_order(2, Buy, 100, 8));
  CHECK_EQ(f.eng.modify(2, 102, 8), Reason::Ok);
  const auto t = f.trades();
  CHECK_EQ(t.size(), std::size_t(1));
  CHECK_EQ(t[0].qty, Qty(5));
  CHECK_EQ(f.eng.best_bid(), std::optional<Price>(102));
  CHECK_EQ(f.eng.find(2)->state, S::PartiallyFilled);
  CHECK(f.ok());
}

TEST(modify_quantity_is_total_order_size) {
  // FIX semantics: a replace's quantity includes what has already filled.
  Fx f;
  f.eng.submit(limit_order(1, Sell, 100, 100));
  f.eng.submit(limit_order(2, Buy, 100, 60));  // 60 filled, 40 open
  CHECK_EQ(f.eng.modify(1, 100, 80), Reason::Ok);
  CHECK_EQ(f.eng.find(1)->leaves, Qty(20));  // not 80: total exposure stays 80
  CHECK_EQ(f.eng.find(1)->state, S::PartiallyFilled);
  CHECK_EQ(f.eng.modify(1, 100, 50), Reason::Ok);  // below what already filled
  CHECK_EQ(f.final_state(1), S::Cancelled);
  CHECK_EQ(f.last_reason(1), Reason::Replaced);
  CHECK(f.eng.find(1) == nullptr);
  CHECK(f.ok());
}

TEST(order_ids_are_scoped_per_owner) {
  Fx f;
  CHECK_EQ(f.eng.submit(limit_order(5, Buy, 100, 1, TimeInForce::GTC, 1)), Reason::Ok);
  CHECK_EQ(f.eng.submit(limit_order(5, Buy, 99, 1, TimeInForce::GTC, 2)), Reason::Ok);  // no collision
  CHECK_EQ(f.eng.cancel(5, 3), Reason::UnknownOrder);  // cannot touch others' orders
  CHECK_EQ(f.eng.modify(5, 98, 1, 3), Reason::UnknownOrder);
  CHECK_EQ(f.eng.cancel(5, 2), Reason::Ok);
  CHECK_EQ(f.eng.find(5, 1)->price, Price(100));
  f.eng.submit(limit_order(5, Sell, 100, 1, TimeInForce::GTC, 2));
  const Trade t = f.trades().back();
  CHECK_EQ(t.buy_owner, OwnerId(1));
  CHECK_EQ(t.sell_owner, OwnerId(2));
  CHECK(f.ok());
}

TEST(market_collar_limits_sweep) {
  EngineConfig c = cfg(1, 1000, 64);
  c.market_collar_ticks = 2;
  Fx f(c);
  for (Price p = 100; p <= 105; ++p) f.eng.submit(limit_order(OrderId(p), Sell, p, 1));
  f.eng.submit(market_order(1, Buy, 10));  // touch 100 -> may trade up to 102
  CHECK_EQ(f.trades().size(), std::size_t(3));
  CHECK_EQ(f.trades().back().price, Price(102));
  CHECK_EQ(f.last_reason(1), Reason::IocRemainder);
  CHECK_EQ(f.eng.best_ask(), std::optional<Price>(103));
  CHECK(f.ok());
}

TEST(modify_rejections) {
  Fx f;
  f.eng.submit(limit_order(1, Sell, 100, 5));
  f.eng.submit(limit_order(2, Buy, 99, 5, TimeInForce::GTC, 0, true));
  CHECK_EQ(f.eng.modify(2, 100, 5), Reason::PostOnlyWouldCross);
  CHECK_EQ(f.eng.modify(2, 99, 0), Reason::InvalidQty);
  CHECK_EQ(f.eng.modify(2, 5000, 1), Reason::InvalidPrice);
  CHECK_EQ(f.eng.modify(42, 99, 1), Reason::UnknownOrder);
  CHECK_EQ(f.eng.find(2)->price, Price(99));
  CHECK(f.ok());
}

// ------------------------------------------------------------- validation

TEST(new_order_validation) {
  Fx f(cfg(1, 1000, 2));
  CHECK_EQ(f.eng.submit(limit_order(0, Buy, 100, 1)), Reason::InvalidId);
  CHECK_EQ(f.eng.submit(limit_order(1, Buy, 100, 0)), Reason::InvalidQty);
  CHECK_EQ(f.eng.submit(limit_order(1, Buy, 0, 1)), Reason::InvalidPrice);
  CHECK_EQ(f.eng.submit(limit_order(1, Buy, 1001, 1)), Reason::InvalidPrice);
  CHECK_EQ(f.eng.submit(limit_order(1, Buy, 100, 1)), Reason::Ok);
  CHECK_EQ(f.eng.submit(limit_order(1, Buy, 100, 1)), Reason::DuplicateId);
  CHECK_EQ(f.eng.submit(limit_order(2, Buy, 100, 1)), Reason::Ok);
  CHECK_EQ(f.eng.submit(limit_order(3, Buy, 100, 1)), Reason::BookFull);
  CHECK_EQ(f.eng.submit(limit_order(3, Buy, 100, 1'000'000'001)), Reason::InvalidQty);
  // Every rejected new order still produces exactly one PendingNew -> Rejected event.
  int rejected = 0;
  for (const Event& e : f.rec.events)
    if (e.kind == Event::Kind::Order && e.to == S::Rejected) {
      CHECK_EQ(e.from, S::PendingNew);
      ++rejected;
    }
  CHECK_EQ(rejected, 7);
  CHECK(f.ok());
}

TEST(full_book_still_accepts_liquidity_taking_orders) {
  // Regression: a full pool used to reject IOC/market orders too, so nothing
  // but cancels could ever drain the book.
  Fx f(cfg(1, 1000, 2));
  f.eng.submit(limit_order(1, Sell, 100, 5));
  f.eng.submit(limit_order(2, Sell, 101, 5));
  CHECK_EQ(f.eng.submit(limit_order(3, Buy, 100, 6)), Reason::BookFull);  // would rest
  CHECK_EQ(f.eng.submit(limit_order(3, Buy, 100, 1)), Reason::Ok);        // certain to fill fully
  CHECK_EQ(f.eng.submit(market_order(4, Buy, 4)), Reason::Ok);
  CHECK_EQ(f.final_state(4), S::Filled);
  CHECK_EQ(f.eng.submit(limit_order(5, Buy, 101, 2, TimeInForce::IOC)), Reason::Ok);
  CHECK_EQ(f.eng.submit(limit_order(6, Buy, 101, 3, TimeInForce::FOK)), Reason::Ok);
  CHECK_EQ(f.final_state(6), S::Filled);
  CHECK_EQ(f.eng.resting_orders(), std::size_t(0));
  CHECK(f.ok());
}

TEST(replace_on_full_book_keeps_its_slot) {
  Fx f(cfg(1, 1000, 1));
  f.eng.submit(limit_order(1, Buy, 100, 5));
  CHECK_EQ(f.eng.modify(1, 101, 9), Reason::Ok);
  CHECK_EQ(f.eng.find(1)->price, Price(101));
  CHECK(f.ok());
}

TEST(config_rejects_overflow_prone_settings) {
  RecordingListener l;
  auto throws = [&](EngineConfig c) {
    try {
      Engine e(c, l);
    } catch (const std::invalid_argument&) {
      return true;
    }
    return false;
  };
  EngineConfig c;
  c.min_price = INT64_MIN;
  c.max_price = INT64_MAX;  // span computation must not overflow
  CHECK(throws(c));
  c = EngineConfig{};
  c.max_orders = 1u << 20;
  c.max_order_qty = Qty{1} << 50;  // 2^70 worst-case level total
  CHECK(throws(c));
  c = EngineConfig{};
  c.max_price = c.min_price + (1 << 24);
  CHECK(throws(c));
  CHECK(!throws(EngineConfig{}));
}

TEST(listener_reentry_aborts) {
  struct Evil {
    MatchingEngine<Evil>* eng = nullptr;
    void on_order(const Order& o, OrderState, Reason) { eng->cancel(o.id); }
    void on_trade(const Trade&) {}
    void on_reject(OrderId, OwnerId, Command, Reason) {}
    void on_session(SessionState, SessionState) {}
  };
  CHECK(aborts([] {
    Evil l;
    MatchingEngine<Evil> e(cfg(1, 100, 8), l);
    l.eng = &e;
    e.set_session(SessionState::PreOpen);
    e.submit(limit_order(1, Buy, 50, 1));
  }));
}

TEST(hash_index_resists_crafted_ids) {
  // ids k * C^-1 all collided under the previous unkeyed multiplicative hash.
  const std::uint64_t C = 0x9E3779B97F4A7C15ull;
  std::uint64_t inv = C;
  for (int i = 0; i < 6; ++i) inv *= 2 - C * inv;
  auto time_adds = [&](bool hostile) {
    NullListener l;
    MatchingEngine<NullListener> e(cfg(1, 100000, 1u << 16), l);
    e.set_session(SessionState::PreOpen);
    const auto t0 = std::chrono::steady_clock::now();
    for (std::uint64_t j = 1; j <= 20000; ++j) e.submit(limit_order(hostile ? j * inv : j, Buy, 1 + Price(j % 1000), 1));
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  };
  const double benign = time_adds(false), hostile = time_adds(true);
  CHECK(hostile < 10 * benign + 0.005);
}

TEST(id_reusable_after_order_completes) {
  Fx f;
  f.eng.submit(limit_order(1, Buy, 100, 1));
  f.eng.cancel(1);
  CHECK_EQ(f.eng.submit(limit_order(1, Buy, 100, 1)), Reason::Ok);
  CHECK(f.ok());
}

TEST(negative_price_ladder) {
  Fx f(cfg(-50, 50, 64));
  f.eng.submit(limit_order(1, Sell, -10, 5));
  f.eng.submit(limit_order(2, Buy, -5, 3));
  CHECK_EQ(f.trades().size(), std::size_t(1));
  CHECK_EQ(f.trades()[0].price, Price(-10));
  CHECK_EQ(f.eng.best_ask(), std::optional<Price>(-10));
  CHECK_EQ(f.eng.submit(limit_order(3, Buy, -51, 1)), Reason::InvalidPrice);
  CHECK(f.ok());
}

// ------------------------------------------------------------ sessions

TEST(session_gating) {
  Fx f(cfg(1, 1000, 64), /*open=*/false);
  CHECK_EQ(f.eng.session(), SessionState::Closed);
  CHECK_EQ(f.eng.submit(limit_order(1, Buy, 100, 1)), Reason::SessionClosed);
  CHECK_EQ(f.eng.set_session(SessionState::Open), Reason::InvalidTransition);
  CHECK_EQ(f.eng.set_session(SessionState::PreOpen), Reason::Ok);
  CHECK_EQ(f.eng.submit(limit_order(2, Buy, 100, 1, TimeInForce::IOC)), Reason::NotAllowedInAuction);
  CHECK_EQ(f.eng.submit(market_order(3, Buy, 1)), Reason::NotAllowedInAuction);
  CHECK_EQ(f.eng.submit(limit_order(4, Buy, 100, 1)), Reason::Ok);
  CHECK_EQ(f.eng.set_session(SessionState::Open), Reason::Ok);
  CHECK_EQ(f.eng.set_session(SessionState::Halted), Reason::Ok);
  CHECK_EQ(f.eng.submit(limit_order(5, Buy, 100, 1)), Reason::SessionHalted);
  CHECK_EQ(f.eng.modify(4, 101, 1), Reason::SessionHalted);
  CHECK_EQ(f.eng.cancel(4), Reason::Ok);  // cancels always allowed
  CHECK(f.ok());
}

TEST(preopen_book_may_cross_without_trading) {
  Fx f(cfg(1, 1000, 64), false);
  f.eng.set_session(SessionState::PreOpen);
  f.eng.submit(limit_order(1, Buy, 105, 5));
  f.eng.submit(limit_order(2, Sell, 100, 5));
  CHECK(f.trades().empty());
  CHECK_EQ(f.eng.best_bid(), std::optional<Price>(105));
  CHECK_EQ(f.eng.best_ask(), std::optional<Price>(100));
  CHECK(f.ok());
}

TEST(opening_auction_uncross) {
  Fx f(cfg(1, 1000, 64), false);
  f.eng.set_session(SessionState::PreOpen);
  f.eng.submit(limit_order(1, Buy, 102, 10));
  f.eng.submit(limit_order(2, Buy, 101, 5));
  f.eng.submit(limit_order(3, Buy, 100, 10));
  f.eng.submit(limit_order(4, Sell, 99, 5));
  f.eng.submit(limit_order(5, Sell, 100, 10));
  f.eng.submit(limit_order(6, Sell, 101, 10));
  // Executable volume: 99->5, 100->15, 101->15, 102->10. Tie at 100/101 with
  // equal imbalance (10) -> closest to midpoint reference 99+(102-99)/2 = 100.
  f.eng.set_session(SessionState::Open);
  const auto t = f.trades();
  CHECK_EQ(t.size(), std::size_t(3));
  Qty vol = 0;
  for (const Trade& tr : t) {
    CHECK_EQ(tr.price, Price(100));
    CHECK_EQ(tr.aggressor, Aggressor::None);
    vol += tr.qty;
  }
  CHECK_EQ(vol, Qty(15));
  CHECK_EQ(t[0].buy_id, OrderId(1));
  CHECK_EQ(t[0].sell_id, OrderId(4));
  CHECK_EQ(f.eng.best_bid(), std::optional<Price>(100));
  CHECK_EQ(f.eng.best_ask(), std::optional<Price>(101));
  CHECK_EQ(f.eng.last_trade_price(), std::optional<Price>(100));
  CHECK(f.ok());  // checks the book is no longer crossed
}

TEST(auction_prefers_max_volume_then_min_imbalance) {
  Fx f(cfg(1, 1000, 64), false);
  f.eng.set_session(SessionState::PreOpen);
  f.eng.submit(limit_order(1, Buy, 105, 20));
  f.eng.submit(limit_order(2, Sell, 100, 5));
  f.eng.submit(limit_order(3, Sell, 103, 10));
  // 100: min(20,5)=5; 103: min(20,15)=15; 105: min(20,15)=15 -> imbalance 5 both;
  // reference = 100+(105-100)/2 = 102 -> 103 is closer.
  f.eng.set_session(SessionState::Open);
  const auto t = f.trades();
  CHECK_EQ(t.size(), std::size_t(2));
  CHECK_EQ(t[0].price, Price(103));
  CHECK_EQ(f.eng.find(1)->leaves, Qty(5));
  CHECK(f.ok());
}

TEST(auction_price_can_fall_between_occupied_levels) {
  // Every price in [100, 105] clears 10 lots with zero imbalance, so the
  // reference price (midpoint 102) decides, even though no order rests there.
  Fx f(cfg(1, 1000, 64), false);
  f.eng.set_session(SessionState::PreOpen);
  f.eng.submit(limit_order(1, Buy, 105, 10));
  f.eng.submit(limit_order(2, Sell, 100, 10));
  f.eng.set_session(SessionState::Open);
  CHECK_EQ(f.trades().size(), std::size_t(1));
  CHECK_EQ(f.trades()[0].price, Price(102));
  CHECK(f.ok());
}

TEST(reopening_auction_anchors_on_last_trade) {
  Fx f(cfg(1, 1000, 64));
  f.eng.submit(limit_order(1, Sell, 104, 1));
  f.eng.submit(limit_order(2, Buy, 104, 1));  // last trade 104
  f.eng.set_session(SessionState::Halted);
  f.eng.set_session(SessionState::PreOpen);
  f.eng.submit(limit_order(3, Buy, 110, 10));
  f.eng.submit(limit_order(4, Sell, 90, 10));
  f.eng.set_session(SessionState::Open);
  CHECK_EQ(f.trades().back().price, Price(104));
  CHECK(f.ok());
}

TEST(close_expires_day_orders_keeps_gtc) {
  Fx f;
  f.eng.submit(limit_order(1, Buy, 100, 5, TimeInForce::Day));
  f.eng.submit(limit_order(2, Buy, 100, 5, TimeInForce::GTC));
  f.eng.submit(limit_order(3, Sell, 105, 5, TimeInForce::Day));
  CHECK_EQ(f.eng.set_session(SessionState::Closed), Reason::Ok);
  CHECK_EQ(f.final_state(1), S::Expired);
  CHECK_EQ(f.final_state(3), S::Expired);
  CHECK_EQ(f.last_reason(1), Reason::SessionEnd);
  CHECK(f.eng.find(2) != nullptr);
  CHECK_EQ(f.eng.resting_orders(), std::size_t(1));
  CHECK_EQ(f.eng.cancel(2), Reason::Ok);  // cancel allowed while closed
  CHECK(f.ok());
}

// ---------------------------------------------------- internal structures

TEST(level_bitmap_matches_bruteforce) {
  std::mt19937_64 rng(7);
  for (std::size_t n : {1u, 63u, 64u, 65u, 4095u, 4096u, 4097u, 300000u}) {
    detail::LevelBitmap bm(n);
    std::vector<bool> ref(n, false);
    for (int step = 0; step < 4000; ++step) {
      const std::size_t i = rng() % n;
      if (rng() & 1) { bm.set(i); ref[i] = true; }
      else { bm.clear(i); ref[i] = false; }
      const std::size_t q = rng() % (n + 2);
      std::size_t want_next = detail::LevelBitmap::npos, want_prev = detail::LevelBitmap::npos;
      for (std::size_t k = q; k < n; ++k) if (ref[k]) { want_next = k; break; }
      for (std::size_t k = std::min(q + 1, n); k-- > 0;) if (ref[k]) { want_prev = k; break; }
      CHECK_EQ(bm.next_at_or_after(q), want_next);
      CHECK_EQ(bm.prev_at_or_before(q), want_prev);
    }
  }
}

TEST(order_index_matches_map) {
  std::mt19937_64 rng(11);
  constexpr std::size_t kMax = 500;
  detail::OrderIndex idx(kMax, 0x1234);
  std::map<std::pair<OrderId, OwnerId>, Order*> ref;
  std::vector<Order> storage(2000 * 3);
  for (int step = 0; step < 200000; ++step) {
    const OrderId k = 1 + rng() % 2000;
    const OwnerId w = OwnerId(rng() % 3);
    if (ref.count({k, w})) {
      CHECK(idx.erase(k, w));
      ref.erase({k, w});
    } else if (ref.size() < kMax) {
      Order* v = &storage[(k - 1) * 3 + w];
      v->id = k;
      v->owner = w;
      idx.insert(v);
      ref[{k, w}] = v;
    }
    const OrderId pk = 1 + rng() % 2000;
    const OwnerId pw = OwnerId(rng() % 3);
    const auto it = ref.find({pk, pw});
    CHECK_EQ(idx.find(pk, pw), it == ref.end() ? nullptr : it->second);
    CHECK_EQ(idx.size(), ref.size());
  }
}

int main() { return tf::run_all(); }
