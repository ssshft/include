#pragma once
// =============================================================================
// OmsShm.h — OMS 订单存储的共享内存环形数组
//
// 单文件 header-only 实现, 包含:
//   1. SHM 内存布局 (Header / Slot 数组 / 主索引 / 别名索引)
//   2. OmsShmWriter — OMS 侧写入 (insert / update / reclaim)
//   3. OmsShmReader — 策略侧读取 (lookup / iterate, 无锁 seqlock)
//   4. FNV-1a hash 和 open-addressing 索引助手
//
// 核心特性:
//   - 固定容量, 内存永远不涨
//   - 环形覆盖, 只覆盖 FINISHED slot, 绝不覆盖 LIVE
//   - 单写多读 (OMS 单进程写, 多个策略进程 mmap 读)
//   - 无锁 read (seqlock)
//   - 跨进程持久 (mmap 文件, tb 重启保留活单状态)
//   - 三层 key 索引: orderSysId(主) / clientOrderId / orderId
//
// 使用契约:
//   - **只允许 OMS 进程写**, 多写会导致 slot state race
//   - Reader 只做 lookup, 不修改任何 slot 或 index
//   - RCommand 必须是 trivially copyable POD (union 结构 OK)
//
// 依赖:
//   - Linux (mmap / shm)
//   - C++17
//   - pubsub_protocol.h (RCommand)
// =============================================================================

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <chrono>
#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "pubsub_protocol.h"

namespace oms {
namespace shm {

// =============================================================================
// 常量 / 类型
// =============================================================================

constexpr uint32_t kMagic          = 0x4F4D5352;   // 'OMSR' little-endian
constexpr uint32_t kVersion        = 2;            // v2: 加 last_update_time_ns + max_live_stale_ns
constexpr uint32_t kDefaultCapacity = 100'000;     // slot 数
constexpr uint64_t kDefaultMinReclaimAgeNs   = 60ULL * 1'000'000'000;         // 60s: FINISHED 最小 TTL
constexpr uint64_t kDefaultMaxLiveStaleNs    = 24ULL * 3600 * 1'000'000'000;  // 24h: LIVE 无更新最长容忍
constexpr uint32_t kInvalidSlot    = UINT32_MAX;
constexpr uint32_t kMaxProbeSlots  = 128;          // 环形 alloc 最大探测
constexpr uint32_t kMaxProbeIndex  = 32;           // 索引 open-addressing 最大探测
constexpr uint32_t kMaxReadRetry   = 16;           // seqlock 读重试上限

// Index entry 里的 key_hash 特殊值
constexpr uint64_t kHashEmpty      = 0;
constexpr uint64_t kHashTombstone  = ~0ULL;

// 三层索引类型
enum IndexKind : uint32_t {
    IDX_ORDER_SYS_ID  = 0,   // 主: orderSysId → slot
    IDX_CLIENT_ORDER  = 1,   // 别名: clientOrderId → slot
    IDX_EXCHANGE_ID   = 2,   // 别名: exchange orderId → slot
    IDX_COUNT         = 3,
};

// Slot 状态机
enum SlotState : uint32_t {
    SLOT_EMPTY      = 0,   // 从未用过或已被 reclaim 完成
    SLOT_LIVE       = 1,   // 活单 (NEW / PARTFILLED / PENDING_*)
    SLOT_FINISHED   = 2,   // 终结 (FILLED / CANCELED / REJECTED), 达阈值后可 reclaim
    SLOT_RECLAIMING = 3,   // 正在覆盖中 (writer 独占, reader 应重试)
};

// =============================================================================
// FNV-1a 64-bit hash
// =============================================================================
inline uint64_t fnv1a(const char* p, size_t n) noexcept {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < n; ++i) {
        h ^= static_cast<uint8_t>(p[i]);
        h *= 0x100000001b3ULL;
    }
    // 避开 kHashEmpty / kHashTombstone 两个保留值
    if (h == kHashEmpty)     h = 1;
    if (h == kHashTombstone) h = 2;
    return h;
}

inline uint64_t fnv1a(std::string_view sv) noexcept {
    return fnv1a(sv.data(), sv.size());
}

inline size_t strnlen_max(const char* s, size_t maxlen) noexcept {
    size_t n = 0;
    while (n < maxlen && s[n] != '\0') ++n;
    return n;
}

// =============================================================================
// Header (固定 4 KB, cacheline 对齐)
// =============================================================================
struct alignas(64) OmsShmHeader {
    uint32_t magic;                 // 校验
    uint32_t version;
    uint32_t slot_capacity;         // N
    uint32_t index_capacity;        // 2N, 单个索引大小
    uint32_t index_kinds;           // = IDX_COUNT (3)

    uint64_t min_reclaim_age_ns;    // FINISHED slot 至少 idle 多久才能回收
    uint64_t max_live_stale_ns;     // LIVE slot 多久无更新就当卡单 / 僵尸, 允许强制回收

    // 单写者原子递增, wrap 后取模到 slot
    std::atomic<uint64_t> next_slot_hint;

    // 全局 monotonic 序列, 每次写 +1 (供审计 / debug)
    std::atomic<uint64_t> global_seq;

    // owner 元数据
    uint64_t created_ts_ns;
    uint32_t owner_pid;
    uint32_t _pad0;

    // 统计 (best-effort, 非事务)
    std::atomic<uint64_t> total_inserts;
    std::atomic<uint64_t> total_updates;
    std::atomic<uint64_t> total_reclaims;
    std::atomic<uint64_t> total_alloc_failures;
    std::atomic<uint64_t> total_stale_live_reclaims;  // v2 新增: 卡单强制回收次数

