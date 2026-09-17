#pragma once
// Minimal test helper (no external deps for offline MVP).
#include <cstdio>
#include <string>

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond)                                                            \
  do {                                                                         \
    if (cond) {                                                                \
      ++g_pass;                                                                \
    } else {                                                                   \
      ++g_fail;                                                                \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);               \
    }                                                                          \
  } while (0)

#define CHECK_EQ(a, b)                                                         \
  do {                                                                         \
    auto _a = (a);                                                             \
    auto _b = (b);                                                             \
    if (_a == _b) {                                                            \
      ++g_pass;                                                                \
    } else {                                                                   \
      ++g_fail;                                                                \
      std::printf("FAIL %s:%d: %s != %s\n", __FILE__, __LINE__, #a, #b);        \
    }                                                                          \
  } while (0)

#define TEST_MAIN(name)                                                        \
  int main() {                                                                 \
    run_##name();                                                              \
    std::printf("%s: pass=%d fail=%d\n", #name, g_pass, g_fail);               \
    return g_fail == 0 ? 0 : 1;                                                \
  }
