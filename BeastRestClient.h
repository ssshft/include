// BeastRestClient.h
//
// 异步 REST 客户端 (HFT-grade 优化版):
//
//   1. 异步 API (async_request),只回调,不阻塞
//   2. 预建连接池 (max_connections),持久 keep-alive 复用 TCP+TLS
//   3. 单 worker 线程驱动 io_context,所有 async callback 在 worker 线程触发
//   4. 单生产者契约:async_request 必须从同一线程调用 (SPSC queue),无锁
//   5. 失败连接自动后台重连,不影响其他连接
//
// HFT 优化:
//   - 用 SPSC return queue 取代 boost::object_pool (无隐藏锁)
//   - DNS resolve 仅一次,所有连接共享 endpoints
//   - 并行 establish (8 线程同时握手), 启动时间从 ~2s 降到 ~250ms
//   - pause spin 取代 sleep_for, 唤醒立即
//   - http::request 内联在 Connection 里,无 per-request 分配
//
// 使用契约:
//   - **async_request 只允许从 1 个线程调用** (策略线程)
//   - **callback 在 worker 线程执行**, 用户自己保证 callback 内部线程安全
//
// 示例:
//   net::HttpConfig cfg{.host = "api.binance.com", .max_connections = 32};
//   auto client = std::make_shared<net::RestClient>(cfg);
//   client->set_default_header("X-MBX-APIKEY", api_key);
//   client->async_request(http::verb::post, "/api/v3/order", form_body,
//                         "application/x-www-form-urlencoded",
//                         [](auto ec, auto resp) {
//                             if (!ec) handle_response(resp);
//                         });

// HttpResponse
// resp.status_code   HTTP 状态码(200/400/404); 0表示请求未发出
// resp.body      响应体，使用json解析
// resp.rtt_ns     端到端延迟(含网络RTT)
// ec 不为空： 传输层错误(网络断 / TLS失败 / 超时 / queue满 / pool满 / 无idle conn)
// ec 为空 + status_code = 2xx 业务成功
// ec 为空 + status_code = 4xx/5xx 业务被拒，resp..body里有交易所的错误JSON


#pragma once

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/lockfree/spsc_queue.hpp>
#include <boost/system/errc.hpp>
#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#if defined(__x86_64__) || defined(_M_X64)
  #include <immintrin.h>
  #define BTS_REST_PAUSE() _mm_pause()
#else
  #define BTS_REST_PAUSE() ((void)0)
#endif

#ifdef __linux__
  #include <pthread.h>
  #include <sched.h>
#endif

namespace net {

    namespace beast = boost::beast;
    namespace http  = beast::http;
    namespace asio  = boost::asio;
    namespace ssl   = asio::ssl;
    using tcp = asio::ip::tcp;

    // ========================================================================
    // 数据结构
    // ========================================================================


    struct IdleRing {
        explicit IdleRing(size_t max_conn) {
            size_t cap = 1;
            while (cap < max_conn + 1) {
                cap <<= 1;
            }

            buf_.resize(cap);
            mask_ = cap - 1;
        }

        bool empty() const noexcept {
            return head_ == tail_;
        }

        size_t size() const noexcept {
            return (tail_ - head_) & mask_;
        }

        void push(size_t v) noexcept {
            buf_[tail_ & mask_] = v;
            ++tail_;
        }

        size_t pop_front() noexcept {
            size_t v = buf_[head_ & mask_];
            ++head_;
            return v;
        }

    private:
        std::vector<size_t> buf_;
        size_t mask_ = 0;
        size_t head_ = 0;
        size_t tail_ = 0;
    };


    struct HttpResponse {
        int         status_code = 0;
        std::string body;
        int64_t     rtt_ns = 0;
    };

    using HttpCallback = std::function<void(boost::system::error_code, HttpResponse)>;

