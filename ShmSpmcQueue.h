#pragma once

#include "ShmQueueCommon.h"

#include <array>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <new>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

// ShmSpmcQueue.h
// SPMC 共享内存队列：单生产者、多消费者。一个类，两个用途 ——
//   交易系统 -> 策略（回报）：pop() 逐条读，落后要报 Gap
//   行情推送：              popLatest() 只取最新，允许跳过
//
// ---------------------------------------------------------------------------
// 用法
// ---------------------------------------------------------------------------
//   using shm_spmc::Start;        // 也可以写 <队列类型>::Start
//
//   // 回报通道：每个消费者各开一个对象（策略、监控各一个）
//   Tb2UtradeRcmd q("/tb2utrade_rcmd");                    // 默认 Start::AfterLatest
//   q.push(rcmd);                                          // 生产者（交易系统）
//   q.pop(rcmd);                                           // 消费者：逐条
//
//   // 行情通道：也用默认的 AfterLatest —— 绝不读本次打开之前的快照
//   MarketDataSpmc md("/marketdata_spmc");
//   md.push(snapshot);
//   md.popLatest(snapshot);                                // 只取最新
//
//   // 消费者循环
//   unsigned step = 0;
//   for (;;) {
//       const auto r = q.pop(rcmd);
//       if (r.status == Tb2UtradeRcmd::ReadStatus::Message) { step = 0; handle(rcmd); continue; }
//       if (r.status == Tb2UtradeRcmd::ReadStatus::Gap) {
//           // r.lost 条回报已被覆盖 —— 必须记录并去 OMS 对齐状态
//       }
//       q.backoff(step);                                   // Empty / Gap / Contended
//   }
//
//   // 只想问"这次有没有新消息"：bool tryPopLatest(out)
//
// ---------------------------------------------------------------------------
// 关键语义
// ---------------------------------------------------------------------------
//   Start（构造时给，只取一次快照，之后游标完全由 pop/popLatest 推进）
//     AfterLatest  只收打开之后发布的。**三个通道都用它（默认值）**：
//                  popLatest 只在 published_seq > last_seq_ 时返回 Message，
//                  所以拿到的每条都是"上次读之后"才发布的 ——
//                  **永远不会读到本次打开之前的旧快照，也不需要判活**。
//     AtLatest     先拿到打开时的最新一条（**可能是打开之前发布的**）。
//                  只有"要立刻显示当前状态"的工具才用，且**必须**自己判活
//                  （看 body.<member>.tsRecv，见 docs/队列使用方式.md §4）。
//     FromOldest   从环里还留着的最早一条开始。监控 / 调试用。
//
//   pop       逐条：游标 +1。要读的那条已被覆盖 -> Gap + lost。
//   popLatest 只取最新：游标跳到 published_seq，跳过的条数在 skipped。
//             它**不会**返回 Gap —— 被甩开对这条通道是设计如此，不是丢数据。
//
//   lost（数据没了）≠ skipped（数据还在，按设计不读）。
//   skipped 是**诊断量**，不是控制流：1:1 交替时它恒为 0（交付后 last_seq_ == latest，
//   下一轮 skipped = latest+1 - latest - 1 ≡ 0）；即使 > 0，该做的动作也一样。
//   要看落后程度用 backlog()。要"每一条都不漏"就换 pop()，那里才需要判 Gap。
//
//   游标在**对象**上，不在共享内存里。所以"一个消费者 = 一个对象"：
//   同一条队列的两个消费者要开两个对象（真实部署里本来就是两个进程）。
//   反之，一个对象只能被一个线程读。
//
// ---------------------------------------------------------------------------
// 契约（违反会静默出错）
// ---------------------------------------------------------------------------
//   1. 单生产者：只有一个线程/进程调用 push()，本类不做多生产者同步。
//   2. T 必须 trivially copyable、非多态。所有进程用相同的 T / Capacity / 布局版本。
//   3. 每个对象的读侧只能由一个线程使用（游标没有同步）。
//   4. push 永不阻塞，满了覆盖最旧的；消费者允许跳过（popLatest）或要求逐条（pop）。
//   5. 共享内存生命周期由调用方管理；不要在消费者仍运行时 unlink。
//
//   6. 读者**必须退避**（用 backoff()）：紧密自旋会把发布拖慢一个数量级
//      （10 个自旋读者 4.5ns -> 248ns），代价随读者数线性叠加；退避的读者几乎免费。
//   7. 重启**绝不销毁已存在的共享内存**：析构 / close() 只 munmap + close(fd)，
//      从不 unlink；唯一会 unlink 的是"本次创建、且还没初始化完成"的对象
//      （creator && !initialized 缺一不可）。SIGKILL 不做任何清理，对象留在
//      /dev/shm 里，下次启动作为 joiner 直接打开。只有 unlinkName() 会真正删除。
//
// 载荷刻意用 seq_cst 而不是 relaxed（528B 载荷 = 66 次 locked 写）：这是 seqlock，
// "载荷 relaxed + 版本号 seq_cst"不能排除撕裂读的环形执行；要排除得在写者侧和读者侧
// 各加一个 seq_cst fence，而读者侧的 fence 在 x86-64 上是 mfence，反而推高读者成本。
// 本队列在真实速率下有 1~2 个数量级余量，所以选择内存序清晰、跨进程保守。
//
// 三个文件的分工：ShmQueueCommon.h（共用词汇）/ ShmSpscQueue.h（策略 -> 交易系统）/
// 本文件（交易系统 -> 策略，以及行情）。细节见 docs/队列使用方式.md。

