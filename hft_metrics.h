#pragma once

// ============================================================================
// include/hft_metrics.h
//
// HFT 轻量级延时 / 计数统计（header-only，无第三方依赖）
//
// 六条设计约束（这六条就是验收标准，改本文件前先看它）
// ---------------------------------------------------------------------------
//   1. 通用性  不绑定行情 / 订单 / 队列。本文件不认识任何业务类型，
//              指标只是一个字符串 + 一个 id。谁埋点谁起名。
//   2. 低侵入  只加宏，不改结构体、不改函数签名、不改队列接口。
//              埋点代码删掉之后，业务代码一个字都不用改。
//   3. 低开销  热路径不加锁、不 malloc、不格式化、不做系统调用。
//              实测（arm64）：HFT_CNT 1.5 ns，HFT_SCOPE 34.7 ns
//              （= 两次 clock_gettime，各约 13 ns）；开 TSC 后约 15 ns。
//   4. 可关闭  **只有编译期一个开关，而且默认是关的**：不传
//              -DHFT_METRICS_ENABLED=1 时所有宏展开成 ((void)0)，连参数都不求值
//              （HFT_RECORD_SINCE 里的 HFT_NOW() 不会执行），目标码里连 clock_gettime
//              这个符号都不剩（实测 5355 B → 96 B、外部符号 34 → 0）。
//              线上要测就显式打开（-DHFT_METRICS_ENABLED=1），见文件后面第 4 条说明。
//              **没有运行期开关**（没有 set_enabled / 没有环境变量）—— 热路径里
//              连一次判断都省掉，也不会出现"线上被谁悄悄关了"。
//              想降低开销用 HFT_SCOPE_SAMPLED(name, N) 的 1/N 采样。
//   5. 多线程  每个线程一个 shard（thread_local），热路径只写自己的 cache line；
//              线程退出时把 shard 并入 retired，数据不丢；无全局锁、无 false sharing。
//   6. 可扩展  函数耗时（HFT_SCOPE）、阶段耗时（HFT_SCOPE / HFT_RECORD_SINCE）、
//              计数（HFT_CNT）、瞬时值（HFT_GAUGE）、分位数（直方图合并）。
//              加新指标不需要改任何注册表代码，宏里写个新名字就行。
//
//   附加一条时钟规矩：**每个指标自带时钟域**。段内计时（HFT_SCOPE）走 Fast 域
//   （默认 MONOTONIC，确认 constant_tsc 后可换 TSC）；跨线程 / 跨进程的时间戳
//   （HFT_NOW / HFT_RECORD_SINCE）永远走 MONOTONIC。见下面「时钟」一节 ——
//   这两者绝不能混，混了就是静默算错。
//
//   明确**不做**的事：队列等待耗时（消息在队列里待了多久）。那需要在队列布局里
//   加影子时间戳数组，属于"改队列接口"，与第 2 条冲突，且会让 shm 布局版本号
//   变化、四个进程必须同时重启。见 docs/延时统计库-设计.md §3。
//
// ---------------------------------------------------------------------------
// 用法
// ---------------------------------------------------------------------------
//   HFT_CNT("db.frames");                     // 计数器 +1
//   HFT_CNT_ADD("db.bytes", n);               // 计数器 +n
//   HFT_GAUGE("db.ring.depth", depth);        // 瞬时值
//
//   {
//       HFT_SCOPE("db.parse");                // 段内计时（RAII）
//       ...
//   }
//   {
//       HFT_SCOPE_SAMPLED("db.publish", 16);  // 1/16 采样，N 必须是 2 的幂
//       ...
//   }
//
//   手动时间戳（用于跨语句 / 已经拿到 t0 的地方）：
//       uint64_t t0 = HFT_NOW();              // 单调钟，跨线程/跨进程可比
//       ...
//       HFT_RECORD_SINCE("db.parse", t0);
//
//   已经拿到起点时间戳的地方（例如共享内存里别人盖好的 mono 戳）：
//       uint64_t t0 = pdata->__updatestamp;   // mono ns，生产者盖的
//       if (t0) { HFT_RECORD_SINCE("strat.wait", t0); }
//
//   HFT_SCOPE 展开成"声明 + 对象"，必须放在大括号块里：
//       if (ok) { HFT_SCOPE("x"); ... }
//   不要写成 if (ok) HFT_SCOPE("x");（没有大括号会编译失败，这是故意的）。
//
// ---------------------------------------------------------------------------
// 导出：**不需要单开线程**，在已有循环里调 report_if_due 就行
// ---------------------------------------------------------------------------
//   // ① 推荐：放进已有的 1 Hz 定时器（heavy_work 的 on_timer / T_signal），零额外开销
//   void on_timer() {
//       ...
//       hft::metrics::report_if_due();          // 没到点就是一次比较，直接返回
//   }
//
//   // ② main 的 while 循环里也可以，但**不要每圈都判时**（见下）
//   while (running) {
//       ...
//       if ((++n & 0x3FFF) == 0) {              // 每 16384 圈才判一次
//           hft::metrics::report_if_due();
//       }
//   }
//
//   为什么不要每圈都调：`report_if_due()` 内部要读一次时钟（13 ns）。
//   纯 busy-spin 循环一秒能转上千万圈，每圈读一次就是 10% 以上的 CPU。
//   放进已有 1 Hz 定时器 = 0 次额外读钟；放进 while 循环就配一个计数器。
//
//   report_if_due 只在"到点"时才做 snapshot + 格式化（会 malloc、会加锁），
//   所以**只能在非热路径调**；没到点的时候它不分配、不加锁、不格式化。
//
//   要自己拿数据（写文件 / 打 JSON / 上报）：
//       auto snap = hft::metrics::snapshot();
//       std::puts(snap.to_text().c_str());
//       std::puts(snap.to_json().c_str());
//       std::puts(snap.to_json(true).c_str());   // 带 buckets，给离线重算分位/画分布
//
//   确实想要独立线程（进程里没有定时器时）：
//       hft::metrics::Reporter rep;
//       rep.start(std::chrono::milliseconds(1000), [](const auto& s) {
//           std::puts(s.to_text().c_str());
//       });
//
// ---------------------------------------------------------------------------
// 时钟（两个域，按指标分开记）
// ---------------------------------------------------------------------------
//   Fast 域（只给 HFT_SCOPE / HFT_NOW_FAST 用）
//       默认 = MONOTONIC；x86_64 确认 constant_tsc + nonstop_tsc 后加
//       -DHFT_METRICS_USE_TSC=1 可换成 rdtsc，每次省约 20 ns。
//       ⚠️ 只允许"同一线程内 t1 - t0"。跨核 / 跨 socket / VM 里 TSC 有偏移，
//          跨线程或跨进程相减会得到负值（回绕成天文数字）。
//   Mono 域（HFT_NOW / HFT_RECORD / HFT_RECORD_SINCE / HFT_RECORD_NS）
//       永远 CLOCK_MONOTONIC。**任何跨线程、跨进程、读共享内存里时间戳的地方只能用这个。**
//
//   时钟实现统一在 include/hft_time.h；本头只决定每个指标落在哪个域。
//   热路径只存 tick，不做乘除法；换算只发生在 snapshot()。
//   启用 TSC 时**必须在起线程之前调用一次 hft::metrics::warmup()**，
//   否则第一次 HFT_SCOPE 会触发 TSC 校准里的 nanosleep(20ms)，落在交易线程上。
//
// ---------------------------------------------------------------------------
// 约束
// ---------------------------------------------------------------------------
//   * 不保证 async-signal-safe。
//   * 每个进程独立；跨进程合并请把 snapshot 导出后在外部分析。
//     **不要**把两个进程的直方图合并成一个 —— 样本数不同、时钟域可能不同，合并出来的 p99 没有物理意义。
//   * metric 名字必须是编译期字符串字面量；不要在热路径拼接 symbol。
//   * 默认上限 512 个 metric。超过后新 metric 返回 kInvalidMetric，埋点自动变 no-op，
//     并且**第一次溢出会打一条 stderr + 置 metrics.rejected**（不会静默）。
//   * 单次记录超过 kMaxTrackedTicks（约 17 s @MONOTONIC）视为异常样本（回绕 / 时钟混用），
//     直接丢弃并计入 metrics.rejected，不进直方图 —— 否则一次坏样本会永久污染 max 和 p999。
// ============================================================================


#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "hft_time.h"

// **默认关**：不显式传 -DHFT_METRICS_ENABLED=1 就是关的。
// 理由是"没量过的开销不该默认背上"——线上要测必须有人主动打开、并且知道自己在打开。
// 代价是失败模式反过来：忘了开 ⇒ 埋点静默变成空语句，`snapshot()` 里一个指标都没有。
// 所以每个接入的进程都要在自己的构建配置里显式写死取值（见 db/CMakeLists.txt），
// 而且 db 的 main 在关闭时会在启动日志里喊一句，不会让你以为在测。
#ifndef HFT_METRICS_ENABLED
#define HFT_METRICS_ENABLED 0
#endif


// 默认不开，即使在 x86 上也是如
// 被测段 < 500 ns（1 µs 以上纯属浪费）；
// 必须每一次都测，不能采样（比如要看尾部某个特定样本）；
// 裸金属 + 单 socket + 绑核，且 /proc/cpuinfo 里两个 flag 都在：
#ifndef HFT_METRICS_USE_TSC
// 默认不用 TSC：容器/VM 里可能没有 constant_tsc + nonstop_tsc，
// 跨 vCPU 迁移会让 TSC 跳变。确认两个 flag 都有后，再加
//   -DHFT_METRICS_USE_TSC=1
// 可把每次 HFT_SCOPE 再降约 20 ns。
#define HFT_METRICS_USE_TSC 0
#endif


