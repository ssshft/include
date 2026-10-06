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
constexpr uint32_t kVersion        = 3;            // v2: 加 last_update_time_ns + max_live_stale_ns
                                                   // v3: index_capacity 由 2N 改成 next_pow2(4N)
constexpr uint32_t kDefaultCapacity = 100'000;     // slot 数
constexpr uint64_t kDefaultMinReclaimAgeNs   = 60ULL * 1'000'000'000;         // 60s: FINISHED 最小 TTL
constexpr uint64_t kDefaultMaxLiveStaleNs    = 24ULL * 3600 * 1'000'000'000;  // 24h: LIVE 无更新最长容忍
constexpr uint32_t kInvalidSlot    = UINT32_MAX;
constexpr uint32_t kMaxProbeSlots  = 128;          // 环形 alloc 最大探测

// 索引 open-addressing 最大探测步数。
//   ★ 这不是一个"给大点就安全"的余量常数: 环填满后 `empty` 必然排干到 0
//     (live + tomb == index_capacity), 插入能否成功**完全**取决于本值步内能否碰到
//     tombstone, 而线性探测下所需步数的上界 = **最长连续 live 段**。
//   ★ 实测 (OMS_SHM_REVIEW.md §2.5): 索引 2N 时最长连续段 19 → 35 → 38~55 → 41~49,
//     **全部 > 32** → 已经在静默丢单 (slot_cap=65536: 1.9e-4, 随时间恶化到 ~2.5e-4)。
//     容量改成 4N 后连续段塌到 13~20, 对 32 有 1.6 倍以上余量。
//     **所以该修的是容量 (index_capacity), 不是把这个数调大。**
//   ★ 调大本值只是治标: 门槛往后挪, 连续段照样随容量缓慢增长, 永远说不清余量。
//     要判断够不够, 看 Stats::index_occupancy[].max_live_run 与本值的比值。
constexpr uint32_t kMaxProbeIndex  = 32;
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

// 索引名的唯一来源 (日志 / 工具共用, 避免两处字符串漂移)
inline const char* index_kind_name(uint32_t k) noexcept {
    switch (k) {
        case IDX_ORDER_SYS_ID: return "orderSysId";
        case IDX_CLIENT_ORDER: return "clientOrderId";
        case IDX_EXCHANGE_ID:  return "orderId";
        default:               return "?";
    }
}

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

inline bool is_pow2(uint32_t n) noexcept {
    return n != 0 && (n & (n - 1)) == 0;
}

// 向上取整到 2 的幂 (n <= 2^31; 调用方需先做范围校验)
inline uint32_t next_pow2(uint32_t n) noexcept {
    if (n <= 1) return 1;
    --n;
    n |= n >> 1;  n |= n >> 2;  n |= n >> 4;
    n |= n >> 8;  n |= n >> 16;
    return n + 1;
}

// 索引探测的第 i 个 bucket。
//   ★ 三处探测 (index_probe_find / insert_index / 经由前者的 tombstone_index)
//     **必须**走同一个函数, 否则 lookup 与 insert 会得到不同的探测序列。
//   ★ cap 是 2 的幂时 (hash+i) & (cap-1) 才是一个满射的线性探测序列;
//     cap 不是 2 的幂时 mask 的高位恒为 0, 可命中的 bucket 数塌缩到 2^popcount(mask),
//     索引会提前饱和。所以这里必须跟 alloc_slot 一样分 pow2 / 非 pow2 两条路。
inline uint32_t index_probe_bucket(uint64_t hash, uint32_t i,
                                   uint32_t cap, uint32_t mask, bool pow2) noexcept {
    const uint64_t h = hash + i;
    return pow2 ? static_cast<uint32_t>(h & mask)
                : static_cast<uint32_t>(h % cap);
}

