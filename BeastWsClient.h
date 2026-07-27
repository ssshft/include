// BeastWsClient.h
//
// HFT-grade async WebSocket client, 基于 boost::beast + boost::asio。
//
// 设计目标:
//   1. 接口对齐 cpprestsdk: on_message/on_close/on_open/on_error + start/stop/send_text/send_binary
//   2. **单 IO 线程**, callback inline 在 IO 线程触发, 无 thread pool dispatch jitter
//   3. 整条 connect 链全 async, 重连内置 (指数退避)
//   4. 三种 ping/pong 策略 (Binance ServerOnly / OKX-Bybit ClientPeriodicText / HTX Disabled)
//
// 使用契约:
//   - **必须用 shared_ptr 持有**, 用 `WsClient::create(cfg)` 工厂构造
//   - on_message 在 IO 线程同步触发, callback 必须不阻塞 (sub-100us)
//   - (data, len) 指向 read_buffer_ 内部 storage, callback 返回后失效, 须同步消费
//   - **必须先 stop() 再让 shared_ptr 引用归零**, 否则 callback 链自我保活, dtor 不跑
//   - 不要在 callback 里销毁 WsClient (self-join 死锁)
//
// 使用示例:
//   net::WsConfig cfg;
//   cfg.url = "wss://stream.binance.com:9443/ws/btcusdt@bookTicker";
//   cfg.subscribe_messages = {...};
//   cfg.idle_timeout_sec      = 60;   // 60s 任何 frame 都没 → 重连 (抓 TCP 死)
//   cfg.data_idle_timeout_sec = 30;   // 30s 没业务 frame → 重连 (抓伪健康连接)
//
//   auto ws = net::WsClient::create(cfg);
//   ws->on_message([](const uint8_t* data, size_t len, bool is_binary, int64_t ts) {
//       rapidjson::Document doc;
//       doc.Parse(reinterpret_cast<const char*>(data), len);   // 零拷贝
//   });
//   ws->on_close([](int code, const std::string& reason) {
//       // 仅作记录, **不需要写重连逻辑** —— lib 自动指数退避重连 + 重发订阅
//       LOG_WARN("ws closed: {} (auto-reconnecting)", reason);
//   });
//   ws->start();
//   // ... 用一段时间 ...
//   ws->stop();      // 必须先停
//   ws.reset();      // refcount 归零, dtor 跑


// ws->on_message([](const uint8_t* data, size_t len, bool /*is_binary*/, int64_t ts) {
//     rapidjson::Document doc;
//     doc.Parse(reinterpret_cast<const char*>(data), len);   // 零拷贝
//     if (!doc.HasParseError()) {
//         double bid = doc["b"].GetDouble();
//         double ask = doc["a"].GetDouble();
//         // ...
//     }
// });

// simdjson::ondemand::parser parser;   // 长期持有, 避免反复初始化

// ws->on_message([&parser](const uint8_t* data, size_t len, bool is_binary, int64_t ts) {
//     // 选项 A: padded_string 拷贝 (一次 memcpy + alloc, ~100ns 对小帧)
//     simdjson::padded_string padded(reinterpret_cast<const char*>(data), len);
//     auto doc = parser.iterate(padded);
//     double bid = double(doc["b"]);

//     // 选项 B (高级): 如果保证 buffer 末尾有 padding (自己定制 read_buffer), 
//     // 可以用 padded_string_view 零拷贝。但 beast::flat_buffer 不保证, 不推荐
// });