    struct RestClientConfig {
        std::string host;
        uint16_t    port = 443;
        bool        use_tls = true;
        bool        verify_peer = false;
        // 交易所 REST 速率上限典型 20-50 req/s, RTT 50-100ms,
        // 稳态并发 ≈ rate * RTT 一般 < 8, 8 个连接 + keep-alive 已远超需求。
        // 大并发还会触发交易所"connection attempts / IP"上限 (Binance 300/5min)。
        // 真要更高吞吐再调大, 不要按 "越多越好" 思维设置。
        size_t      max_connections = 8;
        // queue 容量只需略大于 max_connections (SPSC 主要靠 pool 兜底背压)
        size_t      request_queue_capacity = 256;
        // 启动时并行建连的线程数 (8 conns 用不到 8 线程, min(4, max_connections))
        size_t      parallel_establish_threads = 4;
        // worker 线程绑核 (-1 = 不绑)
        int         cpu_core = -1;
        // **整个请求** (write+read 总和) 的 RTT 预算 (0 = 不开)
        // 注意:不是 per-op, 而是端到端总预算。HFT 倾向严格上限。
        int         request_timeout_ms = 30'000;
        // 池大小:**硬上限**, 用满后 async_request 立即 fast-fail。
        // 设为 max_connections * 4 ~ 8 即可 (在线 ≤ max_connections, 余量给归还流水)。
        size_t      request_pool_size = 64;
    };

    // ========================================================================
    // RequestPool: 单生产单消费 freelist, 替代 boost::object_pool
    //   - alloc() 只在 caller 线程调用
    //   - free_from_worker() 只在 worker 线程调用 (push 到 SPSC return queue)
    //   - free_from_caller() 只在 caller 线程调用 (queue-full 路径, 直接归还本地)
    //   - alloc() 会先 drain return_queue, 把 worker 归还的对象拿回本地复用
    //
    // 不变量 (**硬上限 + sizing 保证 push 永不失败**):
    //   - 池在构造时一次性预分配 fixed_size 个 Request, 运行期不再 new
    //   - alloc() 用尽时返回 nullptr, 由 caller fast-fail
    //   - return_queue 容量 = fixed_size + 32 > 任何时刻 in-pool 的最大值,
    //     所以 free_from_worker 的 push 数学上不可能失败
    //   - debug 下 assert; release 信任不变量, push 不查返回值
    // ========================================================================

    struct Request {
        http::verb  method = http::verb::get;
        std::string path;
        std::string body;
        std::string content_type;
        HttpCallback callback;
        std::chrono::steady_clock::time_point start_time;
        Request* next_free = nullptr;
    };

    class RequestPool {
    public:
        explicit RequestPool(size_t fixed_size = 1024)
            : return_queue_(fixed_size + 32)
            , capacity_(fixed_size)
        {
            all_.reserve(fixed_size);
            for (size_t i = 0; i < fixed_size; ++i) {
                auto r = std::make_unique<Request>();
                r->next_free = head_local_;
                head_local_ = r.get();
                all_.push_back(std::move(r));
            }
        }

        ~RequestPool() = default;

        RequestPool(const RequestPool&) = delete;
        RequestPool& operator=(const RequestPool&) = delete;

        // caller 线程调用; 池满返回 nullptr (caller 必须 fast-fail)
        Request* alloc() noexcept {
            // 1. 先 drain worker 归还的 (一次性把累积的拿回来, 摊销开销)
            Request* freed;
            while (return_queue_.pop(freed)) {
                freed->next_free = head_local_;
                head_local_ = freed;
            }
            // 2. 从 local list 取
            if (head_local_) {
                auto* r = head_local_;
                head_local_ = r->next_free;
                r->next_free = nullptr;
                return r;
            }
            // 3. 用完 → fast-fail
            return nullptr;
        }

        // worker 线程调用 (callback 完成后归还)
        // sizing 保证 push 永远成功 (return_queue 容量 = capacity + 32 > 任意时刻 queued_returns)。
        // release build 信任不变量, 不查返回; debug build assert 一下做 sanity check。
        void free_from_worker(Request* r) noexcept {
            reset(r);
#ifndef NDEBUG
            bool ok = return_queue_.push(r);
            assert(ok && "RequestPool::return_queue full — sizing invariant violated");
#else
            return_queue_.push(r);
#endif
        }

        // caller 线程调用 (queue 满 / 池空时回调后, 同线程归还)
        void free_from_caller(Request* r) noexcept {
            reset(r);
            r->next_free = head_local_;
            head_local_ = r;
        }

