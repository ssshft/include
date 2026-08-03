// InstrumentInfoShm.h
//
// 币对基础信息的共享内存布局 (方案 A: 定长数组 + seqlock)。
//
// 场景:
//   - **writer**: contractinfo 进程, 每 60s 从交易所 REST 拉一次, 全量原子交换到 SHM
//   - **reader**: db / tb / utrade 等所有下游进程, 通过 SecurityManager 读, 只读不写
//
// 关键设计:
//   1. Fixed-size layout: Header + entries[capacity]。capacity 在 SHM 创建时写死,
//      readers 直接读 Header.capacity 拿到当前段大小。新交易所上线导致条目上涨 →
//      修改 config 的 instrumentShmCapacity 后重启 contractinfo, 段会被 unlink+recreate,
//      readers 检测到 magic/generation 不连续会自动 reopen。
//
//   2. Seqlock 并发协议 (single-writer / multi-reader, 无锁):
//        写方: generation.fetch_add(1)   [偶→奇, 表示写中]
//              memcpy entries + 更新 count/last_update_us
//              generation.fetch_add(1)   [奇→偶, 表示完成]
//        读方: do { g1 = load(); if (g1 & 1) retry;
//                   copy data;
//                   g2 = load(); if (g1 == g2) break; else retry; } while (true);
//      这样 reader 保证拿到"一次完整的原子快照", 不会读到半写状态。
//
//   3. 60s 一次的全量 swap 已经能覆盖"新币对上架"、"币对下架"、"参数变更" 全部情况,
//      不做增量维护 (增量维护要处理条目删除 / 补空位 / 索引迁移, 复杂度暴增而没有实际收益)。
//
//   4. SHM 段生命期:
//      writer 首次调 open() 时 shm_open(O_CREAT). 若已存在且 capacity 一致 → 复用。
//      若 capacity 不一致 → shm_unlink + 重新 O_CREAT (老 readers 应立即察觉并重新 open)。
//      正常情况下段常驻内存, 不 unlink; 手工 rm /dev/shm/<name> 才彻底释放。
//
// 只在 Linux POSIX 环境使用 (shm_open + mmap), 不依赖 boost::interprocess。

#pragma once

#include "data_struct.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>


namespace sm::shm {

// ============================================================================
// 常量
// ============================================================================
constexpr uint32_t    kInfoShmMagic     = 0x494E5354;   // 'INST' little-endian
constexpr uint32_t    kInfoShmVersion   = 1;
constexpr const char* kDefaultShmName   = "/bts_instrument_info";
constexpr uint32_t    kDefaultCapacity  = 16384;


// ============================================================================
// 布局: 8 字节对齐, Header + capacity 个 InstrumentInfo
// ============================================================================
#pragma pack(push, 8)
struct Header {
    std::atomic<uint32_t> magic;             // 校验 = kInfoShmMagic
    std::atomic<uint32_t> version;           // schema 版本 = kInfoShmVersion
    std::atomic<uint64_t> generation;        // seqlock: 奇=写中, 偶=稳定
    std::atomic<uint64_t> last_update_us;    // 上次写完的时间戳 (可选, 用于监控)
    std::atomic<uint32_t> count;             // 当前有效条目数 (≤ capacity)
    uint32_t              capacity;          // 段能容纳的最大条目数 (创建时固定)
    uint32_t              entry_size;        // sizeof(md::InstrumentInfo), 兼容性校验
    uint32_t              _pad;
};
#pragma pack(pop)
static_assert(sizeof(Header) == 40, "Header size unexpected");

inline size_t total_bytes(uint32_t capacity) noexcept {
    return sizeof(Header) + static_cast<size_t>(capacity) * sizeof(md::InstrumentInfo);
}


// ============================================================================
// Writer (contractinfo 端)
// ============================================================================
class Writer {
public:
    Writer() = default;
    ~Writer() { close(); }

    Writer(const Writer&) = delete;
    Writer& operator=(const Writer&) = delete;

