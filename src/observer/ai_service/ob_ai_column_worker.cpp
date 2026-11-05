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

#define USING_LOG_PREFIX SERVER

#include "observer/ai_service/ob_ai_column_worker.h"
#include "observer/ai_service/ob_ai_service_proxy.h"
#include "sql/engine/expr/ob_expr_ai/ob_ai_func_utils.h"
#include "sql/engine/expr/ob_expr_ai/ob_ai_func.h"
#include "share/ob_server_struct.h"
#include "common/ob_timeout_ctx.h"
#include "share/ob_share_util.h"
#include "share/inner_table/ob_inner_table_schema.h"
#include "common/mysqlclient/ob_mysql_transaction.h"
#include "lib/utility/utility.h"
#include "lib/string/ob_sql_string.h"

namespace oceanbase {
namespace observer {

using namespace oceanbase::common;
using namespace oceanbase::common::sqlclient;
using namespace oceanbase::share;
using namespace oceanbase::sql;

static const int64_t AI_COLUMN_WORKER_INTERVAL_US = 200 * 1000;       // 200ms
static const int64_t AI_COLUMN_WORKER_BATCH = 128;                    // task rows per tick
static const int64_t AI_COLUMN_WORKER_TICK_BUDGET_US = 150 * 1000;    // 150ms per tick
static const int64_t AI_COLUMN_WORKER_RETRY_BACKOFF_SEC = 1;          // retry backoff

ObAiColumnWorker::ObAiColumnWorker()
  : scheduled_(false), running_(false)
{
}

ObAiColumnWorker::~ObAiColumnWorker()
{
}

int ObAiColumnWorker::start(common::ObTimer &timer)
{
  int ret = OB_SUCCESS;
  if (scheduled_) {
    // already scheduled
  } else if (OB_FAIL(timer.schedule(*this, AI_COLUMN_WORKER_INTERVAL_US, true))) {
    LOG_ERROR("schedule ai column worker failed", K(ret));
  } else {
    scheduled_ = true;
    LOG_INFO("ai column worker scheduled");
  }
  return ret;
}

void ObAiColumnWorker::runTimerTask()
{
  int ret = OB_SUCCESS;
  if (running_) {
    // The previous tick is still running (a model call can block for a
    // while); never run two ticks concurrently.
  } else {
    running_ = true;
    if (OB_FAIL(process_tick_())) {
    }
    running_ = false;
  }
}

int ObAiColumnWorker::read_candidate_tasks_(ObIArray<AiTaskItem> &tasks)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  SMART_VAR(ObMySQLProxy::MySQLResult, res) {
    ObMySQLResult *result = NULL;
    if (OB_ISNULL(GCTX.sql_proxy_)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(sql.assign_fmt(
        "SELECT table_id, rowkey_predicate, column_id, request_id, state, "
        "retry_count, max_retries, ai_model_name FROM oceanbase.%s "
        "WHERE state IN (%ld, %ld) AND eligible_ts <= CURRENT_TIMESTAMP(6) "
        "ORDER BY eligible_ts LIMIT %ld",
        OB_ALL_AI_COLUMN_TASK_TNAME,
        OB_AI_COL_TASK_STATE_PENDING, OB_AI_COL_TASK_STATE_RETRY,
        AI_COLUMN_WORKER_BATCH))) {
      LOG_WARN("build candidate sql failed", K(ret));
    } else if (OB_FAIL(GCTX.sql_proxy_->read(res, sql.ptr()))) {
      LOG_WARN("read candidate tasks failed", K(ret), K(sql));
    } else if (OB_ISNULL(result = res.get_result())) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      while (OB_SUCC(ret) && OB_SUCC(result->next())) {
        AiTaskItem item;
        int64_t table_id = 0;
        ObString rowkey_predicate;
        ObString request_id;
        ObString ai_model_name;
        if (OB_FAIL(result->get_int("table_id", table_id))) {
        } else if (OB_FAIL(result->get_varchar("rowkey_predicate", rowkey_predicate))) {
        } else if (OB_FAIL(result->get_int("column_id", item.column_id_))) {
        } else if (OB_FAIL(result->get_varchar("request_id", request_id))) {
        } else if (OB_FAIL(result->get_int("state", item.state_))) {
        } else if (OB_FAIL(result->get_int("retry_count", item.retry_count_))) {
        } else if (OB_FAIL(result->get_int("max_retries", item.max_retries_))) {
        } else if (OB_FAIL(result->get_varchar("ai_model_name", ai_model_name))) {
        } else if (rowkey_predicate.length() >= static_cast<int>(sizeof(item.rowkey_predicate_))
            || request_id.length() >= static_cast<int>(sizeof(item.request_id_))
            || ai_model_name.length() >= static_cast<int>(sizeof(item.ai_model_name_))) {
          ret = OB_SIZE_OVERFLOW;
          LOG_WARN("ai column task value too long", K(ret));
        } else {
          item.table_id_ = static_cast<uint64_t>(table_id);
          MEMCPY(item.rowkey_predicate_, rowkey_predicate.ptr(), rowkey_predicate.length());
          item.rowkey_predicate_[rowkey_predicate.length()] = '\0';
          MEMCPY(item.request_id_, request_id.ptr(), request_id.length());
          item.request_id_[request_id.length()] = '\0';
          MEMCPY(item.ai_model_name_, ai_model_name.ptr(), ai_model_name.length());
          item.ai_model_name_[ai_model_name.length()] = '\0';
          if (OB_FAIL(tasks.push_back(item))) {
            LOG_WARN("push task item failed", K(ret));
          }
        }
      }
      if (OB_ITER_END == ret) {
        ret = OB_SUCCESS;
      }
    }
  }
  return ret;
}

