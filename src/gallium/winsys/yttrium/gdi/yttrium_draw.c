/*
 * SPDX-FileCopyrightText: 2026 Ake Rehnman <ake.rehnman@gmail.com>
 * SPDX-License-Identifier: MPL-2.0
 */

#include "yttrium_draw.h"

#include <stdint.h>
#include <string.h>
#include <windows.h>

#include "pipe/p_context.h"
#include "pipe/p_defines.h"
#include "pipe/p_state.h"
#include "util/blend.h"
#include "util/format/u_format.h"
#include "util/u_math.h"
#include "util/u_memory.h"

#include "yttrium_internal.h"
#include "yttrium_context.h"
#include "yttrium_gdi_public.h"
#include "yttrium_pipeline.h"
#include "yttrium_resource.h"
#include "yttrium_shader.h"
#include "yttrium_trace.h"
#include "yttrium_venus.h"

static void
yttrium_record_draw_failure(struct yttrium_context *yctx)
{
   if (yctx)
      InterlockedExchange(&yctx->draw_failure, 1);
}

bool
yttrium_gdi_take_draw_failure(struct pipe_context *ctx)
{
   if (!ctx)
      return false;

   /* u_threaded_context exposes its driver context without draining queued
    * work.  The atomic latch is specifically meant to preserve asynchronous
    * submission, so do not use threaded_context_unwrap_sync here. */
   if (ctx->callback) {
      struct pipe_context *driver = threaded_context(ctx)->pipe;

      if (!driver || driver->draw_vbo != yttrium_draw_vbo)
         return false;
      ctx = driver;
   } else if (ctx->draw_vbo != yttrium_draw_vbo) {
      return false;
   }

   return InterlockedExchange(&yttrium_context(ctx)->draw_failure, 0) != 0;
}

static float
yttrium_absf(float value)
{
   return value < 0.0f ? -value : value;
}

static uint32_t
yttrium_floor_clamp_u32(float value, uint32_t limit)
{
   if (!(value > 0.0f))
      return 0;
   if (value >= (float)limit)
      return limit;
   return (uint32_t)floorf(value);
}

static struct yttrium_resource *
yttrium_get_stream_output_dummy_target(struct pipe_context *ctx)
{
   struct yttrium_context *yctx = yttrium_context(ctx);
   struct pipe_resource templ;

   if (yctx->so_dummy_target)
      return yttrium_resource(yctx->so_dummy_target);

   memset(&templ, 0, sizeof(templ));
   templ.target = PIPE_TEXTURE_2D;
   templ.format = PIPE_FORMAT_R8G8B8A8_UNORM;
   templ.width0 = 1;
   templ.height0 = 1;
   templ.depth0 = 1;
   templ.array_size = 1;
   templ.last_level = 0;
   templ.nr_samples = 1;
   templ.nr_storage_samples = 1;
   templ.usage = PIPE_USAGE_DEFAULT;
   templ.bind = PIPE_BIND_RENDER_TARGET;

   yctx->so_dummy_target = ctx->screen->resource_create(ctx->screen, &templ);
   if (!yctx->so_dummy_target) {
      YTTRIUM_WARN("yttrium: stream-output dummy render target allocation failed\n");
      return NULL;
   }

   return yttrium_resource(yctx->so_dummy_target);
}

static bool
yttrium_has_live_stream_output_target(const struct yttrium_context *yctx)
{
   for (unsigned i = 0; i < yctx->num_so_targets; i++) {
      if (yctx->so_targets[i])
         return true;
   }

   return false;
}

static struct yttrium_resource *
yttrium_get_uav_only_dummy_target(struct pipe_context *ctx,
                                  unsigned width,
                                  unsigned height,
                                  unsigned samples)
{
   struct yttrium_context *yctx = yttrium_context(ctx);
   struct pipe_resource templ;

   width = MAX2(width, 1u);
   height = MAX2(height, 1u);
   samples = MAX2(samples, 1u);

   unsigned count = 0;
   uint64_t cached_size = 0;
   for (unsigned i = 0; i < ARRAY_SIZE(yctx->uav_only_dummy_targets); i++) {
      struct pipe_resource *target = yctx->uav_only_dummy_targets[i];
      if (!target)
         break;
      if (target->width0 == width && target->height0 == height &&
          MAX2(target->nr_samples, 1u) == samples) {
         /* Move references, without dropping ownership of the hit. */
         for (unsigned j = i; j > 0; j--)
            yctx->uav_only_dummy_targets[j] =
               yctx->uav_only_dummy_targets[j - 1];
         yctx->uav_only_dummy_targets[0] = target;
         return yttrium_resource(target);
      }
      cached_size += yttrium_resource(target)->venus.allocation_size;
      count++;
   }

   memset(&templ, 0, sizeof(templ));
   templ.target = PIPE_TEXTURE_2D;
   templ.format = PIPE_FORMAT_R8G8B8A8_UNORM;
   templ.width0 = width;
   templ.height0 = height;
   templ.depth0 = 1;
   templ.array_size = 1;
   templ.last_level = 0;
   templ.nr_samples = samples;
   templ.nr_storage_samples = samples;
   templ.usage = PIPE_USAGE_DEFAULT;
   templ.bind = PIPE_BIND_RENDER_TARGET;

   struct pipe_resource *target =
      ctx->screen->resource_create(ctx->screen, &templ);
   if (!target) {
      YTTRIUM_WARN("yttrium: uav-only dummy render target allocation failed width=%u height=%u samples=%u\n",
                   width, height, samples);
      return NULL;
   }

   /* Alternating UAV raster sizes must not allocate a new image on every
    * pass: its image ID is also part of the graphics-pipeline cache key.
    * These are write-masked scratch attachments, so ordered draws can reuse
    * them without preserving any application-visible contents.
    *
    * Bound this cache's retained allocation sizes to 64 MiB, except for a
    * single larger target required by the current draw. Eviction drops only
    * our reference; cached pipelines and in-flight batches keep theirs.
    */
   const uint64_t target_size = yttrium_resource(target)->venus.allocation_size;
   const uint64_t cache_budget = 64ull * 1024 * 1024;
   while (count && (count == ARRAY_SIZE(yctx->uav_only_dummy_targets) ||
                    cached_size + target_size > cache_budget)) {
      count--;
      cached_size -=
         yttrium_resource(yctx->uav_only_dummy_targets[count])->venus.allocation_size;
      pipe_resource_reference(&yctx->uav_only_dummy_targets[count], NULL);
   }
   for (unsigned i = count; i > 0; i--)
      yctx->uav_only_dummy_targets[i] = yctx->uav_only_dummy_targets[i - 1];
   yctx->uav_only_dummy_targets[0] = target;
   return yttrium_resource(target);
}

