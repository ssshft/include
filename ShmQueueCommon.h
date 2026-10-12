#pragma once

// ShmQueueCommon.h
// ShmSpscQueue / ShmSpmcQueue 共用的词汇与系统调用封装。
// 本文件不依赖 T / Capacity，也不含任何布局定义。
//
// ---------------------------------------------------------------------------
// 内容
// ---------------------------------------------------------------------------
//   OpenMode         打开方式：CreateOnly / OpenOnly / OpenOrCreate
//   QueueStatus      SPSC 的 push/pop 结果
//   Error            非异常路径的错误载体（errno + 定长文本）
//   Start            SPMC 消费者对象的起点
//   ReadStatus       SPMC 的读结果：Message / Empty / Gap / Contended
//   ReadResult       ReadStatus + seq / lost / skipped
//   validName()      POSIX shm 名字规则
//   lockExclusive()  带超时的非阻塞 flock(LOCK_EX)
//   unlockFd()       flock(LOCK_UN)，容忍 EINTR
//   mapShared()      mmap + MAP_POPULATE + MADV_HUGEPAGE
//   cpu_relax()      自旋让步原语 —— **已抽到 include/SpinBackoff.h**（这里只转发）
//   backoff()        读者空转时的 pause / yield 阶梯（不含 sleep）—— 同上
//   backoff_sleep()  阶梯 + 低频睡眠 —— 同上
//   validPayload<T>()  T 的约束：trivially copyable、非多态、非空
//   HeaderView       头部校验的统一视图 + headerMatches()
//
// ---------------------------------------------------------------------------
// 用法（两个队列一样）
// ---------------------------------------------------------------------------
//   Error err{};
//   if (!q.open("/name", OpenMode::OpenOrCreate, &err)) {   // 默认值：两边都用它
//       log(err.code, err.message);
//   }
//   // OpenOnly：对象不存在就失败、绝不自己创建 —— 只在"能保证生产者已起"时用。
//   // ⚠️ 本项目两个进程**互为对端**（tb 消费 tcmd / 生产 rcmd，策略反过来），
//   //    两边消费端都用 OpenOnly 会互相等待 = 启动死锁。见 docs/队列使用方式.md §1.2。
//
//   // 异常路径：ShmSpmcQueue 的构造函数 / openOrCreate 直接抛，
//   // 异常文本就是这里的 badNameMessage() / systemError()。
//
// ---------------------------------------------------------------------------
// 共享内存通信靠什么（两个队列都一样）
// ---------------------------------------------------------------------------
//   1. **mmap 同一份物理页** —— 通信本体。两个进程把同一个 shm 对象映射进各自
//      地址空间，拿到指向同一块物理内存的两个虚拟地址。这是唯一真正"共享"的东西。
//   2. **原子操作 + acquire/release 内存序** —— 同步本体。共享内存只提供"同一块
//      字节"，不提供任何顺序保证。"生产者写完"对"消费者可见"，靠的是 release store
//      与 acquire load 配对构成的 happens-before；普通读写在跨进程下没有任何保证。
//   3. **flock** —— 只在初始化期用，串行化"创建者写 header"与"joiner 读 header"，
//      不参与热路径。
//
//   ⚠️ 跨进程**没有** std::mutex（它只在单进程内有效）。所以要么无锁（本仓库的选择，
//   全靠原子操作），要么用 PTHREAD_PROCESS_SHARED 的进程间锁。也因此
//   std::atomic<uint64_t> **必须 lock-free** —— 否则它内部会退化成进程内锁，跨进程
//   完全无效。这就是 requireLockFree() / is_always_lock_free 存在的唯一理由。
//
// ---------------------------------------------------------------------------
// SPSC 与 SPMC 的关键区别：**读者有几个，决定游标在哪里**
// ---------------------------------------------------------------------------
//   SPSC（ShmSpscQueue）  恰好一写一读 => 游标全局只有一份，直接放进共享内存
//                         （QueueLayout::head / tail）。槽位不需要序号仲裁 => 裸 T。
//                         满了拒绝（Full），一条都不丢；push / pop 各只有一次
//                         release store，x86-64 上 0 个 locked RMW。
//
//   SPMC（ShmSpmcQueue）  一写多读，每个消费者要**独立**推进自己的进度 => 游标不能
//                         放共享内存（否则消费者互相推进），必须是**对象**的私有成员。
//                         槽位要能判断"我要读的这条是否已被覆盖" => 每个槽位带一个
//                         seqlock 版本号。满了覆盖最旧的；落后报 Gap（逐条读）
//                         或 skipped（只取最新）。
//
//   一句话：SPSC 的同步状态是**共享的一份**（所以无损、不需要序号仲裁）；
//   SPMC 的同步状态是**每消费者一份**（所以能多消费者，代价是槽位更大 + 可丢）。
//
// ---------------------------------------------------------------------------
// 有意**不共用**的东西
// ---------------------------------------------------------------------------
//   QueueLayout / Slot   两个队列的槽位表示必须不同（裸 T vs seqlock），
//                        合并只有"给 SPSC 白加版本号"或"破坏 SPMC 的 seqlock"
//                        两条路。共用的只有头部字段的**校验规则**（HeaderView）。
//   读侧策略             SPSC 的 head/tail 在共享内存里；SPMC 每个对象一份私有游标。
//   kMagic               一个队列一个常量，防止把回报队列当行情队列打开。
//
// 详见 docs/两个SPMC队列的差异与合并可行性.md。

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <unistd.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include "SpinBackoff.h"   // spin::cpu_relax / backoff / backoff_sleep（§5.1 转发）

