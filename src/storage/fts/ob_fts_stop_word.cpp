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

#include "object/ob_object.h"
#define USING_LOG_PREFIX STORAGE_FTS

#include "share/rc/ob_server_runtime.h"
#include "storage/fts/ob_fts_stop_word.h"
#include "storage/fts/ob_fts_parser_helper.h"
#include "storage/fts/ob_fts_parser_property.h"

namespace oceanbase
{
namespace storage
{

namespace
{
// True when an utf8mb4 token is well-formed UTF-8 up to and including a 3-byte
// sequence whose lead byte is 0xE4..0xE9, i.e. a code point in U+4000..U+9FFF.
// Under utf8mb4_general_ci those code points keep a sort weight >= 0x4000 (the
// ob_unicase_default pages 0x40..0x9F are NULL, so ob_tosort_unicode leaves them
// unchanged), while every built-in stop word is ASCII with weight <= 0x7A.  Such
// a token can therefore never compare equal to a stop word and the collation
// hash probe is provably always a miss.
//
// The scan validates each preceding sequence exactly like ob_mb_wc_utf8mb4 and
// bails out on the first malformed/truncated byte: ob_hash_sort_utf8mb4 stops
// decoding at such a byte, so a CJK code point after it would never reach the
// hash and the prefix could still be an ASCII stop word.  Returning false there
// keeps the probe on the safe side.  For a token that starts with CJK (the
// common IK case) this returns on the first character.
OB_INLINE bool contains_utf8mb4_cjk(const ObString &word)
{
  const unsigned char *ptr = reinterpret_cast<const unsigned char *>(word.ptr());
  const int64_t len = word.length();
  int64_t i = 0;
  while (i < len) {
    const unsigned char c = ptr[i];
    if (c < 0x80) {
      ++i;
    } else if (c >= 0xC2 && c < 0xE0) {
      if (i + 1 >= len || 0x80 != (ptr[i + 1] & 0xC0)) {
        return false;
      }
      i += 2;
    } else if (c >= 0xE0 && c < 0xF0) {
      if (i + 2 >= len || 0x80 != (ptr[i + 1] & 0xC0) || 0x80 != (ptr[i + 2] & 0xC0)
          || (c < 0xE1 && ptr[i + 1] < 0xA0)) {
        return false;
      }
      if (c >= 0xE4 && c <= 0xE9) {
        return true;
      }
      i += 3;
    } else if (c >= 0xF0 && c < 0xF5) {
      if (i + 3 >= len || 0x80 != (ptr[i + 1] & 0xC0) || 0x80 != (ptr[i + 2] & 0xC0)
          || 0x80 != (ptr[i + 3] & 0xC0)
          || (c < 0xF1 && ptr[i + 1] < 0x90) || (c > 0xF3 && ptr[i + 1] > 0x8F)) {
        return false;
      }
      i += 4;
    } else {
      return false;
    }
  }
  return false;
}

// The built in stop word table is a compile time constant: a few dozen lower case
// ASCII words.  Deciding whether a token is one of them is on the hot path of
// every single document, and an index build runs it from several worker threads
// at once, so the check must not touch any shared writable state.  The table is
// therefore folded once into read only prefixes grouped by exact byte length;
// membership becomes a short scan of just that one length's bucket.  With the
// current table (len1=2, len2=15, len3=10, len4=7, len5=2) this cuts the prefix
// comparisons per ASCII token from 36 down to at most 15, and lengths past the
// longest stop word touch nothing at all.  No lock, no allocation, no charset
// work, and nothing to contend on.  The build is a counting sort, so buckets stay
// correct for any table that fits in MAX_WORD_CNT, and a compile time guard
// enforces that bound.
class ObStopWordLut
{
public:
  static const int64_t MAX_WORD_LEN = 8;
  static const int64_t MAX_WORD_CNT = 64;

