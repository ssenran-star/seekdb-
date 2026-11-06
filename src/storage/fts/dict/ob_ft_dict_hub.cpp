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

#define USING_LOG_PREFIX STORAGE_FTS

#include "storage/fts/dict/ob_ft_dict_hub.h"

#include "lib/ob_errno.h"
#include "lib/oblog/ob_log_module.h"
#include "lib/utility/ob_macro_utils.h"
#include "storage/fts/dict/ob_ft_cache_container.h"
#include "storage/fts/dict/ob_ft_dict_def.h"
#include "storage/fts/dict/ob_ft_range_dict.h"
namespace oceanbase
{
namespace storage
{

/**
 * A dictionary that is shared by all the parsers.
 *
 * The container keeps the handle of the cache block alive, the dictionary
 * points at the container.
 */
struct ObFTSharedDict
{
  ObFTCacheRangeContainer container;
  ObIFTDict *dict;
  ObFTSharedDict(ObIAllocator &alloc) : container(alloc), dict(nullptr) {}
};
int ObFTDictHub::init()
{
  static constexpr int K_MAX_DICT_BUCKET = 128; // for now, only built-in dicts.
  int ret = OB_SUCCESS;
  if (OB_FAIL(dict_map_.create(K_MAX_DICT_BUCKET, "dict_map"))) {
  } else if (OB_FAIL(shared_dict_map_.create(K_MAX_DICT_BUCKET, "shared_dict_map"))) {
  } else if (OB_FAIL(rw_dict_lock_.init(K_MAX_DICT_BUCKET))) {
  } else {
    is_inited_ = true;
  }
  return ret;
};

int ObFTDictHub::destroy()
{
  int ret = OB_SUCCESS;
  is_inited_ = false;
  return ret;
}
int ObFTDictHub::build_cache(const ObFTDictDesc &desc, ObFTCacheRangeContainer &container)
{
  int ret = OB_SUCCESS;
  ObFTDictInfoKey key(static_cast<uint64_t>(desc.type_));
  ObFTDictInfo info;
  container.reset();

  if (!is_inited_) {
    ret = OB_NOT_INIT;
  } else {
    ObBucketHashWLockGuard guard(rw_dict_lock_, key.hash());

    // try if valid with no recursive lock
    if (OB_FAIL(get_dict_info(key, info))) {
      if (OB_HASH_NOT_EXIST == ret) {
        // dict not exist, make new one, by caller
        ret = OB_ENTRY_NOT_EXIST;
      } else {
      }
    } else if (OB_FAIL(ObFTRangeDict::try_load_cache(desc, info.range_count_, container))) {
      if (OB_ENTRY_NOT_EXIST == ret) {
      } else {
      }
    }

    if (OB_FAIL(ret)) {
      if (OB_ENTRY_NOT_EXIST == ret) {
        if (OB_FAIL(ObFTRangeDict::build_cache_from_ik_dict(desc, container))) {
        } else if (FALSE_IT(info.range_count_ = container.get_handles().size())) {
        } else if (OB_FAIL(put_dict_info(key, info))) {
        }
      }
    }
  }
  return ret;
}

int ObFTDictHub::load_cache(const ObFTDictDesc &desc, ObFTCacheRangeContainer &container)
{
  int ret = OB_SUCCESS;
  ObFTDictInfo info;
  container.reset();
  ObFTDictInfoKey key(static_cast<uint64_t>(desc.type_));
  if (!is_inited_) {
    ret = OB_NOT_INIT;
  } else {
    {
      ObBucketHashRLockGuard guard(rw_dict_lock_, key.hash());
      if (OB_FAIL(get_dict_info(key, info))) {
        if (OB_HASH_NOT_EXIST == ret) {
          // dict not exist, make new one, by caller
          ret = OB_ENTRY_NOT_EXIST;
        } else {
        }
      }
    }
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(ObFTRangeDict::try_load_cache(desc, info.range_count_, container))) {
      if (OB_ENTRY_NOT_EXIST == ret) {
        // dict not exist, make new one, by caller
      } else {
      }
    }
  }