// 读取侧的类型定义在 ShmQueueCommon.h，这里只做转发。
// 提到命名空间而不是嵌套在类里：嵌套 enum 是按模板实例各自独立的类型，
// ShmSpmcQueue<A,64>::ReadStatus 和 <A,16>::ReadStatus 会互不相干。
// 转发之后 `shm_spmc::ReadStatus` 和 `ShmSpmcQueue<T,C>::ReadStatus` 都有效。
namespace shm_spmc {
using shm_queue::ReadResult;
using shm_queue::ReadStatus;
using shm_queue::Start;
} // namespace shm_spmc

template <typename T, std::size_t Capacity>
class ShmSpmcQueue {
    static_assert(shm_queue::validPayload<T>(),
                  "T must be trivially copyable, non-polymorphic and non-empty");
    static_assert(Capacity >= 2, "Capacity must be >= 2");

    // 槽号 = (seq - 1) % Capacity。**不是 2 的幂也能正确工作**（oldestAvailable 用的是
    // latest - Capacity + 1，也不依赖 2 的幂），所以这里曾经只断言 Capacity >= 2。
    // 但编译器**只在 2 的幂时**把 % 优化成 & (Capacity - 1)；否则发布热路径上是一条
    // 真正的 64 位除法（几十个周期），而且**不会报任何错**。SPSC 那边是硬性要求，
    // 这里对齐，免得 ShmSpmcQueue<T, 1000> 静默编译成带除法的版本。
    static_assert((Capacity & (Capacity - 1)) == 0,
                  "Capacity must be a power of two");