// sbe
// ws->on_message([&parser](const uint8_t* data, size_t len, bool is_binary, int64_t ts) {
//    if (is_binary) {
//        SbeDecoder::TradesView view(data, len);
//    }
// }
//
// 订阅 / 连接两种风格:
//   A) URL 自带订阅 (Binance 风格):    wss://host/ws/btcusdt@depth20
//   B) URL + handshake 后发订阅消息 (绝大多数交易所):
//        connect wss://host/path → on_open 触发 → 发 subscribe JSON
//        我们的 lib 在 cfg.subscribe_messages 里配好, on_connected 自动发,
//        每次重连都会重新发 (业务侧零代码管理订阅恢复)
//
// ----- 示例 1: Binance JSON 行情, URL 自带订阅 (无需 subscribe body) -----
//   bts::net::WsConfig cfg;
//   cfg.url = "wss://stream.binance.com:9443/ws/btcusdt@bookTicker";
//   cfg.ping_mode = WsConfig::PingMode::ServerOnly;       // Binance: server 发 PING
//   cfg.idle_timeout_sec      = 60;
//   cfg.data_idle_timeout_sec = 30;
//
//   auto ws = bts::net::WsClient::create(cfg);
//   ws->on_message([](const uint8_t* d, size_t n, bool, int64_t ts) {
//       rapidjson::Document doc;
//       doc.Parse(reinterpret_cast<const char*>(d), n);
//   });
//   ws->start();
//
// ----- 示例 2: Binance JSON 行情, 多 stream 用 /stream 复用一个 ws -----
//   // Binance 的 /stream 端点支持后续动态订阅, 但简单做法是 URL 自带:
//   cfg.url = "wss://stream.binance.com:9443/stream?"
//             "streams=btcusdt@bookTicker/btcusdt@trade/ethusdt@depth20";
//   // 或者 URL 用 /ws + 通过 SUBSCRIBE 命令动态订阅 (下一例)
//
// ----- 示例 3: Binance 通过 subscribe 命令订阅 (URL 不带 stream) -----
//   cfg.url = "wss://stream.binance.com:9443/ws";
//   // 连上之后立刻发的 JSON, on_open 之后自动发, 重连也会重发
//   cfg.subscribe_messages = {
//       R"({"method":"SUBSCRIBE",)"
//       R"("params":["btcusdt@bookTicker","btcusdt@trade","ethusdt@depth20"],)"
//       R"("id":1})"
//   };
//   auto ws = bts::net::WsClient::create(cfg);
//   ws->on_message([](auto* d, auto n, bool, auto ts) { /* ... */ });
//   ws->start();
//
// ----- 示例 4: OKX 公共行情 (subscribe JSON 是数组对象格式) -----
//   bts::net::WsConfig cfg;
//   cfg.url = "wss://ws.okx.com:8443/ws/v5/public";
//   cfg.ping_mode = WsConfig::PingMode::ClientPeriodicText;   // OKX 要 client 主动 ping
//   cfg.client_ping_interval_sec = 20;
//   cfg.client_ping_text         = "ping";                    // OKX 是 text "ping"
//   cfg.subscribe_messages = {
//       R"({"op":"subscribe","args":[)"
//         R"({"channel":"books5","instId":"BTC-USDT"},)"
//         R"({"channel":"trades","instId":"BTC-USDT"})"
//       R"(]})"
//   };
//   auto ws = bts::net::WsClient::create(cfg);
//   ws->on_message([&ws](const uint8_t* d, size_t n, bool, int64_t) {
//       // OKX 用 text "ping" 心跳 — server 偶尔会主动发 text "ping", 业务侧回 "pong"
//       if (n == 4 && std::memcmp(d, "ping", 4) == 0) { ws->send_text("pong"); return; }
//       rapidjson::Document doc;
//       doc.Parse(reinterpret_cast<const char*>(d), n);
//       // ... 解析 channel / data 字段
//   });
//   ws->start();
//
// ----- 示例 5: Bybit 公共行情 (JSON op=subscribe, args 是字符串数组) -----
//   bts::net::WsConfig cfg;
//   cfg.url = "wss://stream.bybit.com/v5/public/spot";
//   cfg.ping_mode = WsConfig::PingMode::ClientPeriodicText;
//   cfg.client_ping_interval_sec = 20;
//   cfg.client_ping_text         = R"({"op":"ping"})";        // Bybit 是 JSON 帧
//   cfg.subscribe_messages = {
//       R"({"op":"subscribe","args":["orderbook.1.BTCUSDT","publicTrade.BTCUSDT"]})"
//   };
//   auto ws = bts::net::WsClient::create(cfg);
//   ws->on_message([](auto* d, auto n, bool, auto ts) { /* ... */ });
//   ws->start();
//
// ----- 示例 6: HTX (Huobi) 公共行情 (gzip binary frame + sub 字段) -----
//   bts::net::WsConfig cfg;
//   cfg.url = "wss://api.huobi.pro/ws";
//   cfg.ping_mode = WsConfig::PingMode::Disabled;             // HTX 是应用层 ping
//   cfg.subscribe_messages = {
//       R"({"sub":"market.btcusdt.depth.step0","id":"id1"})",
//       R"({"sub":"market.btcusdt.trade.detail","id":"id2"})"
//   };
//   auto ws = bts::net::WsClient::create(cfg);
//   ws->on_message([&ws](const uint8_t* d, size_t n, bool is_binary, int64_t) {
//       // HTX 推送是 gzip 压缩的 binary frame, 需要 gunzip 才能拿到 JSON
//       std::string raw = gunzip(d, n);
//       // HTX 服务器主动发 {"ping":1234567890}, 客户端需要回 {"pong":1234567890}
//       if (raw.find("\"ping\"") != std::string::npos) {
//           std::string pong = raw; auto p = pong.find("ping"); pong.replace(p, 4, "pong");
//           ws->send_text(std::move(pong));
//           return;
//       }
//       // ... rapidjson 解析 raw
//   });
//   ws->start();
//
// ----- 示例 7: OKX 私有账户流 (需要 login 鉴权) -----
//   bts::net::WsConfig cfg;
//   cfg.url = "wss://ws.okx.com:8443/ws/v5/private";
//   cfg.ping_mode = WsConfig::PingMode::ClientPeriodicText;
//   cfg.client_ping_interval_sec = 20;
//   cfg.client_ping_text = "ping";
//   // OKX 私有流: 先发 login, 然后再发 subscribe
//   // sign = base64(hmac_sha256(ts + "GET" + "/users/self/verify", secret_key))
//   std::string ts  = std::to_string(time(nullptr));
//   std::string sig = okx_sign(ts, secret_key);
//   cfg.subscribe_messages = {
//       fmt::format(R"({{"op":"login","args":[{{)"
//                   R"("apiKey":"{}","passphrase":"{}","timestamp":"{}","sign":"{}")"
//                   R"(}}]}})", api_key, passphrase, ts, sig),
//       R"({"op":"subscribe","args":[{"channel":"orders","instType":"SPOT"}]})"
//   };
//   // 注意: login + subscribe 在 cfg.subscribe_messages 里**按顺序发**,
//   //        OKX 接收到 login response 之前订阅可能会失败 — 极少数情况要分两步:
//   //        on_message 里看到 login 成功的回执后再调 ws->send_text(subscribe_json)。
//   //        但大多数情况下顺序发就 OK (OKX 内部排队)
//
// ----- 示例 8: Binance 用户数据流 (listen_key 路径鉴权) -----
//   // Binance 私有流通过 listen_key 鉴权, URL 路径里带:
//   std::string listen_key = call_binance_rest_to_get_listen_key();
//   bts::net::WsConfig cfg;
//   cfg.url = "wss://stream.binance.com:9443/ws/" + listen_key;
//   cfg.ping_mode = WsConfig::PingMode::ServerOnly;
//   // 不需要 subscribe_messages, 路径自带订阅 (用户的所有账户事件)
//   auto ws = bts::net::WsClient::create(cfg);
//   ws->on_message([](auto* d, auto n, bool, auto ts) { /* 账户更新 */ });
//   ws->start();
//   // ⚠️ listen_key 每 60min 要 REST PUT 一次 keep-alive, 业务侧自己起 timer 续命
//
// ----- 示例 9: 运行期动态订阅/退订 (不在 cfg.subscribe_messages 里写) -----
//   // 比如新加一个 symbol 的订阅, 不重连
//   ws->send_text(R"({"method":"SUBSCRIBE","params":["solusdt@bookTicker"],"id":42})");
//   // 退订:
//   ws->send_text(R"({"method":"UNSUBSCRIBE","params":["btcusdt@trade"],"id":43})");
//   //
//   // 注意: 这种动态订阅在**重连后会丢失** (cfg.subscribe_messages 才是 lib 自动重发的源)。
//   //       如果要重连后保持, 把动态订阅也加到 cfg.subscribe_messages 里 (不过那时 cfg
//   //       已经 const, 实际工程上做法是: 业务侧维护一个"当前订阅集合", on_open 时
//   //       通过 send_text 全部发一遍, 替代 cfg.subscribe_messages)
//
// ----- 示例 10: 用 fmt 动态构造 subscribe (例如订阅当前账户感兴趣的 symbol) -----
//   std::vector<std::string> symbols = {"btcusdt", "ethusdt", "solusdt"};
//   std::string params;
//   for (size_t i = 0; i < symbols.size(); ++i) {
//       if (i) params += ",";
//       params += "\"" + symbols[i] + "@bookTicker\"";
//   }
//   cfg.subscribe_messages = {
//       fmt::format(R"({{"method":"SUBSCRIBE","params":[{}],"id":1}})", params)
//   };



