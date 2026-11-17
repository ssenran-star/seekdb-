#pragma once
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>

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

    uint8_t get_u8() { return read<uint8_t>(); }
    uint32_t get_u32() { return read<uint32_t>(); }
    uint64_t get_u64() { return read<uint64_t>(); }
    int64_t get_i64() { return read<int64_t>(); }
    std::string get_str() {
        uint32_t n = get_u32();
        check(n);
        std::string s = data.substr(pos_, n);
        pos_ += n;
        return s;
    }
    Vector get_vec() {
        uint32_t n = get_u32();
        check(n * sizeof(float));
        Vector v(n);
        std::memcpy(v.data(), data.data() + pos_, n * sizeof(float));
        pos_ += n * sizeof(float);
        return v;
    }
    Row get_row(size_t ncols) {
        Row row;
        row.reserve(ncols);
        for (size_t k = 0; k < ncols; ++k) {
            Value v;
            v.type = static_cast<ValType>(get_u8());
            switch (v.type) {
                case ValType::BIGINT: v.i = get_i64(); break;
                case ValType::VARCHAR: v.s = get_str(); break;
                case ValType::VECTOR: v.v = get_vec(); break;
                default: throw DbError("internal_error", "bad value type in log");
            }
            row.push_back(std::move(v));
        }
        return row;
    }
    bool done() const { return pos_ >= data.size(); }

private:
    void append(const void* p, size_t n) {
        data.append(static_cast<const char*>(p), n);
    }
    template <typename T>
    T read() {
        check(sizeof(T));
        T v;
        std::memcpy(&v, data.data() + pos_, sizeof(T));
        pos_ += sizeof(T);
        return v;
    }
    void check(size_t n) const {
        if (pos_ + n > data.size()) throw DbError("internal_error", "log record truncated");
    }
    size_t pos_ = 0;
};

struct RecordView {
    uint64_t offset = 0;
    uint64_t txn = 0;
    uint32_t stmt = 0;
    uint8_t type = 0;
    std::string payload;
};

class Log {
public:
    ~Log();
    void open(const std::string& dir);
    uint64_t append(uint8_t type, uint64_t txn, uint32_t stmt, const std::string& payload);
    void sync();
    void drop_cache();
    void scan(const std::function<void(const RecordView&)>& cb);
    RecordView read_at(uint64_t offset);
    void close();
    uint64_t size() const { return size_; }

private:
    void flush_buf();
    int fd_ = -1;
    std::string path_;
    std::string wbuf_;
    uint64_t size_ = 0;
    uint64_t drop_pos_ = 0;
};

void sync_dir(const std::string& dir);

}  // namespace mvdb