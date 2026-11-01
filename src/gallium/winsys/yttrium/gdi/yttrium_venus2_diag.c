/*
 * SPDX-FileCopyrightText: 2026 Ake Rehnman <ake.rehnman@gmail.com>
 * SPDX-License-Identifier: MPL-2.0
 */

#include <stdint.h>

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
}
