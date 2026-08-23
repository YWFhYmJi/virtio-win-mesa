/*
 * SPDX-FileCopyrightText: 2026 Ake Rehnman <ake.rehnman@gmail.com>
 * SPDX-License-Identifier: MPL-2.0
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "util/u_math.h"
#include "yttrium_options.h"
#include "yttrium_trace.h"
#include "yttrium_venus2_private.h"

#include "venus-protocol/vn_protocol_driver_defines.h"

void
yttrium_venus_trace_timing(uint32_t point,
                           uint32_t status,
                           uint64_t start_us,
                           const char *label,
                           uint64_t a,
                           uint64_t b,
                           uint32_t c,
                           uint32_t d)
{
   if (!start_us)
      return;

   /*
    * Only outliers are worth reporting during normal tracing, but the same
    * points are the only decomposition of per-draw cost we have, and a draw
    * costs tens of microseconds - so at the 1000 us default they emitted 150
    * events against 71901 draws and said nothing about where the time goes.
    *
    * Set D3D10UMD_YTTRIUM_TIMING_SLOW_US=0 to emit every point.  That is ~6
    * events per draw, tens of thousands per second, so it costs something and
    * inflates what it measures; it is for a decomposition run, not for normal
    * tracing.
    */
   static int threshold = -1;

   if (threshold < 0)
      threshold = (int)yttrium_gdi_debug_get_num_option(
                     "D3D10UMD_YTTRIUM_TIMING_SLOW_US",
                     YTTRIUM_VENUS_TIMING_SLOW_US);

   const uint64_t elapsed_us = yttrium_trace_now_us() - start_us;
   if (status || elapsed_us >= (uint64_t)threshold)
      yttrium_trace_timing(point, status, elapsed_us, label, a, b, c, d);
}

static const char *
yttrium_venus_sync_wait_kind_name(uint32_t kind)
{
   switch (kind) {
   case YTTRIUM_VENUS_SYNC_WAIT_RAW_SUBMIT:
      return "raw_submit";
   case YTTRIUM_VENUS_SYNC_WAIT_RING_SPACE:
      return "ring_space";
   case YTTRIUM_VENUS_SYNC_WAIT_RING_SEQNO:
      return "ring_seqno";
   case YTTRIUM_VENUS_SYNC_WAIT_BATCH_COMPLETION:
      return "batch_completion";
   default:
      return "unknown";
   }
}

