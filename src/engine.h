#pragma once
#include <condition_variable>
#include <cstddef>
#include <cstdlib>
#include <functional>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <string_view>
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

// Compact PK -> WAL-offset map. Sequential / dense BIGINT keys (the
// 10k and 600k probes, and typical official loads) live in a flat
// vector at 8 bytes/row instead of an unordered_map node (~40-48B plus
// buckets). Sparse or huge-gap keys fall back to the hash map.
class LatestMap {
public:
    bool get(int64_t pk, uint64_t& off) const {
        if (has_base_) {
            const int64_t idx = pk - base_;
            if (idx >= 0 && idx < static_cast<int64_t>(slots_.size())) {
                const uint64_t v = slots_[static_cast<size_t>(idx)];
                if (v != 0) {
                    off = v;
                    return true;
                }
            }
        }
        auto it = overflow_.find(pk);
        if (it == overflow_.end()) return false;
        off = it->second;
        return true;
    }

    void set(int64_t pk, uint64_t off) {
        if (try_dense(pk)) {
            uint64_t& slot = slots_[static_cast<size_t>(pk - base_)];
            if (slot == 0) {
                auto oit = overflow_.find(pk);
                if (oit != overflow_.end()) overflow_.erase(oit);
                else ++n_;
            }
            slot = off;
            return;
        }
        auto [it, inserted] = overflow_.emplace(pk, off);
        if (inserted) ++n_;
        else it->second = off;
    }


    void erase(int64_t pk) {
        if (has_base_) {
            const int64_t idx = pk - base_;
            if (idx >= 0 && idx < static_cast<int64_t>(slots_.size()) &&
                slots_[static_cast<size_t>(idx)] != 0) {
                slots_[static_cast<size_t>(idx)] = 0;
                --n_;
            }
        }
        auto it = overflow_.find(pk);
        if (it != overflow_.end()) {
            overflow_.erase(it);
            --n_;
        }
    }

    size_t size() const { return n_; }
    bool empty() const { return n_ == 0; }
    void clear() {
        slots_.clear();
        overflow_.clear();
        n_ = 0;
        has_base_ = false;
        base_ = 0;
    }

    template <typename Fn>
    void for_each(Fn&& fn) const {
        if (has_base_) {
            for (size_t i = 0; i < slots_.size(); ++i) {
                if (slots_[i] != 0)
                    fn(base_ + static_cast<int64_t>(i), slots_[i]);
            }
        }
        for (const auto& kv : overflow_) fn(kv.first, kv.second);
    }

private:
    static constexpr int64_t kMaxSlots = 4ll << 20;
    static constexpr int64_t kMaxExtend = 4096;

    bool try_dense(int64_t pk) {
        if (!has_base_) {
            has_base_ = true;
            base_ = pk;
            slots_.assign(1, 0);
            return true;
        }
        const int64_t idx = pk - base_;
        if (idx >= 0 && idx < static_cast<int64_t>(slots_.size())) return true;
        if (idx >= 0) {
            const int64_t need = idx + 1;
            const int64_t grow = need - static_cast<int64_t>(slots_.size());
            if (need <= kMaxSlots && grow <= (kMaxExtend > static_cast<int64_t>(slots_.size()) ? kMaxExtend : static_cast<int64_t>(slots_.size()))) {
                slots_.resize(static_cast<size_t>(need), 0);
                return true;
            }
            return false;
        }
        // Extend left: pk is below base_.
        const int64_t grow = -idx;
        const int64_t need = static_cast<int64_t>(slots_.size()) + grow;
        if (need <= kMaxSlots && grow <= (kMaxExtend > static_cast<int64_t>(slots_.size()) ? kMaxExtend : static_cast<int64_t>(slots_.size()))) {
            slots_.insert(slots_.begin(), static_cast<size_t>(grow), 0);
            base_ = pk;
            return true;
        }
        return false;
    }

    std::vector<uint64_t> slots_;
    std::unordered_map<int64_t, uint64_t> overflow_;
    int64_t base_ = 0;
    size_t n_ = 0;
    bool has_base_ = false;
};

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
        LatestMap latest;
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
        std::unique_ptr<PackedScan> packed;
        uint64_t packed_epoch = 0;
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
    bool decode_visible_scan(Table& t, int64_t pk,
                             const std::vector<ValType>& mid_types,
                             const std::vector<int>& mid_cols,
                             int64_t* mid_i, std::string_view* mid_s,
                             const float*& vec, int& dim);
    void ensure_packed_scan(Table& t);
    void populate_packed_from_latest(Table& t, std::vector<std::pair<int64_t, uint64_t>>* snap_heads);
    void apply_snap_heads(Table& t, const std::vector<std::pair<int64_t, uint64_t>>& snap_heads);
    void promote_latest_to_snap(Table& t);
    void maybe_incremental_packed(Table& t, const std::vector<Row>* new_rows);
    void abort_incremental_packed(Table& t);
    void maybe_incremental_after_txn();
    void maybe_rotate_wal(bool force = false);
    void load_packed_as_latest(Table& t);
    bool row_from_packed(Table& t, uint64_t snap_off, Row& out);
    struct Cand { float dsq; int64_t pk; int32_t idx; };
    std::vector<Cand> cands_;

    ExecResult exec_create_table(const Statement& st);
    ExecResult exec_create_index(const Statement& st);
    ExecResult exec_insert(const Statement& st);
    ExecResult exec_update(const Statement& st);
    ExecResult exec_delete(const Statement& st);
    ExecResult exec_select(const Statement& st, const std::string& mode);

    std::string data_dir_;
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
    void release_insert_cache();
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
