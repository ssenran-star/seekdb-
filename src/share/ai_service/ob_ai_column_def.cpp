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

namespace oceanbase {
namespace share {

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
} // namespace oceanbase