    // 打开或创建 SHM 段。
    //   name     : 例如 "/bts_instrument_info" (必须以 / 开头, POSIX shm_open 规则)
    //   capacity : 段能容纳的最大条目数, 首次创建时写入 Header, 后续以此为准
    //
    // 已存在段的处理:
    //   - magic/version 校验失败 → unlink + recreate
    //   - capacity 不匹配 → unlink + recreate (老 readers 需要重连)
    //   - 都一致 → 复用, 只重写 count/entries
    bool open(const std::string& name, uint32_t capacity) {
        name_ = name;
        wanted_capacity_ = capacity;

        // 尝试 open existing
        int fd = shm_open(name.c_str(), O_RDWR, 0666);
        if (fd >= 0) {
            struct stat st{};
            if (fstat(fd, &st) == 0) {
                // 校验大小 = Header + capacity * entry_size
                if (static_cast<size_t>(st.st_size) == total_bytes(capacity)) {
                    // 先 mmap Header 探测 magic/version/capacity
                    void* peek = mmap(nullptr, sizeof(Header), PROT_READ, MAP_SHARED, fd, 0);
                    if (peek != MAP_FAILED) {
                        auto* h = reinterpret_cast<Header*>(peek);
                        // acquire-load magic 建立 happens-before, 后续字段读取安全
                        bool ok = h->magic.load(std::memory_order_acquire) == kInfoShmMagic
                               && h->version.load(std::memory_order_relaxed) == kInfoShmVersion
                               && h->capacity       == capacity
                               && h->entry_size     == sizeof(md::InstrumentInfo);
                        munmap(peek, sizeof(Header));
                        if (ok) {
                            // 复用
                            if (!mmap_all(fd, capacity, /*create=*/false)) {
                                return false;
                            }
                            // Crash-recovery: 前任 Writer 若崩溃在两次 fetch_add 之间,
                            // gen 会停在奇数 (in-progress 状态)。 我们接管后必须先把
                            // gen bump 到下一个偶数, 恢复 "stable" 起点, 否则接下来的
                            // publish 里两次 fetch_add 会把 gen 变成 even→odd (invariant 反转),
                            // reader 要么看到脏数据要么永远等待。
                            uint64_t g = header_->generation.load(std::memory_order_acquire);
                            if (g & 1) {
                                header_->generation.fetch_add(1, std::memory_order_release);
                                LOG_WARN("[shm] recovered from previous writer crash "
                                         "(generation was odd, bumped to even).");
                            }
                            return true;
                        }
                    }
                }
            }
            ::close(fd);
            // 不匹配 → unlink + recreate
            shm_unlink(name.c_str());
        }

        // 创建新段
        fd = shm_open(name.c_str(), O_RDWR | O_CREAT | O_EXCL, 0666);
        if (fd < 0) {
            return false;
        }
        if (ftruncate(fd, total_bytes(capacity)) != 0) {
            int e = errno; ::close(fd); shm_unlink(name.c_str()); errno = e;
            return false;
        }
        return mmap_all(fd, capacity, /*create=*/true);
    }

    // 全量原子交换: 覆盖所有条目 + bump generation。
    // 契约: 只允许在同一个线程 (contractinfo 的 update 线程) 调用。
    bool publish(const std::vector<md::InstrumentInfo>& infos) {
        if (!header_) return false;

        uint32_t n = static_cast<uint32_t>(infos.size());
        if (n > header_->capacity) {
            // 溢出 → 只写前 capacity 条, 记录一次警告 (contractinfo 侧会打 log)
            n = header_->capacity;
        }

        // 1) generation 偶→奇 (标记写中)
        header_->generation.fetch_add(1, std::memory_order_release);

        // 2) 拷贝数据 + 更新 count
        // 注意先写 count 还是先写数据? 我们两次 generation bump 已经保证 seqlock
        // 语义, 顺序不重要, 只要都在两次 fetch_add 之间即可。
        std::memcpy(entries_, infos.data(), static_cast<size_t>(n) * sizeof(md::InstrumentInfo));
        header_->count.store(n, std::memory_order_relaxed);
        header_->last_update_us.store(now_us(), std::memory_order_relaxed);

        // 3) generation 奇→偶 (标记完成)
        header_->generation.fetch_add(1, std::memory_order_release);
        return true;
    }

    void close() noexcept {
        if (map_addr_) {
            munmap(map_addr_, map_size_);
            map_addr_ = nullptr;
            map_size_ = 0;
            header_ = nullptr;
            entries_ = nullptr;
        }
    }

    uint32_t capacity() const noexcept { return header_ ? header_->capacity : 0; }

private:
    bool mmap_all(int fd, uint32_t capacity, bool create) {
        map_size_ = total_bytes(capacity);
        map_addr_ = mmap(nullptr, map_size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        ::close(fd);
        if (map_addr_ == MAP_FAILED) {
            map_addr_ = nullptr;
            map_size_ = 0;
            return false;
        }
        header_ = reinterpret_cast<Header*>(map_addr_);
        entries_ = reinterpret_cast<md::InstrumentInfo*>(reinterpret_cast<uint8_t*>(map_addr_) + sizeof(Header));
        if (create) {
            // 初始化 Header, ⚠️ 顺序关键:
            //   非 atomic 字段 (capacity/entry_size/_pad) 和 atomic zero 值先写
            //   version 用 release 存
            //   magic 最后用 release 存 → 对 reader 的 acquire-load magic 建立 happens-before
            // 这样 reader 只要看到 magic == kInfoShmMagic, 前面所有写都保证可见。
            // 否则 reader 可能读到 magic OK 但 capacity=0 的中间态, 导致后续越界读。
            header_->capacity   = capacity;
            header_->entry_size = sizeof(md::InstrumentInfo);
            header_->_pad       = 0;
            header_->generation.store(0, std::memory_order_relaxed);
            header_->last_update_us.store(0, std::memory_order_relaxed);
            header_->count.store(0, std::memory_order_relaxed);
            header_->version.store(kInfoShmVersion, std::memory_order_release);
            // magic 必须最后写, 且 release, 作为整个 Header 初始化完成的信号量
            header_->magic.store(kInfoShmMagic, std::memory_order_release);
        }
        return true;
    }

    static uint64_t now_us() noexcept {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
    }

    void*                  map_addr_ = nullptr;
    size_t                 map_size_ = 0;
    Header*                header_   = nullptr;
    md::InstrumentInfo*    entries_  = nullptr;
    std::string            name_;
    uint32_t               wanted_capacity_ = 0;
};


// ============================================================================
// Reader (SecurityManager / db / tb 端, 只读)
// ============================================================================
class Reader {
public:
    Reader() = default;
    ~Reader() { close(); }

