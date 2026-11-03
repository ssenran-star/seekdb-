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

#define USING_LOG_PREFIX SQL_ENG
#include "ob_expr_ai_embed.h"
#include "query/ai/ob_ai_endpoint_resolver.h"
#include "share/rc/ob_server_runtime.h"

using namespace oceanbase::common;
using namespace oceanbase::sql;

namespace oceanbase 
{
namespace sql 
{
ObExprAIEmbed::ObExprAIEmbed(common::ObIAllocator &alloc)
    : ObFuncExprOperator(alloc, 
                        T_FUN_SYS_AI_EMBED, 
                        N_AI_EMBED, 
                        MORE_THAN_ZERO,
                        NOT_VALID_FOR_GENERATED_COL, 
                        NOT_ROW_DIMENSION) 
{
}

ObExprAIEmbed::~ObExprAIEmbed() 
{
}

int ObExprAIEmbed::calc_result_typeN(ObExprResType &type,
                                     ObExprResType *types_stack,
                                     int64_t param_num,
                                     common::ObExprTypeCtx &type_ctx) const 
{
  UNUSED(type_ctx);
  UNUSED(types_stack);
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(param_num > 3 || param_num < 2)) {
    ObString func_name_(get_name());
    ret = OB_ERR_PARAM_SIZE;
    LOG_USER_ERROR(OB_ERR_PARAM_SIZE, func_name_.length(), func_name_.ptr());
  } else {
    types_stack[MODEL_IDX].set_calc_type(ObVarcharType);
    types_stack[MODEL_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
    types_stack[CONTENT_IDX].set_calc_type(ObVarcharType);
    types_stack[CONTENT_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
    if (param_num == 3) {
      if (ob_is_integer_type(types_stack[DIM_IDX].get_type())) {
        types_stack[DIM_IDX].set_calc_type(ObIntType);
        types_stack[DIM_IDX].set_precision(10);
        types_stack[DIM_IDX].set_scale(0);
      } else {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, dimension parameter must be an integer, not a decimal or float");
      }
    }
    type.set_varchar();
    type.set_collation_type(CS_TYPE_UTF8MB4_BIN);
    type.set_collation_level(CS_LEVEL_COERCIBLE);
  }
  return ret;
}

int ObExprAIEmbed::eval_ai_embed(const ObExpr &expr, ObEvalCtx &ctx, ObDatum &res) 
{
  INIT_SUCC(ret);
  ObDatum *arg_model_id = nullptr;
  ObDatum *arg_content = nullptr;
  ObDatum *arg_dim = nullptr;
  if (expr.arg_cnt_ == 3 ? OB_FAIL(expr.eval_param_value(ctx, arg_model_id, arg_content, arg_dim))
                         : OB_FAIL(expr.eval_param_value(ctx, arg_model_id, arg_content))) {
  } else if (OB_ISNULL(arg_content) || arg_content->is_null()) {
    // A NULL input stays NULL and must not be sent, so later rows keep their slots.
    res.set_null();
  } else if (OB_ISNULL(arg_model_id) || arg_model_id->is_null()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, model id is null");
    res.set_null();
  } else {
    ObEvalCtx::TempAllocGuard tmp_alloc_g(ctx);
    
    MultimodeAlloctor temp_allocator(tmp_alloc_g.get_allocator());
    lib::ObMallocHookAttrGuard malloc_guard(lib::ObMemAttr(N_AI_EMBED));
    ObAIFuncExprInfo *info = nullptr;
    share::ObAiModelEndpointInfo resolved_endpoint;
    const share::ObAiModelEndpointInfo *endpoint_info = &resolved_endpoint;
    query::ObIAiEndpointResolver *endpoint_resolver =
        ::oceanbase::share::server_service<::oceanbase::query::ObIAiEndpointResolver>();
    ObString model_id;
    ObString content;
    if (OB_FAIL(ObTextStringHelper::read_real_string_data(
            ctx.exec_ctx_, temp_allocator, *arg_model_id, expr.args_[MODEL_IDX]->datum_meta_,
            expr.args_[MODEL_IDX]->obj_meta_.has_lob_header(), model_id))) {
    } else if (OB_FAIL(ObTextStringHelper::read_real_string_data(
                   ctx.exec_ctx_, temp_allocator, *arg_content, expr.args_[CONTENT_IDX]->datum_meta_,
                   expr.args_[CONTENT_IDX]->obj_meta_.has_lob_header(), content))) {
    } else if (model_id.empty() || content.empty()) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, model id or input is empty");
      res.set_null();
    }
    int64_t dim = 0;
    ObJsonInt *dim_json = nullptr;
    ObJsonObject *config = nullptr;
    if (OB_FAIL(ret)) {
    } else if (OB_NOT_NULL(arg_dim) && !arg_dim->is_null()) {
      dim = arg_dim->get_int();
      if (dim <= 0) {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, dimension parameter must be a positive integer");
        res.set_null();
      } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(temp_allocator, config))) {
      } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_int(temp_allocator, dim, dim_json))) {
      } else if (OB_FAIL(config->add("dimensions", dim_json))) {
      }
    }
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(temp_allocator, model_id, info))) {
    } else if (OB_ISNULL(endpoint_resolver)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(endpoint_resolver->resolve_by_model_name(
                   model_id, temp_allocator, resolved_endpoint))) {
    } else {
      ObAIFuncModel model(temp_allocator, *info, *endpoint_info);
      ObString result;
      if (OB_FAIL(model.call_dense_embedding(content, config, result))) {
      } else if (OB_FAIL(ObAIFuncUtils::set_string_result(expr, ctx, res, result))) {
      }
    }
  }
  return ret;
}