        size_t capacity() const noexcept { return capacity_; }

    private:
        static void reset(Request* r) noexcept {
            r->method = http::verb::get;
            r->path.clear();
            r->body.clear();
            r->content_type.clear();
            r->callback = nullptr;
            r->next_free = nullptr;
        }

        Request* head_local_ = nullptr;                      // caller-only LIFO
        boost::lockfree::spsc_queue<Request*>  return_queue_;
        std::vector<std::unique_ptr<Request>>   all_;
        size_t                                  capacity_;
    };

    // ========================================================================
    // RestClient
    // ========================================================================

    class RestClient : public std::enable_shared_from_this<RestClient> {
    public:
        explicit RestClient(RestClientConfig cfg)
            : cfg_(validate_and_normalize(std::move(cfg)))
            , ioc_()
            , work_guard_(asio::make_work_guard(ioc_))
            , ssl_ctx_(ssl::context::tls_client)
            , req_queue_(cfg_.request_queue_capacity)
            , req_pool_(cfg_.request_pool_size)
            , stop_(false)
            , idle_indices_(cfg.max_connections)
        {
            if (cfg_.verify_peer) {
                ssl_ctx_.set_default_verify_paths();
                ssl_ctx_.set_verify_mode(ssl::verify_peer);
            } else {
                ssl_ctx_.set_verify_mode(ssl::verify_none);
            }

            // ---- 1. DNS resolve 一次, 所有 conn 共享 ----
            {
                tcp::resolver resolver(ioc_);
                resolved_eps_ = resolver.resolve(cfg_.host, std::to_string(cfg_.port));
            }

            // ---- 2. 预创建 Connection 对象 (不握手, 只构造) ----
            connections_.reserve(cfg_.max_connections);
            for (size_t i = 0; i < cfg_.max_connections; ++i) {
                connections_.push_back(
                    std::make_unique<Connection>(ioc_, ssl_ctx_));
            }

            // ---- 3. 并行 establish (N 个临时线程同时握手) ----
            parallel_establish_all();

            // ---- 4. 收集成功建连的 idle 索引 ----
            for (size_t i = 0; i < connections_.size(); ++i) {
                if (!connections_[i]->dead) {
                    idle_indices_.push(i);
                }
            }
            idle_count_atomic_.store(idle_indices_.size(), std::memory_order_release);

            worker_ = std::thread([this]() { run(); });
        }

        ~RestClient() {
            // 析构顺序很关键, 不要随意改:
            //   1) stop_ + join worker, 让 worker 退出循环, 之后 io_context 是 dtor 线程独占。
            //      若先 ioc_.stop() 再 join, worker 可能正在 poll() 里, 而 dtor 线程稍后再 poll()
            //      会产生双线程 race (回调里访问 idle_indices_, req_pool_ 都是 worker 私有的)。
            //   2) **drain req_queue_**: worker 没来得及 pop 的 pending 请求, 在这里以
            //      operation_aborted 触发一次 callback。保证 "每个 async_request 的 callback
            //      exactly-once" 契约不被 shutdown 破坏。
            //   3) close 所有 socket → cancel pending async, 进而把 operation_aborted 回调入队。
            //   4) work_guard_.reset() + poll() 在 dtor 线程独占地 drain in-flight 回调。
            stop_.store(true, std::memory_order_release);
            if (worker_.joinable()) worker_.join();

            // (2) 兜底 pending 请求 — worker 已退, 此处 dtor 线程是唯一的 SPSC consumer。
            {
                Request* pending = nullptr;
                boost::system::error_code abort_ec(
                    boost::asio::error::operation_aborted,
                    boost::asio::error::get_system_category());
                while (req_queue_.pop(pending)) {
                    try {
                        HttpResponse resp;
                        pending->callback(abort_ec, std::move(resp));
                    } catch (...) {
                        // 用户 callback 抛了, 咽掉; shutdown 路径不能被单个用户 bug 卡死
                    }
                    // free_from_caller 而非 _worker: 此刻 worker 不在, dtor 线程扮演 caller。
                    // 调用会通过 reset(r) 把 callback std::function 析构掉, 释放捕获资源。
                    req_pool_.free_from_caller(pending);
                    total_err_.fetch_add(1, std::memory_order_relaxed);
                }
            }

            // (3) 关 fd + cancel in-flight async → 触发其 callback 入 ioc 队列
            for (auto& c : connections_) c->close();

            // (4) drain in-flight 回调 (write/read/connect/handshake 都以 op_aborted 完成)
            work_guard_.reset();
            boost::system::error_code ec;
            while (ioc_.poll(ec) > 0) {}
            // 至此所有"已提交的 async_request"都拿到了 exactly-once 的 callback,
            // 后续成员析构链不会再触发任何 user callback。
        }