    // 填充到 4 KB, 剩余保留将来扩展
    char pad[4096 - 96];
};
static_assert(sizeof(OmsShmHeader) == 4096, "OmsShmHeader must be 4 KB");

// =============================================================================
// IndexEntry (32 字节, open-addressing hash bucket)
// =============================================================================
struct alignas(32) IndexEntry {
    std::atomic<uint64_t> key_hash;      // 0=空, ~0=tombstone
    std::atomic<uint32_t> slot_idx;
    uint32_t              _pad;
    char                  key_prefix[16]; // key 前 16 字节, 用于冲突验证
};
static_assert(sizeof(IndexEntry) == 32, "IndexEntry must be 32 bytes");

// =============================================================================
// OmsSlot (每单一条, 1024 字节)
//   RCommand 本身 ~656B, 加 seqlock + 时间戳 + 3 个 key 副本 ≈ 848B, pad 到 1024。
//   1024 = 16 cachelines, seqlock + state 落头 cacheline, order body 后续 cachelines。
// =============================================================================
constexpr size_t kSlotSize = 1024;

struct alignas(64) OmsSlot {
    std::atomic<uint64_t> seq;         // seqlock, 偶=stable 奇=writing
    std::atomic<uint32_t> state;       // SlotState
    uint32_t              _pad0;

    uint64_t              create_time_ns;
    uint64_t              last_update_time_ns;  // ★ 每次写入 (create 或 update) 都刷; 用于卡单检测
    uint64_t              finish_time_ns;       // 0 = 尚 live, 非 0 = 变成 FINISHED 的时刻

    // 索引 key 副本 (reclaim 时反查用)。 clientOrderId 存**复合 key** (strategyId + cid),
    // 因为不同策略可能撞同一 int64 client id, 单 int 会误命中。 匹配旧 OMS 的
    // `fmt::format("{}{}", strategyId, clientOrderId)` 格式化。
    char                  orderSysId[64];
    char                  clientOrderId[64];   // strategyId(≤32) + cid_int64(≤20) = ≤52, 64 够
    char                  orderId[64];

    // 完整订单 payload
    pubsub::RCommand      order;

    // pad 到 kSlotSize
    char                  _pad1[kSlotSize
        - sizeof(std::atomic<uint64_t>) // seq
        - sizeof(std::atomic<uint32_t>) // state
        - sizeof(uint32_t)              // _pad0
        - sizeof(uint64_t) * 3          // create / last_update / finish
        - 64 - 64 - 64                  // 3 key (clientOrderId 已扩到 64)
        - sizeof(pubsub::RCommand)];
};
static_assert(sizeof(OmsSlot) == kSlotSize,
    "OmsSlot size drift; if pubsub::RCommand grew, bump kSlotSize");
static_assert(std::atomic<uint64_t>::is_always_lock_free, "atomic<u64> must be lock-free for shm");
static_assert(std::atomic<uint32_t>::is_always_lock_free, "atomic<u32> must be lock-free for shm");

// =============================================================================
// SHM 内存布局辅助
//
// [OmsShmHeader (4 KB)]
// [Slot 数组: capacity * 512 B]
// [Index 数组 × IDX_COUNT: index_capacity * 32 B each]
// =============================================================================
struct OmsShmLayout {
    OmsShmHeader* header    = nullptr;
    OmsSlot*      slots     = nullptr;
    IndexEntry*   indices[IDX_COUNT] = {nullptr, nullptr, nullptr};
    void*         base      = nullptr;
    size_t        total_size = 0;

    static size_t compute_total_size(uint32_t slot_cap, uint32_t idx_cap) noexcept {
        return sizeof(OmsShmHeader)
             + static_cast<size_t>(slot_cap) * sizeof(OmsSlot)
             + static_cast<size_t>(idx_cap)  * sizeof(IndexEntry) * IDX_COUNT;
    }

    void map_from(void* base_ptr, uint32_t slot_cap, uint32_t idx_cap) noexcept {
        base = base_ptr;
        header = reinterpret_cast<OmsShmHeader*>(base);
        char* p = reinterpret_cast<char*>(base) + sizeof(OmsShmHeader);
        slots = reinterpret_cast<OmsSlot*>(p);
        p += static_cast<size_t>(slot_cap) * sizeof(OmsSlot);
        for (uint32_t k = 0; k < IDX_COUNT; ++k) {
            indices[k] = reinterpret_cast<IndexEntry*>(p);
            p += static_cast<size_t>(idx_cap) * sizeof(IndexEntry);
        }
        total_size = compute_total_size(slot_cap, idx_cap);
    }
};

// =============================================================================
// OmsShmSegment — mmap 生命周期基类 (writer / reader 共享)
// =============================================================================
class OmsShmSegment {
public:
    OmsShmSegment() = default;
    virtual ~OmsShmSegment() { close(); }

    OmsShmSegment(const OmsShmSegment&) = delete;
    OmsShmSegment& operator=(const OmsShmSegment&) = delete;

