#pragma once

// ShmSpscQueue.h
// SPSC 共享内存队列：策略 -> 交易系统。单生产者、单消费者、有序、不丢。
//
// ---------------------------------------------------------------------------
// 用法
// ---------------------------------------------------------------------------
//   using shm_spsc::Error;
//   using shm_spsc::OpenMode;
//   using shm_spsc::QueueStatus;
//
//   Utrade2TbTcmdSpsc q("/utrade2tbtcmd_1001");   // 声明即打开，失败抛异常
//   q.push(cmd);                                  // 生产者
//   q.pop(cmd);                                   // 消费者
//
//   // 需要"打开失败不抛"（例如交易系统轮询 N 条、某策略还没起来）就用 open()：
//   Error err{};
//   if (!q.open(name, OpenMode::OpenOrCreate, &err)) { log(err.message); }   // 默认值
//   // ⚠️ 这里**不要**传 OpenOnly：策略没起来时会失败，而两侧互为对端 → 启动死锁。
//   //    只有在"能保证生产者已起"时才用 OpenOnly（见 docs/队列使用方式.md §1.2）。
//
//   // 消费者循环
//   unsigned step = 0;
//   for (;;) {
//       switch (q.pop(cmd)) {
//       case QueueStatus::Ok:    step = 0; handle(cmd); break;
//       case QueueStatus::Empty: q.backoff(step);       break;   // 不要裸自旋
//       case QueueStatus::Full:  /* 上游过载：记录 + 退避 */     break;
//       default:                 /* Invalid / SystemError */    break;
//       }
//   }
//
//   q.unlinkName("/utrade2tbtcmd_1001");   // 只有协调关闭时才删对象
//
// ---------------------------------------------------------------------------
// 契约（违反会静默丢消息）
// ---------------------------------------------------------------------------
//   1. **恰好一个**生产者调用 push()，**恰好一个**消费者调用 pop()。
//      两个生产者同时 push：读到同一个 head、写同一个槽位、都把 head 存成 h+1
//      —— 静默丢一条，而队列状态看起来完全正常。**本类不做任何检测**：
//      同进程可用调试工具查，跨进程的误用无法用零成本手段发现，只能靠遵守契约。
//      这也是"每策略一条队列"的前提（见 docs/队列使用方式.md）。
//   2. T 必须 trivially copyable、非多态。所有进程用相同的 T / Capacity / 布局版本。
//   3. push 满时返回 Full（不阻塞、不覆盖）；pop 空时返回 Empty。
//   4. 这是易失的 IPC 队列，不是持久化消息队列：pop 之后崩溃无法重放，
//      用 request id + OMS/交易所对账兜底。
//
// ---------------------------------------------------------------------------
// 重启语义：**绝不销毁已经存在的共享内存**
// ---------------------------------------------------------------------------
//   析构 / close() 只 munmap + close(fd)，从不 unlink。唯一的例外是"本次创建、
//   且还没初始化完成"的对象（否则名字会被半成品永久占用）。SIGKILL 不做任何清理，
//   对象留在 /dev/shm 里，下次启动作为 joiner 直接打开 —— 这正是重启后能接着用的前提。
//   只有显式 unlinkName() 才会真正删除。
//
// ---------------------------------------------------------------------------
// 为什么是 SPSC（而不是 SPMC）
// ---------------------------------------------------------------------------
//   方向单向、每条报单都必须被看到（不能跳过）、读者只有一个。
//   单写单读意味着槽位不需要序号仲裁 => 槽位就是裸 T，每槽 288B 而不是 576B，
//   push/pop 各只需一次 release store，x86-64 上没有 locked RMW。
//
// 与 ShmSpmcQueue 的差异：本类用 bool + Error* 报错（适合启动路径与轮询调用点），
// SPMC 用异常。三个文件的分工、内存序细节、性能数据见 docs/队列使用方式.md。

#include "ShmQueueCommon.h"

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <new>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>



// 重新导出共用词汇，所以 `shm_spsc::OpenMode` 这类写法照旧有效。
using shm_queue::Error;
using shm_queue::OpenMode;
using shm_queue::QueueStatus;
using shm_queue::validName;

