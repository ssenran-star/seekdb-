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

#ifndef OB_AI_COLUMN_WORKER_H
#define OB_AI_COLUMN_WORKER_H

#include "lib/task/ob_timer.h"
#include "lib/string/ob_sql_string.h"
#include "lib/allocator/page_arena.h"
#include "common/json_type/ob_json_tree.h"
#include "share/ai_service/ob_ai_column_def.h"

namespace oceanbase {
namespace observer {

// Background worker that consumes the async regeneration tasks queued by the
// DML hook of the AI columns. One tick:
//   1. read the eligible rows of oceanbase.__all_ai_column_task
//   2. group them by (table_id, rowkey_predicate, request_id): all AI columns
//      of one row event share one request id and are published together
//   3. evaluate the AI expression of every column against the current row and
//      call the model with a typed system prompt derived from the column type
//   4. publish the whole group with a single UPDATE and drop the task rows in
//      one transaction; a group whose task rows disappeared meanwhile is
//      dropped so a late result can never overwrite a newer one
//   5. a failed group is retried until max_retries is exhausted, after which
//      the task rows are removed and the column stays NULL
class ObAiColumnWorker : public common::ObTimerTask
{
public:
  ObAiColumnWorker();
  virtual ~ObAiColumnWorker();
  // Schedules the worker on an already initialized timer. The owner of the
  // timer is responsible for stopping it.
  int start(common::ObTimer &timer);
  void runTimerTask() override;

private:
  struct AiTaskItem
  {
    AiTaskItem() { reset(); }
    void reset()
    {
      table_id_ = 0;
      column_id_ = 0;
      state_ = 0;
      retry_count_ = 0;
      max_retries_ = 0;
      rowkey_predicate_[0] = '\0';
      request_id_[0] = '\0';
      ai_model_name_[0] = '\0';
    }
    uint64_t table_id_;
    int64_t column_id_;
    int64_t state_;
    int64_t retry_count_;
    int64_t max_retries_;
    char rowkey_predicate_[1024];
    char request_id_[128];
    char ai_model_name_[128];
  };

  struct AiGroup
  {
    AiGroup() { reset(); }
    void reset()
    {
      table_id_ = 0;
      rowkey_predicate_[0] = '\0';
      request_id_[0] = '\0';
      tasks_.reset();
    }
    uint64_t table_id_;
    char rowkey_predicate_[1024];
    char request_id_[128];
    common::ObSEArray<AiTaskItem, 4> tasks_;
  };

  struct AiValueItem
  {
    AiValueItem() { reset(); }
    void reset()
    {
      column_id_ = 0;
      is_number_ = false;
      number_value_ = 0;
      value_[0] = '\0';
    }
    int64_t column_id_;
    bool is_number_;
    int64_t number_value_;
    char value_[8192];
  };

  int process_tick_();
  int read_candidate_tasks_(common::ObIArray<AiTaskItem> &tasks);
  int process_group_(const AiGroup &group);
  int load_group_metas_(uint64_t table_id,
                        const common::ObIArray<int64_t> &column_ids,
                        common::ObIArray<share::ObAiColumnMetaInfo> &metas);
  int load_qualified_name_(uint64_t table_id, ObSqlString &qualified);
  int eval_prompt_(const ObSqlString &qualified,
                   const share::ObAiColumnMetaInfo &meta,
                   const char *rowkey_predicate,
                   ObSqlString &prompt);
  int call_model_(const share::ObAiColumnMetaInfo &meta,
                  const ObString &prompt,
                  AiValueItem &value);
  int publish_group_(const AiGroup &group,
                     const common::ObIArray<share::ObAiColumnMetaInfo> &metas,
                     const common::ObIArray<AiValueItem> &values);
  int fail_group_(const AiGroup &group);
  int build_typed_config_(common::ObIAllocator &allocator,
                          int64_t dimension,
                          common::ObJsonObject *&config);

  bool scheduled_;
  bool running_;
};

} // namespace observer
} // namespace oceanbase

#endif // OB_AI_COLUMN_WORKER_H