    static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
                  "uint64_t atomics must be always lock-free on this platform");

    // 载荷按 8 字节拆成 kWords 个原子字。
    static constexpr std::size_t kWords =
        (sizeof(T) + sizeof(std::uint64_t) - 1) /
        sizeof(std::uint64_t);

    static constexpr std::uint64_t kMagic =
        0x53504D43514D3031ULL; // "SPMCQM01"

    static constexpr std::uint32_t kAbiVersion = 1;

    // seq 的稳定版本是 seq * 2，写入中版本是 seq * 2 - 1。
    // 乘 2 不溢出，所以上限是 UINT64_MAX / 2 - 1。
    static constexpr std::uint64_t kMaxSeq = UINT64_MAX / 2 - 1;

    // 一次读取内允许的 seqlock 重试次数。有界，避免读者在生产者高速覆盖时无限循环。
    static constexpr unsigned kMaxAttempts = 16;

    // 一个槽位 = 版本号 + 载荷。版本号与载荷在**同一条** cache line 上，
    // 读者一次取到两者，不需要额外的 cache line 往返。
    struct alignas(64) Slot {
        std::atomic<std::uint64_t> version;
        std::array<std::atomic<std::uint64_t>, kWords> payload;
    };

    struct alignas(64) Layout {
        // ready == 1 才允许使用其余字段（在 flock 保护下发布）。
        std::atomic<std::uint32_t> ready;

        std::uint32_t abi_version;
        std::uint64_t magic;
        std::uint64_t layout_size;
        std::uint64_t capacity;
        std::uint64_t payload_words;

        // 全局最新已发布序号。只被生产者写、被所有读者读，
        // 是一条会被多核同时读的热 cache line。
        // 注意这是**全局**序号，不是"我读到哪了" —— 后者是每个对象的私有游标。
        alignas(64) std::atomic<std::uint64_t> published_seq;

        alignas(64) std::array<Slot, Capacity> slots;

        // 头部字段映射到统一视图，校验逻辑与 SPSC 共用（见 ShmQueueCommon.h §6）。
        shm_queue::HeaderView headerView() const noexcept {
            return {magic, abi_version, layout_size, capacity, payload_words};
        }

        static constexpr shm_queue::HeaderView expectedHeader() noexcept {
            return {kMagic, kAbiVersion, sizeof(Layout), Capacity, kWords};
        }
    };