#if defined(__GNUC__) || defined(__clang__)
#define HFT_METRICS_NOINLINE __attribute__((noinline))
#define HFT_METRICS_LIKELY(x) __builtin_expect(!!(x), 1)
#define HFT_METRICS_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define HFT_METRICS_NOINLINE
#define HFT_METRICS_LIKELY(x) (x)
#define HFT_METRICS_UNLIKELY(x) (x)
#endif

namespace hft {
namespace metrics {

// ---------------------------------------------------------------------------
// 基本类型与常量
// ---------------------------------------------------------------------------

using MetricId = std::uint32_t;

inline constexpr MetricId kInvalidMetric = 0xFFFFFFFFu;

// 512：全系统埋点（17 家交易所 × 若干段 + 4 个进程的 18 段划分 + 计数器）会超过 128。
// 上限本身不贵（只是注册表数组），贵的是 snapshot() 的稠密合并
// （≈ kMaxMetrics × kBuckets × 线程数），所以这个值和 kBuckets 要一起看。
inline constexpr std::uint32_t kMaxMetrics = 512;

inline constexpr std::uint32_t kSubBits = 3;     // 每个 2 倍区间 8 个子桶
inline constexpr std::uint32_t kSubCount = 1u << kSubBits;

// 256 桶 = 每指标每线程 2 KB（512 桶是 4 KB，13 个指标就 52 KB，超过 L1d）。
// 分辨率不变（仍是 12.5%），覆盖范围 0 ~ 2^34 tick：
//   MONOTONIC 下 = 0 ~ 17.2 s，足够覆盖任何"还算正常"的延时。
inline constexpr std::uint32_t kBuckets = 256;

// 桶 8..255 覆盖 2^3 .. 2^34，所以单次可记录的最大 tick 是 2^34。
// 超过这个值的样本一律当异常丢弃（见 record_ticks），原因见文件头「约束」。
inline constexpr std::uint32_t kMaxExp = 3u + (kBuckets - 8u) / kSubCount;  // 34
inline constexpr std::uint64_t kMaxTrackedTicks = 1ull << kMaxExp;          // 2^34

static_assert(kBuckets >= 16 && (kBuckets - 8u) % kSubCount == 0,
              "kBuckets 必须是 8 + 8 的整数倍");
static_assert(kMaxMetrics <= 512,
              "g_disabled_mask 只有 8 个 64-bit word；扩大 kMaxMetrics 时要同步扩展");

enum class MetricType : std::uint8_t {
    Counter = 0,
    Gauge = 1,
    Histogram = 2
};

inline const char* to_string(MetricType t) noexcept {
    switch (t) {
        case MetricType::Counter:   return "counter";
        case MetricType::Gauge:     return "gauge";
        case MetricType::Histogram: return "histogram";
    }
    return "unknown";
}

// 指标所属的时钟域。**注册时确定，之后只读**。
//   Mono  CLOCK_MONOTONIC，跨线程/跨进程可比
//   Fast  TSC（若启用），只允许同线程内相减
// 同一个名字用两个域注册是错误（Debug 下 abort），因为 tick 单位不同，
// 混在一个直方图里等于把 ns 和 cycle 加在一起。
enum class ClockKind : std::uint8_t {
    Mono = 0,
    Fast = 1
};

inline const char* to_string(ClockKind c) noexcept {
    switch (c) {
        case ClockKind::Mono: return "mono";
        case ClockKind::Fast: return "fast";
    }
    return "unknown";
}


// ---------------------------------------------------------------------------
// 时钟
// ---------------------------------------------------------------------------

namespace detail {

// ---- Fast 域：只给 HFT_SCOPE / HFT_NOW_FAST 用 ----
// 默认就是 MONOTONIC；开 TSC 后才变 rdtsc。
//
// ⚠️ TSC 只在 **x86_64/i386** 上生效（`#if ... && (defined(__x86_64__) || defined(__i386__))`），
//    arm64 上即使定义了 HFT_METRICS_USE_TSC=1 也还是 MONOTONIC。
//
// ⚠️ **TSC 不是"只能同进程用"**：它是 CPU 的计数器，同一台机器上所有进程读的是同一个值；
//    而 tsc_ns_per_tick() 是拿 CLOCK_MONOTONIC 标定的，CLOCK_MONOTONIC 又是全系统一个
//    ⇒ 同机两个进程换算出来的时间轴是一致的，A 的 tick 给 B 用是能对上的。
//    真正的边界是：① **跨机器绝对不行**（TSC 起点是各自的 boot）；
//    ② 同机多 socket 有风险（constant_tsc/nonstop_tsc 只保证频率不变，不保证各 socket 偏移一致）；
//    ③ VM 迁移 / vCPU 热插拔会让 TSC 跳变。
//    所以**库不提供跨进程的 TSC 路径**：HFT_SCOPE 只把耗时记进直方图、不暴露 t0，
//    HFT_NOW() 又固定是 Mono 域。要跨进程就老老实实用 Mono 域。
inline std::uint64_t fast_ticks() noexcept {
#if HFT_METRICS_USE_TSC && (defined(__x86_64__) || defined(__i386__))
    return ::hft::time::tsc_ticks();
#else
    return ::hft::time::mono_ticks();
#endif
}

inline double fast_ns_per_tick() {
#if HFT_METRICS_USE_TSC && (defined(__x86_64__) || defined(__i386__))
    return ::hft::time::tsc_ns_per_tick();
#else
    return ::hft::time::mono_ns_per_tick();
#endif
}

inline const char* fast_clock_name() noexcept {
#if HFT_METRICS_USE_TSC && (defined(__x86_64__) || defined(__i386__))
    return ::hft::time::tsc_clock_name();
#else
    return ::hft::time::mono_clock_name();
#endif
}

// ---- Mono 域：HFT_NOW / HFT_RECORD / HFT_RECORD_SINCE / HFT_RECORD_NS ----
// 永远是 CLOCK_MONOTONIC，**不受 HFT_METRICS_USE_TSC 影响**。
// 跨线程 / 跨进程 / 读共享内存里时间戳的差值只能用这个域。
//
// ⚠️ **混域比跨进程危险得多。** HFT_RECORD_SINCE(name, t0) 展开成 HFT_NOW() - t0，
//    用的是 Mono 域。如果 t0 是 HFT_NOW_FAST() 采的（开 TSC 后是 rdtsc，起点是 boot 时的
//    TSC 计数），两个域的数相减：
//      · 结果为负 ⇒ 无符号回绕成 ~2^64，被 kMaxTrackedTicks(2^34 ≈ 17 s) 拦掉，计入
//        metrics.rejected —— 至少你会看到计数；
//      · 结果为正且 < 17 s ⇒ **不拦**，而 TSC 3 GHz 对 mono 1 GHz 差 3 倍，你会得到
//        一个"看起来合理"的错值，悄悄污染 p50/p99，没有任何提示。
//    规则：**t0 用哪个宏采的，就用同一族的宏去减**。默认构建下两个域是同一个函数
//    （都读 CLOCK_MONOTONIC），所以混了也没事 —— 一旦开了 TSC 就不是了。
inline std::uint64_t mono_ticks() noexcept {
    return ::hft::time::mono_ticks();
}

inline double mono_ns_per_tick() noexcept {
    return ::hft::time::mono_ns_per_tick();
}

inline double ns_per_tick_for(ClockKind c) {
    return c == ClockKind::Fast ? fast_ns_per_tick() : mono_ns_per_tick();
}

inline const char* clock_name_for(ClockKind c) noexcept {
    return c == ClockKind::Fast ? fast_clock_name() : ::hft::time::mono_clock_name();
}

// 兼容旧名字（tests/metrics 和外部代码在用）：等价于 Fast 域。
inline std::uint64_t now_ticks() noexcept { return fast_ticks(); }
inline const char* clock_name() noexcept { return fast_clock_name(); }
inline double ns_per_tick() { return fast_ns_per_tick(); }

inline std::uint64_t wall_now_ns() noexcept {
    return ::hft::time::wall_ns();
}


// 把 tick 映射到对数线性桶：
//   0..7         => 精确值 0..7
//   >= 8         => 8 + (e-3)*8 + 低 3 位，e = floor(log2(v))
//   >= 2^34      => 饱和到最后一桶（正常情况下进不来，record_ticks 已经先拦掉了）
inline std::uint32_t bucket_of(std::uint64_t v) noexcept {
    if (v < 8) {
        return static_cast<std::uint32_t>(v);
    }
    const std::uint32_t e = 63u - static_cast<std::uint32_t>(__builtin_clzll(v));
    if (e >= kMaxExp) {
        return kBuckets - 1;
    }
    const std::uint32_t m = static_cast<std::uint32_t>((v >> (e - kSubBits)) & (kSubCount - 1u));
    return 8u + (e - 3u) * kSubCount + m;
}

inline std::uint64_t bucket_lower(std::uint32_t idx) noexcept {
    if (idx < 8) {
        return idx;
    }
    const std::uint32_t e = (idx - 8u) / kSubCount + 3u;
    const std::uint32_t m = (idx - 8u) % kSubCount;
    return (1ull << e) + (static_cast<std::uint64_t>(m) << (e - kSubBits));
}

// 上界直接取"下一桶的下界"，这样**构造上保证连续**：
// 旧实现在 e >= 63 时返回 ~0ull，导致 idx 489..511 这 23 个桶的 lower 比上一桶的
// upper 还大（不连续）。现在桶数收到 256、最大 e = 33，那一段永远走不到，
// 但用 next.lower 表达比"再算一遍公式 + 特判"更难写错。
inline std::uint64_t bucket_upper(std::uint32_t idx) noexcept {
    if (idx + 1u >= kBuckets) {
        return kMaxTrackedTicks;
    }
    return bucket_lower(idx + 1u);
}

inline double bucket_mid(std::uint32_t idx) noexcept {
    return (static_cast<double>(bucket_lower(idx)) +
            static_cast<double>(bucket_upper(idx))) * 0.5;
}


// ---------------------------------------------------------------------------
// 编译期字符串 hash（FNV-1a）
// ---------------------------------------------------------------------------

constexpr std::uint64_t fnv1a(const char* s) noexcept {
    std::uint64_t h = 1469598103934665603ull;
    while (*s != '\0') {
        h ^= static_cast<std::uint8_t>(*s++);
        h *= 1099511628211ull;
    }
    return h;
}

} // namespace detail

// ---------------------------------------------------------------------------
// 注册表
// ---------------------------------------------------------------------------

namespace detail {

struct MetricSlot {
    std::uint64_t hash{0};
    const char* name{nullptr};
    MetricType type{MetricType::Counter};
    ClockKind clock{ClockKind::Mono};
};

// 被拒样本数：① 单次 tick 超过 kMaxTrackedTicks（回绕 / 时钟混用）；
// ② 名字注册溢出。两者都会在 snapshot() 里作为 metrics.rejected 出现。
// 用全局原子而不是每线程分片：正常情况下它恒为 0，一旦非 0 就是需要人看的事。
inline std::atomic<std::uint64_t> g_rejected{0};
inline std::atomic<bool> g_overflow_warned{false};

struct Registry {
    std::mutex mu;
    MetricSlot slots[kMaxMetrics];
    std::uint32_t used{0};
};

inline Registry& registry() {
    // 故意不析构：避免静态析构顺序问题（线程退出时还可能访问）。
    static Registry* r = new Registry();
    return *r;
}

// 首次调用在调用点的 static 初始化里，走锁；之后只读。
//
// 溢出（超过 kMaxMetrics）时返回 kInvalidMetric，埋点变 no-op。
// ⚠️ 这是最坏的失败模式（你以为在测，其实没测），所以**第一次溢出必须响**：
// 打一条 stderr + 计数。调用方在启动后应该 snapshot 一次看 metrics.rejected。
inline MetricId metric_id(const char* name, MetricType type, ClockKind clock) {
    if (name == nullptr) {
        return kInvalidMetric;
    }
    const std::uint64_t h = fnv1a(name);
    Registry& r = registry();
    std::lock_guard<std::mutex> lk(r.mu);

    for (std::uint32_t i = 0; i < r.used; ++i) {
        if (r.slots[i].hash == h &&
            std::strcmp(r.slots[i].name, name) == 0) {
#ifndef NDEBUG
            if (r.slots[i].type != type) {
                std::fprintf(stderr,
                             "[metrics] name '%s' registered twice with different types\n",
                             name);
                std::abort();
            }
            if (r.slots[i].clock != clock) {
                std::fprintf(stderr,
                             "[metrics] name '%s' registered twice in different clock domains "
                             "(first=%s, now=%s) —— HFT_SCOPE 用 Fast 域，"
                             "HFT_RECORD/HFT_RECORD_SINCE 用 Mono 域，不能同名\n",
                             name, to_string(r.slots[i].clock), to_string(clock));
                std::abort();
            }
#endif
            return i;
        }
    }
    if (r.used >= kMaxMetrics) {
        g_rejected.fetch_add(1, std::memory_order_relaxed);
        if (!g_overflow_warned.exchange(true, std::memory_order_relaxed)) {
            std::fprintf(stderr,
                         "[metrics] metric 名字超过上限 %u，'%s' 及之后全部静默 no-op。"
                         "要么加 kMaxMetrics，要么把一次性探针删掉\n",
                         kMaxMetrics, name);
        }
        return kInvalidMetric;
    }
    const MetricId id = r.used++;
    r.slots[id].hash = h;
    r.slots[id].name = name;
    r.slots[id].type = type;
    r.slots[id].clock = clock;
    return id;
}

} // namespace detail


// ---------------------------------------------------------------------------
// 全局瞬时值
// ---------------------------------------------------------------------------
// **没有运行期开关。** 能不能测只由编译期 HFT_METRICS_ENABLED 决定（见文件头第 4 条）。
// 这样热路径里连一次判断都省掉，也不会出现"线上被谁悄悄关了"这种事。
// 要临时不测某个指标，就在编译期关掉整库，或者把那一行埋点删掉。

// Gauge 是全局瞬时值，不做 per-thread 合并。
inline std::atomic<std::int64_t> g_gauges[kMaxMetrics]{};

namespace detail {

inline MetricId find_metric(const char* name) {
    if (name == nullptr) {
        return kInvalidMetric;
    }
    const std::uint64_t h = fnv1a(name);
    Registry& r = registry();
    std::lock_guard<std::mutex> lk(r.mu);
    for (std::uint32_t i = 0; i < r.used; ++i) {
        if (r.slots[i].hash == h &&
            std::strcmp(r.slots[i].name, name) == 0) {
            return i;
        }
    }
    return kInvalidMetric;
}

} // namespace detail

// 查一个名字有没有被注册过（启动自检 / 报表用，不在热路径）。
inline MetricId find_metric(const char* name) {
    return detail::find_metric(name);
}

// ---------------------------------------------------------------------------
// 每线程存储
// ---------------------------------------------------------------------------

namespace detail {

struct Histogram {
    std::atomic<std::uint64_t> counts[kBuckets];

