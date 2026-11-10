# MiniVectorDB 竞赛镜像：多阶段构建。
# 构建阶段把编译器和依赖全部装进镜像层，运行阶段不联网、不装任何运行时依赖。
#
# 基础镜像选择说明：
#   构建阶段用 gcc:13（官方镜像，基于 Debian bookworm，自带 gcc/g++/make）。
#   没有选 "debian:bookworm + apt install g++" 的原因：
#     1) 版本锁定更硬：镜像 tag 固定 GCC 主版本 13，构建产物可复现；
#     2) Dockerfile 内不需要任何 apt 源操作，严格满足“正式执行时禁止联网”；
#     3) gcc:13 与运行阶段 debian:bookworm-slim 同为 bookworm，glibc 2.36 完全一致，
#        动态链接的二进制可以直接在运行阶段加载。
#   运行阶段用 debian:bookworm-slim，不用 alpine：
#     alpine 是 musl libc，与构建阶段产出的 glibc 动态链接二进制不兼容
#     （运行时会报 "not found" / loader error）。若将来要换 alpine，
#     构建阶段必须同步换成 gcc:13-alpine（musl），两者不能混用。

FROM gcc:13 AS builder

WORKDIR /src/minivectordb

# 只拷贝源码与头文件；仓库根目录其它内容（题包、CI 配置等）不进入构建产物。
COPY minivectordb/include/ ./include/
COPY minivectordb/src/ ./src/

# 产物写到 /out，运行阶段只 COPY 这一个文件。
# 编译选项与 minivectordb/Makefile 保持一致：
#   -std=c11                   题目要求 C11
#   -O3                        优化（与本地验证版本一致：4 路累加器距离依赖 -O3）
#   -Wall -Wextra              打开告警，但不加 -Werror，避免小问题直接中断构建
#   -D_POSIX_C_SOURCE=200809L / -D_DEFAULT_SOURCE
#                              -std=c11 会隐藏 POSIX 声明（fsync/mkdir/ftruncate/fileno 等），
#                              不显式开启的话实现方代码会出现隐式声明告警甚至报错。
#   -I include                 头文件搜索路径
RUN set -eux; \
    mkdir -p /out; \
    gcc -std=c11 -O3 -Wall -Wextra \
        -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -D_FILE_OFFSET_BITS=64 \
        -I include \
        src/*.c \
        -o /out/minivectordb \
        -lm; \
    chmod 0755 /out/minivectordb

FROM debian:bookworm-slim AS runtime

LABEL org.opencontainers.image.title="minivectordb" \
      org.opencontainers.image.description="MiniVectorDB stdio JSON-lines server"

# 数据目录：判题器会把一个可写目录挂载到 /data，且容器根文件系统是 --read-only，
# 因此程序只能往 /data（或 VECTORDB_DATA_DIR 指定的目录）写，不能依赖 /tmp 或其它路径。
RUN mkdir -p /data
WORKDIR /data

COPY --from=builder /out/minivectordb /usr/local/bin/minivectordb

# 数据目录优先级：VECTORDB_DATA_DIR 环境变量 > /data。
# 这里的取值只是容器内默认值；判题器若注入该环境变量，以环境变量为准。
ENV VECTORDB_DATA_DIR=/data
# 不设置 LANG/LC_ALL：协议报文是 UTF-8 字节流，程序按字节处理即可，
# 不依赖 locale。默认的 C locale 一定存在，避免 setlocale 失败的风险。

# exec 形式，信号能直接送到数据库进程（崩溃测试会直接杀进程）；
# 不使用 CMD，不带任何默认参数，不监听网络端口：
# 判题器通过 stdin/stdout 的 JSON Lines 协议交互。
ENTRYPOINT ["/usr/local/bin/minivectordb"]