        RestClient(const RestClient&) = delete;
        RestClient& operator=(const RestClient&) = delete;

        // 单生产者契约:必须从同一线程调用
        void async_request(http::verb method,
                           std::string path,
                           std::string body,
                           std::string content_type,
                           HttpCallback callback)
        {
            Request* req = req_pool_.alloc();
            if (!req) {
                // 池满 → 立即在 caller 线程回调 fast-fail。
                // 用户契约: callback 不抛。若违约属于用户 bug, 不在 hot path 兜底。
                HttpResponse resp;
                boost::system::error_code ec(boost::system::errc::no_buffer_space,
                                             boost::system::system_category());
                callback(ec, std::move(resp));
                total_err_.fetch_add(1, std::memory_order_relaxed);
                return;
            }

            req->method       = method;
            req->path         = std::move(path);
            req->body         = std::move(body);
            req->content_type = std::move(content_type);
            req->callback     = std::move(callback);
            req->start_time   = std::chrono::steady_clock::now();

            if (!req_queue_.push(req)) {
                // queue 满 → 立即在 caller 线程回调
                HttpResponse resp;
                boost::system::error_code ec(boost::system::errc::no_stream_resources,
                                             boost::system::system_category());
                req->callback(ec, std::move(resp));
                req_pool_.free_from_caller(req);
                total_err_.fetch_add(1, std::memory_order_relaxed);
            }
        }

        // 设置常驻 header (例如 "X-MBX-APIKEY"). **必须在第一次 async_request 之前调用**,
        // 不是线程安全 (worker 线程的 send_request 会读取 default_headers_)。
        void set_default_header(std::string name, std::string value) {
            default_headers_.emplace_back(std::move(name), std::move(value));
        }

        // ---- 监控 ----
        // idle_count() 跨线程安全 (atomic 维护, idle_indices_ 自身只在 worker 线程读写)
        size_t   idle_count()    const noexcept {
            return idle_count_atomic_.load(std::memory_order_relaxed);
        }
        uint64_t total_sent()    const noexcept { return total_sent_.load(std::memory_order_relaxed); }
        uint64_t total_done()    const noexcept { return total_done_.load(std::memory_order_relaxed); }
        uint64_t total_err()     const noexcept { return total_err_.load(std::memory_order_relaxed); }
        uint64_t total_reconn()  const noexcept { return total_reconn_.load(std::memory_order_relaxed); }

    private:
        // ====================================================================
        // 内部类型
        // ====================================================================

        struct Connection {
            using StreamType = beast::ssl_stream<beast::tcp_stream>;
            std::unique_ptr<StreamType>       stream;
            beast::flat_buffer                buffer;
            http::response<http::string_body> response;
            // 内联 http_req 复用, 无 per-req 分配
            http::request<http::string_body>  http_req;
            bool dead = false;
            bool reconnecting = false;   // worker-only; 防止 worker 重复触发 async_reconnect
            uint32_t retry_backoff_ms = 0;   // 上次失败的 backoff 长度; 0 = 首次失败
            std::chrono::steady_clock::time_point next_retry_at;  // 早于此时间禁止 retry

            Connection(asio::io_context& ioc, ssl::context& ctx)
                : stream(std::make_unique<StreamType>(ioc, ctx)) {}