    // 打开或创建 SHM 文件, mmap 进内存。
    //   file_path : 例 "/dev/shm/tb_oms.dat"
    //   slot_cap  : 只在**创建新文件**时生效; 打开已有文件时忽略, 从 header 读
    //   create_if_missing : true 时不存在就创建
    //   read_only : reader 用 true (只 mmap 读)
    void open(const std::string& file_path, uint32_t slot_cap,
              bool create_if_missing, bool read_only)
    {
        close();
        file_path_ = file_path;
        read_only_ = read_only;

        int flags = read_only ? O_RDONLY : (O_RDWR | (create_if_missing ? O_CREAT : 0));
        fd_ = ::open(file_path.c_str(), flags, 0666);
        if (fd_ < 0) {
            throw std::runtime_error("OmsShm: open failed for " + file_path
                                     + " errno=" + std::to_string(errno));
        }

        // 决定容量: 新建时用参数, 已存在则先 stat 出总大小反推
        struct stat st;
        if (::fstat(fd_, &st) != 0) {
            ::close(fd_); fd_ = -1;
            throw std::runtime_error("OmsShm: fstat failed");
        }

        uint32_t use_cap = slot_cap;
        size_t   expect_size = OmsShmLayout::compute_total_size(use_cap, use_cap * 2);

        if (st.st_size == 0) {
            // 新文件
            if (read_only) {
                throw std::runtime_error("OmsShm: read-only open on empty file " + file_path);
            }
            if (::ftruncate(fd_, static_cast<off_t>(expect_size)) != 0) {
                ::close(fd_); fd_ = -1;
                throw std::runtime_error("OmsShm: ftruncate failed");
            }
            created_new_ = true;
        } else {
            // 已存在: 保留原大小 (不 truncate, 保留 owner 的容量)
            if (static_cast<size_t>(st.st_size) < sizeof(OmsShmHeader)) {
                ::close(fd_); fd_ = -1;
                throw std::runtime_error("OmsShm: existing file too small");
            }
            expect_size = static_cast<size_t>(st.st_size);
            created_new_ = false;
        }

        int prot = read_only ? PROT_READ : (PROT_READ | PROT_WRITE);
        int map_flags = MAP_SHARED;
        void* p = ::mmap(nullptr, expect_size, prot, map_flags, fd_, 0);
        if (p == MAP_FAILED) {
            ::close(fd_); fd_ = -1;
            throw std::runtime_error("OmsShm: mmap failed errno=" + std::to_string(errno));
        }

        // 若是新建, 初始化 header 拿到实际容量
        if (created_new_) {
            layout_.map_from(p, use_cap, use_cap * 2);
            std::memset(p, 0, expect_size);
            layout_.header->magic          = kMagic;
            layout_.header->version        = kVersion;
            layout_.header->slot_capacity  = use_cap;
            layout_.header->index_capacity = use_cap * 2;
            layout_.header->index_kinds    = IDX_COUNT;
            layout_.header->min_reclaim_age_ns = kDefaultMinReclaimAgeNs;
            layout_.header->max_live_stale_ns  = kDefaultMaxLiveStaleNs;
            layout_.header->created_ts_ns  = now_ns();
            layout_.header->owner_pid      = static_cast<uint32_t>(::getpid());
            layout_.header->next_slot_hint.store(0);
            layout_.header->global_seq.store(0);
        } else {
            // 打开已有: 先临时 map 4K 读 header, 拿到实际容量后重新 map (若大小不符)
            auto* tmp_hdr = reinterpret_cast<OmsShmHeader*>(p);
            if (tmp_hdr->magic != kMagic || tmp_hdr->version != kVersion) {
                ::munmap(p, expect_size);
                ::close(fd_); fd_ = -1;
                throw std::runtime_error("OmsShm: magic/version mismatch");
            }
            uint32_t hdr_cap = tmp_hdr->slot_capacity;
            uint32_t hdr_idx = tmp_hdr->index_capacity;
            size_t expected = OmsShmLayout::compute_total_size(hdr_cap, hdr_idx);
            if (expected != expect_size) {
                ::munmap(p, expect_size);
                ::close(fd_); fd_ = -1;
                throw std::runtime_error("OmsShm: file size mismatch header capacity");
            }
            layout_.map_from(p, hdr_cap, hdr_idx);
        }
    }

