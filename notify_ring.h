// notify_ring_v3.h
//
// v3 相对 notify_ring.h (v2) 的改进：
//
// 1) **Per-channel 通知合并（解决重复 pop_last）**
//    同一 (vids, market_type) 在 consumer 尚未 ack 前只会占用 ring 里 **一条** 通知。
//    - Producer: 仅当 dirty bitmap 位从 0→1 时写 ring slot；位已为 1 则只更新 SPMC（由调用方完成），
//      不再 fetch_add(head)，避免 1ms 内 100 次 tick → 100 个 ring slot → 100 次 pop_last。
//    - Consumer: 处理完一条 ring 通知后 **清除** 该 channel 的 bitmap 位，下一次 notify 才能再次 0→1 入队。
//
// 2) **去掉 stall spin**：slot 未就绪直接 skip，依赖 bitmap 兜底（与 v2 相同语义）。
//
// 3) **可选阻塞等待**：ring 空时 futex_wait(wake_seq)，避免外层 pause 空转（Linux）。
//
// 语义不变：payload 仍以 SPMC last-value-wins 为准；通知是 wakeup，可合并。
//
// SHM 名建议与 v2 区分，例如 /bts_dbp_notify_v3_1

#pragma once

#include <cstdint>
#include <cstring>
#include <cerrno>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string>

#if defined(__x86_64__) || defined(_M_X64)
  #include <immintrin.h>
  #define NOTIFY_RING_V3_PAUSE() _mm_pause()
#else
  #define NOTIFY_RING_V3_PAUSE() ((void)0)
#endif

#if defined(__linux__)
  #include <linux/futex.h>
  #include <sys/syscall.h>
  #ifndef SYS_futex
    #define SYS_futex 202
  #endif
  #define NOTIFY_RING_V3_HAS_FUTEX 1
#else
  #define NOTIFY_RING_V3_HAS_FUTEX 0
#endif

namespace notify_ring_v3 {

constexpr uint32_t kMagic       = 0x4E4F5449;
constexpr uint32_t kVersion     = 3;
constexpr uint32_t kRingSlots   = 8192;
constexpr uint32_t kSlotMask    = kRingSlots - 1;

constexpr int      kMarketTypeBits = 4;
constexpr uint32_t kMarketTypeMask = (1u << kMarketTypeBits) - 1;
constexpr uint32_t kMaxVids        = 32768;
constexpr uint32_t kMaxTopicId     = kMaxVids << kMarketTypeBits;
constexpr uint32_t kBitmapWords    = kMaxTopicId / 64;

inline int32_t encode_topic_id(uint32_t vids, uint16_t market_type) noexcept {
    return static_cast<int32_t>((vids << kMarketTypeBits) | (market_type & kMarketTypeMask));
}

inline void decode_topic_id(int32_t topic_id, uint32_t& vids, uint16_t& mt) noexcept {
    vids = static_cast<uint32_t>(topic_id) >> kMarketTypeBits;
    mt   = static_cast<uint16_t>(static_cast<uint32_t>(topic_id) & kMarketTypeMask);
}

inline bool topic_id_valid(int32_t topic_id) noexcept {
    return static_cast<uint32_t>(topic_id) < kMaxTopicId;
}

// ===================== SHM POD =====================

struct alignas(64) RingHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t ring_slots;
    uint32_t producer_id;
    char     producer_name[32];
    uint8_t  _pad_static[16];

    alignas(64) uint64_t head;
    uint8_t  _pad_head[56];

    alignas(64) uint64_t tail;
    uint8_t  _pad_tail[56];

    // consumer 阻塞等待（仅 Linux futex 使用）
    alignas(64) uint32_t wake_seq;
    uint8_t  _pad_wake[60];

    alignas(64) uint64_t drop_count;
    uint64_t overrun_count;
    uint64_t overrun_events;
    uint64_t stall_count;
    uint64_t total_publish;           // 实际写入 ring 的次数（合并后）
    uint64_t coalesce_count;          // notify 被合并、未写 ring 的次数
    uint64_t bitmap_dispatch_count;
    uint64_t bitmap_scan_count;
};

struct alignas(64) RingSlot {
    uint64_t seq;
    uint32_t vids;
    uint16_t market_type;
    uint16_t producer_id;
    uint64_t event_ts_ns;
    uint8_t  _pad[32];
};

