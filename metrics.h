#pragma once

// ============================================================================
// include/metrics.h
//
// HFT 轻量级延时 / 计数统计（header-only，无第三方依赖）
//
// 目标：
//   1. 易用：一行宏埋点，不需要声明、不需要初始化、不需要改构建。
//   2. 热路径安全：不加锁、不 malloc、不格式化、不做系统调用。
//   3. 默认低开销：两次时钟读取 + 一次直方图自增
//      （MONOTONIC 约 25-55 ns，平台相关；启用 TSC 后约 15-25 ns）。
//   4. 可关 / 可采样：
//        - 编译期 -DHFT_METRICS_ENABLED=0  完全裁掉，零成本；
//        - 运行期 HFT_METRICS=0            所有埋点变 no-op；
//        - HFT_SCOPE_SAMPLED(name, N)      1/N 采样。
//   5. p99 由直方图合并，不做“平均 p99”。
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
//       uint64_t t0 = HFT_NOW();
//       ...
//       HFT_RECORD_SINCE("db.parse", t0);
//
//   HFT_SCOPE 展开成“声明 + 对象”，必须放在大括号块里：
//       if (ok) { HFT_SCOPE("x"); ... }
//   不要写成 if (ok) HFT_SCOPE("x");（没有大括号会编译失败，这是故意的）。
//
// ---------------------------------------------------------------------------
// 导出（报告线程 / 离线工具里调用，不在热路径）
// ---------------------------------------------------------------------------
//   auto snap = hft::metrics::snapshot();
//   std::puts(snap.to_text().c_str());
//   std::puts(snap.to_json().c_str());
//
//   周期采集：
//       hft::metrics::Reporter rep;
//       rep.start(std::chrono::milliseconds(1000), [](const auto& s) {
//           std::puts(s.to_text().c_str());
//       });
//
// ---------------------------------------------------------------------------
// 时钟
// ---------------------------------------------------------------------------
//   默认          CLOCK_MONOTONIC（vDSO），容器/VM 里也可靠；
//   x86_64        确认 constant_tsc + nonstop_tsc 后，加
//                   -DHFT_METRICS_USE_TSC=1
//                 可把每次 HFT_SCOPE 再降约 20 ns。
//
//   时钟实现统一在 include/hft_time.h；本头只决定用 mono 还是 tsc。
//   热路径只存 tick，不做乘除法；换算只发生在 snapshot()。
//
// ---------------------------------------------------------------------------
// 约束
// ---------------------------------------------------------------------------
//   * 不保证 async-signal-safe。
//   * 每个进程独立；跨进程合并请把 snapshot 导出后在外部分析。
//   * metric 名字必须是编译期字符串字面量；不要在热路径拼接 symbol。
//   * 默认上限 128 个 metric。超过后新 metric 返回 kInvalidMetric，埋点自动变 no-op。
// ============================================================================

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "hft_time.h"

#ifndef HFT_METRICS_ENABLED
#define HFT_METRICS_ENABLED 1
#endif

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
inline constexpr std::uint32_t kMaxMetrics = 128;
inline constexpr std::uint32_t kBuckets = 512;   // 1 ns ~ 2^63 tick，12.5% 分辨率
inline constexpr std::uint32_t kSubBits = 3;     // 每个 2 倍区间 8 个子桶

static_assert(kMaxMetrics <= 128,
              "g_disabled_mask 只有 2 个 64-bit word；扩大 kMaxMetrics 时要同步扩展");

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

// ---------------------------------------------------------------------------
// 时钟
// ---------------------------------------------------------------------------