    void close() noexcept {
        if (layout_.base) {
            ::munmap(layout_.base, layout_.total_size);
            layout_ = {};
        }
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

    bool                is_open()      const noexcept { return layout_.base != nullptr; }
    bool                created_new()  const noexcept { return created_new_; }
    const OmsShmLayout& layout()       const noexcept { return layout_; }
    OmsShmHeader*       header()       const noexcept { return layout_.header; }
    OmsSlot*            slots()        const noexcept { return layout_.slots; }
    IndexEntry*         index(uint32_t k) const noexcept { return layout_.indices[k]; }
    uint32_t            slot_capacity()  const noexcept { return layout_.header ? layout_.header->slot_capacity : 0; }
    uint32_t            index_capacity() const noexcept { return layout_.header ? layout_.header->index_capacity : 0; }

    // =====================================================================
    // 只读查询接口 (Writer / Reader 都能调, 不修改任何状态)
    //   - 都用 seqlock, reader / writer 并发安全
    //   - Writer 侧用来: query_order 响应、update 前查现有单、对账
    //   - Reader 侧用来: 策略 / 工具查询
    // =====================================================================

    // seqlock read 的一致快照 (含 3 层 key 副本 + order body)
    struct ReadSnapshot {
        pubsub::RCommand order;
        char             orderSysId[64];
        char             clientOrderId[64];   // 复合 key: strategyId + cid
        char             orderId[64];
    };

    bool lookup_by_orderSysId(std::string_view id, pubsub::RCommand& out) const {
        return lookup_impl(IDX_ORDER_SYS_ID, id, out);
    }

    // 主入口: 用 strategyId + cid 复合查, 匹配旧 OMS 的 `strategyId+cid` 格式化。
    // 不同策略可能撞同一 int64 cid, 单 cid 会误命中, 必须带 strategyId。
    bool lookup_by_client(std::string_view strategyId, int64_t cid, pubsub::RCommand& out) const {
        char buf[64];
        int n = compose_client_key(buf, sizeof(buf), strategyId, cid);
        if (n <= 0) return false;
        return lookup_impl(IDX_CLIENT_ORDER, std::string_view(buf, static_cast<size_t>(n)), out);
    }

    // 底层: 直接用**已复合的字符串**查 (CLI / debug 用). 如果不带 strategyId 前缀,
    // 查不到 —— 因为 writer 存的都是复合 key。
    bool lookup_by_clientOrderId(std::string_view composed, pubsub::RCommand& out) const {
        return lookup_impl(IDX_CLIENT_ORDER, composed, out);
    }

    bool lookup_by_orderId(std::string_view id, pubsub::RCommand& out) const {
        return lookup_impl(IDX_EXCHANGE_ID, id, out);
    }

    // 复合 client key 格式化函数 (writer/reader 都用, 保证一致性)。
    // 返回写入字节数 (不含 '\0'), 溢出返回 <=0。
    // 格式**必须**与 OrderManager 老代码 `fmt::format("{}{}", strategyId, cid)` 一致。
    static int compose_client_key(char* buf, size_t buf_size,
                                  std::string_view strategyId, int64_t cid) noexcept {
        // 手动拼接, 避免 snprintf 对 %s 处理 std::string_view 的 UB
        if (buf_size < 24) return -1;
        size_t off = 0;
        size_t sid_n = strategyId.size();
        if (sid_n >= buf_size - 20) sid_n = buf_size - 20 - 1;
        std::memcpy(buf + off, strategyId.data(), sid_n);
        off += sid_n;
        int m = std::snprintf(buf + off, buf_size - off, "%lld", static_cast<long long>(cid));
        if (m <= 0 || static_cast<size_t>(m) >= buf_size - off) return -1;
        off += static_cast<size_t>(m);
        buf[off] = '\0';
        return static_cast<int>(off);
    }

    // 遍历所有 LIVE / FINISHED (慢, O(N), 只用于 snapshot / 对账, 别在 hot path 调)
    size_t iterate_live(const std::function<void(const pubsub::RCommand&)>& cb) const {
        return iterate_state(SLOT_LIVE, cb);
    }
    size_t iterate_finished(const std::function<void(const pubsub::RCommand&)>& cb) const {
        return iterate_state(SLOT_FINISHED, cb);
    }

    // 便捷: 查存不存在, 不需要拿 order 内容
    bool exists_by_orderSysId(std::string_view id) const {
        pubsub::RCommand dummy;
        return lookup_by_orderSysId(id, dummy);
    }

protected:
    static inline void pause_or_yield() noexcept {
    #if defined(__x86_64__) || defined(_M_X64)
        __builtin_ia32_pause();
    #else
        std::this_thread::yield();
    #endif
    }

    // seqlock 无锁读一致快照, 返回 false = slot 已空或 writer 一直忙
    bool read_slot_snapshot(uint32_t idx, ReadSnapshot& snap) const {
        OmsSlot& s = slots()[idx];
        for (uint32_t retry = 0; retry < kMaxReadRetry; ++retry) {
            uint64_t s1 = s.seq.load(std::memory_order_acquire);
            if (s1 & 1ULL) { pause_or_yield(); continue; }   // writer 在写
            uint32_t st = s.state.load(std::memory_order_acquire);
            if (st == SLOT_EMPTY || st == SLOT_RECLAIMING) return false;
            std::memcpy(snap.orderSysId,    s.orderSysId,    sizeof(snap.orderSysId));
            std::memcpy(snap.clientOrderId, s.clientOrderId, sizeof(snap.clientOrderId));
            std::memcpy(snap.orderId,       s.orderId,       sizeof(snap.orderId));
            std::memcpy(&snap.order, &s.order, sizeof(pubsub::RCommand));
            uint64_t s2 = s.seq.load(std::memory_order_acquire);
            if (s1 == s2) return true;
        }
        return false;
    }

    bool lookup_impl(IndexKind kind, std::string_view key, pubsub::RCommand& out) const {
        IndexEntry* arr = index(kind);
        if (!arr || key.empty()) return false;
        uint32_t cap = index_capacity();
        uint64_t hash = fnv1a(key);
        bool found = false;
        uint32_t bucket = index_probe_find(arr, cap, hash, key, found);
        if (!found) return false;
        uint32_t slot_idx = arr[bucket].slot_idx.load(std::memory_order_acquire);
        if (slot_idx == kInvalidSlot || slot_idx >= slot_capacity()) return false;
        ReadSnapshot snap;
        if (!read_slot_snapshot(slot_idx, snap)) return false;
        // 校验用**快照里的 key** (跟 order 一起 seqlock 保护), 防 reclaim race
        std::string_view stored;
        switch (kind) {
            case IDX_ORDER_SYS_ID: stored = std::string_view(snap.orderSysId,    strnlen_max(snap.orderSysId,    sizeof(snap.orderSysId))); break;
            case IDX_CLIENT_ORDER: stored = std::string_view(snap.clientOrderId, strnlen_max(snap.clientOrderId, sizeof(snap.clientOrderId))); break;
            case IDX_EXCHANGE_ID:  stored = std::string_view(snap.orderId,       strnlen_max(snap.orderId,       sizeof(snap.orderId))); break;
            default: return false;
        }
        if (stored != key) return false;
        out = snap.order;
        return true;
    }

    size_t iterate_state(uint32_t want_state,
                         const std::function<void(const pubsub::RCommand&)>& cb) const {
        size_t n = 0;
        uint32_t cap = slot_capacity();
        ReadSnapshot snap;
        for (uint32_t i = 0; i < cap; ++i) {
            uint32_t st = slots()[i].state.load(std::memory_order_acquire);
            if (st != want_state) continue;
            if (read_slot_snapshot(i, snap)) {
                cb(snap.order);
                ++n;
            }
        }
        return n;
    }

public:
    // 只读统计 (Writer / Reader 都能调, 不修改任何状态)
    struct Stats {
        uint32_t empty = 0, live = 0, finished = 0, reclaiming = 0;
        uint32_t live_fresh = 0, live_stale = 0;
        uint32_t capacity = 0;
        uint64_t total_inserts = 0, total_updates = 0, total_reclaims = 0;
        uint64_t total_stale_live_reclaims = 0, total_alloc_failures = 0;
        uint64_t min_reclaim_age_ns = 0, max_live_stale_ns = 0;
    };
    Stats stats() const {
        Stats s{};
        if (!header()) return s;
        s.capacity = slot_capacity();
        uint64_t now = now_ns();
        uint64_t max_live_stale = header()->max_live_stale_ns;
        for (uint32_t i = 0; i < s.capacity; ++i) {
            const OmsSlot& slot = slots()[i];
            uint32_t st = slot.state.load(std::memory_order_relaxed);
            switch (st) {
                case SLOT_EMPTY:      ++s.empty; break;
                case SLOT_LIVE: {
                    ++s.live;
                    if (max_live_stale > 0 && (now - slot.last_update_time_ns) > max_live_stale) ++s.live_stale;
                    else                                                                        ++s.live_fresh;
                    break;
                }
                case SLOT_FINISHED:   ++s.finished; break;
                case SLOT_RECLAIMING: ++s.reclaiming; break;
            }
        }
        s.total_inserts             = header()->total_inserts.load(std::memory_order_relaxed);
        s.total_updates             = header()->total_updates.load(std::memory_order_relaxed);
        s.total_reclaims            = header()->total_reclaims.load(std::memory_order_relaxed);
        s.total_stale_live_reclaims = header()->total_stale_live_reclaims.load(std::memory_order_relaxed);
        s.total_alloc_failures      = header()->total_alloc_failures.load(std::memory_order_relaxed);
        s.min_reclaim_age_ns        = header()->min_reclaim_age_ns;
        s.max_live_stale_ns         = header()->max_live_stale_ns;
        return s;
    }

    static uint64_t now_ns() noexcept {
        auto n = std::chrono::steady_clock::now().time_since_epoch();
        return std::chrono::duration_cast<std::chrono::nanoseconds>(n).count();
    }

protected:
    std::string     file_path_;
    int             fd_ = -1;
    bool            read_only_ = false;
    bool            created_new_ = false;
    OmsShmLayout    layout_;
};

// =============================================================================
// 内部工具: 索引 open-addressing 探测
//   查找: 返回 bucket 索引 (命中); found=false 时返回可插入的空/tombstone bucket
// =============================================================================
inline uint32_t index_probe_find(IndexEntry* idx_arr, uint32_t cap,
                                 uint64_t hash, std::string_view key,
                                 bool& found_out) noexcept
{
    found_out = false;
    uint32_t mask = cap - 1;
    uint32_t first_slot = UINT32_MAX;   // 记录第一个可 reuse (empty/tombstone) 的 bucket
    for (uint32_t i = 0; i < kMaxProbeIndex; ++i) {
        uint32_t b = static_cast<uint32_t>((hash + i) & mask);
        IndexEntry& e = idx_arr[b];
        uint64_t h = e.key_hash.load(std::memory_order_acquire);
        if (h == kHashEmpty) {
            if (first_slot == UINT32_MAX) first_slot = b;
            return first_slot;   // probe 遇空即停 (查找失败, 但返回可插入位置)
        }
        if (h == kHashTombstone) {
            if (first_slot == UINT32_MAX) first_slot = b;
            continue;   // 跳过 tombstone 继续查
        }
        if (h == hash) {
            // 验证 key 前缀
            size_t cmp_n = key.size() < 16 ? key.size() : 16;
            if (std::memcmp(e.key_prefix, key.data(), cmp_n) == 0) {
                found_out = true;
                return b;
            }
        }
    }
    return first_slot;   // 探测满仍未找到, 返回可插入位置 (可能是 UINT32_MAX 表示全占)
}

// =============================================================================
// OmsShmWriter — 只由 OMS 进程调用
//
// 使用契约: 全部操作**串行化**从单线程调用。 多线程调用会破坏 next_slot_hint 语义
// 以及 slot state 状态机。 (若确需多线程, 在 caller 侧上一层 mutex 即可)
// =============================================================================
class OmsShmWriter : public OmsShmSegment {
public:
    OmsShmWriter() = default;