// class BinanceBookTickerClient {
//     public:
//         void start_ws(const std::string& symbol) {
//             net::WsConfig cfg;
//             cfg.url = "wss://stream.binance.com:9443/ws/" + symbol + "@bookTicker";
//             cfg.idle_timeout_sec      = 60;
//             cfg.data_idle_timeout_sec = 30;
    
//             ws_ = net::WsClient::create(cfg);
    
//             // 用一行 lambda 转发给成员函数。lambda 只捕 this, SSO 不分配堆。
//             ws_->on_message([this](const uint8_t* d, size_t n, bool b, int64_t t) {
//                 this->handle_message(d, n, b, t);
//             });
//             ws_->on_open ([this]() { this->handle_open(); });
//             ws_->on_close([this](int c, const std::string& r) { this->handle_close(c, r); });
//             ws_->on_error([this](const std::string& m) { this->handle_error(m); });
    
//             ws_->start();
//         }
    
//         ~BinanceBookTickerClient() {
//             // ★ 必须先 stop, 再 reset, 顺序不能颠倒
//             if (ws_) { ws_->stop(); ws_.reset(); }
//         }
    
//     private:
//         // 这里是真正的业务逻辑, 跟 lambda 完全脱钩
//         void handle_message(const uint8_t* data, size_t len, bool /*is_binary*/, int64_t ts_ns) {
//             rapidjson::Document doc;
//             doc.Parse(reinterpret_cast<const char*>(data), len);
//             if (doc.HasParseError()) return;
    
//             // 业务字段访问:
//             double bid = doc["b"].GetDouble();
//             double ask = doc["a"].GetDouble();
//             last_bid_ = bid;
//             last_ask_ = ask;
//             // ... push 到 SHM ring, 通知策略 ...
//         }
    
//         void handle_open() {
//             LOG_INFO("bookTicker ws connected");
//             ++connect_count_;
//         }
//         void handle_close(int code, const std::string& reason) {
//             LOG_WARN("bookTicker ws closed: code={} reason={} (auto-reconnect)", code, reason);
//         }
//         void handle_error(const std::string& msg) {
//             LOG_ERROR("bookTicker ws error: {}", msg);
//         }
    
//         std::shared_ptr<net::WsClient> ws_;
//         double   last_bid_ = 0, last_ask_ = 0;
//         uint64_t connect_count_ = 0;
//     };
    
//     // 使用
//     auto client = std::make_unique<BinanceBookTickerClient>();
//     client->start_ws("btcusdt");
//     // ... 运行 ...
//     client.reset();   // dtor 自动 stop + reset ws_