template <typename T, std::size_t Capacity>
struct alignas(64) QueueLayout {
    static_assert(Capacity >= 2, "Capacity must be >= 2");
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");
    static_assert(shm_queue::validPayload<T>(), "T must be trivially copyable, non-polymorphic and non-empty");
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free, "uint64_t atomics must be always lock-free on this platform");

    static constexpr std::uint64_t kMagic = 0x53505343514D3031ULL; // "SPSCQM01"
    // 布局版本。任何字段增删/含义变化都必须递增，否则旧对象会被静默按新布局解释。
    static constexpr std::uint32_t kVersion = 1;
    static constexpr std::uint64_t kMask = Capacity - 1;

    // 初始化后不可变，初始化期间由 flock 保护。
    std::uint64_t magic;
    std::uint32_t version;
    std::uint32_t header_size;
    std::uint64_t total_size;
    std::uint64_t capacity;
    std::uint64_t item_size;
    std::uint64_t reserved[4];

    // 分处不同 cache line，减少 false sharing：
    // head 只被生产者写、消费者读；tail 只被消费者写、生产者读。
    // 这两条游标就是 SPSC 的全部同步状态 —— 它们**在共享内存里**，
    // 因为恰好只有一写一读（对比 SPMC：游标是每个消费者对象的私有成员）。
    alignas(64) std::atomic<std::uint64_t> head;
    alignas(64) std::atomic<std::uint64_t> tail;

    // 裸 T 数组：SPSC 不需要每槽序号来仲裁并发占位，
    // 所以省掉了 SPMC 的 version 字段，每槽开销 = sizeof(T)。
    alignas(64) T slots[Capacity];

    // 头部字段映射到统一视图，校验逻辑与 SPMC 共用（见 ShmQueueCommon.h §6）。
    shm_queue::HeaderView headerView() const noexcept {
        return {magic, version, total_size, capacity, item_size};
    }

    static constexpr shm_queue::HeaderView expectedHeader() noexcept {
        return {kMagic, kVersion, sizeof(QueueLayout), Capacity, sizeof(T)};
    }

    QueueLayout() noexcept
        : magic(kMagic),
          version(kVersion),
          header_size(static_cast<std::uint32_t>(offsetof(QueueLayout, head))),
          total_size(sizeof(QueueLayout)),
          capacity(Capacity),
          item_size(sizeof(T)),
          reserved{0, 0, 0, 0},
          head(0),
          tail(0),
          slots{} {}
};

template <typename T, std::size_t Capacity>
class ShmSpscQueue {
public:
    using Layout = QueueLayout<T, Capacity>;

    static constexpr std::size_t kCapacity = Capacity;

    ShmSpscQueue() noexcept = default;
    ~ShmSpscQueue() { close(); }

    // 一句声明就打开，失败抛 std::runtime_error（文本即 Error.message）。
    // 要"失败不抛"仍然用 open()。
    // mode 默认 OpenOrCreate；传 OpenOnly 可避免"造出一条没人写的队列"。
    explicit ShmSpscQueue(const char* name, OpenMode mode = OpenMode::OpenOrCreate, mode_t permissions = 0660) {
        Error err{};
        if (!open(name, mode, &err, permissions)) {
            throw std::runtime_error(err.message);
        }
    }

    ShmSpscQueue(const ShmSpscQueue&) = delete;
    ShmSpscQueue& operator=(const ShmSpscQueue&) = delete;

    ShmSpscQueue(ShmSpscQueue&& other) noexcept { moveFrom(other); }
    ShmSpscQueue& operator=(ShmSpscQueue&& other) noexcept {
        if (this != &other) {
            close();
            moveFrom(other);
        }
        return *this;
    }