static VkPrimitiveTopology
yttrium_pipe_topology(unsigned mode)
{
   switch (mode) {
   case MESA_PRIM_POINTS:
      return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
   case MESA_PRIM_LINES:
      return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
   case MESA_PRIM_LINE_STRIP:
      return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
   case MESA_PRIM_LINES_ADJACENCY:
      return VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY;
   case MESA_PRIM_LINE_STRIP_ADJACENCY:
      return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY;
   case MESA_PRIM_TRIANGLES:
      return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
   case MESA_PRIM_PATCHES:
      return VK_PRIMITIVE_TOPOLOGY_PATCH_LIST;
   case MESA_PRIM_TRIANGLE_STRIP:
      return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
   case MESA_PRIM_TRIANGLES_ADJACENCY:
      return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY;
   case MESA_PRIM_TRIANGLE_STRIP_ADJACENCY:
      return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY;
   case MESA_PRIM_TRIANGLE_FAN:
      return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
   default:
      YTTRIUM_WARN("yttrium: unsupported primitive mode %u, using triangle list\n",
                   mode);
      return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
   }
}

static VkCompareOp
yttrium_pipe_compare_op(enum pipe_compare_func func)
{
   switch (func) {
   case PIPE_FUNC_NEVER:
      return VK_COMPARE_OP_NEVER;
   case PIPE_FUNC_LESS:
      return VK_COMPARE_OP_LESS;
   case PIPE_FUNC_EQUAL:
      return VK_COMPARE_OP_EQUAL;
   case PIPE_FUNC_LEQUAL:
      return VK_COMPARE_OP_LESS_OR_EQUAL;
   case PIPE_FUNC_GREATER:
      return VK_COMPARE_OP_GREATER;
   case PIPE_FUNC_NOTEQUAL:
      return VK_COMPARE_OP_NOT_EQUAL;
   case PIPE_FUNC_GEQUAL:
      return VK_COMPARE_OP_GREATER_OR_EQUAL;
   case PIPE_FUNC_ALWAYS:
      return VK_COMPARE_OP_ALWAYS;
   default:
      YTTRIUM_WARN("yttrium: unsupported depth compare func %u, using ALWAYS\n",
                  func);
      return VK_COMPARE_OP_ALWAYS;
   }
}

static VkStencilOp
yttrium_pipe_stencil_op(enum pipe_stencil_op op)
{
   switch (op) {
   case PIPE_STENCIL_OP_KEEP:
      return VK_STENCIL_OP_KEEP;
   case PIPE_STENCIL_OP_ZERO:
      return VK_STENCIL_OP_ZERO;
   case PIPE_STENCIL_OP_REPLACE:
      return VK_STENCIL_OP_REPLACE;
   case PIPE_STENCIL_OP_INCR:
      return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
   case PIPE_STENCIL_OP_DECR:
      return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
   case PIPE_STENCIL_OP_INCR_WRAP:
      return VK_STENCIL_OP_INCREMENT_AND_WRAP;
   case PIPE_STENCIL_OP_DECR_WRAP:
      return VK_STENCIL_OP_DECREMENT_AND_WRAP;
   case PIPE_STENCIL_OP_INVERT:
      return VK_STENCIL_OP_INVERT;
   default:
      YTTRIUM_WARN("yttrium: unsupported stencil op %u, using KEEP\n", op);
      return VK_STENCIL_OP_KEEP;
   }
}

static VkStencilOpState
yttrium_pipe_stencil_state(const struct pipe_stencil_state *state,
                           uint32_t reference)
{
   if (!state) {
      return (VkStencilOpState) {
         .failOp = VK_STENCIL_OP_KEEP,
         .passOp = VK_STENCIL_OP_KEEP,
         .depthFailOp = VK_STENCIL_OP_KEEP,
         .compareOp = VK_COMPARE_OP_ALWAYS,
         .compareMask = 0xff,
         .writeMask = 0xff,
         .reference = reference,
      };
   }

   return (VkStencilOpState) {
      .failOp = yttrium_pipe_stencil_op(state->fail_op),
      .passOp = yttrium_pipe_stencil_op(state->zpass_op),
      .depthFailOp = yttrium_pipe_stencil_op(state->zfail_op),
      .compareOp = yttrium_pipe_compare_op(state->func),
      .compareMask = state->valuemask,
      .writeMask = state->writemask,
      .reference = reference,
   };
}

