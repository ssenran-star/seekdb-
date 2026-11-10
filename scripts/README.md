# 本地构建与自测

三个脚本都在 `scripts/` 下，可以从任意 cwd 运行（仓库根目录由脚本自身位置推导）。

## 1. 构建

```sh
./scripts/build.sh          # 构建，产物在 minivectordb/build/minivectordb
./scripts/build.sh clean    # 清理 build/ 目录
```

编译器按 `CC` 环境变量 → `cc` → `gcc` → `clang` → `python -m ziglang cc` 的顺序查找。
本机没有 gcc 时，可以用 pip 装的 ziglang 自带的 clang 前端编译：

```sh
python3 -m pip install ziglang    # 只需一次
```

有 make 时优先走 `minivectordb/Makefile`（编译选项的唯一定义处），make 不可用或失败时
退回直接编译。也可以直接用 make：

```sh
make -C minivectordb
make -C minivectordb clean
```

Windows 上产物带 `.exe` 后缀（zig cc 生成的就是 `.exe`）。

## 2. 跑公开用例

```sh
VECTORDB_BIN=/path/to/minivectordb ./scripts/run_public_tests.sh
```

不设置 `VECTORDB_BIN` 时默认使用 `minivectordb/build/minivectordb`。
其余参数透传给题包自带的 `protocol_check/run.py`，例如：

```sh
# 只测启动握手与关闭
VECTORDB_BIN=./minivectordb/build/minivectordb ./scripts/run_public_tests.sh --handshake-only

# 只跑指定用例（--case 可重复）
VECTORDB_BIN=./minivectordb/build/minivectordb ./scripts/run_public_tests.sh --case sql_crud --case storage_restart
```

脚本会新建一个临时输出目录（`result.json`、`cases.json`、每个用例的 stderr 日志），
逐个用例打印 PASS/FAIL，最后给出汇总。退出码非 0 表示有用例失败。

`--command` 模式是本地开发专用，没有容器隔离。**正式提交前必须再测一遍实际 Docker 镜像**：

```sh
docker build -t minivectordb:local .
export VECTORDB_IMAGE='minivectordb:local'
python3 protocol_check/run.py --image "$VECTORDB_IMAGE" --output "protocol-results/$(date +%Y%m%d-%H%M%S)"
```

## 3. Windows（Git Bash）

Git Bash 里路径用正斜杠，引号照常写：

```sh
cd /d/workbuddy\ log/2026-11-10-13-52-50/seekdb

./scripts/build.sh
VECTORDB_BIN="$(pwd)/minivectordb/build/minivectordb.exe" ./scripts/run_public_tests.sh
```

注意仓库路径 `workbuddy log` 含空格，路径一定要加引号。
如果 `make` 可用（MSYS2 / Git for Windows 的 MinGW 环境），`make -C minivectordb` 同样能跑。

**已知限制：题包自带的 `protocol_check/run.py` 在 Windows 上跑不起来。**
它用 `selectors` 监听子进程 stdout 管道（Windows 的 `SelectSelector` 只支持套接字），
清理进程时还调用了 `os.killpg`（Windows 上不存在），所以 `--command` 模式会报
`WinError 10038` 和 `module 'os' has no attribute 'killpg'`。
这是题包自带工具的限制；`scripts/` 不修改题包目录，因此有两个办法：

1. 用 WSL 或 Linux 跑 `run_public_tests.sh`（推荐，行为与判题环境一致）；
2. 走 Docker 镜像模式（同样不依赖上面的管道监听）：

```sh
docker build -t minivectordb:local .
python3 protocol_check/run.py --image minivectordb:local --output "protocol-results/$(date +%Y%m%d-%H%M%S)"
```

## 4. Linux

```sh
cd seekdb
./scripts/build.sh
VECTORDB_BIN="$PWD/minivectordb/build/minivectordb" ./scripts/run_public_tests.sh
```

## 5. 环境约定

- `VECTORDB_DATA_DIR` 优先于 `/data`：程序读环境变量，读不到才用 `/data`。
  容器内 Dockerfile 已把 `VECTORDB_DATA_DIR` 默认设为 `/data`。
- 容器根文件系统是只读的，只有挂载到 `/data` 的目录可写，程序不能依赖 `/tmp`。
- stdout 只能输出协议报文（每行一个 JSON 对象），日志一律写 stderr。
- 启动后必须主动输出一行 `{"type":"ready"}` 并 flush。