    Histogram() {
        for (std::uint32_t i = 0; i < kBuckets; ++i) {
            counts[i].store(0, std::memory_order_relaxed);
        }
    }
};

struct ThreadShard {
    std::atomic<std::uint64_t> counters[kMaxMetrics];
    std::atomic<Histogram*> hists[kMaxMetrics];
    std::atomic<std::uint64_t> hist_count[kMaxMetrics];
    std::atomic<std::uint64_t> hist_sum[kMaxMetrics];
    std::atomic<std::uint64_t> hist_min[kMaxMetrics];
    std::atomic<std::uint64_t> hist_max[kMaxMetrics];

    ThreadShard() {
        for (std::uint32_t i = 0; i < kMaxMetrics; ++i) {
            counters[i].store(0, std::memory_order_relaxed);
            hists[i].store(nullptr, std::memory_order_relaxed);
            hist_count[i].store(0, std::memory_order_relaxed);
            hist_sum[i].store(0, std::memory_order_relaxed);
            hist_min[i].store(~0ull, std::memory_order_relaxed);
            hist_max[i].store(0, std::memory_order_relaxed);
        }
    }
};

struct ShardList {
    std::mutex mu;
    std::vector<ThreadShard*> live;

    // 线程退出时把它的数据并到这里，保证数据不丢。
    std::atomic<std::uint64_t> retired_counters[kMaxMetrics];
    std::atomic<std::uint64_t> retired_count[kMaxMetrics];
    std::atomic<std::uint64_t> retired_sum[kMaxMetrics];
    std::atomic<std::uint64_t> retired_min[kMaxMetrics];
    std::atomic<std::uint64_t> retired_max[kMaxMetrics];
    std::unique_ptr<Histogram> retired_hist[kMaxMetrics];

