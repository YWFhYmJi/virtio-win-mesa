/*
 * SPDX-FileCopyrightText: 2026 Ake Rehnman <ake.rehnman@gmail.com>
 * SPDX-License-Identifier: MPL-2.0
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "util/u_math.h"
#include "yttrium_trace.h"
#include "yttrium_options.h"
#include "yttrium_venus2_private.h"

static int
yttrium_venus_option_state(const char *name)
{
   const char *value = yttrium_gdi_debug_get_option(name, NULL);

   if (!value)
      return -1;

   return strcmp(value, "0") != 0 &&
          _stricmp(value, "false") != 0 &&
          _stricmp(value, "no") != 0 &&
          _stricmp(value, "off") != 0;
}

static bool
yttrium_venus_option_enabled(const char *name)
{
   return yttrium_venus_option_state(name) > 0;
}

static bool
yttrium_venus_option_enabled_default(const char *name, bool dfault)
{
   const int state = yttrium_venus_option_state(name);

   return state < 0 ? dfault : state != 0;
}

/*
 * Ring size, rounded down to a power of two because the ring indexes with
 * buffer_mask = buffer_size - 1.  A command has to fit whole, so a workload
 * with large shader modules needs this raised; the default is the size the
 * regression suites are green at.  See the ENV define for why.
 */
uint32_t
yttrium_venus_ring_buffer_size(void)
{
   static int initialized;
   static uint32_t size = YTTRIUM_VENUS_RING_BUFFER_SIZE_DEFAULT;

   if (!initialized) {
      const int64_t value = yttrium_gdi_debug_get_num_option(
         YTTRIUM_VENUS_RING_BUFFER_SIZE_ENV,
         YTTRIUM_VENUS_RING_BUFFER_SIZE_DEFAULT);

      if (value >= YTTRIUM_VENUS_RING_BUFFER_SIZE_DEFAULT) {
         uint64_t clamped = MIN2((uint64_t)value,
                                 (uint64_t)YTTRIUM_VENUS_RING_BUFFER_SIZE_MAX);
         uint32_t pot = YTTRIUM_VENUS_RING_BUFFER_SIZE_DEFAULT;

         while ((uint64_t)pot * 2 <= clamped)
            pot *= 2;
         size = pot;
      }
      initialized = 1;
   }

   return size;
}

uint32_t
yttrium_venus_batch_count(void)
{
   static int initialized;
   static uint32_t count = YTTRIUM_VENUS_BATCH_COUNT_DEFAULT;

   if (!initialized) {
      const int64_t value = yttrium_gdi_debug_get_num_option(
         "D3D10UMD_YTTRIUM_BATCH_COUNT", YTTRIUM_VENUS_BATCH_COUNT_DEFAULT);

      if (value > 0) {
         count = (uint32_t)MIN2((uint64_t)value,
                                (uint64_t)YTTRIUM_VENUS_BATCH_COUNT_MAX);
      }
      initialized = 1;
   }

   return count;
}

bool
yttrium_venus_async_batch_enabled(void)
{
   /*
    * Venus2 command-buffer ownership is always asynchronous.  This is no
    * longer an experimental batching policy: disabling it selects the old
    * shared-command-buffer path, which has to wait for GPU completion before
    * every reset and therefore violates the backend ownership contract.
    *
    * Individual batching/coalescing policies can still decide to submit a
    * one-operation batch, but they must never select synchronous ownership.
    */
   return true;
}

bool
yttrium_venus_batch_epoch_wait_enabled(void)
{
   static int enabled = -1;

   if (enabled < 0)
      enabled = yttrium_venus_option_enabled_default(
         "D3D10UMD_YTTRIUM_BATCH_EPOCH_WAIT", true);

   return enabled != 0;
}

bool
yttrium_venus_batch_fence_feedback_enabled(void)
{
   /*
    * Mapped completion feedback is part of the Venus2 asynchronous ownership
    * protocol, not an experiment.  Without it the guest can only discover a
    * reusable batch by issuing a synchronous Venus fence query/wait.
    */
   return true;
}

bool
yttrium_venus_draw_backing_pool_enabled(void)
{
   static int enabled = -1;

   if (enabled < 0)
      enabled = yttrium_venus_option_enabled_default(
         YTTRIUM_VENUS_DRAW_BACKING_POOL_ENV, true);

   return enabled != 0;
}

static bool
yttrium_venus_batch_feature_enabled(const char *name)
{
   int state = yttrium_venus_option_state(name);

   if (state >= 0)
      return state != 0;

   return yttrium_venus_async_batch_enabled();
}