    // 便捷 open: 强制读写模式, 不存在则创建。
    // 打开已有 shm 时**自动扫崩溃残留** (RECLAIMING 状态的 slot 强制归位 EMPTY),
    // 防止 tb 崩溃后 slot 永久泄漏。
    void open(const std::string& file_path, uint32_t slot_cap = kDefaultCapacity) {
        OmsShmSegment::open(file_path, slot_cap, /*create*/true, /*read_only*/false);
        // 更新 owner_pid 便于运维追踪当前拥有者
        if (header()) {
            header()->owner_pid = static_cast<uint32_t>(::getpid());
        }
        if (!created_new()) {
            recover_orphan_slots();
        }
    }

    // 崩溃恢复: 扫全表, 把 SLOT_RECLAIMING (writer 崩在 CAS 之后 / 写入完成之前的
    // 中间态) 强制归位 EMPTY, 并 tombstone 关联的索引项 (若有)。
    // 副作用: 若 slot 上有部分写入的旧 orderSysId, 会被抛弃, 数据丢失 <= 1 单/次 crash。
    // 只该在 open() 且已确认 tb 是 sole writer 的场景下调用。
    size_t recover_orphan_slots() {
        size_t recovered = 0;
        uint32_t cap = slot_capacity();
        for (uint32_t i = 0; i < cap; ++i) {
            OmsSlot& s = slots()[i];
            uint32_t st = s.state.load(std::memory_order_acquire);
            if (st != SLOT_RECLAIMING) continue;
            std::fprintf(stderr,
                "[OmsShm][WARN] recover orphan RECLAIMING slot=%u orderSysId=%s "
                "(previous tb likely crashed mid-alloc, discarding)\n",
                i, s.orderSysId);
            // 先 tombstone (若之前 CAS 前尚未 tombstone 旧 index, 现在补上)
            tombstone_all_indices_of_slot(s);
            // 清 key 副本, 避免下次 alloc 拿到又误 tombstone
            std::memset(s.orderSysId,    0, sizeof(s.orderSysId));
            std::memset(s.clientOrderId, 0, sizeof(s.clientOrderId));
            std::memset(s.orderId,       0, sizeof(s.orderId));
            s.state.store(SLOT_EMPTY, std::memory_order_release);
            // 顺便刷 seq 到偶数, 保证任何 reader 看到的都是 stable 状态
            uint64_t seq = s.seq.load(std::memory_order_acquire);
            if (seq & 1) s.seq.store(seq + 1, std::memory_order_release);
            ++recovered;
        }
        if (recovered > 0) {
            std::fprintf(stderr, "[OmsShm] recovered %zu orphan slot(s) from previous crash\n", recovered);
        }
        return recovered;
    }

