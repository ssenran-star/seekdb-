#pragma once
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "common.h"

namespace mvdb {

enum RecType : uint8_t {
    REC_CREATE_TABLE = 1,
    REC_CREATE_INDEX = 2,
    REC_INSERT = 3,
    REC_UPDATE = 4,
    REC_DELETE = 5,
    REC_COMMIT = 6,
    REC_ABORT = 7
};

class ByteBuf {
public:
    std::string data;

    void put_u8(uint8_t v) { data.push_back(static_cast<char>(v)); }
    void put_u32(uint32_t v) { append(&v, 4); }
    void put_u64(uint64_t v) { append(&v, 8); }
    void put_i64(int64_t v) { append(&v, 8); }
    void put_str(const std::string& s) {
        put_u32(static_cast<uint32_t>(s.size()));
        data += s;
    }
    void put_vec(const Vector& v) {
        put_u32(static_cast<uint32_t>(v.size()));
        append(v.data(), v.size() * sizeof(float));
    }
    void put_row(const Row& row) {
        size_t need = row.size();
        for (const Value& v : row) {
            if (v.type == ValType::BIGINT) need += 9;
            else if (v.type == ValType::VARCHAR) need += 5 + v.s.size();
            else need += 5 + v.v.size() * 4;
        }
        data.reserve(data.size() + need);
        for (const Value& v : row) {
            put_u8(static_cast<uint8_t>(v.type));
            switch (v.type) {
                case ValType::BIGINT: put_i64(v.i); break;
                case ValType::VARCHAR: put_str(v.s); break;
                case ValType::VECTOR: put_vec(v.v); break;
            }
        }
    }

    void wrap(const char* p, size_t n) {
        rd_ = p;
        rd_n_ = n;
        pos_ = 0;
    }

    uint8_t get_u8() { return read<uint8_t>(); }
    uint32_t get_u32() { return read<uint32_t>(); }
    uint64_t get_u64() { return read<uint64_t>(); }
    int64_t get_i64() { return read<int64_t>(); }
    std::string get_str() {
        uint32_t n = get_u32();
        check(n);
        std::string s(rd_ + pos_, n);
        pos_ += n;
        return s;
    }
    Vector get_vec() {
        uint32_t n = get_u32();
        check(n * sizeof(float));
        Vector v(n);
        std::memcpy(v.data(), rd_ + pos_, n * sizeof(float));
        pos_ += n * sizeof(float);
        return v;
    }
    Row get_row(size_t ncols) {
        Row row;
        read_row_into(row, ncols);
        return row;
    }
    void read_row_into(Row& row, size_t ncols) {
        if (row.size() != ncols) row.resize(ncols);
        for (size_t k = 0; k < ncols; ++k) {
            Value& v = row[k];
            v.type = static_cast<ValType>(get_u8());
            switch (v.type) {
                case ValType::BIGINT:
                    v.i = get_i64();
                    v.s.clear();
                    v.v.clear();
                    break;
                case ValType::VARCHAR: {
                    uint32_t n = get_u32();
                    check(n);
                    v.s.assign(rd_ + pos_, n);
                    pos_ += n;
                    v.i = 0;
                    v.v.clear();
                    break;
                }
                case ValType::VECTOR: {
                    uint32_t n = get_u32();
                    check(n * sizeof(float));
                    v.v.resize(n);
                    std::memcpy(v.v.data(), rd_ + pos_, n * sizeof(float));
                    pos_ += n * sizeof(float);
                    v.i = 0;
                    v.s.clear();
                    break;
                }
                default:
                    throw DbError("internal_error", "bad value type in log");
            }
        }
        if (row.size() > ncols) row.resize(ncols);
    }
    bool done() const { return pos_ >= rd_n_; }
    void skip(size_t n) { check(n); pos_ += n; }
    const char* ptr() const { return rd_ + pos_; }
    const float* get_vec_ptr(uint32_t& n) {
        n = get_u32();
        check(n * sizeof(float));
        const float* p = reinterpret_cast<const float*>(rd_ + pos_);
        pos_ += n * sizeof(float);
        return p;
    }
    void skip_value() {
        switch (static_cast<ValType>(get_u8())) {
            case ValType::BIGINT: skip(8); break;
            case ValType::VARCHAR: skip(get_u32()); break;
            case ValType::VECTOR: {
                uint32_t n = get_u32();
                skip(n * sizeof(float));
                break;
            }
            default:
                throw DbError("internal_error", "bad value type in log");
        }
    }

private:
    void append(const void* p, size_t n) {
        data.append(static_cast<const char*>(p), n);
    }
    template <typename T>
    T read() {
        check(sizeof(T));
        T v;
        std::memcpy(&v, rd_ + pos_, sizeof(T));
        pos_ += sizeof(T);
        return v;
    }
    void check(size_t n) const {
        if (pos_ + n > rd_n_) throw DbError("internal_error", "log record truncated");
    }
    const char* rd_ = nullptr;
    size_t rd_n_ = 0;
    size_t pos_ = 0;
};

struct RecordView {
    uint64_t offset = 0;
    uint64_t txn = 0;
    uint32_t stmt = 0;
    uint8_t type = 0;
    // Points into Log's read window; valid until the next Log read/scan/append.
    const char* payload = nullptr;
    size_t payload_len = 0;
};

class Log {
public:
    ~Log();
    void open(const std::string& dir);
    uint64_t append(uint8_t type, uint64_t txn, uint32_t stmt, const std::string& payload);
    void sync();
    void drop_cache();
    void drop_upto(uint64_t off);
    void scan(const std::function<void(const RecordView&)>& cb);
    RecordView read_at(uint64_t offset);
    void close();
    uint64_t size() const { return size_; }

private:
    void flush_buf();
    void invalidate_read_buf();
    bool ensure_in_buf(uint64_t off, size_t n);
    const char* ptr_at(uint64_t off) const { return rbuf_.data() + (off - rbuf_off_); }

