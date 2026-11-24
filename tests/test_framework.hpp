#pragma once

// Minimal dependency-free test harness.

#include <cstdio>
#include <functional>
#include <optional>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "ob/state_machine.hpp"
#include "ob/types.hpp"

namespace tf {

struct Case {
  const char* name;
  void (*fn)();
};
inline std::vector<Case>& registry() {
  static std::vector<Case> r;
  return r;
}
inline int& failures() {
  static int f = 0;
  return f;
}
struct Registrar {
  Registrar(const char* n, void (*f)()) { registry().push_back({n, f}); }
};

template <class T>
std::string show(const T& v) {
  if constexpr (std::is_enum_v<T>) {
    using ob::to_string;
    return std::string(to_string(v));
  } else if constexpr (requires { v.has_value(); }) {
    return v.has_value() ? show(*v) : std::string("nullopt");
  } else {
    std::ostringstream os;
    os << v;
    return os.str();
  }
}

inline void fail(const char* file, int line, const std::string& msg) {
  std::printf("    FAIL %s:%d: %s\n", file, line, msg.c_str());
  ++failures();
}

inline int run_all() {
  int failed_cases = 0;
  for (const Case& c : registry()) {
    const int before = failures();
    c.fn();
    const bool ok = failures() == before;
    std::printf("[%s] %s\n", ok ? " OK " : "FAIL", c.name);
    if (!ok) ++failed_cases;
  }
  std::printf("\n%zu tests, %d failed\n", registry().size(), failed_cases);
  return failed_cases == 0 ? 0 : 1;
}

}  // namespace tf

#define TEST(name)                                      \
  static void name();                                   \
  static ::tf::Registrar tf_reg_##name(#name, &name);   \
  static void name()

#define CHECK(cond)                                     \
  do {                                                  \
    if (!(cond)) {                                      \
      ::tf::fail(__FILE__, __LINE__, #cond);            \
      return;                                           \
    }                                                   \
  } while (0)

#define CHECK_EQ(a, b)                                                                         \
  do {                                                                                         \
    const auto tf_a = (a);                                                                    \
    const auto tf_b = (b);                                                                    \
    if (!(tf_a == tf_b)) {                                                                     \
      ::tf::fail(__FILE__, __LINE__,                                                           \
                 std::string(#a " == " #b " (") + ::tf::show(tf_a) + " vs " + ::tf::show(tf_b) + ")"); \
      return;                                                                                  \
    }                                                                                          \
  } while (0)