    // 运行期调整两个 TTL (立即生效, alloc_slot 下一次调用就看到新值)
    void set_min_reclaim_age_ns(uint64_t ns) noexcept {
        if (header()) header()->min_reclaim_age_ns = ns;
    }
    void set_max_live_stale_ns(uint64_t ns) noexcept {
        if (header()) header()->max_live_stale_ns = ns;
    }

    // 清空所有 slot 和索引 (**仅用于工具 / 测试**, 别在生产 OMS 里调)
    void reset_all() {
        if (!is_open()) return;
        uint32_t sc = slot_capacity();
        uint32_t ic = index_capacity();
        std::memset(slots(), 0, static_cast<size_t>(sc) * sizeof(OmsSlot));
        for (uint32_t k = 0; k < IDX_COUNT; ++k) {
            std::memset(index(k), 0, static_cast<size_t>(ic) * sizeof(IndexEntry));
        }
        header()->next_slot_hint.store(0);
        header()->global_seq.store(0);
        header()->total_inserts.store(0);
        header()->total_updates.store(0);
        header()->total_reclaims.store(0);
        header()->total_alloc_failures.store(0);
    }

    // 主接口: upsert 一条订单
    //   如果 orderSysId 已存在 → update 现有 slot
    //   否则 → alloc 新 slot 写入
    // 返回: slot_idx (kInvalidSlot 表示失败, 环满且无 FINISHED 可回收)
    uint32_t upsert(const pubsub::RCommand& rcmd) {
        std::string_view sys_id_sv(rcmd.body.orderResponse.orderSysId);
        if (sys_id_sv.empty()) {
            // 没主键无法索引
            return kInvalidSlot;
        }
        // 复用现有 slot?
        uint32_t existing = find_slot(IDX_ORDER_SYS_ID, sys_id_sv);
        if (existing != kInvalidSlot) {
            update_slot(existing, rcmd);
            header()->total_updates.fetch_add(1, std::memory_order_relaxed);
            return existing;
        }
        // 新 slot
        uint32_t idx = alloc_slot();
        if (idx == kInvalidSlot) {
            header()->total_alloc_failures.fetch_add(1, std::memory_order_relaxed);
            return kInvalidSlot;
        }
        write_new_slot(idx, rcmd);
        if (!insert_all_indices(idx, rcmd)) {
            // 索引全满 (罕见, index_capacity=2N + linear probe 一般不会到) — 回滚 slot
            std::fprintf(stderr,
                "[OmsShm][ERROR] insert_index full, rollback slot=%u orderSysId=%s. "
                "Consider raising index_capacity or investigating tombstone leak.\n",
                idx, rcmd.body.orderResponse.orderSysId);
            OmsSlot& s = slots()[idx];
            s.seq.fetch_add(1, std::memory_order_release);
            s.state.store(SLOT_EMPTY, std::memory_order_release);
            std::memset(s.orderSysId,    0, sizeof(s.orderSysId));
            std::memset(s.clientOrderId, 0, sizeof(s.clientOrderId));
            std::memset(s.orderId,       0, sizeof(s.orderId));
            s.seq.fetch_add(1, std::memory_order_release);
            header()->total_alloc_failures.fetch_add(1, std::memory_order_relaxed);
            return kInvalidSlot;
        }
        header()->total_inserts.fetch_add(1, std::memory_order_relaxed);
        return idx;
    }

