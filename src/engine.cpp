#include "engine.h"

#include <algorithm>
#include <cctype>
#include <condition_variable>
#include <cstring>
#include <functional>
#if defined(__linux__)
#include <malloc.h>
#endif
#include <mutex>
#include <queue>
#include <string_view>
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

void mid_layout(const TableSchema& s, std::vector<ValType>& types, std::vector<int>& cols) {
    types.clear();
    cols.clear();
    for (size_t i = 0; i < s.cols.size(); ++i) {
        if (static_cast<int>(i) == s.pk_index || static_cast<int>(i) == s.vector_index) continue;
        if (s.cols[i].type == ValType::VECTOR) continue;
        types.push_back(s.cols[i].type);
        cols.push_back(static_cast<int>(i));
    }
}

bool pk_where_ok(int64_t pk, int pk_index, const std::vector<std::pair<int, Predicate>>& where) {
    for (auto& [idx, p] : where) {
        if (idx != pk_index) continue;
        if (p.literal.kind != Literal::INT)
            throw DbError("type_mismatch", "BIGINT comparison needs integer");
        int ord = (pk > p.literal.i) - (pk < p.literal.i);
        if (!cmp_op(p.op, ord)) return false;
    }
    return true;
}

void unpack_mid(const char* mid, size_t mid_len, const std::vector<ValType>& types,
                int64_t* mid_i, std::string_view* mid_s) {
    const char* p = mid;
    const char* e = mid + mid_len;
    size_t ii = 0, ss = 0;
    for (ValType t : types) {
        if (t == ValType::BIGINT) {
            if (p + 8 > e) throw DbError("internal_error", "packed mid truncated");
            std::memcpy(&mid_i[ii++], p, 8);
            p += 8;
        } else {
            if (p + 4 > e) throw DbError("internal_error", "packed mid truncated");
            uint32_t n = 0;
            std::memcpy(&n, p, 4);
            p += 4;
            if (p + n > e) throw DbError("internal_error", "packed mid truncated");
            mid_s[ss++] = std::string_view(p, n);
            p += n;
        }
    }
}

bool mid_where_ok(int64_t pk, int pk_index, const std::vector<int>& mid_cols,
                  const std::vector<ValType>& mid_types,
                  const int64_t* mid_i, const std::string_view* mid_s,
                  const std::vector<std::pair<int, Predicate>>& where) {
    for (auto& [idx, p] : where) {
        if (idx == pk_index) {
            if (p.literal.kind != Literal::INT)
                throw DbError("type_mismatch", "BIGINT comparison needs integer");
            int ord = (pk > p.literal.i) - (pk < p.literal.i);
            if (!cmp_op(p.op, ord)) return false;
            continue;
        }
        size_t ii = 0, ss = 0;
        bool found = false;
        int64_t iv = 0;
        std::string_view sv;
        ValType typ = ValType::BIGINT;
        for (size_t k = 0; k < mid_cols.size(); ++k) {
            if (mid_types[k] == ValType::BIGINT) {
                if (mid_cols[k] == idx) { found = true; typ = ValType::BIGINT; iv = mid_i[ii]; break; }
                ++ii;
            } else {
                if (mid_cols[k] == idx) { found = true; typ = ValType::VARCHAR; sv = mid_s[ss]; break; }
                ++ss;
            }
        }
        if (!found) throw DbError("unsupported_sql", "vector column in WHERE");
        if (typ == ValType::BIGINT) {
            if (p.literal.kind != Literal::INT)
                throw DbError("type_mismatch", "BIGINT comparison needs integer");
            int ord = (iv > p.literal.i) - (iv < p.literal.i);
            if (!cmp_op(p.op, ord)) return false;
        } else {
            if (p.literal.kind != Literal::STRING)
                throw DbError("type_mismatch", "VARCHAR comparison needs string");
            int ord = sv.compare(std::string_view(p.literal.data(), p.literal.size()));
            ord = (ord > 0) - (ord < 0);
            if (!cmp_op(p.op, ord)) return false;
        }
    }
    return true;
}

bool table_too_big_for_fastscan(int64_t n, int dim) {
    return n > 500000 || (dim > 0 && n * static_cast<int64_t>(dim) * 4 > 512LL * 1024 * 1024);
}

// Start the packed snapshot (and SNAP_ROW WAL) at half the FastScan
// cutoff so the remaining prefix is not written twice. 10k stays on
// the in-memory FastScan path: 10000 * 256 * 4 = 10MiB.
bool should_pack_snapshot(int64_t n, int dim) {
    if (table_too_big_for_fastscan(n, dim)) return true;
    if (n > 250000) return true;
    return dim > 0 && n * static_cast<int64_t>(dim) * 4 > 256LL * 1024 * 1024;
}

int table_dim(const TableSchema& s) {
    return s.vector_index >= 0 ? static_cast<int>(s.cols[static_cast<size_t>(s.vector_index)].len) : 0;
}

// Packed-snapshot offsets in LatestMap. WAL offsets start at 8 and never
// have the high bit set, so this is a disjoint address space.
constexpr uint64_t kSnapFlag = 1ull << 63;
constexpr uint64_t kSnapOffMask = ~kSnapFlag;
constexpr uint64_t kRotateAfter = 64ull << 20;

uint64_t append_packed_row(PackedScan& packed, const TableSchema& s, const Row& row,
                           const std::vector<ValType>& mid_types,
                           const std::vector<int>& mid_cols) {
    int64_t pk = row[static_cast<size_t>(s.pk_index)].i;
    std::vector<int64_t> mid_i(mid_types.size());
    std::vector<std::string_view> mid_s(mid_types.size());
    size_t ii = 0, ss = 0;
    for (size_t k = 0; k < mid_cols.size(); ++k) {
        const Value& v = row[static_cast<size_t>(mid_cols[k])];
        if (mid_types[k] == ValType::BIGINT) mid_i[ii++] = v.i;
        else mid_s[ss++] = std::string_view(v.s);
    }
    const float* vec = nullptr;
    if (s.vector_index >= 0) {
        const Vector& vv = row[static_cast<size_t>(s.vector_index)].v;
        if (!vv.empty()) vec = vv.data();
    }
    return packed.append_row(pk, mid_types, mid_i.data(), mid_s.data(), vec);
}