    ShardList() {
        for (std::uint32_t i = 0; i < kMaxMetrics; ++i) {
            retired_counters[i].store(0, std::memory_order_relaxed);
            retired_count[i].store(0, std::memory_order_relaxed);
            retired_sum[i].store(0, std::memory_order_relaxed);
            retired_min[i].store(~0ull, std::memory_order_relaxed);
            retired_max[i].store(0, std::memory_order_relaxed);
        }
    }
};

inline ShardList& shard_list() {
    // 故意不析构。
    static ShardList* s = new ShardList();
    return *s;
}

struct ThreadShardHolder {
    ThreadShard* shard{nullptr};
    ~ThreadShardHolder();
};

inline thread_local ThreadShardHolder t_holder;

HFT_METRICS_NOINLINE inline ThreadShard* create_shard() {
    auto* s = new ThreadShard();
    {
        ShardList& sl = shard_list();
        std::lock_guard<std::mutex> lk(sl.mu);
        sl.live.push_back(s);
    }
    t_holder.shard = s;
    return s;
}

HFT_METRICS_NOINLINE inline Histogram* ensure_hist(ThreadShard& s, MetricId id) {
    auto* h = new Histogram();
    s.hists[id].store(h, std::memory_order_release);
    return h;
}

inline void merge_into_retired(ShardList& sl, ThreadShard* s) {
    for (std::uint32_t i = 0; i < kMaxMetrics; ++i) {
        const std::uint64_t c = s->counters[i].load(std::memory_order_relaxed);
        if (c != 0) {
            sl.retired_counters[i].fetch_add(c, std::memory_order_relaxed);
        }

        const std::uint64_t n = s->hist_count[i].load(std::memory_order_relaxed);
        if (n == 0) {
            continue;
        }
        sl.retired_count[i].fetch_add(n, std::memory_order_relaxed);
        sl.retired_sum[i].fetch_add(
            s->hist_sum[i].load(std::memory_order_relaxed),
            std::memory_order_relaxed);

        const std::uint64_t mn = s->hist_min[i].load(std::memory_order_relaxed);
        const std::uint64_t mx = s->hist_max[i].load(std::memory_order_relaxed);
        if (mn < sl.retired_min[i].load(std::memory_order_relaxed)) {
            sl.retired_min[i].store(mn, std::memory_order_relaxed);
        }
        if (mx > sl.retired_max[i].load(std::memory_order_relaxed)) {
            sl.retired_max[i].store(mx, std::memory_order_relaxed);
        }

        Histogram* h = s->hists[i].load(std::memory_order_relaxed);
        if (h == nullptr) {
            continue;
        }
        if (!sl.retired_hist[i]) {
            sl.retired_hist[i].reset(new Histogram());
        }
        Histogram* dst = sl.retired_hist[i].get();
        for (std::uint32_t b = 0; b < kBuckets; ++b) {
            const std::uint64_t v = h->counts[b].load(std::memory_order_relaxed);
            if (v != 0) {
                dst->counts[b].fetch_add(v, std::memory_order_relaxed);
            }
        }
    }
}

HFT_METRICS_NOINLINE inline void destroy_shard(ThreadShard* s) {
    if (s == nullptr) {
        return;
    }
    ShardList& sl = shard_list();
    {
        std::lock_guard<std::mutex> lk(sl.mu);
        for (auto it = sl.live.begin(); it != sl.live.end(); ++it) {
            if (*it == s) {
                sl.live.erase(it);
                break;
            }
        }
        merge_into_retired(sl, s);
    }
    for (std::uint32_t i = 0; i < kMaxMetrics; ++i) {
        delete s->hists[i].load(std::memory_order_relaxed);
    }
    delete s;
}

inline ThreadShardHolder::~ThreadShardHolder() {
    destroy_shard(shard);
    shard = nullptr;
}

inline ThreadShard& shard() {
    ThreadShard* s = t_holder.shard;
    if (HFT_METRICS_UNLIKELY(s == nullptr)) {
        s = create_shard();
    }
    return *s;
}

} // namespace detail

// ---------------------------------------------------------------------------
// 记录原语
// ---------------------------------------------------------------------------

namespace detail {

inline void add_counter(MetricId id, std::uint64_t v) noexcept {
    if (id == kInvalidMetric) {
        return;
    }
    ThreadShard& s = shard();
    const std::uint64_t c = s.counters[id].load(std::memory_order_relaxed);
    s.counters[id].store(c + v, std::memory_order_relaxed);
}

inline void set_gauge(MetricId id, std::int64_t v) noexcept {
    if (id == kInvalidMetric) {
        return;
    }
    g_gauges[id].store(v, std::memory_order_relaxed);
}

inline void record_ticks(MetricId id, std::uint64_t ticks) noexcept {
    if (id == kInvalidMetric) {
        return;
    }

    // 异常样本拦截。两个来源都会走到这里：
    //   ① 无符号回绕：now < t0（跨核 TSC 偏移、mono 与 wall 混用、t0 来自别的进程）
    //      ⇒ 差值是一个接近 2^64 的巨值；
    //   ② 真的卡了十几秒（这时这条链路已经废了，不需要进分布）。
    // 不拦的后果：一次坏样本就让 max 和 p999 永久报废（实测能到 1.8e19 ns = 585 年），
    // 而且没有任何提示。宁可丢样本 + 计数，也不要一个永远错下去的指标。
    if (HFT_METRICS_UNLIKELY(ticks >= kMaxTrackedTicks)) {
        g_rejected.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    ThreadShard& s = shard();
    Histogram* h = s.hists[id].load(std::memory_order_relaxed);
    if (HFT_METRICS_UNLIKELY(h == nullptr)) {
        h = ensure_hist(s, id);
    }

    const std::uint32_t b = bucket_of(ticks);
    const std::uint64_t c = h->counts[b].load(std::memory_order_relaxed);
    h->counts[b].store(c + 1, std::memory_order_relaxed);

    const std::uint64_t n = s.hist_count[id].load(std::memory_order_relaxed);
    s.hist_count[id].store(n + 1, std::memory_order_relaxed);

    const std::uint64_t sum = s.hist_sum[id].load(std::memory_order_relaxed);
    s.hist_sum[id].store(sum + ticks, std::memory_order_relaxed);

    const std::uint64_t mn = s.hist_min[id].load(std::memory_order_relaxed);
    if (ticks < mn) {
        s.hist_min[id].store(ticks, std::memory_order_relaxed);
    }
    const std::uint64_t mx = s.hist_max[id].load(std::memory_order_relaxed);
    if (ticks > mx) {
        s.hist_max[id].store(ticks, std::memory_order_relaxed);
    }
}

inline void record_ns(MetricId id, double ns) noexcept {
    // 只有 Mono 域的指标会走这里（HFT_RECORD_NS），所以固定用 mono 的比例。
    const double t = ns / mono_ns_per_tick();
    record_ticks(id, t <= 0.0 ? 0ull : static_cast<std::uint64_t>(t + 0.5));
}

} // namespace detail

// ---------------------------------------------------------------------------
// RAII 段
// ---------------------------------------------------------------------------

class Scope {
public:
    // 段内计时固定走 Fast 域（默认 MONOTONIC，开 TSC 后是 rdtsc）。
    // Fast 域只允许"同一线程内 t1 - t0"：跨核迁移时 TSC 可能倒退，
    // 那时 t1 - t0 会回绕成巨值，由 record_ticks 的钳制丢弃并计入 metrics.rejected。
    Scope(MetricId id, bool sampled = true) noexcept
        : id_((sampled && id != kInvalidMetric) ? id : kInvalidMetric),
          t0_(id_ != kInvalidMetric ? detail::fast_ticks() : 0) {}

    ~Scope() {
        if (id_ != kInvalidMetric) {
            detail::record_ticks(id_, detail::fast_ticks() - t0_);
        }
    }

    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    MetricId id_;
    std::uint64_t t0_;
};

// ---------------------------------------------------------------------------
// Snapshot / 百分位 / 导出
// ---------------------------------------------------------------------------

struct MetricSnapshot {
    std::string name;
    MetricType type{MetricType::Counter};

    // 本指标所属的时钟域 + 换算比例。**每个指标自己带一份**：
    // 开了 TSC 之后 HFT_SCOPE 的指标是 Fast 域、HFT_RECORD 的指标是 Mono 域，
    // 两者的 ns/tick 不同，混用同一个比例会把一部分指标算错。
    ClockKind clock{ClockKind::Mono};
    double ns_per_tick{1.0};

    std::uint64_t count{0};
    std::uint64_t sum_ticks{0};
    std::uint64_t min_ticks{0};
    std::uint64_t max_ticks{0};

    double mean_ns{0.0};
    double min_ns{0.0};
    double max_ns{0.0};
    double p50_ns{0.0};
    double p90_ns{0.0};
    double p99_ns{0.0};
    double p999_ns{0.0};

    std::int64_t gauge{0};

    std::uint64_t buckets[kBuckets]{};
};

struct Snapshot {
    std::uint64_t ts_ns{0};
    double ns_per_tick{1.0};   // Mono 域的比例（兼容字段；每个指标另有自己的）

    std::vector<MetricSnapshot> metrics;

    std::string to_json(bool with_buckets = false) const;