static bool
yttrium_topology_supports_primitive_restart(VkPrimitiveTopology topology)
{
   switch (topology) {
   case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP:
   case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY:
   case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP:
   case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY:
   case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN:
      return true;
   default:
      return false;
   }
}

static unsigned
yttrium_surface_sample_count(const struct pipe_surface *surface)
{
   if (!surface || !surface->texture)
      return 0;

   return surface->nr_samples ? surface->nr_samples :
      MAX2(surface->texture->nr_samples, 1u);
}

static unsigned
yttrium_surface_forced_sample_count(const struct pipe_surface *surface)
{
   if (!surface || !surface->texture || surface->texture->nr_samples > 1)
      return 0;

   return surface->nr_samples > 1 ? surface->nr_samples : 0;
}

static unsigned
yttrium_framebuffer_sample_count(const struct yttrium_context *yctx)
{
   unsigned samples = 0;

   for (uint32_t i = 0; i < yctx->fb.nr_cbufs; i++) {
      samples = yttrium_surface_sample_count(&yctx->fb.cbufs[i]);
      if (samples)
         return samples;
   }

   samples = yttrium_surface_sample_count(&yctx->fb.zsbuf);
   if (samples)
      return samples;

   return MAX2(yctx->fb.samples, 1u);
}

static unsigned
yttrium_framebuffer_forced_sample_count(const struct yttrium_context *yctx)
{
   unsigned samples = 0;

   for (uint32_t i = 0; i < yctx->fb.nr_cbufs; i++) {
      samples = yttrium_surface_forced_sample_count(&yctx->fb.cbufs[i]);
      if (samples)
         return samples;
   }

   return yttrium_surface_forced_sample_count(&yctx->fb.zsbuf);
}

static void
yttrium_make_venus_draw_state(struct yttrium_context *yctx,
                              const struct yttrium_resource *res,
                              VkPrimitiveTopology topology,
                              struct yttrium_venus_draw_state *state)
{
   struct yttrium_screen *screen = yttrium_screen(yctx->base.screen);

   memset(state, 0, sizeof(*state));

   state->topology = topology;
   state->cull_mode = VK_CULL_MODE_NONE;
   state->front_face = VK_FRONT_FACE_COUNTER_CLOCKWISE;
   state->blend_enable = VK_FALSE;
   state->logic_op_enable = VK_FALSE;
   state->logic_op = VK_LOGIC_OP_COPY;
   state->sample_mask = yctx->sample_mask;
   state->rasterization_samples = yttrium_framebuffer_sample_count(yctx);
   state->forced_sample_count = yttrium_framebuffer_forced_sample_count(yctx);
   state->color_write_mask = VK_COLOR_COMPONENT_R_BIT |
                             VK_COLOR_COMPONENT_G_BIT |
                             VK_COLOR_COMPONENT_B_BIT |
                             VK_COLOR_COMPONENT_A_BIT;
   state->src_color_blend_factor = VK_BLEND_FACTOR_ONE;
   state->dst_color_blend_factor = VK_BLEND_FACTOR_ZERO;
   state->color_blend_op = VK_BLEND_OP_ADD;
   state->src_alpha_blend_factor = VK_BLEND_FACTOR_ONE;
   state->dst_alpha_blend_factor = VK_BLEND_FACTOR_ZERO;
   state->alpha_blend_op = VK_BLEND_OP_ADD;
   state->rt_count = yctx->fb.nr_cbufs;
   for (uint32_t i = 0; i < PIPE_MAX_COLOR_BUFS; i++) {
      state->rt_blend_enable[i] = state->blend_enable;
      state->rt_color_write_mask[i] = state->color_write_mask;
      state->rt_src_color_blend_factor[i] = state->src_color_blend_factor;
      state->rt_dst_color_blend_factor[i] = state->dst_color_blend_factor;
      state->rt_color_blend_op[i] = state->color_blend_op;
      state->rt_src_alpha_blend_factor[i] = state->src_alpha_blend_factor;
      state->rt_dst_alpha_blend_factor[i] = state->dst_alpha_blend_factor;
      state->rt_alpha_blend_op[i] = state->alpha_blend_op;
   }
   state->depth_bias_format = yctx->fb.zsbuf.texture ?
      yctx->fb.zsbuf.format : PIPE_FORMAT_NONE;
   state->depth_test_enable = VK_FALSE;
   state->depth_write_enable = VK_FALSE;
   state->depth_compare_op = VK_COMPARE_OP_ALWAYS;
   state->stencil_test_enable = VK_FALSE;
   state->stencil_front = yttrium_pipe_stencil_state(NULL, 0);
   state->stencil_back = yttrium_pipe_stencil_state(NULL, 0);
   memcpy(state->blend_constants, yctx->blend_color.color,
          sizeof(state->blend_constants));

