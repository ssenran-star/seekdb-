/**
 * Copyright (c) 2021 OceanBase
 * OceanBase is licensed under MulanPSL - 2.0.
 * You can use this software according to the terms and conditions of the MulanPSL - 2.0 license.
 * You may obtain a copy of MulanPSL - 2.0 at:
 *          http://license.coscl.org.cn/MulanPSL - 2.0
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the MulanPSL - 2.0 license for more details.
 */

#include "share/ai_service/ob_ai_column_def.h"

#include "lib/container/ob_array.h"
#include "lib/string/ob_sql_string.h"
#include "common/mysqlclient/ob_mysql_proxy.h"
#include "common/mysqlclient/ob_mysql_result.h"
#include "share/inner_table/ob_inner_table_schema.h"
#include "sql/session/ob_basic_session_info.h"
#include "sql/session/ob_sql_session_info.h"

namespace oceanbase {
namespace share {

using namespace oceanbase::common;
using namespace oceanbase::sql;

ObAiColumnRegistry::ObAiColumnRegistry()
  : count_(0), lock_(common::ObLatchIds::DEFAULT_SPIN_RWLOCK)
{
  for (int64_t i = 0; i < MAX_AI_COLUMN_TABLES; i++) {
    table_ids_[i] = 0;
  }
}

ObAiColumnRegistry &ObAiColumnRegistry::get_instance()
{
  static ObAiColumnRegistry instance;
  return instance;
}

int ObAiColumnRegistry::register_table(uint64_t table_id)
{
  int ret = OB_SUCCESS;
  common::SpinWLockGuard guard(lock_);
  bool found = false;
  for (int64_t i = 0; OB_SUCC(ret) && !found && i < count_; i++) {
    if (table_ids_[i] == table_id) {
      found = true;
    }
  }
  if (OB_SUCC(ret) && !found) {
    if (count_ >= MAX_AI_COLUMN_TABLES) {
      ret = OB_SIZE_OVERFLOW;
      SHARE_LOG(WARN, "ai column registry is full", K(table_id), K_(count));
    } else {
      table_ids_[count_++] = table_id;
    }
  }
  return ret;
}

int ObAiColumnRegistry::unregister_table(uint64_t table_id)
{
  int ret = OB_SUCCESS;
  common::SpinWLockGuard guard(lock_);
  for (int64_t i = 0; i < count_; i++) {
    if (table_ids_[i] == table_id) {
      table_ids_[i] = table_ids_[count_ - 1];
      count_--;
      break;
    }
  }
  return ret;
}

bool ObAiColumnRegistry::has_ai_columns(uint64_t table_id) const
{
  bool found = false;
  common::SpinRLockGuard guard(lock_);
  for (int64_t i = 0; !found && i < count_; i++) {
    if (table_ids_[i] == table_id) {
      found = true;
    }
  }
  return found;
}

int64_t ObAiColumnRegistry::table_count() const
{
  common::SpinRLockGuard guard(lock_);
  return count_;
}


} // namespace share

namespace share {

// ---------------------------------------------------------------------------
// The strict read check of the AI columns.
//
// The check is column wide: a statement that references an AI column fails
// when that column is not completely generated yet, no matter which rows the
// statement selects.
//
// An AI column is ready when all of its rows have a value. A column with a
// NULL value has either a pending regeneration task or a task that failed for
// good, in both cases the value is not available yet.
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// The strict read check of the AI columns.
//
// The check is column wide: a statement that references an AI column fails
// when that column is not completely generated yet, no matter which rows the
// statement selects.
//
// An AI column is ready when all of its rows have a value. A column with a
// NULL value has either a pending regeneration task or a task that failed for
// good, in both cases the value is not available yet.
//
// The columns a statement references are recognised in the original text of
// the statement. The resolver keeps the referenced columns on the logical
// statement, but that statement is not reachable from every execution path,
// while the statement text always is.
// ---------------------------------------------------------------------------
namespace {
// True when name appears in the SQL text as a whole identifier.
bool ai_sql_text_references(const ObString &sql_text, const char *name, int64_t name_len)
{
  bool found = false;
  const char *text = sql_text.ptr();
  const int64_t len = sql_text.length();
  for (int64_t i = 0; !found && i + name_len <= len; ++i) {
    const char c = text[i];
    if (i > 0) {
      const char prev = text[i - 1];
      if (('a' <= prev && prev <= 'z') || ('A' <= prev && prev <= 'Z')
          || ('0' <= prev && prev <= '9') || '_' == prev || '$' == prev) {
        continue;
      }
    }
    if (i + name_len < len) {
      const char next = text[i + name_len];
      if (('a' <= next && next <= 'z') || ('A' <= next && next <= 'Z')
          || ('0' <= next && next <= '9') || '_' == next || '$' == next) {
        continue;
      }
    }
    bool match = true;
    for (int64_t k = 0; k < name_len; ++k) {
      char d = text[i + k];
      if (d >= 'A' && d <= 'Z') { d = (char)(d + 32); }
      char e = name[k];
      if (e >= 'A' && e <= 'Z') { e = (char)(e + 32); }
      if (d != e) { match = false; break; }
    }
    if (match) { found = true; }
  }
  return found;
}
} // namespace

int ObAiColumnStrictReadChecker::check_strict_read(
    sql::ObSQLSessionInfo *session,
    common::ObMySQLProxy *proxy,
    const common::ObString &sql_text)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(session) || OB_ISNULL(proxy) || sql_text.length() == 0) {
    // nothing to check
  } else {
    bool strict_read = false;
    if (OB_FAIL(session->get_ai_column_strict_read(strict_read))) {
      LOG_WARN("get ai column strict read failed", K(ret));
    } else if (!strict_read) {
      // the strict read is off, this is the common case
    } else {
      // One query lists every AI column of the server together with the
      // qualified name of its table.
      ObSqlString meta_sql;
      if (OB_FAIL(meta_sql.assign_fmt(
          "SELECT a.table_id AS table_id, a.column_id AS column_id, "
          "a.column_name AS column_name, t.table_name AS tbl_name, "
          "d.database_name AS db_name "
          "FROM oceanbase.%s a JOIN oceanbase.%s t ON t.table_id = a.table_id "
          "JOIN oceanbase.%s d ON d.database_id = t.database_id",
          OB_ALL_AI_COLUMN_TNAME, OB_ALL_TABLE_TNAME,
          OB_ALL_DATABASE_TNAME))) {
        LOG_WARN("build strict read meta sql failed", K(ret));
      } else {
        SMART_VAR(ObMySQLProxy::MySQLResult, res) {
          common::sqlclient::ObMySQLResult *result = NULL;
          if (OB_FAIL(proxy->read(res, meta_sql.ptr()))) {
            LOG_WARN("read strict read meta failed", K(ret), K(meta_sql));
          } else if (OB_ISNULL(result = res.get_result())) {
            ret = OB_ERR_UNEXPECTED;
          } else {
            while (OB_SUCC(ret) && OB_SUCC(result->next())) {
              int64_t table_id = 0;
              int64_t column_id = 0;
              ObString column_name;
              ObString tbl_name;
              ObString db_name;
              if (OB_FAIL(result->get_int("table_id", table_id))) {
                LOG_WARN("get strict read table id failed", K(ret));
              } else if (OB_FAIL(result->get_int("column_id", column_id))) {
                LOG_WARN("get strict read column id failed", K(ret));
              } else if (OB_FAIL(result->get_varchar("column_name", column_name))) {
                LOG_WARN("get strict read column name failed", K(ret));
              } else if (OB_FAIL(result->get_varchar("tbl_name", tbl_name))) {
                LOG_WARN("get strict read table name failed", K(ret));
              } else if (OB_FAIL(result->get_varchar("db_name", db_name))) {
                LOG_WARN("get strict read database name failed", K(ret));
              } else if (!ai_sql_text_references(sql_text, column_name.ptr(),
                                                  column_name.length())) {
                // The statement does not reference this AI column.
              } else {
                ObSqlString count_sql;
                int64_t unfinished = 0;
                if (OB_FAIL(count_sql.assign_fmt(
                    "SELECT count(*) AS unfinished FROM `%.*s`.`%.*s` "
                    "WHERE `%.*s` IS NULL",
                    db_name.length(), db_name.ptr(),
                    tbl_name.length(), tbl_name.ptr(),
                    column_name.length(), column_name.ptr()))) {
                  LOG_WARN("build strict read count sql failed", K(ret));
                } else {
                  SMART_VAR(ObMySQLProxy::MySQLResult, count_res) {
                    common::sqlclient::ObMySQLResult *count_result = NULL;
                    if (OB_FAIL(proxy->read(count_res, count_sql.ptr()))) {
                      LOG_WARN("read strict read count failed", K(ret), K(count_sql));
                    } else if (OB_ISNULL(count_result = count_res.get_result())) {
                      ret = OB_ERR_UNEXPECTED;
                    } else if (OB_FAIL(count_result->next())) {
                      if (OB_ITER_END != ret) {
                        LOG_WARN("read strict read count row failed", K(ret));
                      }
                    } else if (OB_FAIL(count_result->get_int("unfinished", unfinished))) {
                      LOG_WARN("get strict read count failed", K(ret));
                    } else if (0 < unfinished) {
                      ret = OB_ERR_AI_COLUMN_UNFINISHED_TASKS;
                      LOG_USER_ERROR(OB_ERR_AI_COLUMN_UNFINISHED_TASKS,
                                     table_id, column_id);
                    }
                  }
                }
              }
            }
            if (OB_ITER_END == ret) { ret = OB_SUCCESS; }
          }
        }
      }
    }
  }
  return ret;
}

} // namespace share
} // namespace oceanbase