            // 用预 resolve 的 endpoints, 不再 DNS lookup
            bool establish(const tcp::resolver::results_type& eps,
                            const std::string& host, bool use_tls) {
                try {
                    asio::connect(beast::get_lowest_layer(*stream).socket(), eps);
                    if (use_tls) {
                        if (!SSL_set_tlsext_host_name(stream->native_handle(), host.c_str()))
                            return false;
                        stream->handshake(ssl::stream_base::client);
                    }
                    dead = false;
                    return true;
                } catch (const std::exception& e) {
                    std::cerr << "[rest] establish error: " << e.what() << std::endl;
                    dead = true;
                    return false;
                }
            }

            // 运行期重连走 RestClient::start_reconnect (async, 不阻塞 worker),
            // 不再提供同步 reestablish。

            void close() noexcept {
                // HFT: 跳过同步 TLS shutdown (asio::ssl::stream::shutdown 会阻塞等
                // peer's close_notify, 浪费一个 RTT, 对端死活我们也不关心), 直接关 fd。
                // peer 看到 TCP RST/FIN, 对交易所完全可接受。
                //
                // 不用 beast::tcp_stream::cancel() (它没有 ec-overload, 而 close() 必须 noexcept):
                //   - expires_never()   关 deadline (后续即使 timer wait 触发, 也是 op_aborted no-op)
                //   - socket.cancel(ec) cancel pending socket async ops (ec-overload, 不抛)
                //   - shutdown + close  发 FIN 然后释放 fd
                beast::error_code ec;
                auto& tcp = beast::get_lowest_layer(*stream);
                tcp.expires_never();
                tcp.socket().cancel(ec);
                tcp.socket().shutdown(asio::ip::tcp::socket::shutdown_both, ec);
                tcp.socket().close(ec);
                dead = true;
            }
        };

        // ====================================================================
        // 并行 establish
        //   - **仅在 RestClient 构造期间调用一次**, 不影响运行期稳定性
        //   - 子线程在函数内 emplace_back 创建, 在末尾 join, 函数返回后无后台残留
        //   - 不同线程写不同 connections_[i] (next_idx 原子分配, 无重叠)
        //   - ssl_ctx_ 共享读, OpenSSL SSL_CTX 在 init 完成后是线程安全的
        //   - 64 个 TLS 握手并行可把启动时间从 ~2s 降到 ~250ms
        //   - 失败的连接打上 dead=true, 后续由 worker 线程的 reconnect_dead_connections() 串行修复
        // ====================================================================

        void parallel_establish_all() {
            const size_t n_thr = std::min<size_t>(
                cfg_.parallel_establish_threads, cfg_.max_connections);
            std::vector<std::thread> ths;
            ths.reserve(n_thr);
            std::atomic<size_t> next_idx{0};

            for (size_t t = 0; t < n_thr; ++t) {
                ths.emplace_back([this, &next_idx]() {
                    for (;;) {
                        size_t i = next_idx.fetch_add(1);
                        if (i >= connections_.size()) return;
                        connections_[i]->establish(resolved_eps_, cfg_.host, cfg_.use_tls);
                    }
                });
            }
            for (auto& t : ths) t.join();
        }

        // ====================================================================
        // worker 主循环 (单线程, 全程无锁)
        // ====================================================================

        void run() {
            if (cfg_.cpu_core >= 0) {
                cpu_set_t cpuset;
                CPU_ZERO(&cpuset);
                CPU_SET(cfg_.cpu_core, &cpuset);
                pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
            }
            auto last_reconnect_check = std::chrono::steady_clock::now();

            while (!stop_.load(std::memory_order_acquire)) {
                bool busy = false;

                // 1. **先**驱动 io_context (callback 释放 conn, idle_indices_ 更新)
                //    这是关键:必须在 dispatch 新请求之前把已完成的 callback 处理掉,
                //    否则刚从 SPSC pop 的请求会因为 idle_indices_ 还是空而 error。
                if (ioc_.poll() > 0) busy = true;

                // 2. **再** drain SPSC 请求队列 (此时 idle conn 已经是最新)
                Request* req = nullptr;
                while (req_queue_.pop(req)) {
                    busy = true;
                    try_dispatch(req);
                }

                // 3. 每 2s 后台修复死连接
                auto now = std::chrono::steady_clock::now();
                if (now - last_reconnect_check > std::chrono::seconds(2)) {
                    reconnect_dead_connections();
                    last_reconnect_check = now;
                }

                // 4. 空闲 spin (HFT 模式: 不睡, pause + yield)
                if (!busy) {
                    for (int i = 0; i < 64; ++i) BTS_REST_PAUSE();
                    std::this_thread::yield();
                }
            }
        }

