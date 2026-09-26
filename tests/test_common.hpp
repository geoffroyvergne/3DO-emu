#pragma once

#include <cstdio>

// Minimal assertion helpers shared by the headless test executables.
inline int g_failures = 0;

#define CHECK_EQ(actual, expected)                                                            \
    do {                                                                                      \
        const auto a_ = (actual);                                                             \
        const auto e_ = (expected);                                                           \
        if (a_ != e_) {                                                                       \
            std::fprintf(stderr, "%s:%d: %s == 0x%08X, expected 0x%08X\n", __FILE__, __LINE__, \
                         #actual, static_cast<unsigned>(a_), static_cast<unsigned>(e_));      \
            ++g_failures;                                                                     \
        }                                                                                     \
    } while (0)

inline int report(const char* suite) {
    if (g_failures == 0) std::printf("%s: all tests passed\n", suite);
    return g_failures == 0 ? 0 : 1;
}
