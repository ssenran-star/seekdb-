#!/bin/sh
# MiniVectorDB 本地构建脚本。
#
# 用法：
#   ./scripts/build.sh            # 构建
#   ./scripts/build.sh clean      # 清理产物
#
# 编译器查找顺序：$CC -> cc -> gcc -> clang -> python -m ziglang cc
# 产物：minivectordb/build/minivectordb（Windows 上为 minivectordb.exe）
#
# 脚本可从任意 cwd 运行：仓库根目录由脚本自身位置推导。
set -eu
# pipefail 不是 POSIX 的一部分（dash 不支持），可用则开启，不可用则跳过。
(set -o pipefail) 2>/dev/null && set -o pipefail || true

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/.." && pwd)
src_dir="$repo_root/minivectordb"
build_dir="$src_dir/build"
obj_dir="$build_dir/obj"

# 与 minivectordb/Makefile 保持一致：
#   -std=c11 题目要求 C11；-O2 优化；-Wall -Wextra 打开告警但不加 -Werror。
#   -std=c11 会隐藏 POSIX 声明，必须显式开启特性宏。
# 注意：这里只放“自身不含空格”的编译选项，可以按词展开；
# 含空格的头文件路径在下面单独用引号传参。
cflags="-std=c11 -O2 -Wall -Wextra -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -D_FILE_OFFSET_BITS=64"

die() {
    printf 'build.sh: %s\n' "$*" >&2
    exit 1
}

case "$(uname -s 2>/dev/null || echo unknown)" in
    MINGW* | MSYS* | CYGWIN* | Windows_NT) exe_suffix=".exe" ;;
    *) exe_suffix="" ;;
esac
target="$build_dir/minivectordb$exe_suffix"

if [ "${1:-build}" = "clean" ]; then
    rm -rf "$build_dir"
    printf '已清理: %s\n' "$build_dir"
    exit 0
fi

[ -d "$src_dir/src" ] || die "找不到源码目录: $src_dir/src"

# 1) 选编译器
cc_cmd=""
if [ -n "${CC:-}" ]; then
    cc_cmd="$CC"
else
    for candidate in cc gcc clang; do
        if command -v "$candidate" >/dev/null 2>&1; then
            cc_cmd="$candidate"
            break
        fi
    done
fi
if [ -z "$cc_cmd" ]; then
    # 本机没有 gcc 时，用 pip 安装的 ziglang 自带 clang 前端编译 C 代码。
    for py in python3 python; do
        if command -v "$py" >/dev/null 2>&1 && "$py" -m ziglang version >/dev/null 2>&1; then
            cc_cmd="$py -m ziglang cc"
            break
        fi
    done
fi
[ -n "$cc_cmd" ] || die "找不到 C 编译器（cc/gcc/clang），也没有可用的 python -m ziglang cc"

printf '编译器: %s\n' "$cc_cmd"

# 2) 有 make 时优先走 Makefile（编译选项的唯一定义处），失败再退回直接编译。
built=0
if command -v make >/dev/null 2>&1; then
    printf '使用 make 构建...\n'
    if make -C "$src_dir" CC="$cc_cmd" all; then
        built=1
    else
        printf 'make 构建失败，改用直接编译...\n' >&2
    fi
fi

if [ "$built" -eq 0 ]; then
    mkdir -p "$obj_dir"
    # $cflags 里的每个 token 自身都不含空格，按词展开是正确的；
    # 但头文件路径可能含空格（仓库路径 "workbuddy log"），
    # 所以必须作为单独一个带引号的参数传，不能混进字符串里展开。
    # 目标文件列表用位置参数保存，保证含空格的路径不会被拆开。
    set --
    for source_file in "$src_dir"/src/*.c; do
        [ -f "$source_file" ] || continue
        obj_file="$obj_dir/$(basename -- "$source_file" .c).o"
        # shellcheck disable=SC2086
        $cc_cmd $cflags "-I$src_dir/include" -c -o "$obj_file" "$source_file"
        set -- "$@" "$obj_file"
    done
    [ "$#" -gt 0 ] || die "在 $src_dir/src 下没有找到 .c 源文件"
    # shellcheck disable=SC2086
    $cc_cmd $cflags -o "$target" "$@" -lm
fi

[ -f "$target" ] || die "构建结束但没有找到产物: $target"
printf '构建完成: %s\n' "$target"