    // with_header=true：时间戳行 + 列头 + 数据行（打到 stderr / 终端用）。
    // with_header=false：时间戳行 + 数据行（落文件时列头只在新建文件写一次）。
    // **时间戳行永远有** —— 否则一份文件里分不清哪几行是哪一秒的，事后无法分析。
    std::string to_text(bool with_header = true) const;
};

namespace detail {

inline double percentile_ns(const std::uint64_t* buckets,
                             std::uint64_t total,
                             double q,
                             double ns_per_tick) noexcept {
    if (total == 0) {
        return 0.0;
    }
    if (q <= 0.0) {
        q = 0.0;
    } else if (q >= 1.0) {
        q = 1.0;
    }

    std::uint64_t target = static_cast<std::uint64_t>(q * static_cast<double>(total));
    if (target >= total) {
        target = total - 1;
    }

    std::uint64_t acc = 0;
    for (std::uint32_t i = 0; i < kBuckets; ++i) {
        acc += buckets[i];
        if (acc > target) {
            return bucket_mid(i) * ns_per_tick;
        }
    }
    return bucket_mid(kBuckets - 1) * ns_per_tick;
}

// 根据 buckets 重新计算 count / min / max / mean / 百分位。
// 用于 diff()：两个累计 snapshot 相减后，分布必须重建，不能沿用累计值。
inline void recompute_stats(MetricSnapshot& m, double ns_per_tick) noexcept {
    if (m.type != MetricType::Histogram) {
        return;
    }

    std::uint64_t total = 0;
    int first = -1;
    int last = -1;
    for (std::uint32_t i = 0; i < kBuckets; ++i) {
        if (m.buckets[i] != 0) {
            if (first < 0) {
                first = static_cast<int>(i);
            }
            last = static_cast<int>(i);
            total += m.buckets[i];
        }
    }

    m.count = total;
    if (total == 0) {
        m.sum_ticks = 0;
        m.min_ticks = 0;
        m.max_ticks = 0;
        m.mean_ns = 0.0;
        m.min_ns = 0.0;
        m.max_ns = 0.0;
        m.p50_ns = 0.0;
        m.p90_ns = 0.0;
        m.p99_ns = 0.0;
        m.p999_ns = 0.0;
        return;
    }

    const std::uint32_t fi = static_cast<std::uint32_t>(first);
    const std::uint32_t li = static_cast<std::uint32_t>(last);
    m.min_ticks = bucket_lower(fi);
    m.max_ticks = bucket_upper(li);
    m.min_ns = static_cast<double>(m.min_ticks) * ns_per_tick;
    m.max_ns = static_cast<double>(m.max_ticks) * ns_per_tick;
    m.mean_ns = static_cast<double>(m.sum_ticks) /
                static_cast<double>(total) * ns_per_tick;
    m.p50_ns = percentile_ns(m.buckets, total, 0.50, ns_per_tick);
    m.p90_ns = percentile_ns(m.buckets, total, 0.90, ns_per_tick);
    m.p99_ns = percentile_ns(m.buckets, total, 0.99, ns_per_tick);
    m.p999_ns = percentile_ns(m.buckets, total, 0.999, ns_per_tick);
}

} // namespace detail

inline Snapshot snapshot() {
    Snapshot snap;
    snap.ts_ns = detail::wall_now_ns();
    snap.ns_per_tick = detail::ns_per_tick();

    auto& reg = detail::registry();
    std::uint32_t used = 0;
    {
        std::lock_guard<std::mutex> lk(reg.mu);
        used = reg.used;
    }

    std::vector<std::uint64_t> hist(static_cast<std::size_t>(used) * kBuckets, 0);
    std::vector<std::uint64_t> cnt(used, 0);
    std::vector<std::uint64_t> sum(used, 0);
    std::vector<std::uint64_t> mn(used, ~0ull);
    std::vector<std::uint64_t> mx(used, 0);
    std::vector<std::uint64_t> ncnt(used, 0);

    auto merge_shard = [&](const detail::ThreadShard* s) {
        for (std::uint32_t i = 0; i < used; ++i) {
            cnt[i] += s->counters[i].load(std::memory_order_relaxed);

            const std::uint64_t n = s->hist_count[i].load(std::memory_order_relaxed);
            if (n == 0) {
                continue;
            }
            ncnt[i] += n;
            sum[i] += s->hist_sum[i].load(std::memory_order_relaxed);

            const std::uint64_t a = s->hist_min[i].load(std::memory_order_relaxed);
            if (a < mn[i]) {
                mn[i] = a;
            }
            const std::uint64_t b = s->hist_max[i].load(std::memory_order_relaxed);
            if (b > mx[i]) {
                mx[i] = b;
            }

            const detail::Histogram* h =
                s->hists[i].load(std::memory_order_acquire);
            if (h != nullptr) {
                std::uint64_t* row = hist.data() + static_cast<std::size_t>(i) * kBuckets;
                for (std::uint32_t k = 0; k < kBuckets; ++k) {
                    row[k] += h->counts[k].load(std::memory_order_relaxed);
                }
            }
        }
    };

    {
        detail::ShardList& sl = detail::shard_list();
        std::lock_guard<std::mutex> lk(sl.mu);

        for (std::uint32_t i = 0; i < used; ++i) {
            cnt[i] += sl.retired_counters[i].load(std::memory_order_relaxed);

            const std::uint64_t n = sl.retired_count[i].load(std::memory_order_relaxed);
            if (n == 0) {
                continue;
            }
            ncnt[i] += n;
            sum[i] += sl.retired_sum[i].load(std::memory_order_relaxed);

            const std::uint64_t a = sl.retired_min[i].load(std::memory_order_relaxed);
            if (a < mn[i]) {
                mn[i] = a;
            }
            const std::uint64_t b = sl.retired_max[i].load(std::memory_order_relaxed);
            if (b > mx[i]) {
                mx[i] = b;
            }

            if (sl.retired_hist[i]) {
                std::uint64_t* row = hist.data() + static_cast<std::size_t>(i) * kBuckets;
                const detail::Histogram* h = sl.retired_hist[i].get();
                for (std::uint32_t k = 0; k < kBuckets; ++k) {
                    row[k] += h->counts[k].load(std::memory_order_relaxed);
                }
            }
        }

        for (const detail::ThreadShard* s : sl.live) {
            merge_shard(s);
        }
    }

    snap.metrics.reserve(used + 1);
    for (std::uint32_t i = 0; i < used; ++i) {
        MetricSnapshot m;
        m.name = reg.slots[i].name != nullptr ? reg.slots[i].name : "";
        m.type = reg.slots[i].type;
        m.clock = reg.slots[i].clock;
        m.ns_per_tick = detail::ns_per_tick_for(m.clock);

        if (m.type == MetricType::Histogram) {
            m.count = ncnt[i];
            m.sum_ticks = sum[i];
            if (m.count != 0) {
                m.min_ticks = (mn[i] == ~0ull) ? 0 : mn[i];
                m.max_ticks = mx[i];
                m.mean_ns = static_cast<double>(m.sum_ticks) /
                            static_cast<double>(m.count) * m.ns_per_tick;
                m.min_ns = static_cast<double>(m.min_ticks) * m.ns_per_tick;
                m.max_ns = static_cast<double>(m.max_ticks) * m.ns_per_tick;

                const std::uint64_t* row =
                    hist.data() + static_cast<std::size_t>(i) * kBuckets;
                m.p50_ns = detail::percentile_ns(row, m.count, 0.50, m.ns_per_tick);
                m.p90_ns = detail::percentile_ns(row, m.count, 0.90, m.ns_per_tick);
                m.p99_ns = detail::percentile_ns(row, m.count, 0.99, m.ns_per_tick);
                m.p999_ns = detail::percentile_ns(row, m.count, 0.999, m.ns_per_tick);
                std::memcpy(m.buckets, row, sizeof(m.buckets));
            }
        } else if (m.type == MetricType::Gauge) {
            m.count = 1;
            m.gauge = g_gauges[i].load(std::memory_order_relaxed);
        } else {
            m.count = cnt[i];
        }

        snap.metrics.push_back(std::move(m));
    }

    // 被拒样本（异常值 / 注册溢出）。只有非 0 时才出现，出现就说明有东西要查：
    // 非 0 的 metrics.rejected 意味着"你看到的分位是剔掉坏样本之后的"，
    // 而坏样本本身（回绕、时钟混用、超过 512 个指标）才是根因。
    const std::uint64_t rejected = detail::g_rejected.load(std::memory_order_relaxed);
    if (rejected != 0) {
        MetricSnapshot m;
        m.name = "metrics.rejected";
        m.type = MetricType::Counter;
        m.count = rejected;
        snap.metrics.push_back(std::move(m));
    }

    return snap;
}

// 窗口分布：later - earlier。
// 计数器做差；直方图逐桶相减并重建百分位；gauge 取 later 的值。
// 两个 snapshot 都必须是同一个进程里、later 晚于 earlier。
inline Snapshot diff(const Snapshot& earlier, const Snapshot& later) {
    Snapshot out;
    out.ts_ns = later.ts_ns;
    out.ns_per_tick = later.ns_per_tick;
    out.metrics.reserve(later.metrics.size());

    for (const MetricSnapshot& m : later.metrics) {
        const MetricSnapshot* p = nullptr;
        for (const MetricSnapshot& x : earlier.metrics) {
            if (x.name == m.name) {
                p = &x;
                break;
            }
        }

        MetricSnapshot d = m;
        if (p != nullptr && p->type == m.type) {
            if (m.count >= p->count) {
                d.count = m.count - p->count;
                d.sum_ticks = m.sum_ticks >= p->sum_ticks
                                  ? m.sum_ticks - p->sum_ticks
                                  : 0;
                for (std::uint32_t i = 0; i < kBuckets; ++i) {
                    d.buckets[i] = m.buckets[i] >= p->buckets[i]
                                       ? m.buckets[i] - p->buckets[i]
                                       : 0;
                }
            } else {
                // 计数器被重置 / 新 metric：把 later 当作新窗口。
                d = m;
            }
        }
        // d = m 已经把 clock / ns_per_tick 一起带过来了，用 d 自己的比例换算。
        detail::recompute_stats(d, d.ns_per_tick);
        out.metrics.push_back(std::move(d));
    }

    return out;
}

// ---------------------------------------------------------------------------
// 重置 / 预热
// ---------------------------------------------------------------------------

namespace detail {

inline void zero_shard(ThreadShard& s) noexcept {
    for (std::uint32_t i = 0; i < kMaxMetrics; ++i) {
        s.counters[i].store(0, std::memory_order_relaxed);
        s.hist_count[i].store(0, std::memory_order_relaxed);
        s.hist_sum[i].store(0, std::memory_order_relaxed);
        s.hist_min[i].store(~0ull, std::memory_order_relaxed);
        s.hist_max[i].store(0, std::memory_order_relaxed);
        Histogram* h = s.hists[i].load(std::memory_order_relaxed);
        if (h != nullptr) {
            // 保留已分配的直方图，只清零 —— 避免 reset 之后第一次埋点又 malloc。
            for (std::uint32_t b = 0; b < kBuckets; ++b) {
                h->counts[b].store(0, std::memory_order_relaxed);
            }
        }
    }
}

} // namespace detail

// 把当前所有分片的计数与直方图清零（gauge 不动：它是瞬时值，清了没有意义）。
//
// 用途：跑完 warmup（连接建立、合约信息加载、页错误、分位数预热）之后"从现在开始算"。
// ⚠️ 热路径是无锁的，所以本函数与并发记录之间存在竞争：可能丢一次自增，
//    或有一条样本落进正在被清的桶。**只在启动阶段或明确的时间点调用一次**，
//    不要周期性调用 —— 周期性需求应该用 Reporter 的窗口模式，那是无竞争的。
inline void reset() {
    detail::ShardList& sl = detail::shard_list();
    std::lock_guard<std::mutex> lk(sl.mu);

    for (detail::ThreadShard* s : sl.live) {
        detail::zero_shard(*s);
    }
    for (std::uint32_t i = 0; i < kMaxMetrics; ++i) {
        sl.retired_counters[i].store(0, std::memory_order_relaxed);
        sl.retired_count[i].store(0, std::memory_order_relaxed);
        sl.retired_sum[i].store(0, std::memory_order_relaxed);
        sl.retired_min[i].store(~0ull, std::memory_order_relaxed);
        sl.retired_max[i].store(0, std::memory_order_relaxed);
        sl.retired_hist[i].reset();
    }
    detail::g_rejected.store(0, std::memory_order_relaxed);
}

// 预分配**当前线程**的运行期内存：24 KB 的分片 + 每个已注册直方图 2 KB。
//
// 为什么要它：分片和直方图都是**首次埋点时惰性分配**的。如果某个 metric 的第一条样本
// 出现在 09:30:00.000（撤单 / 报错 / 平仓这类少见分支），那一次 malloc + 清零就是
// 2–4 µs 的停顿，正好落在最不该停顿的位置。提前分配掉，热路径上就真的没有 malloc 了。
//
// 用法：**每个会埋点的线程**在开始干活之前各自调一次 —— 分片是 per-thread 的，
// 主线程调了不代表交易线程调了。没调也不会错，只是第一次埋点会停顿。
//
// ⚠️ 只能预分配**已经注册过**的 metric（注册发生在宏第一次执行时）。想要"零惰性分配"，
//    就在线程 init 里把每个 metric 各执行一次（自然注册），再调 warmup_thread()；
//    否则每个 metric 首次命中时仍会有一次 2 KB 分配 —— 那一次是 0.3 µs，可以接受。
inline void warmup_thread() {
    detail::ThreadShard& s = detail::shard();   // 24 KB，create_shard
    detail::Registry& r = detail::registry();
    std::lock_guard<std::mutex> lk(r.mu);
    for (std::uint32_t i = 0; i < r.used; ++i) {
        if (r.slots[i].type == MetricType::Histogram &&
            s.hists[i].load(std::memory_order_relaxed) == nullptr) {
            detail::ensure_hist(s, i);          // 2 KB
        }
    }
}

// 显式预热。两件事：
//   1. 时钟校准 —— 启用 TSC 时首次换算会做一次 20 ms 的 nanosleep，
//      如果那一次发生在 HFT_SCOPE 里，就是交易线程上凭空 20 ms 停顿；
//   2. warmup_thread() —— 把当前线程的分片和直方图分配掉。
// **在起任何工作线程之前调用一次**；每个工作线程自己再调一次 warmup_thread()。
// 没调用也不会错，只是第一次埋点会停顿。
inline void warmup() {
    (void)detail::fast_ns_per_tick();
    (void)detail::mono_ns_per_tick();
    warmup_thread();
}

// ---------------------------------------------------------------------------
// 文本 / JSON 导出（报告路径，允许分配和格式化）
// ---------------------------------------------------------------------------

namespace detail {

inline void append_escaped(std::string& out, const std::string& s) {
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:   out += c; break;
        }
    }
}

inline void append_u64(std::string& out, std::uint64_t v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%llu", static_cast<unsigned long long>(v));
    out += buf;
}

inline void append_i64(std::string& out, std::int64_t v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(v));
    out += buf;
}

inline void append_double(std::string& out, double v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.3f", v);
    out += buf;
}

// 墙钟 ns → "YYYY-MM-DD HH:MM:SS.mmm"（本地时区）。
// 用 localtime_r 而不是 localtime：snapshot() 可能在任意线程被调用，
// localtime 内部用静态缓冲区，多线程同时调用会互相覆盖。
inline void format_wall_ns(std::int64_t ns, char* buf, std::size_t n) {
    const std::time_t sec = static_cast<std::time_t>(ns / 1000000000LL);
    const long ms = static_cast<long>((ns % 1000000000LL) / 1000000LL);
    std::tm tm{};
    localtime_r(&sec, &tm);
    std::snprintf(buf, n, "%04d-%02d-%02d %02d:%02d:%02d.%03ld",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec, ms);
}

} // namespace detail

