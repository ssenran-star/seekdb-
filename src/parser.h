#pragma once
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "common.h"

namespace mvdb {

enum class StmtType {
    CREATE_TABLE,
    CREATE_INDEX,
    INSERT,
    SELECT,
    UPDATE,
    DELETE_ROWS,
    BEGIN,
    COMMIT,
    ROLLBACK
};

struct Predicate {
    std::string column;
    std::string op;  // = < <= > >=
    Literal literal;
};

struct SelectItem {
    bool is_distance = false;
    std::string column;
    std::string alias;
    Vector query_vec;
    std::string raw_vec;
};

struct Statement {
    StmtType type;
    std::string table;
    std::string index_name;
    std::string column;

    std::vector<ColumnDef> cols;
    std::string pk_col;

    std::vector<std::string> insert_cols;
    std::vector<std::vector<Literal>> rows;

    std::vector<SelectItem> items;
    bool star = false;
    std::vector<Predicate> where;
    bool has_order = false;
    std::string order_by;
    bool order_desc = false;
    bool has_limit = false;
    int64_t limit = 0;

    std::vector<std::pair<std::string, Literal>> sets;
};

Statement parse_sql(const std::string& sql);

}  // namespace mvdb