int ObAiColumnWorker::load_group_metas_(uint64_t table_id,
                                        const ObIArray<int64_t> &column_ids,
                                        ObIArray<share::ObAiColumnMetaInfo> &metas)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  SMART_VAR(ObMySQLProxy::MySQLResult, res) {
    ObMySQLResult *result = NULL;
    if (OB_ISNULL(GCTX.sql_proxy_)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(sql.assign_fmt(
        "SELECT column_id, column_name, source_expr, ai_model_name, dimension, max_retries "
        "FROM oceanbase.%s WHERE table_id = %ld AND column_id IN (",
        OB_ALL_AI_COLUMN_TNAME, table_id))) {
      LOG_WARN("build meta sql failed", K(ret));
    } else {
      for (int64_t i = 0; OB_SUCC(ret) && i < column_ids.count(); i++) {
        if (i > 0 && OB_FAIL(sql.append(", "))) {
        } else if (OB_FAIL(sql.append_fmt("%ld", column_ids.at(i)))) {
        }
      }
      if (OB_SUCC(ret) && OB_FAIL(sql.append(")"))) {
      } else if (OB_FAIL(GCTX.sql_proxy_->read(res, sql.ptr()))) {
        LOG_WARN("read ai column metas failed", K(ret), K(sql));
      } else if (OB_ISNULL(result = res.get_result())) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        while (OB_SUCC(ret) && OB_SUCC(result->next())) {
          share::ObAiColumnMetaInfo meta;
          int64_t col_id = 0;
          int64_t dimension = 0;
          int64_t max_retries = 2;
          ObString column_name;
          ObString source_expr;
          ObString ai_model_name;
          if (OB_FAIL(result->get_int("column_id", col_id))) {
          } else if (OB_FAIL(result->get_varchar("column_name", column_name))) {
          } else if (OB_FAIL(result->get_varchar("source_expr", source_expr))) {
          } else if (OB_FAIL(result->get_varchar("ai_model_name", ai_model_name))) {
          } else if (OB_FAIL(result->get_int("dimension", dimension))) {
          } else if (OB_FAIL(result->get_int("max_retries", max_retries))) {
          } else if (column_name.length() >= static_cast<int>(sizeof(meta.column_name_))
              || source_expr.length() >= static_cast<int>(sizeof(meta.source_expr_))
              || ai_model_name.length() >= static_cast<int>(sizeof(meta.ai_model_name_))) {
            ret = OB_SIZE_OVERFLOW;
            LOG_WARN("ai column meta value too long", K(ret));
          } else {
            meta.column_id_ = static_cast<uint64_t>(col_id);
            meta.dimension_ = dimension;
            meta.max_retries_ = max_retries;
            MEMCPY(meta.column_name_, column_name.ptr(), column_name.length());
            meta.column_name_[column_name.length()] = '\0';
            MEMCPY(meta.source_expr_, source_expr.ptr(), source_expr.length());
            meta.source_expr_[source_expr.length()] = '\0';
            MEMCPY(meta.ai_model_name_, ai_model_name.ptr(), ai_model_name.length());
            meta.ai_model_name_[ai_model_name.length()] = '\0';
            if (OB_FAIL(metas.push_back(meta))) {
              LOG_WARN("push meta failed", K(ret));
            }
          }
        }
        if (OB_ITER_END == ret) {
          ret = OB_SUCCESS;
        }
      }
    }
  }
  return ret;
}

