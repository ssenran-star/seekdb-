#include "engine.h"

#include <algorithm>
#include <cctype>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>

#include "simd.h"

namespace mvdb {

namespace {
bool ieq(const std::string& a, const std::string& b) { return ieq_ascii(a, b); }

int find_col(const TableSchema& s, const std::string& name) {
    for (size_t k = 0; k < s.cols.size(); ++k)
        if (ieq_ascii(s.cols[k].name, name)) return static_cast<int>(k);
    throw DbError("column_not_found", "unknown column: " + name);
}

Cell to_cell(const Value& v) {
    Cell c;
    switch (v.type) {
        case ValType::BIGINT: c.kind = Cell::INT; c.i = v.i; break;
        case ValType::VARCHAR: c.kind = Cell::STR; c.s = v.s; break;
        case ValType::VECTOR: c.kind = Cell::VEC; c.v = v.v; break;
    }
    return c;
}

bool cmp_op(const std::string& op, int ord) {
    if (op == "=") return ord == 0;
    if (op == "<") return ord < 0;
    if (op == "<=") return ord <= 0;
    if (op == ">") return ord > 0;
    if (op == ">=") return ord >= 0;
    return false;
}
}  // namespace

void Engine::start_helper() {
    if (helper_started_) return;
    helper_started_ = true;
    helper_th_ = std::thread([this] {
        for (;;) {
            std::unique_lock<std::mutex> lk(helper_mu_);
            helper_cv_.wait(lk, [&] { return helper_stop_ || helper_has_job_; });
            if (helper_stop_ && !helper_has_job_) return;
            auto fn = std::move(helper_fn_);
            helper_has_job_ = false;
            lk.unlock();
            fn();
            lk.lock();
            helper_done_ = true;
            helper_cv_.notify_one();
        }
    });
}

void Engine::run_with_helper(std::function<void()> helper, std::function<void()> mainwork) {
    start_helper();
    {
        std::lock_guard<std::mutex> lk(helper_mu_);
        helper_fn_ = std::move(helper);
        helper_has_job_ = true;
        helper_done_ = false;
    }
    helper_cv_.notify_one();
    mainwork();
    std::unique_lock<std::mutex> lk(helper_mu_);
    helper_cv_.wait(lk, [&] { return helper_done_; });
}

void Engine::open(const std::string& data_dir) {
    log_.open(data_dir);
    struct TxnInfo {
        bool data = false;
        bool commit = false;
    };
    std::unordered_map<uint64_t, TxnInfo> txns;
    log_.scan([&](const RecordView& rv) {
        next_txn_ = std::max(next_txn_, rv.txn + 1);
        ByteBuf b;
        b.data = rv.payload;
        switch (rv.type) {
            case REC_CREATE_TABLE: {
                TableSchema s;
                s.table_id = b.get_u32();
                s.name = b.get_str();
                uint32_t n = b.get_u32();
                for (uint32_t k = 0; k < n; ++k) {
                    ColumnDef c;
                    c.name = b.get_str();
                    c.type = static_cast<ValType>(b.get_u8());
                    c.len = b.get_u32();
                    s.cols.push_back(std::move(c));
                }
                s.pk_index = static_cast<int>(b.get_u32());
                s.vector_index = static_cast<int>(b.get_u32());
                Table t;
                t.schema = std::move(s);
                t.create_txn = rv.txn;
                by_name_[t.schema.name] = t.schema.table_id;
                next_table_id_ = std::max(next_table_id_, t.schema.table_id + 1);
                tables_[t.schema.table_id] = std::move(t);
                txns[rv.txn].data = true;
                break;
            }
            case REC_CREATE_INDEX: {
                uint32_t tid = b.get_u32();
                std::string iname = b.get_str();
                auto it = tables_.find(tid);
                if (it != tables_.end()) {
                    it->second.schema.ann_index_name = iname;
                    it->second.index_txn = rv.txn;
                }
                txns[rv.txn].data = true;
                break;
            }
            case REC_INSERT:
            case REC_UPDATE:
            case REC_DELETE: {
                uint32_t tid = b.get_u32();
                int64_t pk = b.get_i64();
                auto it = tables_.find(tid);
                if (it != tables_.end()) it->second.latest[pk] = rv.offset;
                txns[rv.txn].data = true;
                break;
            }
            case REC_COMMIT:
                txns[rv.txn].commit = true;
                committed_max_ = std::max(committed_max_, rv.txn);
                break;
            case REC_ABORT:
                aborted_.insert(rv.txn);
                break;
            default:
                break;
        }
    });
    for (auto& [txn, info] : txns)
        if (info.data && !info.commit) aborted_.insert(txn);
    for (auto it = tables_.begin(); it != tables_.end();) {
        Table& t = it->second;
        if (aborted_.count(t.create_txn) || t.create_txn > committed_max_) {
            by_name_.erase(t.schema.name);
            it = tables_.erase(it);
            continue;
        }
        if (!t.schema.ann_index_name.empty() &&
            (aborted_.count(t.index_txn) || t.index_txn > committed_max_))
            t.schema.ann_index_name.clear();
        ++it;
    }
    for (auto& [id, t] : tables_)
        if (!t.schema.ann_index_name.empty()) rebuild_ann(t);
    log_.drop_cache();
}

Engine::Table& Engine::get_table(const std::string& name) {
    auto it = by_name_.find(name);
    if (it == by_name_.end()) {
        for (auto& [n, id] : by_name_)
            if (ieq(n, name)) return tables_[id];
        throw DbError("table_not_found", "unknown table: " + name);
    }
    return tables_[it->second];
}

bool Engine::visible(uint64_t txn, uint32_t stmt) const {
    if (cur_txn_ != 0 && txn == cur_txn_) return stmt_aborted_.count(stmt) == 0;
    return txn <= committed_max_ && aborted_.count(txn) == 0;
}

bool Engine::load_row(Table& t, int64_t pk, Row& out) {
    auto it = t.latest.find(pk);
    if (it == t.latest.end()) return false;
    uint64_t off = it->second;
    while (off != 0) {
        RecordView rv = log_.read_at(off);
        ByteBuf b;
        b.data = rv.payload;
        b.get_u32();
        b.get_i64();
        uint64_t prev = b.get_u64();
        if (rv.type == REC_DELETE) {
            if (visible(rv.txn, rv.stmt)) return false;
            off = prev;
            continue;
        }
        if (visible(rv.txn, rv.stmt)) {
            out = b.get_row(t.schema.cols.size());
            return true;
        }
        off = prev;
    }
    return false;
}

const Row* Engine::resolve_row(Table& t, int64_t pk) {
    auto it = t.latest.find(pk);
    if (it == t.latest.end()) return nullptr;
    auto& tc = row_cache_[t.schema.table_id];
    auto cit = tc.find(pk);
    if (cit != tc.end() && cit->second.offset == it->second) return &cit->second.row;
    uint64_t head = it->second;
    uint64_t off = head;
    while (off != 0) {
        RecordView rv = log_.read_at(off);
        ByteBuf b;
        b.data = rv.payload;
        b.get_u32();
        b.get_i64();
        uint64_t prev = b.get_u64();
        if (rv.type == REC_DELETE) {
            if (visible(rv.txn, rv.stmt)) return nullptr;
            off = prev;
            continue;
        }
        if (visible(rv.txn, rv.stmt)) {
            Row parsed = b.get_row(t.schema.cols.size());
            return cache_put(t.schema.table_id, pk, head, std::move(parsed));
        }
        off = prev;
    }
    return nullptr;
}

Row* Engine::cache_put(uint32_t tid, int64_t pk, uint64_t head, Row row) {
    size_t bytes = 64;
    for (auto& v : row) bytes += v.s.size() + v.v.size() * 4 + 16;
    constexpr size_t kCacheCap = 256ull * 1024ull * 1024ull;
    if (cache_bytes_ + bytes > kCacheCap) {
        row_cache_.clear();
        cache_bytes_ = 0;
    }
    auto& slot = row_cache_[tid][pk];
    slot = CachedRow{head, std::move(row)};
    cache_bytes_ += bytes;
    return &slot.row;
}

bool Engine::row_matches(const Row& row,
                         const std::vector<std::pair<int, Predicate>>& where) {
    for (auto& [idx, p] : where) {
        const Value& v = row[idx];
        int ord = 0;
        if (v.type == ValType::BIGINT) {
            if (p.literal.kind != Literal::INT)
                throw DbError("type_mismatch", "BIGINT comparison needs integer");
            ord = (v.i > p.literal.i) - (v.i < p.literal.i);
        } else if (v.type == ValType::VARCHAR) {
            if (p.literal.kind != Literal::STRING)
                throw DbError("type_mismatch", "VARCHAR comparison needs string");
            ord = v.s.compare(p.literal.s);
            ord = (ord > 0) - (ord < 0);
        } else {
            throw DbError("unsupported_sql", "vector column in WHERE");
        }
        if (!cmp_op(p.op, ord)) return false;
    }
    return true;
}

uint64_t Engine::statement_txn(bool& is_autocommit) {
    if (cur_txn_ != 0) {
        is_autocommit = false;
        return cur_txn_;
    }
    is_autocommit = true;
    return next_txn_++;
}

void Engine::append_commit(uint64_t txn) {
    log_.append(REC_COMMIT, txn, 0, "");
    log_.sync();
    committed_max_ = std::max(committed_max_, txn);
}

void Engine::rebuild_ann(Table& t) {
    t.ann = std::make_unique<Hnsw>();
    if (t.schema.vector_index < 0) return;
    for (auto& [pk, off] : t.latest) {
        const Row* row = resolve_row(t, pk);
        if (row) t.ann->insert(pk, (*row)[t.schema.vector_index].v);
    }
}

ExecResult Engine::exec_create_table(const Statement& st) {
    bump_cache();
    if (cur_txn_ != 0) throw DbError("unsupported_sql", "DDL inside transaction");
    for (auto& [n, id] : by_name_)
        if (ieq(n, st.table)) throw DbError("table_exists", "table already exists");
    TableSchema s;
    s.table_id = next_table_id_++;
    s.name = st.table;
    s.cols = st.cols;
    int vec_count = 0;
    for (size_t k = 0; k < s.cols.size(); ++k) {
        if (ieq(s.cols[k].name, st.pk_col)) s.pk_index = static_cast<int>(k);
        if (s.cols[k].type == ValType::VECTOR) {
            s.vector_index = static_cast<int>(k);
            ++vec_count;
        }
    }
    if (vec_count > 1) throw DbError("unsupported_sql", "at most one vector column");
    uint64_t txn = next_txn_++;
    ByteBuf b;
    b.put_u32(s.table_id);
    b.put_str(s.name);
    b.put_u32(static_cast<uint32_t>(s.cols.size()));
    for (auto& c : s.cols) {
        b.put_str(c.name);
        b.put_u8(static_cast<uint8_t>(c.type));
        b.put_u32(c.len);
    }
    b.put_u32(static_cast<uint32_t>(s.pk_index));
    b.put_u32(static_cast<uint32_t>(s.vector_index));
    log_.append(REC_CREATE_TABLE, txn, 0, b.data);
    append_commit(txn);
    Table t;
    t.schema = std::move(s);
    t.create_txn = txn;
    by_name_[t.schema.name] = t.schema.table_id;
    tables_[t.schema.table_id] = std::move(t);
    return {};
}

ExecResult Engine::exec_create_index(const Statement& st) {
    bump_cache();
    if (cur_txn_ != 0) throw DbError("unsupported_sql", "DDL inside transaction");
    Table& t = get_table(st.table);
    if (t.schema.vector_index < 0 ||
        !ieq(t.schema.cols[t.schema.vector_index].name, st.column))
        throw DbError("column_not_found", "index column must be the vector column");
    if (!t.schema.ann_index_name.empty()) throw DbError("index_exists", "vector index exists");
    uint64_t txn = next_txn_++;
    ByteBuf b;
    b.put_u32(t.schema.table_id);
    b.put_str(st.index_name);
    b.put_str(st.column);
    log_.append(REC_CREATE_INDEX, txn, 0, b.data);
    append_commit(txn);
    t.schema.ann_index_name = st.index_name;
    t.index_txn = txn;
    rebuild_ann(t);
    return {};
}

ExecResult Engine::exec_insert(const Statement& st) {
    bump_cache();
    Table& t = get_table(st.table);
    const TableSchema& s = t.schema;
    std::vector<int> mapping;
    if (st.insert_cols.empty()) {
        for (size_t k = 0; k < s.cols.size(); ++k) mapping.push_back(static_cast<int>(k));
    } else {
        if (st.insert_cols.size() != s.cols.size())
            throw DbError("unsupported_sql", "partial column insert not supported");
        for (auto& name : st.insert_cols) mapping.push_back(find_col(s, name));
    }
    std::vector<Row> staged;
    std::unordered_set<int64_t> seen;
    for (auto& raw : st.rows) {
        if (raw.size() != mapping.size())
            throw DbError("type_mismatch", "INSERT value count mismatch");
        Row row(s.cols.size());
        for (size_t j = 0; j < raw.size(); ++j)
            row[mapping[j]] = coerce(raw[j], s.cols[mapping[j]]);
        int64_t pk = row[s.pk_index].i;
        if (seen.count(pk)) throw DbError("duplicate_key", "duplicate primary key");
        if (resolve_row(t, pk)) throw DbError("duplicate_key", "duplicate primary key");
        seen.insert(pk);
        staged.push_back(std::move(row));
    }
    bool is_auto = false;
    uint64_t txn = statement_txn(is_auto);
    uint32_t stmt = ++cur_stmt_;
    for (auto& row : staged) {
        int64_t pk = row[s.pk_index].i;
        auto it = t.latest.find(pk);
        uint64_t prev = it == t.latest.end() ? 0 : it->second;
        ByteBuf b;
        b.put_u32(s.table_id);
        b.put_i64(pk);
        b.put_u64(prev);
        b.put_row(row);
        uint64_t off = log_.append(REC_INSERT, txn, stmt, b.data);
        t.latest[pk] = off;
        if (t.ann && s.vector_index >= 0) t.ann->insert(pk, row[s.vector_index].v);
        cache_put(s.table_id, pk, off, std::move(row));
        if (!is_auto) txn_writes_.emplace_back(s.table_id, pk);
    }
    if (is_auto) append_commit(txn);
    ExecResult r;
    r.affected = static_cast<int64_t>(staged.size());
    return r;
}

ExecResult Engine::exec_update(const Statement& st) {
    bump_cache();
    Table& t = get_table(st.table);
    const TableSchema& s = t.schema;
    std::vector<std::pair<int, Value>> sets;
    for (auto& [name, lit] : st.sets) {
        int idx = find_col(s, name);
        if (idx == s.pk_index) throw DbError("unsupported_sql", "primary key update");
        sets.emplace_back(idx, coerce(lit, s.cols[idx]));
    }
    std::vector<std::pair<int, Predicate>> where;
    for (auto& p : st.where) where.emplace_back(find_col(s, p.column), p);
    std::vector<std::pair<int64_t, Row>> matches;
    for (auto& [pk, off] : t.latest) {
        const Row* row = resolve_row(t, pk);
        if (!row || !row_matches(*row, where)) continue;
        matches.emplace_back(pk, *row);
    }
    bool is_auto = false;
    uint64_t txn = statement_txn(is_auto);
    uint32_t stmt = ++cur_stmt_;
    for (auto& [pk, old] : matches) {
        Row row = old;
        for (auto& [idx, val] : sets) row[idx] = val;
        auto it = t.latest.find(pk);
        uint64_t prev = it == t.latest.end() ? 0 : it->second;
        ByteBuf b;
        b.put_u32(s.table_id);
        b.put_i64(pk);
        b.put_u64(prev);
        b.put_row(row);
        uint64_t off = log_.append(REC_UPDATE, txn, stmt, b.data);
        t.latest[pk] = off;
        if (t.ann && s.vector_index >= 0) {
            t.ann->erase(pk);
            t.ann->insert(pk, row[s.vector_index].v);
        }
        cache_put(s.table_id, pk, off, std::move(row));
        if (!is_auto) txn_writes_.emplace_back(s.table_id, pk);
    }
    if (is_auto) append_commit(txn);
    ExecResult r;
    r.affected = static_cast<int64_t>(matches.size());
    return r;
}

ExecResult Engine::exec_delete(const Statement& st) {
    bump_cache();
    Table& t = get_table(st.table);
    const TableSchema& s = t.schema;
    std::vector<std::pair<int, Predicate>> where;
    for (auto& p : st.where) where.emplace_back(find_col(s, p.column), p);
    std::vector<int64_t> matches;
    for (auto& [pk, off] : t.latest) {
        const Row* row = resolve_row(t, pk);
        if (!row || !row_matches(*row, where)) continue;
        matches.push_back(pk);
    }
    bool is_auto = false;
    uint64_t txn = statement_txn(is_auto);
    uint32_t stmt = ++cur_stmt_;
    for (int64_t pk : matches) {
        auto it = t.latest.find(pk);
        uint64_t prev = it == t.latest.end() ? 0 : it->second;
        ByteBuf b;
        b.put_u32(s.table_id);
        b.put_i64(pk);
        b.put_u64(prev);
        uint64_t off = log_.append(REC_DELETE, txn, stmt, b.data);
        t.latest[pk] = off;
        if (t.ann) t.ann->erase(pk);
        row_cache_[s.table_id].erase(pk);
        if (!is_auto) txn_writes_.emplace_back(s.table_id, pk);
    }
    if (is_auto) append_commit(txn);
    ExecResult r;
    r.affected = static_cast<int64_t>(matches.size());
    return r;
}

void Engine::build_fast_scan(Table& t) {
    auto& sc = t.scan;
    if (sc.epoch == cache_epoch_) return;
    int dim = t.schema.vector_index >= 0 ? static_cast<int>(t.schema.cols[t.schema.vector_index].len) : 0;
    int64_t n = static_cast<int64_t>(t.latest.size());
    sc.pks.clear(); sc.vecs.clear(); sc.rows.clear(); sc.vcols.clear();
    sc.dim = dim;
    if (n > 500000 || (dim > 0 && n * dim * 4 > 512LL * 1024 * 1024)) {
        sc.n = 0; sc.epoch = cache_epoch_;
        return;
    }
    sc.pks.reserve(n); sc.rows.reserve(n);
    if (dim > 0) sc.vecs.reserve(static_cast<size_t>(n) * dim);
    for (auto& c : t.schema.cols) {
        if (c.type == ValType::VARCHAR) sc.vcols.emplace_back();
    }
    for (auto& vc : sc.vcols) vc.reserve(n);
    sc.vidx.assign(sc.vcols.size(), {});
    for (auto& [pk, off] : t.latest) {
        const Row* row = resolve_row(t, pk);
        if (!row) continue;
        sc.pks.push_back(pk);
        if (dim > 0) {
            const auto& v = (*row)[t.schema.vector_index].v;
            sc.vecs.insert(sc.vecs.end(), v.begin(), v.end());
        }
        size_t vi = 0;
        for (size_t c = 0; c < t.schema.cols.size(); c++) {
            if (t.schema.cols[c].type == ValType::VARCHAR) {
                const std::string& sref = (*row)[c].s;
                sc.vcols[vi].push_back(sref);
                sc.vidx[vi][sref].push_back(static_cast<int32_t>(sc.pks.size() - 1));
                ++vi;
            }
        }
        sc.rows.push_back(*row);
        // vectors live in sc.vecs; don't hold a second copy per row
        if (dim > 0) {
            sc.rows.back()[t.schema.vector_index].v.clear();
            sc.rows.back()[t.schema.vector_index].v.shrink_to_fit();
        }
    }
    sc.n = static_cast<int64_t>(sc.pks.size());
    sc.epoch = cache_epoch_;
    // The scan snapshot now serves queries; free the decoded-row cache so
    // peak RSS / memcg charge stays low (memory.peak counts page cache too).
    row_cache_.clear();
    cache_bytes_ = 0;
    log_.drop_cache();
}

ExecResult Engine::exec_select(const Statement& st, const std::string& mode) {
    Table& t = get_table(st.table);
    const TableSchema& s = t.schema;
    std::vector<SelectItem> items = st.items;
    if (st.star) {
        items.clear();
        for (auto& c : s.cols) {
            SelectItem it;
            it.column = c.name;
            items.push_back(std::move(it));
        }
    }
    std::vector<int> item_col;
    std::vector<Vector> item_qvec;
    std::vector<std::string> names;
    for (auto& it : items) {
        int idx = find_col(s, it.column);
        item_col.push_back(idx);
        if (it.is_distance) {
            if (s.cols[idx].type != ValType::VECTOR)
                throw DbError("type_mismatch", "L2_DISTANCE needs vector column");
            item_qvec.push_back(parse_vector_literal(it.raw_vec, s.cols[idx].len));
            names.push_back(it.alias.empty() ? "distance" : it.alias);
        } else {
            item_qvec.push_back({});
            names.push_back(it.alias.empty() ? s.cols[idx].name : it.alias);
        }
    }
    std::vector<std::pair<int, Predicate>> where;
    for (auto& p : st.where) {
        int idx = find_col(s, p.column);
        if (s.cols[idx].type == ValType::VECTOR)
            throw DbError("unsupported_sql", "vector column in WHERE");
        where.emplace_back(idx, p);
    }
    bool order_dist = false;
    int order_col = -1;
    int order_item = -1;
    if (st.has_order) {
        for (size_t k = 0; k < items.size(); ++k) {
            if (ieq(names[k], st.order_by) || ieq(items[k].column, st.order_by)) {
                order_item = static_cast<int>(k);
                order_dist = items[k].is_distance;
                order_col = item_col[k];
                break;
            }
        }
        if (order_item < 0) {
            order_col = find_col(s, st.order_by);
            order_dist = false;
        }
        if (!order_dist && s.cols[order_col].type == ValType::VECTOR)
            throw DbError("unsupported_sql", "ORDER BY vector column");
    }

    struct OutRow {
        int64_t pk;
        std::vector<double> dists;
        std::vector<Cell> cells;
        Cell order_cell;
    };
    auto collect = [&](int64_t pk, const Row& row) {
        OutRow o;
        o.pk = pk;
        o.dists.resize(items.size(), 0.0);
        o.cells.reserve(items.size());
        for (size_t k = 0; k < items.size(); ++k) {
            if (items[k].is_distance) {
                o.dists[k] = l2_distance(row[item_col[k]].v, item_qvec[k]);
                Cell c;
                c.kind = Cell::REAL;
                c.d = o.dists[k];
                o.cells.push_back(std::move(c));
            } else {
                o.cells.push_back(to_cell(row[item_col[k]]));
            }
        }
        if (st.has_order && !order_dist) o.order_cell = to_cell(row[order_col]);
        return o;
    };

    std::vector<OutRow> out;
    bool use_ann = mode == "ann" && order_dist && st.has_limit && st.limit > 0 && t.ann;
    if (use_ann) {
        auto accept = [&](int64_t pk) {
            const Row* row = resolve_row(t, pk);
            return row && row_matches(*row, where);
        };
        int ef = static_cast<int>(std::max<int64_t>(512, 8 * st.limit));
        auto cands = t.ann->search(item_qvec[order_item], static_cast<int>(st.limit), ef, accept);
        if (static_cast<int64_t>(cands.size()) < st.limit) {
            use_ann = false;
        } else {
            for (auto& c : cands) {
                const Row* row = resolve_row(t, c.pk);
                if (row) out.push_back(collect(c.pk, *row));
            }
        }
    }
    bool fast_done = false;
    if (!use_ann) {
        build_fast_scan(t);
        auto& sc = t.scan;
        if (sc.n > 0) {
            int dim = sc.dim;
            const float* qvec = order_dist ? item_qvec[order_item].data() : nullptr;
            if (order_dist && st.has_limit && st.limit > 0 && st.limit <= 4096) {
                size_t K = static_cast<size_t>(st.limit);
                cands_.resize(K);
                size_t top_n = 0;
                auto entry_worse = [&](const Cand& a, const Cand& b) {
                    if (a.dsq != b.dsq) return st.order_desc ? (a.dsq > b.dsq) : (a.dsq < b.dsq);
                    return a.pk < b.pk;
                };
                auto better_than_top = [&](float dsq, int64_t pk) {
                    if (dsq != cands_[0].dsq)
                        return st.order_desc ? (dsq > cands_[0].dsq) : (dsq < cands_[0].dsq);
                    return pk < cands_[0].pk;
                };
                auto consider = [&](float dsq, int64_t pk, int32_t idx) {
                    if (top_n < K) {
                        cands_[top_n++] = {dsq, pk, idx};
                        if (top_n == K)
                            std::make_heap(cands_.begin(), cands_.begin() + K, entry_worse);
                    } else if (better_than_top(dsq, pk)) {
                        std::pop_heap(cands_.begin(), cands_.begin() + K, entry_worse);
                        cands_[K - 1] = {dsq, pk, idx};
                        std::push_heap(cands_.begin(), cands_.begin() + K, entry_worse);
                    }
                };
                alignas(64) float qalign[512];
                const float* quse = qvec;
                if (qvec && dim > 0 && dim <= 512) {
                    std::memcpy(qalign, qvec, static_cast<size_t>(dim) * sizeof(float));
                    quse = qalign;
                }
                // Distance to the query vector; rows that lose to the current
                // heap top are rejected by consider() after full computation.
                // Official 4-accumulator l2_sq_fast is kept for the scalar path;
                // early-abandon kernels are not reintroduced.
                auto dist_at = [&](const float* v) { return l2_sq_fast(v, quse, dim); };
                auto fill_range = [&](int64_t lo, int64_t hi, std::vector<Cand>& outh) {
                    outh.resize(K);
                    size_t nfill = 0;
                    auto worse = [&](const Cand& a, const Cand& b) {
                        if (a.dsq != b.dsq) return st.order_desc ? (a.dsq > b.dsq) : (a.dsq < b.dsq);
                        return a.pk < b.pk;
                    };
                    auto better = [&](float dsq, int64_t pk) {
                        if (dsq != outh[0].dsq)
                            return st.order_desc ? (dsq > outh[0].dsq) : (dsq < outh[0].dsq);
                        return pk < outh[0].pk;
                    };
                    auto take = [&](int64_t i, float dsq) {
                        int64_t pk = sc.pks[static_cast<size_t>(i)];
                        if (nfill < K) {
                            outh[nfill++] = {dsq, pk, static_cast<int32_t>(i)};
                            if (nfill == K)
                                std::make_heap(outh.begin(), outh.begin() + static_cast<std::ptrdiff_t>(K), worse);
                        } else if (better(dsq, pk)) {
                            std::pop_heap(outh.begin(), outh.begin() + static_cast<std::ptrdiff_t>(K), worse);
                            outh[K - 1] = {dsq, pk, static_cast<int32_t>(i)};
                            std::push_heap(outh.begin(), outh.begin() + static_cast<std::ptrdiff_t>(K), worse);
                        }
                    };
                    int64_t i = lo;
                    if (dim == 128 && !st.order_desc) {
                        const float* base = sc.vecs.data();
                        for (; i + 4 <= hi; i += 4) {
                            const float* vp = base + static_cast<size_t>(i) * 128u;
                            if (i + 20 <= hi)
                                _mm_prefetch(reinterpret_cast<const char*>(vp + 16 * 128), _MM_HINT_T0);
                            float ds[4];
                            l2_sq_fast_128x4(vp, vp + 128, vp + 256, vp + 384, quse, ds);
                            take(i, ds[0]);
                            take(i + 1, ds[1]);
                            take(i + 2, ds[2]);
                            take(i + 3, ds[3]);
                        }
                    }
                    for (; i < hi; i++) {
                        const float* vp = &sc.vecs[static_cast<size_t>(i) * static_cast<size_t>(dim)];
                        take(i, l2_sq_fast(vp, quse, dim));
                    }
                    outh.resize(nfill);
                };
                auto fill_ids = [&](const int32_t* ids, size_t lo, size_t hi, std::vector<Cand>& outh) {
                    outh.resize(K);
                    size_t nfill = 0;
                    auto worse = [&](const Cand& a, const Cand& b) {
                        if (a.dsq != b.dsq) return st.order_desc ? (a.dsq > b.dsq) : (a.dsq < b.dsq);
                        return a.pk < b.pk;
                    };
                    auto better = [&](float dsq, int64_t pk) {
                        if (dsq != outh[0].dsq)
                            return st.order_desc ? (dsq > outh[0].dsq) : (dsq < outh[0].dsq);
                        return pk < outh[0].pk;
                    };
                    for (size_t k = lo; k < hi; ++k) {
                        int64_t i = ids[k];
                        const float* vp = &sc.vecs[static_cast<size_t>(i) * static_cast<size_t>(dim)];
                        float dsq = l2_sq_fast(vp, quse, dim);
                        int64_t pk = sc.pks[static_cast<size_t>(i)];
                        if (nfill < K) {
                            outh[nfill++] = {dsq, pk, static_cast<int32_t>(i)};
                            if (nfill == K)
                                std::make_heap(outh.begin(), outh.begin() + static_cast<std::ptrdiff_t>(K), worse);
                        } else if (better(dsq, pk)) {
                            std::pop_heap(outh.begin(), outh.begin() + static_cast<std::ptrdiff_t>(K), worse);
                            outh[K - 1] = {dsq, pk, static_cast<int32_t>(i)};
                            std::push_heap(outh.begin(), outh.begin() + static_cast<std::ptrdiff_t>(K), worse);
                        }
                    }
                    outh.resize(nfill);
                };
                auto merge_parts = [&](std::vector<Cand>& left, std::vector<Cand>& right) {
                    cands_.clear();
                    cands_.insert(cands_.end(), left.begin(), left.end());
                    cands_.insert(cands_.end(), right.begin(), right.end());
                    top_n = cands_.size();
                };
                if (where.empty()) {
                    if (sc.n >= 2048 && !st.order_desc) {
                        std::vector<Cand> left, right;
                        int64_t mid = sc.n / 2;
                        run_with_helper([&] { fill_range(0, mid, left); },
                                        [&] { fill_range(mid, sc.n, right); });
                        merge_parts(left, right);
                    } else if (dim == 128 && !st.order_desc) {
                        fill_range(0, sc.n, cands_);
                        top_n = cands_.size();
                    } else {
                        for (int64_t i = 0; i < sc.n; i++) {
                            consider(dist_at(&sc.vecs[static_cast<size_t>(i) * static_cast<size_t>(dim)]),
                                     sc.pks[static_cast<size_t>(i)], static_cast<int32_t>(i));
                        }
                    }
                } else {
                    bool use_prefilter = false;
                    int pf_vi = -1;
                    std::string pf_val;
                    if (where.size() == 1 && where[0].second.op == "=") {
                        int col = where[0].first;
                        if (s.cols[col].type == ValType::VARCHAR &&
                            where[0].second.literal.kind == Literal::STRING) {
                            int vi = 0;
                            for (int c = 0; c < col; c++)
                                if (s.cols[c].type == ValType::VARCHAR) vi++;
                            if (vi < static_cast<int>(sc.vcols.size())) {
                                use_prefilter = true;
                                pf_vi = vi;
                                pf_val = where[0].second.literal.s;
                            }
                        }
                    }
                    if (use_prefilter) {
                        const auto& col_data = sc.vcols[pf_vi];
                        bool used_map = false;
                        if (pf_vi < static_cast<int>(sc.vidx.size())) {
                            auto it = sc.vidx[pf_vi].find(pf_val);
                            if (it != sc.vidx[pf_vi].end()) {
                                used_map = true;
                                const auto& ids = it->second;
                                if (ids.size() >= 2048 && !st.order_desc) {
                                    std::vector<Cand> left, right;
                                    size_t mid = ids.size() / 2;
                                    run_with_helper([&] { fill_ids(ids.data(), 0, mid, left); },
                                                    [&] { fill_ids(ids.data(), mid, ids.size(), right); });
                                    merge_parts(left, right);
                                } else {
                                    for (int32_t i : ids)
                                        consider(dist_at(&sc.vecs[static_cast<size_t>(i) * static_cast<size_t>(dim)]),
                                                 sc.pks[static_cast<size_t>(i)], i);
                                }
                            } else {
                                used_map = true;  // value absent -> no rows
                            }
                        }
                        if (!used_map) {
                            for (int64_t i = 0; i < sc.n; i++) {
                                if (col_data[static_cast<size_t>(i)] != pf_val) continue;
                                consider(dist_at(&sc.vecs[static_cast<size_t>(i) * static_cast<size_t>(dim)]),
                                         sc.pks[static_cast<size_t>(i)], static_cast<int32_t>(i));
                            }
                        }
                    } else if (sc.n >= 2048 && !st.order_desc) {
                        std::vector<int32_t> ids;
                        ids.reserve(static_cast<size_t>(sc.n));
                        for (int64_t i = 0; i < sc.n; i++) {
                            if (row_matches(sc.rows[static_cast<size_t>(i)], where))
                                ids.push_back(static_cast<int32_t>(i));
                        }
                        if (ids.size() >= 2048) {
                            std::vector<Cand> left, right;
                            size_t mid = ids.size() / 2;
                            run_with_helper([&] { fill_ids(ids.data(), 0, mid, left); },
                                            [&] { fill_ids(ids.data(), mid, ids.size(), right); });
                            merge_parts(left, right);
                        } else {
                            for (int32_t i : ids)
                                consider(dist_at(&sc.vecs[static_cast<size_t>(i) * static_cast<size_t>(dim)]),
                                         sc.pks[static_cast<size_t>(i)], i);
                        }
                    } else {
                        for (int64_t i = 0; i < sc.n; i++) {
                            if (!row_matches(sc.rows[static_cast<size_t>(i)], where)) continue;
                            consider(dist_at(&sc.vecs[static_cast<size_t>(i) * static_cast<size_t>(dim)]),
                                     sc.pks[static_cast<size_t>(i)], static_cast<int32_t>(i));
                        }
                    }
                }
                cands_.resize(top_n);
                std::sort(cands_.begin(), cands_.end(), [&](const Cand& a, const Cand& b) {
                    if (a.dsq != b.dsq) return st.order_desc ? (a.dsq > b.dsq) : (a.dsq < b.dsq);
                    return a.pk < b.pk;
                });
                // Two-way split can yield up to 2K candidates; keep only TopK
                // before the official double-precision recompute / tie re-sort.
                if (cands_.size() > K) cands_.resize(K);
            } else {
                cands_.clear();
                for (int64_t i = 0; i < sc.n; i++) {
                    if (!row_matches(sc.rows[i], where)) continue;
                    Cand c;
                    c.pk = sc.pks[i];
                    c.idx = static_cast<int32_t>(i);
                    c.dsq = order_dist ? l2_sq_fast(&sc.vecs[i * dim], qvec, dim) : 0.0f;
                    cands_.push_back(c);
                }
                auto cand_less = [&](const Cand& a, const Cand& b) {
                    if (st.has_order) {
                        int ord = 0;
                        if (order_dist) {
                            ord = (a.dsq > b.dsq) - (a.dsq < b.dsq);
                        } else if (s.cols[order_col].type == ValType::BIGINT) {
                            int64_t va = sc.rows[a.idx][order_col].i, vb = sc.rows[b.idx][order_col].i;
                            ord = (va > vb) - (va < vb);
                        } else {
                            ord = sc.rows[a.idx][order_col].s.compare(sc.rows[b.idx][order_col].s);
                            ord = (ord > 0) - (ord < 0);
                        }
                        if (st.order_desc) ord = -ord;
                        if (ord != 0) return ord < 0;
                    }
                    return a.pk < b.pk;
                };
                size_t K = st.has_limit ? static_cast<size_t>(st.limit) : cands_.size();
                if (K < cands_.size())
                    std::partial_sort(cands_.begin(), cands_.begin() + K, cands_.end(), cand_less);
                else
                    std::sort(cands_.begin(), cands_.end(), cand_less);
                if (K < cands_.size()) cands_.resize(K);
            }
            // Recompute the shortlisted distances in double and re-sort: the
            // float pass only picks candidates; ties ("距离相同按主键升序") and
            // output values need the exact sum over the stored float32 vector.
            if (order_dist && !cands_.empty()) {
                const float* q = item_qvec[order_item].data();
                std::vector<double> dsq_d(cands_.size());
                for (size_t ci = 0; ci < cands_.size(); ++ci) {
                    const float* v = &sc.vecs[static_cast<int64_t>(cands_[ci].idx) * dim];
                    double s = 0.0;
                    for (int j = 0; j < dim; ++j) {
                        double t = (double)v[j] - (double)q[j];
                        s += t * t;
                    }
                    dsq_d[ci] = s;
                }
                std::vector<size_t> perm(cands_.size());
                for (size_t ci = 0; ci < perm.size(); ++ci) perm[ci] = ci;
                std::sort(perm.begin(), perm.end(), [&](size_t a, size_t b) {
                    if (dsq_d[a] != dsq_d[b]) return st.order_desc ? (dsq_d[a] > dsq_d[b]) : (dsq_d[a] < dsq_d[b]);
                    return cands_[a].pk < cands_[b].pk;
                });
                std::vector<Cand> sorted;
                sorted.reserve(cands_.size());
                for (size_t ci : perm) sorted.push_back(cands_[ci]);
                cands_.swap(sorted);
            }
            for (auto& c : cands_) {
                const Row& row = sc.rows[c.idx];
                OutRow o;
                o.pk = c.pk;
                o.cells.reserve(items.size());
                for (size_t k = 0; k < items.size(); ++k) {
                    if (items[k].is_distance) {
                        Cell cell;
                        cell.kind = Cell::REAL;
                        const float* v = &sc.vecs[static_cast<int64_t>(c.idx) * dim];
                        const Vector& q = item_qvec[k];
                        double s = 0.0;
                        for (int j = 0; j < dim; ++j) {
                            double t = (double)v[j] - (double)q[j];
                            s += t * t;
                        }
                        cell.d = std::sqrt(s);
                        o.cells.push_back(std::move(cell));
                    } else if (item_col[k] == t.schema.vector_index && dim > 0) {
                        Cell cell;
                        cell.kind = Cell::VEC;
                        cell.v.assign(sc.vecs.begin() + static_cast<int64_t>(c.idx) * dim,
                                      sc.vecs.begin() + (static_cast<int64_t>(c.idx) + 1) * dim);
                        o.cells.push_back(std::move(cell));
                    } else {
                        o.cells.push_back(to_cell(row[item_col[k]]));
                    }
                }
                out.push_back(std::move(o));
            }
            fast_done = true;
        }
    }
    if (!use_ann && !fast_done) {
        if (order_dist && st.has_limit && st.limit > 0 && st.limit <= 4096) {
            size_t K = static_cast<size_t>(st.limit);
            cands_.resize(K);
            size_t top_n = 0;
            const Vector& qv = item_qvec[order_item];
            int dim = static_cast<int>(qv.size());
            alignas(64) float qalign[512];
            const float* quse = qv.data();
            if (dim > 0 && dim <= 512) {
                std::memcpy(qalign, qv.data(), static_cast<size_t>(dim) * sizeof(float));
                quse = qalign;
            }
            int vcol = item_col[order_item];
            auto entry_worse = [&](const Cand& a, const Cand& b) {
                if (a.dsq != b.dsq) return st.order_desc ? (a.dsq > b.dsq) : (a.dsq < b.dsq);
                return a.pk < b.pk;
            };
            auto better_than_top = [&](float dsq, int64_t pk) {
                if (dsq != cands_[0].dsq)
                    return st.order_desc ? (dsq > cands_[0].dsq) : (dsq < cands_[0].dsq);
                return pk < cands_[0].pk;
            };
            std::vector<std::pair<uint64_t, int64_t>> recs;
            recs.reserve(t.latest.size());
            for (auto& [pk, off] : t.latest) recs.emplace_back(off, pk);
            std::sort(recs.begin(), recs.end());
            Row row;
            uint64_t last_drop = 0;
            constexpr uint64_t kDropEvery = 8ull << 20;
            for (auto& [off, pk] : recs) {
                if (!load_row(t, pk, row) || !row_matches(row, where)) continue;
                const auto& vec = row[static_cast<size_t>(vcol)].v;
                float dsq = l2_sq_fast(vec.data(), quse, dim);
                if (top_n < K) {
                    cands_[top_n++] = {dsq, pk, 0};
                    if (top_n == K)
                        std::make_heap(cands_.begin(), cands_.begin() + static_cast<std::ptrdiff_t>(K), entry_worse);
                } else if (better_than_top(dsq, pk)) {
                    std::pop_heap(cands_.begin(), cands_.begin() + static_cast<std::ptrdiff_t>(K), entry_worse);
                    cands_[K - 1] = {dsq, pk, 0};
                    std::push_heap(cands_.begin(), cands_.begin() + static_cast<std::ptrdiff_t>(K), entry_worse);
                }
                if (off - last_drop >= kDropEvery) {
                    log_.drop_upto(off);
                    last_drop = off;
                }
            }
            log_.drop_cache();
            cands_.resize(top_n);
            std::sort(cands_.begin(), cands_.end(), [&](const Cand& a, const Cand& b) {
                if (a.dsq != b.dsq) return st.order_desc ? (a.dsq > b.dsq) : (a.dsq < b.dsq);
                return a.pk < b.pk;
            });
            if (cands_.size() > K) cands_.resize(K);
            for (auto& c : cands_) {
                const Row* rp = resolve_row(t, c.pk);
                if (rp) out.push_back(collect(c.pk, *rp));
            }
            // collect() already uses double L2; re-sort so ties follow
            // "equal distance -> smaller pk first" on the exact values.
            std::sort(out.begin(), out.end(), [&](const OutRow& a, const OutRow& b) {
                double da = a.dists[order_item], db = b.dists[order_item];
                if (da != db) return st.order_desc ? (da > db) : (da < db);
                return a.pk < b.pk;
            });
        } else {
            for (auto& [pk, off] : t.latest) {
                const Row* row = resolve_row(t, pk);
                if (!row || !row_matches(*row, where)) continue;
                out.push_back(collect(pk, *row));
            }
            std::sort(out.begin(), out.end(), [&](const OutRow& a, const OutRow& b) {
                if (st.has_order) {
                    int ord = 0;
                    if (order_dist) {
                        double da = a.dists[order_item], db = b.dists[order_item];
                        ord = (da > db) - (da < db);
                    } else if (a.order_cell.kind == Cell::INT) {
                        ord = (a.order_cell.i > b.order_cell.i) - (a.order_cell.i < b.order_cell.i);
                    } else {
                        ord = a.order_cell.s.compare(b.order_cell.s);
                        ord = (ord > 0) - (ord < 0);
                    }
                    if (st.order_desc) ord = -ord;
                    if (ord != 0) return ord < 0;
                }
                return a.pk < b.pk;
            });
            if (st.has_limit && static_cast<int64_t>(out.size()) > st.limit)
                out.resize(static_cast<size_t>(st.limit));
        }
    }

    ExecResult r;
    r.is_query = true;
    r.columns = names;
    for (auto& o : out) r.rows.push_back(std::move(o.cells));
    return r;
}

ExecResult Engine::execute(const std::string& sql, const std::string& search_mode) {
    Statement st = parse_sql(sql);
    switch (st.type) {
        case StmtType::BEGIN:
            bump_cache();
            if (cur_txn_ != 0) throw DbError("transaction_active", "transaction already active");
            cur_txn_ = next_txn_++;
            cur_stmt_ = 0;
            stmt_aborted_.clear();
            txn_writes_.clear();
            return {};
        case StmtType::COMMIT:
            bump_cache();
            if (cur_txn_ == 0) throw DbError("no_transaction", "no active transaction");
            append_commit(cur_txn_);
            cur_txn_ = 0;
            stmt_aborted_.clear();
            txn_writes_.clear();
            return {};
        case StmtType::ROLLBACK:
            bump_cache();
            if (cur_txn_ == 0) throw DbError("no_transaction", "no active transaction");
            log_.append(REC_ABORT, cur_txn_, 0, "");
            aborted_.insert(cur_txn_);
            for (auto& [tid, pk] : txn_writes_) row_cache_[tid].erase(pk);
            txn_writes_.clear();
            cur_txn_ = 0;
            stmt_aborted_.clear();
            return {};
        case StmtType::CREATE_TABLE: return exec_create_table(st);
        case StmtType::CREATE_INDEX: return exec_create_index(st);
        case StmtType::INSERT: return exec_insert(st);
        case StmtType::UPDATE: return exec_update(st);
        case StmtType::DELETE_ROWS: return exec_delete(st);
        case StmtType::SELECT: return exec_select(st, search_mode);
    }
    throw DbError("internal_error", "unhandled statement");
}

void Engine::shutdown() {
    if (helper_started_) {
        {
            std::lock_guard<std::mutex> lk(helper_mu_);
            helper_stop_ = true;
        }
        helper_cv_.notify_one();
        if (helper_th_.joinable()) helper_th_.join();
        helper_started_ = false;
    }
    if (cur_txn_ != 0) {
        log_.append(REC_ABORT, cur_txn_, 0, "");
        aborted_.insert(cur_txn_);
        for (auto& [tid, pk] : txn_writes_) row_cache_[tid].erase(pk);
        txn_writes_.clear();
        cur_txn_ = 0;
    }
    log_.sync();
    log_.close();
}

}  // namespace mvdb