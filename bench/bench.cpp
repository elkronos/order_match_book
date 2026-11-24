// Throughput and latency benchmark with a synthetic, realistic message mix:
//   45% passive limit adds near the touch, 45% cancels, 4% modifies,
//    5% marketable IOC limits, 1% market orders.
// Commands are pre-generated so RNG cost is excluded from timings.
//
// usage: ob_bench [num_ops=10000000] [cpu_to_pin]

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

#if defined(__x86_64__)
#include <x86intrin.h>
#endif
#if defined(__linux__)
#include <sched.h>
#endif

#include "ob/matching_engine.hpp"

using namespace ob;

namespace {

struct CountingListener {
  std::uint64_t trades = 0, volume = 0, order_events = 0, rejects = 0;
  void on_order(const Order&, OrderState, Reason) noexcept { ++order_events; }
  void on_trade(const Trade& t) noexcept { ++trades; volume += t.qty; }
  void on_reject(OrderId, OwnerId, Command, Reason) noexcept { ++rejects; }
  void on_session(SessionState, SessionState) noexcept {}
};

struct Op {
  enum Kind : std::uint8_t { Submit, Cancel, Modify } kind;
  NewOrder req;
  OrderId id;
  Price price;
  Qty qty;
};

std::vector<Op> generate(std::size_t n, std::uint64_t seed) {
  std::mt19937_64 rng(seed);
  std::vector<Op> ops;
  ops.reserve(n);
  std::vector<OrderId> live;  // candidates for cancel/modify (may already be filled)
  live.reserve(n);
  OrderId next_id = 1;
  Price mid = 500'000;
  std::geometric_distribution<int> depth(0.25);
  auto qty = [&] { return Qty(1 + rng() % 100); };

  for (std::size_t i = 0; i < n; ++i) {
    if (rng() % 64 == 0) mid += (rng() & 1) ? 1 : -1;  // slow random walk
    const auto r = rng() % 100;
    const Side side = (rng() & 1) ? Side::Buy : Side::Sell;
    if (r < 45 || live.empty()) {
      const Price off = depth(rng);
      const Price px = side == Side::Buy ? mid - 1 - off : mid + 1 + off;
      const OrderId id = next_id++;
      ops.push_back({Op::Submit, limit_order(id, side, px, qty()), 0, 0, 0});
      live.push_back(id);
    } else if (r < 90) {
      const std::size_t k = rng() % live.size();
      ops.push_back({Op::Cancel, {}, live[k], 0, 0});
      live[k] = live.back();
      live.pop_back();
    } else if (r < 94) {
      const OrderId id = live[rng() % live.size()];
      ops.push_back({Op::Modify, {}, id, mid + ((rng() & 1) ? 3 : -3), qty()});
    } else if (r < 99) {
      const Price px = side == Side::Buy ? mid + 2 : mid - 2;
      ops.push_back({Op::Submit, limit_order(next_id++, side, px, qty(), TimeInForce::IOC), 0, 0, 0});
    } else {
      ops.push_back({Op::Submit, market_order(next_id++, side, qty()), 0, 0, 0});
    }
  }
  return ops;
}

template <class E>
inline void apply(E& eng, const Op& op) {
  switch (op.kind) {
    case Op::Submit: eng.submit(op.req); break;
    case Op::Cancel: eng.cancel(op.id); break;
    case Op::Modify: eng.modify(op.id, op.price, op.qty); break;
  }
}

// Serialised TSC reads: lfence keeps the timed work from drifting outside the
// [start, stop] window under out-of-order execution.
inline std::uint64_t tsc_start() {
#if defined(__x86_64__)
  _mm_lfence();
  const std::uint64_t t = __rdtsc();
  _mm_lfence();
  return t;
#else
  return std::uint64_t(std::chrono::steady_clock::now().time_since_epoch().count());
#endif
}
inline std::uint64_t tsc_stop() {
#if defined(__x86_64__)
  unsigned aux;
  const std::uint64_t t = __rdtscp(&aux);
  _mm_lfence();
  return t;
#else
  return tsc_start();
#endif
}
inline std::uint64_t ticks() { return tsc_start(); }

double ticks_per_ns() {
#if defined(__x86_64__)
  const auto t0 = std::chrono::steady_clock::now();
  const std::uint64_t c0 = ticks();
  while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(200)) {}
  const std::uint64_t c1 = ticks();
  const double ns = double(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
  return double(c1 - c0) / ns;
#else
  return 1.0;
#endif
}

EngineConfig bench_config() {
  EngineConfig c;
  c.max_orders = 1u << 21;
  return c;
}

}  // namespace