        // ====================================================================
        // 请求派发
        // ====================================================================

        void try_dispatch(Request* req) {
            // FIFO 调度: 从队首取, 队尾归还 → 每个连接被轮换使用, 避免
            //   1) 冷 conn 被交易所/防火墙 keep-alive timeout RST
            //   2) TLS session cache / TCP cwnd 退化
            // N=8 时 erase(begin()) ≈ 一个 cacheline 的 memmove, 可忽略。
            while (!idle_indices_.empty()) {
                size_t idx = idle_indices_.pop_front();
                idle_count_atomic_.fetch_sub(1, std::memory_order_relaxed);
                Connection* conn = connections_[idx].get();
                if (!conn->dead) {
                    total_sent_.fetch_add(1, std::memory_order_relaxed);
                    send_request(conn, req, idx);
                    return;
                }
                // dead conn 还在 idle_indices_ 是历史路径残留 (理论上 release 应该过滤掉),
                // 继续找下一个 alive 的
            }
            // 无空闲连接,立即回调错误
            HttpResponse resp;
            boost::system::error_code ec(boost::system::errc::no_stream_resources,
                                         boost::system::system_category());
            req->callback(ec, std::move(resp));
            req_pool_.free_from_worker(req);
            total_err_.fetch_add(1, std::memory_order_relaxed);
        }

        void release_connection(size_t idx, bool alive) {
            Connection* conn = connections_[idx].get();
            if (alive && !conn->dead) {
                idle_indices_.push(idx);
                idle_count_atomic_.fetch_add(1, std::memory_order_relaxed);
            } else {
                conn->dead = true;
            }
        }

        // ====================================================================
        // 发送请求 (inline http_req in conn, 无 per-req 分配)
        // ====================================================================

        void send_request(Connection* conn, Request* req, size_t conn_idx) {
            // 重置 http_req:
            //   - base().clear() 会清除**所有 fields + method + target**, 但
            //     **不重置 version 字段**, 所以下面要显式 version(11)
            //   - body().clear() 清掉上次请求体, 保留 string capacity 复用堆
            //   - 后续显式 set 的所有字段 (method/target/host/UA/keep_alive)
            //     都覆盖上次状态, 保证无残留
            conn->http_req.base().clear();
            conn->http_req.body().clear();

            conn->http_req.method(req->method);
            conn->http_req.target(req->path);
            conn->http_req.version(11);
            conn->http_req.set(http::field::host, cfg_.host);
            conn->http_req.set(http::field::user_agent, "bts-rest/1.0");
            conn->http_req.keep_alive(true);

            if (!req->body.empty()) {
                conn->http_req.body() = req->body;
                if (!req->content_type.empty())
                    conn->http_req.set(http::field::content_type, req->content_type);
                conn->http_req.prepare_payload();
            }

            // 应用默认 header (X-MBX-APIKEY 等). 这些 header 已在 base().clear() 中被清除,
            // 所以每次都要重新 set。
            for (auto& kv : default_headers_) {
                conn->http_req.set(kv.first, kv.second);
            }

            // **整个请求 (write+read) 的总预算**, 不是 per-op。HFT 倾向严格上限。
            // expires_after 只是设 deadline, 不启动定时器; deadline 在下次 async 操作开始时生效。
            // read_response 中不再 reset, 因此 write+read 共享一个总预算。
            if (cfg_.request_timeout_ms > 0) {
                beast::get_lowest_layer(*conn->stream).expires_after(
                    std::chrono::milliseconds(cfg_.request_timeout_ms));
            } else {
                beast::get_lowest_layer(*conn->stream).expires_never();
            }

            auto start = req->start_time;
            http::async_write(*conn->stream, conn->http_req,
                [this, conn, req, conn_idx, start]
                (beast::error_code ec, size_t /*n*/) mutable {
                    if (ec) {
                        HttpResponse resp;
                        resp.rtt_ns = elapsed_ns(start);
                        req->callback(ec, std::move(resp));
                        req_pool_.free_from_worker(req);
                        release_connection(conn_idx, /*alive=*/false);
                        total_err_.fetch_add(1, std::memory_order_relaxed);
                        return;
                    }
                    read_response(conn, req, conn_idx, start);
                });
        }

