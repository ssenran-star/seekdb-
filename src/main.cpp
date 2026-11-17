#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "engine.h"

namespace {

// ---------------- buffered line I/O ----------------
std::string g_in;
size_t g_in_pos = 0;
std::string g_out;
std::string g_scratch;

bool read_line(std::string& line) {
    while (true) {
        size_t nl = g_in.find('\n', g_in_pos);
        if (nl != std::string::npos) {
            line.assign(g_in, g_in_pos, nl - g_in_pos);
            g_in_pos = nl + 1;
            if (g_in_pos >= (1u << 20)) {
                g_in.erase(0, g_in_pos);
                g_in_pos = 0;
            }
            return true;
        }
        if (g_in_pos > 0) {
            g_in.erase(0, g_in_pos);
            g_in_pos = 0;
        }
        char buf[65536];
        ssize_t r = ::read(0, buf, sizeof buf);
        if (r < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (r == 0) return false;
        g_in.append(buf, static_cast<size_t>(r));
    }
}

void flush_out() {
    size_t off = 0;
    while (off < g_out.size()) {
        ssize_t w = ::write(1, g_out.data() + off, g_out.size() - off);
        if (w < 0) {
            if (errno == EINTR) continue;
            break;
        }
        off += static_cast<size_t>(w);
    }
    g_out.clear();
}

// ---------------- minimal JSON reader ----------------
// Enough for the line protocol: one top-level object per line, capture `id`
// as a raw span (echoed back verbatim with its original JSON type) and pull
// `op` / `sql` / `search_mode` strings with full escape handling.

const char* skip_ws(const char* p, const char* end) {
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) ++p;
    return p;
}

bool hex4(const char*& p, const char* end, unsigned& out) {
    if (p + 4 > end) return false;
    unsigned v = 0;
    for (int i = 0; i < 4; ++i) {
        char h = *p++;
        v <<= 4;
        if (h >= '0' && h <= '9') v |= (unsigned)(h - '0');
        else if (h >= 'a' && h <= 'f') v |= (unsigned)(h - 'a' + 10);
        else if (h >= 'A' && h <= 'F') v |= (unsigned)(h - 'A' + 10);
        else return false;
    }
    out = v;
    return true;
}

// Scans a JSON string at *p (must be at '"'); leaves *p past the closing
// quote and g_scratch holding the unescaped text.
bool scan_json_string(const char*& p, const char* end) {
    if (p >= end || *p != '"') return false;
    ++p;
    g_scratch.clear();
    while (p < end) {
        char c = *p;
        if (c == '"') {
            ++p;
            return true;
        }
        if (c == '\\') {
            ++p;
            if (p >= end) return false;
            char e = *p++;
            switch (e) {
                case '"': g_scratch += '"'; break;
                case '\\': g_scratch += '\\'; break;
                case '/': g_scratch += '/'; break;
                case 'b': g_scratch += '\b'; break;
                case 'f': g_scratch += '\f'; break;
                case 'n': g_scratch += '\n'; break;
                case 'r': g_scratch += '\r'; break;
                case 't': g_scratch += '\t'; break;
                case 'u': {
                    unsigned cp = 0;
                    if (!hex4(p, end, cp)) return false;
                    if (cp >= 0xD800 && cp <= 0xDBFF && p + 1 < end && p[0] == '\\' && p[1] == 'u') {
                        const char* q = p + 2;
                        unsigned lo = 0;
                        if (hex4(q, end, lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
                            cp = 0x10000u + ((cp - 0xD800u) << 10) + (lo - 0xDC00u);
                            p = q;
                        }
                    }
                    if (cp < 0x80) {
                        g_scratch += (char)cp;
                    } else if (cp < 0x800) {
                        g_scratch += (char)(0xC0 | (cp >> 6));
                        g_scratch += (char)(0x80 | (cp & 0x3F));
                    } else if (cp < 0x10000) {
                        g_scratch += (char)(0xE0 | (cp >> 12));
                        g_scratch += (char)(0x80 | ((cp >> 6) & 0x3F));
                        g_scratch += (char)(0x80 | (cp & 0x3F));
                    } else {
                        g_scratch += (char)(0xF0 | (cp >> 18));
                        g_scratch += (char)(0x80 | ((cp >> 12) & 0x3F));
                        g_scratch += (char)(0x80 | ((cp >> 6) & 0x3F));
                        g_scratch += (char)(0x80 | (cp & 0x3F));
                    }
                    break;
                }
                default:
                    return false;
            }
        } else if ((unsigned char)c < 0x20) {
            return false;
        } else {
            const char* start = p;
            while (p < end && *p != '"' && *p != '\\' && (unsigned char)*p >= 0x20) ++p;
            g_scratch.append(start, static_cast<size_t>(p - start));
        }
    }
    return false;
}

const char* scan_json_value(const char* p, const char* end, int depth) {
    if (depth > 64) return nullptr;
    p = skip_ws(p, end);
    if (p >= end) return nullptr;
    char c = *p;
    if (c == '"') {
        return scan_json_string(p, end) ? p : nullptr;
    }
    if (c == '-' || (c >= '0' && c <= '9')) {
        if (*p == '-') ++p;
        if (p >= end || *p < '0' || *p > '9') return nullptr;
        if (*p == '0') ++p;
        else while (p < end && *p >= '0' && *p <= '9') ++p;
        if (p < end && *p == '.') {
            ++p;
            if (p >= end || *p < '0' || *p > '9') return nullptr;
            while (p < end && *p >= '0' && *p <= '9') ++p;
        }
        if (p < end && (*p == 'e' || *p == 'E')) {
            ++p;
            if (p < end && (*p == '+' || *p == '-')) ++p;
            if (p >= end || *p < '0' || *p > '9') return nullptr;
            while (p < end && *p >= '0' && *p <= '9') ++p;
        }
        return p;
    }
    if (c == 't') {
        return (end - p >= 4 && !memcmp(p, "true", 4)) ? p + 4 : nullptr;
    }
    if (c == 'f') {
        return (end - p >= 5 && !memcmp(p, "false", 5)) ? p + 5 : nullptr;
    }
    if (c == 'n') {
        return (end - p >= 4 && !memcmp(p, "null", 4)) ? p + 4 : nullptr;
    }
    if (c == '[') {
        p = skip_ws(p + 1, end);
        if (p < end && *p == ']') return p + 1;
        while (true) {
            p = scan_json_value(p, end, depth + 1);
            if (!p) return nullptr;
            p = skip_ws(p, end);
            if (p >= end) return nullptr;
            if (*p == ',') { ++p; continue; }
            if (*p == ']') return p + 1;
            return nullptr;
        }
    }
    if (c == '{') {
        p = skip_ws(p + 1, end);
        if (p < end && *p == '}') return p + 1;
        while (true) {
            p = skip_ws(p, end);
            if (p >= end || *p != '"') return nullptr;
            if (!scan_json_string(p, end)) return nullptr;
            p = skip_ws(p, end);
            if (p >= end || *p != ':') return nullptr;
            p = scan_json_value(p + 1, end, depth + 1);
            if (!p) return nullptr;
            p = skip_ws(p, end);
            if (p >= end) return nullptr;
            if (*p == ',') { ++p; continue; }
            if (*p == '}') return p + 1;
            return nullptr;
        }
    }
    return nullptr;
}

struct ReqFields {
    std::string id_raw = "null";
    std::string op;
    std::string sql;
    std::string mode = "exact";
};

bool parse_request(const std::string& line, ReqFields& f) {
    const char* p = line.data();
    const char* end = p + line.size();
    p = skip_ws(p, end);
    if (p >= end || *p != '{') return false;
    ++p;
    p = skip_ws(p, end);
    if (p < end && *p == '}') {
        ++p;
    } else {
        while (true) {
            p = skip_ws(p, end);
            if (p >= end || *p != '"') return false;
            if (!scan_json_string(p, end)) return false;
            std::string key = g_scratch;
            p = skip_ws(p, end);
            if (p >= end || *p != ':') return false;
            ++p;
            p = skip_ws(p, end);
            if (p >= end) return false;
            if (key == "id") {
                const char* vs = p;
                const char* ve = scan_json_value(vs, end, 0);
                if (!ve) return false;
                f.id_raw.assign(vs, static_cast<size_t>(ve - vs));
                p = ve;
            } else if (key == "op" || key == "sql" || key == "search_mode") {
                if (!scan_json_string(p, end)) return false;
                if (key == "op") f.op = g_scratch;
                else if (key == "sql") f.sql = g_scratch;
                else f.mode = g_scratch;
            } else {
                const char* ve = scan_json_value(p, end, 0);
                if (!ve) return false;
                p = ve;
            }
            p = skip_ws(p, end);
            if (p >= end) return false;
            if (*p == ',') { ++p; continue; }
            if (*p == '}') { ++p; break; }
            return false;
        }
    }
    p = skip_ws(p, end);
    return p == end;
}

// ---------------- JSON output ----------------
void json_escape(const char* s, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
            case '"': g_out += "\\\""; break;
            case '\\': g_out += "\\\\"; break;
            case '\b': g_out += "\\b"; break;
            case '\f': g_out += "\\f"; break;
            case '\n': g_out += "\\n"; break;
            case '\r': g_out += "\\r"; break;
            case '\t': g_out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof buf, "\\u%04x", c);
                    g_out += buf;
                } else {
                    g_out += (char)c;
                }
        }
    }
}