    // 打开或创建。失败返回 false 并把原因填进 err（含 errno 描述）。
    // permissions 受 umask 影响；私有 IPC 建议 0600，除非需要专用组共享。
    //
    // 真正必要的操作只有四步：shm_open -> flock -> （创建者 ftruncate+mmap+写 header
    // / joiner fstat+mmap+校验 header）-> unlock。剩下的都是失败清理。
    bool open(const char* name, OpenMode mode, Error* err = nullptr, mode_t permissions = 0600) noexcept {
        close();

        auto fail = [&](int e, const char* msg) noexcept -> bool {
            if (err) err->set(e, msg);
            close();
            return false;
        };

        if (!shm_queue::validName(name)) {
            if (err) err->setMessage(EINVAL, shm_queue::kMsgBadName);
            return false;
        }

        bool created = false;
        int fd = -1;

        // 三种模式只在这一段有差别。
        if (mode == OpenMode::CreateOnly || mode == OpenMode::OpenOrCreate) {
            fd = ::shm_open(name, O_RDWR | O_CREAT | O_EXCL, permissions);
            if (fd >= 0) {
                created = true;
            } else if (errno == EEXIST && mode == OpenMode::OpenOrCreate) {
                fd = ::shm_open(name, O_RDWR, permissions);
            }
        } else {
            fd = ::shm_open(name, O_RDWR, permissions);
        }

        if (fd < 0) {
            return fail(errno, "shm_open failed");
        }

        // 失败清理只写一次：munmap + unlock + close；并且当对象还是"半成品"
        // （本次创建、header 还没写完）时把名字 unlink 掉，避免它永久占用这个名字
        // 却永远打不开。一旦 formed = true（header 已完整）就绝不 unlink ——
        // 这正是"挂掉重启不能销毁已创建的共享内存"的落点。
        //
        // 注意 errno 在 fail(errno, ...) 的实参里就被读走了，早于析构里的清理调用。
        struct Cleanup {
            int fd;
            bool created;
            const char* name;
            void* mapped = nullptr;
            bool locked = false;
            bool formed = false;
            bool keep = false;

            ~Cleanup() noexcept {
                if (keep) return;
                if (mapped) ::munmap(mapped, sizeof(Layout));
                if (locked) shm_queue::unlockFd(fd);
                if (fd >= 0) ::close(fd);
                if (created && !formed) ::shm_unlink(name);
            }
        } cleanup{fd, created, name};

        // 带超时的非阻塞 flock：串行化初始化与校验，也顺便让 joiner 不会看到
        // 一个半初始化的对象（阻塞式 flock 在持锁方卡住时会永久挂起）。
        if (!shm_queue::lockExclusive(fd, shm_queue::kLockTimeoutMs)) {
            return fail(errno ? errno : ETIMEDOUT,
                        "flock(LOCK_EX) timed out or failed");
        }
        cleanup.locked = true;

        if (created) {
            if (::ftruncate(fd, static_cast<off_t>(sizeof(Layout))) != 0) {
                return fail(errno, "ftruncate failed");
            }

            void* addr = shm_queue::mapShared(fd, sizeof(Layout));
            if (addr == MAP_FAILED) {
                return fail(errno, "mmap failed while creating queue");
            }
            cleanup.mapped = addr;

            // placement new 跑完默认成员初始化，header 就完整有效了（ctor 是
            // noexcept，不会抛）。从这里起失败也不再 unlink。
            Layout* layout = ::new (addr) Layout();
            cleanup.formed = true;

            // 刻意不调 msync()：初始化协议是 flock()，所有进程映射同一份 tmpfs 页，
            // 内核保证一致性；msync(MS_SYNC) 只对需要回写的后端存储有意义。
            if (!shm_queue::unlockFd(fd)) {
                return fail(errno, "flock(LOCK_UN) failed");
            }
            cleanup.locked = false;

            fd_ = fd;
            layout_ = layout;
            mapped_size_ = sizeof(Layout);
            name_ = name;
            owner_created_ = true;
            cleanup.keep = true;
            return true;
        }

        struct stat st {};
        if (::fstat(fd, &st) != 0) {
            return fail(errno, "fstat failed");
        }

        // 只做下界检查：部分平台（macOS/BSD）把 shm 对象按页对齐，st_size 会大于
        // ftruncate 请求值，精确比较会误判。真正的判别依据是下面的 header 校验；
        // 下界仍然必要 —— 对象被截断时 mmap 之后的 header 读取会越界。
        if (st.st_size < static_cast<off_t>(sizeof(Layout))) {
            return fail(EINVAL, shm_queue::kMsgStaleObject);
        }

        void* addr = shm_queue::mapShared(fd, sizeof(Layout));
        if (addr == MAP_FAILED) {
            return fail(errno, "mmap failed while opening queue");
        }
        cleanup.mapped = addr;

        Layout* layout = static_cast<Layout*>(addr);

        // 头部逐字段必须吻合，否则对象是别的 T / Capacity / 布局版本留下的。
        // header_size 是 SPSC 独有字段（head 在结构里的偏移），单独再查一次。
        if (!shm_queue::headerMatches(layout->headerView(),
                                      Layout::expectedHeader()) ||
            layout->header_size != offsetof(Layout, head)) {
            return fail(EINVAL, shm_queue::kMsgHeaderMismatch);
        }

        if (!shm_queue::unlockFd(fd)) {
            return fail(errno, "flock(LOCK_UN) failed");
        }
        cleanup.locked = false;

        fd_ = fd;
        layout_ = layout;
        mapped_size_ = sizeof(Layout);
        name_ = name;
        owner_created_ = false;
        cleanup.keep = true;
        return true;
    }

    void close() noexcept {
        if (layout_) {
            ::munmap(static_cast<void*>(layout_), mapped_size_);
            layout_ = nullptr;
        }
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
        mapped_size_ = 0;
        name_.clear();
        owner_created_ = false;
    }

    bool isOpen() const noexcept { return layout_ != nullptr; }
    const char* name() const noexcept { return name_.c_str(); }
    bool createdByThisHandle() const noexcept { return owner_created_; }

