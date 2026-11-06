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

#ifndef _OCEANBASE_STORAGE_FTS_DICT_OB_FT_RANGE_DICT_H_
#define _OCEANBASE_STORAGE_FTS_DICT_OB_FT_RANGE_DICT_H_

#include "lib/alloc/alloc_struct.h"
#include "lib/allocator/ob_allocator.h"
#include "lib/allocator/page_arena.h"
#include "lib/charset/ob_charset.h"
#include "lib/container/ob_vector.h"
#include "lib/utility/ob_macro_utils.h"
#include "share/rc/ob_server_runtime.h"
#include "storage/fts/dict/ob_ft_cache_container.h"
#include "storage/fts/dict/ob_ft_dict.h"
#include <atomic>

#include "storage/fts/dict/ob_ft_dict_def.h"
#include "storage/fts/dict/ob_ft_dict_iterator.h"

namespace oceanbase
{
namespace storage
{
class ObFTRangeDict final : public ObIFTDict
{
public:
  ObFTRangeDict(ObIAllocator &alloc,
                ObFTCacheRangeContainer *range_container,
                const ObFTDictDesc &desc)
      : is_inited_(false), desc_(desc), range_alloc_(lib::ObMemAttr("Range Dict")),
        range_dicts_(&range_alloc_), range_container_(range_container),
        last_range_idx_(-1)
  {
  }
  ~ObFTRangeDict() override{};

public:
  struct ObFTRange final
  {
    ObFTSingleWord start_;
    ObFTSingleWord end_;
    ObIFTDict *dict_; // a cache dict
  };

public:
  int init() override;
  int match(const ObString &single_word, ObDATrieHit &hit) const override;
  int match(const ObString &words, bool &is_match) const override;
  int match_with_hit(const ObString &single_word,
                     const ObDATrieHit &last_hit,
                     ObDATrieHit &hit) const override;

public:
  int build_dict_from_cache(const ObFTCacheRangeContainer &range_container);

  static int try_load_cache(const ObFTDictDesc &desc,
                            const uint32_t range_count,
                            ObFTCacheRangeContainer &range_container);
  static int build_cache(const ObFTDictDesc &desc, ObFTCacheRangeContainer &range_container);

  static int build_cache_from_ik_dict(const ObFTDictDesc &desc, ObFTCacheRangeContainer &range_container);

private:
  // build cache
  static int build_ranges(const ObFTDictDesc &desc,
                          ObIFTDictIterator &iter,
                          ObFTCacheRangeContainer &range_container);

  // build one range's cache
  static int build_one_range(const ObFTDictDesc &desc,
                             const int32_t range_id,
                             ObIFTDictIterator &iter,
                             ObFTCacheRangeContainer &container,
                             bool &build_next_range);

  static int build_ranges_concurrently_thread_pool(const ObFTDictDesc &desc,
                                                   ObIFTDictIterator &iter,
                                                   ObFTCacheRangeContainer &range_container);

private:
  void destroy()
  {
    for (int64_t i = 0; i < range_dicts_.size(); i++) {
      range_dicts_[i].dict_->~ObIFTDict(); // destroy dict
    }
    range_dicts_.reset();
    range_alloc_.reset();
  }

  int find_first_char_range(const ObString &single_word, ObIFTDict *&dict) const;

  // 判断 single_word 是否落在某个 range 的 [start_, end_] 区间内。
  // UTF8MB4_BIN 的排序语义就是码点序，而合法的 UTF-8 编码保持码点序，
  // 因此可以直接用无符号字节比较，省掉 strnncoll 的逐字符解码开销。
  static inline bool range_contains(const ObFTRange &range,
                                    const ObString &single_word,
                                    const bool bin_cmp)
  {
    bool contained = false;
    if (bin_cmp) {
      contained = (ft_bin_strcmp(range.start_.get_word(), single_word) <= 0)
                  && (ft_bin_strcmp(range.end_.get_word(), single_word) >= 0);
    } else {
      contained = (ObCharset::strcmp(ObCollationType::CS_TYPE_UTF8MB4_BIN,
                                     range.start_.get_word(),
                                     single_word) <= 0)
                  && (ObCharset::strcmp(ObCollationType::CS_TYPE_UTF8MB4_BIN,
                                        range.end_.get_word(),
                                        single_word) >= 0);
    }
    return contained;
  }

  static inline int ft_bin_strcmp(const ObString &l, const ObString &r)
  {
    const int64_t min_len = MIN(l.length(), r.length());
    int result = (min_len > 0) ? memcmp(l.ptr(), r.ptr(), static_cast<size_t>(min_len)) : 0;
    if (0 == result) {
      result = (l.length() < r.length()) ? -1 : ((l.length() > r.length()) ? 1 : 0);
    }
    return result;
  }

private:
  static constexpr int DEFAULT_KEY_PER_RANGE = 50000; // by estimated
  bool is_inited_;
  ObFTDictDesc desc_;
  ObArenaAllocator range_alloc_;
  ObVector<ObFTRange, ObArenaAllocator> range_dicts_;
  ObFTCacheRangeContainer *range_container_; // only used to read cache
  // 分词是顺序扫描文本，连续字符通常落在同一个 range 上，
  // 记住上次命中的 range 可以省掉大部分线性比较。
  // 该字段只是查找提示，即使并发写坏也只会退化成一次全扫描，不影响正确性。
  // The dictionary is shared by all the parsers, so the hint that remembers
  // the range that was matched last has to be accessed atomically.  A wrong
  // value only costs a binary search, it cannot change the result.
  mutable std::atomic<int64_t> last_range_idx_;

private:
  DISALLOW_COPY_AND_ASSIGN(ObFTRangeDict);
};

} //  namespace storage
} //  namespace oceanbase

#endif // _OCEANBASE_STORAGE_FTS_DICT_OB_FT_RANGE_DICT_H_