namespace detail {

#if HFT_METRICS_USE_TSC && (defined(__x86_64__) || defined(__i386__))
inline std::uint64_t now_ticks() noexcept {
    return ::hft::time::tsc_ticks();
}
#else
inline std::uint64_t now_ticks() noexcept {
    return ::hft::time::mono_ticks();
}
#endif

inline const char* clock_name() noexcept {
#if HFT_METRICS_USE_TSC && (defined(__x86_64__) || defined(__i386__))
    return ::hft::time::tsc_clock_name();
#else
    return ::hft::time::mono_clock_name();
#endif
}

inline double ns_per_tick() {
#if HFT_METRICS_USE_TSC && (defined(__x86_64__) || defined(__i386__))
    return ::hft::time::tsc_ns_per_tick();
#else
    return ::hft::time::mono_ns_per_tick();
#endif
}

inline std::uint64_t wall_now_ns() noexcept {
    return ::hft::time::wall_ns();
}

// 把 tick 映射到对数线性桶：
//   0..7         => 精确值 0..7
//   >= 8         => 8 + (e-3)*8 + 低 3 位，e = floor(log2(v))
inline std::uint32_t bucket_of(std::uint64_t v) noexcept {
    if (v < 8) {
        return static_cast<std::uint32_t>(v);
    }
    const std::uint32_t e = 63u - static_cast<std::uint32_t>(__builtin_clzll(v));
    const std::uint32_t m = static_cast<std::uint32_t>((v >> (e - 3u)) & 7u);
    return 8u + (e - 3u) * 8u + m;
}

inline std::uint64_t bucket_lower(std::uint32_t idx) noexcept {
    if (idx < 8) {
        return idx;
    }
    const std::uint32_t e = (idx - 8u) / 8u + 3u;
    const std::uint32_t m = (idx - 8u) % 8u;
    return (1ull << e) + (static_cast<std::uint64_t>(m) << (e - 3u));
}

inline std::uint64_t bucket_upper(std::uint32_t idx) noexcept {
    if (idx < 8) {
        return idx + 1ull;
    }
    const std::uint32_t e = (idx - 8u) / 8u + 3u;
    const std::uint32_t m = (idx - 8u) % 8u;
    if (e >= 63u) {
        return ~0ull;  // 避免 (1<<63)+(8<<60) 溢出
    }
    return (1ull << e) + (static_cast<std::uint64_t>(m + 1u) << (e - 3u));
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
};

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
inline MetricId metric_id(const char* name, MetricType type) {
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
#endif
            return i;
        }
    }
    if (r.used >= kMaxMetrics) {
        return kInvalidMetric;
    }
    const MetricId id = r.used++;
    r.slots[id].hash = h;
    r.slots[id].name = name;
    r.slots[id].type = type;
    return id;
}

} // namespace detail

// ---------------------------------------------------------------------------
// 全局开关
// ---------------------------------------------------------------------------

inline std::atomic<bool> g_enabled{true};

// 0 表示启用；某个 bit 置 1 表示该 metric 关闭。常量初始化，避开静态初始化顺序。
inline std::atomic<std::uint64_t> g_disabled_mask[2]{{0}, {0}};

// Gauge 是全局瞬时值，不做 per-thread 合并。
inline std::atomic<std::int64_t> g_gauges[kMaxMetrics]{};

inline bool enabled(MetricId id) noexcept {
    if (id == kInvalidMetric) {
        return false;
    }
    if (!g_enabled.load(std::memory_order_relaxed)) {
        return false;
    }
    return ((g_disabled_mask[id >> 6].load(std::memory_order_relaxed) >>
             (id & 63u)) & 1ull) == 0;
}

inline void set_enabled(bool on) noexcept {
    g_enabled.store(on, std::memory_order_relaxed);
}

inline bool is_enabled() noexcept {
    return g_enabled.load(std::memory_order_relaxed);
}

inline void set_metric_enabled(MetricId id, bool on) noexcept {
    if (id == kInvalidMetric) {
        return;
    }
    const std::uint64_t bit = 1ull << (id & 63u);
    if (on) {
        g_disabled_mask[id >> 6].fetch_and(~bit, std::memory_order_relaxed);
    } else {
        g_disabled_mask[id >> 6].fetch_or(bit, std::memory_order_relaxed);
    }
}

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

inline MetricId find_metric(const char* name) {
    return detail::find_metric(name);
}

// 只对“已经注册过”的名字生效；不存在则 no-op，不会凭空注册成别的类型。
inline void set_metric_enabled(const char* name, bool on) {
    const MetricId id = detail::find_metric(name);
    if (id != kInvalidMetric) {
        set_metric_enabled(id, on);
    }
}

namespace detail {

// 运行期 HFT_METRICS=0 关闭；只做一次。
struct EnvInit {
    EnvInit() {
        const char* v = std::getenv("HFT_METRICS");
        if (v != nullptr && v[0] == '0' && v[1] == '\0') {
            g_enabled.store(false, std::memory_order_relaxed);
        }
    }
};
inline EnvInit g_env_init;

} // namespace detail

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
    if (!enabled(id)) {
        return;
    }
    ThreadShard& s = shard();
    const std::uint64_t c = s.counters[id].load(std::memory_order_relaxed);
    s.counters[id].store(c + v, std::memory_order_relaxed);
}

inline void set_gauge(MetricId id, std::int64_t v) noexcept {
    if (!enabled(id)) {
        return;
    }
    g_gauges[id].store(v, std::memory_order_relaxed);
}

inline void record_ticks(MetricId id, std::uint64_t ticks) noexcept {
    if (!enabled(id)) {
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
    const double t = ns / detail::ns_per_tick();
    record_ticks(id, t <= 0.0 ? 0ull : static_cast<std::uint64_t>(t + 0.5));
}

} // namespace detail

// ---------------------------------------------------------------------------
// RAII 段
// ---------------------------------------------------------------------------

class Scope {
public:
    Scope(MetricId id, bool sampled = true) noexcept
        : id_((sampled && enabled(id)) ? id : kInvalidMetric),
          t0_(id_ != kInvalidMetric ? detail::now_ticks() : 0) {}