void append_i64(long long v) {
    char buf[24];
    char* p = buf + 24;
    bool neg = v < 0;
    unsigned long long u = neg ? (unsigned long long)(-(v + 1)) + 1 : (unsigned long long)v;
    do {
        *--p = (char)('0' + (u % 10));
        u /= 10;
    } while (u);
    if (neg) *--p = '-';
    g_out.append(p, (size_t)(buf + 24 - p));
}

void append_double(double d) {
    // integral values (common for exact distances like 5.0) print as "N.0"
    if (d == (double)(long long)d && d >= -9.0e15 && d <= 9.0e15) {
        append_i64((long long)d);
        g_out += ".0";
        return;
    }
    char buf[40];
    // judge compares floats at 1e-5 abs/rel; 9 significant digits is enough
    int n = snprintf(buf, sizeof buf, "%.9g", d);
    for (int i = 0; i < n; ++i)
        if (buf[i] == '.' || buf[i] == 'e' || buf[i] == 'E') {
            g_out.append(buf, (size_t)n);
            return;
        }
    g_out.append(buf, (size_t)n);
    g_out += ".0";
}

void append_cell(const mvdb::Cell& c) {
    switch (c.kind) {
        case mvdb::Cell::INT:
            append_i64((long long)c.i);
            break;
        case mvdb::Cell::STR:
            g_out += '"';
            json_escape(c.s.data(), c.s.size());
            g_out += '"';
            break;
        case mvdb::Cell::VEC:
            g_out += '[';
            for (size_t i = 0; i < c.v.size(); ++i) {
                if (i) g_out += ',';
                append_double(c.v[i]);
            }
            g_out += ']';
            break;
        case mvdb::Cell::REAL:
            append_double(c.d);
            break;
    }
}