void trim_heap() {
#if defined(__linux__)
    ::malloc_trim(0);
#endif
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
    data_dir_ = data_dir;
    log_.open(data_dir);
    struct TxnInfo {
        bool data = false;
        bool commit = false;
    };
    std::unordered_map<uint64_t, TxnInfo> txns;
    struct PackedBase { uint32_t tid = 0; uint64_t offset = 0; uint64_t txn = 0; };
    std::unordered_map<uint32_t, PackedBase> packed_bases;
    log_.scan([&](const RecordView& rv) {
        next_txn_ = std::max(next_txn_, rv.txn + 1);
        ByteBuf b;
        b.wrap(rv.payload, rv.payload_len);
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
            case REC_DELETE:
            case REC_SNAP_ROW: {
                uint32_t tid = b.get_u32();
                int64_t pk = b.get_i64();
                auto it = tables_.find(tid);
                if (it != tables_.end()) it->second.latest.set(pk, rv.offset);
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
            case REC_PACKED_BASE: {
                uint32_t tid = b.get_u32();
                packed_bases[tid] = PackedBase{tid, rv.offset, rv.txn};
                txns[rv.txn].data = true;
                break;
            }
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
    // A committed packed-base record means the WAL was rotated. Rebuild
    // LatestMap from the snapshot, then apply WAL records written after it.
    for (auto& [tid, pb] : packed_bases) {
        auto it = tables_.find(tid);
        if (it == tables_.end()) continue;
        if (aborted_.count(pb.txn) || pb.txn > committed_max_) continue;
        Table& t = it->second;
        t.latest.clear();
        int dim = table_dim(t.schema);
        if (!t.packed) t.packed = std::make_unique<PackedScan>();
        const std::string path = data_dir_ + "/mvdb.scan." + std::to_string(tid);
        if (!t.packed->try_open_checkpoint(path, tid, dim))
            throw DbError("internal_error", "packed snapshot missing after checkpoint");
        load_packed_as_latest(t);
    }
    if (!packed_bases.empty()) {
        log_.scan([&](const RecordView& rv) {
            if (rv.type != REC_INSERT && rv.type != REC_UPDATE &&
                rv.type != REC_DELETE && rv.type != REC_SNAP_ROW) return;
            ByteBuf b;
            b.wrap(rv.payload, rv.payload_len);
            if (rv.payload_len < 4) return;
            uint32_t tid = b.get_u32();
            auto pit = packed_bases.find(tid);
            if (pit == packed_bases.end()) return;
            if (aborted_.count(pit->second.txn) || pit->second.txn > committed_max_) return;
            if (rv.offset <= pit->second.offset) return;
            if (aborted_.count(rv.txn) || rv.txn > committed_max_) return;
            int64_t pk = b.get_i64();
            auto it = tables_.find(tid);
            if (it == tables_.end()) return;
            it->second.latest.set(pk, rv.offset);
            if (rv.type == REC_SNAP_ROW && it->second.packed) {
                b.get_u64();
                it->second.packed->cover_row(b.get_u64());
            }
        }, false);
    }
    for (auto& [id, t] : tables_)
        if (!t.schema.ann_index_name.empty()) rebuild_ann(t);
    log_.drop_cache();
    log_.advise_noreadahead();
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
    uint64_t off = 0;
    if (!t.latest.get(pk, off)) return false;
    if (off & kSnapFlag) return row_from_packed(t, off & kSnapOffMask, out);
    while (off != 0) {
        RecordView rv = log_.read_at(off);
        ByteBuf b;
        b.wrap(rv.payload, rv.payload_len);
        b.get_u32();
        b.get_i64();
        uint64_t prev = b.get_u64();
        if (rv.type == REC_DELETE) {
            if (visible(rv.txn, rv.stmt)) return false;
            off = prev;
            continue;
        }
        if (visible(rv.txn, rv.stmt)) {
            if (rv.type == REC_SNAP_ROW)
                return row_from_packed(t, b.get_u64(), out);
            b.read_row_into(out, t.schema.cols.size());
            return true;
        }
        off = prev;
    }
    return false;
}

const Row* Engine::resolve_row(Table& t, int64_t pk) {
    uint64_t head = 0;
    if (!t.latest.get(pk, head)) return nullptr;
    auto& tc = row_cache_[t.schema.table_id];
    auto cit = tc.find(pk);
    if (cit != tc.end() && cit->second.offset == head) return &cit->second.row;
    if (head & kSnapFlag) {
        Row parsed;
        if (!row_from_packed(t, head & kSnapOffMask, parsed)) return nullptr;
        return cache_put(t.schema.table_id, pk, head, std::move(parsed));
    }
    uint64_t off = head;
    while (off != 0) {
        RecordView rv = log_.read_at(off);
        ByteBuf b;
        b.wrap(rv.payload, rv.payload_len);
        b.get_u32();
        b.get_i64();
        uint64_t prev = b.get_u64();
        if (rv.type == REC_DELETE) {
            if (visible(rv.txn, rv.stmt)) return nullptr;
            off = prev;
            continue;
        }
        if (visible(rv.txn, rv.stmt)) {
            if (rv.type == REC_SNAP_ROW) {
                Row parsed;
                if (!row_from_packed(t, b.get_u64(), parsed)) return nullptr;
                return cache_put(t.schema.table_id, pk, head, std::move(parsed));
            }
            Row parsed = b.get_row(t.schema.cols.size());
            return cache_put(t.schema.table_id, pk, head, std::move(parsed));
        }
        off = prev;
    }
    return nullptr;
}

Row* Engine::cache_put(uint32_t tid, int64_t pk, uint64_t head, Row row) {
    // Count map-node / Row / Value overhead, not just payload. 24MiB is
    // enough for the 10k FastScan build and keeps the 600k write HWM from
    // sitting at a 256MiB decoded-row cache that is discarded later.
    size_t bytes = 128;
    for (auto& v : row) bytes += v.s.size() + v.v.size() * 4 + 64;
    constexpr size_t kCacheCap = 24ull * 1024ull * 1024ull;
    if (cache_bytes_ + bytes > kCacheCap) {
        row_cache_.clear();
        cache_bytes_ = 0;
        trim_heap();
    }
    auto& slot = row_cache_[tid][pk];
    slot = CachedRow{head, std::move(row)};
    cache_bytes_ += bytes;
    return &slot.row;
}

void Engine::release_insert_cache() {
    if (row_cache_.empty() && cache_bytes_ == 0) return;
    row_cache_.clear();
    cache_bytes_ = 0;
    trim_heap();
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
            ord = v.s.compare(std::string_view(p.literal.data(), p.literal.size()));
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
    for (auto& [id, t] : tables_) {
        if (t.packed && t.packed->writing()) t.packed->sync_data();
    }
    log_.append(REC_COMMIT, txn, 0, "");
    log_.sync();
    committed_max_ = std::max(committed_max_, txn);
    for (auto& [id, t] : tables_) {
        if (t.packed && t.packed->writing()) t.packed->persist_header();
    }
}

void Engine::rebuild_ann(Table& t) {
    t.ann = std::make_unique<Hnsw>();
    if (t.schema.vector_index < 0) return;
    t.latest.for_each([&](int64_t pk, uint64_t) {
        const Row* row = resolve_row(t, pk);
        if (row) t.ann->insert(pk, (*row)[t.schema.vector_index].v);
    });
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
    const bool snap_wal = t.packed && (t.packed->ready() || t.packed->writing());
    if (snap_wal && t.packed->ready() && !t.packed->writing())
        t.packed->resume_append();
    std::vector<ValType> mid_types;
    std::vector<int> mid_cols;
    if (snap_wal) mid_layout(s, mid_types, mid_cols);
    ByteBuf ins;
    for (auto& row : staged) {
        int64_t pk = row[s.pk_index].i;
        uint64_t prev = 0;
        t.latest.get(pk, prev);
        ins.data.clear();
        ins.put_u32(s.table_id);
        ins.put_i64(pk);
        ins.put_u64(prev);
        uint64_t off;
        if (snap_wal) {
            uint64_t snap_off = append_packed_row(*t.packed, s, row, mid_types, mid_cols);
            ins.put_u64(snap_off);
            off = log_.append(REC_SNAP_ROW, txn, stmt, ins.data);
        } else {
            ins.put_row(row);
            off = log_.append(REC_INSERT, txn, stmt, ins.data);
        }
        t.latest.set(pk, off);
        if (t.ann && s.vector_index >= 0) t.ann->insert(pk, row[s.vector_index].v);
        // Over-memory tables are served by packed scan, not row_cache_.
        // Stop retaining decoded insert rows once FastScan is off so the
        // shutdown snapshot is not charged on top of a 256MiB cache.
        if (table_too_big_for_fastscan(static_cast<int64_t>(t.latest.size()), table_dim(s)))
            release_insert_cache();
        else
            cache_put(s.table_id, pk, off, std::move(row));
        if (!is_auto) txn_writes_.emplace_back(s.table_id, pk);
    }
    if (is_auto) {
        append_commit(txn);
        // Rows already in the snapshot must not be appended again.
        maybe_incremental_packed(t, snap_wal ? nullptr : &staged);
    }
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
    t.latest.for_each([&](int64_t pk, uint64_t) {
        const Row* row = resolve_row(t, pk);
        if (!row || !row_matches(*row, where)) return;
        matches.emplace_back(pk, *row);
    });
    bool is_auto = false;
    uint64_t txn = statement_txn(is_auto);
    uint32_t stmt = ++cur_stmt_;
    const bool snap_wal = t.packed && (t.packed->ready() || t.packed->writing());
    if (snap_wal && t.packed->ready() && !t.packed->writing())
        t.packed->resume_append();
    std::vector<ValType> mid_types;
    std::vector<int> mid_cols;
    if (snap_wal) mid_layout(s, mid_types, mid_cols);
    for (auto& [pk, oldrow] : matches) {
        Row row = oldrow;
        for (auto& [idx, val] : sets) row[idx] = val;
        uint64_t prev = 0;
        t.latest.get(pk, prev);
        ByteBuf b;
        b.put_u32(s.table_id);
        b.put_i64(pk);
        b.put_u64(prev);
        uint64_t off;
        if (snap_wal) {
            uint64_t snap_off = append_packed_row(*t.packed, s, row, mid_types, mid_cols);
            b.put_u64(snap_off);
            off = log_.append(REC_SNAP_ROW, txn, stmt, b.data);
        } else {
            b.put_row(row);
            off = log_.append(REC_UPDATE, txn, stmt, b.data);
        }
        t.latest.set(pk, off);
        if (t.ann && s.vector_index >= 0) {
            t.ann->erase(pk);
            t.ann->insert(pk, row[s.vector_index].v);
        }
        if (table_too_big_for_fastscan(static_cast<int64_t>(t.latest.size()), table_dim(s)))
            release_insert_cache();
        else
            cache_put(s.table_id, pk, off, std::move(row));
        if (!is_auto) txn_writes_.emplace_back(s.table_id, pk);
    }
    if (is_auto) append_commit(txn);
    if (snap_wal) maybe_rotate_wal(false);
    else if (t.packed && t.packed->writing()) abort_incremental_packed(t);
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
    t.latest.for_each([&](int64_t pk, uint64_t) {
        const Row* row = resolve_row(t, pk);
        if (!row || !row_matches(*row, where)) return;
        matches.push_back(pk);
    });
    bool is_auto = false;
    uint64_t txn = statement_txn(is_auto);
    uint32_t stmt = ++cur_stmt_;
    for (int64_t pk : matches) {
        uint64_t prev = 0;
        t.latest.get(pk, prev);
        ByteBuf b;
        b.put_u32(s.table_id);
        b.put_i64(pk);
        b.put_u64(prev);
        uint64_t off = log_.append(REC_DELETE, txn, stmt, b.data);
        t.latest.set(pk, off);
        if (t.ann) t.ann->erase(pk);
        row_cache_[s.table_id].erase(pk);
        if (!is_auto) txn_writes_.emplace_back(s.table_id, pk);
    }
    if (is_auto) append_commit(txn);
    if (t.packed && t.packed->ready()) maybe_rotate_wal(false);
    else if (t.packed && t.packed->writing()) abort_incremental_packed(t);
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
    uint32_t tid = t.schema.table_id;
    t.latest.for_each([&](int64_t pk, uint64_t) {
        const Row* row = resolve_row(t, pk);
        if (!row) return;
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
        // Evict the insert-time cache entry now that the snapshot owns a
        // copy, so peak RSS is ~FastScan not row_cache + FastScan.
        auto tcit = row_cache_.find(tid);
        if (tcit != row_cache_.end()) tcit->second.erase(pk);
    });
    sc.n = static_cast<int64_t>(sc.pks.size());
    sc.epoch = cache_epoch_;
    // The scan snapshot now serves queries; free the decoded-row cache so
    // peak RSS / memcg charge stays low (memory.peak counts page cache too).
    row_cache_.clear();
    cache_bytes_ = 0;
    log_.drop_cache();
}

bool Engine::decode_visible_scan(Table& t, int64_t pk,
                                 const std::vector<ValType>& mid_types,
                                 const std::vector<int>& mid_cols,
                                 int64_t* mid_i, std::string_view* mid_s,
                                 const float*& vec, int& dim) {
    (void)mid_types;
    (void)mid_cols;
    uint64_t off = 0;
    if (!t.latest.get(pk, off)) return false;
    if (off & kSnapFlag) {
        PackedScan::RowView rv;
        int vdim = table_dim(t.schema);
        if (!t.packed || !t.packed->read_view_at(off & kSnapOffMask, mid_types, vdim, rv))
            return false;
        size_t ii = 0, ss = 0, pos = 0;
        for (ValType typ : mid_types) {
            if (typ == ValType::BIGINT) {
                std::memcpy(&mid_i[ii++], rv.mid + pos, 8);
                pos += 8;
            } else {
                uint32_t n = 0;
                std::memcpy(&n, rv.mid + pos, 4);
                pos += 4;
                mid_s[ss++] = std::string_view(rv.mid + pos, n);
                pos += n;
            }
        }
        vec = rv.vec;
        dim = vdim;
        return true;
    }
    while (off != 0) {
        RecordView rv = log_.read_at(off);
        ByteBuf b;
        b.wrap(rv.payload, rv.payload_len);
        b.get_u32();
        b.get_i64();
        uint64_t prev = b.get_u64();
        if (rv.type == REC_DELETE) {
            if (visible(rv.txn, rv.stmt)) return false;
            off = prev;
            continue;
        }
        if (visible(rv.txn, rv.stmt)) {
            if (rv.type == REC_SNAP_ROW) {
                uint64_t snap_off = b.get_u64();
                PackedScan::RowView prv;
                int vdim = table_dim(t.schema);
                if (!t.packed || !t.packed->read_view_at(snap_off, mid_types, vdim, prv))
                    return false;
                size_t ii = 0, ss = 0, pos = 0;
                for (ValType typ : mid_types) {
                    if (typ == ValType::BIGINT) {
                        std::memcpy(&mid_i[ii++], prv.mid + pos, 8);
                        pos += 8;
                    } else {
                        uint32_t n = 0;
                        std::memcpy(&n, prv.mid + pos, 4);
                        pos += 4;
                        mid_s[ss++] = std::string_view(prv.mid + pos, n);
                        pos += n;
                    }
                }
                vec = prv.vec;
                dim = vdim;
                return true;
            }
            vec = nullptr;
            dim = 0;
            size_t ii = 0, ss = 0;
            for (size_t c = 0; c < t.schema.cols.size(); ++c) {
                ValType typ = static_cast<ValType>(b.get_u8());
                if (static_cast<int>(c) == t.schema.vector_index || typ == ValType::VECTOR) {
                    uint32_t n = 0;
                    const float* p = b.get_vec_ptr(n);
                    if (static_cast<int>(c) == t.schema.vector_index) {
                        vec = p;
                        dim = static_cast<int>(n);
                    }
                    continue;
                }
                if (static_cast<int>(c) == t.schema.pk_index) {
                    if (typ == ValType::BIGINT) b.skip(8);
                    else if (typ == ValType::VARCHAR) b.skip(b.get_u32());
                    else {
                        uint32_t n = 0;
                        b.get_vec_ptr(n);
                    }
                    continue;
                }
                if (typ == ValType::BIGINT) mid_i[ii++] = b.get_i64();
                else if (typ == ValType::VARCHAR) {
                    uint32_t n = b.get_u32();
                    mid_s[ss++] = std::string_view(b.ptr(), n);
                    b.skip(n);
                } else {
                    uint32_t n = 0;
                    b.get_vec_ptr(n);
                }
            }
            return true;
        }
        off = prev;
    }
    return false;
}

void Engine::populate_packed_from_latest(Table& t) {
    std::vector<ValType> mid_types;
    std::vector<int> mid_cols;
    mid_layout(t.schema, mid_types, mid_cols);
    std::vector<int64_t> mid_i(mid_types.size());
    std::vector<std::string_view> mid_s(mid_types.size());
    const uint32_t tid = t.schema.table_id;
    uint64_t last_drop = 0;
    constexpr uint64_t kDropEvery = 2ull << 20;
    // Walk the WAL in offset order instead of materializing a 16-byte
    // (offset, pk) vector of every live row. decode_visible_scan reuses
    // the same 2MiB read window because records are already in order.
    log_.scan([&](const RecordView& rv) {
        if (rv.type != REC_INSERT && rv.type != REC_UPDATE) return;
        ByteBuf b;
        b.wrap(rv.payload, rv.payload_len);
        if (rv.payload_len < 4) return;
        uint32_t rec_tid = b.get_u32();
        if (rec_tid != tid) return;
        int64_t pk = b.get_i64();
        uint64_t head = 0;
        if (!t.latest.get(pk, head) || head != rv.offset) return;
        const float* vec = nullptr;
        int vdim = 0;
        if (!decode_visible_scan(t, pk, mid_types, mid_cols, mid_i.data(), mid_s.data(), vec, vdim))
            return;
        t.packed->append_row(pk, mid_types, mid_i.data(), mid_s.data(), vec);
        if (rv.offset - last_drop >= kDropEvery) {
            log_.drop_upto(rv.offset);
            last_drop = rv.offset;
        }
    }, false);
}

void Engine::abort_incremental_packed(Table& t) {
    if (t.packed) t.packed->abort_build();
    t.packed_epoch = 0;
}

void Engine::maybe_incremental_packed(Table& t, const std::vector<Row>* new_rows) {
    // Start packing before FastScan turns off so later rows are not
    // written both to the WAL and the snapshot. 10k never enters here.
    if (!should_pack_snapshot(static_cast<int64_t>(t.latest.size()), table_dim(t.schema)))
        return;
    try {
        if (t.packed && t.packed->ready() && !t.packed->writing())
            t.packed->resume_append();
        if (t.packed && t.packed->writing()) {
            if (new_rows && !new_rows->empty()) {
                std::vector<ValType> mid_types;
                std::vector<int> mid_cols;
                mid_layout(t.schema, mid_types, mid_cols);
                for (const Row& row : *new_rows)
                    append_packed_row(*t.packed, t.schema, row, mid_types, mid_cols);
            }
            maybe_rotate_wal(false);
            return;
        }
        int dim = table_dim(t.schema);
        if (!t.packed) t.packed = std::make_unique<PackedScan>();
        const std::string path = data_dir_ + "/mvdb.scan." + std::to_string(t.schema.table_id);
        release_insert_cache();
        log_.drop_cache();
        log_.advise_noreadahead();
        t.packed->start_build(path, t.schema.table_id, dim, log_.size(), committed_max_);
        populate_packed_from_latest(t);
        t.packed->set_reuse_key(log_.size(), committed_max_);
        t.packed->finish_build();
        sync_dir(data_dir_);
        log_.drop_cache();
        if (t.packed) t.packed->drop_cache();
        trim_heap();
        t.packed_epoch = cache_epoch_;
        maybe_rotate_wal(false);
    } catch (...) {
        // Snapshot is a cache; a failed incremental build must not fail INSERT.
        abort_incremental_packed(t);
    }
}

void Engine::maybe_incremental_after_txn() {
    // Called after an explicit COMMIT, while txn_writes_ is still populated.
    std::vector<uint32_t> tids;
    for (auto& [tid, pk] : txn_writes_) {
        if (std::find(tids.begin(), tids.end(), tid) == tids.end()) tids.push_back(tid);
    }
    for (uint32_t tid : tids) {
        auto it = tables_.find(tid);
        if (it == tables_.end()) continue;
        Table& t = it->second;
        if (!should_pack_snapshot(static_cast<int64_t>(t.latest.size()), table_dim(t.schema)))
            continue;
        try {
            if (t.packed && t.packed->writing()) {
                std::vector<ValType> mid_types;
                std::vector<int> mid_cols;
                mid_layout(t.schema, mid_types, mid_cols);
                for (auto& [id, pk] : txn_writes_) {
                    if (id != tid) continue;
                    uint64_t off = 0;
                    if (t.latest.get(pk, off) && !(off & kSnapFlag)) {
                        RecordView rv = log_.read_at(off);
                        if (rv.type == REC_SNAP_ROW) continue;
                    }
                    const Row* row = resolve_row(t, pk);
                    if (row) append_packed_row(*t.packed, t.schema, *row, mid_types, mid_cols);
                }
            } else {
                maybe_incremental_packed(t, nullptr);
            }
        } catch (...) {
            abort_incremental_packed(t);
        }
    }
    maybe_rotate_wal(false);
}

bool Engine::row_from_packed(Table& t, uint64_t snap_off, Row& out) {
    if (!t.packed) return false;
    std::vector<ValType> mid_types;
    std::vector<int> mid_cols;
    mid_layout(t.schema, mid_types, mid_cols);
    PackedScan::RowView rv;
    int dim = table_dim(t.schema);
    if (!t.packed->read_view_at(snap_off, mid_types, dim, rv)) return false;
    out.assign(t.schema.cols.size(), {});
    if (t.schema.pk_index >= 0) {
        out[static_cast<size_t>(t.schema.pk_index)].type = ValType::BIGINT;
        out[static_cast<size_t>(t.schema.pk_index)].i = rv.pk;
    }
    size_t pos = 0;
    for (size_t k = 0; k < mid_cols.size(); ++k) {
        const size_t idx = static_cast<size_t>(mid_cols[k]);
        if (mid_types[k] == ValType::BIGINT) {
            int64_t v = 0;
            std::memcpy(&v, rv.mid + pos, 8);
            pos += 8;
            out[idx].type = ValType::BIGINT;
            out[idx].i = v;
        } else {
            uint32_t n = 0;
            std::memcpy(&n, rv.mid + pos, 4);
            pos += 4;
            out[idx].type = ValType::VARCHAR;
            out[idx].s.assign(rv.mid + pos, n);
            pos += n;
        }
    }
    if (t.schema.vector_index >= 0 && rv.vec) {
        const size_t vidx = static_cast<size_t>(t.schema.vector_index);
        out[vidx].type = ValType::VECTOR;
        out[vidx].v.assign(rv.vec, rv.vec + dim);
    }
    return true;
}

void Engine::load_packed_as_latest(Table& t) {
    t.latest.clear();
    if (!t.packed || !t.packed->ready()) return;
    std::vector<ValType> mid_types;
    std::vector<int> mid_cols;
    mid_layout(t.schema, mid_types, mid_cols);
    int dim = table_dim(t.schema);
    auto cur = t.packed->cursor(mid_types, dim);
    while (cur.next())
        t.latest.set(cur.row().pk, kSnapFlag | cur.row_offset());
    t.packed_epoch = cache_epoch_;
}

void Engine::maybe_rotate_wal(bool force) {
    if (cur_txn_ != 0) return;
    bool any_ready = false;
    for (auto& [id, t] : tables_) {
        if (t.packed && t.packed->ready()) any_ready = true;
    }
    if (!any_ready) return;
    if (!force && log_.size() < kRotateAfter) return;
    for (auto& [id, t] : tables_) {
        if (t.packed && t.packed->writing()) {
            t.packed->set_reuse_key(log_.size(), committed_max_);
            t.packed->finish_build();
            sync_dir(data_dir_);
            t.packed_epoch = cache_epoch_;
        }
    }

    release_insert_cache();
    log_.drop_cache();
    try {
        log_.park_as_old();
        uint64_t txn = next_txn_++;
        uint32_t stmt = 1;
        for (auto& [id, t] : tables_) {
            ByteBuf b;
            b.put_u32(t.schema.table_id);
            b.put_str(t.schema.name);
            b.put_u32(static_cast<uint32_t>(t.schema.cols.size()));
            for (auto& c : t.schema.cols) {
                b.put_str(c.name);
                b.put_u8(static_cast<uint8_t>(c.type));
                b.put_u32(c.len);
            }
            b.put_u32(static_cast<uint32_t>(t.schema.pk_index));
            b.put_u32(static_cast<uint32_t>(t.schema.vector_index));
            log_.append(REC_CREATE_TABLE, txn, stmt++, b.data);
            if (!t.schema.ann_index_name.empty()) {
                ByteBuf ib;
                ib.put_u32(t.schema.table_id);
                ib.put_str(t.schema.ann_index_name);
                const std::string& vcol = t.schema.cols[static_cast<size_t>(t.schema.vector_index)].name;
                ib.put_str(vcol);
                log_.append(REC_CREATE_INDEX, txn, stmt++, ib.data);
            }
        }
        for (auto& [id, t] : tables_) {
            if (!(t.packed && t.packed->ready())) continue;
            ByteBuf p;
            p.put_u32(t.schema.table_id);
            log_.append(REC_PACKED_BASE, txn, stmt++, p.data);
        }
        append_commit(txn);
        sync_dir(data_dir_);
        for (auto& [id, t] : tables_) {
            if (!(t.packed && t.packed->ready())) continue;
            t.packed->update_reuse_key(log_.size(), committed_max_);
        }
        sync_dir(data_dir_);
        log_.drop_parked_old();
        for (auto& [id, t] : tables_) {
            t.create_txn = txn;
            if (!t.schema.ann_index_name.empty()) t.index_txn = txn;
            if (t.packed && t.packed->ready()) load_packed_as_latest(t);
        }
        log_.drop_cache();
        trim_heap();
    } catch (...) {
        // Mid-rotate failure: put the complete old WAL back so this process
        // (and the next open) still has every row.
        try { log_.rollback_parked(); } catch (...) {}
    }
}

void Engine::ensure_packed_scan(Table& t) {
    int dim = t.schema.vector_index >= 0
                  ? static_cast<int>(t.schema.cols[static_cast<size_t>(t.schema.vector_index)].len)
                  : 0;
    if (t.packed && t.packed->ready() && t.packed_epoch == cache_epoch_) return;
    if (t.packed && t.packed->writing()) {
        // Incremental snapshot already has every committed row. Finalize
        // with the current reuse key so a restarted query process can open it.
        t.packed->set_reuse_key(log_.size(), committed_max_);
        t.packed->finish_build();
        sync_dir(data_dir_);
        log_.drop_cache();
        if (t.packed) t.packed->drop_cache();
        trim_heap();
        t.packed_epoch = cache_epoch_;
        return;
    }
    // A durable checkpoint (including one left by WAL rotate) must not go
    // through start_build, which unlinks the snapshot.
    if (t.packed && t.packed->ready()) {
        t.packed_epoch = cache_epoch_;
        return;
    }
    if (!t.packed) t.packed = std::make_unique<PackedScan>();
    const std::string path = data_dir_ + "/mvdb.scan." + std::to_string(t.schema.table_id);
    if (t.packed->try_open_existing(path, t.schema.table_id, dim, log_.size(), committed_max_)) {
        t.packed_epoch = cache_epoch_;
        return;
    }
    // Fresh build rereads the WAL and writes a packed file about the same
    // size as the vectors. Drop the insert-time cache and already-durable
    // WAL pages first so memory.peak is not row_cache + WAL + snapshot.
    release_insert_cache();
    log_.drop_cache();
    log_.advise_noreadahead();
    t.packed->start_build(path, t.schema.table_id, dim, log_.size(), committed_max_);
    populate_packed_from_latest(t);
    t.packed->finish_build();
    sync_dir(data_dir_);
    log_.drop_cache();
    if (t.packed) t.packed->drop_cache();
    trim_heap();
    t.packed_epoch = cache_epoch_;
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
    bool point_done = false;
    {
        bool saw_pk_eq = false;
        bool pk_conflict = false;
        int64_t point_pk = 0;
        for (auto& [idx, p] : where) {
            if (idx == s.pk_index && p.op == "=" && p.literal.kind == Literal::INT) {
                if (saw_pk_eq && point_pk != p.literal.i) pk_conflict = true;
                saw_pk_eq = true;
                point_pk = p.literal.i;
            }
        }
        if (pk_conflict) {
            point_done = true;
        } else if (saw_pk_eq) {
            const Row* row = resolve_row(t, point_pk);
            if (row && row_matches(*row, where)) out.push_back(collect(point_pk, *row));
            point_done = true;
        }
    }
    bool use_ann = !point_done && mode == "ann" && order_dist && st.has_limit && st.limit > 0 && t.ann;
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
    bool fast_done = point_done;
    if (!use_ann && !point_done) {
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
                    auto take_id = [&](int64_t i, float dsq) {
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
                    size_t k = lo;
                    if (dim == 128 && !st.order_desc) {
                        const float* base = sc.vecs.data();
                        for (; k + 4 <= hi; k += 4) {
                            const float* p0 = base + static_cast<size_t>(ids[k]) * 128u;
                            const float* p1 = base + static_cast<size_t>(ids[k + 1]) * 128u;
                            const float* p2 = base + static_cast<size_t>(ids[k + 2]) * 128u;
                            const float* p3 = base + static_cast<size_t>(ids[k + 3]) * 128u;
                            if (k + 8 <= hi) {
                                _mm_prefetch(reinterpret_cast<const char*>(
                                                 base + static_cast<size_t>(ids[k + 4]) * 128u),
                                             _MM_HINT_T0);
                                _mm_prefetch(reinterpret_cast<const char*>(
                                                 base + static_cast<size_t>(ids[k + 5]) * 128u),
                                             _MM_HINT_T0);
                                _mm_prefetch(reinterpret_cast<const char*>(
                                                 base + static_cast<size_t>(ids[k + 6]) * 128u),
                                             _MM_HINT_T0);
                                _mm_prefetch(reinterpret_cast<const char*>(
                                                 base + static_cast<size_t>(ids[k + 7]) * 128u),
                                             _MM_HINT_T0);
                            }
                            float ds[4];
                            l2_sq_fast_128x4(p0, p1, p2, p3, quse, ds);
                            take_id(ids[k], ds[0]);
                            take_id(ids[k + 1], ds[1]);
                            take_id(ids[k + 2], ds[2]);
                            take_id(ids[k + 3], ds[3]);
                        }
                    }
                    for (; k < hi; ++k) {
                        int64_t i = ids[k];
                        const float* vp = &sc.vecs[static_cast<size_t>(i) * static_cast<size_t>(dim)];
                        take_id(i, l2_sq_fast(vp, quse, dim));
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
                                pf_val.assign(where[0].second.literal.data(),
                                              where[0].second.literal.size());
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
        bool cold_done = false;
        const bool by_pk_order = !order_dist && (!st.has_order || order_col == s.pk_index);
        const int dim_est = s.vector_index >= 0
                                ? static_cast<int>(s.cols[static_cast<size_t>(s.vector_index)].len)
                                : 0;
        const bool large_table = table_too_big_for_fastscan(static_cast<int64_t>(t.latest.size()), dim_est);
        const bool want_full_scan =
            (order_dist && st.has_limit && st.limit > 0 && st.limit <= 4096) ||
            !(st.has_limit && st.limit > 0 && by_pk_order);
        if (large_table && want_full_scan) {
            std::vector<ValType> mid_types;
            std::vector<int> mid_cols;
            mid_layout(s, mid_types, mid_cols);
            std::vector<int64_t> mid_i(mid_types.size() ? mid_types.size() : 1);
            std::vector<std::string_view> mid_s(mid_types.size() ? mid_types.size() : 1);
            auto packed_match = [&](int64_t pk, const int64_t* mi, const std::string_view* ms) {
                return mid_where_ok(pk, s.pk_index, mid_cols, mid_types, mi, ms, where);
            };
            auto packed_project = [&](int64_t pk, const float* vec, int vdim,
                                      const int64_t* mi, const std::string_view* ms) {
                OutRow o;
                o.pk = pk;
                o.dists.resize(items.size(), 0.0);
                o.cells.reserve(items.size());
                auto fill_col = [&](int idx, Cell& cell) {
                    if (idx == s.pk_index) {
                        cell.kind = Cell::INT;
                        cell.i = pk;
                        return;
                    }
                    if (idx == s.vector_index) {
                        cell.kind = Cell::VEC;
                        if (vec && vdim > 0) cell.v.assign(vec, vec + vdim);
                        return;
                    }
                    size_t ii = 0, ss = 0;
                    for (size_t k = 0; k < mid_cols.size(); ++k) {
                        if (mid_types[k] == ValType::BIGINT) {
                            if (mid_cols[k] == idx) {
                                cell.kind = Cell::INT;
                                cell.i = mi[ii];
                                return;
                            }
                            ++ii;
                        } else {
                            if (mid_cols[k] == idx) {
                                cell.kind = Cell::STR;
                                cell.s.assign(ms[ss].data(), ms[ss].size());
                                return;
                            }
                            ++ss;
                        }
                    }
                };
                for (size_t k = 0; k < items.size(); ++k) {
                    if (items[k].is_distance) {
                        Cell cell;
                        cell.kind = Cell::REAL;
                        const Vector& q = item_qvec[k];
                        int d = vdim;
                        if (d <= 0) d = static_cast<int>(q.size());
                        double sum = 0.0;
                        if (vec) {
                            for (int j = 0; j < d; ++j) {
                                double td = static_cast<double>(vec[j]) - static_cast<double>(q[static_cast<size_t>(j)]);
                                sum += td * td;
                            }
                        }
                        cell.d = std::sqrt(sum);
                        o.dists[k] = cell.d;
                        o.cells.push_back(std::move(cell));
                    } else {
                        Cell cell;
                        fill_col(item_col[k], cell);
                        o.cells.push_back(std::move(cell));
                    }
                }
                if (st.has_order && !order_dist) fill_col(order_col, o.order_cell);
                return o;
            };

            if (t.packed && t.packed->writing()) {
                try { ensure_packed_scan(t); } catch (...) {}
            }
            bool packed_ready = t.packed && t.packed->ready() && t.packed_epoch == cache_epoch_;
            if (!packed_ready) {
                if (!t.packed) t.packed = std::make_unique<PackedScan>();
                const std::string path =
                    data_dir_ + "/mvdb.scan." + std::to_string(s.table_id);
                if (t.packed->try_open_existing(path, s.table_id, dim_est, log_.size(), committed_max_)) {
                    packed_ready = true;
                    t.packed_epoch = cache_epoch_;
                }
            }

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
                auto entry_worse = [&](const Cand& a, const Cand& b) {
                    if (a.dsq != b.dsq) return st.order_desc ? (a.dsq > b.dsq) : (a.dsq < b.dsq);
                    return a.pk < b.pk;
                };
                auto better_than_top = [&](float dsq, int64_t pk) {
                    if (dsq != cands_[0].dsq)
                        return st.order_desc ? (dsq > cands_[0].dsq) : (dsq < cands_[0].dsq);
                    return pk < cands_[0].pk;
                };
                auto consider = [&](float dsq, int64_t pk) {
                    if (top_n < K) {
                        cands_[top_n++] = {dsq, pk, 0};
                        if (top_n == K)
                            std::make_heap(cands_.begin(), cands_.begin() + static_cast<std::ptrdiff_t>(K), entry_worse);
                    } else if (better_than_top(dsq, pk)) {
                        std::pop_heap(cands_.begin(), cands_.begin() + static_cast<std::ptrdiff_t>(K), entry_worse);
                        cands_[K - 1] = {dsq, pk, 0};
                        std::push_heap(cands_.begin(), cands_.begin() + static_cast<std::ptrdiff_t>(K), entry_worse);
                    }
                };
                auto flush_batch = [&](const float* const* vs, const int64_t* pks, int nfill) {
                    if (nfill == 4 && dim == 128 && !st.order_desc) {
                        float ds[4];
                        l2_sq_fast_128x4(vs[0], vs[1], vs[2], vs[3], quse, ds);
                        for (int i = 0; i < 4; ++i) consider(ds[i], pks[i]);
                    } else {
                        for (int i = 0; i < nfill; ++i)
                            consider(l2_sq_fast(vs[i], quse, dim), pks[i]);
                    }
                };

                if (packed_ready) {
                    auto cur = t.packed->cursor(mid_types, dim);
                    alignas(64) float batch_store[4 * 512];
                    const float* batch_v[4];
                    int64_t batch_pk[4];
                    int batch_n = 0;
                    while (cur.next()) {
                        const auto& r = cur.row();
                        if (!pk_where_ok(r.pk, s.pk_index, where)) continue;
                        if (!mid_types.empty()) unpack_mid(r.mid, r.mid_len, mid_types, mid_i.data(), mid_s.data());
                        if (!packed_match(r.pk, mid_i.data(), mid_s.data())) continue;
                        if (!r.vec || dim <= 0) continue;
                        if (dim <= 512) {
                            std::memcpy(batch_store + batch_n * dim, r.vec, static_cast<size_t>(dim) * 4);
                            batch_v[batch_n] = batch_store + batch_n * dim;
                            batch_pk[batch_n] = r.pk;
                            if (++batch_n == 4) {
                                flush_batch(batch_v, batch_pk, 4);
                                batch_n = 0;
                            }
                        } else {
                            consider(l2_sq_fast(r.vec, quse, dim), r.pk);
                        }
                    }
                    if (batch_n) flush_batch(batch_v, batch_pk, batch_n);
                } else {
                    const std::string path =
                        data_dir_ + "/mvdb.scan." + std::to_string(s.table_id);
                    t.packed->start_build(path, s.table_id, dim_est, log_.size(), committed_max_);
                    std::vector<std::pair<uint64_t, int64_t>> recs;
                    recs.reserve(t.latest.size());
                    t.latest.for_each([&](int64_t pk, uint64_t off) { recs.emplace_back(off, pk); });
                    std::sort(recs.begin(), recs.end());
                    const float* vec = nullptr;
                    int vdim = 0;
                    uint64_t last_drop = 0;
                    constexpr uint64_t kDropEvery = 2ull << 20;
                    for (auto& [off, pk] : recs) {
                        vec = nullptr;
                        vdim = 0;
                        if (!decode_visible_scan(t, pk, mid_types, mid_cols, mid_i.data(), mid_s.data(), vec, vdim))
                            continue;
                        t.packed->append_row(pk, mid_types, mid_i.data(), mid_s.data(), vec);
                        if (packed_match(pk, mid_i.data(), mid_s.data()) && vec && dim > 0)
                            consider(l2_sq_fast(vec, quse, dim), pk);
                        if (off - last_drop >= kDropEvery) {
                            log_.drop_upto(off);
                            last_drop = off;
                        }
                    }
                    t.packed->finish_build();
                    sync_dir(data_dir_);
                    t.packed_epoch = cache_epoch_;
                    trim_heap();
                }

                if (t.packed) t.packed->drop_cache();
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
                std::sort(out.begin(), out.end(), [&](const OutRow& a, const OutRow& b) {
                    double da = a.dists[order_item], db = b.dists[order_item];
                    if (da != db) return st.order_desc ? (da > db) : (da < db);
                    return a.pk < b.pk;
                });
                cold_done = true;
            } else {
                if (!packed_ready) ensure_packed_scan(t);
                auto out_less = [&](const OutRow& a, const OutRow& b) {
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
                };
                auto cur = t.packed->cursor(mid_types, dim_est);
                const bool limited = st.has_limit && st.limit > 0;
                const size_t K = limited ? static_cast<size_t>(st.limit) : 0;
                while (cur.next()) {
                    const auto& r = cur.row();
                    if (!pk_where_ok(r.pk, s.pk_index, where)) continue;
                    if (!mid_types.empty()) unpack_mid(r.mid, r.mid_len, mid_types, mid_i.data(), mid_s.data());
                    if (!packed_match(r.pk, mid_i.data(), mid_s.data())) continue;
                    OutRow o = packed_project(r.pk, r.vec, dim_est, mid_i.data(), mid_s.data());
                    if (!limited) {
                        out.push_back(std::move(o));
                    } else if (out.size() < K) {
                        out.push_back(std::move(o));
                        if (out.size() == K) std::make_heap(out.begin(), out.end(), out_less);
                    } else if (out_less(o, out[0])) {
                        std::pop_heap(out.begin(), out.end(), out_less);
                        out.back() = std::move(o);
                        std::push_heap(out.begin(), out.end(), out_less);
                    }
                }
                if (t.packed) t.packed->drop_cache();
                std::sort(out.begin(), out.end(), out_less);
                cold_done = true;
            }
        }

        if (!cold_done && order_dist && st.has_limit && st.limit > 0 && st.limit <= 4096) {
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
            t.latest.for_each([&](int64_t pk, uint64_t off) { recs.emplace_back(off, pk); });
            std::sort(recs.begin(), recs.end());
            Row row;
            uint64_t last_drop = 0;
            constexpr uint64_t kDropEvery = 2ull << 20;
            for (auto& [off, pk] : recs) {
                if (!pk_where_ok(pk, s.pk_index, where) || !load_row(t, pk, row) || !row_matches(row, where)) continue;
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
        } else if (!cold_done) {
            auto out_less = [&](const OutRow& a, const OutRow& b) {
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
            };
            // Default order (and ORDER BY pk) with LIMIT: walk keys, only
            // resolve rows until K visible matches. Avoids materializing a
            // 2M-row result and faulting the whole WAL into page cache.
            const bool by_pk = !order_dist && (!st.has_order || order_col == s.pk_index);
            if (st.has_limit && st.limit > 0 && by_pk) {
                std::vector<int64_t> pks;
                pks.reserve(t.latest.size());
                t.latest.for_each([&](int64_t pk, uint64_t) { pks.push_back(pk); });
                if (st.has_order && st.order_desc)
                    std::sort(pks.begin(), pks.end(), std::greater<int64_t>());
                else
                    std::sort(pks.begin(), pks.end());
                Row row;
                const size_t K = static_cast<size_t>(st.limit);
                for (int64_t pk : pks) {
                    if (!pk_where_ok(pk, s.pk_index, where) || !load_row(t, pk, row) || !row_matches(row, where)) continue;
                    out.push_back(collect(pk, row));
                    if (out.size() == K) break;
                }
                log_.drop_cache();
            } else if (st.has_limit && st.limit > 0) {
                std::vector<std::pair<uint64_t, int64_t>> recs;
                recs.reserve(t.latest.size());
                t.latest.for_each([&](int64_t pk, uint64_t off) { recs.emplace_back(off, pk); });
                std::sort(recs.begin(), recs.end());
                Row row;
                uint64_t last_drop = 0;
                constexpr uint64_t kDropEvery = 2ull << 20;
                const size_t K = static_cast<size_t>(st.limit);
                for (auto& [off, pk] : recs) {
                    if (!pk_where_ok(pk, s.pk_index, where) || !load_row(t, pk, row) || !row_matches(row, where)) continue;
                    OutRow o = collect(pk, row);
                    if (out.size() < K) {
                        out.push_back(std::move(o));
                        if (out.size() == K)
                            std::make_heap(out.begin(), out.end(), out_less);
                    } else if (out_less(o, out[0])) {
                        std::pop_heap(out.begin(), out.end(), out_less);
                        out.back() = std::move(o);
                        std::push_heap(out.begin(), out.end(), out_less);
                    }
                    if (off - last_drop >= kDropEvery) {
                        log_.drop_upto(off);
                        last_drop = off;
                    }
                }
                log_.drop_cache();
                std::sort(out.begin(), out.end(), out_less);
            } else {
                std::vector<std::pair<uint64_t, int64_t>> recs;
                recs.reserve(t.latest.size());
                t.latest.for_each([&](int64_t pk, uint64_t off) { recs.emplace_back(off, pk); });
                std::sort(recs.begin(), recs.end());
                Row row;
                uint64_t last_drop = 0;
                constexpr uint64_t kDropEvery = 2ull << 20;
                out.reserve(t.latest.size());
                for (auto& [off, pk] : recs) {
                    if (!pk_where_ok(pk, s.pk_index, where) || !load_row(t, pk, row) || !row_matches(row, where)) continue;
                    out.push_back(collect(pk, row));
                    if (off - last_drop >= kDropEvery) {
                        log_.drop_upto(off);
                        last_drop = off;
                    }
                }
                log_.drop_cache();
                std::sort(out.begin(), out.end(), out_less);
            }
        }
    }

    if (st.has_limit && static_cast<int64_t>(out.size()) > st.limit)
        out.resize(static_cast<size_t>(st.limit));
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
            maybe_incremental_after_txn();
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
    // Durable WAL first so a packed snapshot header (log size +
    // committed_max) matches what a restarted query process recovers.
    // COMMIT/autocommit fsync is unchanged; this only covers a possible
    // shutdown abort record.
    log_.sync();
    bool build_packed = false;
    for (auto& [id, t] : tables_) {
        if (table_too_big_for_fastscan(static_cast<int64_t>(t.latest.size()), table_dim(t.schema)))
            build_packed = true;
    }
    if (build_packed) {
        release_insert_cache();
        log_.drop_cache();
    }
    for (auto& [id, t] : tables_) {
        // Over-memory tables only; the 10k FastScan path stays untouched.
        if (table_too_big_for_fastscan(static_cast<int64_t>(t.latest.size()), table_dim(t.schema))) {
            try {
                ensure_packed_scan(t);
            } catch (...) {
                // Snapshot is a cache; a failed build must not fail shutdown.
                if (t.packed) t.packed->close();
            }
        }
    }
    try {
        maybe_rotate_wal(true);
    } catch (...) {
    }
    for (auto& [id, t] : tables_) {
        if (t.packed) t.packed->close();
    }
    log_.drop_cache();
    log_.close();
}

}  // namespace mvdb