// 64 (meta) + 64 (head) + 64 (tail) + 64 (wake) + 64 (stats) = 320
static_assert(sizeof(RingHeader) == 320, "RingHeader layout mismatch");
static_assert(sizeof(RingSlot)   == 64,  "RingSlot layout mismatch");

constexpr size_t kSlotsBytes  = sizeof(RingSlot) * kRingSlots;
constexpr size_t kBitmapBytes = sizeof(uint64_t) * kBitmapWords;
constexpr size_t kShmBytes    = sizeof(RingHeader) + kSlotsBytes + kBitmapBytes;

// ===================== 内部工具 =====================

inline void bitmap_word_bit(int32_t topic_id, uint32_t& word_idx, uint64_t& bit_mask) noexcept {
    const auto tid = static_cast<uint32_t>(topic_id);
    word_idx = tid >> 6;
    bit_mask = 1ULL << (tid & 63);
}

inline void clear_topic_dirty(uint64_t* bitmap, int32_t topic_id) noexcept {
    if (!topic_id_valid(topic_id)) return;
    uint32_t w = 0;
    uint64_t m = 0;
    bitmap_word_bit(topic_id, w, m);
    __atomic_fetch_and(&bitmap[w], ~m, __ATOMIC_RELEASE);
}

inline bool test_and_set_topic_dirty(uint64_t* bitmap, int32_t topic_id) noexcept {
    // 返回 true 表示 **此前为 clean（0→1）**，应写 ring
    if (!topic_id_valid(topic_id)) return false;
    uint32_t w = 0;
    uint64_t m = 0;
    bitmap_word_bit(topic_id, w, m);
    const uint64_t old = __atomic_fetch_or(&bitmap[w], m, __ATOMIC_ACQUIRE);
    return (old & m) == 0;
}

#if NOTIFY_RING_V3_HAS_FUTEX
inline void futex_wake(uint32_t* addr, int n = 1) noexcept {
    syscall(SYS_futex, addr, FUTEX_WAKE_PRIVATE, n, nullptr, nullptr, 0);
}

inline void futex_wait(uint32_t* addr, uint32_t expected) noexcept {
    syscall(SYS_futex, addr, FUTEX_WAIT_PRIVATE, expected, nullptr, nullptr, 0);
}
#endif

// ===================== SHM 映射 =====================

class NotifyRing {
public:
    NotifyRing() = default;
    ~NotifyRing() { close_internal(); }
    NotifyRing(const NotifyRing&) = delete;
    NotifyRing& operator=(const NotifyRing&) = delete;

    bool create(const std::string& shm_name, uint32_t producer_id,
                const std::string& producer_name) {
        shm_name_ = shm_name;
        shm_unlink(shm_name_.c_str());
        fd_ = shm_open(shm_name_.c_str(), O_CREAT | O_EXCL | O_RDWR, 0660);
        if (fd_ < 0) return false;
        if (ftruncate(fd_, static_cast<off_t>(kShmBytes)) != 0) {
            ::close(fd_);
            fd_ = -1;
            shm_unlink(shm_name_.c_str());
            return false;
        }
        if (!do_mmap()) return false;
        std::memset(header_, 0, kShmBytes);
        header_->magic       = kMagic;
        header_->version     = kVersion;
        header_->ring_slots  = kRingSlots;
        header_->producer_id = producer_id;
        std::strncpy(header_->producer_name, producer_name.c_str(),
                     sizeof(header_->producer_name) - 1);
        return true;
    }

    bool attach(const std::string& shm_name) {
        shm_name_ = shm_name;
        fd_ = shm_open(shm_name_.c_str(), O_RDWR, 0);
        if (fd_ < 0) return false;
        if (!do_mmap()) return false;
        if (header_->magic != kMagic || header_->version != kVersion) return false;
        if (header_->ring_slots != kRingSlots) return false;
        return true;
    }