public:
    // 类内别名，让 ShmSpmcQueue<T,C>::ReadStatus / ::OpenMode / ::Start 这些写法有效，
    // 也让两个队列的打开方式看起来完全一样。
    using OpenMode   = shm_queue::OpenMode;
    using Start      = shm_spmc::Start;
    using ReadStatus = shm_spmc::ReadStatus;
    using ReadResult = shm_spmc::ReadResult;

    // 一句声明就打开，失败抛 std::runtime_error（文本即 Error.message）。
    // 语义与 openOrCreate(name, start, open_mode, permissions) 完全一致。
    // start 决定本对象游标的初始位置（见文件顶部）；生产者不读，用默认值即可。
    // open_mode 默认 OpenOrCreate；要"只打开已存在的对象"就传 OpenOnly。
    explicit ShmSpmcQueue(const std::string& name,
                          Start start = Start::AfterLatest,
                          OpenMode open_mode = OpenMode::OpenOrCreate,
                          mode_t permissions = 0660)
        : ShmSpmcQueue(openOrCreate(name, start, open_mode, permissions)) {}

    ShmSpmcQueue(const ShmSpmcQueue&) = delete;
    ShmSpmcQueue& operator=(const ShmSpmcQueue&) = delete;

    ShmSpmcQueue(ShmSpmcQueue&& other) noexcept
        : fd_(std::exchange(other.fd_, -1)),
          layout_(std::exchange(other.layout_, nullptr)),
          last_seq_(std::exchange(other.last_seq_, 0)),
          created_by_this_handle_(
              std::exchange(other.created_by_this_handle_, false)) {}

    ShmSpmcQueue& operator=(ShmSpmcQueue&& other) noexcept {
        if (this != &other) {
            closeMapping();
            fd_ = std::exchange(other.fd_, -1);
            layout_ = std::exchange(other.layout_, nullptr);
            last_seq_ = std::exchange(other.last_seq_, 0);
            created_by_this_handle_ =
                std::exchange(other.created_by_this_handle_, false);
        }
        return *this;
    }

    ~ShmSpmcQueue() {
        closeMapping();
    }

    // 创建或打开共享内存。name 必须以 '/' 开头，例如 "/tb2utrade_rcmd"。
    // 第一次创建的进程负责初始化，其他进程在 flock 上等它做完。
    //
    // start：本对象游标的初始位置，只在**打开的那一刻**取一次快照，
    //        之后完全由 pop / popLatest 推进 —— 换起点就再打开一个对象。
    // open_mode：OpenOrCreate（默认）/ OpenOnly（不存在就抛）/ CreateOnly（已存在就抛）。
    //
    // 同名对象已存在但布局不匹配会抛；size 为 0（创建者在 ftruncate 前被 SIGKILL）
    // 会立刻抛出提示 unlinkName() 的异常，需要人工清理。
    // 失败抛 std::invalid_argument（名字非法）/ std::runtime_error（系统调用或布局不匹配）。
    static ShmSpmcQueue openOrCreate(const std::string& file,
                                     Start start = Start::AfterLatest,
                                     OpenMode open_mode = OpenMode::OpenOrCreate,
                                     mode_t permissions = 0660) {
        std::string name = file;
        if (file[0] != '/') {
            name = "/" + file;
        }

        bool creator = false;
        int fd = -1;

        // 三种模式的差别只在这一段；其余流程（flock、初始化、校验、清理）完全一样。
        if (open_mode == OpenMode::CreateOnly ||
            open_mode == OpenMode::OpenOrCreate) {
            fd = ::shm_open(name.c_str(),
                            O_RDWR | O_CREAT | O_EXCL,
                            permissions);
            if (fd >= 0) {
                creator = true;
            } else if (errno == EEXIST &&
                       open_mode == OpenMode::OpenOrCreate) {
                fd = ::shm_open(name.c_str(), O_RDWR, permissions);
            }
        } else {
            fd = ::shm_open(name.c_str(), O_RDWR, permissions);
        }

        if (fd < 0) {
            throw shm_queue::systemError("shm_open");
        }

        Layout* mapped = nullptr;
        bool locked = false;

        // 对象是否已经完成初始化、成为**有效**的共享内存。
        // 这个标志决定失败路径允不允许 unlink：只有"本次创建、且还没成形"的对象
        // 才能删。一旦初始化完成，即使收尾步骤（例如 LOCK_UN）失败也必须留在原地。
        bool initialized = false;

        try {
            // 带超时的非阻塞 flock（阻塞式 flock 没有上界）。
            // 这把锁同时承担"等创建者初始化完"的职责：creator 从 ftruncate 之前
            // 一直持锁到 ready=1 之后才放，所以拿到锁 = 创建者已离开临界区。
            if (!shm_queue::lockExclusive(fd, shm_queue::kLockTimeoutMs)) {
                const int e = errno;   // 先抓住，后面构造消息不能再依赖全局 errno
                if (e == ETIMEDOUT) {
                    throw std::runtime_error(
                        "flock(LOCK_EX) timed out after " +
                        std::to_string(shm_queue::kLockTimeoutMs) + " ms");
                }
                throw std::runtime_error(shm_queue::errnoText("flock LOCK_EX", e));
            }
            locked = true;

            struct stat st {};

            if (::fstat(fd, &st) != 0) {
                throw shm_queue::systemError("fstat");
            }

            if (creator) {
                if (::ftruncate(
                        fd,
                        static_cast<off_t>(sizeof(Layout))) != 0) {
                    throw shm_queue::systemError("ftruncate");
                }

                void* mem = shm_queue::mapShared(fd, sizeof(Layout));

                if (mem == MAP_FAILED) {
                    throw shm_queue::systemError("mmap creator");
                }

                mapped = ::new (mem) Layout;

                // 先把 ready 归零，再初始化其余字段，最后才置 ready=1。
                mapped->ready.store(
                    0, std::memory_order_seq_cst);

                mapped->published_seq.store(
                    0, std::memory_order_seq_cst);

                // 批量清零用 relaxed 就够：ready 的 seq_cst store 是发布点，
                // 任何看到 ready==1 的进程必然看到全部初始化结果。
                // 用 seq_cst 会让 Capacity=4096 的清零变成约 27 万次 locked 写。
                for (auto& slot : mapped->slots) {
                    slot.version.store(
                        0, std::memory_order_relaxed);

                    for (auto& word : slot.payload) {
                        word.store(
                            0, std::memory_order_relaxed);
                    }
                }

                mapped->abi_version = kAbiVersion;
                mapped->magic = kMagic;
                mapped->layout_size = sizeof(Layout);
                mapped->capacity = Capacity;
                mapped->payload_words = kWords;

                // 创建者自己也要检查一遍。
                requireLockFree(*mapped);

                // 最后发布初始化完成标志。
                mapped->ready.store(
                    1, std::memory_order_seq_cst);

                // 到此对象已经有效，后面任何失败都不许 unlink 它。
                initialized = true;
            } else {
                // 只做下界检查：部分平台（macOS/BSD）把 shm 对象按页对齐，
                // st_size 会大于 ftruncate 请求值。真正的判别依据是下面的 header 校验。
                if (st.st_size <
                    static_cast<off_t>(sizeof(Layout))) {
                    throw std::runtime_error(shm_queue::kMsgStaleObject);
                }

                void* mem = shm_queue::mapShared(fd, sizeof(Layout));

                if (mem == MAP_FAILED) {
                    throw shm_queue::systemError("mmap existing");
                }

                mapped = static_cast<Layout*>(mem);

                if (mapped->ready.load(
                        std::memory_order_seq_cst) != 1) {
                    throw std::runtime_error(shm_queue::kMsgNotInitialized);
                }

                // 头部必须逐字段吻合，否则对象是别的 T / Capacity / 布局版本留下的。
                if (!shm_queue::headerMatches(mapped->headerView(),
                                              Layout::expectedHeader())) {
                    throw std::runtime_error(shm_queue::kMsgHeaderMismatch);
                }

                requireLockFree(*mapped);
            }

            if (!shm_queue::unlockFd(fd)) {
                throw shm_queue::systemError("flock LOCK_UN");
            }
            locked = false;

            ShmSpmcQueue result;
            result.fd_ = fd;
            result.layout_ = mapped;
            result.last_seq_ = cursorFor(start, *mapped);
            result.created_by_this_handle_ = creator;
            return result;
        } catch (...) {
            if (mapped != nullptr) {
                ::munmap(mapped, sizeof(Layout));
            }

            if (locked) {
                shm_queue::unlockFd(fd);
            }

            if (fd >= 0) {
                ::close(fd);
            }

            // 只清理"本次创建、且还没初始化完成"的对象。两道门槛缺一不可：
            //   creator      —— 不是本次创建的，一律不删（别人可能正在用）；
            //   !initialized —— 已经初始化完成的，一律不删（它已经是有效对象）。
            // 注意进程被 SIGKILL 时不会有任何清理动作，这正是重启后能接着用的前提。
            if (creator && !initialized) {
                ::shm_unlink(name.c_str());
            }

            throw;
        }
    }

    // 删除同名共享内存对象。只在"所有进程都已停止使用"的协调关闭 / 恢复流程里调用。
    // 这是唯一的重置手段：把 published_seq 归零需要保证所有读者都已停止，
    // 而这个约束无法用 flock 表达（读者不持锁）。ENOENT 视为成功（幂等）。
    static void unlinkName(const std::string& file) {
        std::string name = file;
        if (file[0] != '/') {
            name = "/" + file;
        }
        
        if (::shm_unlink(name.c_str()) != 0 && errno != ENOENT) {
            throw shm_queue::systemError("shm_unlink");
        }
    }

    // 唯一生产者调用。成功返回递增序号（从 1 开始）；
    // 0 表示无法继续发布（未打开，或序号达到 kMaxSeq 上限）。
    //
    // 永不阻塞：容量用尽时直接覆盖最旧的槽位，由读者侧报 Gap（逐条读）
    // 或跳过（popLatest）。返回类型与 ShmSpscQueue::push 不同：
    // SPSC 是有损拒绝（Full），本队列是覆盖语义，所以返回序号更有用。
    std::uint64_t push(const T& value) noexcept {
        if (layout_ == nullptr) {
            return 0;
        }

        const std::uint64_t previous =
            layout_->published_seq.load(
                std::memory_order_seq_cst);

        // 避免 seq * 2 溢出。
        if (previous >= kMaxSeq) {
            return 0;
        }

        const std::uint64_t seq = previous + 1;
        Slot& slot = layout_->slots[(seq - 1) % Capacity];

        const std::uint64_t writing_version = seq * 2 - 1;
        const std::uint64_t stable_version = seq * 2;

        // 标记正在写入。读者看到这个版本号会重试。
        slot.version.store(
            writing_version, std::memory_order_seq_cst);

        // 原子写入载荷，避免覆盖时与消费者发生普通内存读写竞争。
        // words 先整体清零，保证 T 的尾部不足 8 字节时不会残留上一条消息的数据。
        std::array<std::uint64_t, kWords> words{};
        std::memcpy(words.data(), &value, sizeof(T));

        for (std::size_t i = 0; i < kWords; ++i) {
            slot.payload[i].store(
                words[i], std::memory_order_seq_cst);
        }

        // 槽位载荷已完整写入。
        slot.version.store(
            stable_version, std::memory_order_seq_cst);

        // 最后发布全局序号：读者看到它就意味着槽位可读。
        layout_->published_seq.store(
            seq, std::memory_order_seq_cst);

        return seq;
    }

    // 逐条读：读本对象游标的下一条，读到就前进一条。
    //
    //   Message    out / result.seq 有效。
    //   Empty      没有比游标更新的消息（含"从未发布过"）。
    //   Gap        游标那条已被覆盖。游标前移到"环里还存活的最早一条"，
    //              result.lost 给出丢掉多少条。**必须**记录并去 OMS 对齐状态。
    //   Contended  kMaxAttempts 次都没取得一致快照（生产者正在高频覆盖），
    //              调用方应退避后重试，不要原地死循环。
    //
    // 建议循环：Message -> 立即再调一次；Gap -> 记录 lost + 对齐；Empty / Contended -> backoff。
    ReadResult pop(T& out) noexcept {
        return readAtCursor(out, 0);
    }

    // 只取最新：把游标直接推到 published_seq，再走一次逐条读。
    //
    //   Message    result.seq 就是当前最新，result.skipped 给出跳过的条数。
    //   Empty      没有比游标更新的消息（含"从未发布过"）。
    //   Contended  kMaxAttempts 次都没取得一致快照，调用方应退避后重试。
    //
    // **不会**返回 Gap。从"瞄一眼 published_seq"到"真正读槽位"之间生产者可能
    // 又发了一整圈，但那是"只取最新"的固有情形，不是数据丢失，所以这里重新瞄一次。
    //
    // out 只有在 status == Message 时才会被更新。
    // Start 仍然有效：Start::AfterLatest 打开的对象，在生产者发出新消息之前
    // popLatest() 返回 Empty（游标已经是当前最新）。要"一打开就先拿快照"用 AtLatest。
    ReadResult popLatest(T& out) noexcept {
        if (layout_ == nullptr) {
            return {ReadStatus::Empty};
        }

        // 本次调用开始时的游标。只有真正交付了消息才允许前进，
        // 否则重试时 skipped 会算不准、nextSequence() 也会虚报。
        const std::uint64_t from = last_seq_;

        for (unsigned attempt = 0;
             attempt < kMaxAttempts;
             ++attempt) {

            const std::uint64_t latest = publishedSequence();

            if (latest == 0 || latest <= from) {
                return {ReadStatus::Empty};
            }

            // 把游标停在 latest 的前一条，readAtCursor 就正好读到 latest。
            last_seq_ = latest - 1;

            // 首次读取（from == 0）不算跳过：订阅者只是"来晚了"。
            // 否则新订阅者一上来就会报一个巨大的 skipped。
            const std::uint64_t skipped =
                (from == 0) ? 0 : (latest - from - 1);

            const ReadResult result = readAtCursor(out, skipped);

            if (result.status == ReadStatus::Message) {
                return result;
            }

            // 没读到就还原游标：游标只在交付消息时前进。
            last_seq_ = from;

            if (result.status != ReadStatus::Gap) {
                return result;      // Contended
            }

            // Gap：被生产者甩开了一整圈，重新瞄一次最新。
        }

        return {ReadStatus::Contended};
    }

    // 便捷形式：只关心"这次有没有拿到新消息"。
    // false 同时覆盖 Empty 与 Contended；需要区分时用 popLatest()。
    bool tryPopLatest(T& out) noexcept {
        return popLatest(out).status == ReadStatus::Message;
    }

    // 本对象已读到的最后一条序号；0 = 还没读到任何一条。
    // 这是**本对象**的游标，不是队列的全局进度（那是 publishedSequence()）。
    std::uint64_t lastSequence() const noexcept { return last_seq_; }

    // 本对象下一条要读的序号。逐条读时等于 lastSequence() + 1。
    std::uint64_t nextSequence() const noexcept { return last_seq_ + 1; }

    // 本对象还剩多少条没读（含已被覆盖的部分）。仅用于监控 / 背压观测。
    std::uint64_t backlog() const noexcept {
        if (layout_ == nullptr) return 0;
        const std::uint64_t latest = publishedSequence();
        return latest > last_seq_ ? latest - last_seq_ : 0;
    }

    // 队列全局最新已发布序号（所有消费者看到的是同一个值）。0 = 还没发布过。
    std::uint64_t publishedSequence() const noexcept {
        if (layout_ == nullptr) {
            return 0;
        }

        return layout_->published_seq.load(
            std::memory_order_seq_cst);
    }

    // 消费者空转退避。step 由调用方持有，读到 Empty / Contended 时调用一次。
    // 阶梯与理由见 spin::backoff()（include/SpinBackoff.h；shm_queue::backoff 是转发）。
    static void backoff(unsigned& step) noexcept { shm_queue::backoff(step); }

    static constexpr std::size_t capacity() noexcept { return Capacity; }

    // 共享内存对象占用的字节数（= sizeof(Layout)）。用于 /dev/shm 容量预算。
    static constexpr std::size_t layoutSize() noexcept { return sizeof(Layout); }

    // 每槽字节数（含版本号与对齐填充）。
    static constexpr std::size_t slotSize() noexcept { return sizeof(Slot); }

    // 当前是否已映射到共享内存。句柄被 move 之后，被 move 的那一方返回 false。
    bool isOpen() const noexcept { return layout_ != nullptr; }

    // 本次打开是"创建了对象"还是"接入已有对象"。默认 OpenOrCreate 下两边都可能创建，
    // 所以这个信息要打日志：**消费端打出 true 通常说明生产者还没起、或名字写错了**
    // （队列被静默建出来，表现为"没数据、也不报错"）。语义与 SpscQueue 的同名方法一致。
    bool createdByThisHandle() const noexcept {
        return created_by_this_handle_;
    }

    // 主动解除映射。析构时会自动调用；显式调用是为了支持
    // "对端重启后重新 openOrCreate" 这种运维动作。
    // 调用后本对象回到未打开状态（游标一并归零），可以再次 openOrCreate()。
    void close() noexcept { closeMapping(); }

    // 默认构造出的是"未打开"状态（layout_ == nullptr）。
    // openOrCreate() 是**工厂**；"未打开"本身是合法状态，所有入口都对
    // layout_ == nullptr 做了检查（push 返回 0、pop / popLatest 返回 Empty）。
    ShmSpmcQueue() = default;