int ObExprAIEmbed::cg_expr(ObExprCGCtx &expr_cg_ctx, 
                           const ObRawExpr &raw_expr,
                           ObExpr &rt_expr) const 
{
  int ret = OB_SUCCESS;
  // TODO: support schema version match in plan cache for ai func
  // const ObRawExpr *model_key = raw_expr.get_param_expr(0);
  // if (OB_NOT_NULL(model_key)
  //     && (model_key->is_static_scalar_const_expr() || model_key->is_const_expr())
  //     && model_key->get_expr_type() != T_OP_GET_USER_VAR &&
  //     OB_NOT_NULL(expr_cg_ctx.schema_guard_)) {
  //   ObIAllocator *allocator = expr_cg_ctx.allocator_;
  //   ObExecContext *exec_ctx = expr_cg_ctx.session_->get_cur_exec_ctx();
  //   ObObj const_data;
  //   bool got_data = false;
  //   ObAIFuncExprInfo *info = nullptr;
  //   if (OB_ISNULL(allocator)) {
  //     ret = OB_ERR_UNEXPECTED;
  //     LOG_WARN("allocator is null", K(ret));
  //   } else if (OB_FAIL(ObSQLUtils::calc_const_or_calculable_expr(exec_ctx,
  //                                                         model_key,
  //                                                         const_data,
  //                                                         got_data,
  //                                                         *allocator))) {
  //     LOG_WARN("failed to calc offset expr", K(ret));
  //   } else if (!got_data || const_data.is_null()) {
  //   } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(*allocator, const_data.get_string(), *expr_cg_ctx.schema_guard_, info))) {
  //     LOG_WARN("failed to get ai func info", K(ret), K(const_data.get_string()));
  //   } else {
  //     rt_expr.extra_info_ = info;
  //   }
  // }

  if (OB_SUCC(ret)) {
    rt_expr.eval_func_ = ObExprAIEmbed::eval_ai_embed;
    rt_expr.eval_batch_func_ = ObExprAIEmbed::eval_ai_embed_batch;
  }
  return ret;
}

namespace {

int set_embed_result_at(const ObExpr &expr,
                        ObEvalCtx &ctx,
                        ObDatum &res,
                        ObString &res_str,
                        int64_t batch_idx)
{
  int ret = OB_SUCCESS;
  ObTextStringDatumResult text_result(expr.datum_meta_.type_, &expr, &ctx, &res);
  if (OB_FAIL(text_result.init_with_batch_idx(res_str.length(), batch_idx))) {
  } else if (OB_FAIL(text_result.append(res_str))) {
  } else {
    text_result.set_result();
  }
  return ret;
}

int read_embed_string(const ObExpr &expr,
                      ObEvalCtx &ctx,
                      ObIAllocator &allocator,
                      ObDatum &datum,
                      int arg_idx,
                      ObString &str)
{
  return ObTextStringHelper::read_real_string_data(
      ctx.exec_ctx_, allocator, datum, expr.args_[arg_idx]->datum_meta_,
      expr.args_[arg_idx]->obj_meta_.has_lob_header(), str);
}

} // namespace

