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

inline size_t utf8_count(const char* s, size_t n) {
    size_t c = 0;
    for (size_t i = 0; i < n; ++i)
        if ((static_cast<unsigned char>(s[i]) & 0xC0) != 0x80) ++c;
    return c;
}

inline size_t utf8_count(const std::string& s) { return utf8_count(s.data(), s.size()); }

inline std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

// ASCII case-insensitive equality without allocating.
inline bool ieq_ascii(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        char x = a[i], y = b[i];
        if (x >= 'a' && x <= 'z') x = static_cast<char>(x - 'a' + 'A');
        if (y >= 'a' && y <= 'z') y = static_cast<char>(y - 'a' + 'A');
        if (x != y) return false;
    }
    return true;
}

inline bool ieq_ascii(const std::string& a, const char* b) {
    size_t i = 0;
    for (; i < a.size(); ++i) {
        char x = a[i];
        if (x >= 'a' && x <= 'z') x = static_cast<char>(x - 'a' + 'A');
        if (!b[i] || x != b[i]) return false;
    }
    return b[i] == '\0';
}

inline bool fast_parse_float(const char* s, const char* lim, const char** end, float* out) {
    // Exact for <= 15 significant decimal digits (integer numerator stays within
    // 2^53 and a single IEEE division rounds once) - matches strtod->float cast.
    const char* p = s;
    double sign = 1.0;
    if (p < lim && *p == '-') { sign = -1.0; ++p; }
    else if (p < lim && *p == '+') ++p;
    if (!(p < lim && ((*p >= '0' && *p <= '9') || *p == '.'))) { *end = s; return false; }
    double num = 0;
    int digits = 0;
    bool any = false;
    while (p < lim && *p >= '0' && *p <= '9') { num = num * 10.0 + (*p - '0'); ++p; ++digits; any = true; }
    double scale = 1.0;
    if (p < lim && *p == '.') {
        ++p;
        while (p < lim && *p >= '0' && *p <= '9') {
            num = num * 10.0 + (*p - '0');
            scale *= 10.0;
            ++p; ++digits; any = true;
        }
    }
    if (!any || digits > 15) { *end = s; return false; }
    if (p < lim && (*p == 'e' || *p == 'E' || *p == 'x' || *p == 'X' ||
                    (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z'))) {
        *end = s; return false;
    }
    *end = p;
    *out = static_cast<float>(sign * num / scale);
    return true;
}

inline Vector parse_vector_literal(const char* base, size_t len, uint32_t dim) {
    const char* p = base;
    const char* end = base + len;
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) ++p;
    const char* e = end;
    while (e > p && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) --e;
    if (e - p < 2 || *p != '[' || e[-1] != ']')
        throw DbError("invalid_vector", "vector literal must be bracketed");
    ++p;
    --e;  // interior span [p, e)
    Vector out;
    const char* q = p;
    while (q < e && (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n')) ++q;
    if (q >= e) {
        if (dim != 0) throw DbError("dimension_mismatch", "empty vector literal");
        return out;
    }
    out.reserve(dim ? dim : 8);
    while (true) {
        while (p < e && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) ++p;
        if (p >= e) throw DbError("invalid_vector", "empty vector element");
        const char* endp = nullptr;
        float f = 0;
        if (!fast_parse_float(p, e, &endp, &f)) {
            errno = 0;
            char* end2 = nullptr;
            double d = std::strtod(p, &end2);
            if (end2 == p || end2 > e)
                throw DbError("invalid_vector", "non-numeric vector element");
            if (std::isnan(d) || std::isinf(d))
                throw DbError("invalid_vector", "NaN/Infinity not allowed");
            if (d > std::numeric_limits<float>::max() || d < -std::numeric_limits<float>::max())
                throw DbError("invalid_vector", "float32 overflow");
            f = static_cast<float>(d);
            if (std::isinf(f)) throw DbError("invalid_vector", "float32 overflow");
            endp = end2;
        } else {
            if (std::isnan(f) || std::isinf(f))
                throw DbError("invalid_vector", "NaN/Infinity not allowed");
        }
        out.push_back(f);
        p = endp;
        while (p < e && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) ++p;
        if (p >= e) break;
        if (*p == ',') { ++p; continue; }
        throw DbError("invalid_vector", "non-numeric vector element");
    }
    if (out.size() != dim)
        throw DbError("dimension_mismatch", "vector dimension mismatch");
    return out;
}

inline Vector parse_vector_literal(const std::string& raw, uint32_t dim) {
    return parse_vector_literal(raw.data(), raw.size(), dim);
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
    // Optional view into the source SQL. Valid for the duration of execute().
    const char* p = nullptr;
    size_t n = 0;
    const char* data() const { return p ? p : s.data(); }
    size_t size() const { return p ? n : s.size(); }
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
            if (utf8_count(lit.data(), lit.size()) > col.len)
                throw DbError("varchar_too_long", "string exceeds VARCHAR length");
            v.s.assign(lit.data(), lit.size());
            break;
        case ValType::VECTOR:
            if (lit.kind != Literal::STRING)
                throw DbError("type_mismatch", "VECTOR column needs '[...]' literal");
            v.v = parse_vector_literal(lit.data(), lit.size(), col.len);
            break;
    }
    return v;
}

}  // namespace mvdb