int ObAiColumnWorker::load_qualified_name_(uint64_t table_id, ObSqlString &qualified)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  SMART_VAR(ObMySQLProxy::MySQLResult, res) {
    ObMySQLResult *result = NULL;
    ObString db_name;
    ObString tbl_name;
    if (OB_ISNULL(GCTX.sql_proxy_)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(sql.assign_fmt(
        "SELECT d.database_name AS db_name, t.table_name AS tbl_name FROM oceanbase.%s t "
        "JOIN oceanbase.%s d ON t.database_id = d.database_id WHERE t.table_id = %ld",
        OB_ALL_TABLE_TNAME, OB_ALL_DATABASE_TNAME, table_id))) {
      LOG_WARN("build qualified name sql failed", K(ret));
    } else if (OB_FAIL(GCTX.sql_proxy_->read(res, sql.ptr()))) {
      LOG_WARN("read qualified name failed", K(ret), K(sql));
    } else if (OB_ISNULL(result = res.get_result())) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(result->next())) {
      if (OB_ITER_END != ret) {
        LOG_WARN("read qualified name row failed", K(ret));
      } else {
        ret = OB_ENTRY_NOT_EXIST;
        LOG_WARN("ai column table not found", K(ret), K(table_id));
      }
    } else if (OB_FAIL(result->get_varchar("db_name", db_name))) {
    } else if (OB_FAIL(result->get_varchar("tbl_name", tbl_name))) {
    } else if (OB_FAIL(qualified.assign_fmt("`%.*s`.`%.*s`",
                                            db_name.length(), db_name.ptr(),
                                            tbl_name.length(), tbl_name.ptr()))) {
      LOG_WARN("assign qualified name failed", K(ret));
    }
  }
  return ret;
}

int ObAiColumnWorker::eval_prompt_(const ObSqlString &qualified,
                                   const share::ObAiColumnMetaInfo &meta,
                                   const char *rowkey_predicate,
                                   ObSqlString &prompt)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  SMART_VAR(ObMySQLProxy::MySQLResult, res) {
    ObMySQLResult *result = NULL;
    ObString value;
    if (OB_ISNULL(GCTX.sql_proxy_)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(sql.assign_fmt("SELECT %s AS ai_prompt FROM %s WHERE %s",
                                      meta.source_expr_, qualified.ptr(), rowkey_predicate))) {
      LOG_WARN("build eval sql failed", K(ret));
    } else if (OB_FAIL(GCTX.sql_proxy_->read(res, sql.ptr()))) {
      LOG_WARN("eval ai expression failed", K(ret), K(sql));
    } else if (OB_ISNULL(result = res.get_result())) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(result->next())) {
      if (OB_ITER_END == ret) {
        // The row disappeared (deleted while the task was pending).
        ret = OB_ENTRY_NOT_EXIST;
      } else {
        LOG_WARN("eval ai expression row failed", K(ret));
      }
    } else if (OB_FAIL(result->get_varchar("ai_prompt", value))) {
      LOG_WARN("get evaluated prompt failed", K(ret));
    } else if (OB_FAIL(prompt.assign(value.ptr(), value.length()))) {
      LOG_WARN("assign prompt failed", K(ret));
    }
  }
  return ret;
}

int ObAiColumnWorker::build_typed_config_(ObIAllocator &allocator,
                                          int64_t dimension,
                                          common::ObJsonObject *&config)
{
  int ret = OB_SUCCESS;
  const char *system_prompt = NULL;
  if (OB_AI_COL_DIM_JSON == dimension) {
    system_prompt = "Return exactly one valid JSON value";
  } else if (OB_AI_COL_DIM_INT == dimension) {
    system_prompt = "Return a base-10 integer";
  } else {
    system_prompt = NULL;  // varchar and vector columns use the default prompt
  }
  if (OB_NOT_NULL(system_prompt)) {
    ObJsonString *prompt_node = NULL;
    ObString key("ai_system_prompt");
    ObString value(strlen(system_prompt), system_prompt);
    if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(allocator, config))) {
      LOG_WARN("build typed config object failed", K(ret));
    } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_string(allocator, value, prompt_node))) {
      LOG_WARN("build typed config string failed", K(ret));
    } else if (OB_FAIL(config->add(key, prompt_node))) {
      LOG_WARN("add typed config entry failed", K(ret));
    }
  } else {
    config = NULL;
  }
  return ret;
}

