// compat.h — Standalone build compatibility (replaces rtc_base and absl deps)
#pragma once
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

#define RTC_DCHECK(x) assert(x)
#define RTC_DCHECK_EQ(a, b) assert((a) == (b))
#define RTC_DCHECK_NE(a, b) assert((a) != (b))

namespace absl {
using string_view = std::string_view;
inline std::string StrCat(const std::string& a, const std::string& b) {
  return a + b;
}
inline std::string StrCat(const std::string& a, int b) {
  return a + std::to_string(b);
}
}  // namespace absl
