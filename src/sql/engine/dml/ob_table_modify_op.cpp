/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define USING_LOG_PREFIX SQL_ENG

#include "ob_table_modify_op.h"
#include "share/ob_autoincrement_service.h"
#include "query/session/ob_inner_sql_connection_access.h"
#include "sql/engine/ob_physical_plan.h"
#include "sql/engine/dml/ob_dml_service.h"
#include "lib/string/ob_sql_string.h"
#include "share/schema/ob_schema_getter_guard.h"

namespace oceanbase
{
using namespace common;
using namespace common::sqlclient;
using namespace storage;
using namespace share;
using namespace share::schema;
using namespace observer;
namespace sql
{
int ForeignKeyHandle::do_handle(ObTableModifyOp &op,
                                const ObDMLBaseCtDef &dml_ctdef,
                                ObDMLBaseRtDef &dml_rtdef)
{
  int ret = OB_SUCCESS;
  if (op.need_foreign_key_checks()) {
    const ObExprPtrIArray &old_row = dml_ctdef.old_row_;
    const ObExprPtrIArray &new_row = dml_ctdef.new_row_;
    const bool save_in_ignore_cascading = op.get_exec_ctx().get_das_ctx().in_ignore_cascading_;
    op.get_exec_ctx().get_das_ctx().is_fk_cascading_ = true;
    op.get_exec_ctx().get_das_ctx().in_ignore_cascading_ = dml_ctdef.das_base_ctdef_.is_ignore_;
    if (OB_FAIL(op.check_stack())) {
    }
    for (int i = 0; OB_SUCC(ret) && i < dml_ctdef.fk_args_.count(); i++) {
      const ObForeignKeyArg &fk_arg = dml_ctdef.fk_args_.at(i);
      ObForeignKeyChecker *fk_checker = dml_rtdef.fk_checker_array_.at(i);
      if (OB_SUCC(ret) && !new_row.empty()) {
        if (ACTION_CHECK_EXIST == fk_arg.ref_action_) {
          // insert or update.
          bool is_foreign_key_cascade = ObSQLUtils::is_fk_nested_sql(&op.get_exec_ctx());
          bool in_ignore_cascading = false;
          ObExecContext* parent_ctx = op.get_exec_ctx().get_parent_ctx();
          if (OB_NOT_NULL(parent_ctx)) {
            in_ignore_cascading = parent_ctx->get_das_ctx().in_ignore_cascading_;
          }

          if (is_foreign_key_cascade && !in_ignore_cascading) {
            // nested update can not check parent row.
          } else if (OB_FAIL(check_exist(op, fk_arg, new_row, fk_checker, false, fk_arg.use_das_scan_))) {
          }
        }
      }
      if (OB_SUCC(ret) && !old_row.empty()) {
        if (ACTION_RESTRICT == fk_arg.ref_action_ || ACTION_NO_ACTION == fk_arg.ref_action_) {
          // update or delete.
          bool has_changed = false;
          if (OB_FAIL(value_changed(op, fk_arg.columns_, old_row, new_row, has_changed))) {
          } else if (!has_changed) {
            // nothing.
          } else if (OB_FAIL(check_exist(op, fk_arg, old_row, fk_checker, true, fk_arg.use_das_scan_))) {
          }
        } else if (ACTION_CASCADE == fk_arg.ref_action_) {
          // update or delete.
          bool is_self_ref = false;
          if (OB_FAIL(is_self_ref_row(op.get_eval_ctx(), old_row, fk_arg, is_self_ref))) {
          } else if (new_row.empty() && is_self_ref && op.is_fk_nested_session()) {
            // delete self refercnced row should not cascade delete.
          } else if (OB_FAIL(check_exist_inner_sql(op, fk_arg, old_row, true, true))) {
          } else if (OB_FAIL(cascade(op, fk_arg, old_row, new_row))) {
          } else if (!new_row.empty() && is_self_ref) {
            // we got here only when:
            //  1. handling update operator and
            //  2. foreign key constraint is self reference
            // need to change %new_row and %old_row
            //   (see 
            // xxx_row_res_info helps to restore row. restore row before get_next_row()
            //op.fk_self_ref_row_res_infos_.reset();
            for (int64_t i = 0; OB_SUCC(ret) && i < fk_arg.columns_.count(); i++) {
              const int32_t val_idx = fk_arg.columns_.at(i).idx_;
              const int32_t name_idx = fk_arg.columns_.at(i).name_idx_;
              if (OB_SUCC(ret)) {
                const ObDatum &updated_value = new_row.at(val_idx)->
                                                    locate_expr_datum(op.get_eval_ctx());
                ObDatum &new_row_name_col = new_row.at(name_idx)->
                                                  locate_expr_datum(op.get_eval_ctx());
                ObDatum &old_row_name_col = old_row.at(name_idx)->
                                                  locate_expr_datum(op.get_eval_ctx());

                OX(new_row_name_col = updated_value);
                OX(old_row_name_col = updated_value);
              }
            }
          }
        } else if (ACTION_SET_NULL == fk_arg.ref_action_) {
          if (OB_FAIL(check_exist_inner_sql(op, fk_arg, old_row, true, true))) {
          } else if (OB_FAIL(set_null(op, fk_arg, old_row))) {
          }
        }
      } // if (old_row.is_valid())
    } // for
    //fk cascading end
    op.get_exec_ctx().get_das_ctx().is_fk_cascading_ = false;
    op.get_exec_ctx().get_das_ctx().in_ignore_cascading_ = save_in_ignore_cascading;
  } else {
  }
  return ret;
}

int ForeignKeyHandle::value_changed(ObTableModifyOp &op,
                                    const ObIArray<ObForeignKeyColumn> &columns,
                                    const ObExprPtrIArray &old_row,
                                    const ObExprPtrIArray &new_row,
                                    bool &has_changed)
{
  int ret = OB_SUCCESS;
  has_changed = false;
  if (!old_row.empty() && !new_row.empty()) {
    ObDatum *old_row_col = NULL;
    ObDatum *new_row_col = NULL;
    for (int64_t i = 0; OB_SUCC(ret) && !has_changed && i < columns.count(); ++i) {
      int64_t col_idx = columns.at(i).idx_;
      CK(col_idx < old_row.count());
      CK(col_idx < new_row.count());
      CK(OB_NOT_NULL(old_row.at(col_idx)));
      CK(OB_NOT_NULL(new_row.at(col_idx)));
      OZ(old_row.at(col_idx)->eval(op.get_eval_ctx(), old_row_col));
      OZ(new_row.at(col_idx)->eval(op.get_eval_ctx(), new_row_col));
      OX(has_changed = (false == ObDatum::binary_equal(*old_row_col, *new_row_col)));
    }
  } else {
    has_changed = true;
  }
  return ret;
}

int ForeignKeyHandle::check_exist(ObTableModifyOp &modify_op, const ObForeignKeyArg &fk_arg,
                         const ObExprPtrIArray &row, ObForeignKeyChecker *fk_checker, bool expect_zero, bool use_das_scan)
{
  int ret = OB_SUCCESS;
  DEBUG_SYNC(BEFORE_FOREIGN_KEY_CONSTRAINT_CHECK);
  // insertup and merge_into currently have a core problem when checking foreign keys through das_scan_task.
  // Because both insertup and merge_into have both INSERT and UPDATE semantics,
  // the data may come from the new_row of INSERT or the new_row of UPDATE during the operation.
  // Earlier versions did not consider this problem, so there is a possibility of core
  bool is_insertup = false;
  ObPhyOperatorType op_type = modify_op.get_spec().get_type();
  if (op_type == PHY_INSERT_ON_DUP) {
    is_insertup = true;
  }
  if (use_das_scan && !is_insertup) {
    ret = check_exist_scan_task(modify_op, fk_arg, row, fk_checker);
  } else {
    if (OB_FAIL(check_exist_inner_sql(modify_op, fk_arg, row, expect_zero, true))) {
    } else if (OB_FAIL(check_exist_inner_sql(modify_op, fk_arg, row, expect_zero, false))) {
    }
  }
  return ret;
}

int ForeignKeyHandle::check_exist_scan_task(ObTableModifyOp &modify_op, const ObForeignKeyArg &fk_arg,
                         const ObExprPtrIArray &row, ObForeignKeyChecker *fk_checker)
{
  int ret = OB_SUCCESS;
  bool has_result = false;
  if (OB_ISNULL(fk_checker)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (!fk_arg.use_das_scan_) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(fk_checker->do_fk_check_single_row(fk_arg.columns_, row, has_result))) {
  } else {
    if (!has_result) {
      ret = OB_ERR_NO_REFERENCED_ROW;
    } else {
      ret = OB_SUCCESS;
    }
  }
  return ret;
}

int ForeignKeyHandle::check_exist_inner_sql(ObTableModifyOp &op,
                                            const ObForeignKeyArg &fk_arg,
                                            const ObExprPtrIArray &row,
                                            bool expect_zero,
                                            bool iter_uncommitted_row)
{
  int ret = OB_SUCCESS;
  const char *select_fmt = "select /*+ no_parallel */ 1 from `%.*s`.`%.*s` where %.*s for update";
  ObArenaAllocator alloc(ObModIds::OB_MODULE_PAGE_ALLOCATOR,
                          OB_MALLOC_NORMAL_BLOCK_SIZE);
  char *stmt_buf = nullptr;
  int64_t stmt_len = 0;
  char *where_buf = nullptr;
  int64_t where_len = 0;
  int64_t stmt_pos = 0;
  int64_t where_pos = 0;
  const ObString &database_name = fk_arg.database_name_;
  const ObString &table_name = fk_arg.table_name_;
  if (row.empty()) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(gen_where(op.get_eval_ctx(), where_buf, where_len, where_pos,
                               fk_arg.columns_, row, alloc, op.get_obj_print_params()))) {
    if (OB_LIKELY(OB_ERR_NULL_VALUE == ret)) {
      // skip check exist if any column in foreign key is NULL.
      ret = OB_SUCCESS;
      stmt_pos = 0;
    } else {
    }
  } else if (OB_FAIL(databuff_printf(stmt_buf, stmt_len, stmt_pos, alloc, select_fmt,
                                     database_name.length(), database_name.ptr(),
                                     table_name.length(), table_name.ptr(),
                                     static_cast<int>(where_pos), where_buf))) {
  } else {
    stmt_buf[stmt_pos++] = 0;
  }
  if (OB_SUCC(ret) && stmt_pos > 0) {
    if (iter_uncommitted_row) {
      op.get_exec_ctx().get_das_ctx().iter_uncommitted_row_ = true;
    }
    SMART_VAR(ObMySQLProxy::MySQLResult, res) {
      if (OB_FAIL(op.begin_nested_session(fk_arg.is_self_ref_))) {
      } else {
        // must call end_nested_session() if begin_nested_session() success.
        bool is_zero = false;
        if (OB_FAIL(op.execute_read(stmt_buf, res))) {
        } else {
          // must call res.get_result()->close() if execute_read() success.
          if (OB_ISNULL(res.get_result())) {
            ret = OB_ERR_UNEXPECTED;
          } else if (OB_FAIL(res.get_result()->next())) {
            if (OB_ITER_END == ret) {
              is_zero = true;
              ret = OB_SUCCESS;
            } else {
            }
          }
          if (OB_SUCC(ret)) {
            if (is_zero != expect_zero) {
              // If it is a self-referencing foreign key new row references new row
              bool is_self_ref = false;
              // The FOR UPDATE check query does not use aggregate functions, so when is_zero is false
              // judge whether only one row is affected by whether the second record can be obtained.
              bool is_affect_only_one = false;
              if (!is_zero && OB_FAIL(res.get_result()->next())) {
                if (OB_ITER_END == ret) {
                  is_affect_only_one = true;
                  ret = OB_SUCCESS;
                } else {
                }
              }
              /**
               *  expect_zero is false when fk_arg.ref_action_ is equal to
               *  ACTION_CHECK_EXIST, if the is_zero != expect_zero condition is
               *  true, then is_zero is true, need to exclude the case of self reference.
               *  other cases return OB_ERR_NO_REFERENCED_ROW.
               *
               *  expect_zero is true when fk_arg.ref_action_ is equal to
               *  ACTION_RESTRICT or ACTION_NO_ACTION, if is_zero != expect_zero condition
               *  is true, then is_zero is false, need to exclude the case of self reference and
               *  only affect one row. other cases return OB_ERR_ROW_IS_REFERENCED.
               */
              if (!iter_uncommitted_row) {
                if (OB_FAIL(is_self_ref_row(op.get_eval_ctx(), row, fk_arg, is_self_ref))) {
                } else if (is_zero && !is_self_ref) {
                  ret = OB_ERR_NO_REFERENCED_ROW;
                } else if (!is_zero) {
                  ret = OB_ERR_ROW_IS_REFERENCED;
                }
              }
            }
          }
          int close_ret = res.get_result()->close();
          if (OB_SUCCESS != close_ret) {
            LOG_WARN("failed to close", K(close_ret));
            if (OB_SUCCESS == ret) {
              ret = close_ret;
            }
          }
        }
        int end_ret = op.end_nested_session();
        if (OB_SUCCESS != end_ret) {
          LOG_WARN("failed to end nested session", K(end_ret));
          if (OB_SUCCESS == ret) {
            ret = end_ret;
          }
        }
      }
    }
  }
  op.get_exec_ctx().get_das_ctx().iter_uncommitted_row_ = false;