inline std::string Snapshot::to_json(bool with_buckets) const {
    std::string out;
    out.reserve(256 + metrics.size() * (with_buckets ? 1024 : 256));
    out += "{\"ts_ns\":";
    detail::append_u64(out, ts_ns);
    out += ",\"ns_per_tick\":";
    detail::append_double(out, ns_per_tick);
    out += ",\"nbuckets\":";
    detail::append_u64(out, kBuckets);
    out += ",\"metrics\":[";

    bool first = true;
    for (const MetricSnapshot& m : metrics) {
        if (!first) {
            out += ',';
        }
        first = false;

        out += "{\"name\":\"";
        detail::append_escaped(out, m.name);
        out += "\",\"type\":\"";
        out += to_string(m.type);
        out += "\",\"clock\":\"";
        out += to_string(m.clock);
        out += "\",\"count\":";
        detail::append_u64(out, m.count);

        if (m.type == MetricType::Gauge) {
            out += ",\"value\":";
            detail::append_i64(out, m.gauge);
        } else if (m.type == MetricType::Histogram) {
            out += ",\"ns_per_tick\":";
            detail::append_double(out, m.ns_per_tick);
            out += ",\"mean_ns\":";
            detail::append_double(out, m.mean_ns);
            out += ",\"min_ns\":";
            detail::append_double(out, m.min_ns);
            out += ",\"max_ns\":";
            detail::append_double(out, m.max_ns);
            out += ",\"p50_ns\":";
            detail::append_double(out, m.p50_ns);
            out += ",\"p90_ns\":";
            detail::append_double(out, m.p90_ns);
            out += ",\"p99_ns\":";
            detail::append_double(out, m.p99_ns);
            out += ",\"p999_ns\":";
            detail::append_double(out, m.p999_ns);

            // 直方图桶：只导非零桶，格式是 [idx,count, idx,count, ...]。
            // 拿到它就能离线重算任意分位、画分布、算"超过 X ns 的比例"，
            // 也能和 perf 计数器按时间对齐看尾部形状。
            // 桶 i 的区间是 [bucket_lower(i), bucket_lower(i+1))，
            // 0..7 精确，>=8 时 e=(i-8)/8+3、m=(i-8)%8、lower=2^e + m*2^(e-3)。
            if (with_buckets) {
                out += ",\"buckets\":[";
                bool bfirst = true;
                for (std::uint32_t i = 0; i < kBuckets; ++i) {
                    if (m.buckets[i] == 0) {
                        continue;
                    }
                    if (!bfirst) {
                        out += ',';
                    }
                    bfirst = false;
                    detail::append_u64(out, i);
                    out += ',';
                    detail::append_u64(out, m.buckets[i]);
                }
                out += ']';
            }
        }
        out += '}';
    }
    out += "]}";
    return out;
}

inline std::string Snapshot::to_text(bool with_header) const {
    std::string out;
    out.reserve(256 + metrics.size() * 160);

    // 列宽 = 最长可能的名字。现在是 db 按交易所拆开的网络耗时：
    //   db.netdelay.bitunix.usdt_futures.depth20 = 40 字符
    // 名字超过列宽不会截断，只是把这一行的数字列顶歪 —— 表格就对不齐了，
    // 所以这个数跟命名方案是绑死的，改名字前缀/分段时要一起改。
    char line[256];

    // 时间戳行。每块都有 —— 130 B 的列头可以省，这 30 B 不能省：
    // 没有它，一个文件里几十万行数据分不清是哪一秒的。
    char tsbuf[40];
    detail::format_wall_ns(static_cast<std::int64_t>(ts_ns), tsbuf, sizeof(tsbuf));
    std::snprintf(line, sizeof(line), "# %s\n", tsbuf);
    out += line;

    if (with_header) {
        std::snprintf(line, sizeof(line),
                      "%-40s %-10s %12s %12s %12s %12s %12s %12s\n",
                      "name", "type", "count", "p50_ns", "p90_ns",
                      "p99_ns", "p999_ns", "max_ns");
        out += line;
    }

    for (const MetricSnapshot& m : metrics) {
        if (m.type == MetricType::Histogram) {
            std::snprintf(line, sizeof(line),
                          "%-40s %-10s %12llu %12.1f %12.1f %12.1f %12.1f %12.1f\n",
                          m.name.c_str(), to_string(m.type),
                          static_cast<unsigned long long>(m.count),
                          m.p50_ns, m.p90_ns, m.p99_ns, m.p999_ns, m.max_ns);
        } else if (m.type == MetricType::Gauge) {
            std::snprintf(line, sizeof(line),
                          "%-40s %-10s %12lld\n",
                          m.name.c_str(), to_string(m.type),
                          static_cast<long long>(m.gauge));
        } else {
            std::snprintf(line, sizeof(line),
                          "%-40s %-10s %12llu\n",
                          m.name.c_str(), to_string(m.type),
                          static_cast<unsigned long long>(m.count));
        }
        out += line;
    }
    return out;
}

