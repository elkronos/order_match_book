// Interactive / scriptable front-end. Reads commands from stdin, prints every
// engine event. The engine starts in the Closed session.
//
//   session closed|preopen|open|halted
//   buy  <id> <qty> <price> [gtc|day|ioc|fok] [post] [owner=N]
//   sell <id> <qty> <price> [gtc|day|ioc|fok] [post] [owner=N]
//   mbuy <id> <qty> [fok] [owner=N]          market buy
//   msell <id> <qty> [fok] [owner=N]         market sell
//   cancel <id> [owner=N]
//   modify <id> <price> <total_qty> [owner=N]
//   book [levels]
//   # comment

#include <cstdio>
#include <iostream>
#include <sstream>
#include <string>

#include "ob/listeners.hpp"
#include "ob/matching_engine.hpp"

using namespace ob;

namespace {

struct PrintingListener {
  RecordingListener rec;
  void flush() {
    for (const Event& e : rec.events) std::printf("  %s\n", e.to_string().c_str());
    rec.events.clear();
  }
  void on_order(const Order& o, OrderState f, Reason r) { rec.on_order(o, f, r); }
  void on_trade(const Trade& t) { rec.on_trade(t); }
  void on_reject(OrderId id, OwnerId o, Command c, Reason r) { rec.on_reject(id, o, c, r); }
  void on_session(SessionState f, SessionState t) { rec.on_session(f, t); }
};

OwnerId parse_owner(std::istringstream& in) {
  std::string tok;
  if (!(in >> tok)) return 0;
  if (tok.rfind("owner=", 0) != 0) throw std::invalid_argument("unknown flag '" + tok + "'");
  return OwnerId(std::stoul(tok.substr(6)));
}

bool parse_session(const std::string& s, SessionState& out) {
  if (s == "closed") out = SessionState::Closed;
  else if (s == "preopen") out = SessionState::PreOpen;
  else if (s == "open") out = SessionState::Open;
  else if (s == "halted" || s == "halt") out = SessionState::Halted;
  else return false;
  return true;
}

void parse_flags(std::istringstream& in, NewOrder& r) {
  std::string tok;
  while (in >> tok) {
    if (tok == "gtc") r.tif = TimeInForce::GTC;
    else if (tok == "day") r.tif = TimeInForce::Day;
    else if (tok == "ioc") r.tif = TimeInForce::IOC;
    else if (tok == "fok") r.tif = TimeInForce::FOK;
    else if (tok == "post") r.post_only = true;
    else if (tok.rfind("owner=", 0) == 0) r.owner = OwnerId(std::stoul(tok.substr(6)));
    else throw std::invalid_argument("unknown flag '" + tok + "'");
  }
}

template <class E>
void print_book(const E& eng, std::size_t depth) {
  std::vector<std::string> asks;
  eng.for_each_level(Side::Sell, depth, [&](Price p, Qty q, std::uint32_t n) {
    char buf[96];
    std::snprintf(buf, sizeof buf, "             | %10lld | %8llu (%u)", (long long)p, (unsigned long long)q, n);
    asks.emplace_back(buf);
  });
  std::printf("  session=%s  resting=%zu\n", std::string(to_string(eng.session())).c_str(), eng.resting_orders());
  std::printf("      bid qty  |    price   |  ask qty\n");
  for (auto it = asks.rbegin(); it != asks.rend(); ++it) std::printf("  %s\n", it->c_str());
  eng.for_each_level(Side::Buy, depth, [&](Price p, Qty q, std::uint32_t n) {
    std::printf("  %8llu (%u) | %10lld |\n", (unsigned long long)q, n, (long long)p);
  });
}

}  // namespace

int main() {
  PrintingListener l;
  MatchingEngine<PrintingListener> eng(EngineConfig{1, 1'000'000, 1u << 16}, l);
  std::string line;
  while (std::getline(std::cin, line)) {
    if (line.empty() || line[0] == '#') {
      if (!line.empty()) std::printf("%s\n", line.c_str());
      continue;
    }
    std::printf("> %s\n", line.c_str());
    std::istringstream in(line);
    std::string cmd;
    in >> cmd;
    try {
      if (cmd == "session") {
        std::string s;
        SessionState st;
        if (!(in >> s) || !parse_session(s, st)) throw std::invalid_argument("bad session");
        eng.set_session(st);
      } else if (cmd == "buy" || cmd == "sell") {
        NewOrder r;
        r.side = cmd == "buy" ? Side::Buy : Side::Sell;
        if (!(in >> r.id >> r.qty >> r.price)) throw std::invalid_argument("usage: buy|sell id qty price");
        parse_flags(in, r);
        eng.submit(r);
      } else if (cmd == "mbuy" || cmd == "msell") {
        NewOrder r;
        r.side = cmd == "mbuy" ? Side::Buy : Side::Sell;
        r.type = OrderType::Market;
        r.tif = TimeInForce::IOC;
        if (!(in >> r.id >> r.qty)) throw std::invalid_argument("usage: mbuy|msell id qty");
        parse_flags(in, r);
        eng.submit(r);
      } else if (cmd == "cancel") {
        OrderId id;
        if (!(in >> id)) throw std::invalid_argument("usage: cancel id [owner=N]");
        eng.cancel(id, parse_owner(in));
      } else if (cmd == "modify") {
        OrderId id;
        Price p;
        Qty q;
        if (!(in >> id >> p >> q)) throw std::invalid_argument("usage: modify id price total_qty [owner=N]");
        eng.modify(id, p, q, parse_owner(in));
      } else if (cmd == "book") {
        std::size_t depth = 10;
        in >> depth;
        print_book(eng, depth);
      } else {
        throw std::invalid_argument("unknown command '" + cmd + "'");
      }
    } catch (const std::exception& e) {
      std::printf("  error: %s\n", e.what());
    }
    l.flush();
  }
  return 0;
}