   if (yctx->rasterizer) {
      const bool line_topology =
         topology == VK_PRIMITIVE_TOPOLOGY_LINE_LIST ||
         topology == VK_PRIMITIVE_TOPOLOGY_LINE_STRIP ||
         topology == VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY ||
         topology == VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY;

      if (line_topology && yctx->rasterizer->state.line_last_pixel) {
         static volatile LONG warned;

         if (InterlockedCompareExchange(&warned, 1, 0) == 0) {
            YTTRIUM_WARN(
               "yttrium: WARNING: line endpoint rasterization fallback "
               "owner=venus2-raster "
               "reason=line-last-pixel-not-represented-in-Vulkan-pipeline "
               "fallback=host-default-line-rasterization topology=%u "
               "samples=%u\n",
               topology, state->rasterization_samples);
         }
      }

      state->rasterizer_discard_enable =
         yctx->rasterizer->state.rasterizer_discard ? VK_TRUE : VK_FALSE;
      switch (yctx->rasterizer->state.cull_face) {
      case PIPE_FACE_FRONT:
         state->cull_mode = VK_CULL_MODE_FRONT_BIT;
         break;
      case PIPE_FACE_BACK:
         state->cull_mode = VK_CULL_MODE_BACK_BIT;
         break;
      case PIPE_FACE_FRONT_AND_BACK:
         state->cull_mode = VK_CULL_MODE_FRONT_AND_BACK;
         break;
      default:
         state->cull_mode = VK_CULL_MODE_NONE;
         break;
      }
      state->front_face = yctx->rasterizer->state.front_ccw ?
         VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
      state->depth_bias_constant_factor =
         yctx->rasterizer->state.offset_units;
      state->depth_bias_units_unscaled =
         yctx->rasterizer->state.offset_units_unscaled ? VK_TRUE : VK_FALSE;
      state->depth_bias_clamp = yctx->rasterizer->state.offset_clamp;
      state->depth_bias_slope_factor =
         yctx->rasterizer->state.offset_scale;
      state->depth_bias_enable =
         (state->depth_bias_constant_factor != 0.0f ||
          state->depth_bias_clamp != 0.0f ||
          state->depth_bias_slope_factor != 0.0f) ? VK_TRUE : VK_FALSE;
      state->depth_clamp_enable =
         (!yctx->rasterizer->state.depth_clip_near ||
          !yctx->rasterizer->state.depth_clip_far) &&
         yttrium_venus_depth_clamp_enabled(screen->venus) ? VK_TRUE :
         VK_FALSE;
   }

   if (yctx->blend) {
      const struct pipe_blend_state *blend = &yctx->blend->state;
      state->logic_op_enable =
         blend->logicop_enable &&
         yttrium_venus_logic_op_enabled(screen->venus) ? VK_TRUE : VK_FALSE;
      state->logic_op = yctx->blend->logic_op;
      state->alpha_to_coverage_enable =
         blend->alpha_to_coverage ? VK_TRUE : VK_FALSE;

      memcpy(state->rt_blend_enable, yctx->blend->rt_blend_enable,
             sizeof(state->rt_blend_enable));
      memcpy(state->rt_color_write_mask, yctx->blend->rt_color_write_mask,
             sizeof(state->rt_color_write_mask));
      memcpy(state->rt_src_color_blend_factor, yctx->blend->rt_src_color_blend_factor,
             sizeof(state->rt_src_color_blend_factor));
      memcpy(state->rt_dst_color_blend_factor, yctx->blend->rt_dst_color_blend_factor,
             sizeof(state->rt_dst_color_blend_factor));
      memcpy(state->rt_color_blend_op, yctx->blend->rt_color_blend_op,
             sizeof(state->rt_color_blend_op));
      memcpy(state->rt_src_alpha_blend_factor, yctx->blend->rt_src_alpha_blend_factor,
             sizeof(state->rt_src_alpha_blend_factor));
      memcpy(state->rt_dst_alpha_blend_factor, yctx->blend->rt_dst_alpha_blend_factor,
             sizeof(state->rt_dst_alpha_blend_factor));
      memcpy(state->rt_alpha_blend_op, yctx->blend->rt_alpha_blend_op,
             sizeof(state->rt_alpha_blend_op));

      state->blend_enable = state->rt_blend_enable[0];
      state->color_write_mask = state->rt_color_write_mask[0];
      state->src_color_blend_factor = state->rt_src_color_blend_factor[0];
      state->dst_color_blend_factor = state->rt_dst_color_blend_factor[0];
      state->color_blend_op = state->rt_color_blend_op[0];
      state->src_alpha_blend_factor = state->rt_src_alpha_blend_factor[0];
      state->dst_alpha_blend_factor = state->rt_dst_alpha_blend_factor[0];
      state->alpha_blend_op = state->rt_alpha_blend_op[0];
   }

   if (yctx->dsa) {
      const struct pipe_depth_stencil_alpha_state *dsa = &yctx->dsa->state;
      state->depth_test_enable = dsa->depth_enabled ? VK_TRUE : VK_FALSE;
      state->depth_write_enable =
         dsa->depth_enabled && dsa->depth_writemask ? VK_TRUE : VK_FALSE;
      state->depth_compare_op = yttrium_pipe_compare_op(dsa->depth_func);
      state->alpha_test_enable =
         dsa->alpha_enabled && dsa->alpha_func != PIPE_FUNC_ALWAYS ?
         VK_TRUE : VK_FALSE;
      state->alpha_func = dsa->alpha_func;
      state->alpha_ref_value = dsa->alpha_ref_value;

      if (dsa->stencil[0].enabled || dsa->stencil[1].enabled) {
         const struct pipe_stencil_state *front = &dsa->stencil[0];
         const struct pipe_stencil_state *back =
            dsa->stencil[1].enabled ? &dsa->stencil[1] :
                                      &dsa->stencil[0];
         state->stencil_test_enable = VK_TRUE;
         state->stencil_front = yttrium_pipe_stencil_state(
            front, yctx->stencil_ref.ref_value[0]);
         state->stencil_back = yttrium_pipe_stencil_state(
            back, yctx->stencil_ref.ref_value[1]);
      }
   }

