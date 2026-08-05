# 使用 Ubuntu 22.04 作为基础镜像
FROM ubuntu:22.04

# 设置环境变量，避免交互式提示
ENV DEBIAN_FRONTEND=noninteractive
ENV TZ=Etc/UTC

# 更新包列表并安装必要工具
RUN apt-get update && apt-get install -y \
    # 基础工具
    build-essential \
    gcc \
    g++ \
    gdb \
    clang \
    cmake \
    make \
    git \
    curl \
    wget \
    vim \
    nano \
    # 其他有用的开发工具
    pkg-config \
    valgrind \
    automake \
    autoconf \
    libtool \
    # 清理缓存
    && rm -rf /var/lib/apt/lists/*

# 设置工作目录
WORKDIR /workspace

# 验证安装
RUN g++ --version && \
    cmake --version && \
    make --version

# 设置默认命令
CMD ["/bin/bash"]