    // 显式删除 (罕见, 一般 orderSysId 生命周期跟 slot 一致, 由 reclaim 自动清)
    bool remove(std::string_view orderSysId) {
        uint32_t idx = find_slot(IDX_ORDER_SYS_ID, orderSysId);
        if (idx == kInvalidSlot) return false;
        OmsSlot& s = slots()[idx];
        // Tombstone 所有 index 项
        tombstone_all_indices_of_slot(s);
        // 标记 slot 为 EMPTY, 让后续 alloc 直接用
        s.seq.fetch_add(1, std::memory_order_release);   // odd
        s.state.store(SLOT_EMPTY, std::memory_order_release);
        std::memset(s.orderSysId, 0, sizeof(s.orderSysId));
        std::memset(s.clientOrderId, 0, sizeof(s.clientOrderId));
        std::memset(s.orderId, 0, sizeof(s.orderId));
        s.seq.fetch_add(1, std::memory_order_release);   // even
        return true;
    }

private:
    // 找一个可用 slot. 优先级:
    //   1. EMPTY  → 直接用
    //   2. FINISHED 且 age > min_reclaim_age_ns → 回收 (最常见路径)
    //   3. LIVE 且 age > max_live_stale_ns → **强制回收** (卡单 / 僵尸兜底), 打 WARN
    // 找到即 CAS 到 RECLAIMING, 保证只有一个 writer 抢到 slot。
    uint32_t alloc_slot() {
        uint32_t cap = slot_capacity();
        uint32_t mask = cap - 1;
        bool pow2 = (cap & (cap - 1)) == 0;
        uint64_t now = now_ns();
        uint64_t min_finished_age = header()->min_reclaim_age_ns;
        uint64_t max_live_stale   = header()->max_live_stale_ns;

        for (uint32_t attempt = 0; attempt < kMaxProbeSlots; ++attempt) {
            uint64_t h = header()->next_slot_hint.fetch_add(1, std::memory_order_relaxed);
            uint32_t idx = pow2 ? static_cast<uint32_t>(h & mask)
                                : static_cast<uint32_t>(h % cap);
            OmsSlot& s = slots()[idx];
            uint32_t st = s.state.load(std::memory_order_acquire);

            // (1) EMPTY: 直接用
            if (st == SLOT_EMPTY) {
                uint32_t expected = SLOT_EMPTY;
                if (s.state.compare_exchange_strong(expected, SLOT_RECLAIMING,
                        std::memory_order_acq_rel)) {
                    return idx;
                }
                continue;
            }

            // (2) FINISHED 且过了最小 TTL
            if (st == SLOT_FINISHED) {
                uint64_t finish_age = now - s.finish_time_ns;
                if (finish_age < min_finished_age) continue;
                uint32_t expected = SLOT_FINISHED;
                if (s.state.compare_exchange_strong(expected, SLOT_RECLAIMING,
                        std::memory_order_acq_rel)) {
                    tombstone_all_indices_of_slot(s);
                    header()->total_reclaims.fetch_add(1, std::memory_order_relaxed);
                    return idx;
                }
                continue;
            }

            // (3) LIVE 且**长时间无更新** — 卡单 / 上层 bug 导致的僵尸单, 强制回收兜底
            //     `last_update_time_ns` 每次 upsert (create + update) 都刷,
            //     所以正常成交/撤单流程下 LIVE slot 的 last_update 永远是最近的。
            //     只有当 tb 或策略挂了没送 execution report, slot 才会 stale。
            if (st == SLOT_LIVE && max_live_stale > 0) {
                uint64_t idle_age = now - s.last_update_time_ns;
                if (idle_age < max_live_stale) continue;
                uint32_t expected = SLOT_LIVE;
                if (s.state.compare_exchange_strong(expected, SLOT_RECLAIMING,
                        std::memory_order_acq_rel)) {
                    // ⚠ 强制回收 LIVE 意味着**订单状态永久丢失**, 打 WARN 便于运维查
                    std::fprintf(stderr,
                        "[OmsShm][WARN] force reclaim STALE LIVE slot=%u orderSysId=%s "
                        "idle_age_ms=%llu (> max_live_stale_ms=%llu). Zombie order?\n",
                        idx, s.orderSysId,
                        (unsigned long long)(idle_age / 1'000'000),
                        (unsigned long long)(max_live_stale / 1'000'000));
                    tombstone_all_indices_of_slot(s);
                    header()->total_stale_live_reclaims.fetch_add(1, std::memory_order_relaxed);
                    header()->total_reclaims.fetch_add(1, std::memory_order_relaxed);
                    return idx;
                }
                continue;
            }
            // 其他 (LIVE 但没 stale, 或 RECLAIMING 中): 跳过
        }
        return kInvalidSlot;
    }

    void write_new_slot(uint32_t idx, const pubsub::RCommand& rcmd) {
        OmsSlot& s = slots()[idx];
        s.seq.fetch_add(1, std::memory_order_release);   // odd = writing
        uint64_t t = now_ns();
        s.create_time_ns      = t;
        s.last_update_time_ns = t;    // ← 卡单检测基准
        s.finish_time_ns      = 0;
        copy_key(s.orderSysId, sizeof(s.orderSysId), rcmd.body.orderResponse.orderSysId);
        // 复合 client key: strategyId + int64 cid (匹配 OMS 老逻辑, 防跨策略撞车)
        int64_t cid = rcmd.body.orderResponse.clientOrderId;
        compose_client_key(s.clientOrderId, sizeof(s.clientOrderId),
                           std::string_view(rcmd.body.orderResponse.strategyId,
                                            strnlen_max(rcmd.body.orderResponse.strategyId,
                                                        sizeof(rcmd.body.orderResponse.strategyId))),
                           cid);
        copy_key(s.orderId, sizeof(s.orderId), rcmd.body.orderResponse.orderId);
        std::memcpy(&s.order, &rcmd, sizeof(pubsub::RCommand));
        SlotState next_state = is_terminal(rcmd.body.orderResponse.orderStatus)
                             ? SLOT_FINISHED : SLOT_LIVE;
        if (next_state == SLOT_FINISHED) {
            s.finish_time_ns = t;
        }
        s.state.store(next_state, std::memory_order_release);
        s.seq.fetch_add(1, std::memory_order_release);   // even = stable
        header()->global_seq.fetch_add(1, std::memory_order_relaxed);
    }

    void update_slot(uint32_t idx, const pubsub::RCommand& rcmd) {
        OmsSlot& s = slots()[idx];
        s.seq.fetch_add(1, std::memory_order_release);
        uint64_t t = now_ns();
        s.last_update_time_ns = t;    // ← 每次更新都刷, LIVE 只要还在活跃就永远不会 stale
        // 完整覆盖 order
        std::memcpy(&s.order, &rcmd, sizeof(pubsub::RCommand));
        bool orderId_changed = false;
        if (rcmd.body.orderResponse.orderId[0] &&
            std::strncmp(s.orderId, rcmd.body.orderResponse.orderId, sizeof(s.orderId)) != 0) {
            copy_key(s.orderId, sizeof(s.orderId), rcmd.body.orderResponse.orderId);
            orderId_changed = true;
        }
        if (is_terminal(rcmd.body.orderResponse.orderStatus)) {
            if (s.finish_time_ns == 0) s.finish_time_ns = t;
            s.state.store(SLOT_FINISHED, std::memory_order_release);
        } else {
            s.state.store(SLOT_LIVE, std::memory_order_release);
        }
        s.seq.fetch_add(1, std::memory_order_release);
        if (orderId_changed && s.orderId[0]) {
            insert_index(IDX_EXCHANGE_ID, s.orderId, idx);
        }
        header()->global_seq.fetch_add(1, std::memory_order_relaxed);
    }

    // 复制字符串到定长 buffer, null-terminate
    static void copy_key(char* dst, size_t dst_sz, const char* src) noexcept {
        if (!src) { dst[0] = 0; return; }
        size_t n = strnlen_max(src, dst_sz - 1);
        std::memcpy(dst, src, n);
        dst[n] = 0;
    }

