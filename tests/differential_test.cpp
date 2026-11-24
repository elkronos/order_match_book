// Differential fuzzer: drives MatchingEngine and the naive ReferenceEngine
// with identical random command streams and requires byte-identical event
// streams, identical return codes, identical book contents, and a clean
// structural audit after every single command.
//
// usage: ob_differential_test [seeds=300] [ops_per_seed=4000]

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>

#include "ob/listeners.hpp"
#include "ob/matching_engine.hpp"
#include "reference_engine.hpp"

using namespace ob;

namespace {

struct Rng {
  std::mt19937_64 g;
  explicit Rng(std::uint64_t s) : g(s) {}
  std::uint64_t below(std::uint64_t n) { return g() % n; }
  bool chance(unsigned pct) { return below(100) < pct; }
  std::int64_t between(std::int64_t lo, std::int64_t hi) { return lo + std::int64_t(below(std::uint64_t(hi - lo + 1))); }
};

std::string describe(const NewOrder& r) {
  return "submit id=" + std::to_string(r.id) + " " + std::string(to_string(r.side)) + " " +
         std::string(to_string(r.type)) + " " + std::string(to_string(r.tif)) + " px=" +
         std::to_string(r.price) + " qty=" + std::to_string(r.qty) + " owner=" + std::to_string(r.owner) +
         (r.post_only ? " post" : "");
}

bool run_seed(std::uint64_t seed, int ops) {
  Rng rng(seed);
  EngineConfig cfg;
  // Narrow ladders force dense crossing; wide ones exercise the multi-layer
  // occupancy bitmap with sparse, far-apart levels.
  const bool wide = rng.chance(15);
  if (wide) { cfg.min_price = -1000; cfg.max_price = 4000; }  // 3 bitmap layers
  else if (rng.chance(30)) { cfg.min_price = -20; cfg.max_price = 20; }
  else { cfg.min_price = 1; cfg.max_price = 40; }
  const std::uint32_t caps[] = {4, 16, 64, 256};
  cfg.max_orders = caps[rng.below(4)];
  cfg.max_order_qty = rng.chance(50) ? 15 : 1'000'000;
  cfg.market_collar_ticks = rng.chance(50) ? 0 : Price(rng.below(6));
  cfg.hash_seed = seed;
  const OrderId id_space = cfg.max_orders * 3 + 5;
  const Price span = cfg.max_price - cfg.min_price;

  RecordingListener el, rl;
  MatchingEngine<RecordingListener> eng(cfg, el);
  testing::ReferenceEngine ref(cfg, rl);

  auto rand_owner = [&] { return rng.chance(60) ? OwnerId(0) : OwnerId(1 + rng.below(3)); };
  auto rand_price = [&] {
    if (rng.chance(3)) return rng.chance(50) ? cfg.min_price - 1 : cfg.max_price + 1;
    if (wide) {  // clusters around a few anchors spread over the ladder, plus outliers
      if (rng.chance(5)) return rng.between(cfg.min_price, cfg.max_price);
      static constexpr Price anchors[] = {-990, 100, 2000, 3990};
      return std::clamp(anchors[rng.below(4)] + rng.between(-12, 12), cfg.min_price, cfg.max_price);
    }
    return cfg.min_price + span / 4 + rng.between(0, span / 2);
  };

  std::size_t compared = 0;
  for (int op = 0; op < ops; ++op) {
    std::string what;
    Reason re{}, rr{};
    const auto roll = rng.below(100);
    if (roll < 3) {
      static constexpr SessionState targets[] = {SessionState::Open, SessionState::Open, SessionState::Closed,
                                                 SessionState::PreOpen, SessionState::Halted};
      const SessionState to = targets[rng.below(5)];
      what = "session -> " + std::string(to_string(to));
      re = eng.set_session(to);
      rr = ref.set_session(to);
    } else if (roll < 60) {
      NewOrder r;
      r.id = rng.chance(1) ? 0 : 1 + rng.below(id_space);
      r.side = rng.chance(50) ? Side::Buy : Side::Sell;
      r.type = rng.chance(8) ? OrderType::Market : OrderType::Limit;
      const auto t = rng.below(100);
      r.tif = t < 50 ? TimeInForce::GTC : t < 75 ? TimeInForce::Day : t < 90 ? TimeInForce::IOC : TimeInForce::FOK;
      r.price = rand_price();
      r.qty = rng.chance(2) ? 0 : 1 + rng.below(20);
      r.owner = rand_owner();
      r.post_only = rng.chance(10);
      what = describe(r);
      re = eng.submit(r);
      rr = ref.submit(r);
    } else if (roll < 82) {
      const OrderId id = rng.below(id_space + 1);
      const OwnerId owner = rand_owner();
      what = "cancel id=" + std::to_string(id) + " owner=" + std::to_string(owner);
      re = eng.cancel(id, owner);
      rr = ref.cancel(id, owner);
    } else {
      const OrderId id = rng.below(id_space + 1);
      const Price px = rand_price();
      const Qty q = rng.chance(5) ? 0 : 1 + rng.below(25);
      const OwnerId owner = rand_owner();
      what = "modify id=" + std::to_string(id) + " px=" + std::to_string(px) + " qty=" + std::to_string(q) +
             " owner=" + std::to_string(owner);
      re = eng.modify(id, px, q, owner);
      rr = ref.modify(id, px, q, owner);
    }

    auto fail = [&](const std::string& msg) {
      std::printf("seed %llu op %d [%s]: %s\n", (unsigned long long)seed, op, what.c_str(), msg.c_str());
      std::printf("  engine events (from #%zu):\n", compared);
      for (std::size_t i = compared; i < el.events.size(); ++i) std::printf("    %s\n", el.events[i].to_string().c_str());
      std::printf("  reference events (from #%zu):\n", compared);
      for (std::size_t i = compared; i < rl.events.size(); ++i) std::printf("    %s\n", rl.events[i].to_string().c_str());
      return false;
    };

    if (re != rr) return fail("return code " + std::string(to_string(re)) + " vs " + std::string(to_string(rr)));
    if (el.events.size() != rl.events.size()) return fail("event count differs");
    for (std::size_t i = compared; i < el.events.size(); ++i)
      if (!(el.events[i] == rl.events[i])) return fail("event #" + std::to_string(i) + " differs");
    compared = el.events.size();

    // The structural audit is O(ladder width): every command on narrow
    // ladders, periodically on wide ones (events and book are still compared
    // after every command).
    if (!wide || op % 32 == 31 || op == ops - 1)
      if (const std::string err = eng.check_invariants(); !err.empty()) return fail("invariant: " + err);
    if (eng.best_bid() != ref.best_bid() || eng.best_ask() != ref.best_ask()) return fail("BBO differs");
    for (Side s : {Side::Buy, Side::Sell}) {
      std::vector<Order> mine;
      eng.for_each_level(s, SIZE_MAX, [&](Price p, Qty, std::uint32_t) {
        eng.for_each_order_at(s, p, [&](const Order& o) { mine.push_back(o); });
      });
      const std::vector<Order> theirs = ref.side_in_priority(s);
      if (mine.size() != theirs.size()) return fail("book size differs");
      for (std::size_t i = 0; i < mine.size(); ++i) {
        const Order &a = mine[i], &b = theirs[i];
        if (a.id != b.id || a.owner != b.owner || a.price != b.price || a.leaves != b.leaves || a.cum != b.cum || a.state != b.state)
          return fail("book contents differ at " + std::string(to_string(s)) + " position " + std::to_string(i));
      }
    }
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  const int seeds = argc > 1 ? std::atoi(argv[1]) : 300;
  const int ops = argc > 2 ? std::atoi(argv[2]) : 4000;
  for (int s = 1; s <= seeds; ++s) {
    if (!run_seed(std::uint64_t(s), ops)) {
      std::printf("FAILED\n");
      return 1;
    }
  }
  std::printf("differential test passed: %d seeds x %d ops = %lld commands, engine == reference\n", seeds, ops,
              (long long)seeds * ops);
  return 0;
}
