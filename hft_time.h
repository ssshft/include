#pragma once

// ============================================================================
// include/hft_time.h
//
// 进程内统一时钟入口。目标平台：Linux x86_64。
// 只做一件事：把“墙钟”和“单调钟”分开，并给热路径一个明确的选择。
//
//   hft::time::mono_ns()        单调 ns：延时、排队、超时、指标、排序。
//   hft::time::mono_us()        单调 µs：需要保持 µs 单位的差值计算。
//   hft::time::wall_ns()        墙钟 ns：日志、展示、跨天对时。
//   hft::time::wall_us()        墙钟 µs：兼容 crypto::getCurrentTime()。
//   hft::time::mono_ticks()     原始单调 tick；配合 mono_ns_per_tick() 使用。
//   hft::time::tsc_ticks()      可选 TSC 通道；配合 tsc_ns_per_tick() 使用。
//   hft::time::cycles()         微基准用；不是 ns，不要跨线程直接比较。
//
// 约束：
//   * mono_* 在同一 host 的所有进程间可比（同一内核时钟）。
//   * wall_* 会受 NTP/adjtime 影响，不能用于差值。
//   * tsc_* 只适合同一线程/同一 socket 的短段；跨核/跨进程先验证
//     constant_tsc + nonstop_tsc。
//   * 非 x86 分支只是为了让头文件能编译，不是支持平台。
// ============================================================================

#include <cstdint>
#include <ctime>
#include <sys/time.h>

namespace hft {
namespace time {

// ---------------------------------------------------------------------------
// 单调钟：CLOCK_MONOTONIC，单位 ns。
// ---------------------------------------------------------------------------

inline std::uint64_t mono_ticks() noexcept {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    // 先转秒、再乘 1e9，最后加 tv_nsec；不是只保留秒。
    return static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ull +
           static_cast<std::uint64_t>(ts.tv_nsec);
}

inline double mono_ns_per_tick() noexcept {
    return 1.0;
}

inline const char* mono_clock_name() noexcept {
    return "monotonic";
}

inline std::uint64_t mono_ns() noexcept {
    return mono_ticks();
}

inline std::int64_t mono_us() noexcept {
    // 只对 tv_nsec 做 /1000；除以编译期常量，编译器会优化成乘法+移位。
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::int64_t>(ts.tv_sec) * 1000000LL + ts.tv_nsec / 1000;
}

// ---------------------------------------------------------------------------
// 可选 TSC 通道：只有确认 constant_tsc + nonstop_tsc 后才用。
// ---------------------------------------------------------------------------

inline std::uint64_t tsc_ticks() noexcept {
#if defined(__x86_64__) || defined(__i386__)
    return __builtin_ia32_rdtsc();
#else
    return mono_ticks();
#endif
}

inline const char* tsc_clock_name() noexcept {
#if defined(__x86_64__) || defined(__i386__)
    return "tsc";
#else
    return "monotonic";
#endif
}

inline double tsc_ns_per_tick() noexcept {
#if defined(__x86_64__) || defined(__i386__)
    // 只在第一次调用时做一次 20 ms 校准；不在热路径。
    static const double v = [] {
        struct timespec a{}, b{};
        clock_gettime(CLOCK_MONOTONIC, &a);
        const std::uint64_t t0 = tsc_ticks();
        struct timespec req{0, 20 * 1000 * 1000};
        nanosleep(&req, nullptr);
        const std::uint64_t t1 = tsc_ticks();
        clock_gettime(CLOCK_MONOTONIC, &b);

        const double ns = static_cast<double>(b.tv_sec - a.tv_sec) * 1e9 +
                          static_cast<double>(b.tv_nsec - a.tv_nsec);
        const double ticks = static_cast<double>(t1 - t0);
        if (ticks > 0.0 && ns > 0.0) {
            return ns / ticks;
        }
        return 1.0;
    }();
    return v;
#else
    return 1.0;
#endif
}

inline std::uint64_t cycles() noexcept {
    return tsc_ticks();
}

// ---------------------------------------------------------------------------
// 墙钟：日志、展示、交易所字段、跨天对时。会跳，不用于差值。
// ---------------------------------------------------------------------------

inline std::uint64_t wall_ns() noexcept {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ull +
           static_cast<std::uint64_t>(ts.tv_nsec);
}

inline std::int64_t wall_us() noexcept {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return static_cast<std::int64_t>(tv.tv_sec) * 1000000LL + tv.tv_usec;
}

inline std::int64_t wall_ms() noexcept {
    // 只对 tv_usec 做 /1000；除以编译期常量，编译器会优化成乘法+移位。
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return static_cast<std::int64_t>(tv.tv_sec) * 1000LL + tv.tv_usec / 1000;
}

inline std::int64_t wall_s() noexcept {
    // 秒就是 tv_sec，不需要除法。
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return static_cast<std::int64_t>(tv.tv_sec);
}

} // namespace time
} // namespace hft