namespace shm_queue {

// ===========================================================================
// §1 打开方式与状态码
// ===========================================================================

enum class OpenMode {
    CreateOnly,   // 对象已存在就失败（EEXIST）
    OpenOnly,     // 对象不存在就失败 —— 只在"能保证生产者已起"时用；**别当默认**
                  // （本项目两侧互为对端，两边消费端都用它 = 启动死锁）
    OpenOrCreate  // 不存在才创建，已存在就打开（默认；重启后能直接接上）
};

// 无损队列（SPSC）的 push/pop 结果。
// 和 §3 的 ReadResult 是两套词汇：SPSC 一条不丢，只需要"成功/空/满"。
enum class QueueStatus {
    Ok,
    Empty,
    Full,
    Invalid,      // 句柄没打开（layout_ == nullptr）
    SystemError
};

// 非异常路径的错误载体。定长数组而不是 std::string，是为了能按值传递、
// 并在 noexcept 路径上使用（set() 不分配内存）。
struct Error {
    int code = 0;
    char message[256]{};

    // "msg: strerror(e)"
    void set(int e, const char* msg) noexcept {
        code = e;
        std::snprintf(message, sizeof(message), "%s: %s", msg, std::strerror(e));
    }

    // 只有 msg，不带 strerror（用于 errno 无意义的调用方错误，例如名字非法）
    void setMessage(int e, const char* msg) noexcept {
        code = e;
        std::snprintf(message, sizeof(message), "%s", msg);
    }
};

// ===========================================================================
// §2 共用的错误文本
// ===========================================================================
// 这些字符串会被测试断言（例如必须含 "unlinkName"），也会被运维照着做，
// 所以放一处，避免两边措辞漂移。

inline constexpr const char* kMsgBadName =
    "invalid POSIX shm name: must start with '/' and contain no further '/'";

// 对象比预期布局小：T/Capacity 不匹配，或创建者在 ftruncate 前被 SIGKILL。
inline constexpr const char* kMsgStaleObject =
    "SHM object is smaller than the expected layout; wrong T/Capacity, or a "
    "stale object left by a crashed creator (call unlinkName() then retry)";

// 对象存在、size 也对，但 ready != 1：创建者死在初始化中间。
inline constexpr const char* kMsgNotInitialized =
    "SHM object is not initialized; a previous creator likely died during init "
    "(call unlinkName() then retry)";

// magic / version / size / capacity / item_size 任一不符。
inline constexpr const char* kMsgHeaderMismatch =
    "SHM header mismatch: incompatible queue layout";

// 抛异常版本的名字错误，带上是哪个名字。
inline std::string badNameMessage(const std::string& name) {
    return "invalid POSIX shm name '" + name +
           "': must start with '/' and contain no further '/'";
}

inline std::string errnoText(const char* what, int e) {
    return std::string(what) + ": " + std::strerror(e);
}

// 用当前 errno 造一个 std::runtime_error。
inline std::runtime_error systemError(const char* what) {
    return std::runtime_error(errnoText(what, errno));
}

// ===========================================================================
// §3 读侧状态（ShmSpmcQueue 的 pop / popLatest 用）
// ===========================================================================
// 放在命名空间而不是嵌套在类里：嵌套 enum 是按模板实例各自独立的类型，
// ShmSpmcQueue<A,64>::ReadStatus 和 <A,16>::ReadStatus 会互不相干。
// 类内保留同名别名，旧写法照样能用。

// 打开一个消费者对象时的起点。游标是**对象**的私有状态，
// 所以起点在构造时给，换起点就再开一个对象。
// **三个通道都用默认的 AfterLatest** —— 它既不复放历史，也读不到本次打开之前的快照
// （popLatest 里 `latest <= from` 直接返回 Empty），所以不需要额外判活。
enum class Start {
    AfterLatest, // 只收打开之后发布的 —— **三个通道都用它**（默认值），不会读到旧快照
    AtLatest,    // 先读打开时的最新一条（**可能是打开之前发布的**）：只给"要立刻显示
                 // 当前状态"的工具用，且必须自己判活；业务通道别用
    FromOldest,  // 从环里还留着的最早一条开始（监控 / 调试）
};

enum class ReadStatus {
    Message,    // 读到了新消息（out / seq 有效）
    Empty,      // 没有比游标更新的消息
    Gap,        // 要读的已被覆盖，游标被迫前移；lost 给出丢掉的条数
    Contended   // kMaxAttempts 次都没取得一致快照，稍后重试
};

struct ReadResult {
    ReadStatus status = ReadStatus::Empty;

