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

#ifndef OB_AI_COLUMN_DEF_H
#define OB_AI_COLUMN_DEF_H

#include <stdint.h>
#include "lib/lock/ob_spin_rwlock.h"

namespace oceanbase {
namespace share {

// Task states stored in oceanbase.__all_ai_column_task.state.
static const int64_t OB_AI_COL_TASK_STATE_PENDING = 1;  // eligible for the worker
static const int64_t OB_AI_COL_TASK_STATE_RUNNING = 2;  // claimed by the worker
static const int64_t OB_AI_COL_TASK_STATE_RETRY   = 3;  // failed, waiting for eligible_ts

// Result kinds stored in oceanbase.__all_ai_column.dimension.
static const int64_t OB_AI_COL_DIM_JSON   = 1;  // json column, typed prompt
static const int64_t OB_AI_COL_DIM_INT    = 2;  // integer column, typed prompt
static const int64_t OB_AI_COL_DIM_VECTOR = 3;  // vector column, embeddings

// Per row status of one AI column, mirrored into the hidden __ai_state_<col>
// column so that ALTER TABLE ... RETRY AI COLUMN ... WHERE status = '...'
// can be evaluated as an ordinary predicate.
static const int64_t OB_AI_COL_ROW_STATUS_NONE    = 0;
static const int64_t OB_AI_COL_ROW_STATUS_PENDING = 1;
static const int64_t OB_AI_COL_ROW_STATUS_READY   = 2;
static const int64_t OB_AI_COL_ROW_STATUS_FAILED  = 3;

// One row of oceanbase.__all_ai_column, as needed by the DML hook and by the
// background worker.
struct ObAiColumnMetaInfo
{
  ObAiColumnMetaInfo() { reset(); }
  void reset()
  {
    column_id_ = 0;
    dimension_ = 0;
    max_retries_ = 2;
    column_name_[0] = '\0';
    source_expr_[0] = '\0';
    ai_model_name_[0] = '\0';
  }
  char column_name_[128];
  char source_expr_[2048];
  char ai_model_name_[128];
  uint64_t column_id_;
  int64_t dimension_;
  int64_t max_retries_;
};

// Process wide registry of the tables that own at least one AI column. It is
// maintained by the DDL layer so that the DML fast path can decide with one
// linear scan whether after-row AI processing is needed at all.
//
// The implementation lives in ob_ai_column_def.cpp on purpose: the registry
// must be a single object shared by every translation unit, it cannot be
// header only because the build compiles sources in unity (jumbo) units and
// a header only singleton would be duplicated per unity unit.
class ObAiColumnRegistry
{
public:
  static ObAiColumnRegistry &get_instance();
  int register_table(uint64_t table_id);
  int unregister_table(uint64_t table_id);
  bool has_ai_columns(uint64_t table_id) const;
  int64_t table_count() const;

private:
  ObAiColumnRegistry();
  ObAiColumnRegistry(const ObAiColumnRegistry &);
  ObAiColumnRegistry &operator=(const ObAiColumnRegistry &);

  static const int64_t MAX_AI_COLUMN_TABLES = 512;
  uint64_t table_ids_[MAX_AI_COLUMN_TABLES];
  int64_t count_;
  mutable common::SpinRWLock lock_;
};

} // namespace share
} // namespace oceanbase

#endif // OB_AI_COLUMN_DEF_H
