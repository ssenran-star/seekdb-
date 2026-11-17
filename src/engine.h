#pragma once
#include <condition_variable>
#include <cstddef>
#include <cstdlib>
#include <functional>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "common.h"
#include "hnsw.h"
#include "parser.h"
#include "storage.h"

namespace mvdb {

template <typename T, std::size_t Align = 64>
struct AlignAlloc {
    using value_type = T;
    using size_type = std::size_t;
    using difference_type = std::ptrdiff_t;
    using propagate_on_container_move_assignment = std::true_type;
    using is_always_equal = std::true_type;
    template <typename U> struct rebind { using other = AlignAlloc<U, Align>; };
    AlignAlloc() noexcept = default;
    template <typename U> AlignAlloc(const AlignAlloc<U, Align>&) noexcept {}
    T* allocate(std::size_t n) {
        if (n == 0) return nullptr;
        void* p = nullptr;
        if (::posix_memalign(&p, Align, n * sizeof(T)) != 0) throw std::bad_alloc();
        return static_cast<T*>(p);
    }
    void deallocate(T* p, std::size_t) noexcept { std::free(p); }
};
template <typename T, typename U, std::size_t A>
inline bool operator==(const AlignAlloc<T, A>&, const AlignAlloc<U, A>&) noexcept { return true; }
template <typename T, typename U, std::size_t A>
inline bool operator!=(const AlignAlloc<T, A>&, const AlignAlloc<U, A>&) noexcept { return false; }

struct Cell {
    enum Kind { INT, STR, VEC, REAL } kind = INT;
    int64_t i = 0;
    std::string s;
    Vector v;
    double d = 0.0;
};

struct ExecResult {
    bool is_query = false;
    std::vector<std::string> columns;
    std::vector<std::vector<Cell>> rows;
    int64_t affected = 0;
};

class Engine {
public:
    void open(const std::string& data_dir);
    ExecResult execute(const std::string& sql, const std::string& search_mode);
    void shutdown();

private:
    struct Table {
        TableSchema schema;
        std::unordered_map<int64_t, uint64_t> latest;
        std::unique_ptr<Hnsw> ann;
        uint64_t create_txn = 0;
        uint64_t index_txn = 0;
        struct FastScan {
            uint64_t epoch = 0;
            int64_t n = 0;
            int dim = 0;
            std::vector<int64_t> pks;
            std::vector<float, AlignAlloc<float, 64>> vecs;
            std::vector<Row> rows;
            std::vector<std::vector<std::string>> vcols;
            // per VARCHAR column: value -> matching row positions (vcols order)
            std::vector<std::unordered_map<std::string, std::vector<int32_t>>> vidx;
        };
        FastScan scan;
    };

    Table& get_table(const std::string& name);
    bool visible(uint64_t txn, uint32_t stmt) const;
    const Row* resolve_row(Table& t, int64_t pk);
    bool load_row(Table& t, int64_t pk, Row& out);
    bool row_matches(const Row& row,
                     const std::vector<std::pair<int, Predicate>>& where);
    void append_commit(uint64_t txn);
    uint64_t statement_txn(bool& is_autocommit);
    void rebuild_ann(Table& t);
    void build_fast_scan(Table& t);
    struct Cand { float dsq; int64_t pk; int32_t idx; };
    std::vector<Cand> cands_;

    ExecResult exec_create_table(const Statement& st);
    ExecResult exec_create_index(const Statement& st);
    ExecResult exec_insert(const Statement& st);
    ExecResult exec_update(const Statement& st);
    ExecResult exec_delete(const Statement& st);
    ExecResult exec_select(const Statement& st, const std::string& mode);

    Log log_;
    struct CachedRow {
        uint64_t offset = 0;
        Row row;
    };
    std::unordered_map<uint32_t, std::unordered_map<int64_t, CachedRow>> row_cache_;
    uint64_t cache_epoch_ = 0;
    size_t cache_bytes_ = 0;
    void bump_cache() { ++cache_epoch_; }
    Row* cache_put(uint32_t tid, int64_t pk, uint64_t head, Row row);
    std::vector<std::pair<uint32_t, int64_t>> txn_writes_;
    std::unordered_map<uint32_t, Table> tables_;
    std::unordered_map<std::string, uint32_t> by_name_;
    uint32_t next_table_id_ = 1;
    uint64_t next_txn_ = 1;
    uint64_t committed_max_ = 0;
    std::unordered_set<uint64_t> aborted_;
    uint64_t cur_txn_ = 0;
    uint32_t cur_stmt_ = 0;
    std::unordered_set<uint32_t> stmt_aborted_;

    void start_helper();
    void run_with_helper(std::function<void()> helper, std::function<void()> mainwork);
    std::thread helper_th_;
    std::mutex helper_mu_;
    std::condition_variable helper_cv_;
    std::function<void()> helper_fn_;
    bool helper_has_job_ = false;
    bool helper_done_ = true;
    bool helper_stop_ = false;
    bool helper_started_ = false;
};

}  // namespace mvdb