        void read_response(Connection* conn, Request* req, size_t conn_idx,
                            std::chrono::steady_clock::time_point start)
        {
            // 关键:每次 read 前重置 response 和 buffer
            conn->response = {};
            conn->buffer.consume(conn->buffer.size());

            http::async_read(*conn->stream, conn->buffer, conn->response,
                [this, conn, req, conn_idx, start]
                (beast::error_code ec, size_t /*n*/) mutable {
                    HttpResponse resp;
                    resp.rtt_ns = elapsed_ns(start);

                    if (ec) {
                        // read 失败时也尽量回填**已接收的** status_code 和 (可能部分的) body,
                        // 例如服务器已经发回 4xx + body 但 TCP 中途断, 或 timeout 但 header 已到。
                        // 上层可以据此做诊断 / 区分"网络出错"和"业务被拒"。
                        // (没收到任何数据时, result_int()==0, body()=="", 等价于留空)
                        resp.status_code = static_cast<int>(conn->response.result_int());
                        resp.body        = std::move(conn->response.body());
                        req->callback(ec, std::move(resp));
                        req_pool_.free_from_worker(req);
                        release_connection(conn_idx, /*alive=*/false);
                        total_err_.fetch_add(1, std::memory_order_relaxed);
                        return;
                    }

                    resp.status_code = static_cast<int>(conn->response.result_int());
                    resp.body        = std::move(conn->response.body());
                    bool alive = !conn->response.need_eof();

                    req->callback(ec, std::move(resp));
                    req_pool_.free_from_worker(req);
                    release_connection(conn_idx, alive);
                    total_done_.fetch_add(1, std::memory_order_relaxed);
                });
        }

        // ====================================================================
        // 死连接后台重连 (**async**, 不阻塞 worker)
        //
        // 原来的 sync establish + handshake 每个连接要 ~200-400ms (TCP 3-way + TLS 3 RTT),
        // 全死时 8 conn = 1.6-3.2s worker 完全卡住, in-flight 回调全部延迟。
        //
        // 现在改成 async_connect + async_handshake, 全部由 worker 自己的 ioc_.poll() 推进,
        // 回调也在 worker 线程触发 → idle_indices_ 修改仍然单线程, 无需锁。
        // 单次 reconnect_dead_connections() 调用只是 kick off, 立刻返回。
        // ====================================================================