    // status == Message 时有效：本次读到的消息序号
    std::uint64_t seq = 0;

    // status == Gap 时有效：**永久丢掉**的条数 —— 已被生产者覆盖，再也读不到。
    // 回报通道上必须记录 / 告警。
    std::uint64_t lost = 0;

    // status == Message 时有效，且只有 popLatest() 会填：按"只取最新"跳过的条数。
    // 这些消息**还在环里**，只是不读。持续 > 0 说明消费者在掉队。
    //
    // lost 和 skipped 是两件事：lost 是数据没了，skipped 是数据还在但按设计不要。
    std::uint64_t skipped = 0;
};

// ===========================================================================
// §4 名字校验
// ===========================================================================

// POSIX shm 名字规则：'/' 开头、后面至少还有一个字符、且不再出现第二个 '/'。
//
// 必须在这里挡住，而不是留给 shm_open：Linux 上 shm_open("/a/b") 报的是
// ENOENT（"系统调用失败"），和真正的原因（名字写错）差很远；
// macOS 更宽松，没有前导斜杠的名字竟然能成功。名字写错是编程错误，
// 应当在调用点立刻报出来。
inline bool validName(const char* name) noexcept {
    if (!name || name[0] != '/' || name[1] == '\0') return false;
    return std::strchr(name + 1, '/') == nullptr;
}

inline bool validName(const std::string& name) noexcept {
    if (name.size() < 2) return false;
    if (name[0] != '/') return false;
    return name.find('/', 1) == std::string::npos;
}

// ===========================================================================
// §5 flock / mmap / 自旋退避（转发 include/SpinBackoff.h）
// ===========================================================================

// flock() 的超时预算。初始化只做 ftruncate + mmap + 写 header，正常在微秒级；
// 2 秒只可能被"持锁进程被挂起"触发。
inline constexpr int kLockTimeoutMs = 2000;

// 带超时的非阻塞 flock(LOCK_EX)。
//
// 不用阻塞式 flock：它没有上界，持锁方被 SIGSTOP / 卡住时启动流程会**永久挂起**。
// 失败返回 false 并**保留 errno**（ETIMEDOUT = 超时；其余是真实错误，
// 含 ENOTSUP —— macOS 上 POSIX shm 不支持 flock，测试用 -DSHM_TEST_APPLE_SHIM 绕开）。
//
// 两个调用方对失败的处理不同（一个填 Error*、一个抛异常），
// 所以这里只做最底层的一件事：加锁 / 不成功就带着 errno 返回。
inline bool lockExclusive(int fd, int timeout_ms) noexcept {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);
    for (;;) {
        if (::flock(fd, LOCK_EX | LOCK_NB) == 0) return true;

        const int e = errno;
        if (e == EINTR) continue;                 // 信号打断，重试
        if (e != EWOULDBLOCK && e != EAGAIN) {    // 真错误（含 ENOTSUP）
            errno = e;
            return false;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            errno = ETIMEDOUT;
            return false;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
}

// LOCK_UN 不会阻塞，但同样会被信号打断。刻意不抛异常：
// 清理路径（catch 块）里也会调用它，在那里抛会 terminate。
inline bool unlockFd(int fd) noexcept {
    for (;;) {
        if (::flock(fd, LOCK_UN) == 0) return true;
        if (errno == EINTR) continue;
        return false;
    }
}

// mmap 共享内存。
//   MAP_POPULATE   建映射时一次填好页表，避免头几百次操作出现缺页毛刺。
//                  代价是 open() 变慢，而 open() 在启动路径上。
//   MADV_HUGEPAGE  Linux 上尽量用 2MB 大页，减少 TLB miss；内核没开 THP 时静默失败。
// bytes 由调用方给（sizeof(Layout)），所以这里不需要知道布局。
inline void* mapShared(int fd, std::size_t bytes) noexcept {
    int flags = MAP_SHARED;
#ifdef MAP_POPULATE
    flags |= MAP_POPULATE;
#endif
    void* addr = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, flags, fd, 0);
    if (addr == MAP_FAILED) return MAP_FAILED;
#ifdef MADV_HUGEPAGE
    ::madvise(addr, bytes, MADV_HUGEPAGE);
#endif
    return addr;
}

// ---------------------------------------------------------------------------
// §5.1 自旋让步与退避 —— 已抽到 include/SpinBackoff.h（R48）
// ---------------------------------------------------------------------------
// 这三个函数**不依赖共享内存、不依赖队列**，任何 "while 轮询、失败就重试" 的
// 循环都能用（db / dbp / tb / utrade_hft 的热循环都是这个形状）。所以它们住在
// 自己的头文件里，本文件只是转发一次，保持既有调用点不变。
//
//   spin::cpu_relax()         一条 CPU 让步指令。**无状态、无策略**。
//                             给"排空本地缓冲"的循环（db 的 mQueue、dbp 的 suber）。
//   spin::backoff(step)       阶梯 0..7 立刻重试 → 8..63 pause → 64+ yield()。
//                             给"等对端进程 μs 级事件"的热路径（报单 / 回报 / 行情）。
//   spin::backoff_sleep(step) 阶梯 + step > 256 后睡 20µs。
//                             给"低频**且**不独占核"的通道。
//
//   step 由调用方持有；**拿到数据的那一轮必须归零**。
//
// 完整的档位表、三个原语的量级差（pause 10-140 周期 / yield 系统调用 0.5-2µs /
// sleep_for 实测 50-60µs）、以及逐循环建议，见 docs/队列使用方式.md §6.4 与
// docs/全系统性能与延迟审查.md 的 P0-5。
using spin::backoff;
using spin::backoff_sleep;
using spin::cpu_relax;

// ===========================================================================
// §6 载荷与头部的公共校验
// ===========================================================================

// 载荷类型 T 的共同约束。
//
// **不要求** is_standard_layout：队列对 T 的全部操作就是整对象赋值和 memcpy，
// 对 trivially copyable 类型标准明确允许按对象表示逐字节搬运。要求它反而会误伤
// 合法载荷 —— md::CryptoMarketData（768B）因为 `Depth1 : public MDBase` 这种
// 基类与派生类各带数据成员的写法而不满足 standard-layout。
//
// 必须挡住的是**多态类型**：带虚函数的类同样可以 trivially copyable，
// 但把它的 vptr 复制到另一个进程毫无意义（ASLR）。
template <typename T>
inline constexpr bool validPayload() noexcept {
    return std::is_trivially_copyable<T>::value &&
           !std::is_polymorphic<T>::value &&
           sizeof(T) > 0;
}

// 头部里"与布局无关"的那几个字段。两个队列的字段名不同
// （SPSC: version/header_size/total_size/item_size；
//   SPMC: abi_version/layout_size/payload_words），但**校验规则完全一样**：
// 这几个值必须全部吻合，否则对象是别的 T / Capacity / 布局版本留下的。
struct HeaderView {
    std::uint64_t magic = 0;
    std::uint32_t version = 0;
    std::uint64_t size = 0;       // SPSC: total_size；SPMC: layout_size
    std::uint64_t capacity = 0;
    std::uint64_t item_size = 0;  // SPSC: item_size；SPMC: payload_words
};

inline bool headerMatches(const HeaderView& got, const HeaderView& want) noexcept {
    return got.magic == want.magic &&
           got.version == want.version &&
           got.size == want.size &&
           got.capacity == want.capacity &&
           got.item_size == want.item_size;
}

} // namespace shm_queue