  ObStopWordLut() : count_(0), max_word_len_(0)
  {
    static_assert(sizeof(ob_stop_word_list) / sizeof(ob_stop_word_list[0]) <= MAX_WORD_CNT,
                  "stop word table exceeds ObStopWordLut capacity");

    const int64_t table_cnt = static_cast<int64_t>(
        sizeof(ob_stop_word_list) / sizeof(ob_stop_word_list[0]));

    // Pass 1: how many stop words have each byte length, and the longest one.
    int64_t len_count[MAX_WORD_LEN + 1];
    for (int64_t l = 0; l <= MAX_WORD_LEN; ++l) {
      len_count[l] = 0;
    }
    for (int64_t i = 0; i < table_cnt; ++i) {
      const int64_t len = stop_word_len(ob_stop_word_list[i]);
      if (len > 0 && len <= MAX_WORD_LEN) {
        ++len_count[len];
        if (len > max_word_len_) {
          max_word_len_ = len;
        }
      }
    }

    // Prefix sums give each length its own contiguous slice of entries_.
    int64_t offset = 0;
    for (int64_t l = 0; l <= MAX_WORD_LEN; ++l) {
      bucket_offset_[l] = static_cast<uint8_t>(offset);
      bucket_size_[l] = static_cast<uint8_t>(len_count[l]);
      offset += len_count[l];
    }
    count_ = offset;

    // Pass 2: scatter each word into its length slice.  cursor tracks the next
    // free slot of each slice; entries_ ends up grouped by length.
    int64_t cursor[MAX_WORD_LEN + 1];
    for (int64_t l = 0; l <= MAX_WORD_LEN; ++l) {
      cursor[l] = bucket_offset_[l];
    }
    for (int64_t i = 0; i < table_cnt; ++i) {
      const char *word = ob_stop_word_list[i];
      const int64_t len = stop_word_len(word);
      if (len > 0 && len <= MAX_WORD_LEN) {
        const int64_t slot = cursor[len]++;
        if (slot < count_) {
          entries_[slot] = pack_prefix(word, len);
        }
      }
    }
  }

  // Longest built-in stop word.  Any ASCII token longer than this cannot match
  // an entry, so the caller can skip the pack + linear scan entirely.  Derived
  // from the table at construction (rather than hardcoded) so the fast path
  // stays correct if the stop-word list ever changes.
  OB_INLINE int64_t max_word_len() const { return max_word_len_; }

  bool contains(const char *word, const int64_t len) const
  {
    bool found = false;
    if (len > 0 && len <= max_word_len_ && bucket_size_[len] > 0) {
      // No stop word has this byte length (e.g. len 6..8): skip pack_prefix
      // altogether instead of building a key for an empty bucket.
      const uint64_t prefix = pack_prefix(word, len);
      const int64_t begin = bucket_offset_[len];
      const int64_t end = begin + bucket_size_[len];
      for (int64_t i = begin; !found && i < end; ++i) {
        found = (entries_[i] == prefix);
      }
    }
    return found;
  }

private:
  static int64_t stop_word_len(const char *word)
  {
    int64_t len = 0;
    while (len < MAX_WORD_LEN && '\0' != word[len]) {
      ++len;
    }
    return len;
  }