// The AI column metadata may keep the quotes of the DDL text around the
// model name; the endpoint is stored without them.
static void ai_trim_model_name(const char *in, char *out, int64_t out_cap)
{
  int64_t len = (OB_NOT_NULL(in)) ? static_cast<int64_t>(strlen(in)) : 0;
  const char *p = in;
  while (len > 0 && (' ' == *p || '\t' == *p || '\n' == *p || '\r' == *p)) { ++p; --len; }
  while (len > 0 && (' ' == p[len - 1] || '\t' == p[len - 1]
                     || '\n' == p[len - 1] || '\r' == p[len - 1])) { --len; }
  if (len > 1 && ('\'' == *p || '"' == *p) && p[len - 1] == *p) { ++p; len -= 2; }
  if (len >= out_cap) { len = out_cap - 1; }
  if (len > 0) { MEMCPY(out, p, len); }
  out[len > 0 ? len : 0] = '\0';
}

int ObAiColumnWorker::call_model_(const share::ObAiColumnMetaInfo &meta,
                                  const ObString &prompt,
                                  AiValueItem &value)
{
  int ret = OB_SUCCESS;
  ObArenaAllocator allocator(ObModIds::OB_MODULE_PAGE_ALLOCATOR, OB_MALLOC_NORMAL_BLOCK_SIZE);
  share::ObAiModelEndpointInfo endpoint;
  char model_buf[128];
  ai_trim_model_name(meta.ai_model_name_, model_buf, sizeof(model_buf));
  const ObString model_name(strlen(model_buf), model_buf);
  const bool is_embed = (OB_AI_COL_DIM_VECTOR == meta.dimension_);
  if (OB_ISNULL(GCTX.sql_proxy_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(ObAiServiceProxy::select_ai_endpoint_by_ai_model_name(
      allocator, *GCTX.sql_proxy_, model_name, OB_ORIGIN_AND_INSENSITIVE, endpoint))) {
    LOG_WARN("load ai model endpoint failed", K(ret), K(model_name));
  } else {
    ObAIFuncExprInfo info(allocator, is_embed ? T_FUN_SYS_AI_EMBED : T_FUN_SYS_AI_COMPLETE);
    info.name_ = ObString::make_string(is_embed ? "ai_embed" : "ai_complete");
    info.type_ = is_embed ? share::EndpointType::DENSE_EMBEDDING
                          : share::EndpointType::COMPLETION;
    info.model_ = model_name;
    ObAIFuncModel model(allocator, info, endpoint);
    common::ObJsonObject *config = NULL;
    ObString mutable_prompt(prompt.length(), prompt.ptr());
    ObString result;
    if (!is_embed && OB_FAIL(build_typed_config_(allocator, meta.dimension_, config))) {
      LOG_WARN("build typed config failed", K(ret));
    } else if (is_embed && OB_FAIL(model.call_dense_embedding(mutable_prompt, NULL, result))) {
      LOG_WARN("call dense embedding failed", K(ret));
    } else if (!is_embed && OB_FAIL(model.call_completion(mutable_prompt, config, result))) {
      LOG_WARN("call completion failed", K(ret));
    } else if (result.length() >= static_cast<int>(sizeof(value.value_))) {
      ret = OB_SIZE_OVERFLOW;
      LOG_WARN("ai model result too long", K(ret), K(result.length()));
    } else {
      MEMCPY(value.value_, result.ptr(), result.length());
      value.value_[result.length()] = '\0';
      if (OB_AI_COL_DIM_INT == meta.dimension_) {
        value.is_number_ = true;
        value.number_value_ = strtoll(value.value_, NULL, 10);
      }
    }
  }
  return ret;
}

int ObAiColumnWorker::publish_group_(const AiGroup &group,
                                     const ObIArray<share::ObAiColumnMetaInfo> &metas,
                                     const ObIArray<AiValueItem> &values)
{
  int ret = OB_SUCCESS;
  ObMySQLTransaction trans;
  ObSqlString qualified;
  if (OB_ISNULL(GCTX.sql_proxy_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (metas.count() != values.count() || metas.count() != group.tasks_.count()) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("ai column publish arity mismatch", K(ret), K(metas.count()),
             K(values.count()), K(group.tasks_.count()));
  } else if (OB_FAIL(load_qualified_name_(group.table_id_, qualified))) {
    LOG_WARN("load qualified name failed", K(ret), K(group.table_id_));
  } else if (OB_FAIL(trans.start(GCTX.sql_proxy_))) {
    LOG_WARN("start publish transaction failed", K(ret));
  } else {
    bool superseded = false;
    // Stale guard: the task rows of this request must still exist. A newer
    // DML on the same row removed them, so this (late) result must not be
    // published.
    ObSqlString chk;
    int64_t pending = 0;
    if (OB_FAIL(chk.assign_fmt("SELECT count(*) AS pending FROM oceanbase.%s WHERE table_id = %ld "
                               "AND rowkey_predicate = ",
                               OB_ALL_AI_COLUMN_TASK_TNAME, group.table_id_))) {
    } else if (OB_FAIL(sql_append_hex_escape_str(ObString(group.rowkey_predicate_), chk))) {
    } else if (OB_FAIL(chk.append(" AND request_id = "))) {
    } else if (OB_FAIL(sql_append_hex_escape_str(ObString(group.request_id_), chk))) {
    } else {
      SMART_VAR(ObMySQLProxy::MySQLResult, res) {
        ObMySQLResult *result = NULL;
        if (OB_FAIL(trans.read(res, chk.ptr()))) {
          LOG_WARN("stale guard read failed", K(ret), K(chk));
        } else if (OB_ISNULL(result = res.get_result())) {
          ret = OB_ERR_UNEXPECTED;
        } else if (OB_FAIL(result->next())) {
          if (OB_ITER_END != ret) {
            LOG_WARN("stale guard row failed", K(ret));
          }
        } else if (OB_FAIL(result->get_int("pending", pending))) {
          LOG_WARN("stale guard get count failed", K(ret));
        }
      }
    }
    if (OB_SUCC(ret) && static_cast<int64_t>(group.tasks_.count()) != pending) {
      superseded = true;
      LOG_INFO("ai column group superseded, drop late result", K(group.table_id_),
               K(group.rowkey_predicate_), K(group.request_id_), K(pending));
    }
    if (OB_SUCC(ret) && !superseded) {
      ObSqlString sql;
      int64_t affected = 0;
      if (OB_FAIL(sql.assign_fmt("UPDATE %s SET ", qualified.ptr()))) {
      } else {
        // One UPDATE publishes every AI column of the group atomically, so
        // readers never observe a partially refreshed row.
        for (int64_t i = 0; OB_SUCC(ret) && i < metas.count(); i++) {
          const AiValueItem &v = values.at(i);
          if (i > 0 && OB_FAIL(sql.append(", "))) {
          } else if (v.is_number_) {
            if (OB_FAIL(sql.append_fmt("`%s` = %ld", metas.at(i).column_name_,
                                       v.number_value_))) {
              LOG_WARN("append numeric set item failed", K(ret));
            }
          } else {
            if (OB_FAIL(sql.append_fmt("`%s` = '", metas.at(i).column_name_))) {
              LOG_WARN("append set item failed", K(ret));
            } else {
              for (int64_t c = 0; OB_SUCC(ret) && '\0' != v.value_[c]; c++) {
                if ('\'' == v.value_[c]) {
                  if (OB_FAIL(sql.append("''"))) {
                  }
                } else if (OB_FAIL(sql.append_fmt("%c", v.value_[c]))) {
                }
              }
              if (OB_SUCC(ret) && OB_FAIL(sql.append("'"))) {
              }
            }
          }
        }
      }
      if (OB_SUCC(ret) && OB_FAIL(sql.append_fmt(" WHERE %s", group.rowkey_predicate_))) {
      } else if (OB_FAIL(trans.write(sql.ptr(), affected))) {
        LOG_WARN("publish ai column values failed", K(ret), K(sql));
      } else {
        ObSqlString del;
        if (OB_FAIL(del.assign_fmt("DELETE FROM oceanbase.%s WHERE table_id = %ld AND rowkey_predicate = ",
                                   OB_ALL_AI_COLUMN_TASK_TNAME, group.table_id_))) {
        } else if (OB_FAIL(sql_append_hex_escape_str(ObString(group.rowkey_predicate_), del))) {
        } else if (OB_FAIL(del.append(" AND request_id = "))) {
        } else if (OB_FAIL(sql_append_hex_escape_str(ObString(group.request_id_), del))) {
        } else if (OB_FAIL(trans.write(del.ptr(), affected))) {
          LOG_WARN("delete published task rows failed", K(ret), K(del));
        }
      }
    }
    int end_ret = trans.end(OB_SUCCESS == ret);
    if (OB_SUCCESS != end_ret) {
      LOG_WARN("end publish transaction failed", K(end_ret));
      if (OB_SUCCESS == ret) { ret = end_ret; }
    }
  }
  return ret;
}

int ObAiColumnWorker::fail_group_(const AiGroup &group)
{
  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < group.tasks_.count(); i++) {
    const AiTaskItem &task = group.tasks_.at(i);
    ObSqlString sql;
    int64_t affected = 0;
    if (OB_ISNULL(GCTX.sql_proxy_)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (task.retry_count_ < task.max_retries_) {
      if (OB_FAIL(sql.assign_fmt(
          "UPDATE oceanbase.%s SET state = %ld, retry_count = retry_count + 1, "
          "eligible_ts = DATE_ADD(CURRENT_TIMESTAMP(6), INTERVAL %ld SECOND), "
          "error_message = 'model call failed' "
          "WHERE table_id = %ld AND rowkey_predicate = ",
          OB_ALL_AI_COLUMN_TASK_TNAME, OB_AI_COL_TASK_STATE_RETRY,
          AI_COLUMN_WORKER_RETRY_BACKOFF_SEC, group.table_id_))) {
      } else if (OB_FAIL(sql_append_hex_escape_str(ObString(group.rowkey_predicate_), sql))) {
      } else if (OB_FAIL(sql.append_fmt(" AND column_id = %ld AND request_id = ",
                                        task.column_id_))) {
      } else if (OB_FAIL(sql_append_hex_escape_str(ObString(group.request_id_), sql))) {
      } else if (OB_FAIL(GCTX.sql_proxy_->write(sql.ptr(), affected))) {
        LOG_WARN("mark ai column task for retry failed", K(ret), K(sql));
      }
    } else {
      // max_retries exhausted: drop the task, the column stays NULL.
      if (OB_FAIL(sql.assign_fmt("DELETE FROM oceanbase.%s WHERE table_id = %ld AND rowkey_predicate = ",
                                 OB_ALL_AI_COLUMN_TASK_TNAME, group.table_id_))) {
      } else if (OB_FAIL(sql_append_hex_escape_str(ObString(group.rowkey_predicate_), sql))) {
      } else if (OB_FAIL(sql.append_fmt(" AND column_id = %ld AND request_id = ",
                                        task.column_id_))) {
      } else if (OB_FAIL(sql_append_hex_escape_str(ObString(group.request_id_), sql))) {
      } else if (OB_FAIL(GCTX.sql_proxy_->write(sql.ptr(), affected))) {
        LOG_WARN("drop failed ai column task failed", K(ret), K(sql));
      }
    }
  }
  return ret;
}

int ObAiColumnWorker::process_group_(const AiGroup &group)
{
  int ret = OB_SUCCESS;
  ObArray<int64_t> column_ids;
  ObArray<share::ObAiColumnMetaInfo> metas;
  ObArray<AiValueItem> values;
  ObSqlString qualified;
  bool row_gone = false;
  if (OB_FAIL(load_qualified_name_(group.table_id_, qualified))) {
    LOG_WARN("load qualified name failed", K(ret), K(group.table_id_));
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < group.tasks_.count(); i++) {
      if (OB_FAIL(column_ids.push_back(group.tasks_.at(i).column_id_))) {
        LOG_WARN("push column id failed", K(ret));
      }
    }
  }
  if (OB_SUCC(ret) && OB_FAIL(load_group_metas_(group.table_id_, column_ids, metas))) {
    LOG_WARN("load group metas failed", K(ret), K(group.table_id_));
  } else if (OB_SUCC(ret) && metas.count() != group.tasks_.count()) {
    // The AI column metadata disappeared (table dropped or altered).
    row_gone = true;
  }
  if (OB_SUCC(ret) && !row_gone) {
    for (int64_t i = 0; OB_SUCC(ret) && i < metas.count(); i++) {
      const share::ObAiColumnMetaInfo &meta = metas.at(i);
      ObSqlString prompt;
      AiValueItem value;
      value.column_id_ = static_cast<int64_t>(meta.column_id_);
      if (OB_FAIL(eval_prompt_(qualified, meta, group.rowkey_predicate_, prompt))) {
        if (OB_ENTRY_NOT_EXIST == ret) {
          row_gone = true;
          ret = OB_SUCCESS;
          break;
        }
      } else if (OB_FAIL(call_model_(meta, prompt.string(), value))) {
      } else if (OB_FAIL(values.push_back(value))) {
        LOG_WARN("push value failed", K(ret));
      }
    }
  }
  if (OB_SUCC(ret)) {
    if (row_gone) {
      // The row or its metadata disappeared: drop the pending tasks.
      if (OB_FAIL(fail_group_(group))) {
      }
    } else if (values.count() != group.tasks_.count()) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("ai column value count mismatch", K(ret), K(values.count()),
               K(group.tasks_.count()));
    } else if (OB_FAIL(publish_group_(group, metas, values))) {
    }
  }
  if (OB_FAIL(ret)) {
    int tmp_ret = fail_group_(group);
    if (OB_SUCCESS != tmp_ret) {
      LOG_WARN("fail group after error failed", K(tmp_ret));
    }
  }
  return ret;
}