    Reader(const Reader&) = delete;
    Reader& operator=(const Reader&) = delete;

    // 打开已存在的 SHM 段。
    //   name  : 与 Writer 同名
    //   wait_for_writer_ms: 若 SHM 尚未创建, 每 100ms 重试, 最多等这么久 (0=不等)
    //
    // 返回 false 表示段不存在 / 校验失败, 上层可回退到 Redis 或退出。
    bool open(const std::string& name, int wait_for_writer_ms = 5000) {
        name_ = name;
        auto deadline = std::chrono::steady_clock::now()
                      + std::chrono::milliseconds(wait_for_writer_ms);

        for (;;) {
            int fd = shm_open(name.c_str(), O_RDONLY, 0666);
            if (fd >= 0) {
                struct stat st{};
                if (fstat(fd, &st) == 0 && st.st_size >= static_cast<off_t>(sizeof(Header))) {
                    map_size_ = static_cast<size_t>(st.st_size);
                    map_addr_ = mmap(nullptr, map_size_, PROT_READ, MAP_SHARED, fd, 0);
                    ::close(fd);
                    if (map_addr_ != MAP_FAILED) {
                        header_ = reinterpret_cast<const Header*>(map_addr_);
                        // 校验 header:
                        //   先 acquire-load magic, 通过则 Writer 已完成整个 Header 初始化,
                        //   之后读 version/entry_size/capacity 都保证一致 (Writer 用 release 写 magic 建立 happens-before)。
                        if (header_->magic.load(std::memory_order_acquire) == kInfoShmMagic &&
                            header_->version.load(std::memory_order_relaxed) == kInfoShmVersion &&
                            header_->entry_size     == sizeof(md::InstrumentInfo) &&
                            map_size_ >= total_bytes(header_->capacity))
                        {
                            entries_ = reinterpret_cast<const md::InstrumentInfo*>(
                                reinterpret_cast<const uint8_t*>(map_addr_) + sizeof(Header));
                            return true;
                        }
                        munmap(map_addr_, map_size_);
                        map_addr_ = nullptr;
                        map_size_ = 0;
                        header_ = nullptr;
                    }
                } else {
                    ::close(fd);
                }
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    // 拿一份全量快照 (seqlock 协议保证一致性, 不会读到半写)。
    // 内部循环直到读到一致的 generation, 一般 1-2 轮内完成。
    bool snapshot(std::vector<md::InstrumentInfo>& out) const {
        if (!header_ || !entries_) return false;

        for (int retry = 0; retry < 16; ++retry) {
            uint64_t g1 = header_->generation.load(std::memory_order_acquire);
            if (g1 & 1) {                    // 写中, 稍等再试
                std::this_thread::yield();
                continue;
            }
            uint32_t n = header_->count.load(std::memory_order_relaxed);
            if (n > header_->capacity) n = header_->capacity;

            out.resize(n);
            std::memcpy(out.data(), entries_, static_cast<size_t>(n) * sizeof(md::InstrumentInfo));

            uint64_t g2 = header_->generation.load(std::memory_order_acquire);
            if (g1 == g2) return true;       // 读到一次完整快照
            // 否则 writer 中间刷新了, 再读一次
        }
        out.clear();
        return false;   // 极端情况 (writer 疯狂刷) 才会到这
    }

    uint64_t generation() const noexcept {
        return header_ ? header_->generation.load(std::memory_order_acquire) : 0;
    }

    uint64_t last_update_us() const noexcept {
        return header_ ? header_->last_update_us.load(std::memory_order_relaxed) : 0;
    }

    uint32_t count() const noexcept {
        return header_ ? header_->count.load(std::memory_order_relaxed) : 0;
    }

    uint32_t capacity() const noexcept {
        return header_ ? header_->capacity : 0;
    }

    void close() noexcept {
        if (map_addr_) {
            munmap(map_addr_, map_size_);
            map_addr_ = nullptr;
            map_size_ = 0;
            header_ = nullptr;
            entries_ = nullptr;
        }
    }

private:
    void*                            map_addr_ = nullptr;
    size_t                           map_size_ = 0;
    const Header*                    header_   = nullptr;
    const md::InstrumentInfo*        entries_  = nullptr;
    std::string                      name_;
};

} // namespace sm::shm