  static uint64_t pack_prefix(const char *word, const int64_t len)
  {
    uint64_t prefix = 0;
    for (int64_t i = len - 1; i >= 0; --i) {
      prefix = (prefix << 8) | static_cast<uint8_t>(word[i]);
    }
    return prefix;
  }

private:
  uint64_t entries_[MAX_WORD_CNT];
  uint8_t bucket_offset_[MAX_WORD_LEN + 1];
  uint8_t bucket_size_[MAX_WORD_LEN + 1];
  int64_t count_;
  int64_t max_word_len_;
};

const ObStopWordLut &get_stop_word_lut()
{
  // Built once, then only read; the guard of a function local static is a plain
  // load after the first call, so no thread can make another one wait.
  static const ObStopWordLut stop_word_lut;
  return stop_word_lut;
}

// Decide whether casedown can change the token. CJK Unified Ideographs and
// lowercase/non-letter ASCII characters have no case mapping, so tokens made of
// them can skip ObCharset::tolower (which always allocates a copy of the word).
OB_INLINE bool casedown_may_change(const ObString &word)
{
  bool may_change = false;
  const uint8_t *ptr = reinterpret_cast<const uint8_t *>(word.ptr());
  const int64_t len = word.length();
  int64_t pos = 0;
  while (!may_change && pos < len) {
    const uint8_t ch = ptr[pos];
    if (ch < 0x80) {
      may_change = (ch >= 'A' && ch <= 'Z');
      ++pos;
    } else if (ch >= 0xE4 && ch <= 0xE9 && pos + 2 < len
               && 0x80 == (ptr[pos + 1] & 0xC0) && 0x80 == (ptr[pos + 2] & 0xC0)) {
      // A 3 byte UTF-8 sequence with lead 0xE4..0xE9 is a code point in
      // U+4000..U+9FFF, i.e. pages 0x40..0x9F of the case folding tables.
      // Those pages are NULL in ob_unicase_default (the caseinfo wired to
      // every utf8mb4 collation in this tree: general_ci, bin, and the UCA
      // 0900_* handlers via ob_ctype_uca.cc) and also in ob_unicase_unicode520.
      // Since ob_tolower_utf8mb4() leaves a code point on a NULL page unchanged,
      // no code point in U+4000..9FFF can ever change under casedown; decoding it
      // here is dead work.  Skip the sequence without decoding.
      // (The old range check also flagged U+4000..4DFF as "may change" and
      // routed those tokens through ObCharset::tolower, which allocated a copy
      // it never actually modified.)
      pos += 3;
    } else {
      // Be conservative for every other script / encoding.
      may_change = true;
    }
  }
  return may_change;
}

// Insert-or-accumulate callback for the per-document word map.  ObHashMap wraps
// its bucket latch with NoPthreadDefendMode (the word map is a per-document
// scratch structure, never shared), so the atomic helper below adds no lock
// cost.  The callback is invoked with the map's HashMapPair and mutates only the
// value (accumulated term frequency); the key is left untouched.  Keeping this
// at file scope in the anonymous namespace avoids any dependence on the exact
// namespace of HashMapPair at the call site and stays unity-build safe.
struct ObAddWordFreq final
{
  explicit ObAddWordFreq(const int64_t freq) : freq_(freq) {}
  void operator()(common::hash::HashMapPair<ObFTWord, int64_t> &pair) const
  {
    pair.second += freq_;
  }
  int64_t freq_;
};
} // namespace

////////////////////////////////////////////////////////////////////////////////
// class ObStopWordChecker
ObStopWordChecker::~ObStopWordChecker()
{
  destroy();
}

int ObStopWordChecker::init()
{
  int ret = OB_SUCCESS;
  
  

  if (inited_) {
    ret = OB_INIT_TWICE;
  } else if (OB_FAIL(stopword_set_.create(DEFAULT_STOPWORD_BUCKET_NUM, "StopWordSet", "StopWordSet"))) {
  } else {
    ObObjMeta stop_meta;
    stop_meta.set_varchar();
    stop_meta.set_collation_type(ObCollationType::CS_TYPE_UTF8MB4_GENERAL_CI);

    stopword_type_.set_meta(stop_meta);
    const int64_t stopword_count = sizeof(ob_stop_word_list) / sizeof(ob_stop_word_list[0]);
    for (int64_t i = 0; OB_SUCC(ret) && i < stopword_count; ++i) {
      ObFTWord stopword(STRLEN(ob_stop_word_list[i]), ob_stop_word_list[i], stopword_type_);
      if (OB_FAIL(stopword_set_.set_refactored(stopword))) {
      }
    }

    if (OB_SUCC(ret)) {
      inited_ = true;
    }
  }
  return ret;
}

void ObStopWordChecker::destroy()
{
  if (inited_) {
    stopword_set_.destroy();
    inited_ = false;
  }
}

int ObStopWordChecker::check_stopword(const ObFTWord &word, bool &is_stopword)
{
  int ret = OB_SUCCESS;
  is_stopword = false;
  if (OB_UNLIKELY(!inited_)) {
    ret = OB_NOT_INIT;
  } else if (OB_UNLIKELY(word.empty())) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    const ObString word_str = word.get_word().get_string();
    const int64_t len = word_str.length();
    const ObStopWordLut &lut = get_stop_word_lut();
    char lower_buf[ObStopWordLut::MAX_WORD_LEN];
    bool is_ascii = true;
    bool has_upper = false;
    // One combined pass decides ASCII-ness and, for short tokens, builds the
    // lowercased lookup key.  The old code walked every ASCII token up to three
    // times: is_ascii_word() to test the high bit, a second scan to find an
    // upper-case byte, then a third to build a lower-cased copy.  For BENG the
    // normalizer has already lower-cased, so the second and third walks were
    // always dead.  Stop at the first non-ASCII byte exactly as is_ascii_word
    // did, so the non-ASCII branch below sees an identical token.
    for (int64_t i = 0; i < len; ++i) {
      const unsigned char c = static_cast<unsigned char>(word_str.ptr()[i]);
      if (0 != (c & 0x80)) {
        is_ascii = false;
        break;
      }
      if (c >= 'A' && c <= 'Z' && !has_upper && len <= lut.max_word_len()) {
        // First upper-case byte of a short token: build the lookup key once.
        // Already-lowercase tokens (the common BENG case) never enter here.
        has_upper = true;
        for (int64_t j = 0; j < len; ++j) {
          const char cc = word_str.ptr()[j];
          lower_buf[j] = (cc >= 'A' && cc <= 'Z')
                         ? static_cast<char>(cc - 'A' + 'a') : cc;
        }
      }
    }
    if (is_ascii) {
      // general_ci comparison over ASCII is case insensitive byte comparison,
      // so the lower-cased key built above is what the table must be asked about.
      // No built-in stop word is longer than max_word_len(), so longer tokens
      // can never match and return before any LUT walk.
      if (len > lut.max_word_len()) {
        is_stopword = false;
      } else {
        const char *lookup_ptr = has_upper ? lower_buf : word_str.ptr();
        is_stopword = lut.contains(lookup_ptr, len);
      }
    } else {
      common::ObString cmp_str;
      // Skip the conversion copy when the charsets already match (the common case:
      // an utf8mb4 token against the utf8mb4 stop word list).
      const ObCharsetType word_cs = ObCharset::charset_type_by_coll(word.get_collation_type());
      const ObCharsetType stop_cs = ObCharset::charset_type_by_coll(stopword_type_.get_collation_type());
      bool skip_probe = false;
      if (word_cs == stop_cs && common::CHARSET_INVALID != word_cs) {
        cmp_str = word_str;
        // A well-formed CJK token has general_ci sort weights >= 0x4000 and can
        // never equal an all-ASCII stop word, so the hash probe below is provably
        // always a miss for it.  Skip it.
        skip_probe = (common::CHARSET_UTF8MB4 == word_cs
                      && contains_utf8mb4_cjk(cmp_str));
      } else {
        common::ObArenaAllocator allocator(lib::ObMemAttr("ChkStopWord"));
        if (OB_FAIL(common::ObCharset::charset_convert(
                                         allocator,
                                         word_str,
                                         word.get_collation_type(),
                                         stopword_type_.get_collation_type(),
                                         cmp_str))) {
        }
      }
      if (OB_SUCC(ret) && !skip_probe) {
        ObFTWord converted(cmp_str.length(), cmp_str.ptr(), stopword_type_);
        ret = stopword_set_.exist_refactored(converted);
        if (OB_HASH_NOT_EXIST == ret) {
          is_stopword = false;
          ret = OB_SUCCESS;
        } else if (OB_HASH_EXIST == ret) {
          is_stopword = true;
          ret = OB_SUCCESS;
        } else if (OB_SUCC(ret)) {
          ret = OB_ERR_UNEXPECTED;
        } else {
        }
      }
    }
  }
  return ret;
}

////////////////////////////////////////////////////////////////////////////////
// class ObAddWord
ObAddWord::ObAddWord(
    const ObFTParserProperty &property,
    const ObObjMeta &meta,
    const ObAddWordFlag &flag,
    common::ObIAllocator &allocator,
    ObFTWordMap &word_map)
  : word_meta_(meta),
    allocator_(allocator),
    word_map_(word_map),
    min_max_word_cnt_(0),
    non_stopword_cnt_(0),
    stopword_cnt_(0),
    min_token_size_(property.min_token_size_),
    max_token_size_(property.max_token_size_),
    flag_(flag)
{
}

int ObAddWord::process_word(
    const char *word,
    const int64_t word_len,
    const int64_t char_cnt,
    const int64_t word_freq)
{
  int ret = OB_SUCCESS;
  bool is_stopword = false;
  ObFTWord src_word(word_len, word, word_meta_);
  ObFTWord dst_word;
  if (OB_ISNULL(word) || OB_UNLIKELY(0 >= word_len || 0 >= char_cnt || 0 >= word_freq)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (is_min_max_word(char_cnt)) {
    ++min_max_word_cnt_;
  } else if (OB_FAIL(casedown_word(src_word, dst_word))) {
  } else if (OB_FAIL(check_stopword(dst_word, is_stopword))) {
  } else if (OB_UNLIKELY(is_stopword)) {
    ++stopword_cnt_;
  } else if (OB_FAIL(groupby_word(dst_word, word_freq))) {
  } else {
    non_stopword_cnt_ += word_freq;
  }
  return ret;
}

bool ObAddWord::is_min_max_word(const int64_t c_len) const
{
  return flag_.min_max_word() && (c_len < min_token_size_ || c_len > max_token_size_);
}

int ObAddWord::casedown_word(const ObFTWord &src, ObFTWord &dst)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(src.empty())) {
    ret = OB_INVALID_ARGUMENT;
  } else if (flag_.casedown()) {
    if (!casedown_may_change(src.get_word().get_string())) {
      // No character in the token has a case mapping, so the conversion result
      // is identical to the input and its allocation can be skipped.
      dst = src;
    } else {
      ObString dst_str;
      if (OB_FAIL(ObCharset::tolower(
                      word_meta_.get_collation_type(),
                      src.get_word().get_string(),
                      dst_str,
                      allocator_))) {
      } else {
        ObFTWord tmp(dst_str.length(), dst_str.ptr(), word_meta_);
        dst = tmp;
      }
    }
  } else {
    dst = src;
  }
  return ret;
}