  return ret;
}

int ForeignKeyHandle::cascade(ObTableModifyOp &op,
                              const ObForeignKeyArg &fk_arg,
                              const ObExprPtrIArray &old_row,
                              const ObExprPtrIArray &new_row)
{
  int ret = OB_SUCCESS;
  ObArenaAllocator alloc(ObModIds::OB_MODULE_PAGE_ALLOCATOR,
                          OB_MALLOC_NORMAL_BLOCK_SIZE);
  char *stmt_buf = nullptr;
  int64_t stmt_len = 0;
  char *where_buf = nullptr;
  int64_t where_len = 0;
  int64_t stmt_pos = 0;
  int64_t where_pos = 0;
  const ObString &database_name = fk_arg.database_name_;
  const ObString &table_name = fk_arg.table_name_;
  if (OB_FAIL(op.get_exec_ctx().check_status())) {
  } else if (old_row.empty()) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(gen_where(op.get_eval_ctx(), where_buf, where_len, where_pos,
                               fk_arg.columns_, old_row, alloc, op.get_obj_print_params()))) {
    if (OB_LIKELY(OB_ERR_NULL_VALUE == ret)) {
      // skip cascade if any column in foreign key is NULL.
      ret = OB_SUCCESS;
      stmt_pos = 0;
    } else {
    }
  } else {
    if (!new_row.empty()) {
      // update.
      const char *update_fmt = "update `%.*s`.`%.*s` set %.*s where %.*s";
      char *set_buf = nullptr;
      int64_t set_len = 0;
      int64_t set_pos = 0;
      if (OB_FAIL(gen_set(op.get_eval_ctx(), set_buf, set_len, set_pos,
                          fk_arg.columns_, new_row, alloc, op.get_obj_print_params()))) {
      } else if (OB_FAIL(databuff_printf(stmt_buf, stmt_len, stmt_pos, alloc, update_fmt,
                                         database_name.length(), database_name.ptr(),
                                         table_name.length(), table_name.ptr(),
                                         static_cast<int>(set_pos), set_buf,
                                         static_cast<int>(where_pos), where_buf))) {
      } else {
        stmt_buf[stmt_pos++] = 0;
      }
    } else {
      // delete.
      const char *delete_fmt = "delete from `%.*s`.`%.*s` where %.*s";
      if (OB_FAIL(databuff_printf(stmt_buf, stmt_len, stmt_pos, alloc, delete_fmt,
                                  database_name.length(), database_name.ptr(),
                                  table_name.length(), table_name.ptr(),
                                  static_cast<int>(where_pos), where_buf))) {
      } else {
        stmt_buf[stmt_pos++] = 0;
      }
    }
  }
  if (OB_SUCC(ret) && stmt_pos > 0) {
    if (OB_FAIL(op.begin_nested_session(fk_arg.is_self_ref_))) {
    } else {
      if (OB_FAIL(op.execute_write(stmt_buf))) {
      }
      int end_ret = op.end_nested_session();
      if (OB_SUCCESS != end_ret) {
        LOG_WARN("failed to end nested session", K(end_ret));
        if (OB_SUCCESS == ret) {
          ret = end_ret;
        }
      }
    }
  }
  return ret;
}

int ForeignKeyHandle::set_null(ObTableModifyOp &op,
                              const ObForeignKeyArg &fk_arg,
                              const ObExprPtrIArray &old_row)
{
  int ret = OB_SUCCESS;
  
  ObArenaAllocator alloc(ObModIds::OB_MODULE_PAGE_ALLOCATOR,
                          OB_MALLOC_NORMAL_BLOCK_SIZE);
  char *stmt_buf = nullptr;
  int64_t stmt_len = 0;
  char *where_buf = nullptr;
  int64_t where_len = 0;
  int64_t stmt_pos = 0;
  int64_t where_pos = 0;
  const ObString &database_name = fk_arg.database_name_;
  const ObString &table_name = fk_arg.table_name_;
  if (old_row.empty()) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(gen_where(op.get_eval_ctx(), where_buf, where_len, where_pos,
                               fk_arg.columns_, old_row, alloc, op.get_obj_print_params()))) {
    if (OB_LIKELY(OB_ERR_NULL_VALUE == ret)) {
      // skip cascade if any column in foreign key is NULL.
      ret = OB_SUCCESS;
      stmt_pos = 0;
    } else {
    }
  } else {
    const char *update_fmt = "update `%.*s`.`%.*s` set %.*s where %.*s";
    char *set_buf = nullptr;
    int64_t set_len = 0;
    int64_t set_pos = 0;
    if (OB_FAIL(gen_column_null_value(op.get_eval_ctx(), set_buf, set_len, set_pos, fk_arg.columns_, alloc, op.get_obj_print_params()))) {
    } else if (OB_FAIL(databuff_printf(stmt_buf, stmt_len, stmt_pos, alloc, update_fmt,
                                      database_name.length(), database_name.ptr(),
                                        table_name.length(), table_name.ptr(),
                                        static_cast<int>(set_pos), set_buf,
                                        static_cast<int>(where_pos), where_buf))) {
    } else {
      stmt_buf[stmt_pos++] = 0;
    }
  }
  
  if (OB_SUCC(ret) && stmt_pos > 0) {
    if (OB_FAIL(op.begin_nested_session(fk_arg.is_self_ref_))) {
    } else {
      if (OB_FAIL(op.execute_write(stmt_buf))) {
      }
      int end_ret = op.end_nested_session();
      if (OB_SUCCESS != end_ret) {
        LOG_WARN("failed to end nested session", K(end_ret));
        if (OB_SUCCESS == ret) {
          ret = end_ret;
        }
      }
    }
  }

  return ret;
}

int ForeignKeyHandle::gen_set(ObEvalCtx &eval_ctx, char *&buf, int64_t &len, int64_t &pos,
                              const ObIArray<ObForeignKeyColumn> &columns,
                              const ObExprPtrIArray &row,
                              ObIAllocator &alloc,
                              const ObObjPrintParams &print_params)
{
  return gen_column_value(eval_ctx, buf, len, pos, columns, row, ", ", alloc, print_params, false);
}

int ForeignKeyHandle::gen_where(ObEvalCtx &eval_ctx, char *&buf, int64_t &len, int64_t &pos,
                                const ObIArray<ObForeignKeyColumn> &columns,
                                const ObExprPtrIArray &row,
                                ObIAllocator &alloc,
                                const ObObjPrintParams &print_params)
{
  return gen_column_value(eval_ctx, buf, len, pos, columns, row, " and ", alloc, print_params, true);
}