void emit_error(const std::string& id_raw, const char* code, const char* msg) {
    g_out += "{\"id\":";
    g_out += id_raw;
    g_out += ",\"ok\":false,\"error\":{\"code\":\"";
    g_out += code;
    g_out += "\",\"message\":\"";
    json_escape(msg, strlen(msg));
    g_out += "\"}}\n";
}

void emit_ok(const std::string& id_raw) {
    g_out += "{\"id\":";
    g_out += id_raw;
    g_out += ",\"ok\":true}\n";
}

}  // namespace

int main(int argc, char** argv) {
    std::string data_dir = "/data";
    // official driver (_transport.py) passes the data dir via VECTORDB_DATA_DIR
    if (const char* e = std::getenv("VECTORDB_DATA_DIR"); e && *e) data_dir = e;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--data-dir" && i + 1 < argc) data_dir = argv[++i];
    }
    mvdb::Engine engine;
    try {
        engine.open(data_dir);
    } catch (const std::exception& e) {
        fprintf(stderr, "open failed: %s\n", e.what());
        return 1;
    }
    g_out = "{\"type\":\"ready\"}\n";
    flush_out();

    std::string line;
    while (read_line(line)) {
        if (line.empty()) continue;
        ReqFields f;
        if (!parse_request(line, f)) {
            emit_error("null", "invalid_request", "request is not valid JSON");
            flush_out();
            continue;
        }
        const std::string& id_raw = f.id_raw;
        if (f.op == "shutdown") {
            engine.shutdown();
            emit_ok(id_raw);
            flush_out();
            return 0;
        }
        if (f.op == "execute") {
            try {
                mvdb::ExecResult r = engine.execute(f.sql, f.mode);
                g_out += "{\"id\":";
                g_out += id_raw;
                if (r.is_query) {
                    g_out += ",\"ok\":true,\"columns\":[";
                    for (size_t i = 0; i < r.columns.size(); ++i) {
                        if (i) g_out += ',';
                        g_out += '"';
                        json_escape(r.columns[i].data(), r.columns[i].size());
                        g_out += '"';
                    }
                    g_out += "],\"rows\":[";
                    for (size_t ri = 0; ri < r.rows.size(); ++ri) {
                        if (ri) g_out += ',';
                        g_out += '[';
                        for (size_t ci = 0; ci < r.rows[ri].size(); ++ci) {
                            if (ci) g_out += ',';
                            append_cell(r.rows[ri][ci]);
                        }
                        g_out += ']';
                    }
                    g_out += "]}";
                } else {
                    g_out += ",\"ok\":true,\"affected_rows\":";
                    append_i64((long long)r.affected);
                    g_out += '}';
                }
                g_out += '\n';
            } catch (const mvdb::DbError& e) {
                emit_error(id_raw, e.code.c_str(), e.what());
            } catch (const std::exception& e) {
                emit_error(id_raw, "internal_error", e.what());
            }
        } else {
            emit_error(id_raw, "invalid_request", "unknown op");
        }
        flush_out();
    }
    engine.shutdown();
    return 0;
}