  return ret;
}


int ObFTDictHub::get_shared_dict(const ObFTDictDesc &desc, ObIFTDict *&dict)
{
  int ret = OB_SUCCESS;
  dict = nullptr;
  if (!is_inited_) {
    ret = OB_NOT_INIT;
  } else {
    ObFTDictInfoKey key(static_cast<uint64_t>(desc.type_));
    // The fast path: the dictionary is already there.
    {
      ObBucketHashRLockGuard guard(rw_dict_lock_, key.hash());
      ObFTSharedDict *entry = nullptr;
      if (OB_FAIL(ret = shared_dict_map_.get_refactored(key, entry))) {
        if (OB_HASH_NOT_EXIST == ret) {
          ret = OB_SUCCESS;
        }
      } else if (OB_NOT_NULL(entry)) {
        dict = entry->dict;
      }
    }
    if (OB_SUCC(ret) && OB_ISNULL(dict)) {
      // Only one thread builds the dictionary, the others wait for it.
      lib::ObMutexGuard build_guard(shared_dict_build_lock_);
      ObFTSharedDict *entry = nullptr;
      {
        // Another thread may have built it in the meantime.
        ObBucketHashRLockGuard guard(rw_dict_lock_, key.hash());
        if (OB_FAIL(ret = shared_dict_map_.get_refactored(key, entry))) {
          if (OB_HASH_NOT_EXIST == ret) {
            ret = OB_SUCCESS;
          }
        } else if (OB_NOT_NULL(entry)) {
          dict = entry->dict;
        }
      }
      if (OB_SUCC(ret) && OB_ISNULL(dict)) {
        // The dictionary is built outside of the bucket lock, load_cache()
        // and build_cache() take the lock themselves.
        entry = OB_NEWx(ObFTSharedDict, &shared_dict_allocator_, shared_dict_allocator_);
        if (OB_ISNULL(entry)) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
        } else if (OB_FAIL(ret = load_cache(desc, entry->container))) {
          if (OB_ENTRY_NOT_EXIST == ret) {
            ret = OB_SUCCESS;
            if (OB_FAIL(ret = build_cache(desc, entry->container))) {
            }
          }
        }
        if (OB_SUCC(ret)) {
          ObFTRangeDict *range_dict = OB_NEWx(ObFTRangeDict,
                                              &shared_dict_allocator_,
                                              shared_dict_allocator_,
                                              &entry->container,
                                              desc);
          if (OB_ISNULL(range_dict)) {
            ret = OB_ALLOCATE_MEMORY_FAILED;
          } else if (OB_FAIL(ret = range_dict->init())) {
            // the arena keeps the memory, the dictionary is simply not used
          } else {
            entry->dict = range_dict;
          }
        }
        if (OB_SUCC(ret)) {
          ObBucketHashWLockGuard guard(rw_dict_lock_, key.hash());
          ObFTSharedDict *published = nullptr;
          if (OB_FAIL(ret = shared_dict_map_.get_refactored(key, published))) {
            if (OB_HASH_NOT_EXIST == ret) {
              ret = OB_SUCCESS;
            }
          }
          if (OB_SUCC(ret)) {
            if (OB_NOT_NULL(published)) {
              // another thread was faster, use its dictionary
              dict = published->dict;
            } else if (OB_FAIL(ret = shared_dict_map_.set_refactored(key, entry, 1))) {
            } else {
              dict = entry->dict;
            }
          }
        }
      }
    }
  }
  return ret;
}

int ObFTDictHub::get_dict_info(const ObFTDictInfoKey &key, ObFTDictInfo &info)
{
  int ret = OB_SUCCESS;

  if (OB_FAIL(dict_map_.get_refactored(key, info))) {
    if (OB_HASH_NOT_EXIST != ret) {
    }
  }

  return ret;
}

int ObFTDictHub::put_dict_info(const ObFTDictInfoKey &key, const ObFTDictInfo &info)
{
  int ret = OB_SUCCESS;
  const int cover_exist_flag = 1;
  if (OB_FAIL(dict_map_.set_refactored(key, info, cover_exist_flag))) {
  }

  return ret;
}
} //  namespace storage
} //  namespace oceanbase