int ForeignKeyHandle::gen_column_value(ObEvalCtx &eval_ctx, char *&buf, int64_t &len, int64_t &pos,
                                       const ObIArray<ObForeignKeyColumn> &columns,
                                       const ObExprPtrIArray &row,
                                       const char *delimiter,
                                       ObIAllocator &alloc,
                                       const ObObjPrintParams &print_params,
                                       bool forbid_null)
{
  const char *column_fmt = "`%.*s` = ";
  int ret = OB_SUCCESS;
  ObDatum *col_datum = NULL;
  if (OB_ISNULL(delimiter) || OB_ISNULL(print_params.tz_info_)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (columns.count() <= 0) {
    ret= OB_ERR_UNEXPECTED;
    LOG_ERROR("columns count of fk is zero or less than zero", K(ret), K(columns.count()), K(columns));
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < columns.count(); i++) {
    OZ(row.at(columns.at(i).idx_)->eval(eval_ctx, col_datum));
    if (OB_SUCC(ret)) {
      const ObString &col_name = columns.at(i).name_;
      const ObObjMeta &col_obj_meta = row.at(columns.at(i).idx_)->obj_meta_;
      ObObj col_obj;
      if (forbid_null && col_datum->is_null()) {
        ret = OB_ERR_NULL_VALUE;
        // NO LOG.
      } else if (OB_FAIL(databuff_printf(buf, len, pos, alloc, column_fmt,
                                        col_name.length(), col_name.ptr()))) {
      } else if (OB_FAIL(col_datum->to_obj(col_obj, col_obj_meta))) {
      } else if (OB_FAIL(col_obj.print_sql_literal(buf, len, pos, alloc, print_params))) {
      } else if (OB_FAIL(databuff_printf(buf, len, pos, alloc, "%s", delimiter))) {
      }
    }
  }
  if (OB_SUCC(ret)) {
    pos -= STRLEN(delimiter);
    buf[pos++] = 0;
  }
  return ret;
}

int  ForeignKeyHandle::gen_column_null_value(ObEvalCtx &ctx, char *&buf, int64_t &len, int64_t &pos,
                              const common::ObIArray<ObForeignKeyColumn> &columns,
                              common::ObIAllocator &alloc,
                              const common::ObObjPrintParams &print_params)
{
  int ret = OB_SUCCESS;
  const char *column_fmt = "`%.*s` = ";
  const char *delimiter = ", ";
  if (OB_ISNULL(delimiter) || OB_ISNULL(print_params.tz_info_)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (columns.count() <= 0) {
    ret= OB_ERR_UNEXPECTED;
    LOG_ERROR("columns count of fk is zero or less than zero", K(ret), K(columns.count()), K(columns));
  }
  ObObj col_obj;
  col_obj.set_null();
  for (int64_t i = 0; OB_SUCC(ret) && i < columns.count(); i++) {
    if (OB_SUCC(ret)) {
      const ObString &col_name = columns.at(i).name_;
      if (OB_FAIL(databuff_printf(buf, len, pos, alloc, column_fmt,
                                        col_name.length(), col_name.ptr()))) {
      } else if (OB_FAIL(col_obj.print_sql_literal(buf, len, pos, alloc, print_params))) {
      } else if (OB_FAIL(databuff_printf(buf, len, pos, alloc, "%s", delimiter))) {
      }
    }
  }
  if (OB_SUCC(ret)) {
    pos -= STRLEN(delimiter);
    buf[pos++] = 0;
  }
  return ret;
}

int ForeignKeyHandle::is_self_ref_row(ObEvalCtx &eval_ctx,
                                      const ObExprPtrIArray &row,
                                      const ObForeignKeyArg &fk_arg,
                                      bool &is_self_ref)
{
  int ret = OB_SUCCESS;
  const common::ObDatumAccessContext *access_ctx = nullptr;
  is_self_ref = fk_arg.is_self_ref_;
  ObDatum *name_col = NULL;
  ObDatum *val_col = NULL;
  if (OB_FAIL(eval_ctx.get_datum_access_ctx(access_ctx))) {
  }
  for (int64_t i = 0; OB_SUCC(ret) && is_self_ref && i < fk_arg.columns_.count(); i++) {
    const int32_t name_idx = fk_arg.columns_.at(i).name_idx_;
    const int32_t val_idx = fk_arg.columns_.at(i).idx_;
    ObExprCmpFuncType cmp_func = row.at(name_idx)->basic_funcs_->null_first_cmp_;
    // TODO qubin.qb: uncomment below block revert the defensive check
    //OZ((cmp_func = (row.at(name_idx)->basic_funcs_->null_first_cmp_))
    //   != row.at(val_idx)->basic_funcs_->null_first_cmp_);
    OZ(row.at(name_idx)->eval(eval_ctx, name_col));
    OZ(row.at(val_idx)->eval(eval_ctx, val_col));
    int cmp_ret = 0;
    if (OB_FAIL(cmp_func(*name_col, *val_col, cmp_ret, access_ctx))) {
    } else {
      is_self_ref = (0 == cmp_ret);
    }
  }
  return ret;
}

// make MY_SPEC macro available.
OB_INLINE static const ObTableModifySpec &get_my_spec(const ObTableModifyOp &op)
{
  return op.get_spec();
}

ObTableModifySpec::ObTableModifySpec(common::ObIAllocator &alloc,
                                     const ObPhyOperatorType type)
  : ObOpSpec(alloc, type),
    expr_frame_info_(NULL),
    ab_stmt_id_(nullptr),
    flags_(0),
    das_dop_(0)
{
}

OB_DEF_SERIALIZE(ObTableModifySpec)
{
  int ret = OB_SUCCESS;
  BASE_SER((ObTableModifySpec, ObOpSpec));
  OB_UNIS_ENCODE(flags_);
  OB_UNIS_ENCODE(ab_stmt_id_);
  OB_UNIS_ENCODE(das_dop_);
  return ret;
}

OB_DEF_DESERIALIZE(ObTableModifySpec)
{
  int ret = OB_SUCCESS;
  BASE_DESER((ObTableModifySpec, ObOpSpec));
  OB_UNIS_DECODE(flags_);
  OB_UNIS_DECODE(ab_stmt_id_);
  OB_UNIS_DECODE(das_dop_);
  return ret;
}

OB_DEF_SERIALIZE_SIZE(ObTableModifySpec)
{
  int64_t len = 0;
  BASE_ADD_LEN((ObTableModifySpec, ObOpSpec));
  OB_UNIS_ADD_LEN(flags_);
  OB_UNIS_ADD_LEN(ab_stmt_id_);
  OB_UNIS_ADD_LEN(das_dop_);
  return len;
}

OB_SERIALIZE_MEMBER(ObTableModifyOpInput);

ObTableModifyOp::ObTableModifyOp(ObExecContext &ctx,
                                 const ObOpSpec &spec,
                                 ObOpInput *input)
  : ObOperator(ctx, spec, input),
    sql_proxy_(NULL),
    inner_conn_guard_(),
    inner_conn_(NULL),
    saved_conn_(),
    need_foreign_key_check_(false),
    need_close_conn_(false),
    iter_end_(false),
    dml_rtctx_(eval_ctx_, ctx, *this),
    execute_single_row_(false),
    dml_modify_rows_(ctx.get_allocator()),
    last_store_row_(),
    saved_session_(NULL),
    ai_table_ctx_(),
    ai_table_ctx_loaded_(false),
    ai_metas_loaded_(false),
    ai_metas_filtered_(false),
    ai_meta_table_id_(common::OB_INVALID_ID),
    ai_metas_()
{
  obj_print_params_ = CREATE_OBJ_PRINT_PARAM(ctx_.get_my_session());
  obj_print_params_.need_cast_expr_ = true;
  // bugfix:
  // in NO_BACKSLASH_ESCAPES, obj_print_sql<ObVarcharType> won't escape.
  // We use skip_escape_ to indicate this case. It will finally be passed to ObHexEscapeSqlStr.
  GET_SQL_MODE_BIT(IS_NO_BACKSLASH_ESCAPES, ctx_.get_my_session()->get_sql_mode(), obj_print_params_.skip_escape_);
}

int ObTableModifyOp::inner_open()
{
  int ret = OB_SUCCESS;
  ObSQLSessionInfo *my_session = GET_MY_SESSION(ctx_);
  if (OB_FAIL(init_foreign_key_operation())) {
  } else if (MY_SPEC.plan_->has_nested_sql() && OB_FAIL(open_inner_conn())) {
  } else if (OB_FAIL(calc_single_table_loc())) {
  } else if (OB_FAIL(check_need_exec_single_row())) {
  } else {
    init_das_dml_ctx();
  }
  return ret;
}

void ObTableModifyOp::clear_dml_evaluated_flag(ObExpr *clear_expr)
{
  if (clear_expr->is_batch_result()) {
    clear_expr->get_evaluated_flags(eval_ctx_).unset(eval_ctx_.get_batch_idx());
  } else {
    clear_expr->get_eval_info(eval_ctx_).clear_evaluated_flag();
  }
  if (0 < clear_expr->parent_cnt_) {
    clear_dml_evaluated_flag(clear_expr->parent_cnt_, clear_expr->parents_);
  }
}

void ObTableModifyOp::clear_dml_evaluated_flag(int64_t parent_cnt, ObExpr **parent_exprs)
{
  for (int64_t i = 0; i < parent_cnt; ++i) {
    clear_dml_evaluated_flag(parent_exprs[i]);
  }
}

void ObTableModifyOp::clear_dml_evaluated_flag()
{
  // Clear the parent expr because the assignment expr may be changed
  for (int64_t i = 0; i < trigger_clear_exprs_.count(); ++i) {
    ObExpr *expr = trigger_clear_exprs_.at(i);
    clear_dml_evaluated_flag(expr);
  }
}

ObDasParallelType ObTableModifyOp::check_das_parallel_type()
{
  ObDasParallelType type = DAS_SERIALIZATION;
  // Local DAS modification runs serially.
  return type;
}

OB_INLINE int ObTableModifyOp::init_das_dml_ctx()
{
  ObSQLSessionInfo *session = GET_MY_SESSION(ctx_);
  const char *label = nullptr;
  switch(MY_SPEC.type_) {
  case PHY_INSERT:
    label = "InsDASCtx";
    break;
  case PHY_UPDATE:
    label = "UpdDASCtx";
    break;
  case PHY_DELETE:
    label = "DelDASCtx";
    break;
  case PHY_LOCK:
    label = "LockDASCtx";
    break;
  default:
    label = "DmlDASCtx";
    break;
  }
  ObMemAttr memattr(label, ObCtxIds::EXECUTE_CTX_ID);
  dml_rtctx_.das_ref_.set_expr_frame_info(nullptr != MY_SPEC.expr_frame_info_
                                        ? MY_SPEC.expr_frame_info_
                                        : &MY_SPEC.plan_->get_expr_frame_info());
  dml_rtctx_.das_ref_.set_mem_attr(memattr);
  dml_rtctx_.das_ref_.get_das_parallel_ctx().set_das_parallel_type(check_das_parallel_type());
  dml_rtctx_.das_ref_.get_das_parallel_ctx().set_das_dop(ctx_.get_das_ctx().get_real_das_dop());
  if (check_das_parallel_type() != DAS_SERIALIZATION) {
    LOG_TRACE("this sql use das parallel submit", K(check_das_parallel_type()));
  }
  return OB_SUCCESS;
}

int ObTableModifyOp::replace_implict_cursor(int64_t affected_rows,
                                            int64_t found_rows,
                                            int64_t matched_rows,
                                            int64_t duplicated_rows)
{
  int ret = OB_SUCCESS;
  bool is_ins_val_opt = ctx_.get_sql_ctx()->is_do_insert_batch_opt();
  ObPhysicalPlanCtx *plan_ctx = GET_PHY_PLAN_CTX(ctx_);
  if (MY_SPEC.ab_stmt_id_ != nullptr && !is_ins_val_opt) {
    ObDatum *stmt_id_datum = nullptr;
    ObImplicitCursorInfo implicit_cursor;
    if (OB_FAIL(prepare_implict_cursor(affected_rows,
                                       found_rows,
                                       matched_rows,
                                       duplicated_rows,
                                       implicit_cursor))) {
    } else if (OB_FAIL(plan_ctx->replace_implicit_cursor_info(implicit_cursor))) {
    }
  }
  return ret;
}

int ObTableModifyOp::prepare_implict_cursor(int64_t affected_rows,
                                            int64_t found_rows,
                                            int64_t matched_rows,
                                            int64_t duplicated_rows,
                                            ObImplicitCursorInfo &implicit_cursor)
{
  int ret = OB_SUCCESS;
  ObDatum *stmt_id_datum = nullptr;
  if (OB_FAIL(MY_SPEC.ab_stmt_id_->eval(eval_ctx_, stmt_id_datum))) {
  } else {
    ObPhysicalPlanCtx *plan_ctx = GET_PHY_PLAN_CTX(ctx_);
    int64_t stmt_id = stmt_id_datum->get_int();
    implicit_cursor.stmt_id_ = stmt_id;
    implicit_cursor.found_rows_ += found_rows;
    implicit_cursor.matched_rows_ += matched_rows;
    implicit_cursor.affected_rows_ += affected_rows;
    implicit_cursor.duplicated_rows_ += duplicated_rows;
    implicit_cursor.last_insert_id_ = plan_ctx->get_autoinc_col_value();
  }
  return ret;
}

int ObTableModifyOp::merge_implict_cursor(int64_t affected_rows,
                                          int64_t found_rows,
                                          int64_t matched_rows,
                                          int64_t duplicated_rows)
{
  int ret = OB_SUCCESS;
  bool is_ins_val_opt = ctx_.get_sql_ctx()->is_do_insert_batch_opt();
  ObPhysicalPlanCtx *plan_ctx = GET_PHY_PLAN_CTX(ctx_);
  if (MY_SPEC.ab_stmt_id_ != nullptr && !is_ins_val_opt) {
    ObDatum *stmt_id_datum = nullptr;
    ObImplicitCursorInfo implicit_cursor;
    if (OB_FAIL(prepare_implict_cursor(affected_rows,
                                       found_rows,
                                       matched_rows,
                                       duplicated_rows,
                                       implicit_cursor))) {
    } else if (OB_FAIL(plan_ctx->merge_implicit_cursor_info(implicit_cursor))) {
    }
  }
  return ret;
}

int ObTableModifyOp::inner_switch_iterator()
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(ObOperator::inner_switch_iterator())) {
  }

  return ret;
}

