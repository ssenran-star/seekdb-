#pragma once
#include <cstdint>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace mvdb {

enum class ValType : uint8_t { BIGINT = 0, VARCHAR = 1, VECTOR = 2 };

struct ColumnDef {
    std::string name;
    ValType type;
    uint32_t len = 0;
};

struct TableSchema {
    uint32_t table_id = 0;
    std::string name;
    std::vector<ColumnDef> cols;
    int pk_index = 0;
    int vector_index = -1;
    std::string ann_index_name;
};

using Vector = std::vector<float>;

struct Value {
    ValType type = ValType::BIGINT;
    int64_t i = 0;
    std::string s;
    Vector v;
};

using Row = std::vector<Value>;

struct DbError : std::runtime_error {
    std::string code;
    DbError(std::string c, const std::string& msg)
        : std::runtime_error(msg), code(std::move(c)) {}
};

inline size_t utf8_count(const std::string& s) {
    size_t n = 0;
    for (unsigned char c : s)
        if ((c & 0xC0) != 0x80) ++n;
    return n;
}

inline std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

inline float fast_parse_float(const char* s, const char** end) {
    const char* p = s;
    float sign = 1.0f;
    if (*p == '-') { sign = -1.0f; p++; }
    else if (*p == '+') p++;
    if (!(*p >= '0' && *p <= '9') && *p != '.') { *end = s; return 0; }
    float val = 0;
    while (*p >= '0' && *p <= '9') { val = val * 10.0f + (*p - '0'); p++; }
    if (*p == '.') {
        p++;
        float frac = 0.1f;
        while (*p >= '0' && *p <= '9') { val += (*p - '0') * frac; frac *= 0.1f; p++; }
    }
    if (*p == 'e' || *p == 'E' || *p == 'x' || *p == 'X' ||
        (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z')) {
        *end = s; return 0;
    }
    *end = p;
    return sign * val;
}

inline bool is_ascii_space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

inline Vector parse_vector_literal(const std::string& raw, uint32_t dim) {
    const char* s = raw.data();
    const char* e = s + raw.size();
    while (s < e && is_ascii_space(*s)) ++s;
    while (e > s && is_ascii_space(e[-1])) --e;
    if (e - s < 2 || *s != '[' || e[-1] != ']')
        throw DbError("invalid_vector", "vector literal must be bracketed");
    ++s;
    --e;
    while (s < e && is_ascii_space(*s)) ++s;
    while (e > s && is_ascii_space(e[-1])) --e;
    Vector out;
    out.reserve(dim);
    if (s >= e) {
        if (dim != 0) throw DbError("dimension_mismatch", "empty vector literal");
        return out;
    }
    while (s < e) {
        while (s < e && is_ascii_space(*s)) ++s;
        if (s >= e) throw DbError("invalid_vector", "empty vector element");
        const char* tok = s;
        const char* endp = nullptr;
        float f = fast_parse_float(tok, &endp);
        if (endp == tok) {
            errno = 0;
            char* end2 = nullptr;
            double d = std::strtod(const_cast<char*>(tok), &end2);
            if (end2 == tok)
                throw DbError("invalid_vector", "non-numeric vector element");
            endp = end2;
            if (std::isnan(d) || std::isinf(d))
                throw DbError("invalid_vector", "NaN/Infinity not allowed");
            if (d > std::numeric_limits<float>::max() || d < -std::numeric_limits<float>::max())
                throw DbError("invalid_vector", "float32 overflow");
            f = static_cast<float>(d);
            if (std::isinf(f)) throw DbError("invalid_vector", "float32 overflow");
        } else if (std::isnan(f) || std::isinf(f)) {
            throw DbError("invalid_vector", "NaN/Infinity not allowed");
        }
        s = endp;
        while (s < e && is_ascii_space(*s)) ++s;
        out.push_back(f);
        if (s >= e) break;
        if (*s != ',') throw DbError("invalid_vector", "non-numeric vector element");
        ++s;
    }
    if (out.size() != dim)
        throw DbError("dimension_mismatch", "vector dimension mismatch");
    return out;
}

inline double l2_distance(const Vector& a, const Vector& b) {
    const size_t n = a.size();
    double sum = 0.0;
    size_t k = 0;
    while (k < n) {
        size_t end = k + 64 < n ? k + 64 : n;
        float acc[8] = {0, 0, 0, 0, 0, 0, 0, 0};
        size_t j = k;
        for (; j + 8 <= end; j += 8) {
            for (int t = 0; t < 8; ++t) {
                float d = a[j + t] - b[j + t];
                acc[t] += d * d;
            }
        }
        for (int t = 0; t < 8; ++t) sum += acc[t];
        for (; j < end; ++j) {
            double d = static_cast<double>(a[j]) - static_cast<double>(b[j]);
            sum += d * d;
        }
        k = end;
    }
    return std::sqrt(sum);
}

struct Literal {
    enum Kind { INT, FLOAT, STRING } kind = INT;
    int64_t i = 0;
    double d = 0.0;
    std::string s;
};

inline Value coerce(const Literal& lit, const ColumnDef& col) {
    Value v;
    v.type = col.type;
    switch (col.type) {
        case ValType::BIGINT:
            if (lit.kind != Literal::INT)
                throw DbError("type_mismatch", "BIGINT column needs integer");
            v.i = lit.i;
            break;
        case ValType::VARCHAR:
            if (lit.kind != Literal::STRING)
                throw DbError("type_mismatch", "VARCHAR column needs string");
            if (utf8_count(lit.s) > col.len)
                throw DbError("varchar_too_long", "string exceeds VARCHAR length");
            v.s = lit.s;
            break;
        case ValType::VECTOR:
            if (lit.kind != Literal::STRING)
                throw DbError("type_mismatch", "VECTOR column needs '[...]' literal");
            v.v = parse_vector_literal(lit.s, col.len);
            break;
    }
    return v;
}

}  // namespace mvdb