        // reconnect 失败时关 fd, 避免 fd 在重试间隔内被无连接 socket 占用。
        // 同时按指数退避推迟下次 retry, 防止 peer 永久不可达时 2s 一发的握手风暴。
        //
        // 变量约定:
        //   retry_backoff_ms = **本次失败之后需要等待的时长** (即"current wait")
        //     - 0 表示从未失败过
        //     - 失败时: 0 → 2s, 否则翻倍, 上限 30s
        //   next_retry_at = now + retry_backoff_ms
        //   成功重连后清零, 下次失败重新从 2s 起步
        //
        // 实际等待序列: 2s → 4s → 8s → 16s → 30s → 30s ...
        void fail_reconnect(Connection* conn) noexcept {
            conn->close();
            conn->reconnecting = false;
            if (conn->retry_backoff_ms == 0) {
                conn->retry_backoff_ms = 2'000;
            } else {
                conn->retry_backoff_ms = std::min<uint32_t>(conn->retry_backoff_ms * 2, 30'000);
            }
            conn->next_retry_at = std::chrono::steady_clock::now()
                                + std::chrono::milliseconds(conn->retry_backoff_ms);
        }

        void mark_reconnected(Connection* conn, size_t idx) noexcept {
            conn->reconnecting = false;
            conn->dead = false;
            conn->retry_backoff_ms = 0;
            conn->next_retry_at = {};
            idle_indices_.push(idx);
            idle_count_atomic_.fetch_add(1, std::memory_order_relaxed);
            total_reconn_.fetch_add(1, std::memory_order_relaxed);
        }

        void start_reconnect(size_t idx) {
            Connection* conn = connections_[idx].get();
            if (!conn->dead || conn->reconnecting) return;
            if (std::chrono::steady_clock::now() < conn->next_retry_at) return;  // 退避中

            conn->reconnecting = true;
            conn->close();   // 关旧 fd / cancel pending
            conn->stream = std::make_unique<Connection::StreamType>(ioc_, ssl_ctx_);
            conn->buffer.consume(conn->buffer.size());
            conn->response = {};
            conn->http_req = {};

            // reconnect 期间不要让 request_timeout 误伤 (handshake 可能慢)
            beast::get_lowest_layer(*conn->stream).expires_never();

            asio::async_connect(
                beast::get_lowest_layer(*conn->stream).socket(),
                resolved_eps_,
                [this, conn, idx](beast::error_code ec, const tcp::endpoint&) {
                    if (ec) { fail_reconnect(conn); return; }
                    if (!cfg_.use_tls) { mark_reconnected(conn, idx); return; }

                    // SNI (字符串非空 + SSL 句柄刚 new, 不会失败)
                    SSL_set_tlsext_host_name(conn->stream->native_handle(), cfg_.host.c_str());
                    conn->stream->async_handshake(ssl::stream_base::client,
                        [this, conn, idx](beast::error_code ec2) {
                            if (ec2) { fail_reconnect(conn); return; }
                            mark_reconnected(conn, idx);
                        });
                });
        }

        void reconnect_dead_connections() {
            // dead 状态机保证: dead=true 的 conn 不会出现在 idle_indices_ 里
            // (release_connection 失败时直接置 dead, 不入 idle; reconnect 成功才入 idle)。
            for (size_t i = 0; i < connections_.size(); ++i) {
                Connection* conn = connections_[i].get();
                if (!conn->dead || conn->reconnecting) continue;
                start_reconnect(i);   // 非阻塞: 只发射 async_connect, 立即返回
            }
        }

        static int64_t elapsed_ns(std::chrono::steady_clock::time_point t0) noexcept {
            return std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - t0).count();
        }

        // 构造期一次性校验 + 自动规整, 避免 0-size 配置出现"沉默假死"客户端,
        // 也避免 pool 比 max_connections 还小这种永远 fast-fail 的配置错误。
        static RestClientConfig validate_and_normalize(RestClientConfig cfg) {
            if (cfg.host.empty())
                throw std::invalid_argument("RestClientConfig.host must be non-empty");
            if (cfg.port == 0)
                throw std::invalid_argument("RestClientConfig.port must be > 0");
            if (cfg.max_connections == 0)
                throw std::invalid_argument("RestClientConfig.max_connections must be > 0");
            if (cfg.request_queue_capacity == 0)
                throw std::invalid_argument("RestClientConfig.request_queue_capacity must be > 0");
            // pool 必须 ≥ max_connections * 2 (在线 + 归还流水), 不够就自动放大
            const size_t min_pool = std::max<size_t>(16, cfg.max_connections * 4);
            if (cfg.request_pool_size < min_pool) cfg.request_pool_size = min_pool;
            if (cfg.parallel_establish_threads == 0) cfg.parallel_establish_threads = 1;
            return cfg;
        }

    private:
        RestClientConfig cfg_;
        asio::io_context ioc_;
        asio::executor_work_guard<asio::io_context::executor_type> work_guard_;
        ssl::context ssl_ctx_;
        tcp::resolver::results_type resolved_eps_;  // DNS 一次, 共享

        boost::lockfree::spsc_queue<Request*> req_queue_;
        RequestPool req_pool_;
        std::atomic<bool> stop_;
        std::thread worker_;

        std::vector<std::unique_ptr<Connection>> connections_;
        IdleRing idle_indices_;
        std::atomic<size_t> idle_count_atomic_{0};  // cross-thread snapshot
        std::vector<std::pair<std::string, std::string>> default_headers_;  // 启动期写入, 运行期只读

        std::atomic<uint64_t> total_sent_{0};
        std::atomic<uint64_t> total_done_{0};
        std::atomic<uint64_t> total_err_{0};
        std::atomic<uint64_t> total_reconn_{0};
    };

} // namespace exchange