const char *
yttrium_venus_command_type_name(uint32_t command_type)
{
   switch (command_type) {
   case VK_COMMAND_TYPE_vkQueueSubmit_EXT:
      return "vkQueueSubmit";
   case VK_COMMAND_TYPE_vkAllocateMemory_EXT:
      return "vkAllocateMemory";
   case VK_COMMAND_TYPE_vkBindBufferMemory_EXT:
      return "vkBindBufferMemory";
   case VK_COMMAND_TYPE_vkBindImageMemory_EXT:
      return "vkBindImageMemory";
   case VK_COMMAND_TYPE_vkGetBufferMemoryRequirements_EXT:
      return "vkGetBufferMemoryRequirements";
   case VK_COMMAND_TYPE_vkGetImageMemoryRequirements_EXT:
      return "vkGetImageMemoryRequirements";
   case VK_COMMAND_TYPE_vkResetFences_EXT:
      return "vkResetFences";
   case VK_COMMAND_TYPE_vkWaitForFences_EXT:
      return "vkWaitForFences";
   case VK_COMMAND_TYPE_vkCreateBuffer_EXT:
      return "vkCreateBuffer";
   case VK_COMMAND_TYPE_vkCreateImage_EXT:
      return "vkCreateImage";
   case VK_COMMAND_TYPE_vkGetImageSubresourceLayout_EXT:
      return "vkGetImageSubresourceLayout";
   case VK_COMMAND_TYPE_vkCreateImageView_EXT:
      return "vkCreateImageView";
   case VK_COMMAND_TYPE_vkCreateShaderModule_EXT:
      return "vkCreateShaderModule";
   case VK_COMMAND_TYPE_vkCreateGraphicsPipelines_EXT:
      return "vkCreateGraphicsPipelines";
   case VK_COMMAND_TYPE_vkCreatePipelineLayout_EXT:
      return "vkCreatePipelineLayout";
   case VK_COMMAND_TYPE_vkCreateSampler_EXT:
      return "vkCreateSampler";
   case VK_COMMAND_TYPE_vkCreateDescriptorSetLayout_EXT:
      return "vkCreateDescriptorSetLayout";
   case VK_COMMAND_TYPE_vkCreateDescriptorPool_EXT:
      return "vkCreateDescriptorPool";
   case VK_COMMAND_TYPE_vkAllocateDescriptorSets_EXT:
      return "vkAllocateDescriptorSets";
   case VK_COMMAND_TYPE_vkCreateFramebuffer_EXT:
      return "vkCreateFramebuffer";
   case VK_COMMAND_TYPE_vkCreateRenderPass_EXT:
      return "vkCreateRenderPass";
   case VK_COMMAND_TYPE_vkDestroyFramebuffer_EXT:
      return "vkDestroyFramebuffer";
   case VK_COMMAND_TYPE_vkDestroyRenderPass_EXT:
      return "vkDestroyRenderPass";
   case VK_COMMAND_TYPE_vkDestroyImageView_EXT:
      return "vkDestroyImageView";
   case VK_COMMAND_TYPE_vkCmdPipelineBarrier_EXT:
      return "vkCmdPipelineBarrier";
   case VK_COMMAND_TYPE_vkCmdBeginRenderPass_EXT:
      return "vkCmdBeginRenderPass";
   case VK_COMMAND_TYPE_vkCmdClearAttachments_EXT:
      return "vkCmdClearAttachments";
   case VK_COMMAND_TYPE_vkCmdClearDepthStencilImage_EXT:
      return "vkCmdClearDepthStencilImage";
   case VK_COMMAND_TYPE_vkCmdEndRenderPass_EXT:
      return "vkCmdEndRenderPass";
   case VK_COMMAND_TYPE_vkAllocateCommandBuffers_EXT:
      return "vkAllocateCommandBuffers";
   case VK_COMMAND_TYPE_vkBeginCommandBuffer_EXT:
      return "vkBeginCommandBuffer";
   case VK_COMMAND_TYPE_vkEndCommandBuffer_EXT:
      return "vkEndCommandBuffer";
   case VK_COMMAND_TYPE_vkResetCommandBuffer_EXT:
      return "vkResetCommandBuffer";
   case VK_COMMAND_TYPE_vkGetPhysicalDeviceFeatures2_EXT:
      return "vkGetPhysicalDeviceFeatures2";
   case VK_COMMAND_TYPE_vkGetPhysicalDeviceProperties2_EXT:
      return "vkGetPhysicalDeviceProperties2";
   case VK_COMMAND_TYPE_vkGetMemoryResourcePropertiesMESA_EXT:
      return "vkGetMemoryResourcePropertiesMESA";
   case VK_COMMAND_TYPE_vkCreateBufferView_EXT:
      return "vkCreateBufferView";
   default:
      return "unknown";
   }
}

static void
yttrium_venus_debug_sync_wait_record_command(
   struct yttrium_venus_sync_wait_diag *diag,
   enum yttrium_venus_sync_wait_kind kind,
   uint64_t elapsed_us,
   uint32_t command_type,
   uint32_t command_size,
   uint32_t reply_size)
{
   if (kind != YTTRIUM_VENUS_SYNC_WAIT_RING_SEQNO ||
       command_type == UINT32_MAX)
      return;

   uint32_t slot = YTTRIUM_VENUS_SYNC_WAIT_DIAG_CMD_SLOTS;
   uint32_t empty_slot = YTTRIUM_VENUS_SYNC_WAIT_DIAG_CMD_SLOTS;
   uint32_t min_slot = 0;
   for (uint32_t i = 0; i < YTTRIUM_VENUS_SYNC_WAIT_DIAG_CMD_SLOTS; i++) {
      if (diag->commands[i].command_type == command_type) {
         slot = i;
         break;
      }
      if (!diag->commands[i].count &&
          empty_slot == YTTRIUM_VENUS_SYNC_WAIT_DIAG_CMD_SLOTS)
         empty_slot = i;
      if (diag->commands[i].count < diag->commands[min_slot].count)
         min_slot = i;
   }

   if (slot == YTTRIUM_VENUS_SYNC_WAIT_DIAG_CMD_SLOTS)
      slot = empty_slot != YTTRIUM_VENUS_SYNC_WAIT_DIAG_CMD_SLOTS ?
         empty_slot : min_slot;

   if (!diag->commands[slot].count ||
       diag->commands[slot].command_type != command_type) {
      memset(&diag->commands[slot], 0, sizeof(diag->commands[slot]));
      diag->commands[slot].command_type = command_type;
   }

   diag->commands[slot].count++;
   diag->commands[slot].wait_us += elapsed_us;
   diag->commands[slot].max_wait_us =
      MAX2(diag->commands[slot].max_wait_us, elapsed_us);
   diag->commands[slot].command_size = command_size;
   diag->commands[slot].reply_size = reply_size;
}

