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
}  // namespace

int main(int argc, char** argv) {
    std::string data_dir = "/data";
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
        std::string op = req.value("op", "");
        if (op == "shutdown") {
            engine.shutdown();
            resp["ok"] = true;
            std::cout << resp.dump() << std::endl;
            return 0;
        }
        if (op == "execute") {
            std::string sql = req.value("sql", "");
            std::string mode = req.value("search_mode", "exact");
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