int ObTableModifyOp::inner_close()
{
  int ret = OB_SUCCESS;
  ObPhysicalPlanCtx *plan_ctx = ctx_.get_physical_plan_ctx();
  // release cache_handle for auto-increment
  share::ObAutoincrementService &auto_service = share::ObAutoincrementService::get_instance();
  ObIArray<share::AutoincParam> &autoinc_params = plan_ctx->get_autoinc_params();
  for (int64_t i = 0; i < autoinc_params.count(); ++i) {
    if (NULL != autoinc_params.at(i).cache_handle_) {
      auto_service.release_handle(autoinc_params.at(i).cache_handle_);
    }
  }
  close_inner_conn();
  if (dml_rtctx_.das_ref_.has_task()) {
    if (OB_FAIL(dml_rtctx_.das_ref_.close_all_task())) {
    } else {
      dml_rtctx_.das_ref_.reset();
    }
  }
  dml_modify_rows_.clear();
  return ret;
}

OB_INLINE int ObTableModifyOp::init_foreign_key_operation()
{
  int ret = OB_SUCCESS;
  ObPhysicalPlanCtx *phy_plan_ctx = GET_PHY_PLAN_CTX(ctx_);
  if (OB_ISNULL(phy_plan_ctx)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (!phy_plan_ctx->need_foreign_key_checks()) {
    // set fk check even if ret != OB_SUCCESS. see ObTableModifyOp::inner_close()
    need_foreign_key_check_ = false;
  } else {
    need_foreign_key_check_ = MY_SPEC.need_foreign_key_check_;
  }
  return ret;
}

int ObTableModifyOp::inner_rescan()
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(ObOperator::inner_rescan())) {
  } else {
    iter_end_ = false;
    if (dml_rtctx_.das_ref_.has_task()) {
      if (OB_FAIL(dml_rtctx_.das_ref_.close_all_task())) {
      } else {
        dml_rtctx_.reuse();
      }
    }
  }
  if (OB_SUCC(ret)) {
    dml_modify_rows_.clear();
    if (OB_FAIL(calc_single_table_loc())) {
    }
  }
  return ret;
}

int ObTableModifyOp::check_need_exec_single_row() {
  int ret = OB_SUCCESS;
  if (MY_SPEC.is_returning_ && (need_foreign_key_checks() || MY_SPEC.need_trigger_fire_)) {
    execute_single_row_ = true;
  }
  return ret;
}

int ObTableModifyOp::get_gi_task()
{
  int ret = OB_SUCCESS;
  ObGranuleTaskInfo gi_task_info;
  GIPrepareTaskMap *gi_prepare_map = nullptr;
  ObDASCtx &das_ctx = ctx_.get_das_ctx();
  ObTableID table_loc_id = OB_INVALID_ID;
  ObTableID ref_table_id = OB_INVALID_ID;
  ObTabletID tablet_id;
  if (OB_FAIL(MY_SPEC.get_single_table_loc_id(table_loc_id, ref_table_id))) {
  } else if (OB_FAIL(ctx_.get_gi_task_map(gi_prepare_map))) {
  } else if (OB_FAIL(gi_prepare_map->get_refactored(MY_SPEC.id_, gi_task_info))) {
    if (ret != OB_HASH_NOT_EXIST) {
    } else {
      LOG_DEBUG("no prepared task info, set table modify to end",
        K(MY_SPEC.get_id()), K(this), K(lbt()));
      // Current DML operator cannot obtain task from GI, indicating current DML operator iter end
      iter_end_ = true;
      ret = OB_SUCCESS;
    }
  } else if (OB_ISNULL(get_input()->table_loc_) &&
      OB_ISNULL(get_input()->table_loc_ = das_ctx.get_table_loc_by_id(table_loc_id, ref_table_id))) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    get_input()->tablet_loc_ = gi_task_info.tablet_loc_;
    LOG_DEBUG("DML operator consume a task", K(ret),
              K(gi_task_info), KPC(get_input()), K(MY_SPEC.id_), K(get_input()));
  }
  return ret;
}

int ObTableModifyOp::calc_single_table_loc()
{
  int ret = OB_SUCCESS;
  if (!MY_SPEC.use_dist_das_) {
    ObTableID table_loc_id = OB_INVALID_ID;
    ObTableID ref_table_id = OB_INVALID_ID;
    ObDASCtx &das_ctx = ctx_.get_das_ctx();
    ObDASTableLoc *table_loc = nullptr;
    if (OB_UNLIKELY(MY_SPEC.gi_above_)) {
      if (OB_FAIL(get_gi_task())) {
      }
    } else if (OB_FAIL(MY_SPEC.get_single_table_loc_id(table_loc_id, ref_table_id))) {
    } else if (OB_ISNULL(table_loc = das_ctx.get_table_loc_by_id(table_loc_id, ref_table_id))) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      get_input()->table_loc_ = table_loc;
      get_input()->tablet_loc_ = table_loc->get_first_tablet_loc();
    }
  }
  return ret;
}