    // 槽位数。满队列可容纳 Capacity 条消息，不会少一条。
    static constexpr std::uint64_t capacity() noexcept { return Capacity; }

    // 共享内存对象占用的字节数（= sizeof(Layout)）。用于 /dev/shm 容量预算。
    static constexpr std::size_t layoutSize() noexcept { return sizeof(Layout); }

    // 每槽字节数。SPSC 的槽位是裸 T，所以就是 sizeof(T)。
    static constexpr std::size_t slotSize() noexcept { return sizeof(T); }

    // 恰好一个生产者线程/进程可以调用。
    // 永不阻塞：满时返回 Full，由调用方决定重试、丢弃还是阻塞等待。
    // 满意味着下游处理不过来，应当记录 + 退避，而不是无限重试（会掩盖上游过载）。
    //
    // 只有两次原子访问：读 tail（acquire）+ 写 head（release）。没有 CAS。
    QueueStatus push(const T& value) noexcept {
        Layout* q = layout_;
        if (!q) return QueueStatus::Invalid;

        const std::uint64_t h = q->head.load(std::memory_order_relaxed);
        const std::uint64_t t = q->tail.load(std::memory_order_acquire);

        if (h - t >= Capacity) {
            return QueueStatus::Full;
        }

        q->slots[h & Layout::kMask] = value;
        q->head.store(h + 1, std::memory_order_release);
        return QueueStatus::Ok;
    }

    // 恰好一个消费者线程/进程可以调用。
    // 永不阻塞：空时返回 Empty。空转请用 backoff()，不要裸自旋。
    QueueStatus pop(T& out) noexcept {
        Layout* q = layout_;
        if (!q) return QueueStatus::Invalid;

        const std::uint64_t t = q->tail.load(std::memory_order_relaxed);
        const std::uint64_t h = q->head.load(std::memory_order_acquire);

        if (t == h) {
            return QueueStatus::Empty;
        }

        out = q->slots[t & Layout::kMask];
        q->tail.store(t + 1, std::memory_order_release);
        return QueueStatus::Ok;
    }

    // 消费者空转退避。step 由调用方持有，读到 Empty 时调用一次。
    // 阶梯与理由见 spin::backoff()（include/SpinBackoff.h；shm_queue::backoff 是转发）。
    static void backoff(unsigned& step) noexcept { shm_queue::backoff(step); }

    // 仅供监控：push/pop 活跃时只是近似快照。
    //
    // 必须先读 tail 再读 head，并且 clamp 到 Capacity。原实现反过来读再返回
    // h - t，消费者在两次读之间清空队列时会回绕成接近 2^64 的巨值
    // （实测 536 万次里出现 5 次），让面板显示荒谬深度、fullApprox() 恒真。
    // 反过来读只会**偏大**，上界是读取窗口内被 push 的条数，所以 clamp 即可。
    std::uint64_t sizeApprox() const noexcept {
        Layout* q = layout_;
        if (!q) return 0;
        const std::uint64_t t = q->tail.load(std::memory_order_acquire);
        const std::uint64_t h = q->head.load(std::memory_order_acquire);
        const std::uint64_t n = h - t;
        return n > Capacity ? Capacity : n;
    }

    bool emptyApprox() const noexcept { return sizeApprox() == 0; }
    bool fullApprox() const noexcept { return sizeApprox() >= Capacity; }

    // 删除共享内存对象。只在"所有进程都已停止使用"的协调关闭 / 恢复流程里调用。
    // 没有 reset()：把 head/tail 归零需要保证双方都不在运行，flock 无法表达。
    // ENOENT 视为成功（幂等），与 ShmSpmcQueue 一致。
    static bool unlinkName(const char* name, Error* err = nullptr) noexcept {
        if (!shm_queue::validName(name)) {
            if (err) err->setMessage(EINVAL, shm_queue::kMsgBadName);
            return false;
        }
        if (::shm_unlink(name) != 0) {
            if (errno == ENOENT) return true;
            if (err) err->set(errno, "shm_unlink failed");
            return false;
        }
        return true;
    }

private:
    int fd_ = -1;
    Layout* layout_ = nullptr;
    std::size_t mapped_size_ = 0;
    std::string name_;
    bool owner_created_ = false;

    void moveFrom(ShmSpscQueue& other) noexcept {
        fd_ = other.fd_;
        layout_ = other.layout_;
        mapped_size_ = other.mapped_size_;
        name_ = std::move(other.name_);
        owner_created_ = other.owner_created_;

        other.fd_ = -1;
        other.layout_ = nullptr;
        other.mapped_size_ = 0;
        other.name_.clear();
        other.owner_created_ = false;
    }
};