int main(int argc, char** argv) {
  const std::size_t n = argc > 1 ? std::size_t(std::atoll(argv[1])) : 10'000'000;
  if (argc > 2) {
#if defined(__linux__)
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(std::atoi(argv[2]), &set);
    if (sched_setaffinity(0, sizeof set, &set) != 0) std::perror("sched_setaffinity");
    else std::printf("pinned to cpu %s\n", argv[2]);
#endif
  }
  std::printf("generating %zu commands...\n", n);
  const std::vector<Op> ops = generate(n, 42);

  // Throughput: best of 3 fresh runs.
  double best_ns = 1e300;
  CountingListener stats;
  for (int run = 0; run < 3; ++run) {
    CountingListener l;
    auto eng = std::make_unique<MatchingEngine<CountingListener>>(bench_config(), l);
    eng->set_session(SessionState::PreOpen);
    eng->set_session(SessionState::Open);
    const auto t0 = std::chrono::steady_clock::now();
    for (const Op& op : ops) apply(*eng, op);
    const auto t1 = std::chrono::steady_clock::now();
    best_ns = std::min(best_ns, double(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()));
    if (run == 0) {
      stats = l;
      std::printf("final book: %zu resting orders, bbo %lld / %lld\n", eng->resting_orders(),
                  (long long)eng->best_bid().value_or(0), (long long)eng->best_ask().value_or(0));
    }
  }
  std::printf("events: %llu order transitions, %llu trades (volume %llu), %llu command rejects\n",
              (unsigned long long)stats.order_events, (unsigned long long)stats.trades,
              (unsigned long long)stats.volume, (unsigned long long)stats.rejects);
  std::printf("\nthroughput: %.1f M commands/s  (%.1f ns/command mean)\n", double(n) / best_ns * 1e3, best_ns / double(n));

  // Per-command latency (TSC timestamps around each call).
  const double tpn = ticks_per_ns();
  std::uint64_t overhead = ~0ull;
  for (int i = 0; i < 100000; ++i) {
    const std::uint64_t a = tsc_start();
    const std::uint64_t b = tsc_stop();
    overhead = std::min(overhead, b - a);
  }
  CountingListener l;
  auto eng = std::make_unique<MatchingEngine<CountingListener>>(bench_config(), l);
  eng->set_session(SessionState::PreOpen);
  eng->set_session(SessionState::Open);
  std::vector<std::uint32_t> lat;
  lat.reserve(n);
  for (const Op& op : ops) {
    const std::uint64_t a = tsc_start();
    apply(*eng, op);
    const std::uint64_t b = tsc_stop();
    const std::uint64_t d = b - a > overhead ? b - a - overhead : 0;
    lat.push_back(std::uint32_t(std::min<std::uint64_t>(d, UINT32_MAX)));
  }
  std::sort(lat.begin(), lat.end());
  auto pct = [&](double p) { return double(lat[std::min(lat.size() - 1, std::size_t(p * double(lat.size())))]) / tpn; };
  std::printf("latency (ns, timer overhead %.1f ns subtracted):\n", double(overhead) / tpn);
  std::printf("  p50 %.0f   p90 %.0f   p99 %.0f   p99.9 %.0f   p99.99 %.0f   max %.0f\n", pct(0.50), pct(0.90),
              pct(0.99), pct(0.999), pct(0.9999), double(lat.back()) / tpn);
  return 0;
}