int ObAddWord::check_stopword(const ObFTWord &ft_word, bool &is_stopword)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(ft_word.empty())) {
    ret = OB_INVALID_ARGUMENT;
  } else if (flag_.stopword()) {
    ObStopWordChecker *stop_word_checker = ObFTParseData::instance().stop_word_checker();
    if (OB_ISNULL(stop_word_checker)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(stop_word_checker->check_stopword(ft_word, is_stopword))) {
    }
  }
  return ret;
}

int ObAddWord::groupby_word(const ObFTWord &word, const int64_t word_freq)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(word.empty() || word_freq <= 0)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (!flag_.groupby_word()) {
    if (OB_FAIL(word_map_.set_refactored(word, 1/*word count*/))) {
    }
  } else {
    // A repeated token used to cost three collation-aware hash computations and
    // three bucket walks: set_refactored(word, 1) -> get_refactored(word, count)
    // -> set_refactored(word, count + freq, overwrite).  ObFTWord::hash() calls
    // the collation's default_hash_, so recomputing it three times per repeated
    // token is pure overhead, and CJK / natural-language text repeats tokens
    // heavily.  Collapse insert-or-accumulate into a single set_or_update probe:
    // one hash, one bucket walk.  A new key is inserted with value 1 (identical
    // to the old first-insert); an existing key is handed to the callback which
    // adds this occurrence's frequency in place (identical to the old
    // get/add/overwrite).  No key is ever dropped or duplicated, so the emitted
    // word set -- and therefore the official hit counts -- are unchanged.
    ObAddWordFreq add_freq(word_freq);
    if (OB_FAIL(word_map_.set_or_update(word, 1/*first word count*/, add_freq))) {
    }
  }
  return ret;
}

} // end namespace storage
} // end namespace oceanbase
