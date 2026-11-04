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

int ObFTWord::hash(uint64_t &hash_val) const
{
  int ret = OB_SUCCESS;
  if (OB_LIKELY(meta_.is_string_type())) {
    // 分词产出的词都是字符串类型，直接对字节做哈希即可。
    // 通用路径要先经 ObDatumFuncs 按 (type, collation) 二次分派再走函数指针，
    // 这条路径在分词热路径上每个词都会走一次，省下的分派开销相当可观。
    // hash 只要求“相等则同值”，字符串与非字符串类型之间不会相等，混用无风险。
    const common::ObString &str = word_.get_string();
    hash_val = common::murmurhash(str.ptr(), str.length(), 0);
  } else {
    common::ObDatumBasicFuncs *funcs = ObDatumFuncs::get_basic_func(meta_.get_type(), meta_.get_collation_type());
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
  const ObCollationType l_cs = meta_.get_collation_type();
  const ObCollationType r_cs = other.meta_.get_collation_type();
  const bool l_bin = (CS_TYPE_UTF8MB4_BIN == l_cs || CS_TYPE_BINARY == l_cs);
  if (OB_LIKELY(l_bin && l_cs == r_cs && meta_.is_string_type() && other.meta_.is_string_type())) {
    // 二进制序的字符串比较等价于字节比较，不必再走 collation 比较函数
    const common::ObString &l_str = word_.get_string();
    const common::ObString &r_str = other.word_.get_string();
    is_equal = (l_str.length() == r_str.length())
               && (0 == l_str.length() || 0 == MEMCMP(l_str.ptr(), r_str.ptr(), l_str.length()));
  } else {
    ObDatumCmpFuncType func = get_datum_cmp_func(meta_, other.meta_);
    if (func == nullptr) {
      ob_abort();
    } else if (OB_FAIL(func(word_, other.word_, cmp_ret, nullptr))) {
      ob_abort();
    } else {
      is_equal = (cmp_ret == 0);
    }
  }
  return is_equal;
}
} // namespace storage
} // namespace oceanbase