bool
yttrium_venus_native_draw_batch_enabled(void)
{
   static int enabled = -1;

   if (enabled < 0)
      enabled = yttrium_venus_batch_feature_enabled(
         YTTRIUM_VENUS_NATIVE_DRAW_BATCH_ENV);

   return enabled != 0;
}

uint32_t
yttrium_venus_native_draw_batch_limit(void)
{
   static int initialized = 0;
   static uint32_t limit = YTTRIUM_VENUS_NATIVE_DRAW_BATCH_LIMIT_DEFAULT;

   if (!initialized) {
      const char *value = yttrium_gdi_debug_get_option(
         YTTRIUM_VENUS_NATIVE_DRAW_BATCH_LIMIT_ENV, NULL);
      if (value) {
         char *end = NULL;
         unsigned long parsed = strtoul(value, &end, 0);
         if (end && *end == '\0' && parsed > 0) {
            limit = (uint32_t)MIN2(parsed,
                                   YTTRIUM_VENUS_NATIVE_DRAW_BATCH_LIMIT_MAX);
         }
      }
      initialized = 1;
   }

   return limit;
}

static bool
yttrium_venus_cpu_vertex_batch_enabled(void)
{
   static int enabled = -1;

   if (enabled < 0)
      enabled = yttrium_venus_batch_feature_enabled(
         YTTRIUM_VENUS_CPU_VERTEX_BATCH_ENV);

   return enabled != 0;
}

static bool
yttrium_venus_sampled_cpu_vertex_batch_enabled(void)
{
   static int enabled = -1;

   if (enabled < 0)
      enabled = yttrium_venus_batch_feature_enabled(
         YTTRIUM_VENUS_SAMPLED_CPU_VERTEX_BATCH_ENV);

   return enabled != 0;
}

bool
yttrium_venus_sampled_cpu_vertex_render_pass_batch_enabled(void)
{
   static int enabled = -1;

   if (enabled < 0)
      enabled = yttrium_venus_batch_feature_enabled(
         YTTRIUM_VENUS_SAMPLED_CPU_VERTEX_RENDER_PASS_BATCH_ENV);

   return enabled != 0;
}

bool
yttrium_venus_checked_descriptor_alloc_enabled(void)
{
   static int enabled = -1;

   if (enabled < 0)
      enabled = yttrium_venus_option_enabled(
         YTTRIUM_VENUS_CHECKED_DESCRIPTOR_ALLOC_ENV);

   return enabled != 0;
}

bool
yttrium_venus_push_descriptor_batch_enabled(void)
{
   static int enabled = -1;

   if (enabled < 0)
      enabled = yttrium_venus_batch_feature_enabled(
         YTTRIUM_VENUS_PUSH_DESCRIPTOR_BATCH_ENV);

   return enabled != 0;
}

bool
yttrium_venus_push_descriptor_layout_rotation_enabled(void)
{
   static int enabled = -1;

   if (enabled < 0)
      enabled = yttrium_venus_option_enabled_default(
         YTTRIUM_VENUS_PUSH_DESCRIPTOR_LAYOUT_ROTATION_ENV, true);

   return enabled != 0;
}

bool
yttrium_venus_render_pass_batch_enabled(void)
{
   static int enabled = -1;

   if (enabled < 0)
      enabled = yttrium_venus_batch_feature_enabled(
         YTTRIUM_VENUS_RENDER_PASS_BATCH_ENV);

   return enabled != 0;
}

bool
yttrium_venus_mixed_draw_transfer_batch_enabled(void)
{
   static int enabled = -1;

   if (enabled < 0)
      enabled = yttrium_venus_batch_feature_enabled(
         YTTRIUM_VENUS_MIXED_DRAW_TRANSFER_BATCH_ENV);

   return enabled != 0;
}

bool
yttrium_venus_direct_cpu_vertex_upload_enabled(void)
{
   static int enabled = -1;

   if (enabled < 0)
      enabled = yttrium_venus_batch_feature_enabled(
         YTTRIUM_VENUS_DIRECT_CPU_VERTEX_UPLOAD_ENV);

   return enabled != 0;
}

bool
yttrium_venus_direct_ubo_upload_enabled(void)
{
   static int enabled = -1;

   if (enabled < 0) {
      const int state = yttrium_venus_option_state(
         YTTRIUM_VENUS_DIRECT_UBO_UPLOAD_ENV);

      /*
       * Persistently mapped UBO arenas are the normal Venus2 publication
       * path.  Keep an explicit opt-out for fault isolation, but do not make
       * production performance depend on a process-local experiment knob.
       */
      enabled = state < 0 ? 1 : state;
   }

   return enabled != 0;
}