    int fd_ = -1;
    std::string path_;
    std::string wbuf_;
    std::string rbuf_;
    uint64_t size_ = 0;
    uint64_t drop_pos_ = 0;
    uint64_t rbuf_off_ = 0;
    size_t rbuf_len_ = 0;
    static constexpr size_t kReadWindow = 2ull << 20;
};

void sync_dir(const std::string& dir);

// On-disk packed snapshot for over-memory exact scans. Built at shutdown
// after the write burst (and lazily on the first cold query if no valid
// file is present). Not built on COMMIT/autocommit, and not for tables
// that still fit in the in-memory FastScan path. Rows are a tight
// sequential layout: rec_size + pk + scalar mid columns + float32[dim].
// Crash recovery still uses the WAL; this file is a cache and is ignored
// unless its header matches the current log size and committed_max.
class PackedScan {
public:
    PackedScan() = default;
    ~PackedScan();
    PackedScan(const PackedScan&) = delete;
    PackedScan& operator=(const PackedScan&) = delete;

    bool try_open_existing(const std::string& path, uint32_t table_id, int dim,
                           uint64_t log_size, uint64_t committed_max);
    void start_build(const std::string& path, uint32_t table_id, int dim,
                     uint64_t log_size, uint64_t committed_max);
    void append_row(int64_t pk, const std::vector<ValType>& mid_types,
                    const int64_t* mid_i, const std::string_view* mid_s,
                    const float* vec);
    void finish_build();

    struct RowView {
        int64_t pk = 0;
        const float* vec = nullptr;
        const char* mid = nullptr;
        size_t mid_len = 0;
    };

    class Cursor {
    public:
        bool next();
        const RowView& row() const { return row_; }

    private:
        friend class PackedScan;
        PackedScan* s_ = nullptr;
        std::vector<ValType> mid_types_;
        int dim_ = 0;
        uint64_t off_ = 0;
        uint64_t end_ = 0;
        RowView row_;
    };
    Cursor cursor(const std::vector<ValType>& mid_types, int dim);

    void drop_cache();
    void close();
    bool ready() const { return ready_; }
    uint64_t n() const { return n_; }
    int dim() const { return dim_; }

private:
    void flush_wbuf();
    void evict_written();
    bool ensure_in_buf(uint64_t off, size_t n);
    const char* ptr_at(uint64_t off) const { return rbuf_.data() + (off - rbuf_off_); }
    void write_header(bool complete);
    void invalidate_read_buf();
    void drop_upto(uint64_t off);

    int fd_ = -1;
    std::string path_;
    std::string wbuf_;
    std::string rbuf_;
    uint64_t size_ = 0;
    uint64_t n_ = 0;
    int dim_ = 0;
    uint32_t table_id_ = 0;
    uint64_t log_size_ = 0;
    uint64_t committed_max_ = 0;
    uint64_t drop_pos_ = 0;
    uint64_t evict_pos_ = 0;
    uint64_t writeback_pos_ = 0;
    uint64_t durable_pos_ = 0;
    uint64_t rbuf_off_ = 0;
    size_t rbuf_len_ = 0;
    bool ready_ = false;
    bool writing_ = false;
    static constexpr uint32_t kMagic = 0x4E415350;  // PSAN
    static constexpr uint32_t kVersion = 1;
    static constexpr uint64_t kHeaderSize = 4096;
    static constexpr size_t kReadWindow = 2ull << 20;
    static constexpr size_t kWriteWindow = 8ull << 20;
};

}  // namespace mvdb