static void
yttrium_venus_debug_sync_wait_top_commands(
   const struct yttrium_venus_sync_wait_diag *diag,
   uint32_t out[3])
{
   out[0] = out[1] = out[2] = YTTRIUM_VENUS_SYNC_WAIT_DIAG_CMD_SLOTS;

   for (uint32_t i = 0; i < YTTRIUM_VENUS_SYNC_WAIT_DIAG_CMD_SLOTS; i++) {
      if (!diag->commands[i].count)
         continue;

      for (uint32_t rank = 0; rank < 3; rank++) {
         if (out[rank] != YTTRIUM_VENUS_SYNC_WAIT_DIAG_CMD_SLOTS &&
             diag->commands[out[rank]].wait_us >= diag->commands[i].wait_us)
            continue;

         for (uint32_t move = 2; move > rank; move--)
            out[move] = out[move - 1];
         out[rank] = i;
         break;
      }
   }
}

void
yttrium_venus_debug_sync_wait(struct yttrium_venus *venus,
                              enum yttrium_venus_sync_wait_kind kind,
                              uint64_t elapsed_us,
                              uint32_t status,
                              const char *label,
                              uint32_t a,
                              uint32_t b,
                              uint32_t command_type,
                              uint32_t command_size,
                              uint32_t reply_size)
{
   if (!venus)
      return;
   if (!yttrium_trace_sync_wait_is_enabled())
      return;

   const char *owner = label && label[0] ? label : "<missing-wait-owner>";
   const uint64_t backlog_command_count =
      kind == YTTRIUM_VENUS_SYNC_WAIT_RING_SEQNO ?
         venus->ring.current_wait_command_count : 0;
   const uint64_t backlog_command_bytes =
      kind == YTTRIUM_VENUS_SYNC_WAIT_RING_SEQNO ?
         venus->ring.current_wait_command_bytes : 0;
   const uint64_t backlog_queue_submit_count =
      kind == YTTRIUM_VENUS_SYNC_WAIT_RING_SEQNO ?
         venus->ring.current_wait_queue_submit_count : 0;

   struct yttrium_venus_sync_wait_diag *diag = &venus->sync_wait_diag;
   const uint64_t now_us = yttrium_trace_now_us();
   yttrium_trace_sync_wait(kind,
                           yttrium_venus_sync_wait_kind_name(kind),
                           status,
                           elapsed_us,
                           command_type,
                           yttrium_venus_command_type_name(command_type),
                           command_size,
                           reply_size,
                           backlog_command_count,
                           backlog_command_bytes,
                           backlog_queue_submit_count,
                           owner,
                           a,
                           b);
   if (!diag->window_start_us)
      diag->window_start_us = now_us;

   diag->total_count++;
   diag->total_wait_us += elapsed_us;
   diag->max_wait_us = MAX2(diag->max_wait_us, elapsed_us);
   diag->timeout_count += status ? 1 : 0;
   diag->last_kind = kind;
   diag->last_status = status;
   diag->last_label = owner;
   diag->last_a = a;
   diag->last_b = b;
   diag->last_command_type = command_type;
   diag->last_command_size = command_size;
   diag->last_reply_size = reply_size;

   yttrium_venus_debug_sync_wait_record_command(
      diag, kind, elapsed_us, command_type, command_size, reply_size);

   switch (kind) {
   case YTTRIUM_VENUS_SYNC_WAIT_RAW_SUBMIT:
      diag->raw_submit_count++;
      break;
   case YTTRIUM_VENUS_SYNC_WAIT_RING_SPACE:
      diag->ring_space_count++;
      break;
   case YTTRIUM_VENUS_SYNC_WAIT_RING_SEQNO:
      diag->ring_seqno_count++;
      break;
   default:
      break;
   }

   const uint64_t window_us = now_us - diag->window_start_us;
   if (window_us < YTTRIUM_VENUS_SYNC_WAIT_DIAG_WINDOW_US &&
       diag->total_count < YTTRIUM_VENUS_SYNC_WAIT_DIAG_MAX_COUNT)
      return;

   if (!yttrium_trace_verbose_etw_text_enabled()) {
      memset(diag, 0, sizeof(*diag));
      return;
   }

   const uint64_t wait_permille =
      window_us ? (diag->total_wait_us * 1000ull) / window_us : 0;
   uint32_t top[3];
   yttrium_venus_debug_sync_wait_top_commands(diag, top);
#define YTTRIUM_SYNC_CMD_FIELD(rank, field) \
   (top[(rank)] == YTTRIUM_VENUS_SYNC_WAIT_DIAG_CMD_SLOTS ? \
    0 : diag->commands[top[(rank)]].field)
#define YTTRIUM_SYNC_CMD_TYPE(rank) \
   (top[(rank)] == YTTRIUM_VENUS_SYNC_WAIT_DIAG_CMD_SLOTS ? \
    UINT32_MAX : diag->commands[top[(rank)]].command_type)
   char msg[512];
   snprintf(msg, sizeof(msg),
            "yttrium: Venus sync wait diag window_us=%llu wait_us=%llu wait_pct=%llu.%u count=%u raw=%u space=%u seqno=%u timeout=%u max_us=%llu last=%s status=%u label=%s a=%u b=%u last_cmd=%s(%u) cmd_size=%u reply_size=%u top0=%s(%u):%u/%llu top1=%s(%u):%u/%llu top2=%s(%u):%u/%llu\n",
            (unsigned long long)window_us,
            (unsigned long long)diag->total_wait_us,
            (unsigned long long)(wait_permille / 10),
            (unsigned)(wait_permille % 10),
            diag->total_count,
            diag->raw_submit_count,
            diag->ring_space_count,
            diag->ring_seqno_count,
            diag->timeout_count,
            (unsigned long long)diag->max_wait_us,
            yttrium_venus_sync_wait_kind_name(diag->last_kind),
            diag->last_status,
            diag->last_label ? diag->last_label : "",
            diag->last_a,
            diag->last_b,
            yttrium_venus_command_type_name(diag->last_command_type),
            diag->last_command_type,
            diag->last_command_size,
            diag->last_reply_size,
            yttrium_venus_command_type_name(YTTRIUM_SYNC_CMD_TYPE(0)),
            YTTRIUM_SYNC_CMD_TYPE(0),
            YTTRIUM_SYNC_CMD_FIELD(0, count),
            (unsigned long long)YTTRIUM_SYNC_CMD_FIELD(0, wait_us),
            yttrium_venus_command_type_name(YTTRIUM_SYNC_CMD_TYPE(1)),
            YTTRIUM_SYNC_CMD_TYPE(1),
            YTTRIUM_SYNC_CMD_FIELD(1, count),
            (unsigned long long)YTTRIUM_SYNC_CMD_FIELD(1, wait_us),
            yttrium_venus_command_type_name(YTTRIUM_SYNC_CMD_TYPE(2)),
            YTTRIUM_SYNC_CMD_TYPE(2),
            YTTRIUM_SYNC_CMD_FIELD(2, count),
            (unsigned long long)YTTRIUM_SYNC_CMD_FIELD(2, wait_us));
#undef YTTRIUM_SYNC_CMD_FIELD
#undef YTTRIUM_SYNC_CMD_TYPE
   yttrium_trace_sync_wait_summary(msg);
   memset(diag, 0, sizeof(*diag));
}
