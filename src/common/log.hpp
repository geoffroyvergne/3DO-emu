#pragma once

#include <bit>
#include <cstdio>
#include <format>
#include <string_view>
#include <utility>

namespace Log {

inline void write(std::string_view level, std::string_view message) {
    std::fprintf(stderr, "[%.*s] %.*s\n", static_cast<int>(level.size()), level.data(),
                 static_cast<int>(message.size()), message.data());
}

template <typename... Args>
void info(std::format_string<Args...> fmt, Args&&... args) {
    write("INFO", std::format(fmt, std::forward<Args>(args)...));
}

template <typename... Args>
void warn(std::format_string<Args...> fmt, Args&&... args) {
    write("WARN", std::format(fmt, std::forward<Args>(args)...));
}

template <typename... Args>
void error(std::format_string<Args...> fmt, Args&&... args) {
    write("ERROR", std::format(fmt, std::forward<Args>(args)...));
}

// Rate limiter for warnings that can fire on every emulated access.
// The first `burst` occurrences are printed in full; after that only a count
// is printed, at 2x, 4x, 8x... the burst, so a flood costs O(log n) lines.
// Suppressed calls return before formatting, so they stay cheap.
// A burst of 1 gives warn-once behaviour. Not thread-safe (the core is single-threaded).
class Limiter {
public:
    constexpr explicit Limiter(std::string_view name, unsigned long long burst = 16)
        : name_(name), burst_(burst == 0 ? 1 : burst) {}

    template <typename... Args>
    void warn(std::format_string<Args...> fmt, Args&&... args) {
        const unsigned long long n = ++count_;
        if (n <= burst_) {
            write("WARN", std::format(fmt, std::forward<Args>(args)...));
            if (n == burst_ && burst_ > 1)
                write("WARN", std::format("{}: further occurrences suppressed", name_));
        } else if (n % burst_ == 0 && std::has_single_bit(n / burst_)) {
            write("WARN", std::format("{}: {} occurrences so far", name_, n));
        }
    }

    [[nodiscard]] unsigned long long count() const { return count_; }
    void reset() { count_ = 0; }

private:
    std::string_view name_;
    unsigned long long burst_;
    unsigned long long count_ = 0;
};

}  // namespace Log