int ObTableModifyOp::open_inner_conn()
{
  int ret = OB_SUCCESS;
  ObSQLSessionInfo *session = NULL;
  if (OB_ISNULL(sql_proxy_ = ctx_.get_sql_proxy())) {
    ret = OB_NOT_INIT;
  } else if (OB_ISNULL(session = ctx_.get_my_session())) {
    ret = OB_NOT_INIT;
  } else if (NULL != session->get_inner_conn()) {
    // do nothing.
  } else if (OB_FAIL(
                 query::ObInnerSQLConnectionAccess::
                     create_connection_with_external_session(
                         session, inner_conn_guard_))) {
  } else if (OB_ISNULL(inner_conn_guard_.get_ptr())) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    /**
     * session is the only data struct which can pass through multi layer nested sql,
     * so we put inner conn in session to share it within multi layer nested sql.
     */
    session->set_inner_conn(inner_conn_guard_.get_ptr());
    need_close_conn_ = true;
  }
  if (OB_SUCC(ret)) {
    inner_conn_ = as_inner_sql_connection(session->get_inner_conn());
    if (OB_ISNULL(inner_conn_)) {
      ret = OB_ERR_UNEXPECTED;
    }
  }
  return ret;
}

int ObTableModifyOp::close_inner_conn()
{
  /**
   * we can call it even if open_inner_conn() failed, because only the one who call
   * open_inner_conn() succeed first will do close job by "if (need_close_conn_)".
   */
  int ret = OB_SUCCESS;
  if (need_close_conn_) {
    ObSQLSessionInfo *session = ctx_.get_my_session();
    if (OB_ISNULL(sql_proxy_) || OB_ISNULL(session)) {
      ret = OB_INVALID_ARGUMENT;
    } else {
      OX(session->set_inner_conn(NULL));
      OX(inner_conn_guard_.reset());
    }
    need_close_conn_ = false;
  }
  sql_proxy_ = NULL;
  inner_conn_guard_.reset();
  inner_conn_ = NULL;
  return ret;
}

int ObTableModifyOp::begin_nested_session(bool skip_cur_stmt_tables)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(inner_conn_)) {
    ret = OB_NOT_INIT;
  } else if (OB_FAIL(inner_conn_->begin_nested_session(get_saved_session(), saved_conn_,
                                                       skip_cur_stmt_tables))) {
  }
  return ret;
}

int ObTableModifyOp::end_nested_session()
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(inner_conn_)) {
    ret = OB_NOT_INIT;
  } else if (OB_FAIL(inner_conn_->end_nested_session(get_saved_session(), saved_conn_))) {
  }
  return ret;
}
int ObTableModifyOp::execute_write(const char *sql)
{
  int ret = OB_SUCCESS;
  int64_t affected_rows = 0;
  if (OB_ISNULL(inner_conn_)) {
    ret = OB_NOT_INIT;
  } else if (OB_ISNULL(sql)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(inner_conn_->execute_write(sql, affected_rows))) {
  }
  return ret;
}

int ObTableModifyOp::execute_read(const char *sql, ObMySQLProxy::MySQLResult &res)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(inner_conn_) || OB_ISNULL(sql_proxy_)) {
    ret = OB_NOT_INIT;
  } else if (OB_ISNULL(sql)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(inner_conn_->execute_read(sql, res))) {
  }
 return ret;
}

int ObTableModifyOp::check_stack()
{
  int ret = OB_SUCCESS;
  const int max_stack_deep = 16;
  bool is_stack_overflow = false;
  ObSQLSessionInfo *session = ctx_.get_my_session();
  if (OB_ISNULL(session)) {
    ret = OB_ERR_NULL_VALUE;
  } else if (session->get_nested_count() > max_stack_deep) {
    ret = OB_SIZE_OVERFLOW;
  } else if (OB_FAIL(check_stack_overflow(is_stack_overflow))) {
  } else if (is_stack_overflow) {
    ret = OB_SIZE_OVERFLOW;
  }
  return ret;
}
OperatorOpenOrder ObTableModifyOp::get_operator_open_order() const
{
  OperatorOpenOrder open_order = OPEN_CHILDREN_FIRST;
  if (spec_.plan_->is_use_px()) {
    // If PX is enabled in DML, there are two scenarios:
    // 1. PDML: in pdml, pdml-op completely does not need its corresponding input to provide runtime parameters, so it will directly return
    // open_order = OPEN_CHILDREN_FIRST
    // 2. DML+PX: In this case, the parameters of the dml run are completely fed by the GI operator above it, for example, such a plan:
    //   PX COORD
    //    PX TRANSMIT
    //      GI (FULL PARTITION WISE)
    //       DELETE
    //        TSC
    //  Such a plan requires GI to drive the deletion, GI rescan delete each time a new Task is iterated,
    //  Pass the information of the corresponding Task to the delete operator.
    open_order = OPEN_CHILDREN_FIRST;
  }
  return open_order;
}

int ObTableModifyOp::submit_all_dml_task()
{
  int ret = OB_SUCCESS;
  if (dml_rtctx_.das_ref_.has_task()) {
    if (dml_rtctx_.need_pick_del_task_first() &&
                OB_FAIL(dml_rtctx_.das_ref_.pick_del_task_to_first())) {
    } else if (OB_FAIL(dml_rtctx_.das_ref_.execute_all_task())) {
    } else if (OB_FAIL(dml_rtctx_.das_ref_.close_all_task())) {
    } else if (OB_FAIL(ObDMLService::handle_after_row_processing(this, &get_dml_modify_row_list()))) {
    } else {
      dml_modify_rows_.clear();
      dml_rtctx_.reuse();
    }
  }
  return ret;
}

//The data to be written by DML will be buffered in the DAS Write Buffer
//When the buffer data exceeds 6M,
//needs to be written to the storage to release the memory.
int ObTableModifyOp::discharge_das_write_buffer()
{
  int ret = OB_SUCCESS;
  if (dml_rtctx_.need_submit_all_tasks()) {
    LOG_INFO("DASWriteBuffer full, now to write storage",
             "buffer memory", dml_rtctx_.das_ref_.get_das_alloc().used(),
             K(dml_rtctx_.get_das_task_memory_size()), K(dml_rtctx_.get_das_parallel_task_size()));
    ret = submit_all_dml_task();
  } else if (execute_single_row_) {
    if (REACH_COUNT_INTERVAL(100)) { // print log per 100 times.
    }
    ret = submit_all_dml_task();
  }
  return ret;
}

int ObTableModifyOp::get_next_row_from_child()
{
  int ret = OB_SUCCESS;
  clear_evaluated_flag();
  if (OB_FAIL(child_->get_next_row())) {
    if (OB_ITER_END != ret) {
    }
  } else {
    LOG_DEBUG("child output row", "row", ROWEXPR2STR(eval_ctx_, child_->get_spec().output_));
  }
  return ret;
}

int ObTableModifyOp::inner_get_next_row()
{
  int ret = OB_SUCCESS;
  if (iter_end_) {
    ret = OB_ITER_END;
  } else {
    int64_t row_count = 0;
    while (OB_SUCC(ret)) {
      if (OB_FAIL(try_check_status())) {
      } else if (OB_FAIL(get_next_row_from_child())) {
        if (OB_ITER_END != ret) {
        } else {
          iter_end_ = true;
          ret = OB_SUCCESS;
          break;
        }
      } else if (OB_FAIL(write_row_to_das_buffer())) {
      } else if (OB_FAIL(discharge_das_write_buffer())) {
      } else if (MY_SPEC.is_returning_) {
        break;
      }
      row_count ++;
    }

    if (OB_FAIL(ret)) {
      record_err_for_load_data(ret, row_count);
    }

    if (OB_SUCC(ret) && iter_end_ && dml_rtctx_.das_ref_.has_task()) {
      //DML operator reach iter end,
      //now submit the remaining rows in the DAS Write Buffer to the storage
      if (OB_FAIL(submit_all_dml_task())) {
      }
    }
    //to post process the DML info after writing all data to the storage or returning one row
    ret = write_rows_post_proc(ret);
    if (OB_SUCC(ret) && iter_end_) {
      ret = OB_ITER_END;
    }
  }
  return ret;
}

int ObTableModifyOp::perform_batch_fk_check()
{
  DEBUG_SYNC(BEFORE_FOREIGN_KEY_CONSTRAINT_CHECK);
  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < fk_checkers_.count(); ++i) {
    bool all_has_result = false;
    ObForeignKeyChecker *fk_checker = fk_checkers_.at(i);
    if (OB_ISNULL(fk_checker)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(fk_checker->do_fk_check_batch(all_has_result))) {
    } else if (!all_has_result) {
      ret = OB_ERR_NO_REFERENCED_ROW;
    }
  }
  return ret;
}
// ==================== AI column support ====================
//
// An AI column is a normal column whose value is produced asynchronously by a
// background worker: every DML that inserts a row, changes a column the AI
// expression depends on, or moves the row key, must (1) invalidate the AI
// values of the row and (2) queue one regeneration task per AI column into
// oceanbase.__all_ai_column_task. A delete removes the pending tasks of the
// deleted row key.
//
// All of this runs in the after-row phase through the session bound inner
// connection, i.e. inside the very same transaction as the user statement, so
// a rollback removes the queued tasks and the invalidation together.

static bool ai_is_ident_char(char c)
{
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
      || (c >= '0' && c <= '9') || '_' == c;
}

// Appends a single quoted SQL string literal. Single quotes are doubled,
// everything else is copied verbatim.
static int ai_append_str_literal(ObSqlString &sql, const char *str)
{
  int ret = OB_SUCCESS;
  if (OB_NOT_NULL(str)) {
    if (OB_FAIL(sql.append("'"))) {
    } else {
      for (int64_t i = 0; OB_SUCC(ret) && '\0' != str[i]; i++) {
        if ('\'' == str[i]) {
          if (OB_FAIL(sql.append("''"))) {
          }
        } else if (OB_FAIL(sql.append_fmt("%c", str[i]))) {
        }
      }
      if (OB_SUCC(ret) && OB_FAIL(sql.append("'"))) {
      }
    }
  }
  return ret;
}