    void close_internal() {
        if (header_) {
            munmap(header_, kShmBytes);
            header_ = nullptr;
            slots_  = nullptr;
            bitmap_ = nullptr;
        }
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

    RingHeader* header() noexcept { return header_; }
    RingSlot*   slots()  noexcept { return slots_;  }
    uint64_t*   bitmap() noexcept { return bitmap_; }

private:
    bool do_mmap() {
        void* p = mmap(nullptr, kShmBytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
        if (p == MAP_FAILED) return false;
        header_ = static_cast<RingHeader*>(p);
        auto* base = reinterpret_cast<uint8_t*>(p);
        slots_  = reinterpret_cast<RingSlot*>(base + sizeof(RingHeader));
        bitmap_ = reinterpret_cast<uint64_t*>(base + sizeof(RingHeader) + kSlotsBytes);
        return true;
    }

    std::string shm_name_;
    int         fd_      = -1;
    RingHeader* header_  = nullptr;
    RingSlot*   slots_   = nullptr;
    uint64_t*   bitmap_  = nullptr;
};

// ===================== Producer =====================

class NotifyProducer {
public:
    bool init(const std::string& shm_name, uint32_t producer_id,
              const std::string& name) {
        if (!ring_.create(shm_name, producer_id, name)) return false;
        header_ = ring_.header();
        slots_  = ring_.slots();
        bitmap_ = ring_.bitmap();
        producer_id_ = producer_id;
        return true;
    }

    // 必须在对应 SPMC publish 之后调用。
    // 同一 channel 在 consumer ack（清 bit）之前：只写 **一次** ring，后续 notify 只计 coalesce_count。
    void notify(uint32_t vids, uint16_t market_type, uint64_t event_ts_ns) noexcept {
        const int32_t topic_id = encode_topic_id(vids, market_type);
        const bool first_dirty = test_and_set_topic_dirty(bitmap_, topic_id);

        if (!first_dirty) {
            __atomic_add_fetch(&header_->coalesce_count, 1, __ATOMIC_RELAXED);
            return;
        }

        // 0→1：该 channel 需要一次 wakeup → 尝试占 ring slot
        uint64_t my_seq = __atomic_add_fetch(&header_->head, 1, __ATOMIC_RELAXED);
        uint64_t idx    = (my_seq - 1) & kSlotMask;
        RingSlot& slot  = slots_[idx];

        const uint64_t expected_prev = my_seq - kRingSlots;
        const uint64_t cur = __atomic_load_n(&slot.seq, __ATOMIC_ACQUIRE);

        if (__builtin_expect(cur == 0 || cur == expected_prev, 1)) {
            slot.vids        = vids;
            slot.market_type = market_type;
            slot.producer_id = static_cast<uint16_t>(producer_id_);
            slot.event_ts_ns = event_ts_ns;
            __atomic_store_n(&slot.seq, my_seq, __ATOMIC_RELEASE);
            __atomic_add_fetch(&header_->total_publish, 1, __ATOMIC_RELAXED);

            const uint32_t wake = __atomic_add_fetch(&header_->wake_seq, 1, __ATOMIC_RELEASE);
#if NOTIFY_RING_V3_HAS_FUTEX
            futex_wake(&header_->wake_seq, 1);
            (void)wake;
#else
            (void)wake;
#endif
        } else {
            // ring 满：通知丢弃，但 bit 仍为 1，consumer 可通过 drain_dirty_bitmap 兜底
            __atomic_add_fetch(&header_->drop_count, 1, __ATOMIC_RELAXED);
        }
    }

    RingHeader* header() noexcept { return header_; }

private:
    NotifyRing  ring_;
    RingHeader* header_  = nullptr;
    RingSlot*   slots_   = nullptr;
    uint64_t*   bitmap_  = nullptr;
    uint32_t    producer_id_ = 0;
};

// ===================== Consumer =====================

class NotifyConsumer {
public:
    using Handler = void(*)(void* ctx, uint32_t vids, uint16_t mt, uint64_t event_ts_ns);
    using BitmapHandler = void(*)(void* ctx, int32_t topic_id);

    bool attach(const std::string& shm_name) {
        if (!ring_.attach(shm_name)) return false;
        header_ = ring_.header();
        slots_  = ring_.slots();
        bitmap_ = ring_.bitmap();
        const uint64_t cur_head = __atomic_load_n(&header_->head, __ATOMIC_ACQUIRE);
        __atomic_store_n(&header_->tail, cur_head, __ATOMIC_RELEASE);
        next_ = cur_head + 1;
        return true;
    }

    // poll 成功处理 ring 事件后，会 clear 对应 channel 的 bitmap 位（ack），
    // 使 producer 下一 burst 可再次 0→1 入队。
    //
    // 返回值同 v2：>0 处理条数；0 空；-1 overrun；-2 stall skip
    int poll(Handler handler, void* ctx, int max_batch = 64) noexcept {
        int processed = 0;

        while (processed < max_batch) {
            const uint64_t idx = (next_ - 1) & kSlotMask;
            RingSlot& slot = slots_[idx];
            const uint64_t s = __atomic_load_n(&slot.seq, __ATOMIC_ACQUIRE);

            if (__builtin_expect(s == next_, 1)) {
                handler(ctx, slot.vids, slot.market_type, slot.event_ts_ns);
                const int32_t tid = encode_topic_id(slot.vids, slot.market_type);
                clear_topic_dirty(bitmap_, tid);

                __atomic_store_n(&header_->tail, next_, __ATOMIC_RELEASE);
                ++next_;
                ++processed;
            } else if (s > next_) {
                const uint64_t cur_head = __atomic_load_n(&header_->head, __ATOMIC_ACQUIRE);
                const uint64_t lost = (cur_head >= next_) ? (cur_head - next_ + 1) : 0;
                __atomic_add_fetch(&header_->overrun_count, lost, __ATOMIC_RELAXED);
                __atomic_add_fetch(&header_->overrun_events, 1, __ATOMIC_RELAXED);
                next_ = cur_head + 1;
                __atomic_store_n(&header_->tail, cur_head, __ATOMIC_RELEASE);
                return processed > 0 ? processed : -1;
            } else {
                const uint64_t head_now = __atomic_load_n(&header_->head, __ATOMIC_ACQUIRE);
                if (head_now < next_) {
                    return processed;
                }
                // stall：不 spin，直接 skip（bitmap 仍为 1，可 drain）
                __atomic_add_fetch(&header_->stall_count, 1, __ATOMIC_RELAXED);
                ++next_;
                __atomic_store_n(&header_->tail, next_ - 1, __ATOMIC_RELEASE);
            }
        }
        return processed;
    }

    // ring 空时阻塞等待（仅 Linux）；被唤醒后再 poll 一轮。
    // timeout_ns==0 表示一直等；>0 暂未实现超时（可后续用 FUTEX_WAIT_BITSET + timespec）。
    int wait_poll(Handler handler, void* ctx, int max_batch = 64,
                  uint64_t /*timeout_ns*/ = 0) noexcept {
#if !NOTIFY_RING_V3_HAS_FUTEX
        return poll(handler, ctx, max_batch);
#else
        for (;;) {
            const int rc = poll(handler, ctx, max_batch);
            if (rc != 0) return rc;
            const uint32_t expected = __atomic_load_n(&header_->wake_seq, __ATOMIC_ACQUIRE);
            futex_wait(&header_->wake_seq, expected);
            // 被唤醒或虚假唤醒后重试 poll
        }
#endif
    }

    int drain_dirty_bitmap(BitmapHandler handler, void* ctx) noexcept {
        int processed = 0;
        __atomic_add_fetch(&header_->bitmap_scan_count, 1, __ATOMIC_RELAXED);

        for (uint32_t w = 0; w < kBitmapWords; ++w) {
            uint64_t bits = __atomic_exchange_n(&bitmap_[w], 0, __ATOMIC_ACQUIRE);
            while (bits) {
                const int bit_idx = __builtin_ctzll(bits);
                const int32_t topic_id = static_cast<int32_t>((w << 6) | bit_idx);
                handler(ctx, topic_id);
                ++processed;
                bits &= bits - 1;
            }
        }

        if (processed > 0) {
            __atomic_add_fetch(&header_->bitmap_dispatch_count,
                               static_cast<uint64_t>(processed), __ATOMIC_RELAXED);
        }
        return processed;
    }

    // 推荐主循环：先 poll ring（每条已合并），overrun/stall 再 bitmap 兜底
    int poll_and_recover(Handler ring_h, BitmapHandler bitmap_h, void* ctx,
                         int max_batch = 64) noexcept {
        const int rc = poll(ring_h, ctx, max_batch);
        if (rc < 0) {
            drain_dirty_bitmap(bitmap_h, ctx);
            return rc;
        }
        return rc;
    }

    RingHeader* header() noexcept { return header_; }

private:
    NotifyRing  ring_;
    RingHeader* header_  = nullptr;
    RingSlot*   slots_   = nullptr;
    uint64_t*   bitmap_  = nullptr;
    uint64_t    next_    = 1;
};

} // namespace notify_ring_v3