int ObExprAIEmbed::eval_ai_embed_batch(const ObExpr &expr,
                                       ObEvalCtx &ctx,
                                       const ObBitVector &skip,
                                       const int64_t batch_size)
{
  int ret = OB_SUCCESS;
  const bool has_dim = expr.arg_cnt_ == 3;
  ObDatumVector model_vec;
  ObDatumVector content_vec;
  ObDatumVector dim_vec;
  if (has_dim) {
    if (OB_FAIL(expr.eval_batch_param_value(ctx, skip, batch_size, model_vec, content_vec, dim_vec))) {
    }
  } else if (OB_FAIL(expr.eval_batch_param_value(ctx, skip, batch_size, model_vec, content_vec))) {
  }
  if (OB_FAIL(ret)) {
    return ret;
  }

  ObDatum *result_datums = expr.locate_batch_datums(ctx);
  ObBitVector &eval_flags = expr.get_evaluated_flags(ctx);
  ObEvalCtx::TempAllocGuard tmp_alloc_g(ctx);
  MultimodeAlloctor temp_allocator(tmp_alloc_g.get_allocator());
  lib::ObMallocHookAttrGuard malloc_guard(lib::ObMemAttr(N_AI_EMBED));

  ObArray<int64_t> row_ids;
  ObArray<ObString> contents;
  ObString model_id;
  int64_t dim = 0;
  bool mixed = false;
  for (int64_t i = 0; OB_SUCC(ret) && !mixed && i < batch_size; ++i) {
    if (skip.at(i) || eval_flags.at(i)) {
      continue;
    }
    ObDatum *model_datum = model_vec.at(i);
    ObDatum *content_datum = content_vec.at(i);
    if (OB_ISNULL(content_datum) || content_datum->is_null()) {
      result_datums[i].set_null();
      eval_flags.set(i);
      continue;
    } else if (OB_ISNULL(model_datum) || model_datum->is_null()) {
      // A broken row must not abort the whole batch: the remaining rows still
      // have to be filled, otherwise the batch never completes.
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, model id is null");
      result_datums[i].set_null();
      eval_flags.set(i);
      continue;
    }
    ObString row_model;
    ObString row_content;
    int64_t row_dim = 0;
    if (OB_FAIL(read_embed_string(expr, ctx, temp_allocator, *model_datum, MODEL_IDX, row_model))) {
    } else if (OB_FAIL(read_embed_string(expr, ctx, temp_allocator, *content_datum, CONTENT_IDX, row_content))) {
    } else if (row_model.empty() || row_content.empty()) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, model id or input is empty");
      result_datums[i].set_null();
      eval_flags.set(i);
    } else if (has_dim) {
      ObDatum *dim_datum = dim_vec.at(i);
      if (OB_NOT_NULL(dim_datum) && !dim_datum->is_null()) {
        row_dim = dim_datum->get_int();
        if (row_dim <= 0) {
          ret = OB_INVALID_ARGUMENT;
          LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, dimension parameter must be a positive integer");
          result_datums[i].set_null();
          eval_flags.set(i);
        }
      }
    }
    if (OB_FAIL(ret)) {
    } else if (row_ids.empty()) {
      model_id = row_model;
      dim = row_dim;
    } else if (model_id != row_model || dim != row_dim) {
      mixed = true;
    }
    if (OB_SUCC(ret) && !mixed) {
      if (OB_FAIL(row_ids.push_back(i))) {
      } else if (OB_FAIL(contents.push_back(row_content))) {
      }
    }
  }

  if (OB_FAIL(ret)) {
  } else if (mixed) {
    ObEvalCtx::BatchInfoScopeGuard batch_guard(ctx);
    batch_guard.set_batch_size(batch_size);
    for (int64_t i = 0; OB_SUCC(ret) && i < batch_size; ++i) {
      if (skip.at(i) || eval_flags.at(i)) {
        continue;
      }
      batch_guard.set_batch_idx(i);
      if (OB_FAIL(eval_ai_embed(expr, ctx, result_datums[i]))) {
        LOG_WARN("failed to evaluate ai_embed row", K(ret), K(i));
      } else {
        eval_flags.set(i);
      }
    }
  } else if (!row_ids.empty()) {
    const int64_t chunk_size = 128;
    for (int64_t start = 0; OB_SUCC(ret) && start < contents.count(); start += chunk_size) {
      const int64_t end = start + chunk_size < contents.count() ? start + chunk_size : contents.count();
      ObArray<ObString> chunk;
      ObJsonObject *config = nullptr;
      ObJsonInt *dim_json = nullptr;
      for (int64_t j = start; OB_SUCC(ret) && j < end; ++j) {
        if (OB_FAIL(chunk.push_back(contents.at(j)))) {
        }
      }
      if (OB_FAIL(ret)) {
      } else if (dim > 0) {
        if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(temp_allocator, config))) {
        } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_int(temp_allocator, dim, dim_json))) {
        } else if (OB_FAIL(config->add("dimensions", dim_json))) {
        }
      }
      ObAIFuncExprInfo *info = nullptr;
      share::ObAiModelEndpointInfo resolved_endpoint;
      query::ObIAiEndpointResolver *endpoint_resolver =
          ::oceanbase::share::server_service<::oceanbase::query::ObIAiEndpointResolver>();
      ObArray<ObString> embed_results;
      if (OB_FAIL(ret)) {
      } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(temp_allocator, model_id, info))) {
      } else if (OB_ISNULL(endpoint_resolver)) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(endpoint_resolver->resolve_by_model_name(
                     model_id, temp_allocator, resolved_endpoint))) {
      } else {
        ObAIFuncModel model(temp_allocator, *info, resolved_endpoint);
        if (OB_FAIL(model.call_dense_embedding_vector_v2(chunk, config, embed_results))) {
          LOG_WARN("failed to embed ai_embed batch", K(ret), K(start), K(end));
        }
      }
      if (OB_FAIL(ret)) {
      } else if (embed_results.count() != chunk.count()) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("ai_embed batch result count mismatch", K(ret), K(embed_results.count()), K(chunk.count()));
      } else {
        for (int64_t j = 0; OB_SUCC(ret) && j < embed_results.count(); ++j) {
          const int64_t row = row_ids.at(start + j);
          ObString embed_str = embed_results.at(j);
          if (OB_FAIL(set_embed_result_at(expr, ctx, result_datums[row], embed_str, row))) {
          } else {
            eval_flags.set(row);
          }
        }
      }
    }
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