bool
yttrium_venus_compact_draw_packets_enabled(void)
{
   static int enabled = -1;

   if (enabled < 0)
      enabled = yttrium_venus_option_enabled_default(
         YTTRIUM_VENUS_COMPACT_DRAW_PACKETS_ENV, true);

   return enabled != 0;
}

static void
yttrium_venus_log_batch_policy_once(void)
{
   static volatile LONG logged;

   if (InterlockedCompareExchange(&logged, 1, 0) != 0)
      return;

   const bool async_batch = yttrium_venus_async_batch_enabled();
   const bool native_draw_batch = yttrium_venus_native_draw_batch_enabled();
   const uint32_t draw_limit = yttrium_venus_native_draw_batch_limit();
   const bool cpu_vertex_batch = yttrium_venus_cpu_vertex_batch_enabled();
   const bool push_descriptor_batch =
      yttrium_venus_push_descriptor_batch_enabled();
   const bool push_descriptor_layout_rotation =
      yttrium_venus_push_descriptor_layout_rotation_enabled();
   const bool render_pass_batch = yttrium_venus_render_pass_batch_enabled();
   const bool mixed_draw_transfer_batch =
      yttrium_venus_mixed_draw_transfer_batch_enabled();
   const bool direct_cpu_vertex_upload =
      yttrium_venus_direct_cpu_vertex_upload_enabled();
   const bool sampled_cpu_vertex_batch =
      yttrium_venus_sampled_cpu_vertex_batch_enabled();
   const bool sampled_cpu_vertex_render_pass_batch =
      yttrium_venus_sampled_cpu_vertex_render_pass_batch_enabled();
   const bool checked_descriptor_alloc =
      yttrium_venus_checked_descriptor_alloc_enabled();
   const bool direct_ubo_upload = yttrium_venus_direct_ubo_upload_enabled();
   const bool compact_draw_packets =
      yttrium_venus_compact_draw_packets_enabled();

   YTTRIUM_LOG("yttrium: Venus2 batch policy async=%u draw=%u "
               "draw_limit=%u cpu_vertex=%u push_descriptor=%u render_pass=%u "
               "mixed_draw_transfer=%u direct_cpu_vertex_upload=%u "
               "sampled_cpu_vertex=%u sampled_cpu_vertex_render_pass=%u "
               "checked_descriptor_alloc=%u direct_ubo_upload=%u "
               "push_descriptor_layout_rotation=%u "
               "compact_draw_packets=%u "
               "source=yttrium-options\n",
               async_batch ? 1 : 0, native_draw_batch ? 1 : 0, draw_limit,
               cpu_vertex_batch ? 1 : 0, push_descriptor_batch ? 1 : 0,
               render_pass_batch ? 1 : 0,
               mixed_draw_transfer_batch ? 1 : 0,
               direct_cpu_vertex_upload ? 1 : 0,
               sampled_cpu_vertex_batch ? 1 : 0,
               sampled_cpu_vertex_render_pass_batch ? 1 : 0,
               checked_descriptor_alloc ? 1 : 0,
               direct_ubo_upload ? 1 : 0,
               push_descriptor_layout_rotation ? 1 : 0,
               compact_draw_packets ? 1 : 0);

   if (!async_batch || !native_draw_batch || !cpu_vertex_batch ||
       !push_descriptor_batch || !render_pass_batch) {
      YTTRIUM_LOG("yttrium: Venus2 async batching is not fully "
                  "enabled async=%u draw=%u cpu_vertex=%u "
                  "push_descriptor=%u render_pass=%u draw_limit=%u; "
                  "required knobs: %s=1 %s=1 %s=1 %s=1 %s=1\n",
                  async_batch ? 1 : 0, native_draw_batch ? 1 : 0,
                  cpu_vertex_batch ? 1 : 0,
                  push_descriptor_batch ? 1 : 0,
                  render_pass_batch ? 1 : 0, draw_limit,
                  YTTRIUM_VENUS_ASYNC_BATCH_ENV,
                  YTTRIUM_VENUS_NATIVE_DRAW_BATCH_ENV,
                  YTTRIUM_VENUS_CPU_VERTEX_BATCH_ENV,
                  YTTRIUM_VENUS_PUSH_DESCRIPTOR_BATCH_ENV,
                  YTTRIUM_VENUS_RENDER_PASS_BATCH_ENV);
   }

   const char *limit_value = yttrium_gdi_debug_get_option(
      YTTRIUM_VENUS_NATIVE_DRAW_BATCH_LIMIT_ENV, NULL);
   if (limit_value) {
      char *end = NULL;
      unsigned long parsed = strtoul(limit_value, &end, 0);
      if (end && *end == '\0' && parsed > draw_limit) {
         YTTRIUM_WARN("yttrium: WARNING: Venus2 draw batch limit clamped "
                      "%s=%lu effective=%u max=%u\n",
                      YTTRIUM_VENUS_NATIVE_DRAW_BATCH_LIMIT_ENV, parsed,
                      draw_limit, YTTRIUM_VENUS_NATIVE_DRAW_BATCH_LIMIT_MAX);
      }
   }
}

