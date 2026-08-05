# =============================================================================
# hft 开发+构建镜像 在hft目录下
#
# 用法:
#   docker build -t hft_dev -f include/hft.dockfile .
#   docker run -itd --name hft_dev_img1 -v .:/workspace hft_dev
#   cd /workspace/<subproject> && mkdir -p build && cd build && cmake .. && make -j
#
# =============================================================================
FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive
ENV TZ=Etc/UTC

# =============================================================================
# 1) 基础工具 + apt 库
# =============================================================================
RUN apt-get update && apt-get install -y --no-install-recommends \
        # -------- 编译 / 调试工具 --------
        build-essential gcc g++ gdb clang \
        cmake make ninja-build \
        git curl wget ca-certificates \
        vim nano less \
        pkg-config valgrind strace \
        automake autoconf libtool \
        # -------- 必需的 C++ 库 (**核心, 生产也用**) --------
        libssl-dev              \
        # OpenSSL: Ed25519 (Binance WS)、HMAC-SHA256/512 (Gate/OKX/Bybit)、TLS 握手
        # 依赖: BeastRestClient / BeastWsClient / ed25519_signer.h
        libboost-all-dev        \
        # boost::beast (HTTP + WebSocket) + asio (event loop) + system + lockfree
        # 全项目 hot path 都基于它
        libtbb-dev              \
        # oneapi::tbb::concurrent_unordered_map
        # 依赖: SecurityManager, OrderManager
        # TODO 后续可迁 ankerl::unordered_dense 后砍
        redis-server            \
        # contractinfo 币对信息 Redis 写入 (USE_INFO_SHM 未开时的默认路径)
        # -------- 过渡期依赖, 后续代码清理后可移除 --------
    && rm -rf /var/lib/apt/lists/*

# =============================================================================
# 2) 需要从源码构建的库 (apt 版本太旧或不存在)
# =============================================================================

WORKDIR /opt

# ---- fmt (格式化, 强依赖) ----
# Ubuntu 22.04 apt 有 libfmt-dev v8.1, 也能用; 但项目 install.txt 是源码方式, 保持一致。
# 用 10.2.1 (LTS 稳定版), 头文件 + 静态库都装到 /usr/local。
RUN git clone --depth 1 --branch 10.2.1 https://github.com/fmtlib/fmt.git && \
    cd fmt && mkdir build && cd build && \
    cmake -DFMT_TEST=OFF -DFMT_DOC=OFF \
          -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
          -DBUILD_SHARED_LIBS=OFF \
          -DCMAKE_INSTALL_PREFIX=/usr/local .. && \
    make -j$(nproc) install && \
    cd /opt && rm -rf fmt

# ---- simdjson (JSON 解析, **必须 3.x** 才有 ondemand rewind + find_field_unordered) ----
# Ubuntu 22.04 apt 是 0.9, 太老不能用; 必须源码。
RUN git clone --depth 1 --branch v3.9.4 https://github.com/simdjson/simdjson.git && \
    cd simdjson && mkdir build && cd build && \
    cmake -DSIMDJSON_JUST_LIBRARY=ON \
          -DSIMDJSON_DEVELOPER_MODE=OFF \
          -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
          -DCMAKE_INSTALL_PREFIX=/usr/local .. && \
    make -j$(nproc) install && \
    cd /opt && rm -rf simdjson

# ---- moodycamel concurrentqueue (无锁 SPSC/MPMC 队列, header-only) ----
RUN git clone --depth 1 https://github.com/cameron314/concurrentqueue.git /opt/concurrentqueue && \
    cp /opt/concurrentqueue/*.h /usr/local/include/ && \
    # 也保留 /opt/concurrentqueue 给项目 CMake include_directories 用
    :

# ---- fmtlog (低延迟异步日志) ----
# header + 单个 .cc, 项目 CMakeLists 直接指向源码目录, 不装 system。
RUN git clone --depth 1 https://github.com/MengRao/fmtlog.git /opt/fmtlog

# ---- cpp_redis (Redis 客户端, contractinfo 写入 + SecurityManager 读取) ----
RUN git clone --depth 1 --recurse-submodules https://github.com/cpp-redis/cpp_redis.git && \
    cd cpp_redis && mkdir build && cd build && \
    cmake -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
          -DCMAKE_INSTALL_PREFIX=/usr/local .. && \
    make -j$(nproc) install && \
    cd /opt && rm -rf cpp_redis

# ---- ldconfig 更新动态链接器缓存 ----
RUN ldconfig

# =============================================================================
# 3) 已从 install.txt 移除 (不再需要)
# =============================================================================
# ❌ libnanomsg-dev       —— 只有 concurrent_queue.h 里的 ConcurrentQueueNN 用, 已被
#                            ConcurrentQueueZMQ 取代 (若确认不用可连类一起删)
# ❌ libmysql++-dev       —— 全库 C++ 代码零 mysql include
# ❌ mysql-server         —— 同上, 只 utrade_hft/script/*.py 用, 应该走应用外的 MySQL
# ❌ libuv-dev            —— 全库零使用
# ❌ NanoLogLite          —— 全库零使用 (fmtlog 覆盖了同类功能)
# ❌ libzmq3-dev / cppzmq —— concurrent_queue.h 里的 ConcurrentQueueZMQ 定义存在但
#                            检查项目里没实际实例化路径; 需要时再补回
#                            (若要用: apt install libzmq3-dev + 源码 cppzmq)

# =============================================================================
# 4) 工作目录 + 验证
# =============================================================================
WORKDIR /workspace

RUN g++ --version && \
    cmake --version && \
    echo "--- library sanity check ---" && \
    ls /usr/local/lib/libfmt.a  || (echo "MISSING: fmt"       && exit 1) && \
    ls /usr/local/lib/libsimdjson.a || (echo "MISSING: simdjson" && exit 1) && \
    ls /usr/include/boost/beast.hpp || (echo "MISSING: boost::beast" && exit 1) && \
    ls /usr/include/openssl/hmac.h  || (echo "MISSING: openssl-dev"  && exit 1) && \
    ls /usr/include/rapidjson/document.h || (echo "MISSING: rapidjson-dev" && exit 1) && \
    ls /usr/include/cpprest/http_client.h || (echo "MISSING: cpprest-dev" && exit 1) && \
    ls /usr/include/tbb/concurrent_unordered_map.h || (echo "MISSING: libtbb-dev" && exit 1) && \
    ls /opt/concurrentqueue/concurrentqueue.h || (echo "MISSING: concurrentqueue" && exit 1) && \
    ls /opt/fmtlog/fmtlog.h                    || (echo "MISSING: fmtlog" && exit 1) && \
    echo "--- all deps installed ---"

CMD ["/bin/bash"]