// ---------------------------------------------------------------------------
// 文本文件落地
// ---------------------------------------------------------------------------
// 解决的问题：一个 1 Hz 的输出会 21 MB/天、一年 7.7 GB，而且名字匹配不上运维
// 现有的 {tag}_{YYYYMMDD}.log 清理规则，没人会删它。三道闸：
//
//   ① **按天命名** metrics_YYYYMMDD.log —— 跨天自动换新文件，命名和运维现有规则一致；
//   ② **按大小轮转** 超过 max_bytes 就地滚成 .1（只留一份）
//      ⇒ 单文件有上限、目录有上限，跑一年也不会涨；
//   ③ **列头只写一次** 每个新文件的第一块带列头，之后只写 "# 时间戳" + 数据行。
//      列头 122 B，占一块的三分之一；时间戳那 30 B 必须留（否则分不清哪一秒）。
//
// 为什么不做成"追加进主 log"：fmtlog 自己持 fd 和异步缓冲，跨天是 setLogFile 换新文件；
// 这边再 fopen 同一个路径就是第二个 writer、第二个偏移，而且跨天之后还钉在昨天那个文件上。
//
// 用法：
//   hft::metrics::TextFileSink sink(logPath, "metrics", 8u << 20);
//   hft::metrics::Poller p(10000000000LL, [&](const hft::metrics::Snapshot& s) {
//       if (!sink.write(s)) std::fputs(s.to_text().c_str(), stderr);   // 打不开就退到 stderr
//   });
//   while (1) { p.poll(); usleep(1000); }
class TextFileSink {
public:
    // dir 为空表示当前目录。prefix 只影响文件名，默认 "metrics" ⇒ metrics_20261101.log。
    explicit TextFileSink(std::string dir, std::string prefix = "metrics",
                          std::uint64_t max_bytes = 8u << 20)
        : dir_(std::move(dir)),
          prefix_(std::move(prefix)),
          max_bytes_(max_bytes) {}

    ~TextFileSink() { close(); }

    TextFileSink(const TextFileSink&) = delete;
    TextFileSink& operator=(const TextFileSink&) = delete;

    // 写一块。返回 false = 没写进去（目录不存在 / 没权限 / 写失败），
    // **调用方自己决定退到哪儿**（一般是 stderr）。统计失败不能把业务进程拖死。
    bool write(const Snapshot& s) {
        // 一个指标都没有 ⇒ 不写。两种情况会走到这里：编译期关掉了统计
        // （HFT_METRICS_ENABLED=0），或者进程刚起来还没注册任何指标。
        // 不挡的话会每 10 s 往文件里写一行表头，纯垃圾。
        if (s.metrics.empty()) {
            return true;
        }

        const std::time_t now = std::time(nullptr);
        std::tm tm{};
        localtime_r(&now, &tm);

        // 跨天 / 首次 / 上次打开失败 ⇒ (重新)打开今天的文件
        if (fp_ == nullptr || tm.tm_year != year_ || tm.tm_mon != mon_ || tm.tm_mday != mday_) {
            if (!open_for(tm)) {
                return false;
            }
        }

        std::string block = s.to_text(need_header_);

        // 轮转判断放在生成之后：要知道这一块多长才判断得准。
        // bytes_ == 0 时不滚 —— 否则 max_bytes 配得比一块还小时会无限滚。
        if (bytes_ != 0 && bytes_ + block.size() > max_bytes_) {
            rotate();
            if (fp_ == nullptr) {
                return false;
            }
            block = s.to_text(need_header_);   // 新文件要重新带列头
        }

        const std::size_t n = std::fwrite(block.data(), 1, block.size(), fp_);
        if (n != block.size()) {
            close();          // 磁盘满 / 管道断：关掉，下次 write 重新打开
            return false;
        }
        // 立刻刷。掉电时最多丢一块，而不是丢一个缓冲区（默认 4 KB ≈ 26 块）。
        std::fflush(fp_);
        bytes_ += static_cast<std::uint64_t>(n);
        need_header_ = false;
        return true;
    }

    bool ok() const { return fp_ != nullptr; }
    const std::string& path() const { return path_; }
    std::uint64_t bytes() const { return bytes_; }

private:
    bool open_for(const std::tm& tm) {
        close();
        year_ = tm.tm_year;
        mon_ = tm.tm_mon;
        mday_ = tm.tm_mday;

        char name[96];
        std::snprintf(name, sizeof(name), "%s_%04d%02d%02d.log",
                      prefix_.c_str(), tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
        path_ = dir_.empty() ? std::string(name) : (dir_ + "/" + name);

        fp_ = std::fopen(path_.c_str(), "a");
        if (fp_ == nullptr) {
            return false;
        }
        // "a" 模式的初始读位置不保证在末尾，显式 seek 才能拿到真实大小
        // （重启后要接着已有的文件写，列头也不能重写）。
        std::fseek(fp_, 0, SEEK_END);
        const long sz = std::ftell(fp_);
        bytes_ = sz > 0 ? static_cast<std::uint64_t>(sz) : 0;
        need_header_ = (bytes_ == 0);
        return true;
    }

    // 就地滚：metrics_YYYYMMDD.log → .1，只留一份。滚完用同名重建。
    void rotate() {
        close();
        const std::string bak = path_ + ".1";
        std::remove(bak.c_str());                    // 上一轮的 .1 直接丢
        std::rename(path_.c_str(), bak.c_str());
        fp_ = std::fopen(path_.c_str(), "a");
        bytes_ = 0;
        need_header_ = true;
    }

    void close() {
        if (fp_ != nullptr) {
            std::fclose(fp_);
            fp_ = nullptr;
        }
    }

    std::string dir_;
    std::string prefix_;
    std::uint64_t max_bytes_;
    std::string path_;
    std::FILE* fp_{nullptr};
    std::uint64_t bytes_{0};
    bool need_header_{true};
    int year_{-1};
    int mon_{-1};
    int mday_{-1};
};

// ---------------------------------------------------------------------------
// 周期采集
// ---------------------------------------------------------------------------
// 两种用法，**优先第一种**：
//   ① Poller / report_if_due()：不占线程，挂在已有的 1 Hz 定时器或 while 循环里。
//      "没到点"这条路径只有一次比较，不分配、不加锁、不格式化。
//   ② Reporter：自己起一个线程 sleep_until。只在进程里完全没有定时器时才用。

class Poller {
public:
    using Callback = std::function<void(const Snapshot&)>;

    // period_ns 默认 1 s；windowed=true 表示回调拿到的是"这一周期内的分布"。
    explicit Poller(std::int64_t period_ns = 1000000000LL,
                    Callback cb = nullptr,
                    bool windowed = true)
        : period_ns_(period_ns > 0 ? period_ns : 1000000000LL),
          cb_(std::move(cb)),
          windowed_(windowed) {}

    void set_callback(Callback cb) { cb_ = std::move(cb); }

    // 到点返回 true（并回调一次）。
    //
    // now_ns 传 0 表示"自己读钟"；已经有 mono 时间戳的地方（例如 OnTimer 的 nowUs）
    // 直接传进来，省掉这一次 clock_gettime。
    //
    // ⚠️ 没到点时**什么都不做**（一次比较就返回）；到点时才会 snapshot + 格式化
    //    （会 malloc、会加锁）—— 所以只能在非热路径调。
    bool poll(std::int64_t now_ns = 0) {
        if (now_ns == 0) {
            now_ns = static_cast<std::int64_t>(hft::time::mono_ns());
        }
        if (next_ns_ == 0) {
            // 第一圈只建立基线：立刻报一次会拿到"进程刚启动"的空分布。
            next_ns_ = now_ns + period_ns_;
            return false;
        }
        if (now_ns < next_ns_) {
            return false;
        }
        // 追赶策略：卡顿之后**只报一次**，然后把下一次推到"现在 + period"。
        // 不这么做的话，一次 30 s 的停顿之后会连报 30 次，每次都 malloc。
        next_ns_ = now_ns + period_ns_;

        Snapshot cur = snapshot();
        if (!windowed_) {
            emit(cur);
            return true;
        }
        if (!has_prev_) {
            // 理论上走不到（poll 第一圈就建了基线），留作 reset() 后的兜底。
            prev_ = std::move(cur);
            has_prev_ = true;
            emit(prev_);
            return true;
        }
        Snapshot d = diff(prev_, cur);
        prev_ = std::move(cur);
        emit(d);
        return true;
    }

    // 重新开始计时（例如配置重载后想让下一个周期从"现在"算起）。
    void reset() {
        next_ns_ = 0;
        has_prev_ = false;
        prev_ = Snapshot{};
    }

private:
    void emit(const Snapshot& s) {
        if (!cb_) {
            return;
        }
        try {
            cb_(s);
        } catch (...) {
            // 打印/上报失败不能把主循环带崩。
        }
    }

    std::int64_t period_ns_;
    std::int64_t next_ns_{0};
    Callback cb_;
    bool windowed_{true};
    bool has_prev_{false};
    Snapshot prev_;
};

// 最省事的一种：全进程一个 1 s 的 Poller，输出到 fp（默认 stderr）。
// 不需要构造任何对象，在 main 的循环或已有定时器里直接调。
//
//   if ((++n & 0x3FFF) == 0) hft::metrics::report_if_due();       // 纯自旋循环
//   void on_timer() { ...; hft::metrics::report_if_due(nowUs * 1000); }  // 已有 1 Hz
//
// period_ns 只在第一次调用时生效（后面的调用忽略它，避免每圈改周期）。
inline bool report_if_due(std::int64_t now_ns = 0,
                          std::int64_t period_ns = 1000000000LL,
                          std::FILE* fp = stderr) {
    static Poller p(period_ns, [fp](const Snapshot& s) {
        std::fputs(s.to_text().c_str(), fp);
        std::fflush(fp);
    });
    return p.poll(now_ns);
}

class Reporter {
public:
    using Callback = std::function<void(const Snapshot&)>;

