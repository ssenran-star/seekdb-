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

#ifndef _OCEANBASE_STORAGE_FTS_DICT_OB_FT_DAT_DICT_H_
#define _OCEANBASE_STORAGE_FTS_DICT_OB_FT_DAT_DICT_H_

#include "lib/allocator/ob_allocator.h"
#include "lib/ob_errno.h"
#include "lib/utility/ob_macro_utils.h"
#include "ob_ft_dict_def.h"
#include "storage/fts/dict/ob_ft_dict.h"

#include <cstddef>

/* File Description:
 * @brief tools to build a double array trie dictionary.
 */

namespace oceanbase
{
namespace storage
{
/* @class ObArrayHashMap
 * @description: a fixed size hash map, used to store words and their code.
 */
class ObArrayHashMap final
{
public:
  struct Entry
  {
    ObFTSingleWord word;
    ObFTWordCode code;
    bool used = false;
  } __attribute__((packed));

  struct Header
  {
    uint32_t buffer_size_;
    uint32_t capacity_;
    uint32_t count_;
    Entry data[1];
  } __attribute__((packed));

  static size_t estimate_capacity(size_t word_num);

  static size_t estimate_size(size_t word_num);

  size_t capacity() const { return header_.capacity_; }

  int init(size_t word_num);

  // no duplicate should be inserted
  int insert(const ObString &key, ObFTWordCode code);

  int find(const ObFTSingleWord &word, ObFTWordCode &code) const;

private:
  struct Header header_;
} __attribute__((packed));

/**
 * @class ObFTDAT
 * @brief ObFTDATrie data structure, which is a memory block, which contains
 *          the code map(optional), the base and check array, and the data block(optional).
 */
struct ObFTDAT
{
public:
  static constexpr int32_t FIRST_BASE = 1;
  static constexpr int32_t FIRST_INDEX = 1;
  // all size in bytes
  int64_t mem_block_size_ = 0;
  // node_num_: number of nodes in the trie.
  size_t node_num_ = 0;
  // word_num_: number of words in the trie.
  size_t word_num_ = 0;
  // array_size_ for the base and check array.
  size_t array_size_ = 0;

  // start offset of every data block
  size_t map_offset_ = 0;
  size_t base_offset_ = 0;
  size_t check_offset_ = 0;

  // use if offset is not 0, it means the data is in the data block
  size_t data_index_offset_ = 0;
  // use if offset is not 0
  size_t data_offset_ = 0;
  // a buffer to store dat
  ObFTSingleWord start_word_;
  ObFTSingleWord end_word_;
  char buff[1] = {0};

public:
  ObArrayHashMap *get_map();
} __attribute__((packed));

template <typename DataType>
class ObFTTrie;

/**
 * @class OBFTDATBuilder
 * @brief oceanbase double array trie builder, build a trie and then a dat.
 */
template <typename DATA_TYPE = void>
class ObFTDATBuilder
{
public:
  ObFTDATBuilder(ObIAllocator &alloc)
      : alloc_(alloc), dat_(nullptr), map_(nullptr), is_inited_(false), next_code_(1)
  {
  }

  ~ObFTDATBuilder() {}

  int init(ObFTTrie<DATA_TYPE> &trie);

  int build_from_trie(ObFTTrie<DATA_TYPE> &trie);

  // get and put to kv_cache
  int get_mem_block(ObFTDAT *&mem, size_t &mem_len)
  {
    mem = dat_;
    if (nullptr == dat_) {
      mem_len = 0;
    } else {
      mem_len = dat_->mem_block_size_;
    }
    return OB_SUCCESS;
  }

private:
  int expand();

  int encode(const ObString &word, ObFTWordCode &code, bool add);

private:
  common::ObIAllocator &alloc_;
  ObFTDAT *dat_;
  ObArrayHashMap *map_;

  bool is_inited_;
  int32_t next_code_;

private:
  DISALLOW_COPY_AND_ASSIGN(ObFTDATBuilder);
};

/**
 * @class ObFTDATReader
 * @brief Reader to do match on a mem block dat.
 */
template <typename DataType = void>
class ObFTDATReader
{
public:
  // The trie alphabet is a small, immutable set of characters, so the
  // character -> code lookup in the word map is a pure function of the
  // character bytes for the whole life of this reader. Remember the result in
  // a tiny direct mapped table to keep the (hash + modulo + probe) sequence off
  // the per-character hot path of the tokenizers.
  static constexpr int64_t CHAR_CODE_CACHE_SLOTS = 512;

  ObFTDATReader(ObFTDAT *dat) : dat_(dat), char_code_cache_() {}

  // Match single word with hit.
  int match_with_hit(const ObString &single_word,
                     const ObDATrieHit &last_hit,
                     ObDATrieHit &hit) const;

private:
  // Pack a 1..3 byte character into a non zero key. Longer characters are not
  // cached, they are rare and would need more bits than the key has.
  static inline bool build_char_code_key(const ObString &ch, uint32_t &key)
  {
    bool ok = false;
    const unsigned char *p = reinterpret_cast<const unsigned char *>(ch.ptr());
    const int32_t len = ch.length();
    if (1 == len) {
      key = (1UL << 24) | static_cast<uint32_t>(p[0]);
      ok = true;
    } else if (2 == len) {
      key = (2UL << 24) | (static_cast<uint32_t>(p[0]) << 8) | static_cast<uint32_t>(p[1]);
      ok = true;
    } else if (3 == len) {
      key = (3UL << 24) | (static_cast<uint32_t>(p[0]) << 16)
            | (static_cast<uint32_t>(p[1]) << 8) | static_cast<uint32_t>(p[2]);
      ok = true;
    }
    return ok;
  }

  static inline uint64_t char_code_slot(const uint32_t key)
  {
    return ((static_cast<uint64_t>(key) * 2654435761UL) >> 20) & (CHAR_CODE_CACHE_SLOTS - 1);
  }

  // One entry is a single 64 bit word so that a reader can never observe a key
  // from one character and a code from another one: high half is the key, low
  // half is code + 1 (0 means "this character is not in the trie alphabet").
  static inline uint64_t pack_char_code(const uint32_t key, const uint32_t code_plus_one)
  {
    return (static_cast<uint64_t>(key) << 32) | static_cast<uint64_t>(code_plus_one);
  }

  inline bool probe_char_code(const uint32_t key, uint32_t &code_plus_one) const
  {
    const uint64_t packed = __atomic_load_n(&char_code_cache_[char_code_slot(key)], __ATOMIC_RELAXED);
    const bool hit = (static_cast<uint32_t>(packed >> 32) == key);
    if (hit) {
      code_plus_one = static_cast<uint32_t>(packed);
    }
    return hit;
  }

  inline void remember_char_code(const uint32_t key, const uint32_t code_plus_one) const
  {
    __atomic_store_n(&char_code_cache_[char_code_slot(key)],
                     pack_char_code(key, code_plus_one), __ATOMIC_RELAXED);
  }

private:
  ObFTDAT *dat_;
  mutable uint64_t char_code_cache_[CHAR_CODE_CACHE_SLOTS];

private:
  DISALLOW_COPY_AND_ASSIGN(ObFTDATReader);
};

} //  namespace storage
} //  namespace oceanbase

#endif // _OCEANBASE_STORAGE_FTS_DICT_OB_FT_DAT_DICT_H_