int ObAiColumnWorker::process_tick_()
{
  int ret = OB_SUCCESS;
  if (0 == ObAiColumnRegistry::get_instance().table_count()) {
    // No table with AI columns exists in this process.
  } else if (OB_ISNULL(GCTX.sql_proxy_)) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    ObTimeoutCtx ctx;
    const int64_t default_timeout = 10 * 1000 * 1000;  // 10s inner sql timeout
    if (OB_FAIL(ObShareUtil::set_default_timeout_ctx(ctx, default_timeout))) {
      LOG_WARN("set timeout ctx failed", K(ret));
    } else {
      ObArray<AiTaskItem> tasks;
      ObArray<AiGroup> groups;
      const int64_t start_ts = ObTimeUtil::current_time();
      if (OB_FAIL(read_candidate_tasks_(tasks))) {
        LOG_WARN("read candidate tasks failed", K(ret));
      } else {
        // Group by (table_id, rowkey_predicate, request_id).
        for (int64_t i = 0; OB_SUCC(ret) && i < tasks.count(); i++) {
          const AiTaskItem &task = tasks.at(i);
          bool found = false;
          for (int64_t g = 0; !found && g < groups.count(); g++) {
            AiGroup &group = groups.at(g);
            if (group.table_id_ == task.table_id_
                && 0 == STRCMP(group.rowkey_predicate_, task.rowkey_predicate_)
                && 0 == STRCMP(group.request_id_, task.request_id_)) {
              if (OB_FAIL(group.tasks_.push_back(task))) {
                LOG_WARN("push group task failed", K(ret));
              }
              found = true;
            }
          }
          if (OB_SUCC(ret) && !found) {
            AiGroup group;
            group.table_id_ = task.table_id_;
            MEMCPY(group.rowkey_predicate_, task.rowkey_predicate_,
                   strlen(task.rowkey_predicate_) + 1);
            MEMCPY(group.request_id_, task.request_id_, strlen(task.request_id_) + 1);
            if (OB_FAIL(group.tasks_.push_back(task))) {
              LOG_WARN("push group task failed", K(ret));
            } else if (OB_FAIL(groups.push_back(group))) {
              LOG_WARN("push group failed", K(ret));
            }
          }
        }
        for (int64_t g = 0; OB_SUCC(ret) && g < groups.count(); g++) {
          if (ObTimeUtil::current_time() - start_ts > AI_COLUMN_WORKER_TICK_BUDGET_US) {
            LOG_INFO("ai column worker tick budget reached", K(g), K(groups.count()));
            break;
          }
          if (OB_FAIL(process_group_(groups.at(g)))) {
            // The group failed and has been queued for retry by
            // process_group_; continue with the remaining groups.
            LOG_WARN("process ai column group failed", K(ret), K(g));
            ret = OB_SUCCESS;
          }
        }
      }
    }
  }
  return ret;
}

} // namespace observer
} // namespace oceanbase