    Reporter() = default;
    Reporter(const Reporter&) = delete;
    Reporter& operator=(const Reporter&) = delete;

    ~Reporter() {
        stop();
    }

    // **默认给窗口值**：回调拿到的是"这 period 内的分布"，不是"进程启动至今的分布"。
    //
    // 这是刻意的默认。累计 p99 会被启动毛刺永久钉住 —— 实测：头 3 秒有 3% 的样本是
    // 50 µs，之后 9.7 万个样本全是 500 ns，累计 p99 仍然报 51200 ns，而窗口 p99 是 496 ns。
    // 24 小时跑下来，一次重连、一次日志刷盘、一次页错误，都会永久留在累计 p99 里。
    //
    // 第一个周期没有基线，回调拿到的是累计值（此时两者等价）。
    void start(std::chrono::milliseconds period, Callback cb) {
        start_impl(period, std::move(cb), true);
    }

    // 需要"从启动至今"的总量和分布时才用这个（比如算总吞吐、总错误数）。
    void start_cumulative(std::chrono::milliseconds period, Callback cb) {
        start_impl(period, std::move(cb), false);
    }

    void stop() {
        running_.store(false, std::memory_order_release);
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    bool running() const noexcept {
        return running_.load(std::memory_order_acquire);
    }

private:
    void start_impl(std::chrono::milliseconds period, Callback cb, bool windowed) {
        stop();
        if (!cb) {
            return;
        }
        cb_ = std::move(cb);
        windowed_ = windowed;
        has_prev_ = false;
        prev_ = Snapshot{};
        period_ = period.count() > 0 ? period : std::chrono::milliseconds(1000);
        running_.store(true, std::memory_order_release);
        thread_ = std::thread([this]() { loop(); });
    }

    void loop() {
        // sleep_until + 累加绝对截止时间，而不是 sleep_for：
        // sleep_for 会把"采集本身的耗时"累加进周期，跑一天就和外部 1 Hz 采样器错开。
        auto next = std::chrono::steady_clock::now() + period_;
        while (running_.load(std::memory_order_acquire)) {
            std::this_thread::sleep_until(next);
            next += period_;
            if (!running_.load(std::memory_order_acquire)) {
                break;
            }
            try {
                Snapshot cur = snapshot();
                if (!windowed_) {
                    cb_(cur);
                    continue;
                }
                if (!has_prev_) {
                    // 第一个周期：没有基线，给累计值（此时窗口 == 累计）。
                    prev_ = std::move(cur);
                    has_prev_ = true;
                    cb_(prev_);
                } else {
                    Snapshot d = diff(prev_, cur);
                    prev_ = std::move(cur);
                    cb_(d);
                }
            } catch (...) {
                // 报告回调不能把采集线程带崩。
            }
        }
    }

    std::atomic<bool> running_{false};
    bool windowed_{true};
    bool has_prev_{false};
    std::chrono::milliseconds period_{1000};
    Snapshot prev_;
    Callback cb_;
    std::thread thread_;
};

} // namespace metrics
} // namespace hft

// ---------------------------------------------------------------------------
// 宏
// ---------------------------------------------------------------------------

#define HFT_DETAIL_CAT2(a, b) a##b
#define HFT_DETAIL_CAT(a, b) HFT_DETAIL_CAT2(a, b)

#if HFT_METRICS_ENABLED

// clock 参数决定这个指标落在哪个时钟域（见文件头「时钟」）。
// 同一个名字用两个域注册是错误 —— Debug 下 abort，因为 tick 单位不同，
// 混在一个直方图里等于把 ns 和 cycle 加在一起。
#define HFT_DETAIL_ID_I(name, type, clock, ctr)                               \
    static const ::hft::metrics::MetricId HFT_DETAIL_CAT(hft_mid_, ctr) =     \
        ::hft::metrics::detail::metric_id((name), (type), (clock))

#define HFT_DETAIL_MONO ::hft::metrics::ClockKind::Mono
#define HFT_DETAIL_FAST ::hft::metrics::ClockKind::Fast

// 计数器
#define HFT_CNT_ADD_I(name, v, ctr)                                           \
    do {                                                                      \
        HFT_DETAIL_ID_I((name), ::hft::metrics::MetricType::Counter,          \
                        HFT_DETAIL_MONO, ctr);                                \
        ::hft::metrics::detail::add_counter(                                   \
            HFT_DETAIL_CAT(hft_mid_, ctr), static_cast<std::uint64_t>(v));    \
    } while (0)
#define HFT_CNT_ADD(name, v) HFT_CNT_ADD_I((name), (v), __COUNTER__)
#define HFT_CNT(name) HFT_CNT_ADD((name), 1)

// 瞬时值
#define HFT_GAUGE_I(name, v, ctr)                                             \
    do {                                                                      \
        HFT_DETAIL_ID_I((name), ::hft::metrics::MetricType::Gauge,            \
                        HFT_DETAIL_MONO, ctr);                                \
        ::hft::metrics::detail::set_gauge(                                     \
            HFT_DETAIL_CAT(hft_mid_, ctr), static_cast<std::int64_t>(v));     \
    } while (0)
#define HFT_GAUGE(name, v) HFT_GAUGE_I((name), (v), __COUNTER__)

// 段内计时（Fast 域：默认 MONOTONIC，开 TSC 后是 rdtsc）
#define HFT_SCOPE_I(name, ctr)                                                \
    HFT_DETAIL_ID_I((name), ::hft::metrics::MetricType::Histogram,            \
                    HFT_DETAIL_FAST, ctr);                                    \
    ::hft::metrics::Scope HFT_DETAIL_CAT(hft_scope_, ctr)(                    \
        HFT_DETAIL_CAT(hft_mid_, ctr))

#define HFT_SCOPE(name) HFT_SCOPE_I((name), __COUNTER__)

// 1/N 采样；N 必须是 2 的幂
#define HFT_SCOPE_SAMPLED_I(name, n, ctr)                                     \
    HFT_DETAIL_ID_I((name), ::hft::metrics::MetricType::Histogram,            \
                    HFT_DETAIL_FAST, ctr);                                    \
    static thread_local std::uint32_t HFT_DETAIL_CAT(hft_smp_, ctr) = 0;      \
    ::hft::metrics::Scope HFT_DETAIL_CAT(hft_scope_, ctr)(                    \
        HFT_DETAIL_CAT(hft_mid_, ctr),                                        \
        ((HFT_DETAIL_CAT(hft_smp_, ctr)++ & (static_cast<std::uint32_t>(n) - 1u)) == 0u))

#define HFT_SCOPE_SAMPLED(name, n) HFT_SCOPE_SAMPLED_I((name), (n), __COUNTER__)

// 手动时间戳。**HFT_NOW 永远是 MONOTONIC** —— 跨线程 / 跨进程 / 读共享内存里
// 别人盖的时间戳只能用这个。想省 20 ns 的段内计时用 HFT_SCOPE（它自己走 Fast 域）。
#define HFT_NOW() (::hft::metrics::detail::mono_ticks())
#define HFT_NOW_FAST() (::hft::metrics::detail::fast_ticks())

#define HFT_RECORD_I(name, ticks, ctr)                                        \
    do {                                                                      \
        HFT_DETAIL_ID_I((name), ::hft::metrics::MetricType::Histogram,        \
                        HFT_DETAIL_MONO, ctr);                                \
        ::hft::metrics::detail::record_ticks(                                  \
            HFT_DETAIL_CAT(hft_mid_, ctr), static_cast<std::uint64_t>(ticks));\
    } while (0)
#define HFT_RECORD(name, ticks) HFT_RECORD_I((name), (ticks), __COUNTER__)
#define HFT_RECORD_SINCE(name, t0) HFT_RECORD((name), HFT_NOW() - (t0))

// 传 ns（主要用于测试 / 已经有 ns 时间戳的地方；热路径优先用 HFT_SCOPE）
#define HFT_RECORD_NS_I(name, ns, ctr)                                        \
    do {                                                                      \
        HFT_DETAIL_ID_I((name), ::hft::metrics::MetricType::Histogram,        \
                        HFT_DETAIL_MONO, ctr);                                \
        ::hft::metrics::detail::record_ns(                                     \
            HFT_DETAIL_CAT(hft_mid_, ctr), static_cast<double>(ns));          \
    } while (0)
#define HFT_RECORD_NS(name, ns) HFT_RECORD_NS_I((name), (ns), __COUNTER__)

#else // HFT_METRICS_ENABLED

#define HFT_CNT_ADD(name, v) ((void)0)
#define HFT_CNT(name) ((void)0)
#define HFT_GAUGE(name, v) ((void)0)
#define HFT_SCOPE(name) do {} while (0)
#define HFT_SCOPE_SAMPLED(name, n) do {} while (0)
#define HFT_NOW() (0ull)
#define HFT_NOW_FAST() (0ull)
#define HFT_RECORD(name, ticks) ((void)0)
#define HFT_RECORD_SINCE(name, t0) ((void)0)
#define HFT_RECORD_NS(name, ns) ((void)0)

#endif // HFT_METRICS_ENABLED