struct yttrium_venus_native_draw_batch_state
yttrium_venus_get_native_draw_batch_state(
   const struct yttrium_pipeline *pipeline,
   bool has_cpu_vertex_upload,
   bool has_sampled_descriptor,
   bool has_ubo_descriptor)
{
   yttrium_venus_log_batch_policy_once();

   struct yttrium_venus_native_draw_batch_state state = {
      .limit = yttrium_venus_native_draw_batch_limit(),
   };
   const bool native_draw_batch_enabled =
      yttrium_venus_native_draw_batch_enabled();
   const bool push_descriptor_batch_enabled =
      yttrium_venus_push_descriptor_batch_enabled();
   const bool pipeline_has_push_layout =
      pipeline->push_descriptor_set_layout && pipeline->push_pipeline_layout;
   const bool pipeline_has_push_pipeline =
      pipeline->push_pipeline != VK_NULL_HANDLE;
   const bool push_descriptors_available =
      push_descriptor_batch_enabled &&
      (has_sampled_descriptor || has_ubo_descriptor) &&
      pipeline_has_push_layout && pipeline_has_push_pipeline;
   const bool cpu_vertex_batch_enabled =
      yttrium_venus_cpu_vertex_batch_enabled();
   const bool sampled_cpu_vertex_batch_enabled =
      yttrium_venus_sampled_cpu_vertex_batch_enabled();
   const bool cpu_vertex_batch_allowed =
      !has_cpu_vertex_upload ||
      (cpu_vertex_batch_enabled &&
       (!has_sampled_descriptor || sampled_cpu_vertex_batch_enabled));

   state.candidate =
      native_draw_batch_enabled &&
      cpu_vertex_batch_allowed &&
      (!has_sampled_descriptor || push_descriptors_available);
   state.use_push_descriptors =
      state.candidate && push_descriptors_available;
   state.native_draw_batch_enabled = native_draw_batch_enabled;
   state.cpu_vertex_batch_enabled = cpu_vertex_batch_enabled;
   state.cpu_vertex_batch_allowed = cpu_vertex_batch_allowed;
   state.push_descriptor_batch_enabled = push_descriptor_batch_enabled;
   state.push_descriptors_available = push_descriptors_available;
   state.pipeline_has_push_layout = pipeline_has_push_layout;
   state.pipeline_has_push_pipeline = pipeline_has_push_pipeline;

   if (!native_draw_batch_enabled)
      state.reject_mask |=
         YTTRIUM_TRACE_NATIVE_DRAW_BATCH_REJECT_DRAW_BATCH_DISABLED;
   if (!cpu_vertex_batch_allowed)
      state.reject_mask |=
         YTTRIUM_TRACE_NATIVE_DRAW_BATCH_REJECT_CPU_VERTEX_DISABLED;
   if (has_cpu_vertex_upload && has_sampled_descriptor &&
       !sampled_cpu_vertex_batch_enabled)
      state.reject_mask |=
         YTTRIUM_TRACE_NATIVE_DRAW_BATCH_REJECT_SAMPLED_CPU_VERTEX_DISABLED;
   if (has_sampled_descriptor && !push_descriptors_available) {
      state.reject_mask |=
         YTTRIUM_TRACE_NATIVE_DRAW_BATCH_REJECT_SAMPLED_WITHOUT_PUSH;
      if (!push_descriptor_batch_enabled)
         state.reject_mask |=
            YTTRIUM_TRACE_NATIVE_DRAW_BATCH_REJECT_PUSH_BATCH_DISABLED;
      if (!pipeline_has_push_layout)
         state.reject_mask |=
            YTTRIUM_TRACE_NATIVE_DRAW_BATCH_REJECT_PUSH_LAYOUT_MISSING;
      if (!pipeline_has_push_pipeline)
         state.reject_mask |=
            YTTRIUM_TRACE_NATIVE_DRAW_BATCH_REJECT_PUSH_PIPELINE_MISSING;
   }

   return state;
}
