#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <iostream>
#include <string>

#include "../third_party/nlohmann/json.hpp"
#include "engine.h"

using json = nlohmann::json;

namespace {
json cell_to_json(const mvdb::Cell& c) {
    switch (c.kind) {
        case mvdb::Cell::INT: return json(c.i);
        case mvdb::Cell::STR: return json(c.s);
        case mvdb::Cell::VEC: return json(c.v);
        case mvdb::Cell::REAL: return json(c.d);
    }
    return json(nullptr);
}

bool skip_ws(const char*& s, const char* e) {
    while (s < e && (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')) ++s;
    return s < e;
}

bool parse_json_string(const char*& s, const char* e, std::string& out) {
    if (s >= e || *s != '"') return false;
    ++s;
    const char* start = s;
    while (s < e) {
        if (*s == '\\') return false;  // escaped; use nlohmann
        if (*s == '"') {
            out.assign(start, s);
            ++s;
            return true;
        }
        ++s;
    }
    return false;
}

bool parse_json_raw_value(const char*& s, const char* e, std::string& out) {
    if (!skip_ws(s, e)) return false;
    if (*s == '"') {
        const char* start = s;
        std::string tmp;
        if (!parse_json_string(s, e, tmp)) return false;
        out.assign(start, s);
        return true;
    }
    if (*s == 'n' && s + 4 <= e && std::strncmp(s, "null", 4) == 0) {
        out.assign("null");
        s += 4;
        return true;
    }
    const char* start = s;
    if (*s == '-') ++s;
    if (s >= e || *s < '0' || *s > '9') return false;
    while (s < e && *s >= '0' && *s <= '9') ++s;
    if (s < e && *s == '.') {
        ++s;
        while (s < e && *s >= '0' && *s <= '9') ++s;
    }
    out.assign(start, s);
    return true;
}

bool parse_execute_line(const std::string& line, std::string& id_raw, std::string& op,
                        std::string& sql, std::string& mode) {
    const char* s = line.data();
    const char* e = s + line.size();
    if (!skip_ws(s, e) || *s != '{') return false;
    ++s;
    id_raw = "null";
    op.clear();
    sql.clear();
    mode = "exact";
    bool first = true;
    while (skip_ws(s, e) && *s != '}') {
        if (!first) {
            if (*s != ',') return false;
            ++s;
            if (!skip_ws(s, e)) return false;
            if (*s == '}') break;
        }
        first = false;
        std::string key;
        if (!parse_json_string(s, e, key)) return false;
        if (!skip_ws(s, e) || *s != ':') return false;
        ++s;
        if (key == "id") {
            if (!parse_json_raw_value(s, e, id_raw)) return false;
        } else if (key == "op") {
            if (!parse_json_string(s, e, op)) return false;
        } else if (key == "sql") {
            if (!parse_json_string(s, e, sql)) return false;
        } else if (key == "search_mode") {
            if (!parse_json_string(s, e, mode)) return false;
        } else {
            if (!skip_ws(s, e)) return false;
            if (*s == '"') {
                std::string tmp;
                if (!parse_json_string(s, e, tmp)) return false;
            } else if (*s == '{' || *s == '[') {
                return false;
            } else {
                std::string tmp;
                if (!parse_json_raw_value(s, e, tmp)) return false;
            }
        }
    }
    return skip_ws(s, e) && *s == '}';
}

void write_json_string(std::ostream& os, const std::string& s) {
    os << '"';
    for (char c : s) {
        if (c == '"' || c == '\\') os << '\\';
        os << c;
    }
    os << '"';
}

void write_query_ok(std::ostream& os, const std::string& id_raw, const mvdb::ExecResult& r) {
    os << "{\"id\":" << id_raw << ",\"ok\":true,\"columns\":[";
    for (size_t i = 0; i < r.columns.size(); ++i) {
        if (i) os << ',';
        write_json_string(os, r.columns[i]);
    }
    os << "],\"rows\":[";
    for (size_t i = 0; i < r.rows.size(); ++i) {
        if (i) os << ',';
        os << '[';
        for (size_t j = 0; j < r.rows[i].size(); ++j) {
            if (j) os << ',';
            const auto& c = r.rows[i][j];
            switch (c.kind) {
                case mvdb::Cell::INT:
                    os << c.i;
                    break;
                case mvdb::Cell::REAL: {
                    char buf[64];
                    std::snprintf(buf, sizeof(buf), "%.17g", c.d);
                    os << buf;
                    break;
                }
                case mvdb::Cell::STR:
                    write_json_string(os, c.s);
                    break;
                case mvdb::Cell::VEC:
                    os << '[';
                    for (size_t k = 0; k < c.v.size(); ++k) {
                        if (k) os << ',';
                        char buf[64];
                        std::snprintf(buf, sizeof(buf), "%.17g", static_cast<double>(c.v[k]));
                        os << buf;
                    }
                    os << ']';
                    break;
            }
        }
        os << ']';
    }
    os << "]}\n";
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
        std::cerr << "open failed: " << e.what() << std::endl;
        return 1;
    }
    std::cout << "{\"type\":\"ready\"}" << std::endl;

    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;
        std::string id_raw, op, sql, mode;
        bool fast = parse_execute_line(line, id_raw, op, sql, mode);
        if (fast && op == "shutdown") {
            engine.shutdown();
            std::cout << "{\"id\":" << id_raw << ",\"ok\":true}" << std::endl;
            return 0;
        }
        if (fast && op == "execute") {
            try {
                mvdb::ExecResult r = engine.execute(sql, mode);
                if (r.is_query) {
                    write_query_ok(std::cout, id_raw, r);
                } else {
                    std::cout << "{\"id\":" << id_raw << ",\"ok\":true,\"affected_rows\":"
                              << r.affected << "}" << std::endl;
                    continue;
                }
            } catch (const mvdb::DbError& e) {
                json resp;
                resp["id"] = json::parse(id_raw);
                resp["ok"] = false;
                resp["error"] = {{"code", e.code}, {"message", e.what()}};
                std::cout << resp.dump() << std::endl;
                continue;
            } catch (const std::exception& e) {
                json resp;
                resp["id"] = json::parse(id_raw);
                resp["ok"] = false;
                resp["error"] = {{"code", "internal_error"}, {"message", e.what()}};
                std::cout << resp.dump() << std::endl;
                continue;
            }
            std::cout.flush();
            continue;
        }
        json req;
        json resp;
        try {
            req = json::parse(line);
        } catch (const std::exception&) {
            resp["id"] = nullptr;
            resp["ok"] = false;
            resp["error"] = {{"code", "invalid_request"}, {"message", "request is not valid JSON"}};
            std::cout << resp.dump() << std::endl;
            continue;
        }
        resp["id"] = req.contains("id") ? req["id"] : json(nullptr);
        op = req.value("op", "");
        if (op == "shutdown") {
            engine.shutdown();
            resp["ok"] = true;
            std::cout << resp.dump() << std::endl;
            return 0;
        }
        if (op == "execute") {
            sql = req.value("sql", "");
            mode = req.value("search_mode", "exact");
            try {
                mvdb::ExecResult r = engine.execute(sql, mode);
                resp["ok"] = true;
                if (r.is_query) {
                    resp["columns"] = r.columns;
                    json rows = json::array();
                    for (auto& row : r.rows) {
                        json jrow = json::array();
                        for (auto& c : row) jrow.push_back(cell_to_json(c));
                        rows.push_back(std::move(jrow));
                    }
                    resp["rows"] = std::move(rows);
                } else {
                    resp["affected_rows"] = r.affected;
                }
            } catch (const mvdb::DbError& e) {
                resp["ok"] = false;
                resp["error"] = {{"code", e.code}, {"message", e.what()}};
            } catch (const std::exception& e) {
                resp["ok"] = false;
                resp["error"] = {{"code", "internal_error"}, {"message", e.what()}};
            }
        } else {
            resp["ok"] = false;
            resp["error"] = {{"code", "invalid_request"}, {"message", "unknown op"}};
        }
        std::cout << resp.dump() << std::endl;
    }
    engine.shutdown();
    return 0;
}
