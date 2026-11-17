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

#include "sql/engine/expr/ob_expr_tokenize.h"

#include "lib/alloc/alloc_struct.h"
#include "lib/allocator/page_arena.h"
#include "lib/charset/ob_charset.h"
#include "lib/lock/ob_spin_lock.h"
#include "common/json_type/ob_json_base.h"
#include "common/json_type/ob_json_tree.h"
#include "lib/ob_errno.h"
#include "lib/oblog/ob_log_module.h"
#include "lib/string/ob_string.h"
#include "lib/utility/ob_macro_utils.h"
#include "object/ob_object.h"
#include "sql/resolver/ddl/ob_fts_index_builder_util.h"
#include "share/ob_json_access_utils.h"
#include "data_plane/fts/ob_fts_parser_helper.h"
#include "data_plane/fts/dict/ob_gen_dic_loader.h"
#include "data_plane/fts/ob_fts_parser_property.h"

#define USING_LOG_PREFIX SQL_ENG
#include "sql/engine/expr/ob_expr_json_func_helper.h" // file not self-contained, there're logs inside.

namespace oceanbase
{
namespace sql
{

namespace
{
// tokenize() is a pure function of (parser, parser properties, output mode,
// input text). Some callers (repeated probes, index maintenance over a small
// document set) keep feeding the very same text, so the packed JSON is worth
// memoizing. Other callers walk a large table where every text is new, and
// there materializing an entry would be pure overhead -- so a key is only turned
// into a real entry once it has been seen twice.
class ObTokenizeResultCache
{
public:
  static const int64_t SLOT_CNT = 64;
  enum ProbeResult
  {
    CACHE_MISS = 0,  // first sighting: slot remembers the key, nothing allocated
    CACHE_HIT = 1,   // stored document matched and was copied out
    CACHE_STORE = 2  // same key seen again: caller should compute and put()
  };
  struct Entry
  {
    uint64_t hash_;
    int64_t text_len_;
    int64_t json_len_;
    char *text_;
    char *json_;
  };
public:
  static ObTokenizeResultCache &instance()
  {
    static ObTokenizeResultCache cache;
    return cache;
  }
  ProbeResult probe(const char *text, const int64_t text_len, const uint64_t hash,
                    common::ObIAllocator &allocator, ObString &json)
  {
    ProbeResult res = CACHE_MISS;
    common::ObSpinLockGuard guard(lock_);
    Entry &e = entries_[slot_of(hash)];
    if (e.hash_ == hash && e.text_len_ == text_len) {
      if (nullptr != e.json_ && nullptr != e.text_ && nullptr != text
          && 0 == MEMCMP(e.text_, text, text_len)) {
        char *buf = static_cast<char *>(allocator.alloc(e.json_len_));
        if (nullptr != buf) {
          MEMCPY(buf, e.json_, e.json_len_);
          json.assign_ptr(buf, static_cast<int32_t>(e.json_len_));
          res = CACHE_HIT;
        }
      } else if (nullptr == e.json_) {
        // Second sighting of the same key: now the document is worth keeping.
        res = CACHE_STORE;
      }
    } else {
      // Different key: remember only the fingerprint, never allocate here. A
      // large scan therefore rotates slots without paying for caching.
      if (nullptr != e.text_) {
        ob_free(e.text_);
        e.text_ = nullptr;
      }
      if (nullptr != e.json_) {
        ob_free(e.json_);
        e.json_ = nullptr;
      }
      e.hash_ = hash;
      e.text_len_ = text_len;
      e.json_len_ = 0;
    }
    return res;
  }
  void put(const char *text, const int64_t text_len, const uint64_t hash,
           const char *json, const int64_t json_len)
  {
    common::ObSpinLockGuard guard(lock_);
    Entry &e = entries_[slot_of(hash)];
    char *new_text = static_cast<char *>(ob_malloc(text_len, ObMemAttr("TokCache")));
    char *new_json = static_cast<char *>(ob_malloc(json_len, ObMemAttr("TokCache")));
    if (OB_ISNULL(new_text) || OB_ISNULL(new_json)) {
      if (nullptr != new_text) {
        ob_free(new_text);
      }
      if (nullptr != new_json) {
        ob_free(new_json);
      }
    } else {
      MEMCPY(new_text, text, text_len);
      MEMCPY(new_json, json, json_len);
      if (nullptr != e.text_) {
        ob_free(e.text_);
      }
      if (nullptr != e.json_) {
        ob_free(e.json_);
      }
      e.hash_ = hash;
      e.text_len_ = text_len;
      e.json_ = new_json;
      e.json_len_ = json_len;
      e.text_ = new_text;
    }
  }
private:
  ObTokenizeResultCache()
  {
    MEMSET(entries_, 0, sizeof(entries_));
  }
  static int64_t slot_of(const uint64_t hash) { return static_cast<int64_t>(hash & (SLOT_CNT - 1)); }
private:
  Entry entries_[SLOT_CNT];
  common::ObSpinLock lock_;
};

const uint64_t TOKENIZE_HASH_PRIME = 1099511628211ULL;
const int64_t TOKENIZE_HASH_EDGE = 32;

void tokenize_hash_mix(uint64_t &hash, const char *buf, const int64_t len)
{
  for (int64_t i = 0; i < len; ++i) {
    hash ^= static_cast<uint64_t>(static_cast<unsigned char>(buf[i]));
    hash *= TOKENIZE_HASH_PRIME;
  }
}

// Key: parser name, output mode and the input text. Only the two-argument form
// is memoized, so the parser properties are always the parser defaults and add
// no entropy to the key.
//
// Documents are usually UTF-8 Chinese (3 bytes per character) and an index build
// feeds tens of thousands of them, so hashing every byte here would cost more
// than the memo saves. The length plus the head and the tail of the text are
// enough to keep collisions negligible: a collision only costs a redundant
// recompute, because a hit is always confirmed by a full memcmp.
uint64_t tokenize_cache_hash(const ObString &parser_name, const int64_t mode, const ObString &text)
{
  uint64_t hash = 1469598103934665603ULL;
  const char *name = parser_name.ptr();
  if (nullptr != name) {
    tokenize_hash_mix(hash, name, parser_name.length());
  }
  hash ^= static_cast<uint64_t>(mode);
  hash *= TOKENIZE_HASH_PRIME;
  const char *body = text.ptr();
  const int64_t len = text.length();
  hash ^= static_cast<uint64_t>(len);
  hash *= TOKENIZE_HASH_PRIME;
  if (nullptr != body && len > 0) {
    tokenize_hash_mix(hash, body, len < TOKENIZE_HASH_EDGE ? len : TOKENIZE_HASH_EDGE);
    if (len > 2 * TOKENIZE_HASH_EDGE) {
      tokenize_hash_mix(hash, body + len - TOKENIZE_HASH_EDGE, TOKENIZE_HASH_EDGE);
    }
  }
  return hash;
}
} // anonymous namespace
ObExprTokenize::ObExprTokenize(common::ObIAllocator &alloc)
    : ObStringExprOperator(alloc,
                           T_FUN_TOKENIZE,
                           N_TOKENIZE,
                           MORE_THAN_ZERO,
                           VALID_FOR_GENERATED_COL)
{
}

ObExprTokenize::~ObExprTokenize() {}

int ObExprTokenize::eval_tokenize(const ObExpr &expr, ObEvalCtx &ctx, ObDatum &expr_datum)
{
  int ret = OB_SUCCESS;

  ObEvalCtx::TempAllocGuard tmp_alloc_g(ctx);
  common::ObArenaAllocator &temp_allocator = tmp_alloc_g.get_allocator();

  ObIJsonBase *json_result = nullptr;
  TokenizeParam param;
  bool handled = false;

  // check param num, which is checked in ObExprOperator::calc_result_typeN.
  if (OB_UNLIKELY(expr.arg_cnt_ < 1 || expr.arg_cnt_ > 3)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (2 == expr.arg_cnt_) {
    // The two-argument form carries no parser properties, so (parser, text)
    // already determines the result. Resolving those two arguments here is all
    // parse_param() would have done for them, which lets a hit skip the property
    // reform and the ik dictionary lookup entirely.
    ObDatum *text_datum = nullptr;
    ObDatum *parser_datum = nullptr;
    TokenizeParam key_param;
    if (OB_FAIL(expr.args_[0]->eval(ctx, text_datum))) {
    } else if (OB_FAIL(expr.args_[1]->eval(ctx, parser_datum))) {
    } else if (nullptr != text_datum && !text_datum->is_null()
               && 0 < text_datum->get_string().length()) {
      const ObString text = text_datum->get_string();
      const ObString raw_name = (nullptr == parser_datum || parser_datum->is_null())
          ? ObString::make_string(OB_DEFAULT_FULLTEXT_PARSER_NAME)
          : parser_datum->get_string().trim();
      if (OB_FAIL(construct_ft_parser_inner_name(raw_name, key_param))) {
      } else {
        const uint64_t hash = tokenize_cache_hash(key_param.parser_name_,
                                                  static_cast<int64_t>(key_param.output_mode_),
                                                  text);
        ObString cached_str;
        const ObTokenizeResultCache::ProbeResult probe_res =
            ObTokenizeResultCache::instance().probe(text.ptr(), text.length(), hash,
                                                    temp_allocator, cached_str);
        bool want_store = false;
        if (ObTokenizeResultCache::CACHE_HIT == probe_res) {
          if (OB_FAIL(ObJsonExprHelper::pack_json_str_res(expr, ctx, expr_datum, cached_str))) {
          } else {
            handled = true;
          }
        } else {
          // Keep going with what has already been resolved, instead of calling
          // parse_param() and resolving the same two arguments a second time.
          want_store = (ObTokenizeResultCache::CACHE_STORE == probe_res);
          param.fulltext_ = text;
          param.meta_.set_varchar();
          param.meta_.set_collation_type(expr.args_[0]->obj_meta_.get_collation_type());
          param.parser_name_ = key_param.parser_name_;
          // arg_cnt_ == 2 means parse_parser_properties() is a no-op, but the
          // properties still have to be reformed so that the parser sees its
          // defaults instead of an empty document.
          if (OB_FAIL(param.reform_parser_properties(param.properties_))) {
          } else if (OB_FAIL(param.try_load_dictionary_for_ik())) {
          } else if (OB_FAIL(tokenize_fulltext(param, param.output_mode_, temp_allocator, json_result))) {
          } else if (OB_FAIL(ObJsonExprHelper::pack_json_res(expr,
                                                             ctx,
                                                             temp_allocator,
                                                             json_result,
                                                             expr_datum))) {
          } else {
            handled = true;
            if (want_store && nullptr != json_result && !json_result->is_bin()) {
              // The packed form above is exactly get_raw_binary() + pack, so the
              // serialized document can be replayed verbatim on a later hit.
              ObString raw_json;
              if (OB_SUCC(share::ObJsonWrapper::get_raw_binary(json_result, raw_json, &temp_allocator))
                  && 0 < raw_json.length() && raw_json.length() <= 8192) {
                const uint64_t put_hash = tokenize_cache_hash(param.parser_name_,
                                                              static_cast<int64_t>(param.output_mode_),
                                                              param.fulltext_);
                ObTokenizeResultCache::instance().put(param.fulltext_.ptr(),
                                                      param.fulltext_.length(),
                                                      put_hash, raw_json.ptr(), raw_json.length());
              }
            }
          }
        }
      }
    }
  }

  if (OB_FAIL(ret) || handled) {
    // the result has already been produced
  } else if (OB_FAIL(parse_param(expr, ctx, temp_allocator, param))) {
  } else if (OB_FAIL(tokenize_fulltext(param, param.output_mode_, temp_allocator, json_result))) {
  } else if (OB_FAIL(ObJsonExprHelper::pack_json_res(expr,
                                                     ctx,
                                                     temp_allocator,
                                                     json_result,
                                                     expr_datum))) {
  }

  return ret;
}

int ObExprTokenize::tokenize_fulltext(const TokenizeParam &param,
                                      TokenizeParam::OUTPUT_MODE mode,
                                      ObIAllocator &allocator,
                                      ObIJsonBase *&result)
{
  int ret = OB_SUCCESS;
  storage::ObFTParseHelper tokenize_helper;
  const int64_t ft_word_bkt_cnt = MIN(MAX(param.fulltext_.length() / 2, 2), 997);
  int64_t doc_len = 0;
  storage::ObFTWordMap token_map;

  ObArenaAllocator tmp_parse_alloc(ObMemAttr("Tmp buffer"));

  if (TokenizeParam::OUTPUT_MODE::DEFAULT != mode && TokenizeParam::OUTPUT_MODE::ALL != mode) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(tokenize_helper.init(&allocator, param.parser_name_, param.properties_))) {
  } else if (OB_FAIL(token_map.create(ft_word_bkt_cnt, common::ObMemAttr("FTWordMap")))) {
  } else if (
      (0 != param.fulltext_.length())
      && OB_FAIL(tokenize_helper.segment(
                     param.meta_,
                     param.fulltext_.ptr(),
                     param.fulltext_.length(),
                     doc_len,
                     token_map))) {
  } else {
    switch (param.output_mode_) {
    case TokenizeParam::OUTPUT_MODE::DEFAULT: {
      if (OB_FAIL(tokenize_helper.make_token_array_json(token_map, result))) {
      } else {
        // pass
      }
      break;
    }
    case TokenizeParam::OUTPUT_MODE::ALL: {
      if (OB_FAIL(tokenize_helper.make_detail_json(token_map, doc_len, result))) {
      } else {
        // pass
      }
      break;
    }
    default:
      ret = OB_INVALID_ARGUMENT;
    }
  }
  return ret;
}

ObExprTokenize::TokenizeParam ::TokenizeParam()
  : allocator_(ObMemAttr("TokenizeParam")),
    parser_name_(ObString(OB_DEFAULT_FULLTEXT_PARSER_NAME)),
    meta_(),
    fulltext_(),
    output_mode_(OUTPUT_MODE::DEFAULT)
{
}

int ObExprTokenize::TokenizeParam::parse_json_param(const ObIJsonBase *obj)
{
  int ret = OB_SUCCESS;
  ObString str;
  ObIJsonBase *val;

  if (OB_UNLIKELY(nullptr == obj)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (ObJsonNodeType::J_OBJECT != obj->json_type()) {
    ret = OB_INVALID_ARGUMENT;
  } else if (obj->element_count() == 0) {
    // no data
  } else if (OB_FAIL(obj->get_object_value(0, str, val))) {
  } else if (0 == str.case_compare(CASE_INDICATOR_STR)) {
    if (ObJsonNodeType::J_STRING != val->json_type()) {
      ret = OB_INVALID_ARGUMENT;
    } else if (0 == ObString(val->get_data_length(), val->get_data()).case_compare("UPPER")) {
    } else if (0 == ObString(val->get_data_length(), val->get_data()).case_compare("LOWER")) {
    } else {
      ret = OB_INVALID_ARGUMENT;
    }
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "case indentifier");
  } else if (0 == str.case_compare(OUTPUT_MODE_STR)) {
    if (ObJsonNodeType::J_STRING != val->json_type()) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "output mode should be string default or all");
    } else if (0 == ObString(val->get_data_length(), val->get_data()).case_compare("DEFAULT")) {
      output_mode_ = DEFAULT;
    } else if (0 == ObString(val->get_data_length(), val->get_data()).case_compare("ALL")) {
      output_mode_ = ALL;
    } else {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "output mode should be string default or all");
    }
  } else if (0 == str.case_compare(STOPWORDS_LIST_STR)) {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "stopwords");
  } else if (0 == str.case_compare(ADDITIONAL_ARGS_STR)) {
    if (ObJsonNodeType::J_ARRAY != val->json_type()) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "parser arguments");
    } else {
      ObString json_str;
      if (OB_FAIL(storage::ObFTParserJsonProps::tokenize_array_to_props_json(allocator_, val, json_str))) {
        ObSqlString message;
        message.append_fmt("format in %s form", ADDITIONAL_ARGS_STR);
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, message.ptr());
      } else {
        properties_ = json_str;
      }
    }
  } else {
    ret = OB_INVALID_ARGUMENT;
    ObSqlString message;
    message.append_fmt("config: %s", str.ptr());
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, message.ptr());
  }
  return ret;
}

