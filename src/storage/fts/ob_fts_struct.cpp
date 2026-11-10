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

#include "data_plane/fts/ob_fts_struct.h"

#include "lib/charset/ob_charset.h"
#include "share/datum/ob_datum_funcs.h"
#include "storage/ob_storage_util.h"

namespace oceanbase
{
namespace storage
{
namespace
{
// True when the word contains only ASCII bytes.  Empty words are treated as
// ASCII because the fast paths below handle zero-length explicitly.
OB_INLINE bool is_ascii_word(const common::ObString &word)
{
  bool is_ascii = true;
  const int64_t len = word.length();
  if (len > 0) {
    const char *ptr = word.ptr();
    if (OB_ISNULL(ptr)) {
      is_ascii = false;
    } else {
      for (int64_t i = 0; i < len; ++i) {
        if (OB_UNLIKELY(static_cast<unsigned char>(ptr[i]) >= 0x80)) {
          is_ascii = false;
          break;
        }
      }
    }
  }
  return is_ascii;
}

// FNV-1a 64-bit: same cheap byte hash already used in the DAT builder.  For
// ASCII tokens whose collation is case-insensitive (general_ci) the casedown
// step has already produced a canonical lower-case byte sequence, so a byte
// hash is distribution-compatible with the generic collation-aware hash while
// avoiding the charset/sort-weight overhead.
OB_INLINE uint64_t ascii_byte_hash(const common::ObString &word)
{
  uint64_t hash = 1469598103934665603ULL; // FNV-1a offset basis
  const unsigned char *ptr = reinterpret_cast<const unsigned char *>(word.ptr());
  for (int64_t i = 0; i < word.length(); ++i) {
    hash ^= static_cast<uint64_t>(ptr[i]);
    hash *= 1099511628211ULL; // FNV-1a prime
  }
  return hash;
}

// Collations for which an ASCII word can be hashed/compared byte-wise after
// the FTS casedown step has canonicalised the token.
OB_INLINE bool coll_type_allows_ascii_fast_path(const common::ObCollationType coll_type)
{
  return common::CS_TYPE_UTF8MB4_GENERAL_CI == coll_type
         || common::CS_TYPE_UTF8MB4_BIN == coll_type
         || common::CS_TYPE_BINARY == coll_type;
}
} // namespace

int ObFTWord::hash(uint64_t &hash_val) const
{
  int ret = OB_SUCCESS;
  const common::ObCollationType coll_type = meta_.get_collation_type();
  if (OB_LIKELY(common::ObVarcharType == meta_.get_type())
      && OB_LIKELY(coll_type_allows_ascii_fast_path(coll_type))
      && OB_LIKELY(is_ascii_word(word_.get_string()))) {
    hash_val = ascii_byte_hash(word_.get_string());
  } else {
    common::ObDatumBasicFuncs *funcs = ObDatumFuncs::get_basic_func(meta_.get_type(), coll_type);
    if (OB_ISNULL(funcs)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (funcs->default_hash_ == nullptr) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      ret = funcs->default_hash_(word_, 0, hash_val, nullptr);
    }
  }
  return ret;
}
bool ObFTWord::operator==(const ObFTWord &other) const
{
  bool is_equal = false;
  int ret = OB_SUCCESS;
  int cmp_ret = 0;
  // Token comparison is the inner-most operation of the per-document word map
  // and of the stop word check.  For binary collations equal length words are
  // simply byte equal, so the per-call comparator lookup (get_datum_cmp_func)
  // can be skipped.  For general_ci the casedown step has already produced a
  // canonical lower-case form, so ASCII tokens can also be compared byte-wise.
  // Different lengths still take the generic path because padding semantics are
  // up to the collation.
  const ObCollationType coll_type = meta_.get_collation_type();
  if (OB_LIKELY(word_.len_ == other.word_.len_)
      && meta_.get_type() == other.meta_.get_type()
      && coll_type == other.meta_.get_collation_type()
      && OB_LIKELY(coll_type_allows_ascii_fast_path(coll_type))
      && (common::CS_TYPE_UTF8MB4_GENERAL_CI != coll_type
          || (is_ascii_word(word_.get_string())
              && is_ascii_word(other.word_.get_string())))) {
    is_equal = (0 == word_.len_)
               || (nullptr != word_.ptr_ && nullptr != other.word_.ptr_
                   && 0 == MEMCMP(word_.ptr_, other.word_.ptr_, word_.len_));
    return is_equal;
  }
  ObDatumCmpFuncType func = get_datum_cmp_func(meta_, other.meta_);
  if (func == nullptr) {
    ob_abort();
  } else if (OB_FAIL(func(word_, other.word_, cmp_ret, nullptr))) {
    ob_abort();
  } else {
    is_equal = (cmp_ret == 0);
  }
  return is_equal;
}
} // namespace storage
} // namespace oceanbase