   state->viewport_count = 1;
   state->viewports[0].x = 0.0f;
   state->viewports[0].y = 0.0f;
   state->render_level = 0;
   state->render_layer = 0;
   state->render_layers = 1;
   state->render_width = res->base.width0;
   state->render_height = res->base.height0;
   memset(state->rt_level, 0, sizeof(state->rt_level));
   memset(state->rt_layer, 0, sizeof(state->rt_layer));

   /* Each target keeps its own mip and slice.  The loop below still takes the
    * extent and the layer count from the first one, which all attachments
    * share. */
   for (uint32_t i = 0;
        i < yctx->fb.nr_cbufs && i < ARRAY_SIZE(state->rt_level); i++) {
      if (!yctx->fb.cbufs[i].texture)
         continue;
      state->rt_level[i] = yctx->fb.cbufs[i].level;
      state->rt_layer[i] = yctx->fb.cbufs[i].first_layer;
   }

   for (uint32_t i = 0; i < yctx->fb.nr_cbufs; i++) {
      if (!yctx->fb.cbufs[i].texture)
         continue;

      const struct pipe_surface *cbuf = &yctx->fb.cbufs[i];
      state->render_level = cbuf->level;
      state->render_layer = cbuf->first_layer;
      state->render_layers =
         cbuf->last_layer >= cbuf->first_layer ?
         cbuf->last_layer - cbuf->first_layer + 1 : 1;
      state->render_width = u_minify(res->base.width0, cbuf->level);
      state->render_height = u_minify(res->base.height0, cbuf->level);
      break;
   }
   if (yctx->fb.zsbuf.texture) {
      state->depth_level = yctx->fb.zsbuf.level;
      state->depth_layer = yctx->fb.zsbuf.first_layer;
      state->depth_layers =
         yctx->fb.zsbuf.last_layer >= yctx->fb.zsbuf.first_layer ?
         yctx->fb.zsbuf.last_layer - yctx->fb.zsbuf.first_layer + 1 : 1;
      if (!yctx->fb.nr_cbufs)
         state->render_layers = state->depth_layers;
   }
   if (!yctx->fb.nr_cbufs && yctx->fb.width && yctx->fb.height) {
      state->render_width = yctx->fb.width;
      state->render_height = yctx->fb.height;
   }
   state->viewports[0].width = (float)state->render_width;
   state->viewports[0].height = (float)state->render_height;
   state->viewports[0].minDepth = 0.0f;
   state->viewports[0].maxDepth = 1.0f;

   state->scissors[0].offset.x = 0;
   state->scissors[0].offset.y = 0;
   state->scissors[0].extent.width = state->render_width;
   state->scissors[0].extent.height = state->render_height;

   const bool scissor_enabled =
      yctx->rasterizer && yctx->rasterizer->state.scissor;

   if (!yctx->num_viewports) {
      state->viewports[0].width = 1.0f;
      state->viewports[0].height = 1.0f;
      state->scissors[0].extent.width = 0;
      state->scissors[0].extent.height = 0;
   } else {
      state->viewport_count = MIN2(yctx->num_viewports,
                                   (unsigned)PIPE_MAX_VIEWPORTS);
      for (unsigned i = 0; i < state->viewport_count; i++) {
         const struct pipe_viewport_state *vp = &yctx->viewports[i];
         const float half_width = yttrium_absf(vp->scale[0]);
         const float signed_half_height = vp->scale[1];
         const float half_height = yttrium_absf(signed_half_height);

         state->viewports[i].x = 0.0f;
         state->viewports[i].y = 0.0f;
         state->viewports[i].width = (float)state->render_width;
         state->viewports[i].height = (float)state->render_height;
         state->viewports[i].minDepth = 0.0f;
         state->viewports[i].maxDepth = 1.0f;
         if (half_width > 0.0f && half_height > 0.0f) {
            state->viewports[i].x = vp->translate[0] - half_width;
            state->viewports[i].y = vp->translate[1] - signed_half_height;
            state->viewports[i].width = half_width * 2.0f;
            state->viewports[i].height = signed_half_height * 2.0f;
            state->viewports[i].minDepth = vp->translate[2];
            state->viewports[i].maxDepth =
               vp->translate[2] + vp->scale[2];
         } else {
            state->viewports[i].width = 1.0f;
            state->viewports[i].height = 1.0f;
         }

         /* Vulkan viewports do not clip; D3D viewport bounds do. */
         const float viewport_x0 =
            MIN2(state->viewports[i].x,
                 state->viewports[i].x + state->viewports[i].width);
         const float viewport_x1 =
            MAX2(state->viewports[i].x,
                 state->viewports[i].x + state->viewports[i].width);
         const float viewport_y0 =
            MIN2(state->viewports[i].y,
                 state->viewports[i].y + state->viewports[i].height);
         const float viewport_y1 =
            MAX2(state->viewports[i].y,
                 state->viewports[i].y + state->viewports[i].height);
         uint32_t minx =
            yttrium_floor_clamp_u32(viewport_x0, state->render_width);
         uint32_t miny =
            yttrium_floor_clamp_u32(viewport_y0, state->render_height);
         uint32_t maxx =
            yttrium_floor_clamp_u32(viewport_x1, state->render_width);
         uint32_t maxy =
            yttrium_floor_clamp_u32(viewport_y1, state->render_height);

         if (scissor_enabled) {
            if (i < yctx->num_scissors) {
               const struct pipe_scissor_state *scissor = &yctx->scissors[i];
               const uint32_t scissor_minx =
                  MIN2(scissor->minx, state->render_width);
               const uint32_t scissor_miny =
                  MIN2(scissor->miny, state->render_height);
               const uint32_t scissor_maxx =
                  MIN2(scissor->maxx, state->render_width);
               const uint32_t scissor_maxy =
                  MIN2(scissor->maxy, state->render_height);

               minx = MAX2(minx, MIN2(scissor_minx, scissor_maxx));
               miny = MAX2(miny, MIN2(scissor_miny, scissor_maxy));
               maxx = MIN2(maxx, MAX2(scissor_minx, scissor_maxx));
               maxy = MIN2(maxy, MAX2(scissor_miny, scissor_maxy));
            } else {
               maxx = minx;
               maxy = miny;
            }
         }

         state->scissors[i].offset.x = (int32_t)MIN2(minx, maxx);
         state->scissors[i].offset.y = (int32_t)MIN2(miny, maxy);
         state->scissors[i].extent.width = maxx > minx ? maxx - minx : 0;
         state->scissors[i].extent.height = maxy > miny ? maxy - miny : 0;

         if (half_width <= 0.0f || half_height <= 0.0f) {
            state->scissors[i].extent.width = 0;
            state->scissors[i].extent.height = 0;
         }
      }
   }

}