bool ObTableModifyOp::ai_expr_has_identifier_(const char *expr, const char *ident)
{
  // Whole-word search that skips quoted string literals, so that a column
  // named "content" is not found inside 'Classify: content'.
  bool found = false;
  if (OB_NOT_NULL(expr) && OB_NOT_NULL(ident) && '\0' != ident[0]) {
    const int64_t ident_len = static_cast<int64_t>(strlen(ident));
    bool in_squote = false;
    bool in_dquote = false;
    for (int64_t i = 0; !found && '\0' != expr[i];) {
      const char c = expr[i];
      if (in_squote) {
        if ('\'' == c && '\'' == expr[i + 1]) {
          i += 2;
        } else {
          if ('\'' == c) { in_squote = false; }
          i++;
        }
      } else if (in_dquote) {
        if ('"' == c) { in_dquote = false; }
        i++;
      } else if ('\'' == c) {
        in_squote = true;
        i++;
      } else if ('"' == c) {
        in_dquote = true;
        i++;
      } else if (ai_is_ident_char(c)) {
        int64_t j = i;
        while (ai_is_ident_char(expr[j])) { j++; }
        if (j - i == ident_len && 0 == STRNCMP(expr + i, ident, ident_len)) {
          found = true;
        }
        i = j;
      } else {
        i++;
      }
    }
  }
  return found;
}

int64_t ObTableModifyOp::ai_row_pos_of_col_id_(const ObDMLBaseCtDef &dml_ctdef, uint64_t col_id)
{
  int64_t pos = OB_INVALID_INDEX;
  for (int64_t i = 0; OB_INVALID_INDEX == pos && i < dml_ctdef.column_ids_.count(); i++) {
    if (dml_ctdef.column_ids_.at(i) == col_id) {
      pos = i;
    }
  }
  return pos;
}

int ObTableModifyOp::load_ai_table_ctx_(uint64_t table_id)
{
  int ret = OB_SUCCESS;
  if (ai_table_ctx_loaded_ && ai_table_ctx_.table_id_ == table_id) {
    // cached for the whole statement
  } else {
    share::schema::ObSchemaGetterGuard &schema_guard = *ctx_.get_sql_ctx()->schema_guard_;
    const share::schema::ObTableSchema *table_schema = NULL;
    const share::schema::ObDatabaseSchema *db_schema = NULL;
    ObArray<uint64_t> rowkey_ids;
    ai_table_ctx_.reset();
    ai_table_ctx_.table_id_ = table_id;
    if (OB_ISNULL(ctx_.get_sql_ctx()->schema_guard_)) {
      ret = OB_ERR_UNEXPECTED;
      SQL_ENG_LOG(WARN, "ai column: schema guard is null", K(ret));
    } else if (OB_FAIL(schema_guard.get_table_schema(table_id, table_schema))) {
      SQL_ENG_LOG(WARN, "ai column: get table schema failed", K(ret), K(table_id));
    } else if (OB_ISNULL(table_schema)) {
      ret = OB_ERR_UNEXPECTED;
      SQL_ENG_LOG(WARN, "ai column: table schema is null", K(ret), K(table_id));
    } else if (OB_FAIL(schema_guard.get_database_schema(table_schema->get_database_id(), db_schema))) {
      SQL_ENG_LOG(WARN, "ai column: get database schema failed", K(ret));
    } else if (OB_ISNULL(db_schema)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(table_schema->get_rowkey_column_ids(rowkey_ids))) {
      SQL_ENG_LOG(WARN, "ai column: get rowkey column ids failed", K(ret));
    } else {
      const ObString db_name = db_schema->get_database_name_str();
      const ObString tbl_name = table_schema->get_table_name_str();
      if (db_name.length() >= static_cast<int>(sizeof(ai_table_ctx_.db_name_))
          || tbl_name.length() >= static_cast<int>(sizeof(ai_table_ctx_.table_name_))) {
        ret = OB_SIZE_OVERFLOW;
      } else {
        MEMCPY(ai_table_ctx_.db_name_, db_name.ptr(), db_name.length());
        ai_table_ctx_.db_name_[db_name.length()] = '\0';
        MEMCPY(ai_table_ctx_.table_name_, tbl_name.ptr(), tbl_name.length());
        ai_table_ctx_.table_name_[tbl_name.length()] = '\0';
        const int64_t col_cnt = table_schema->get_column_count();
        for (int64_t i = 0; OB_SUCC(ret) && i < col_cnt; i++) {
          const share::schema::ObColumnSchemaV2 *col = table_schema->get_column_schema_by_idx(i);
          if (OB_ISNULL(col)) {
            ret = OB_ERR_UNEXPECTED;
          } else if (i >= ObAiTableCtx::MAX_AI_COLS) {
            ret = OB_SIZE_OVERFLOW;
            SQL_ENG_LOG(WARN, "ai column: too many columns", K(ret), K(col_cnt));
          } else {
            const ObString col_name = col->get_column_name_str();
            if (col_name.length() >= static_cast<int>(sizeof(ai_table_ctx_.col_names_[i]))) {
              ret = OB_SIZE_OVERFLOW;
            } else {
              MEMCPY(ai_table_ctx_.col_names_[i], col_name.ptr(), col_name.length());
              ai_table_ctx_.col_names_[i][col_name.length()] = '\0';
              ai_table_ctx_.col_ids_[i] = col->get_column_id();
              ai_table_ctx_.col_cnt_ = i + 1;
            }
          }
        }
        for (int64_t r = 0; OB_SUCC(ret) && r < rowkey_ids.count(); r++) {
          bool found = false;
          for (int64_t c = 0; !found && c < ai_table_ctx_.col_cnt_; c++) {
            if (ai_table_ctx_.col_ids_[c] == rowkey_ids.at(r)) {
              ai_table_ctx_.rowkey_pos_[ai_table_ctx_.rowkey_cnt_++] = c;
              found = true;
            }
          }
          if (!found) {
            ret = OB_ERR_UNEXPECTED;
            SQL_ENG_LOG(WARN, "ai column: rowkey column not found in table", K(ret),
                        K(rowkey_ids.at(r)));
          }
        }
      }
    }
    if (OB_SUCC(ret)) {
      ai_table_ctx_loaded_ = true;
    } else {
      ai_table_ctx_.reset();
      ai_table_ctx_loaded_ = false;
    }
  }
  return ret;
}

int ObTableModifyOp::load_ai_column_metas_(uint64_t table_id)
{
  int ret = OB_SUCCESS;
  if (ai_metas_loaded_ && ai_meta_table_id_ == table_id) {
    // cached for the whole statement
  } else {
    ObSqlString sql;
    SMART_VAR(ObMySQLProxy::MySQLResult, res) {
      ObMySQLResult *result = NULL;
      ai_metas_.reset();
      ai_meta_table_id_ = table_id;
      ai_metas_filtered_ = false;
      if (OB_FAIL(sql.assign_fmt("SELECT column_id, column_name, source_expr, ai_model_name, "
                                 "dimension, max_retries FROM oceanbase.%s WHERE table_id = %ld",
                                 OB_ALL_AI_COLUMN_TNAME, table_id))) {
        SQL_ENG_LOG(WARN, "ai column: build meta sql failed", K(ret));
      } else if (OB_FAIL(execute_read(sql.ptr(), res))) {
        SQL_ENG_LOG(WARN, "ai column: read meta failed", K(ret), K(sql));
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
          } else {
            meta.column_id_ = static_cast<uint64_t>(col_id);
            meta.dimension_ = dimension;
            meta.max_retries_ = max_retries;
            if (column_name.length() < static_cast<int>(sizeof(meta.column_name_))
                && source_expr.length() < static_cast<int>(sizeof(meta.source_expr_))
                && ai_model_name.length() < static_cast<int>(sizeof(meta.ai_model_name_))) {
              MEMCPY(meta.column_name_, column_name.ptr(), column_name.length());
              meta.column_name_[column_name.length()] = '\0';
              MEMCPY(meta.source_expr_, source_expr.ptr(), source_expr.length());
              meta.source_expr_[source_expr.length()] = '\0';
              MEMCPY(meta.ai_model_name_, ai_model_name.ptr(), ai_model_name.length());
              meta.ai_model_name_[ai_model_name.length()] = '\0';
              if (OB_FAIL(ai_metas_.push_back(meta))) {
                SQL_ENG_LOG(WARN, "ai column: push meta failed", K(ret));
              }
            } else {
              ret = OB_SIZE_OVERFLOW;
              SQL_ENG_LOG(WARN, "ai column: meta value too long", K(ret));
            }
          }
        }
        if (OB_ITER_END == ret) {
          ret = OB_SUCCESS;
        }
      }
      if (OB_SUCC(ret)) {
        ai_metas_loaded_ = true;
      } else {
        ai_metas_.reset();
        ai_meta_table_id_ = common::OB_INVALID_ID;
      }
    }
  }
  return ret;
}

int ObTableModifyOp::ai_gen_request_id_(ObSqlString &rid)
{
  // Shared by every AI column of one modified row so that the worker can
  // process and publish the whole group atomically.
  int ret = OB_SUCCESS;
  static volatile int64_t seq = 0;
  const int64_t cur = __sync_fetch_and_add(&seq, 1);
  if (OB_FAIL(rid.assign_fmt("req_%ld_%ld", ObTimeUtil::current_time(), cur))) {
    SQL_ENG_LOG(WARN, "ai column: gen request id failed", K(ret));
  }
  return ret;
}

