#pragma once
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "common.h"
#include "hnsw.h"
#include "parser.h"
#include "storage.h"

namespace mvdb {

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
            std::vector<float> vecs;
            std::vector<int> icol_of;
            std::vector<int> vcol_of;
            std::vector<std::vector<int64_t>> icols;
            std::vector<std::vector<std::string>> vcols;
            std::vector<std::unordered_map<std::string, std::vector<int32_t>>> veq;
        };
        FastScan scan;
    };

    Table& get_table(const std::string& name);
    bool visible(uint64_t txn, uint32_t stmt) const;
    const Row* resolve_row(Table& t, int64_t pk);
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
    uint64_t cache_seen_epoch_ = 0;
    size_t cache_bytes_ = 0;
    void bump_cache() { ++cache_epoch_; }
    std::unordered_map<uint32_t, Table> tables_;
    std::unordered_map<std::string, uint32_t> by_name_;
    uint32_t next_table_id_ = 1;
    uint64_t next_txn_ = 1;
    uint64_t committed_max_ = 0;
    std::unordered_set<uint64_t> aborted_;
    uint64_t cur_txn_ = 0;
    uint32_t cur_stmt_ = 0;
    std::unordered_set<uint32_t> stmt_aborted_;
};

}  // namespace mvdb