#pragma once

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace net {

namespace beast = boost::beast;
namespace asio  = boost::asio;
namespace ssl   = asio::ssl;
namespace ws    = beast::websocket;
using tcp = asio::ip::tcp;

// ============================================================================
// 配置
// ============================================================================

struct WsConfig {
    std::string url;
    std::vector<std::pair<std::string, std::string>> headers;
    std::vector<std::string> subscribe_messages;   // 重连后自动重发

    bool verify_peer = false;

    bool auto_reconnect = true;
    int reconnect_initial_backoff_sec = 1;   // **必须 ≥ 1**, 见 §validate
    int reconnect_max_backoff_sec     = 60;

    enum class PingMode {
        ServerOnly,           // Binance / Gate: beast 自动响应 server PING, 即ping消息体会自动回复pong消息，内容和ping一致
        ClientPeriodicText,   // OKX / Bybit: 周期发应用层 text ping
        Disabled,             // HTX: 用户在 on_message 里处理
    };
    PingMode ping_mode = PingMode::ServerOnly;
    int client_ping_interval_sec = 0;
    std::string client_ping_text;

    // 多久没收到**任何 frame**(含 PING/PONG) → 重连 (0 = 不检查)。
    // 抓"TCP/网络层死了": ping/pong 也算 frame, 所以这里超时一定意味着连接真挂了。
    int idle_timeout_sec = 60;

    // 多久没收到**业务数据 frame** (PING/PONG 不算) → 重连 (0 = 不检查)。
    // 抓"伪健康"连接: TCP 活着 (PING/PONG 在跑), 但行情管道挂了 / 订阅被静默拒绝 /
    // 标的停牌等。HFT 必须防的"看似活着实际过期"状态。
    // 设置建议:
    //   - 高活跃 (BTC bookTicker): 30~60s
    //   - 中活跃 (alt 主流):       120~300s
    //   - 稀疏 (私有账户流):       关掉 (0), 靠 idle_timeout_sec 兜底
    int data_idle_timeout_sec = 0;

    int connect_timeout_sec = 15;
    int handshake_timeout_sec = 15;
};

// ============================================================================
// WsClient
// ============================================================================

class WsClient : public std::enable_shared_from_this<WsClient> {
public:
    // (data, len, is_binary, recv_ts_ns) — IO 线程同步调用
    // data 指针 callback 返回后立即失效, 用户必须同步消费
    using MessageHandler = std::function<void(const uint8_t* data, size_t len, bool is_binary, int64_t recv_ts_ns)>;
    using CloseHandler = std::function<void(int code, const std::string& reason)>;
    using OpenHandler = std::function<void()>;
    using ErrorHandler = std::function<void(const std::string& msg)>;

    // 唯一推荐的构造路径 (强制 shared_ptr, 配合 enable_shared_from_this)
    static std::shared_ptr<WsClient> create(WsConfig cfg) {
        return std::shared_ptr<WsClient>(new WsClient(std::move(cfg)));
    }

    ~WsClient() {
        // 进入 dtor 意味着 refcount=0, 所有 async lambda 已 release self。
        // io_thread 持的是 raw this (避免循环引用), 我们只要 join 拿回 OS thread handle。

        // 安全兜底：如果用户违反契约，在没有显式调用 stop() 的情况下就释放了 shared_ptr
        // 必须在此处强行拦截，强制重置 work_guard 并 stop 掉 ioc，确保 detached 线程没有多余的未决事件去触碰野指针
        if (running_.exchange(false)) {
            if (work_guard_) {
                work_guard_.reset();
            }

            ioc_.stop();
        }

        if (io_thread_.joinable()) {
            if (std::this_thread::get_id() == io_thread_.get_id()) {
                // 如果是 io_thread 自身调用了 stop()，不能 join()，改用 detach() 允许其自行安全退出
                io_thread_.detach();
            } else {
                io_thread_.join();
            }
        }
    }

    WsClient(const WsClient&) = delete;
    WsClient& operator=(const WsClient&) = delete;

    void on_message(MessageHandler h) { 
        msg_handler_   = std::move(h); 
    }

    void on_close(CloseHandler h) {
        close_handler_ = std::move(h); 
    }

    void on_open(OpenHandler h) { 
        open_handler_  = std::move(h); 
    }

    void on_error(ErrorHandler h) { 
        error_handler_ = std::move(h); 
    }

    void start() {
        if (running_.exchange(true)) {
            return;
        }

        work_guard_.emplace(asio::make_work_guard(ioc_));

        asio::post(ioc_, [self = shared_from_this()]() { 
            self->do_connect(); 
        });

        // io_thread 故意只捕 raw this, 否则跟 WsClient 循环引用 → dtor 永不跑。
        // raw this 的生命期保证: ~WsClient 的 join() 等它退出前 this 一定有效。
        io_thread_ = std::thread([this]() {
            try { 
                ioc_.run(); 
            }
            catch (const std::exception& e) { 
                fire_error(std::string("ioc.run: ") + e.what()); 
            }
        });
    }

    void stop() {
        if (!running_.exchange(false)) {
            return;
        }
        // io 线程负责清理 socket / cancel (因为 socket 是 io 线程独占的)。
        // 关键: 这条 posted lambda 必须能跑到, 不能被 ioc.stop() 切掉, 否则:
        //   - socket.cancel() 不发生 → 不会触发 pending async ops 的 op_aborted
        //   - on_disconnect → fire_close 不会触发 → user 拿不到 on_close 通知
        //   破坏 callback exactly-once 契约。
        asio::post(ioc_, [this]() { 
            shutdown_inline(); 
        });

        // 调用线程直接 reset work_guard, 让 ioc.run() 在:
        //   1) 跑完 posted shutdown_inline (cancel socket)
        //   2) drain 完所有 op_aborted 回调 (fire close)
        //   3) drain 完 ping_timer / idle_timer 的 wait 取消回调
        // 之后, 因为没有 work_guard 也没有 pending op, 自然退出。
        //
        // **不** 调 ioc_.stop(): 它会立刻切断 run() 不 dispatch 队列里剩下的 handler,
        // 会把上面的 posted shutdown 跳过。ioc.stop() 留给 dtor 兜底 (违反契约时强制退出)。
        if (work_guard_) {
            work_guard_.reset();
        }

        if (io_thread_.joinable()) {
            if (std::this_thread::get_id() == io_thread_.get_id()) {
                // 如果是 io_thread 自身调用了 stop() (e.g. user 在 callback 里调),
                // 不能 join(), 改用 detach() 允许其自行安全退出。
                // ⚠️ 注意: 此后 io_thread 仍在 ioc.run() 里, 而 dtor 继续往下时
                //   成员 (ioc_, ssl_ctx_, ...) 会被析构, 跟 run() 撞车 → 仍有 UB 风险。
                //   这条路径是 user-error 兜底, 用户应该**避免在 callback 里销毁 WsClient**。
                io_thread_.detach();
            } else {
                io_thread_.join();
            }
        }
    }

    // ---- 发送 (任意线程调; 内部 asio::post 串行化, 无锁) ----
    // 用 std::string 一路 move 到底, 0 拷贝、0 新增堆分配 (HFT 关键)
    void send_text(std::string text) {
        asio::post(ioc_, [self = shared_from_this(), t = std::move(text)]() mutable {
            self->queue_write(std::move(t), /*binary=*/false);
        });
    }

    // binary 也用 std::string 容器 (它本就是 byte sequence, 不区分编码)。
    // 这样 send_text 和 send_binary 走同一条 0 拷贝路径。
    void send_binary(std::string data) {
        asio::post(ioc_, [self = shared_from_this(), d = std::move(data)]() mutable {
            self->queue_write(std::move(d), /*binary=*/true);
        });
    }

    bool is_connected() const noexcept { 
        return connected_.load(std::memory_order_acquire); 
    }

    int64_t last_rx_ns() const noexcept { 
        return last_rx_ns_.load(std::memory_order_relaxed); 
    }

    uint64_t total_rx() const noexcept { 
        return total_rx_.load(std::memory_order_relaxed); 
    }

    uint64_t total_tx() const noexcept { 
        return total_tx_.load(std::memory_order_relaxed); 
    }

    uint64_t reconnect_count() const noexcept { 
        return reconnect_count_.load(std::memory_order_relaxed); 
    }

private:
    explicit WsClient(WsConfig cfg)
        : cfg_(validate(std::move(cfg)))
        , ioc_()
        , ssl_ctx_(ssl::context::tlsv12_client)
        , reconnect_timer_(ioc_)
        , ping_timer_(ioc_)
        , idle_timer_(ioc_)
    {
        if (cfg_.verify_peer) {
            ssl_ctx_.set_default_verify_paths();
            ssl_ctx_.set_verify_mode(ssl::verify_peer);
        } else {
            ssl_ctx_.set_verify_mode(ssl::verify_none);
        }
        current_backoff_ = std::chrono::seconds(cfg_.reconnect_initial_backoff_sec);

        // 1MB 硬上限 (portable, 不依赖单参 ctor 语义)
        read_buffer_.max_size(kMaxFrameBytes);
    }

    using WsStream = ws::stream<beast::ssl_stream<beast::tcp_stream>>;

    static constexpr size_t kMaxFrameBytes = 1 * 1024 * 1024;

    struct OutgoingFrame {
        std::string data;
        bool is_binary;
    };

    static int64_t now_ns() noexcept {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    static WsConfig validate(WsConfig cfg) {
        if (cfg.url.empty()) {
            throw std::invalid_argument("WsConfig.url required");
        }
            
        if (cfg.ping_mode == WsConfig::PingMode::ClientPeriodicText) {
            if (cfg.client_ping_interval_sec <= 0) {
                throw std::invalid_argument("ClientPeriodicText requires client_ping_interval_sec > 0");
            }
                
            if (cfg.client_ping_text.empty()) {
                throw std::invalid_argument("ClientPeriodicText requires client_ping_text");
            }
                
        }
        // **必须 ≥ 1s**: 保证 socket.cancel() 触发的 op_aborted handler 按 FIFO 在
        // reconnect 的 timer 触发之前 drain 干净 → do_connect 替换 stream_ 时无 race
        if (cfg.reconnect_initial_backoff_sec < 1) {
            cfg.reconnect_initial_backoff_sec = 1;
        }

        if (cfg.reconnect_max_backoff_sec < cfg.reconnect_initial_backoff_sec) {
            cfg.reconnect_max_backoff_sec = cfg.reconnect_initial_backoff_sec;
        }
            
        return cfg;
    }

    struct ParsedUrl { 
        std::string host, 
        std::string port, 
        std::string target; 
    };

    static ParsedUrl parse_url(const std::string& url) {
        ParsedUrl r;
        std::string s = url;
        auto scheme_end = s.find("://");
        if (scheme_end != std::string::npos) {
            s = s.substr(scheme_end + 3);
        } 

        auto slash = s.find('/');
        std::string hostport = (slash == std::string::npos) ? s : s.substr(0, slash);
        r.target = (slash == std::string::npos) ? "/" : s.substr(slash);
        auto colon = hostport.find(':');

        if (colon == std::string::npos) { 
            r.host = hostport; 
            r.port = "443"; 
        }
        else { 
            r.host = hostport.substr(0, colon); 
            r.port = hostport.substr(colon + 1); 
        }

        return r;
    }

    void fire_error(const std::string& msg) {
        if (error_handler_) { 
            try { 
                error_handler_(msg); 
            } 
            catch (...) {

            } 
        }
        else {
            std::fprintf(stderr, "[ws-error] %s\n", msg.c_str());
        }
    }

    void fire_open()  { 
        if (open_handler_)  { 
            try { 
                open_handler_();  
            } 
            catch (...) {

            } 
        } 
    }

    void fire_close(int code, const std::string& reason) {
        if (close_handler_) { 
            try { 
                close_handler_(code, reason); 
            } 
            catch (...) {

            } 
        }
    }

    // ========================================================================
    // Connect 链
    //
    // **stream_ 是 shared_ptr<WsStream>**, 每个 async handler 捕获一份副本 + 在
    // 回调入口比对 `current != self->stream_` 检测重连/替换, 防止旧 handler 误操作
    // 新连接 (这就是 HFT 场景下 reconnect 时 send_text 滞后回调的并发竞争修法)。
    //
    // control_callback 例外: 它常驻在 stream 对象内, 不能捕 shared_ptr<WsStream>
    // (否则循环引用)。改用 weak_ptr<WsClient> + raw WsStream* 做身份校验。
    // ========================================================================

    void do_connect() {
        if (!running_.load(std::memory_order_acquire)) {
            return;
        }

        ParsedUrl u = parse_url(cfg_.url);
        stream_ = std::make_shared<WsStream>(ioc_, ssl_ctx_);
        WsStream* stream_raw = stream_.get();

        // control_callback: weak self + raw stream 比对身份。stream 还活着是由
        // 同时在 in-flight 的 async_read handler (持 shared_ptr<WsStream>) 保证的。
        stream_->control_callback([weak_self = weak_from_this(), stream_raw](ws::frame_type kind, beast::string_view payload) {
            auto self = weak_self.lock();
            if (!self) {  // WsClient 已死
                return;
            }   
                                            
            if (self->stream_.get() != stream_raw) { // stream 已替换
                return;      
            }

            self->on_control(kind, payload);
        });

        SSL_set_tlsext_host_name(stream_->next_layer().native_handle(), u.host.c_str());
        cur_host_ = u.host;
        cur_target_ = u.target;

        auto resolver = std::make_shared<tcp::resolver>(ioc_);
        auto current  = stream_;
        resolver->async_resolve(u.host, u.port, [self = shared_from_this(), resolver, current](beast::error_code ec, tcp::resolver::results_type eps) {
            if (current != self->stream_) {
                return;
            }

            if (ec) {
                return self->on_step_fail("resolve", ec);
            }

            self->on_resolved(eps);
        });
    }

    void on_resolved(const tcp::resolver::results_type& eps) {
        beast::get_lowest_layer(*stream_).expires_after(std::chrono::seconds(cfg_.connect_timeout_sec));
        auto current = stream_;
        beast::get_lowest_layer(*stream_).async_connect(eps, [self = shared_from_this(), current](beast::error_code ec, const tcp::endpoint&) {
            if (current != self->stream_) {
                return;
            }

            if (ec) {
                return self->on_step_fail("tcp_connect", ec);
            }

            self->on_tcp_connected();
        });
    }

    void on_tcp_connected() {
        beast::get_lowest_layer(*stream_).expires_after(std::chrono::seconds(cfg_.handshake_timeout_sec));
        auto current = stream_;
        stream_->next_layer().async_handshake(ssl::stream_base::client, [self = shared_from_this(), current](beast::error_code ec) {
            if (current != self->stream_) {
                return;
            }

            if (ec) {
                return self->on_step_fail("tls_handshake", ec);
            }

            self->on_tls_ok();
        });
    }

    void on_tls_ok() {
        ws::stream_base::timeout opt {
            std::chrono::seconds(cfg_.handshake_timeout_sec),
            std::chrono::seconds(cfg_.idle_timeout_sec > 0 ? cfg_.idle_timeout_sec : 60),
            /*keep_alive_pings=*/(cfg_.ping_mode == WsConfig::PingMode::ServerOnly)
        };
        stream_->set_option(opt);
        stream_->set_option(ws::stream_base::decorator([this](ws::request_type& req) {
            req.set(beast::http::field::user_agent, "ws/1.0");

            for (auto& kv : cfg_.headers) {
                req.set(kv.first, kv.second);
            }
        }));

        beast::get_lowest_layer(*stream_).expires_never();

        auto current = stream_;
        stream_->async_handshake(cur_host_, cur_target_, [self = shared_from_this(), current](beast::error_code ec) {
            if (current != self->stream_) {
                return;
            }

            if (ec) {
                return self->on_step_fail("ws_handshake", ec);
            }

            self->on_connected();
        });
    }

    void on_connected() {
        connected_.store(true, std::memory_order_release);
        current_backoff_ = std::chrono::seconds(cfg_.reconnect_initial_backoff_sec);
        int64_t t = now_ns();
        last_rx_ns_.store(t, std::memory_order_relaxed);
        // 给 last_data_rx_ns_ 一个连接建立时刻的初值, 否则订阅生效到第一帧之间会被误判 idle
        last_data_rx_ns_.store(t, std::memory_order_relaxed);

        // 订阅消息: cfg_.subscribe_messages 是 vector<string>, 拷一份到队列 (move 进去)
        for (const auto& m : cfg_.subscribe_messages) {
            std::string temp = m;         // 1. 显式分配/拷贝一次（因为配置必须保留，不可避免）
            queue_write(std::move(temp), /*binary=*/false);
        }

        fire_open();

        beast::error_code ec;
        if (cfg_.ping_mode == WsConfig::PingMode::ClientPeriodicText) {
            ping_timer_.cancel(ec); // 先强行重置旧定时器，防止幽灵定时器多重叠加
            schedule_client_ping();
        }
        if (cfg_.idle_timeout_sec > 0 || cfg_.data_idle_timeout_sec > 0) {
            idle_timer_.cancel(ec); // 防御幽灵 timer 叠加
            schedule_idle_check();
        }
        do_read();
    }

    // 连接链失败: 不走 on_disconnect (它检查 connected_, 在 connect 阶段 connected_=false
    // 会早退导致**永远不重连**)。这里独立处理: 关 socket + schedule_reconnect。
    void on_step_fail(const char* step, beast::error_code ec) {
        fire_error(std::string("ws step ") + step + ": " + ec.message());

        // 将物理 socket 清理逻辑安全地收拢到 io_thread 中，消除 Data Race
        auto current = stream_;
        if (current) {
            beast::error_code ignored;
            beast::get_lowest_layer(*current).socket().cancel(ignored);
            beast::get_lowest_layer(*current).socket().shutdown(asio::ip::tcp::socket::shutdown_both, ignored);
            beast::get_lowest_layer(*current).socket().close(ignored);
        }

        if (running_.load(std::memory_order_acquire) && cfg_.auto_reconnect) {
            schedule_reconnect();
        }
    }

    // ========================================================================
    // Read loop (callback inline 在 IO 线程)
    //
    // 契约: (data, len) 指向 read_buffer_ 内部 storage, callback 返回后失效。
    // ========================================================================

    void do_read() {
        read_buffer_.clear();
        auto current = stream_;
        stream_->async_read(read_buffer_, [self = shared_from_this(), current](beast::error_code ec, size_t /*n*/) {
            if (current != self->stream_) { // stream 已替换, 旧 handler 跑空
                return;   
            }

            if (ec) {
                return self->on_disconnect(ec.message());
            }

            int64_t t = now_ns();
            self->last_rx_ns_.store(t, std::memory_order_relaxed);
            // **关键**: 只在 data frame 路径刷新 last_data_rx_ns_, on_control 里不刷。
            // 这样 data_idle_timeout 能抓到"PING/PONG 在跑但行情停了"的伪健康连接。
            self->last_data_rx_ns_.store(t, std::memory_order_relaxed);
            self->total_rx_.fetch_add(1, std::memory_order_relaxed);

            if (self->msg_handler_) {
                auto buf = self->read_buffer_.data();
                self->msg_handler_(static_cast<const uint8_t*>(buf.data()), buf.size(),
                                    self->stream_->got_binary(), t);
            }
            if (!self->running_.load(std::memory_order_acquire)) {
                return;
            }

            self->do_read();
        });
    }

    // ========================================================================
    // Write queue (std::string 容器, 0 拷贝路径)
    // ========================================================================

    void queue_write(std::string data, bool is_binary) {
        if (!connected_.load(std::memory_order_acquire)) return;

        // 容量硬上限 (背压信号): 50000 帧 ≈ HFT WS 几分钟最大 burst, 实际不应触发。
        // **拒绝新帧而不是 clear()**: HFT 场景下 pending 队列里可能有订单 cancel / ping /
        // 订阅消息, 全清会让上层策略状态错乱 (user 不知道哪些发出去了)。fire_error 让
        // 上层感知背压, 决定是否降级。
        if (write_queue_.size() >= 50000) {
            fire_error("write queue overflow (>=50000 frames), dropping new frame");
            return;
        }

        write_queue_.push_back({std::move(data), is_binary});
        if (!is_writing_) drain_writes();
    }

    void drain_writes() {
        if (write_queue_.empty() || !connected_.load(std::memory_order_acquire)) {
            is_writing_ = false;
            return;
        }
        is_writing_ = true;
        auto& front = write_queue_.front();
        stream_->binary(front.is_binary);
        auto current = stream_;
        stream_->async_write(asio::buffer(front.data),
            [self = shared_from_this(), current](beast::error_code ec, size_t /*n*/) {
                if (current != self->stream_) {
                    // stream 已替换。旧 handler 不能 mutate 新连接的 is_writing_ 状态。
                    return;
                }
                if (ec) {
                    self->is_writing_ = false;
                    return self->on_disconnect(ec.message());
                }
                self->total_tx_.fetch_add(1, std::memory_order_relaxed);
                if (!self->write_queue_.empty()) {
                    self->write_queue_.pop_front();
                }

                self->drain_writes();
            });
    }

    // ========================================================================
    // Ping / 控制帧
    // ========================================================================

    void on_control(ws::frame_type kind, beast::string_view payload) {
        last_rx_ns_.store(now_ns(), std::memory_order_relaxed);
        if (kind == ws::frame_type::close) {
            fire_close(0, std::string(payload.data(), payload.size()));
        }
        // ping → beast 自动 pong; pong → 仅刷 last_rx_ns
    }

    void schedule_client_ping() {
        ping_timer_.expires_after(std::chrono::seconds(cfg_.client_ping_interval_sec));
        ping_timer_.async_wait(
            [self = shared_from_this()](beast::error_code ec) {
                if (ec) {
                    return;
                }

                if (!self->connected_.load() || !self->running_.load()) {
                    return;
                }

                self->queue_write(self->cfg_.client_ping_text, /*binary=*/false);
                self->schedule_client_ping();
            });
    }

    // 同时监控两种 idle:
    //   - any_idle  (cfg_.idle_timeout_sec):       任何 frame 都算, 抓 TCP 死的
    //   - data_idle (cfg_.data_idle_timeout_sec):  只算 data frame, 抓行情停的"伪健康"
    // 至少有一个 > 0 才会被启动。
    void schedule_idle_check() {
        // check 间隔取两个超时较小者的 1/4, 至少 1s
        int base = INT_MAX;
        if (cfg_.idle_timeout_sec > 0) {
            base = std::min(base, cfg_.idle_timeout_sec);
        }      

        if (cfg_.data_idle_timeout_sec > 0) {
            base = std::min(base, cfg_.data_idle_timeout_sec);
        }

        int check_sec = std::max(1, base == INT_MAX ? 60 : base / 4);

        idle_timer_.expires_after(std::chrono::seconds(check_sec));
        idle_timer_.async_wait(
            [self = shared_from_this()](beast::error_code ec) {
                if (ec) {
                    return;
                }

                if (!self->connected_.load() || !self->running_.load()) {
                    return;
                }

                int64_t now = now_ns();

                // 1) 连接层 idle (含 PING/PONG)
                if (self->cfg_.idle_timeout_sec > 0) {
                    int64_t any_idle = now - self->last_rx_ns_.load(std::memory_order_relaxed);
                    if (any_idle > static_cast<int64_t>(self->cfg_.idle_timeout_sec) * 1'000'000'000LL) {
                        self->on_disconnect("idle timeout (no traffic, " + std::to_string(any_idle / 1'000'000) + "ms)");
                        return;
                    }
                }
                // 2) 数据 idle (PING/PONG 不算, 抓伪健康连接)
                if (self->cfg_.data_idle_timeout_sec > 0) {
                    int64_t data_idle = now - self->last_data_rx_ns_.load(std::memory_order_relaxed);
                    if (data_idle > static_cast<int64_t>(self->cfg_.data_idle_timeout_sec) * 1'000'000'000LL) {
                        self->on_disconnect("data idle timeout (no message frames, " + std::to_string(data_idle / 1'000'000) + "ms)");
                        return;
                    }
                }
                self->schedule_idle_check();
            });
    }

    // ========================================================================
    // Disconnect / Reconnect
    // ========================================================================

    void on_disconnect(const std::string& reason) {
        if (!connected_.exchange(false)) {
            return;
        }

        fire_close(-1, reason);

        if (stream_) {
            beast::error_code ec;
            ping_timer_.cancel(ec);
            idle_timer_.cancel(ec);
            beast::get_lowest_layer(*stream_).expires_never();
            beast::get_lowest_layer(*stream_).socket().cancel(ec);
            beast::get_lowest_layer(*stream_).socket().shutdown(asio::ip::tcp::socket::shutdown_both, ec);
            beast::get_lowest_layer(*stream_).socket().close(ec);
            // stream_ 故意保留, 让 op_aborted 回调 (持 shared_ptr<WsStream>) 安全完成。
            // do_connect 会用 make_shared 替换 stream_, 旧的等 handler 释放后自然销毁。
        }
        write_queue_.clear();
        is_writing_ = false;

        if (running_.load(std::memory_order_acquire) && cfg_.auto_reconnect) {
            schedule_reconnect();
        }
    }

    void schedule_reconnect() {
        reconnect_count_.fetch_add(1, std::memory_order_relaxed);
        reconnect_timer_.expires_after(current_backoff_);
        reconnect_timer_.async_wait(
            [self = shared_from_this()](beast::error_code ec) {
                if (ec) {
                    return;
                }

                if (!self->running_.load()) {
                    return;
                }

                auto max_backoff = std::chrono::seconds(self->cfg_.reconnect_max_backoff_sec);
                self->current_backoff_ = std::min(self->current_backoff_ * 2, max_backoff);
                self->do_connect();
            });
    }

    void shutdown_inline() {
        connected_.store(false, std::memory_order_release);
        beast::error_code ec;
        reconnect_timer_.cancel(ec);
        ping_timer_.cancel(ec);
        idle_timer_.cancel(ec);
        if (stream_) {
            beast::get_lowest_layer(*stream_).expires_never();
            beast::get_lowest_layer(*stream_).socket().cancel(ec);
            beast::get_lowest_layer(*stream_).socket().shutdown(asio::ip::tcp::socket::shutdown_both, ec);
            beast::get_lowest_layer(*stream_).socket().close(ec);
        }
        write_queue_.clear();
        is_writing_ = false;
    }

private:
    WsConfig cfg_;
    asio::io_context ioc_;
    ssl::context ssl_ctx_;
    std::optional<asio::executor_work_guard<asio::io_context::executor_type>> work_guard_;
    asio::steady_timer reconnect_timer_;
    asio::steady_timer ping_timer_;
    asio::steady_timer idle_timer_;

    std::shared_ptr<WsStream> stream_;   // ★ shared_ptr 防 race
    beast::flat_buffer read_buffer_;
    std::deque<OutgoingFrame> write_queue_;
    bool is_writing_ = false;

    MessageHandler msg_handler_;
    CloseHandler close_handler_;
    OpenHandler open_handler_;
    ErrorHandler error_handler_;

    std::string cur_host_;
    std::string cur_target_;
    std::chrono::seconds current_backoff_{1};

    std::atomic<bool> running_{false};
    std::atomic<bool> connected_{false};
    std::atomic<int64_t> last_rx_ns_{0};        // 任何 frame (含 PING/PONG) 刷新
    std::atomic<int64_t> last_data_rx_ns_{0};   // 仅 data frame 刷新 (do_read 里)
    std::atomic<uint64_t> total_rx_{0};
    std::atomic<uint64_t> total_tx_{0};
    std::atomic<uint64_t> reconnect_count_{0};

    std::thread io_thread_;
};

} // namespace net