// Formats `col` = <literal> for every row key column, e.g. "`id` = 2".
int ObTableModifyOp::ai_gen_rowkey_predicate_(const ObDMLBaseCtDef &dml_ctdef,
                                              const ObExprPtrIArray &row,
                                              ObSqlString &pred,
                                              bool &is_null_pk)
{
  int ret = OB_SUCCESS;
  ObArenaAllocator alloc(ObModIds::OB_MODULE_PAGE_ALLOCATOR, OB_MALLOC_NORMAL_BLOCK_SIZE);
  char *buf = NULL;
  int64_t buf_len = 1024;  // matches __all_ai_column_task.rowkey_predicate
  int64_t pos = 0;
  is_null_pk = false;
  if (OB_ISNULL(buf = static_cast<char *>(alloc.alloc(buf_len)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    for (int64_t r = 0; OB_SUCC(ret) && r < ai_table_ctx_.rowkey_cnt_; r++) {
      const int64_t row_pos = ai_row_pos_of_col_id_(dml_ctdef,
          ai_table_ctx_.col_ids_[ai_table_ctx_.rowkey_pos_[r]]);
      ObDatum *datum = NULL;
      if (OB_INVALID_INDEX == row_pos || row_pos >= row.count()) {
        ret = OB_ERR_UNEXPECTED;
        SQL_ENG_LOG(WARN, "ai column: rowkey column missing in row", K(ret), K(r));
      } else if (OB_FAIL(row.at(row_pos)->eval(eval_ctx_, datum))) {
        SQL_ENG_LOG(WARN, "ai column: eval rowkey expr failed", K(ret));
      } else if (OB_ISNULL(datum) || datum->is_null()) {
        is_null_pk = true;
        SQL_ENG_LOG(WARN, "ai column: null rowkey datum", "col_name",
                    ai_table_ctx_.col_names_[ai_table_ctx_.rowkey_pos_[r]], K(row_pos));
      } else {
        const char *col_name = ai_table_ctx_.col_names_[ai_table_ctx_.rowkey_pos_[r]];
        ObObj obj;
        if (OB_FAIL(databuff_printf(buf, buf_len, pos, alloc, "`%s` = ", col_name))) {
        } else if (OB_FAIL(datum->to_obj(obj, row.at(row_pos)->obj_meta_))) {
        } else if (OB_FAIL(obj.print_sql_literal(buf, buf_len, pos, alloc, obj_print_params_))) {
        } else if (OB_FAIL(databuff_printf(buf, buf_len, pos, alloc, " and "))) {
        }
      }
    }
    if (OB_SUCC(ret) && !is_null_pk) {
      if (pos >= 5) {
        pos -= 5;  // drop the trailing " and "
      }
      buf[pos++] = '\0';
      if (OB_FAIL(pred.assign(buf))) {
        SQL_ENG_LOG(WARN, "ai column: assign predicate failed", K(ret));
      }
    }
  }
  return ret;
}

int ObTableModifyOp::ai_process_insert_(const ObDMLBaseCtDef &dml_ctdef)
{
  int ret = OB_SUCCESS;
  bool is_null_pk = false;
  ObSqlString pred;
  ObSqlString request_id;
  ObSqlString sql;
  int64_t affected = 0;
  // 1. Does the inserted row carry an explicit AI column value? If so it has
  //    to be invalidated like on update.
  bool need_invalidate = false;
  for (int64_t m = 0; !need_invalidate && m < ai_metas_.count(); m++) {
    const int64_t pos = ai_row_pos_of_col_id_(dml_ctdef, ai_metas_.at(m).column_id_);
    ObDatum *datum = NULL;
    if (OB_INVALID_INDEX != pos && pos < dml_ctdef.new_row_.count()) {
      if (OB_FAIL(dml_ctdef.new_row_.at(pos)->eval(eval_ctx_, datum))) {
        SQL_ENG_LOG(WARN, "ai column: eval ai column failed", K(ret));
        break;
      }
      if (OB_NOT_NULL(datum) && !datum->is_null()) {
        need_invalidate = true;
      }
    }
  }
  if (OB_FAIL(ret)) {
    // already logged
  } else if (OB_FAIL(ai_gen_rowkey_predicate_(dml_ctdef, dml_ctdef.new_row_, pred, is_null_pk))) {
  } else if (is_null_pk) {
    // heap table hidden primary key is filled before the row is written; the
    // predicate is regenerated from the stored row, which is fine.
    ret = OB_ERR_UNEXPECTED;
    SQL_ENG_LOG(WARN, "ai column: null primary key on insert", K(ret));
  } else if (OB_FAIL(ai_gen_request_id_(request_id))) {
  } else {
    if (need_invalidate) {
      ObSqlString invalidate;
      if (OB_FAIL(invalidate.assign_fmt("UPDATE `%s`.`%s` SET ",
                                        ai_table_ctx_.db_name_, ai_table_ctx_.table_name_))) {
      } else {
        for (int64_t m = 0; OB_SUCC(ret) && m < ai_metas_.count(); m++) {
          if (m > 0 && OB_FAIL(invalidate.append(", "))) {
          } else if (OB_FAIL(invalidate.append_fmt("`%s` = NULL", ai_metas_.at(m).column_name_))) {
          }
        }
        if (OB_SUCC(ret) && OB_FAIL(invalidate.append_fmt(" WHERE %s", pred.ptr()))) {
        } else if (OB_FAIL(execute_write(invalidate.ptr()))) {
          SQL_ENG_LOG(WARN, "ai column: invalidate on insert failed", K(ret), K(invalidate));
        }
      }
    }
    if (OB_SUCC(ret)) {
      if (OB_FAIL(sql.assign_fmt("INSERT INTO oceanbase.%s (table_id, rowkey_predicate, column_id, "
                                 "request_id, state, retry_count, max_retries, eligible_ts, "
                                 "normalized_expr, ai_model_name) "
                                 "VALUES ",
                                 OB_ALL_AI_COLUMN_TASK_TNAME))) {
      } else {
        for (int64_t m = 0; OB_SUCC(ret) && m < ai_metas_.count(); m++) {
          const share::ObAiColumnMetaInfo &meta = ai_metas_.at(m);
          ObSqlString esc_expr;
          ObSqlString esc_model;
          if (m > 0 && OB_FAIL(sql.append(", "))) {
          } else if (OB_FAIL(sql.append_fmt("(%ld, ", dml_ctdef.das_base_ctdef_.index_tid_))) {
          } else if (OB_FAIL(ai_append_str_literal(sql, pred.ptr()))) {
          } else if (OB_FAIL(sql.append_fmt(", %ld, ", meta.column_id_))) {
          } else if (OB_FAIL(ai_append_str_literal(sql, request_id.ptr()))) {
          } else if (OB_FAIL(sql.append_fmt(", %ld, 0, %ld, CURRENT_TIMESTAMP(6), ",
                                            share::OB_AI_COL_TASK_STATE_PENDING,
                                            meta.max_retries_))) {
          } else if (OB_FAIL(esc_expr.append(meta.source_expr_))) {
          } else if (OB_FAIL(ai_append_str_literal(sql, esc_expr.ptr()))) {
          } else if (OB_FAIL(sql.append(", "))) {
          } else if (OB_FAIL(esc_model.append(meta.ai_model_name_))) {
          } else if (OB_FAIL(ai_append_str_literal(sql, esc_model.ptr()))) {
          } else if (OB_FAIL(sql.append_fmt(")"))) {
          }
        }
        if (OB_SUCC(ret) && ai_metas_.count() > 0) {
          const int64_t write_ret = execute_write(sql.ptr());
          if (OB_FAIL(write_ret)) {
            ret = write_ret;
          }
        }
      }
    }
  }
  return ret;
}

int ObTableModifyOp::ai_process_update_(const ObDMLBaseCtDef &dml_ctdef)
{
  int ret = OB_SUCCESS;
  bool pk_changed = false;
  bool dep_changed = false;
  bool is_null_pk = false;
  ObSqlString new_pred;
  ObSqlString old_pred;
  ObSqlString request_id;
  // 1. Did a row key column change?
  for (int64_t r = 0; !pk_changed && r < ai_table_ctx_.rowkey_cnt_; r++) {
    const uint64_t col_id = ai_table_ctx_.col_ids_[ai_table_ctx_.rowkey_pos_[r]];
    const int64_t pos = ai_row_pos_of_col_id_(dml_ctdef, col_id);
    ObDatum *old_datum = NULL;
    ObDatum *new_datum = NULL;
    if (OB_INVALID_INDEX == pos) {
      ret = OB_ERR_UNEXPECTED;
      SQL_ENG_LOG(WARN, "ai column: rowkey column missing in update row", K(ret));
    } else if (OB_FAIL(dml_ctdef.old_row_.at(pos)->eval(eval_ctx_, old_datum))) {
    } else if (OB_FAIL(dml_ctdef.new_row_.at(pos)->eval(eval_ctx_, new_datum))) {
    } else if (OB_ISNULL(old_datum) || OB_ISNULL(new_datum)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (old_datum->is_null() != new_datum->is_null()) {
      pk_changed = true;
    } else if (!old_datum->is_null()
        && (old_datum->len_ != new_datum->len_
            || 0 != MEMCMP(old_datum->ptr_, new_datum->ptr_, old_datum->len_))) {
      pk_changed = true;
    }
  }
  // 2. Did a column that an AI expression depends on change?
  for (int64_t m = 0; OB_SUCC(ret) && !dep_changed && m < ai_metas_.count(); m++) {
    const char *source_expr = ai_metas_.at(m).source_expr_;
    for (int64_t c = 0; OB_SUCC(ret) && !dep_changed && c < ai_table_ctx_.col_cnt_; c++) {
      const char *col_name = ai_table_ctx_.col_names_[c];
      if (ai_expr_has_identifier_(source_expr, col_name)) {
        const int64_t pos = ai_row_pos_of_col_id_(dml_ctdef, ai_table_ctx_.col_ids_[c]);
        ObDatum *old_datum = NULL;
        ObDatum *new_datum = NULL;
        if (OB_INVALID_INDEX == pos) {
          ret = OB_ERR_UNEXPECTED;
          SQL_ENG_LOG(WARN, "ai column: dep column missing in update row", K(ret));
        } else if (OB_FAIL(dml_ctdef.old_row_.at(pos)->eval(eval_ctx_, old_datum))) {
        } else if (OB_FAIL(dml_ctdef.new_row_.at(pos)->eval(eval_ctx_, new_datum))) {
        } else if (OB_ISNULL(old_datum) || OB_ISNULL(new_datum)) {
          ret = OB_ERR_UNEXPECTED;
        } else if (old_datum->is_null() != new_datum->is_null()) {
          dep_changed = true;
        } else if (!old_datum->is_null()
            && (old_datum->len_ != new_datum->len_
                || 0 != MEMCMP(old_datum->ptr_, new_datum->ptr_, old_datum->len_))) {
          dep_changed = true;
        }
      }
    }
  }
  if (OB_SUCC(ret) && (pk_changed || dep_changed)) {
    if (OB_FAIL(ai_gen_rowkey_predicate_(dml_ctdef, dml_ctdef.new_row_, new_pred, is_null_pk))) {
    } else if (is_null_pk) {
      ret = OB_ERR_UNEXPECTED;
      SQL_ENG_LOG(WARN, "ai column: null new primary key on update", K(ret));
    } else if (pk_changed && OB_FAIL(ai_gen_rowkey_predicate_(dml_ctdef, dml_ctdef.old_row_,
                                                              old_pred, is_null_pk))) {
    } else if (OB_FAIL(ai_gen_request_id_(request_id))) {
    } else {
      // 3. Invalidate the AI values of the (new) row.
      ObSqlString invalidate;
      if (OB_FAIL(invalidate.assign_fmt("UPDATE `%s`.`%s` SET ",
                                        ai_table_ctx_.db_name_, ai_table_ctx_.table_name_))) {
      } else {
        for (int64_t m = 0; OB_SUCC(ret) && m < ai_metas_.count(); m++) {
          if (m > 0 && OB_FAIL(invalidate.append(", "))) {
          } else if (OB_FAIL(invalidate.append_fmt("`%s` = NULL", ai_metas_.at(m).column_name_))) {
          }
        }
        if (OB_SUCC(ret) && OB_FAIL(invalidate.append_fmt(" WHERE %s", new_pred.ptr()))) {
        } else if (OB_FAIL(execute_write(invalidate.ptr()))) {
          SQL_ENG_LOG(WARN, "ai column: invalidate on update failed", K(ret), K(invalidate));
        }
      }
      // 4. Drop the pending tasks of the old and the new row key.
      if (OB_SUCC(ret)) {
        ObSqlString cleanup;
        if (OB_FAIL(cleanup.assign_fmt("DELETE FROM oceanbase.%s WHERE table_id = %ld AND rowkey_predicate IN (",
                                       OB_ALL_AI_COLUMN_TASK_TNAME,
                                       dml_ctdef.das_base_ctdef_.index_tid_))) {
        } else if (OB_FAIL(sql_append_hex_escape_str(new_pred.string(), cleanup))) {
        } else if (pk_changed && OB_FAIL(cleanup.append(", "))) {
        } else if (pk_changed && OB_FAIL(sql_append_hex_escape_str(old_pred.string(), cleanup))) {
        } else if (OB_FAIL(cleanup.append(")"))) {
        } else if (OB_FAIL(execute_write(cleanup.ptr()))) {
          SQL_ENG_LOG(WARN, "ai column: cleanup tasks failed", K(ret), K(cleanup));
        }
      }
      // 5. Queue the regeneration tasks of the new row key.
      if (OB_SUCC(ret)) {
        ObSqlString sql;
        if (OB_FAIL(sql.assign_fmt("INSERT INTO oceanbase.%s (table_id, rowkey_predicate, column_id, "
                                   "request_id, state, retry_count, max_retries, eligible_ts, "
                                   "normalized_expr, ai_model_name) "
                                   "VALUES ",
                                   OB_ALL_AI_COLUMN_TASK_TNAME))) {
        } else {
          for (int64_t m = 0; OB_SUCC(ret) && m < ai_metas_.count(); m++) {
            const share::ObAiColumnMetaInfo &meta = ai_metas_.at(m);
            ObSqlString esc_expr;
            ObSqlString esc_model;
            if (m > 0 && OB_FAIL(sql.append(", "))) {
            } else if (OB_FAIL(sql.append_fmt("(%ld, ", dml_ctdef.das_base_ctdef_.index_tid_))) {
            } else if (OB_FAIL(sql_append_hex_escape_str(new_pred.string(), sql))) {
            } else if (OB_FAIL(sql.append_fmt(", %ld, ", meta.column_id_))) {
            } else if (OB_FAIL(ai_append_str_literal(sql, request_id.ptr()))) {
            } else if (OB_FAIL(sql.append_fmt(", %ld, 0, %ld, CURRENT_TIMESTAMP(6), ",
                                              share::OB_AI_COL_TASK_STATE_PENDING,
                                              meta.max_retries_))) {
            } else if (OB_FAIL(esc_expr.append(meta.source_expr_))) {
            } else if (OB_FAIL(ai_append_str_literal(sql, esc_expr.ptr()))) {
            } else if (OB_FAIL(sql.append(", "))) {
            } else if (OB_FAIL(esc_model.append(meta.ai_model_name_))) {
            } else if (OB_FAIL(ai_append_str_literal(sql, esc_model.ptr()))) {
            } else if (OB_FAIL(sql.append_fmt(")"))) {
            }
          }
          if (OB_SUCC(ret) && ai_metas_.count() > 0 && OB_FAIL(execute_write(sql.ptr()))) {
            SQL_ENG_LOG(WARN, "ai column: insert task rows failed", K(ret), K(sql));
          }
        }
      }
    }
  }
  return ret;
}

int ObTableModifyOp::ai_process_delete_(const ObDMLBaseCtDef &dml_ctdef)
{
  int ret = OB_SUCCESS;
  bool is_null_pk = false;
  ObSqlString pred;
  ObSqlString cleanup;
  if (OB_FAIL(ai_gen_rowkey_predicate_(dml_ctdef, dml_ctdef.old_row_, pred, is_null_pk))) {
  } else if (is_null_pk) {
    SQL_ENG_LOG(DEBUG, "ai column: null primary key on delete, skip task cleanup");
  } else if (OB_FAIL(cleanup.assign_fmt("DELETE FROM oceanbase.%s WHERE table_id = %ld AND rowkey_predicate = ",
                                        OB_ALL_AI_COLUMN_TASK_TNAME,
                                        dml_ctdef.das_base_ctdef_.index_tid_))) {
  } else if (OB_FAIL(sql_append_hex_escape_str(pred.string(), cleanup))) {
  } else if (OB_FAIL(execute_write(cleanup.ptr()))) {
    SQL_ENG_LOG(WARN, "ai column: cleanup tasks on delete failed", K(ret), K(cleanup));
  }
  return ret;
}

int ObTableModifyOp::process_ai_column_row(const ObDMLModifyRowNode &modify_row)
{
  int ret = OB_SUCCESS;
  const ObDMLBaseCtDef &dml_ctdef = *modify_row.dml_ctdef_;
  const uint64_t table_id = dml_ctdef.das_base_ctdef_.index_tid_;
  ObSQLSessionInfo *session = ctx_.get_my_session();
  if (!share::ObAiColumnRegistry::get_instance().has_ai_columns(table_id)) {
    // fast path, nothing to do
  } else if (OB_NOT_NULL(session) && (session->is_inner() || is_nested_session())) {
    // Internal statements (the worker writeback, FK cascades, our own task
    // bookkeeping) must never queue new tasks, otherwise the invalidation
    // update of one row would recurse forever.
  } else if (OB_FAIL(ctx_.check_status())) {
    SQL_ENG_LOG(WARN, "ai column: check status failed", K(ret));
  } else if (OB_FAIL(load_ai_table_ctx_(table_id))) {
    SQL_ENG_LOG(WARN, "ai column: load table ctx failed", K(ret), K(table_id));
  } else if (ai_table_ctx_.rowkey_cnt_ <= 0) {
    // Without a row key the modified row cannot be addressed, so no task can
    // be queued for it.
    SQL_ENG_LOG(INFO, "ai column: skip, no rowkey", K(table_id));
  } else if (OB_FAIL(open_inner_conn())) {
    SQL_ENG_LOG(WARN, "ai column: open inner conn failed", K(ret));
  } else if (OB_FAIL(begin_nested_session(false))) {
    SQL_ENG_LOG(WARN, "ai column: begin nested session failed", K(ret));
  } else {
    // skip_cur_stmt_tables = true: the internal statements re-resolve the very
    // table that is being modified by the current statement.
    if (OB_FAIL(load_ai_column_metas_(table_id))) {
      SQL_ENG_LOG(WARN, "ai column: load metas failed", K(ret), K(table_id));
    } else if (!ai_metas_filtered_) {
      // Drop metas that do not map to a column of the modified row. The ALTER
      // TABLE ADD COLUMN path stores a placeholder column id, such a meta can
      // never be regenerated and must not break the user statement.
      int64_t write_pos = 0;
      for (int64_t r = 0; r < ai_metas_.count(); r++) {
        if (OB_INVALID_INDEX != ai_row_pos_of_col_id_(dml_ctdef, ai_metas_.at(r).column_id_)) {
          if (write_pos != r) {
            ai_metas_.at(write_pos) = ai_metas_.at(r);
          }
          write_pos++;
        }
      }
      while (ai_metas_.count() > write_pos) {
        ai_metas_.pop_back();
      }
      ai_metas_filtered_ = true;
    }
    if (OB_SUCC(ret) && !ai_metas_.empty()) {
      const ObDmlEventType dml_event = modify_row.dml_event_;
      if (ObDmlEventType::DE_INSERTING == dml_event) {
        ret = ai_process_insert_(dml_ctdef);
      } else if (ObDmlEventType::DE_UPDATING == dml_event) {
        ret = ai_process_update_(dml_ctdef);
      } else if (ObDmlEventType::DE_DELETING == dml_event) {
        ret = ai_process_delete_(dml_ctdef);
      } else {
        ret = OB_ERR_UNEXPECTED;
      }
    }
    int end_ret = end_nested_session();
    if (OB_SUCCESS != end_ret) {
      SQL_ENG_LOG(WARN, "ai column: end nested session failed", K(end_ret));
      if (OB_SUCCESS == ret) { ret = end_ret; }
    }
  }
  return ret;
}

}  // namespace sql
}  // namespace oceanbase
