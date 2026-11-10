#!/bin/sh
# 用题包自带的 protocol_check/run.py 跑公开用例（本地自测）。
#
# 用法：
#   VECTORDB_BIN=/path/to/minivectordb ./scripts/run_public_tests.sh
#   VECTORDB_BIN=/path/to/minivectordb ./scripts/run_public_tests.sh --case sql_crud
#   VECTORDB_BIN=/path/to/minivectordb ./scripts/run_public_tests.sh --handshake-only
#
# 未设置 VECTORDB_BIN 时，默认使用 minivectordb/build/minivectordb（Windows 为 .exe）。
# 其余参数原样透传给 protocol_check/run.py。
#
# 脚本可从任意 cwd 运行：仓库根目录由脚本自身位置推导。
set -eu
# pipefail 不是 POSIX 的一部分（dash 不支持），可用则开启，不可用则跳过。
(set -o pipefail) 2>/dev/null && set -o pipefail || true

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/.." && pwd)

die() {
    printf 'run_public_tests.sh: %s\n' "$*" >&2
    exit 1
}

# 1) 定位可执行文件
bin="${VECTORDB_BIN:-}"
if [ -z "$bin" ]; then
    for candidate in "$repo_root/minivectordb/build/minivectordb" \
                     "$repo_root/minivectordb/build/minivectordb.exe"; do
        if [ -f "$candidate" ]; then
            bin="$candidate"
            break
        fi
    done
fi
[ -n "$bin" ] || die "找不到 minivectordb 可执行文件，请先运行 scripts/build.sh，或设置 VECTORDB_BIN"
[ -f "$bin" ] || die "VECTORDB_BIN 指向的文件不存在: $bin"
# 规范成绝对路径，run.py 会以它作为子进程 argv[0]。
bin=$(CDPATH= cd -- "$(dirname -- "$bin")" && pwd)/$(basename -- "$bin")

# 2) 找 python
python_cmd=""
for candidate in python3 python; do
    if command -v "$candidate" >/dev/null 2>&1; then
        python_cmd="$candidate"
        break
    fi
done
[ -n "$python_cmd" ] || die "找不到 python3 或 python"

runner="$repo_root/protocol_check/run.py"
[ -f "$runner" ] || die "找不到题包自测工具: $runner"

# 题包自带的 protocol_check/run.py 依赖 POSIX 特性：
#   - 用 selectors 监听子进程 stdout 管道（Windows 的 SelectSelector 只支持套接字）
#   - 清理进程时调用 os.killpg（Windows 上不存在）
# 因此在 Windows 上直接跑 --command 会报 WinError 10038 / module 'os' has no attribute 'killpg'。
# 这是题包自带工具的限制，不是本脚本的问题；这里提前提示，并继续尝试执行。
case "$(uname -s 2>/dev/null || echo unknown)" in
    MINGW* | MSYS* | CYGWIN* | Windows_NT)
        printf '提示: 题包自带的 protocol_check/run.py 在 Windows 上无法运行（依赖 POSIX 的 selectors/killpg）。\n' >&2
        printf '      建议改用 WSL/Linux 执行本脚本，或直接用 Docker 镜像模式自测。\n' >&2
        ;;
esac

# 3) 输出目录：run.py 要求目录必须尚不存在，因此先建父目录再拼一个不存在的子目录。
out_root=$(mktemp -d 2>/dev/null || mktemp -d -t vectordb)
out_dir="$out_root/public-$(date +%Y%m%d-%H%M%S 2>/dev/null || echo run)"

# 4) --command 必须是 JSON 数组字符串。用 python 生成，路径里的空格与反斜杠都能正确转义。
command_json=$("$python_cmd" -c 'import json,sys; print(json.dumps([sys.argv[1]]))' "$bin")

printf '被测程序: %s\n' "$bin"
printf '输出目录: %s\n' "$out_dir"

# 5) 跑用例
status=0
"$python_cmd" "$runner" --command "$command_json" --output "$out_dir" "$@" || status=$?

# 6) 汇总（run.py 已经在 stdout 逐行打印了每个用例的 PASS/FAIL，这里再补一份汇总）
report="$out_dir/result.json"
if [ -f "$report" ]; then
    printf '\n===== 用例汇总 =====\n'
    "$python_cmd" -c '
import json, sys
with open(sys.argv[1], encoding="utf-8") as handle:
    report = json.load(handle)
cases = report.get("cases", [])
width = max([len(str(c.get("id", ""))) for c in cases] + [4])
for case in cases:
    print("%-*s  %-8s %6sms  %s" % (
        width, case.get("id", ""), case.get("status", ""),
        case.get("duration_ms", "?"), case.get("message", "")))
passed = sum(1 for c in cases if c.get("status") == "PASS")
print("通过 %d / %d，passed=%s" % (passed, len(cases), report.get("passed")))
' "$report"
    printf '报告: %s\n' "$report"
else
    printf '未生成报告文件: %s\n' "$report" >&2
fi

exit "$status"