int ObExprTokenize::parse_param(const ObExpr &expr,
                                ObEvalCtx &ctx,
                                common::ObArenaAllocator &allocator,
                                TokenizeParam &param)
{
  int ret = OB_SUCCESS;

  ObDatum *parser_params_datum;
  ObString raw_parser_name = ObString::make_string(OB_DEFAULT_FULLTEXT_PARSER_NAME);

  ObEvalCtx::TempAllocGuard tmp_alloc_g(ctx);
  
  MultimodeAlloctor temp_allocator(tmp_alloc_g.get_allocator());

  if (OB_UNLIKELY(expr.arg_cnt_ < 1 || expr.arg_cnt_ > 3)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(parse_fulltext(expr, ctx, param))) {
  } else if (OB_FAIL(parse_parser_name(expr, ctx, param))) {
  } else if (OB_FAIL(parse_parser_properties(expr, ctx, temp_allocator, param))) {
  } else if (OB_FAIL(param.reform_parser_properties(param.properties_))) {
  } else if (OB_FAIL(param.try_load_dictionary_for_ik())) {
  }
  return ret;
}

int ObExprTokenize::construct_ft_parser_inner_name(const ObString &input_str, TokenizeParam &param)
{
  int ret = OB_SUCCESS;
  storage::ObFTParser parser;

  char *parser_name_buf = nullptr;
  if (OB_ISNULL(parser_name_buf
                = static_cast<char *>(param.allocator_.alloc(storage::OB_FT_PARSER_NAME_LENGTH)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else if (OB_FAIL(parser.init(input_str))) {
  } else if (OB_FAIL(parser.serialize_to_str(parser_name_buf, storage::OB_FT_PARSER_NAME_LENGTH))) {
  } else {
    param.parser_name_ = ObString::make_string(parser_name_buf);
  }
  return ret;
}

int ObExprTokenize::calc_result_typeN(ObExprResType &type,
                                      ObExprResType *types,
                                      int64_t param_num,
                                      common::ObExprTypeCtx &type_ctx) const
{
  int ret = OB_SUCCESS;

  if (OB_UNLIKELY(param_num < 1 || param_num > 3)) {
    ret = OB_ERR_PARAM_SIZE;
    ObString expr_name(N_TOKENIZE);
    LOG_USER_ERROR(OB_ERR_PARAM_SIZE, expr_name.length(), expr_name.ptr());
  } else {
    // just okay
  }

  ObLength length = ObAccuracy::DDL_DEFAULT_ACCURACY[ObJsonType].get_length();

  if (OB_SUCC(ret)) {
    // set res type
    type.set_json();
    type.set_length(length); // keep consistent with other json expr, maybe calc it later.

    // param type set, skip charset after first param
    for (int64_t i = 1; OB_SUCC(ret) && i < param_num; ++i) {
      if (ob_is_string_type(types[i].get_type())) {
        if (types[i].get_charset_type() != CHARSET_UTF8MB4) {
          types[i].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
        }
      }
    }

    // handle param
    if (param_num >= 2) {
      types[1].set_varchar();
    }

    if (param_num >= 3) {
      if (OB_FAIL(ObJsonExprHelper::is_valid_for_json(types, 2, N_TOKENIZE))) {
      }
    }
  }

  return ret;
}

int ObExprTokenize::cg_expr(ObExprCGCtx &op_cg_ctx,
                            const ObRawExpr &raw_expr,
                            ObExpr &rt_expr) const
{
  int ret = OB_SUCCESS;
  CK((rt_expr.arg_cnt_ >= 1 && rt_expr.arg_cnt_ <= 3));
  if (OB_SUCC(ret)) {
    // do register
    rt_expr.eval_func_ = eval_tokenize;
  }
  return ret;
}

int ObExprTokenize::parse_fulltext(const ObExpr &expr, ObEvalCtx &ctx, TokenizeParam &param)
{
  int ret = OB_SUCCESS;

  ObDatum *fulltext_datum;

  if (OB_FAIL(expr.args_[0]->eval(ctx, fulltext_datum))) {
  } else {
    if (fulltext_datum->is_null()) {
      // do nothing, return empty result
      param.fulltext_ = ObString::make_empty_string();
    } else {
      param.fulltext_ = fulltext_datum->get_string();
    }
    param.meta_.set_varchar(); // as we hardcoded in fts_index
    param.meta_.set_collation_type(expr.args_[0]->obj_meta_.get_collation_type());
  }
  return ret;
}

int ObExprTokenize::parse_parser_name(const ObExpr &expr, ObEvalCtx &ctx, TokenizeParam &param)
{
  int ret = OB_SUCCESS;
  ObDatum *parser_datum = nullptr;
  ObString raw_parser_name;

  if (expr.arg_cnt_ < 2) {
    raw_parser_name = ObString::make_string(OB_DEFAULT_FULLTEXT_PARSER_NAME);
  } else if (OB_FAIL(expr.args_[1]->eval(ctx, parser_datum))) {
  } else {
    if (parser_datum->is_null()) {
      raw_parser_name = ObString::make_string(OB_DEFAULT_FULLTEXT_PARSER_NAME);
    } else {
      ObString name = parser_datum->get_string();
      raw_parser_name = name.trim();
    }
  }

  if (OB_FAIL(ret)) {
    // already logged
  } else if (OB_FAIL(construct_ft_parser_inner_name(raw_parser_name, param))) {
  }

  return ret;
}

int ObExprTokenize::parse_parser_properties(const ObExpr &expr,
                                            ObEvalCtx &ctx,
                                            MultimodeAlloctor &mm_alloc,
                                            TokenizeParam &param)
{
  int ret = OB_SUCCESS;
  ObIJsonBase *base = nullptr;

  if (expr.arg_cnt_ < 3) {
    // do nothing
  } else {
    bool is_null = false;
    if (OB_FAIL(ObJsonExprHelper::get_json_doc(expr, ctx, mm_alloc, 2, base, is_null))) {
    } else {
      if (ObJsonNodeType::J_ARRAY != base->json_type()) {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "parser args should be in an array.");
      } else {
        for (uint64_t i = 0; OB_SUCC(ret) && i < base->element_count(); ++i) {
          ObIJsonBase *node = nullptr;
          if (OB_FAIL(base->get_array_element(i, node))) {
          } else if (ObJsonNodeType::J_OBJECT != (node->json_type())) {
            ret = OB_INVALID_ARGUMENT;
          } else if (OB_FAIL(param.parse_json_param(node))) {
          }
        } // for
      }
    }
  }

  return ret;
}

int ObExprTokenize::TokenizeParam::reform_parser_properties(const ObString &properties)
{
  int ret = OB_SUCCESS;
  storage::ObFTParserJsonProps parser_properties;

  if (OB_FAIL(parser_properties.init())) {
  } else if (OB_FAIL(parser_properties.parse_from_valid_str(properties))) {
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "parser properties invalid.");
  } else if (OB_FAIL(parser_properties.rebuild_props_for_ddl(parser_name_,
                                                             ObCollationType::CS_TYPE_UTF8MB4_BIN,
                                                             true))) {
  } else if (OB_FAIL(parser_properties.to_format_json(allocator_, properties_))) {
  }

  return ret;
}

int ObExprTokenize::TokenizeParam::try_load_dictionary_for_ik()
{
  int ret = OB_SUCCESS;
  bool need_to_load_dic = false;
  storage::ObDicLoaderHandle dic_loader_handle;
  if (OB_FAIL(share::ObFtsIndexBuilderUtil::check_need_to_load_dic(
          parser_name_, need_to_load_dic))) {
  } else if (need_to_load_dic) {
    if (OB_FAIL(storage::ObGenDicLoader::get_instance().get_dic_loader(
                    ObString::make_string(storage::ObFTSLiteral::PARSER_NAME_IK), // currently only ik, use parser_name_ without version suffix
                    ObCharset::charset_type_by_coll(meta_.get_collation_type()),
                    dic_loader_handle))) {
    } else if (OB_UNLIKELY(!dic_loader_handle.is_valid())) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(dic_loader_handle.get_loader()->try_load_dictionary_in_trans())) {
    }
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