    ~Scope() {
        if (id_ != kInvalidMetric) {
            detail::record_ticks(id_, detail::now_ticks() - t0_);
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
    double ns_per_tick{1.0};
    std::vector<MetricSnapshot> metrics;

    std::string to_json() const;
    std::string to_text() const;
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

    // nearest-rank: rank = ceil(q * total)，1-based。
    // 不能用 floor(q * total) + "acc > target"：这样在 q * total
    // 正好是整数时会多取一个样本（例如 100 个样本的 P50 会取第 51 个）。
    const double pos = q * static_cast<double>(total);
    std::uint64_t rank = static_cast<std::uint64_t>(pos);
    if (static_cast<double>(rank) < pos) {
        ++rank;
    }
    if (rank == 0) {
        rank = 1;                  // q == 0 时返回最小值所在桶
    }
    if (rank > total) {
        rank = total;
    }

    std::uint64_t acc = 0;
    for (std::uint32_t i = 0; i < kBuckets; ++i) {
        acc += buckets[i];
        if (acc >= rank) {
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

    snap.metrics.reserve(used);
    for (std::uint32_t i = 0; i < used; ++i) {
        MetricSnapshot m;
        m.name = reg.slots[i].name != nullptr ? reg.slots[i].name : "";
        m.type = reg.slots[i].type;

        if (m.type == MetricType::Histogram) {
            m.count = ncnt[i];
            m.sum_ticks = sum[i];
            if (m.count != 0) {
                m.min_ticks = (mn[i] == ~0ull) ? 0 : mn[i];
                m.max_ticks = mx[i];
                m.mean_ns = static_cast<double>(m.sum_ticks) /
                            static_cast<double>(m.count) * snap.ns_per_tick;
                m.min_ns = static_cast<double>(m.min_ticks) * snap.ns_per_tick;
                m.max_ns = static_cast<double>(m.max_ticks) * snap.ns_per_tick;

                const std::uint64_t* row =
                    hist.data() + static_cast<std::size_t>(i) * kBuckets;
                m.p50_ns = detail::percentile_ns(row, m.count, 0.50, snap.ns_per_tick);
                m.p90_ns = detail::percentile_ns(row, m.count, 0.90, snap.ns_per_tick);
                m.p99_ns = detail::percentile_ns(row, m.count, 0.99, snap.ns_per_tick);
                m.p999_ns = detail::percentile_ns(row, m.count, 0.999, snap.ns_per_tick);
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
        detail::recompute_stats(d, out.ns_per_tick);
        out.metrics.push_back(std::move(d));
    }

    return out;
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

} // namespace detail

inline std::string Snapshot::to_json() const {
    std::string out;
    out.reserve(256 + metrics.size() * 256);
    out += "{\"ts_ns\":";
    detail::append_u64(out, ts_ns);
    out += ",\"ns_per_tick\":";
    detail::append_double(out, ns_per_tick);
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
        out += "\",\"count\":";
        detail::append_u64(out, m.count);

        if (m.type == MetricType::Gauge) {
            out += ",\"value\":";
            detail::append_i64(out, m.gauge);
        } else if (m.type == MetricType::Histogram) {
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
        }
        out += '}';
    }
    out += "]}";
    return out;
}

inline std::string Snapshot::to_text() const {
    std::string out;
    out.reserve(256 + metrics.size() * 160);

    char line[256];
    std::snprintf(line, sizeof(line),
                  "%-32s %-10s %12s %12s %12s %12s %12s %12s\n",
                  "name", "type", "count", "p50_ns", "p90_ns",
                  "p99_ns", "p999_ns", "max_ns");
    out += line;

    for (const MetricSnapshot& m : metrics) {
        if (m.type == MetricType::Histogram) {
            std::snprintf(line, sizeof(line),
                          "%-32s %-10s %12llu %12.1f %12.1f %12.1f %12.1f %12.1f\n",
                          m.name.c_str(), to_string(m.type),
                          static_cast<unsigned long long>(m.count),
                          m.p50_ns, m.p90_ns, m.p99_ns, m.p999_ns, m.max_ns);
        } else if (m.type == MetricType::Gauge) {
            std::snprintf(line, sizeof(line),
                          "%-32s %-10s %12lld\n",
                          m.name.c_str(), to_string(m.type),
                          static_cast<long long>(m.gauge));
        } else {
            std::snprintf(line, sizeof(line),
                          "%-32s %-10s %12llu\n",
                          m.name.c_str(), to_string(m.type),
                          static_cast<unsigned long long>(m.count));
        }
        out += line;
    }
    return out;
}

// ---------------------------------------------------------------------------
// 周期采集
// ---------------------------------------------------------------------------

class Reporter {
public:
    using Callback = std::function<void(const Snapshot&)>;

    Reporter() = default;
    Reporter(const Reporter&) = delete;
    Reporter& operator=(const Reporter&) = delete;

    ~Reporter() {
        stop();
    }

    void start(std::chrono::milliseconds period, Callback cb) {
        stop();
        if (!cb) {
            return;
        }
        cb_ = std::move(cb);
        period_ = period.count() > 0 ? period : std::chrono::milliseconds(1000);
        running_.store(true, std::memory_order_release);
        thread_ = std::thread([this]() { loop(); });
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
    void loop() {
        while (running_.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(period_);
            if (!running_.load(std::memory_order_acquire)) {
                break;
            }
            try {
                cb_(snapshot());
            } catch (...) {
                // 报告回调不能把采集线程带崩。
            }
        }
    }

    std::atomic<bool> running_{false};
    std::chrono::milliseconds period_{1000};
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

#define HFT_DETAIL_ID_I(name, type, ctr)                                      \
    static const ::hft::metrics::MetricId HFT_DETAIL_CAT(hft_mid_, ctr) =     \
        ::hft::metrics::detail::metric_id((name), (type))

// 计数器
#define HFT_CNT_ADD_I(name, v, ctr)                                           \
    do {                                                                      \
        HFT_DETAIL_ID_I((name), ::hft::metrics::MetricType::Counter, ctr);    \
        ::hft::metrics::detail::add_counter(                                   \
            HFT_DETAIL_CAT(hft_mid_, ctr), static_cast<std::uint64_t>(v));    \
    } while (0)
#define HFT_CNT_ADD(name, v) HFT_CNT_ADD_I((name), (v), __COUNTER__)
#define HFT_CNT(name) HFT_CNT_ADD((name), 1)

// 瞬时值
#define HFT_GAUGE_I(name, v, ctr)                                             \
    do {                                                                      \
        HFT_DETAIL_ID_I((name), ::hft::metrics::MetricType::Gauge, ctr);      \
        ::hft::metrics::detail::set_gauge(                                     \
            HFT_DETAIL_CAT(hft_mid_, ctr), static_cast<std::int64_t>(v));     \
    } while (0)
#define HFT_GAUGE(name, v) HFT_GAUGE_I((name), (v), __COUNTER__)

// 段内计时
#define HFT_SCOPE_I(name, ctr)                                                \
    HFT_DETAIL_ID_I((name), ::hft::metrics::MetricType::Histogram, ctr);      \
    ::hft::metrics::Scope HFT_DETAIL_CAT(hft_scope_, ctr)(                    \
        HFT_DETAIL_CAT(hft_mid_, ctr))

#define HFT_SCOPE(name) HFT_SCOPE_I((name), __COUNTER__)

// 1/N 采样；N 必须是 2 的幂
#define HFT_SCOPE_SAMPLED_I(name, n, ctr)                                     \
    HFT_DETAIL_ID_I((name), ::hft::metrics::MetricType::Histogram, ctr);      \
    static thread_local std::uint32_t HFT_DETAIL_CAT(hft_smp_, ctr) = 0;      \
    ::hft::metrics::Scope HFT_DETAIL_CAT(hft_scope_, ctr)(                    \
        HFT_DETAIL_CAT(hft_mid_, ctr),                                        \
        ((HFT_DETAIL_CAT(hft_smp_, ctr)++ & (static_cast<std::uint32_t>(n) - 1u)) == 0u))

#define HFT_SCOPE_SAMPLED(name, n) HFT_SCOPE_SAMPLED_I((name), (n), __COUNTER__)

// 手动 tick
#define HFT_NOW() (::hft::metrics::detail::now_ticks())

#define HFT_RECORD_I(name, ticks, ctr)                                        \
    do {                                                                      \
        HFT_DETAIL_ID_I((name), ::hft::metrics::MetricType::Histogram, ctr);  \
        ::hft::metrics::detail::record_ticks(                                  \
            HFT_DETAIL_CAT(hft_mid_, ctr), static_cast<std::uint64_t>(ticks));\
    } while (0)
#define HFT_RECORD(name, ticks) HFT_RECORD_I((name), (ticks), __COUNTER__)
#define HFT_RECORD_SINCE(name, t0) HFT_RECORD((name), HFT_NOW() - (t0))

// 传 ns（主要用于测试 / 已经有 ns 时间戳的地方；热路径优先用 HFT_SCOPE）
#define HFT_RECORD_NS_I(name, ns, ctr)                                        \
    do {                                                                      \
        HFT_DETAIL_ID_I((name), ::hft::metrics::MetricType::Histogram, ctr);  \
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
#define HFT_RECORD(name, ticks) ((void)0)
#define HFT_RECORD_SINCE(name, t0) ((void)0)
#define HFT_RECORD_NS(name, ns) ((void)0)

#endif // HFT_METRICS_ENABLED