private:
    // 把 Start 翻译成"游标停在序号几"。三种起点的落点见文件顶部。
    static std::uint64_t cursorFor(Start start,
                                   const Layout& layout) noexcept {
        const std::uint64_t latest =
            layout.published_seq.load(std::memory_order_seq_cst);

        switch (start) {
        case Start::AfterLatest:
            // 游标停在 latest，下一条要读的就是 latest + 1。
            return latest;

        case Start::AtLatest:
            // 游标停在 latest - 1，第一次读正好读到 latest。
            // latest == 0（还什么都没发过）时退回 0，等价于"读第一条"。
            return latest == 0 ? 0 : latest - 1;

        case Start::FromOldest:
            // 游标停在"环里还活着的最早一条"的前一条。
            return oldestAvailable(latest) - 1;
        }

        return latest;
    }

    // 在 latest 已发布的前提下，仍然"可能读到完整内容"的最早序号。
    //
    // 注意这里是 latest - Capacity + 1，比"确定安全"的 latest - Capacity + 2 早一位。
    // **不是 off-by-one**：序号 latest - Capacity + 1 占用的槽位正是生产者**正在**
    // 写入（序号 latest + 1）的那一个，在生产者写下 writing_version 之前内容仍然完整，
    // 此时读到是合法的；一旦生产者开始写，版本校验会失败并重试，
    // 而等到生产者发布了 latest + 1，oldest 自然前移一位并报 Gap。
    // 这个窗口由版本号仲裁，不需要在这里保守地减 2。
    static std::uint64_t oldestAvailable(std::uint64_t latest) noexcept {
        if (latest == 0) {
            return 1;
        }

        if (latest >= Capacity) {
            return latest - Capacity + 1;
        }

        return 1;
    }

    // pop / popLatest 的共同实现，是本类唯一读槽位的地方。
    // 进入时 last_seq_ 已经定好"要读 last_seq_ + 1"；
    // skipped 由调用方给（pop 恒为 0），只随成功的 Message 报出。
    ReadResult readAtCursor(T& out, std::uint64_t skipped) noexcept {
        if (layout_ == nullptr) {
            return {ReadStatus::Empty};
        }

        for (unsigned attempt = 0;
             attempt < kMaxAttempts;
             ++attempt) {

            const std::uint64_t latest = publishedSequence();

            if (last_seq_ >= latest) {
                return {ReadStatus::Empty};
            }

            const std::uint64_t seq = last_seq_ + 1;
            const std::uint64_t oldest = oldestAvailable(latest);

            if (seq < oldest) {
                // 游标那条已经被生产者覆盖掉了。
                const std::uint64_t lost = oldest - seq;
                last_seq_ = oldest - 1;
                return {ReadStatus::Gap, 0, lost, 0};
            }

            const Slot& slot = layout_->slots[(seq - 1) % Capacity];
            const std::uint64_t expected = seq * 2;

            const std::uint64_t v1 =
                slot.version.load(std::memory_order_seq_cst);

            if (v1 != expected) {
                // 槽位可能正在被覆盖。下一轮重新检查 published_seq 和最早可读序号。
                continue;
            }

            std::array<std::uint64_t, kWords> words{};

            for (std::size_t i = 0; i < kWords; ++i) {
                words[i] = slot.payload[i].load(
                    std::memory_order_seq_cst);
            }

            const std::uint64_t v2 =
                slot.version.load(std::memory_order_seq_cst);

            if (v1 != v2 || v2 != expected) {
                // 读取期间发生覆盖，不能接受本次载荷。
                continue;
            }

            // 只有通过版本校验后才复制到调用方。
            std::memcpy(&out, words.data(), sizeof(T));

            last_seq_ = seq;
            return {ReadStatus::Message, seq, 0, skipped};
        }

        // kMaxAttempts 次都没成功。最后再确认一次是不是"确定有缺口"，
        // 免得把"确实丢了一批"误报成"竞争失败"。
        const std::uint64_t latest = publishedSequence();
        const std::uint64_t seq = last_seq_ + 1;

        if (seq <= latest) {
            const std::uint64_t oldest = oldestAvailable(latest);

            if (seq < oldest) {
                const std::uint64_t lost = oldest - seq;
                last_seq_ = oldest - 1;
                return {ReadStatus::Gap, 0, lost, 0};
            }
        }

        return {ReadStatus::Contended};
    }

    // 排除"用内部锁实现的 atomic"（那样会退化成进程内锁，跨进程无效）。
    static void requireLockFree(const Layout& layout) {
        const bool lock_free =
            layout.published_seq.is_lock_free() &&
            layout.slots[0].version.is_lock_free() &&
            layout.slots[0].payload[0].is_lock_free();

        if (!lock_free) {
            throw std::runtime_error(
                "Required atomics are not lock-free");
        }
    }

    void closeMapping() noexcept {
        if (layout_ != nullptr) {
            ::munmap(layout_, sizeof(Layout));
            layout_ = nullptr;
        }

        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }

        last_seq_ = 0;
        created_by_this_handle_ = false;
    }

    int fd_ = -1;
    Layout* layout_ = nullptr;

    // 本对象的读游标（**不共享**）。0 = 还没读到任何一条，所以下一条是 1。
    // 必须在对象里而不是 Layout 里：SPMC 允许多个消费者各自独立推进，
    // 放进共享内存就等于让所有消费者共用一条游标。见文件顶部。
    std::uint64_t last_seq_ = 0;

    // 这个句柄是"创建者"还是"接入者"。默认 OpenOrCreate 下**消费者也可能创建**
    // （生产者还没起来时），所以启动日志里打一行很有价值：消费端出现 created=true
    // 通常意味着生产者没起、或名字写错了。与 SpscQueue::createdByThisHandle() 同名同义。
    bool created_by_this_handle_ = false;
};