static bool
yttrium_resource_is_venus_depth_attachment(const struct yttrium_resource *res)
{
   return res && res->venus.initialized && !res->venus.buffer_backed &&
          res->venus.image &&
          (res->venus.image_usage &
           VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0;
}

static bool
yttrium_resource_is_venus_storage_image(const struct yttrium_resource *res)
{
   return res && res->venus.initialized && !res->venus.buffer_backed &&
          res->venus.image &&
          (res->venus.image_usage & VK_IMAGE_USAGE_STORAGE_BIT) != 0;
}

static struct yttrium_resource *
yttrium_first_fragment_shader_image(const struct yttrium_context *yctx)
{
   if (!yctx)
      return NULL;

   for (unsigned i = 0; i < yctx->num_shader_images[MESA_SHADER_FRAGMENT];
        i++) {
      const struct pipe_image_view *view =
         &yctx->shader_images[MESA_SHADER_FRAGMENT][i];
      if (view->resource)
         return yttrium_resource(view->resource);
   }

   return NULL;
}

static unsigned
yttrium_count_sampler_states(const struct yttrium_context *yctx,
                             mesa_shader_stage stage)
{
   unsigned count = 0;

   if (stage >= MESA_SHADER_STAGES)
      return 0;

   for (unsigned i = 0; i < PIPE_MAX_SAMPLERS; i++) {
      if (yctx->sampler_states[stage][i])
         count++;
   }

   return count;
}

static unsigned
yttrium_count_sampler_views(const struct yttrium_context *yctx,
                            mesa_shader_stage stage)
{
   unsigned count = 0;

   if (stage >= MESA_SHADER_STAGES)
      return 0;

   for (unsigned i = 0; i < PIPE_MAX_SHADER_SAMPLER_VIEWS; i++) {
      if (yctx->sampler_views[stage][i])
         count++;
   }

   return count;
}

/* A dropped draw renders nothing, which reaches the screen as missing geometry,
 * so every drop path has to say so at warning level.  Rate-limited because a
 * draw dropped once per frame would otherwise flood the log.
 */
static bool
yttrium_draw_should_warn_drop(uint32_t *counter)
{
   const uint32_t count = ++(*counter);

   return count <= 8 || (count % 512) == 0;
}

void
yttrium_draw_vbo(struct pipe_context *ctx,
                 const struct pipe_draw_info *info,
                 unsigned drawid_offset,
                 const struct pipe_draw_indirect_info *indirect,
                 const struct pipe_draw_start_count_bias *draws,
                 unsigned num_draws)
{
   struct yttrium_context *yctx = yttrium_context(ctx);
   struct pipe_surface *cbuf = NULL;
   struct yttrium_resource *res = NULL;

   /* The threaded indirect packet only initializes start.  Do not inspect
    * direct-only count/bias fields in diagnostics or fallback probes. */
   struct pipe_draw_start_count_bias indirect_draw = { 0 };
   if (indirect) {
      indirect_draw.start = draws ? draws[0].start : 0;
      draws = &indirect_draw;
   }

   for (uint32_t i = 0; i < yctx->fb.nr_cbufs; i++) {
      if (!yctx->fb.cbufs[i].texture)
         continue;

      cbuf = &yctx->fb.cbufs[i];
      res = yttrium_resource(cbuf->texture);
      break;
   }
   struct yttrium_resource *zs_res =
      yctx->fb.zsbuf.texture ? yttrium_resource(yctx->fb.zsbuf.texture) :
      NULL;
   /*
    * The fixed 1x1 stream-output dummy gives the draw path a resource to own
    * and track when no framebuffer attachment exists.  When at least one SO
    * target is live it is only an anchor: targetless_stream_output below
    * keeps it out of the Vulkan render pass.  Preserve the old attachment
    * route for an all-NULL SO binding with no fragment image, which may still
    * carry query work.
    * A depth buffer is already a usable anchor and attachment, so prefer it.
    *
    * Superposition streams with a depth buffer bound and no colour target,
    * which failed every such pipeline - 169327 of them, all with a zs, all
    * rejected as "color rt0 ... extent=1x1 ... render=512x512" and larger.
    * With zs preferred, res == zs_res selects the depth-only path below,
    * which attaches no colour target at all.
    */
   const bool has_live_so_target =
      !res && !zs_res && yttrium_has_live_stream_output_target(yctx);
   struct yttrium_resource *fragment_image =
      !res && !zs_res && !has_live_so_target ?
      yttrium_first_fragment_shader_image(yctx) : NULL;
   /* State restoration may rebind all SO slots as NULL.  Those slots do not
    * own the draw and must not hide a fragment UAV behind the fixed 1x1 SO
    * dummy.  Keep the existing no-UAV query route unchanged. */
   const bool stream_output_requested =
      !res && !zs_res && yctx->num_so_targets &&
      (has_live_so_target || !fragment_image);
   const bool targetless_stream_output =
      stream_output_requested && has_live_so_target;
   bool uav_only_dummy_target = false;
   unsigned uav_only_dummy_samples = 1;

   const struct yttrium_shader_state *vs = yctx->shaders[MESA_SHADER_VERTEX];
   const struct yttrium_shader_state *fs = yctx->shaders[MESA_SHADER_FRAGMENT];
   if (yttrium_trace_is_enabled()) {
      yttrium_trace_draw_vbo(info ? info->mode : 0,
                             num_draws,
                             draws && num_draws ? draws[0].count : 0,
                             info ? info->instance_count : 0,
                             info ? info->start_instance : 0,
                             cbuf,
                             false,
                             0,
                             vs ? vs->info.num_inputs : 0,
                             vs ? vs->info.num_outputs : 0,
                             fs ? fs->info.num_inputs : 0,
                             fs ? fs->info.num_outputs : 0,
                             yttrium_count_sampler_views(
                                yctx, MESA_SHADER_FRAGMENT),
                             yttrium_count_sampler_states(
                                yctx, MESA_SHADER_FRAGMENT));
   }
   if (stream_output_requested)
      res = yttrium_get_stream_output_dummy_target(ctx);
   else if (!res && zs_res)
      res = zs_res;
   else if (!res)
      res = fragment_image;

   /* A fragment image selected above is a UAV anchor, not a bound color
    * target, even when the resource was created with render-target support. */
   bool color_target =
      res && res != fragment_image &&
      (res->display_target ||
       yttrium_resource_is_venus_color_attachment(res));
   bool depth_target =
      res && res == zs_res &&
      yttrium_resource_is_venus_depth_attachment(res);
   bool storage_image_target =
      res && !color_target && !depth_target &&
      yttrium_resource_is_venus_storage_image(res);
   bool storage_buffer_target =
      res && !color_target && !depth_target &&
      res->base.target == PIPE_BUFFER;

   if (!color_target && !depth_target &&
       (storage_image_target || storage_buffer_target) &&
       !yctx->fb.nr_cbufs && !yctx->fb.zsbuf.texture &&
       yctx->fb.width && yctx->fb.height) {
      uav_only_dummy_samples = MAX2(yctx->fb.samples, 1u);
      struct yttrium_resource *dummy =
         yttrium_get_uav_only_dummy_target(ctx, yctx->fb.width,
                                           yctx->fb.height,
                                           uav_only_dummy_samples);
      if (!dummy) {
         static uint32_t drops;

         if (yttrium_draw_should_warn_drop(&drops))
            YTTRIUM_WARN("yttrium: draw_vbo DROPPED draw, nothing rendered %s pid=%lu reason=uav_only_dummy_target_unavailable drops=%u fb=%ux%u samples=%u mode=%u num_draws=%u\n",
                         yttrium_trace_process_name(), (unsigned long)GetCurrentProcessId(),
                         drops, yctx->fb.width, yctx->fb.height,
                         uav_only_dummy_samples, info ? info->mode : 0,
                         num_draws);
         yttrium_record_draw_failure(yctx);
         return;
      }

      res = dummy;
      color_target = true;
      depth_target = false;
      storage_image_target = false;
      storage_buffer_target = false;
      uav_only_dummy_target = true;
   }

   if (!res) {
      static uint32_t drops;

      if (yttrium_draw_should_warn_drop(&drops))
         YTTRIUM_WARN("yttrium: draw_vbo DROPPED draw, nothing rendered %s pid=%lu reason=no_target_resource drops=%u nr_cbufs=%u zsbuf=%u so_targets=%u fb=%ux%u mode=%u num_draws=%u\n",
                      yttrium_trace_process_name(), (unsigned long)GetCurrentProcessId(),
                      drops, yctx->fb.nr_cbufs,
                      yctx->fb.zsbuf.texture ? 1u : 0u,
                      yctx->num_so_targets, yctx->fb.width, yctx->fb.height,
                      info ? info->mode : 0, num_draws);
      yttrium_record_draw_queries(ctx, info, indirect, draws, num_draws);
      if (targetless_stream_output)
         yttrium_record_draw_failure(yctx);
      return;
   }
   if (!color_target && !depth_target && !storage_image_target &&
       !storage_buffer_target) {
      static uint32_t drops;

      if (yttrium_draw_should_warn_drop(&drops))
         YTTRIUM_WARN("yttrium: draw_vbo DROPPED draw, nothing rendered %s pid=%lu reason=target_not_usable_attachment drops=%u res_id=%u classic=%u buffer_backed=%u display_target=%u target=%u format=%u %ux%u zs=%u mode=%u num_draws=%u\n",
                      yttrium_trace_process_name(), (unsigned long)GetCurrentProcessId(),
                      drops, res->venus_res_id, res->classic_display,
                      res->venus.buffer_backed, res->display_target,
                      res->base.target, res->venus.vk_format,
                      res->venus.width, res->venus.height,
                      res == zs_res, info ? info->mode : 0, num_draws);
      yttrium_record_draw_failure(yctx);
      return;
   }

   struct yttrium_venus_draw_state draw_state;
   const VkPrimitiveTopology topology =
      info ? yttrium_pipe_topology(info->mode) :
             VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
   yttrium_make_venus_draw_state(yctx, res, topology, &draw_state);
   draw_state.targetless_stream_output =
      targetless_stream_output ? VK_TRUE : VK_FALSE;
   if (uav_only_dummy_target) {
      draw_state.rasterization_samples = uav_only_dummy_samples;
      draw_state.forced_sample_count = 0;
      draw_state.alpha_to_coverage_enable = VK_FALSE;
      draw_state.blend_enable = VK_FALSE;
      draw_state.logic_op_enable = VK_FALSE;
      draw_state.color_write_mask = 0;
      for (uint32_t i = 0; i < PIPE_MAX_COLOR_BUFS; i++) {
         draw_state.rt_blend_enable[i] = VK_FALSE;
         draw_state.rt_color_write_mask[i] = 0;
      }
   }
   draw_state.primitive_restart_enable =
      info && info->primitive_restart &&
      yttrium_topology_supports_primitive_restart(topology) ? VK_TRUE :
      VK_FALSE;

   if (yttrium_shader_pipeline_draw_enabled()) {
      const struct yttrium_shader_state *vs =
         yctx->shaders[MESA_SHADER_VERTEX];
      const struct yttrium_shader_state *fs_probe =
         yctx->shaders[MESA_SHADER_FRAGMENT];
      yttrium_trace_debug_stringf(
         "yttrium: shader_draw_probe gate enabled res_id=%u classic=%u mode=%u restart=%u num_draws=%u vs=%p vs_module=%u fs=%p fs_module=%u",
         res->venus_res_id, res->classic_display, info ? info->mode : 0,
         draw_state.primitive_restart_enable, num_draws,
         vs, vs ? yttrium_shader_state_has_module(vs) : 0,
         fs_probe, fs_probe ? yttrium_shader_state_has_module(fs_probe) : 0);
      if (!res->classic_display) {
         enum yttrium_pipeline_draw_result draw_result =
            yttrium_pipeline_try_draw(ctx, res, info, indirect, draws,
                                      num_draws, &draw_state);
         if (draw_result == YTTRIUM_PIPELINE_DRAW_EMITTED) {
            yttrium_record_draw_queries(ctx, info, indirect, draws, num_draws);
            yttrium_trace_debug_stringf(
               "yttrium: shader_draw_probe gate native draw accepted res_id=%u",
               res->venus_res_id);
            return;
         }
         if (draw_result == YTTRIUM_PIPELINE_DRAW_EMIT_FAILED) {
            static uint32_t drops;

            if (yttrium_draw_should_warn_drop(&drops))
               YTTRIUM_WARN("yttrium: shader_draw_probe native draw emit failed; no fallback taken drops=%u hAllocation=0x%lx res_id=%u classic=%u scanout_buffer=%u\n",
                            drops,
                            (unsigned long)res->hAllocation,
                            res->venus_res_id,
                            res->classic_display,
                            res->venus.buffer_backed);
            yttrium_record_draw_failure(yctx);
            return;
         }
      }
      yttrium_trace_debug_stringf(
         "yttrium: shader_draw_probe gate unsupported res_id=%u classic=%u",
         res->venus_res_id, res->classic_display);
   }

   yttrium_trace_draw_skip(YTTRIUM_TRACE_DRAW_SKIP_IGNORED,
                           res->hAllocation, res->venus_res_id,
                           res->classic_display, res->venus.buffer_backed,
                           0);
   {
      static uint32_t drops;

      if (yttrium_draw_should_warn_drop(&drops))
         YTTRIUM_WARN("yttrium: draw_vbo DROPPED draw, nothing rendered %s pid=%lu reason=native_draw_unsupported drops=%u hAllocation=0x%lx res_id=%u classic=%u scanout_buffer=%u format=%u %ux%u topology=%u forced_samples=%u samples=%u mode=%u num_draws=%u\n",
                      yttrium_trace_process_name(), (unsigned long)GetCurrentProcessId(),
                      drops, (unsigned long)res->hAllocation,
                      res->venus_res_id, res->classic_display,
                      res->venus.buffer_backed, res->venus.vk_format,
                      res->venus.width, res->venus.height, topology,
                      draw_state.forced_sample_count,
                      draw_state.rasterization_samples,
                      info ? info->mode : 0, num_draws);
      yttrium_record_draw_failure(yctx);
   }
}