// =============================================================================
// Header (固定 4 KB, cacheline 对齐)
// =============================================================================
struct alignas(64) OmsShmHeader {
    uint32_t magic;                 // 校验
    uint32_t version;
    uint32_t slot_capacity;         // N
    uint32_t index_capacity;        // next_pow2(4N), 单个索引大小; **必须是 2 的幂**
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
    char pad[4096 - 112];
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
// 索引条目 ⇄ slot 的"身份判定"
//
//   索引条目里存的是 `fnv1a(key)` + `key_prefix[16]`, 这只是**桶提示**, 不是身份:
//     - 它不覆盖 key 的第 17 字节以后, 也不把长度当独立判据;
//     - 实测 (N = 100000) 交易所自增 orderId 只有 **101** 个不同前缀, 而
//       strategyId >= 16 字符的复合 clientOrderId 只有 **1** 个前缀 ——
//       对这两类真实 key, 前缀**完全没有区分度**, 判定退化成"只有 64 位 hash"。
//   真正的身份在 slot 的 key 副本里, 所以任何"是不是同一个 key"的判定都必须回读
//   slot 比**完整 key**。前缀只配用来做快速否定 (16 字节比较比回读 slot 便宜)。
//
//   不这么做的后果 (实测): 一次 hash + 前缀碰撞会让 `upsert` 把新单**并进别人的
//   slot** —— 查 A 返回 B 的报单体 (最坏的一类错误: 拿错单的状态去驱动撤单/平仓)。
//   详见 tb/tools/OMS_SHM_REVIEW.md §2.3。
// =============================================================================
enum class EntryKeyMatch : uint8_t {
    kMatch,   // slot 确实持有这个 key
    kOther,   // slot 持有**别的** key —— 真碰撞, 必须继续探测, 绝不能当成命中
    kStale,   // 条目指向的 slot 越界 / 已空 —— 条目陈旧: 不是命中, 但该 bucket 可复用
};

// 按 kind 取 slot 的 key 副本 buffer (越界 / 无 slot → nullptr)
inline const char* slot_key_ptr(const OmsSlot* slots_arr, uint32_t slot_cap,
                                uint32_t slot_idx, IndexKind kind,
                                size_t& cap_out) noexcept
{
    if (!slots_arr || slot_idx == kInvalidSlot || slot_idx >= slot_cap) return nullptr;
    const OmsSlot& s = slots_arr[slot_idx];
    switch (kind) {
        case IDX_ORDER_SYS_ID: cap_out = sizeof(s.orderSysId);    return s.orderSysId;
        case IDX_CLIENT_ORDER: cap_out = sizeof(s.clientOrderId); return s.clientOrderId;
        case IDX_EXCHANGE_ID:  cap_out = sizeof(s.orderId);       return s.orderId;
        default:               cap_out = 0;                       return nullptr;
    }
}

// 索引条目 e 与目标 key 的关系。★ 必须比**完整 key**, 不能用 e.key_prefix。
//   代价敏感: 只做 memcmp(key.size()) + 一个终止符检查, 不做 strlen 扫描。
//   (key 副本是 NUL 结尾的定长 buffer, 由 copy_key / memset 保证)
inline EntryKeyMatch entry_key_match(const IndexEntry& e,
                                     const OmsSlot* slots_arr, uint32_t slot_cap,
                                     IndexKind kind, std::string_view key) noexcept
{
    size_t cap = 0;
    const char* held = slot_key_ptr(slots_arr, slot_cap,
                                    e.slot_idx.load(std::memory_order_acquire),
                                    kind, cap);
    if (!held) return EntryKeyMatch::kStale;
    if (held[0] == '\0') return EntryKeyMatch::kStale;   // 空副本 = 条目陈旧
    if (key.size() >= cap) return EntryKeyMatch::kOther; // 副本装不下这个 key → 不是它
    if (std::memcmp(held, key.data(), key.size()) != 0) return EntryKeyMatch::kOther;
    return held[key.size()] == '\0' ? EntryKeyMatch::kMatch : EntryKeyMatch::kOther;
}


// =============================================================================
// 内部工具: 索引 open-addressing 探测
//   查找: 返回 bucket 索引 (命中); found=false 时返回可插入的空/tombstone bucket
//   ★ 命中判据 = hash 相等 + 前缀相等 (**快速否定**) + slot 里持有完整 key (真判据)。
//     只比 hash + 前缀会把"另一个 key 占着同一个 bucket"当成命中 → 查错单。
// =============================================================================
inline uint32_t index_probe_find(IndexEntry* idx_arr, uint32_t cap,
                                 uint64_t hash, std::string_view key,
                                 const OmsSlot* slots_arr, uint32_t slot_cap,
                                 IndexKind kind,
                                 bool& found_out) noexcept
{
    found_out = false;
    if (cap == 0) return UINT32_MAX;
    const uint32_t mask = cap - 1;
    const bool     pow2 = is_pow2(cap);
    uint32_t first_slot = UINT32_MAX;   // 记录第一个可 reuse (empty/tombstone/stale) 的 bucket
    for (uint32_t i = 0; i < kMaxProbeIndex; ++i) {
        uint32_t b = index_probe_bucket(hash, i, cap, mask, pow2);
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
            size_t cmp_n = key.size() < 16 ? key.size() : 16;
            if (std::memcmp(e.key_prefix, key.data(), cmp_n) == 0) {
                switch (entry_key_match(e, slots_arr, slot_cap, kind, key)) {
                case EntryKeyMatch::kMatch:
                    found_out = true;
                    return b;
                case EntryKeyMatch::kStale:
                    // 条目指向的 slot 已经空了 —— 条目陈旧。不当作命中, 继续找真条目,
                    // 找不到就把它当可插入位置返回 (调用方会复用)。
                    if (first_slot == UINT32_MAX) first_slot = b;
                    continue;
                case EntryKeyMatch::kOther:
                    // hash 与前缀都相同, 但 slot 里是**另一个 key** —— 真碰撞。
                    // 继续探测; 若在此停住, 就会把 B 的查询当成 A 命中, 返回错单。
                    continue;
                }
            }
        }
    }
    return first_slot;   // 探测满仍未找到, 返回可插入位置 (可能是 UINT32_MAX 表示全占)
}


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
        // ★ 索引容量**必须**是 2 的幂 —— 三处探测都用 (hash+i) & (cap-1), 只有 cap 是
        //   2 的幂时该序列才满射。反例: slot_cap=100000 → 2N=200000, mask=0x30D3F
        //   只有 11 个 bit 置位 → 索引最多 2048 个 bucket 就饱和, 之后 upsert 全部失败。
        //   注意: 这里只对**新建**的文件生效; 打开已有文件时容量一律从 header 读。
        //
        // ★ 倍数用 4 而不是 2 (v3 变更, 见 OMS_SHM_REVIEW.md §2.5):
        //   环填满后 `empty` 必然排干到 0 (live + tomb == index_capacity, 与索引开多大
        //   无关), 此后插入能否成功只取决于 kMaxProbeIndex 步内能否碰到 tombstone,
        //   上界 = 最长连续 live 段。2N 时可复用桶密度只有 0.5, 实测最长连续段
        //   19 → 35 → 38~55 → 41~49 (随容量增长), **全部 > 32** → 静默丢单。
        //   4N 把密度提到 0.75, 连续段塌到 13~20 (1M slot 时仍只有 20), 对 32 有
        //   1.6 倍以上余量; 代价是每个索引内存 +100% (默认 100k slot: 24 → 48 MiB)。
        //   实测 update/lookup 单次耗时不变, 平均查询探测步数 2.45 → 1.44。
        uint32_t use_idx = (use_cap > 0) ? next_pow2(use_cap * 4) : 2;
        size_t   expect_size = OmsShmLayout::compute_total_size(use_cap, use_idx);

        if (st.st_size == 0) {
            // 新文件
            if (read_only) {
                throw std::runtime_error("OmsShm: read-only open on empty file " + file_path);
            }
            // slot_cap 只在新建时被采纳, 所以范围校验也只在这里做
            // (OmsShmReader::open 故意传 slot_cap=0, 容量从 header 读)
            // ★ 上界是 2^28 而不是 2^30: index_capacity = next_pow2(4N) 要能放进
            //   uint32_t, 4 * 2^28 = 2^30 是上限; 再大 next_pow2 会先溢出再返回 1,
            //   于是索引容量变成 1 个 bucket —— 静默写坏, 必须在这里挡住。
            if (slot_cap == 0 || slot_cap > (1u << 28)) {
                ::close(fd_); fd_ = -1;
                throw std::runtime_error(
                    "OmsShm: slot_cap out of range (1 .. 2^28; "
                    "index_capacity = next_pow2(4*slot_cap) must fit in uint32_t)");
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
            layout_.map_from(p, use_cap, use_idx);
            std::memset(p, 0, expect_size);
            layout_.header->magic          = kMagic;
            layout_.header->version        = kVersion;
            layout_.header->slot_capacity  = use_cap;
            layout_.header->index_capacity = use_idx;
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
            // ★ 老版本建的文件 index_capacity = 2*slot_cap, 例如 slot_cap=100000 → 200000,
            //   不是 2 的幂 → 索引在 2048 条就静默饱和。必须拒绝, 不能带病运行。
            if (!is_pow2(hdr_idx)) {
                ::munmap(p, expect_size);
                ::close(fd_); fd_ = -1;
                throw std::runtime_error(
                    "OmsShm: index_capacity=" + std::to_string(hdr_idx) +
                    " is not a power of two; this file was created by an older build whose "
                    "index probe saturates early. Delete " + file_path +
                    " and let it be recreated.");
            }
            // ★ 索引倍数校验 (v3)。老文件 index_capacity = next_pow2(2N) **也是 2 的幂**,
            //   所以上面那条 pow2 检查挡不住它 —— 必须在版本号之外再按倍数挡一次。
            //   用 64 位比较避免 hdr_cap 很大时 4 * hdr_cap 溢出。
            if (static_cast<uint64_t>(hdr_idx) < static_cast<uint64_t>(hdr_cap) * 4ULL) {
                ::munmap(p, expect_size);
                ::close(fd_); fd_ = -1;
                throw std::runtime_error(
                    "OmsShm: index_capacity=" + std::to_string(hdr_idx) +
                    " < 4 * slot_capacity=" + std::to_string(hdr_cap) +
                    "; this file was created by a build with the old 2N index sizing, which "
                    "silently drops orders once the ring is full (see OMS_SHM_REVIEW.md §2.5). "
                    "Delete " + file_path + " and let it be recreated.");
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
        uint32_t bucket = index_probe_find(arr, cap, hash, key,
                                           slots(), slot_capacity(), kind, found);
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

        // ★ 索引占用 (每个索引一套)。
        //   稳态下 slot 环填满后: live ≈ slot_cap, tomb ≈ slot_cap, 所以
        //   live + tomb ≈ index_capacity 且 empty → 0 —— 这是**正常现象**, 不是泄漏。
        //   此时插入能否成功全靠 `kMaxProbeIndex` 步内找到 tombstone。
        //   关注点: empty 掉到 0 附近, 且 total_alloc_failures 开始增长 → 索引要加容量。
        struct IndexOccupancy {
            uint32_t live  = 0;   // 有效条目
            uint32_t tomb  = 0;   // tombstone (已删, 仍占 bucket)
            uint32_t empty = 0;   // 从未用过
            uint32_t used() const noexcept { return live + tomb; }

            // ★ 探测余量 (B6): 最长的一段**连续 live bucket**。
            //   线性探测下, 一次插入需要走的步数上界就是这个值 —— 因为插入必须走到
            //   某个可复用 bucket (empty/tombstone), 而连续的 live 段一个都提供不了。
            //   与 kMaxProbeIndex 的比值就是真实余量:
            //     max_live_run <  kMaxProbeIndex        → 有余量 (正常)
            //     3 * max_live_run >= 2 * kMaxProbeIndex → 余量不足 1.5 倍, 要留意
            //     max_live_run >= kMaxProbeIndex        → 已经在丢单 (total_alloc_failures 会涨)
            //   实测 (4N): 6 / 10 / 13~16 / 16 / 19~20 对应 slot_cap
            //   1K / 8K / 64K~512K / 512K / 1M —— 一直 < 32, 且随容量增长很慢。
            //   索引 2N 时则是 19 / 35 / 38~55 / 41~49 / 38~43, 全部 > 32 (见 §2.5)。
            uint32_t max_live_run = 0;
        };
        uint32_t       index_capacity = 0;              // 每个索引的 bucket 数
        uint32_t       probe_max      = 0;              // = kMaxProbeIndex, 便于工具直接算余量
        IndexOccupancy index_occupancy[IDX_COUNT] = {}; // 下标 = IndexKind
    };
    Stats stats() const {
        Stats s{};
        if (!header()) return s;
        s.capacity = slot_capacity();
        s.probe_max = kMaxProbeIndex;
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
        // ★ 索引占用 + 探测余量扫描。纯诊断用 (O(index_capacity * IDX_COUNT)), 不在热路径上。
        s.index_capacity = index_capacity();
        for (uint32_t k = 0; k < IDX_COUNT; ++k) {
            if (index(k)) s.index_occupancy[k] = index_occupancy_of(static_cast<IndexKind>(k));
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

    // 单个索引的占用 + 最长连续 live 段快照 (诊断用, O(index_capacity))。
    // stats() 与 upsert 失败日志共用; 最长连续段与占用共用同一个循环, 不额外扫。
    Stats::IndexOccupancy index_occupancy_of(IndexKind kind) const noexcept {
        Stats::IndexOccupancy occ{};
        if (kind >= IDX_COUNT) return occ;              // 防越界 (index() 本身不检查)
        const uint32_t cap = index_capacity();
        IndexEntry* arr = index(kind);
        if (!arr || cap == 0) return occ;
        uint32_t run = 0;                               // 当前连续 live 段长度
        uint32_t head_run = 0;                          // 从 bucket 0 起的那段 (用于跨尾部合并)
        for (uint32_t i = 0; i < cap; ++i) {
            uint64_t h = arr[i].key_hash.load(std::memory_order_relaxed);
            if (h == kHashEmpty) {
                ++occ.empty;
                if (i == run) head_run = run;           // 刚结束的这段起点是 0
                run = 0;
            } else if (h == kHashTombstone) {
                ++occ.tomb;
                if (i == run) head_run = run;
                run = 0;
            } else {
                ++occ.live;
                if (++run > occ.max_live_run) occ.max_live_run = run;
            }
        }
        // ★ 探测是环形的 ((hash+i) & mask 会绕回 0), 所以尾部那段和从 0 开始的那段
        //   在探测意义上**是连续的**, 必须合并 —— 否则会低估最长连续段。
        if (head_run > 0 && run > 0 && head_run + run > occ.max_live_run) {
            occ.max_live_run = head_run + run;
        }
        return occ;
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
        IndexKind failed = IDX_COUNT;
        if (!insert_all_indices(idx, &failed)) {
            // 索引写不进去 —— 罕见, 但一旦发生这张单就**没进 SHM** (total_alloc_failures++)。
            // ★ 判读 (B6): 环满后 empty 必然排干到 0 (与索引开多大无关), 插入必须走到某个
            //   可复用 bucket, 而连续 live 段一个都不提供 —— 所以"最长连续 live 段
            //   >= probe_max"就是**唯一**成因。别再写"加大 slot_cap": empty 照样排干到 0。
            // ★ index_capacity 是 next_pow2(4 × slot_cap), **不是** 4 × slot_cap。
            // ★ 也**不要**再去查 "tombstone 泄漏" —— 实测已排除: 环满时
            //   live + tomb == index_capacity 是设计如此 (见 OMS_SHM_REVIEW.md §0.2)。
            const Stats::IndexOccupancy occ = index_occupancy_of(failed);
            const uint32_t icap = index_capacity();
            const bool run_too_long = (occ.max_live_run >= kMaxProbeIndex);
            const char* verdict =
                run_too_long
                    ? "最长连续 live 段 >= probe_max → 探测步数被 live 段吃光 (**聚集**)。"
                      "加大 slot_cap 没用, 要加大 index_capacity 的倍数"
                    : "快照里 max_live_run < probe_max, 本不该失败 → 请连同上面的快照上报";
            std::fprintf(stderr,
                "[OmsShm][ERROR] index insert failed: kind=%s slot=%u orderSysId=%s\n"
                "  slot_cap=%u  index_capacity=%u (=next_pow2(4*slot_cap))\n"
                "  probe headroom: max_live_run=%u  probe_max=%u\n"
                "  index occupancy: live=%u tomb=%u empty=%u  used_pct=%.2f\n"
                "  %s\n"
                "  → 该单未写入 SHM, total_alloc_failures 已 +1, 上层会当作写失败处理\n",
                index_kind_name(failed), idx, rcmd.body.orderResponse.orderSysId,
                slot_capacity(), icap,
                occ.max_live_run, kMaxProbeIndex,
                occ.live, occ.tomb, occ.empty,
                icap ? 100.0 * occ.used() / icap : 0.0,
                verdict);
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
        const auto& resp = rcmd.body.orderResponse;

        // ★ 热路径判定: key 的源字段一个都没动 → 完全不做同步 (正常成交/部分成交回报就是这样)。
        //   必须在 memcpy 覆盖 s.order **之前**算, 才能拿旧报单体比。
        //   每个子句都以"这次带没带这个字段"开头 —— 上层没回传该字段时直接短路,
        //   不会因为"报单体里是空的、副本里是满的"而每次都掉进冷路径。
        const bool need_sync =
            (resp.orderSysId[0] && std::strncmp(s.orderSysId, resp.orderSysId,
                                                sizeof(s.orderSysId)) != 0) ||
            (resp.orderId[0]    && std::strncmp(s.orderId, resp.orderId,
                                                sizeof(s.orderId))    != 0) ||
            (resp.strategyId[0] &&
                (s.clientOrderId[0] == '\0' ||
                 s.order.body.orderResponse.clientOrderId != resp.clientOrderId ||
                 std::strncmp(s.order.body.orderResponse.strategyId, resp.strategyId,
                              sizeof(resp.strategyId)) != 0));

        s.seq.fetch_add(1, std::memory_order_release);
        uint64_t t = now_ns();
        s.last_update_time_ns = t;    // ← 每次更新都刷, LIVE 只要还在活跃就永远不会 stale
        // 完整覆盖 order
        std::memcpy(&s.order, &rcmd, sizeof(pubsub::RCommand));
        // 维护不变量: key 副本 == 报单体 (详见 sync_key_copies 的说明)。冷路径, 不内联。
        if (need_sync) sync_key_copies(idx, rcmd);
        if (is_terminal(resp.orderStatus)) {
            if (s.finish_time_ns == 0) s.finish_time_ns = t;
            s.state.store(SLOT_FINISHED, std::memory_order_release);
        } else {
            s.state.store(SLOT_LIVE, std::memory_order_release);
        }
        s.seq.fetch_add(1, std::memory_order_release);
        header()->global_seq.fetch_add(1, std::memory_order_relaxed);
    }

    // 把 slot 的 3 个 key 副本同步成报单体里的值, 变了就把对应的索引项迁过去。
    //
    //   ★ 为什么必须同步: `find_slot` / `lookup_impl` 的**身份复核读的是 key 副本**。
    //     副本一旦与报单体分叉, 复核就退化成"比一个永不改变的值", 等于没比 ——
    //     这正是 B5 里"用 A 的 key 查回 B 的报单体"能成立的原因
    //     (当时 update_slot 只覆盖报单体, 完全不碰 orderSysId / clientOrderId 副本)。
    //   ★ 正常生命周期下这些 key 是 set-once (orderSysId / orderId 由交易所回报首次
    //     设定后再也不变), 所以这是**冷路径**: update_slot 先用一次短比较把它挡掉。
    //     实测把它塞进每次 update 会让 update 从 41 ns/op 涨到 74 ns/op
    //     (compose_client_key 里的 snprintf 是大头), 挡掉之后回到 42 ns/op 附近。
    //   ★ 报单体里字段为空 = "这次没带这个字段" → 不动副本, 不能把已建立的 key 抹掉。
    //   ★ 顺序: 先 tombstone 旧索引项, 再改副本。反过来的话 tombstone_index 拿旧 key
    //     去比 slot 的副本会比不中, 旧条目就永远留在索引里了。
    //   ★ 在 seqlock odd 窗口内被调用: 只在 key 真的变了时执行, 罕见。索引自带原子,
    //     读者若先读到新条目, 取快照时会等 seqlock 关闭, 不会看到半截数据。
    void sync_key_copies(uint32_t idx, const pubsub::RCommand& rcmd) {
        OmsSlot& s = slots()[idx];
        const auto& resp = rcmd.body.orderResponse;
        sync_one_key(idx, IDX_ORDER_SYS_ID, s.orderSysId, sizeof(s.orderSysId),
                     resp.orderSysId);
        if (resp.strategyId[0]) {
            // 复合 client key: strategyId + cid。strategyId 为空时**不重算** ——
            // 上层可能只回传了 cid, 重算会得到一个与创建时不同的 key, 反而写坏别名索引。
            char ck[64];
            const int cn = compose_client_key(
                ck, sizeof(ck),
                std::string_view(resp.strategyId,
                                 strnlen_max(resp.strategyId, sizeof(resp.strategyId))),
                resp.clientOrderId);
            if (cn > 0) {
                sync_one_key(idx, IDX_CLIENT_ORDER, s.clientOrderId,
                             sizeof(s.clientOrderId), ck);
            }
        }
        sync_one_key(idx, IDX_EXCHANGE_ID, s.orderId, sizeof(s.orderId), resp.orderId);
    }

    // 同步一个 key 副本: 变了就 tombstone 旧索引项 → 改副本 → 挂新索引项。
    void sync_one_key(uint32_t idx, IndexKind kind, char* dst, size_t dst_sz,
                      const char* new_val) {
        if (!new_val || !new_val[0]) return;                   // 这次没带这个字段
        if (std::strncmp(dst, new_val, dst_sz) == 0) return;   // 没变
        const std::string_view old_val(dst, strnlen_max(dst, dst_sz));
        const std::string_view nv(new_val, strnlen_max(new_val, dst_sz - 1));
        if (!old_val.empty()) {
            // 非空 → 非空 才叫"变更"; "" → 非空 是首次设定 (orderId 的正常路径), 静默
            std::fprintf(stderr,
                "[OmsShm][WARN] slot=%u %s key 变更: \"%.*s\" -> \"%.*s\" "
                "(报单体与 key 副本不一致, 已迁移索引)\n",
                idx, index_kind_name(kind),
                static_cast<int>(old_val.size()), old_val.data(),
                static_cast<int>(nv.size()), nv.data());
            tombstone_index(kind, old_val);
        }
        const size_t nn = nv.size();
        std::memcpy(dst, nv.data(), nn);
        dst[nn] = 0;
        insert_index(kind, nv, idx);
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
    // 返回 false 时 *failed_kind = 失败的那个索引 (供上层日志定位; 未失败则 = IDX_COUNT)
    bool insert_all_indices(uint32_t idx, IndexKind* failed_kind = nullptr) {
        if (failed_kind) *failed_kind = IDX_COUNT;
        OmsSlot& s = slots()[idx];
        if (s.orderSysId[0]) {
            if (!insert_index(IDX_ORDER_SYS_ID, s.orderSysId, idx)) {
                if (failed_kind) *failed_kind = IDX_ORDER_SYS_ID;
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
        if (cap == 0) return false;
        const uint32_t mask = cap - 1;
        const bool     pow2 = is_pow2(cap);
        uint32_t first_reusable = UINT32_MAX;   // 第一个 tombstone 或 empty
        for (uint32_t i = 0; i < kMaxProbeIndex; ++i) {
            uint32_t b = index_probe_bucket(hash, i, cap, mask, pow2);
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
                    switch (entry_key_match(e, slots(), slot_capacity(), kind, key)) {
                    case EntryKeyMatch::kMatch:
                        // 覆盖已有 (同 orderSysId 复用 slot 的场景)
                        e.slot_idx.store(slot_idx, std::memory_order_release);
                        return true;
                    case EntryKeyMatch::kStale:
                        // 条目指向的 slot 已经空了 → 条目陈旧, 这个 bucket 可复用。
                        // 记下来继续探测 (后面可能有真条目), 循环结束再决定用不用它。
                        if (first_reusable == UINT32_MAX) first_reusable = b;
                        continue;
                    case EntryKeyMatch::kOther:
                        // hash + 前缀都撞上了, 但 slot 里是**别的 key**。
                        // ★ 绝不能在这里覆盖 —— 覆盖就等于把别人的单"顶掉",
                        //   之后查那个 key 会命中这个 slot 并返回错单 (§2.3)。
                        continue;
                    }
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
    //   ★ 索引命中只是**候选**: index_probe_find 已经回读 slot 用完整 key 复核过,
    //     found_out 为 true ⟺ 那个 slot 确实持有这个 key (kOther / kStale 都不会置位)。
    //     少了这道复核, 一次 hash + 前缀碰撞就会让 upsert 把新单**并进别人的 slot**
    //     (查 A 返回 B 的报单体) —— 见 OMS_SHM_REVIEW.md §2.3。
    //   ★ 这里再补一次 slot_idx 越界检查: 返回值会被直接拿去索引 slots[], 不能脏。
    uint32_t find_slot(IndexKind kind, std::string_view key) {
        IndexEntry* arr = index(kind);
        const uint32_t cap = index_capacity();
        if (!arr || cap == 0 || key.empty()) return kInvalidSlot;
        const uint64_t hash = fnv1a(key);
        bool found = false;
        const uint32_t bucket = index_probe_find(arr, cap, hash, key,
                                                 slots(), slot_capacity(), kind, found);
        if (!found) return kInvalidSlot;
        const uint32_t slot_idx = arr[bucket].slot_idx.load(std::memory_order_acquire);
        if (slot_idx == kInvalidSlot || slot_idx >= slot_capacity()) return kInvalidSlot;
        return slot_idx;
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
        uint32_t bucket = index_probe_find(arr, cap, hash, key,
                                           slots(), slot_capacity(), kind, found);
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