    static bool is_terminal(OrderStatus st) noexcept {
        return st == OS_FILLED || st == OS_CANCELED || st == OS_REJECTED
            || st == OS_FAILED;
    }

    // 插入三层索引. 主索引 (orderSysId) 失败 → 整体失败; 别名失败只 log, 因为
    // 主索引 OK 就能查到, 别名冗余用。
    bool insert_all_indices(uint32_t idx, const pubsub::RCommand& /*rcmd*/) {
        OmsSlot& s = slots()[idx];
        if (s.orderSysId[0]) {
            if (!insert_index(IDX_ORDER_SYS_ID, s.orderSysId, idx)) {
                return false;   // 主索引失败, 上层回滚
            }
        }
        if (s.clientOrderId[0]) {
            if (!insert_index(IDX_CLIENT_ORDER, s.clientOrderId, idx)) {
                std::fprintf(stderr, "[OmsShm][WARN] clientOrderId alias insert full slot=%u\n", idx);
            }
        }
        if (s.orderId[0]) {
            if (!insert_index(IDX_EXCHANGE_ID, s.orderId, idx)) {
                std::fprintf(stderr, "[OmsShm][WARN] orderId alias insert full slot=%u\n", idx);
            }
        }
        return true;
    }

    bool insert_index(IndexKind kind, std::string_view key, uint32_t slot_idx) {
        IndexEntry* arr = index(kind);
        uint32_t cap = index_capacity();
        uint64_t hash = fnv1a(key);
        // insert 特有: 优先复用 tombstone (若探测过程中先命中 tombstone 后命中 key,
        // 应挪到 tombstone 位置压缩; 若不命中 key, 直接用 tombstone 或 empty)。
        // 我们的探测: 遇到 EMPTY 停; tombstone 记 first_reusable 继续; 命中 hash+prefix 就改。
        uint32_t mask = cap - 1;
        uint32_t first_reusable = UINT32_MAX;   // 第一个 tombstone 或 empty
        for (uint32_t i = 0; i < kMaxProbeIndex; ++i) {
            uint32_t b = static_cast<uint32_t>((hash + i) & mask);
            IndexEntry& e = arr[b];
            uint64_t h = e.key_hash.load(std::memory_order_acquire);
            if (h == kHashEmpty) {
                if (first_reusable == UINT32_MAX) first_reusable = b;
                break;   // 到 empty 说明后面必然 empty
            }
            if (h == kHashTombstone) {
                if (first_reusable == UINT32_MAX) first_reusable = b;
                continue;
            }
            if (h == hash) {
                size_t cmp_n = key.size() < 16 ? key.size() : 16;
                if (std::memcmp(e.key_prefix, key.data(), cmp_n) == 0) {
                    // 覆盖已有 (同 orderSysId 复用 slot 的场景)
                    e.slot_idx.store(slot_idx, std::memory_order_release);
                    return true;
                }
            }
        }
        if (first_reusable == UINT32_MAX) return false;   // 全占
        IndexEntry& e = arr[first_reusable];
        size_t cmp_n = key.size() < 16 ? key.size() : 16;
        std::memcpy(e.key_prefix, key.data(), cmp_n);
        if (cmp_n < 16) std::memset(e.key_prefix + cmp_n, 0, 16 - cmp_n);
        e.slot_idx.store(slot_idx, std::memory_order_release);
        e.key_hash.store(hash, std::memory_order_release);   // 最后 store hash → publish
        return true;
    }

    // 找 slot_idx (writer 用, 无需 seqlock, 因为写操作串行)
    uint32_t find_slot(IndexKind kind, std::string_view key) {
        IndexEntry* arr = index(kind);
        uint32_t cap = index_capacity();
        uint64_t hash = fnv1a(key);
        bool found = false;
        uint32_t bucket = index_probe_find(arr, cap, hash, key, found);
        if (!found) return kInvalidSlot;
        return arr[bucket].slot_idx.load(std::memory_order_acquire);
    }

    // Tombstone slot 关联的所有索引项
    void tombstone_all_indices_of_slot(OmsSlot& s) {
        if (s.orderSysId[0])    tombstone_index(IDX_ORDER_SYS_ID, s.orderSysId);
        if (s.clientOrderId[0]) tombstone_index(IDX_CLIENT_ORDER, s.clientOrderId);
        if (s.orderId[0])       tombstone_index(IDX_EXCHANGE_ID,  s.orderId);
    }

    void tombstone_index(IndexKind kind, std::string_view key) {
        IndexEntry* arr = index(kind);
        uint32_t cap = index_capacity();
        uint64_t hash = fnv1a(key);
        bool found = false;
        uint32_t bucket = index_probe_find(arr, cap, hash, key, found);
        if (!found) return;
        IndexEntry& e = arr[bucket];
        e.key_hash.store(kHashTombstone, std::memory_order_release);
        e.slot_idx.store(kInvalidSlot, std::memory_order_release);
    }
};

// =============================================================================
// OmsShmReader — 策略进程只读访问的语义化 wrapper
//
// 所有 lookup_by_* / iterate_* / stats() 都定义在基类 OmsShmSegment 里,
// Writer 也可以直接调用 (OMS 侧的 query_order / 对账场景)。
// Reader 特有的只是 open() 用 PROT_READ mmap, 编译期区分意图 + 运行期强制只读:
// 若代码里误用了 Writer 的 upsert 但对象类型是 Reader → SEGV, 不会静默写坏 shm。
// =============================================================================
class OmsShmReader : public OmsShmSegment {
public:
    OmsShmReader() = default;

    void open(const std::string& file_path) {
        OmsShmSegment::open(file_path, 0, /*create*/false, /*read_only*/true);
    }
};

}  // namespace shm
}  // namespace oms
