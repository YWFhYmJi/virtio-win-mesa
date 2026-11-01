/*
 * SPDX-FileCopyrightText: 2026 Ake Rehnman <ake.rehnman@gmail.com>
 * SPDX-License-Identifier: MPL-2.0
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "util/u_math.h"

#include "yttrium_gdi_public.h"
#include "yttrium_options.h"
#include "yttrium_venus2_private.h"
#include "yttrium_trace.h"
#include "yttrium_shared_image.h"

#include "venus-protocol/vn_protocol_driver_buffer.h"
#include "venus-protocol/vn_protocol_driver_device.h"
#include "venus-protocol/vn_protocol_driver_device_memory.h"
#include "venus-protocol/vn_protocol_driver_image.h"

static bool
yttrium_venus_sync_allocations_enabled(void)
{
   static volatile LONG cached = -1;
   LONG enabled = cached;
   if (enabled < 0) {
      const LONG resolved = yttrium_gdi_debug_get_bool_option(
         "D3D10UMD_YTTRIUM_SYNC_ALLOCATIONS", false) ? 1 : 0;
      const LONG previous = InterlockedCompareExchange(&cached, resolved, -1);
      enabled = previous < 0 ? resolved : previous;
      if (previous < 0 && enabled) {
         yttrium_gdi_user_logf(
            "yttrium: Venus synchronous allocations enabled owner=venus2 option=D3D10UMD_YTTRIUM_SYNC_ALLOCATIONS action=wait-for-vkAllocateMemory-replies\n");
      }
   }
   return enabled != 0;
}

VkResult
yttrium_venus_allocate_memory(struct yttrium_venus *venus,
                              const VkMemoryAllocateInfo *info,
                              const VkMemoryRequirements *requirements,
                              VkDeviceMemory *memory,
                              uint64_t memory_id,
                              const char *owner,
                              bool force_sync)
{
   const bool synchronous =
      yttrium_venus_sync_allocations_enabled() || force_sync;
   struct vn_ring_submit_command submit;
   vn_submit_vkAllocateMemory(
      &venus->vn_ring, synchronous ? VK_COMMAND_GENERATE_REPLY_BIT_EXT : 0,
      venus->device_handle, info, NULL, memory, &submit);

   /* The generated vn_call wrapper reports OUT_OF_HOST_MEMORY for a missing
    * reply too.  Keep transport failures separate from host allocation results.
    */
   VkResult result = VK_ERROR_UNKNOWN;
   const char *reason = "ring-submit-or-wait-failed";
   if (submit.ring_seqno_valid) {
      if (!synchronous)
         return VK_SUCCESS;

      struct vn_cs_decoder *reply =
         vn_ring_get_command_reply(&venus->vn_ring, &submit);
      if (reply) {
         result = vn_decode_vkAllocateMemory_reply(
            reply, venus->device_handle, info, NULL, memory);
         reason = "host-result";
      } else {
         reason = "missing-reply";
      }
      vn_ring_free_command_reply(&venus->vn_ring, &submit);
      if (result == VK_SUCCESS)
         return result;
   }

   /* An unsuccessful allocation does not produce a usable host memory object.
    * In particular, do not pass the preallocated guest handle to bind/free.
    */
   *memory = VK_NULL_HANDLE;
   VkExternalMemoryHandleTypeFlags export_types = 0;
   uint32_t import_resource_id = 0;
   for (const VkBaseInStructure *next = info->pNext; next; next = next->pNext) {
      if (next->sType == VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO)
         export_types = ((const VkExportMemoryAllocateInfo *)next)->handleTypes;
      else if (next->sType == VK_STRUCTURE_TYPE_IMPORT_MEMORY_RESOURCE_INFO_MESA)
         import_resource_id =
            ((const VkImportMemoryResourceInfoMESA *)next)->resourceId;
   }
   const VkMemoryType *type =
      &venus->memory_props.memoryTypes[info->memoryTypeIndex];
   const VkMemoryHeap *heap = &venus->memory_props.memoryHeaps[type->heapIndex];
   YTTRIUM_WARN("yttrium: ERROR: Venus memory allocation failed owner=venus2 operation=vkAllocateMemory resource_owner=%s reason=%s result=%d synchronous=%u memory_id=%llu size=%llu type=%u property_flags=0x%x heap=%u heap_size=%llu heap_flags=0x%x required_size=%llu alignment=%llu memory_type_bits=0x%x export_types=0x%x import_resource_id=%u action=fail-allocation\n",
                owner, reason, result, synchronous,
                (unsigned long long)memory_id,
                (unsigned long long)info->allocationSize,
                info->memoryTypeIndex, type->propertyFlags, type->heapIndex,
                (unsigned long long)heap->size, heap->flags,
                (unsigned long long)requirements->size,
                (unsigned long long)requirements->alignment,
                requirements->memoryTypeBits, export_types, import_resource_id);
   return result;
}

static bool
yttrium_venus_sample_count(unsigned sample_count,
                           VkSampleCountFlagBits *out_samples)
{
   switch (sample_count) {
   case 0:
   case 1:
      *out_samples = VK_SAMPLE_COUNT_1_BIT;
      return true;
   case 2:
      *out_samples = VK_SAMPLE_COUNT_2_BIT;
      return true;
   case 4:
      *out_samples = VK_SAMPLE_COUNT_4_BIT;
      return true;
   case 8:
      *out_samples = VK_SAMPLE_COUNT_8_BIT;
      return true;
   case 16:
      *out_samples = VK_SAMPLE_COUNT_16_BIT;
      return true;
   default:
      *out_samples = VK_SAMPLE_COUNT_1_BIT;
      return false;
   }
}

static VkImageCreateFlags
yttrium_venus_image_create_flags(const struct yttrium_venus *venus,
                                  VkImageCreateFlags extra_flags,
                                  VkImageUsageFlags usage,
                                  VkSampleCountFlagBits samples,
                                  VkImageTiling tiling)
{
   VkImageCreateFlags image_flags =
      extra_flags |
      ((usage & (VK_IMAGE_USAGE_SAMPLED_BIT |
                 VK_IMAGE_USAGE_STORAGE_BIT |
                 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)) ?
          VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT :
          0);

   if (venus->multisampled_render_to_single_sampled &&
       samples == VK_SAMPLE_COUNT_1_BIT &&
       tiling == VK_IMAGE_TILING_OPTIMAL &&
       (usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                 VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)))
      image_flags |=
         VK_IMAGE_CREATE_MULTISAMPLED_RENDER_TO_SINGLE_SAMPLED_BIT_EXT;

   return image_flags;
}

static bool
yttrium_venus_image_properties_fit(const VkImageFormatProperties *props,
                                    uint32_t width,
                                    uint32_t height,
                                    uint32_t depth,
                                    uint32_t levels,
                                    uint32_t layers,
                                    VkSampleCountFlagBits samples)
{
   return width && height && depth && levels && layers &&
      width <= props->maxExtent.width &&
      height <= props->maxExtent.height &&
      depth <= props->maxExtent.depth &&
      levels <= props->maxMipLevels &&
      layers <= props->maxArrayLayers &&
      (props->sampleCounts & samples);
}

static bool
yttrium_venus_select_depth_storage_format(
   struct yttrium_venus *venus, VkFormat *vk_format,
   VkImageType image_type, uint32_t width, uint32_t height, uint32_t depth,
   uint32_t levels, uint32_t layers, VkImageCreateFlags extra_flags,
   VkImageUsageFlags usage, VkSampleCountFlagBits samples,
   VkImageTiling tiling, const char *owner)
{
   const VkFormat original_format = *vk_format;
   VkFormat float_format;
   switch (original_format) {
   case VK_FORMAT_D24_UNORM_S8_UINT:
      float_format = VK_FORMAT_D32_SFLOAT_S8_UINT;
      break;
   default:
      return true;
   }

   /* D3D's logical D24 layout does not guarantee native Vulkan D24 support.
    * Select the storage representation before creation, and repeat the same
    * descriptor query when importing a shared allocation. Never change the
    * representation after an allocation, transport, create or bind failure. */
   const VkFormat formats[] = { original_format, float_format };
   for (unsigned i = 0; i < ARRAY_SIZE(formats); i++) {
      const VkPhysicalDeviceImageFormatInfo2 image_info = {
         .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
         .format = formats[i],
         .type = image_type,
         .tiling = tiling,
         .usage = usage,
         .flags = yttrium_venus_image_create_flags(
            venus, extra_flags, usage, samples, tiling),
      };
      VkImageFormatProperties2 image_props = {
         .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2,
      };
      VkResult result = vn_call_vkGetPhysicalDeviceImageFormatProperties2(
         &venus->vn_ring, venus->physical_device, &image_info, &image_props);
      if (venus->failed)
         result = VK_ERROR_DEVICE_LOST;
      const bool fits = result == VK_SUCCESS &&
         yttrium_venus_image_properties_fit(
            &image_props.imageFormatProperties, width, height, depth,
            MAX2(levels, 1), MAX2(layers, 1), samples);
      if (fits) {
         if (i) {
            YTTRIUM_WARN("yttrium: WARNING depth storage representation selected owner=venus2 component=depth-format-selection resource_owner=%s reason=native-format-unsupported fallback=supported-float-depth original_vk_format=%u selected_vk_format=%u type=%u tiling=%u usage=0x%x flags=0x%x extent=%ux%ux%u levels=%u layers=%u samples=0x%x action=use-selected-format\n",
                         owner, original_format, formats[i], image_type,
                         tiling, usage, image_info.flags, width, height,
                         depth, MAX2(levels, 1), MAX2(layers, 1), samples);
         }
         *vk_format = formats[i];
         return true;
      }
      if (!i && result == VK_ERROR_FORMAT_NOT_SUPPORTED)
         continue;

      YTTRIUM_WARN("yttrium: ERROR depth storage unavailable owner=venus2 component=depth-format-selection resource_owner=%s reason=%s original_vk_format=%u queried_vk_format=%u result=%d type=%u tiling=%u usage=0x%x flags=0x%x extent=%ux%ux%u levels=%u layers=%u samples=0x%x action=fail-resource\n",
                   owner, result == VK_SUCCESS ?
                      "descriptor-exceeds-format-limits" : "image-format-query-failed",
                   original_format, formats[i], result, image_type, tiling,
                   usage, image_info.flags, width, height, depth,
                   MAX2(levels, 1), MAX2(layers, 1), samples);
      return false;
   }
   return false;
}

static bool
yttrium_venus_select_feedback_loop_usage(struct yttrium_venus *venus,
                                          VkFormat vk_format,
                                          VkImageType image_type,
                                          uint32_t width,
                                          uint32_t height,
                                          uint32_t depth,
                                          uint32_t levels,
                                          uint32_t layers,
                                          VkImageCreateFlags extra_flags,
                                          VkSampleCountFlagBits samples,
                                          VkImageTiling tiling,
                                          VkImageUsageFlags *usage)
{
   const VkImageUsageFlags attachment_usage =
      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
      VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
   if (!venus->attachment_feedback_loop_layout ||
       !(*usage & VK_IMAGE_USAGE_SAMPLED_BIT) ||
       !(*usage & attachment_usage))
      return true;

   /* Device support does not imply support for every image tiling and format.
    * Feedback is optional at creation; actual feedback draws separately require
    * this bit in resource->image_usage before using the feedback-loop layout.
    * Keep the same decision for exported images and their imported aliases.
    */
   const VkImageUsageFlags feedback_usage =
      *usage | VK_IMAGE_USAGE_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT;
   VkPhysicalDeviceImageFormatInfo2 image_info = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
      .format = vk_format,
      .type = image_type,
      .tiling = tiling,
      .usage = feedback_usage,
      .flags = yttrium_venus_image_create_flags(venus, extra_flags,
                                                 feedback_usage,
                                                 samples, tiling),
   };
   VkImageFormatProperties2 image_props = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2,
   };
   const VkResult feedback_result =
      vn_call_vkGetPhysicalDeviceImageFormatProperties2(
         &venus->vn_ring, venus->physical_device, &image_info, &image_props);
   if (feedback_result == VK_SUCCESS &&
       yttrium_venus_image_properties_fit(
          &image_props.imageFormatProperties, width, height, depth,
          MAX2(levels, 1), MAX2(layers, 1), samples)) {
      *usage = image_info.usage;
      return true;
   }

   /* Do not mistake a transport/allocation failure for an unsupported option. */
   if (feedback_result != VK_SUCCESS &&
       feedback_result != VK_ERROR_FORMAT_NOT_SUPPORTED) {
      YTTRIUM_WARN("yttrium: ERROR: Venus image feedback capability query failed owner=venus2 result=%d vk_format=%u type=%u tiling=%u usage=0x%x flags=0x%x action=fail-resource-create\n",
                   feedback_result, vk_format, image_type, tiling,
                   image_info.usage, image_info.flags);
      return false;
   }

   image_info.usage = *usage;
   image_info.flags = yttrium_venus_image_create_flags(
      venus, extra_flags, *usage, samples, tiling);
   memset(&image_props.imageFormatProperties, 0,
          sizeof(image_props.imageFormatProperties));
   const VkResult base_result =
      vn_call_vkGetPhysicalDeviceImageFormatProperties2(
         &venus->vn_ring, venus->physical_device, &image_info, &image_props);
   if (base_result != VK_SUCCESS ||
       !yttrium_venus_image_properties_fit(
          &image_props.imageFormatProperties, width, height, depth,
          MAX2(levels, 1), MAX2(layers, 1), samples)) {
      YTTRIUM_WARN("yttrium: ERROR: Venus base image capability unavailable owner=venus2 result=%d vk_format=%u type=%u tiling=%u usage=0x%x flags=0x%x extent=%ux%ux%u levels=%u layers=%u samples=0x%x action=fail-resource-create\n",
                   base_result, vk_format, image_type, tiling,
                   image_info.usage, image_info.flags, width, height, depth,
                   MAX2(levels, 1), MAX2(layers, 1), samples);
      return false;
   }

   YTTRIUM_WARN("yttrium: Venus optional image feedback usage unsupported owner=venus2 result=%d vk_format=%u type=%u tiling=%u usage=0x%x flags=0x%x extent=%ux%ux%u levels=%u layers=%u samples=0x%x action=create-with-required-base-usage actual-feedback-draws=reject\n",
                feedback_result, vk_format, image_type, tiling,
                image_info.usage, image_info.flags, width, height, depth,
                MAX2(levels, 1), MAX2(layers, 1), samples);
   return true;
}

static void
yttrium_venus_discard_accepted_image(struct yttrium_venus *venus,
                                     struct yttrium_venus_resource *resource,
                                     bool memory_accepted)
{
   if (memory_accepted) {
      vn_async_vkFreeMemory(&venus->vn_ring, venus->device_handle,
                            resource->memory, NULL);
   }
   vn_async_vkDestroyImage(&venus->vn_ring, venus->device_handle,
                           resource->image, NULL);
   memset(resource, 0, sizeof(*resource));
}

static void
yttrium_venus_discard_accepted_buffer(struct yttrium_venus *venus,
                                      struct yttrium_venus_resource *resource,
                                      bool memory_accepted)
{
   if (memory_accepted) {
      vn_async_vkFreeMemory(&venus->vn_ring, venus->device_handle,
                            resource->memory, NULL);
   }
   vn_async_vkDestroyBuffer(&venus->vn_ring, venus->device_handle,
                            resource->buffer, NULL);
   memset(resource, 0, sizeof(*resource));
}

static void
yttrium_venus_discard_accepted_bound_image(
   struct yttrium_venus *venus,
   struct yttrium_venus_resource *resource)
{
   /* The bind was accepted, so destroy the image before freeing its memory. */
   vn_async_vkDestroyImage(&venus->vn_ring, venus->device_handle,
                           resource->image, NULL);
   vn_async_vkFreeMemory(&venus->vn_ring, venus->device_handle,
                         resource->memory, NULL);
   memset(resource, 0, sizeof(*resource));
}

static bool
yttrium_venus_get_image_subresource_layout_checked(
   struct yttrium_venus *venus,
   const struct yttrium_venus_resource *resource,
   const VkImageSubresource *subresource,
   VkSubresourceLayout *layout,
   const char *owner)
{
   struct vn_ring_submit_command submit;
   vn_submit_vkGetImageSubresourceLayout(
      &venus->vn_ring, VK_COMMAND_GENERATE_REPLY_BIT_EXT,
      venus->device_handle, resource->image, subresource, layout, &submit);

   struct vn_cs_decoder *reply =
      submit.ring_seqno_valid ?
         vn_ring_get_command_reply(&venus->vn_ring, &submit) : NULL;
   if (!reply) {
      if (submit.ring_seqno_valid)
         vn_ring_free_command_reply(&venus->vn_ring, &submit);
      YTTRIUM_WARN("yttrium: ERROR: Venus image subresource layout query failed owner=venus2 operation=vkGetImageSubresourceLayout resource_owner=%s image_id=%llu reason=%s action=fail-resource-create\n",
                   owner ? owner : "<unknown>",
                   (unsigned long long)resource->image_obj.id,
                   submit.ring_seqno_valid ? "missing-reply" :
                                             "local-ring-enqueue-failed");
      return false;
   }

   vn_decode_vkGetImageSubresourceLayout_reply(
      reply, venus->device_handle, resource->image, subresource, layout);
   vn_ring_free_command_reply(&venus->vn_ring, &submit);
   return true;
}

static bool
yttrium_venus_create_image_tiled(struct yttrium_venus *venus,
                                 struct yttrium_venus_resource *resource,
                                 VkImageType image_type,
                                 VkFormat vk_format,
                                 uint32_t width,
                                 uint32_t height,
                                 uint32_t depth,
                                 uint32_t levels,
                                 uint32_t layers,
                                 VkImageCreateFlags extra_flags,
                                 VkImageUsageFlags usage,
                                 VkSampleCountFlagBits samples,
                                 VkImageTiling tiling,
                                 VkImageLayout initial_layout)
{
   yttrium_venus_init_object(venus, &resource->image_obj);
   resource->image = YTTRIUM_VENUS_HANDLE(VkImage, &resource->image_obj);
   const VkImageCreateFlags image_flags =
      yttrium_venus_image_create_flags(venus, extra_flags, usage,
                                       samples, tiling);

   const VkImageCreateInfo image_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .flags = image_flags,
      .imageType = image_type,
      .format = vk_format,
      .extent = { width, height, depth },
      .mipLevels = MAX2(levels, 1),
      .arrayLayers = MAX2(layers, 1),
      .samples = samples,
      .tiling = tiling,
      .usage = usage,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .initialLayout = initial_layout,
   };

   struct vn_ring_submit_command submit;
   vn_submit_vkCreateImage(&venus->vn_ring, 0, venus->device_handle,
                           &image_info, NULL, &resource->image, &submit);
   if (!yttrium_venus_async_submit_succeeded(
          venus, &submit, "vkCreateImage", resource->image_obj.id)) {
      memset(&resource->image_obj, 0, sizeof(resource->image_obj));
      resource->image = VK_NULL_HANDLE;
      return false;
   }

   resource->image_usage = usage;
   resource->vk_format = vk_format;
   resource->width = width;
   resource->height = height;
   resource->depth = MAX2(depth, 1);
   resource->levels = MAX2(levels, 1);
   resource->layers = MAX2(layers, 1);
   resource->samples = samples;
   resource->ds_clear_history_count = 0;
   resource->ds_clear_history_valid =
      yttrium_venus_format_has_depth(vk_format);
   return true;
}

static bool
yttrium_venus_create_image(struct yttrium_venus *venus,
                           struct yttrium_venus_resource *resource,
                           VkFormat vk_format,
                           uint32_t width,
                           uint32_t height,
                           VkImageUsageFlags usage)
{
   return yttrium_venus_create_image_tiled(venus, resource, VK_IMAGE_TYPE_2D,
                                           vk_format, width, height, 1,
                                           1, 1, 0, usage,
                                           VK_SAMPLE_COUNT_1_BIT,
                                           VK_IMAGE_TILING_LINEAR,
                                           VK_IMAGE_LAYOUT_UNDEFINED);
}

uint32_t
yttrium_venus_subresource_width(const struct yttrium_venus_resource *resource,
                                uint32_t level)
{
   if (!resource || !resource->width)
      return 0;
   return MAX2(resource->width >> MIN2(level, 31u), 1);
}

uint32_t
yttrium_venus_subresource_height(const struct yttrium_venus_resource *resource,
                                 uint32_t level)
{
   if (!resource || !resource->height)
      return 0;
   return MAX2(resource->height >> MIN2(level, 31u), 1);
}

uint32_t
yttrium_venus_subresource_depth(const struct yttrium_venus_resource *resource,
                                uint32_t level)
{
   if (!resource || !resource->depth)
      return 0;
   return MAX2(resource->depth >> MIN2(level, 31u), 1);
}

bool
yttrium_venus_resource_is_3d(
   const struct yttrium_venus_resource *resource)
{
   return resource && resource->depth > 1 && MAX2(resource->layers, 1) == 1;
}

bool
yttrium_venus_valid_image_subresource(
   const struct yttrium_venus_resource *resource,
   uint32_t level, uint32_t first_layer, uint32_t layer_count)
{
   const uint32_t levels = resource ? MAX2(resource->levels, 1) : 0;
   const uint32_t layers = resource ? MAX2(resource->layers, 1) : 0;

   return resource && level < levels && first_layer < layers &&
          layer_count && layer_count <= layers - first_layer;
}

void
yttrium_venus_ds_clear_history_reset(struct yttrium_venus_resource *resource)
{
   if (!resource)
      return;

   resource->ds_clear_history_count = 0;
   resource->ds_clear_history_valid = true;
}

void
yttrium_venus_ds_clear_history_invalidate(
   struct yttrium_venus_resource *resource)
{
   if (!resource || !yttrium_venus_format_has_depth(resource->vk_format))
      return;

   resource->ds_clear_history_count = 0;
   resource->ds_clear_history_valid = false;
}

void
yttrium_venus_ds_clear_history_drop_aspects(
   struct yttrium_venus_resource *resource,
   VkImageAspectFlags aspects)
{
   if (!resource || !resource->ds_clear_history_valid || !aspects)
      return;

   uint32_t out = 0;
   for (uint32_t i = 0; i < resource->ds_clear_history_count; i++) {
      struct yttrium_venus_ds_clear_record record =
         resource->ds_clear_history[i];
      record.aspects &= ~aspects;
      if (record.aspects)
         resource->ds_clear_history[out++] = record;
   }
   resource->ds_clear_history_count = out;
}

static bool
yttrium_venus_ds_clear_covers_subresource(
   const struct yttrium_venus_resource *resource,
   uint32_t level, uint32_t layer, uint32_t x, uint32_t y,
   uint32_t width, uint32_t height)
{
   return resource && level == 0 && layer == 0 && x == 0 && y == 0 &&
          width == yttrium_venus_subresource_width(resource, level) &&
          height == yttrium_venus_subresource_height(resource, level) &&
          MAX2(resource->levels, 1) == 1 && MAX2(resource->layers, 1) == 1;
}

void
yttrium_venus_ds_clear_history_note(
   struct yttrium_venus_resource *resource,
   VkImageAspectFlags aspects, double depth, unsigned stencil,
   uint32_t level, uint32_t layer, uint32_t x, uint32_t y,
   uint32_t width, uint32_t height)
{
   if (!resource || !yttrium_venus_format_has_depth(resource->vk_format))
      return;

   aspects &= yttrium_venus_format_aspects(resource->vk_format) &
              (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
   if (!aspects)
      return;

   const bool full_resource_clear =
      yttrium_venus_ds_clear_covers_subresource(resource, level, layer, x, y,
                                                width, height);
   if (!resource->ds_clear_history_valid) {
      if (!full_resource_clear)
         return;
      yttrium_venus_ds_clear_history_reset(resource);
   }

   if (full_resource_clear)
      yttrium_venus_ds_clear_history_drop_aspects(resource, aspects);

   if (resource->ds_clear_history_count >=
       YTTRIUM_VENUS_MAX_DS_CLEAR_HISTORY) {
      yttrium_venus_ds_clear_history_invalidate(resource);
      return;
   }

   struct yttrium_venus_ds_clear_record *record =
      &resource->ds_clear_history[resource->ds_clear_history_count++];
   *record = (struct yttrium_venus_ds_clear_record) {
      .aspects = aspects,
      .depth = (float)depth,
      .stencil = stencil & 0xff,
      .level = level,
      .layer = layer,
      .x = x,
      .y = y,
      .width = width,
      .height = height,
   };
}

bool
yttrium_venus_valid_render_subresource(
   const struct yttrium_venus_resource *resource,
   uint32_t level, uint32_t first_layer, uint32_t layer_count)
{
   if (!resource || level >= MAX2(resource->levels, 1) || !layer_count)
      return false;

   const uint32_t layers = yttrium_venus_resource_is_3d(resource) ?
      yttrium_venus_subresource_depth(resource, level) :
      MAX2(resource->layers, 1);
   return first_layer < layers && layer_count <= layers - first_layer;
}

VkImageViewType
yttrium_venus_render_view_type(const struct yttrium_venus_resource *resource,
                               uint32_t layer_count)
{
   if (yttrium_venus_resource_is_3d(resource) || layer_count > 1)
      return layer_count > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY :
                               VK_IMAGE_VIEW_TYPE_2D;

   return VK_IMAGE_VIEW_TYPE_2D;
}

VkImageSubresourceRange
yttrium_venus_render_barrier_range(
   const struct yttrium_venus_resource *resource,
   VkImageAspectFlags aspect_mask,
   uint32_t level, uint32_t first_layer, uint32_t layer_count)
{
   VkImageSubresourceRange range = {
      .aspectMask = aspect_mask,
      .baseMipLevel = level,
      .levelCount = 1,
      .baseArrayLayer = first_layer,
      .layerCount = layer_count,
   };

   if (yttrium_venus_resource_is_3d(resource)) {
      range.baseArrayLayer = 0;
      range.layerCount = 1;
   }

   return range;
}

/* A buffer bound to memory allocated with the export info below has to
 * declare the same handle type when it is created, or the binding is
 * invalid: the host driver is entitled to have chosen a layout the
 * export cannot satisfy.
 */
static VkExternalMemoryBufferCreateInfo
yttrium_venus_external_buffer_info(void)
{
   return (VkExternalMemoryBufferCreateInfo) {
      .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
      .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
   };
}

static VkExportMemoryAllocateInfo
yttrium_venus_export_memory_info(void)
{
   return (VkExportMemoryAllocateInfo) {
      .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
      .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
   };
}

bool
yttrium_venus2_create_display_image(struct yttrium_venus *venus,
                                   struct yttrium_venus_resource *resource,
                                   uint32_t width,
                                   uint32_t height,
                                   enum pipe_format pipe_format,
                                   uint64_t min_allocation_size,
                                   uint64_t *out_memory_id,
                                   uint64_t *out_allocation_size)
{
   const VkFormat vk_format = yttrium_venus2_pipe_format(pipe_format);

   if (out_memory_id)
      *out_memory_id = 0;
   if (out_allocation_size)
      *out_allocation_size = 0;

   if (!resource || vk_format == VK_FORMAT_UNDEFINED)
      return false;

   if (resource->initialized) {
      if (resource->buffer_backed || !resource->memory_obj.id)
         return false;
      if (out_memory_id)
         *out_memory_id = resource->memory_obj.id;
      if (out_allocation_size)
         *out_allocation_size = resource->allocation_size;
      return true;
   }

   if (!yttrium_venus_ensure_initialized(venus))
      return false;

   VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                              VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                              VK_IMAGE_USAGE_SAMPLED_BIT |
                              VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
   if (!yttrium_venus_select_feedback_loop_usage(
          venus, vk_format, VK_IMAGE_TYPE_2D, width, height, 1, 1, 1,
          0, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_LINEAR, &usage))
      return false;
   if (!yttrium_venus_create_image(venus, resource, vk_format, width, height,
                                    usage)) {
      usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
      if (!yttrium_venus_create_image(venus, resource, vk_format, width,
                                      height, usage))
         return false;
   }

   VkMemoryRequirements reqs;
   memset(&reqs, 0, sizeof(reqs));
   vn_call_vkGetImageMemoryRequirements(&venus->vn_ring,
                                        venus->device_handle,
                                        resource->image, &reqs);

   /*
    * Display images remain CPU-mappable, but prefer coherent device-local
    * memory because the host display path reads them with the GPU on Present.
    * A legacy BAR can satisfy this preference while it has space; otherwise
    * the chooser falls back to any host-visible type.
    */
   const VkMemoryPropertyFlags required_memory =
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
      (resource->cpu_readback ? VK_MEMORY_PROPERTY_HOST_CACHED_BIT : 0);
   const VkMemoryPropertyFlags preferred_memory =
      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
      (resource->cpu_readback ? 0 : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
   const uint32_t memory_type_index =
      yttrium_venus_choose_memory_type(venus, reqs.memoryTypeBits,
                                       required_memory, preferred_memory);
   if (memory_type_index == UINT32_MAX) {
      YTTRIUM_LOG("yttrium: Venus no compatible memory type for display image bits=0x%x %ux%u format=%u req_size=0x%llx cpu_readback=%u required=0x%x preferred=0x%x\n",
                   reqs.memoryTypeBits, width, height, pipe_format,
                   (unsigned long long)reqs.size, resource->cpu_readback,
                   required_memory, preferred_memory);
      vn_async_vkDestroyImage(&venus->vn_ring, venus->device_handle,
                              resource->image, NULL);
      memset(resource, 0, sizeof(*resource));
      return false;
   }

   yttrium_venus_init_object(venus, &resource->memory_obj);
   resource->memory =
      YTTRIUM_VENUS_HANDLE(VkDeviceMemory, &resource->memory_obj);

   VkExportMemoryAllocateInfo export_info =
      yttrium_venus_export_memory_info();
   const VkMemoryAllocateInfo memory_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .pNext = &export_info,
      .allocationSize = MAX2(reqs.size, (VkDeviceSize)min_allocation_size),
      .memoryTypeIndex = memory_type_index,
   };
   if (yttrium_venus_allocate_memory(
          venus, &memory_info, &reqs, &resource->memory,
          resource->memory_obj.id, "display-image", false) != VK_SUCCESS) {
      yttrium_venus_discard_accepted_image(venus, resource, false);
      return false;
   }

   struct vn_ring_submit_command bind_submit;
   vn_submit_vkBindImageMemory(&venus->vn_ring, 0, venus->device_handle,
                               resource->image, resource->memory, 0,
                               &bind_submit);
   if (!yttrium_venus_async_submit_succeeded(
          venus, &bind_submit, "vkBindImageMemory(display-image)",
          resource->image_obj.id)) {
      yttrium_venus_discard_accepted_image(venus, resource, true);
      return false;
   }

   const VkImageSubresource subresource = {
      .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
      .mipLevel = 0,
      .arrayLayer = 0,
   };
   VkSubresourceLayout layout;
   memset(&layout, 0, sizeof(layout));
   if (!yttrium_venus_get_image_subresource_layout_checked(
          venus, resource, &subresource, &layout, "display-image")) {
      yttrium_venus_discard_accepted_bound_image(venus, resource);
      return false;
   }

   resource->layout = VK_IMAGE_LAYOUT_UNDEFINED;
   resource->allocation_size = memory_info.allocationSize;
   resource->image_offset = layout.offset;
   resource->image_size = layout.size;
   resource->image_row_pitch = layout.rowPitch;
   resource->image_array_pitch = layout.arrayPitch;
   resource->initialized = true;
   resource->buffer_backed = false;

   if (out_memory_id)
      *out_memory_id = resource->memory_obj.id;
   if (out_allocation_size)
      *out_allocation_size = memory_info.allocationSize;

   return true;
}

static bool
yttrium_venus_shared_color_formats(enum pipe_format pipe_format,
                                    VkFormat views[2])
{
   switch (pipe_format) {
   case PIPE_FORMAT_R8G8B8A8_UNORM:
   case PIPE_FORMAT_R8G8B8X8_UNORM:
   case PIPE_FORMAT_R8G8B8A8_SRGB:
      views[0] = VK_FORMAT_R8G8B8A8_UNORM;
      views[1] = VK_FORMAT_R8G8B8A8_SRGB;
      return true;
   case PIPE_FORMAT_B8G8R8A8_UNORM:
   case PIPE_FORMAT_B8G8R8X8_UNORM:
   case PIPE_FORMAT_B8G8R8A8_SRGB:
      views[0] = VK_FORMAT_B8G8R8A8_UNORM;
      views[1] = VK_FORMAT_B8G8R8A8_SRGB;
      return true;
   default:
      return false;
   }
}

static VkResult
yttrium_venus_drm_modifiers(struct yttrium_venus *venus, VkFormat format,
                             VkDrmFormatModifierPropertiesEXT **out,
                             uint32_t *out_count);

/* The public ABI is one plane of linear bytes, not a Vulkan tiling enum.
 * Keep the legacy LINEAR descriptor where supported. Some DMA_BUF providers
 * instead require DRM_FORMAT_MOD_LINEAR with an explicit import layout. */
static VkResult
yttrium_venus_query_shared_public_image(
   struct yttrium_venus *venus, uint32_t width, uint32_t height,
   const VkFormat views[2], VkImageUsageFlags usage,
   VkImageCreateFlags flags, bool drm,
   VkExternalMemoryProperties *external_memory)
{
   memset(external_memory, 0, sizeof(*external_memory));
   if (drm && !venus->image_drm_format_modifier)
      return VK_ERROR_FORMAT_NOT_SUPPORTED;
   const VkFormatFeatureFlags required = VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
      VK_FORMAT_FEATURE_TRANSFER_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
      VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
   for (unsigned i = 0; i < 2; i++) {
      if (drm) {
         VkDrmFormatModifierPropertiesEXT *modifiers;
         uint32_t count;
         VkResult result = yttrium_venus_drm_modifiers(
            venus, views[i], &modifiers, &count);
         if (result != VK_SUCCESS)
            return result;
         bool supported = false;
         for (uint32_t j = 0; j < count; j++) {
            if (modifiers[j].drmFormatModifier == 0 &&
                modifiers[j].drmFormatModifierPlaneCount == 1 &&
                (modifiers[j].drmFormatModifierTilingFeatures & required) == required) {
               supported = true;
               break;
            }
         }
         FREE(modifiers);
         if (!supported)
            return VK_ERROR_FORMAT_NOT_SUPPORTED;
      } else {
         VkFormatProperties properties = {0};
         vn_call_vkGetPhysicalDeviceFormatProperties(
            &venus->vn_ring, venus->physical_device, views[i], &properties);
         if (venus->failed)
            return VK_ERROR_DEVICE_LOST;
         if ((properties.linearTilingFeatures & required) != required)
            return VK_ERROR_FORMAT_NOT_SUPPORTED;
      }
   }
   const VkImageFormatListCreateInfo format_list = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO,
      .viewFormatCount = 2, .pViewFormats = views,
   };
   const VkPhysicalDeviceImageDrmFormatModifierInfoEXT modifier_query = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT,
      .pNext = &format_list, .drmFormatModifier = 0,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
   };
   const VkPhysicalDeviceExternalImageFormatInfo external_query = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,
      .pNext = drm ? (const void *)&modifier_query : (const void *)&format_list,
      .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
   };
   const VkPhysicalDeviceImageFormatInfo2 query = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
      .pNext = &external_query, .format = views[0], .type = VK_IMAGE_TYPE_2D,
      .tiling = drm ? VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT : VK_IMAGE_TILING_LINEAR,
      .usage = usage, .flags = flags,
   };
   VkExternalImageFormatProperties external = {
      .sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES,
   };
   VkImageFormatProperties2 properties = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2, .pNext = &external,
   };
   const VkResult result = vn_call_vkGetPhysicalDeviceImageFormatProperties2(
      &venus->vn_ring, venus->physical_device, &query, &properties);
   if (venus->failed)
      return VK_ERROR_DEVICE_LOST;
   if (result != VK_SUCCESS)
      return result;
   *external_memory = external.externalMemoryProperties;
   const VkExternalMemoryFeatureFlags features =
      VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT | VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT;
   if (!yttrium_venus_image_properties_fit(&properties.imageFormatProperties,
          width, height, 1, 1, 1, VK_SAMPLE_COUNT_1_BIT) ||
       (external_memory->externalMemoryFeatures & features) != features ||
       !(external_memory->compatibleHandleTypes & VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT))
      return VK_ERROR_FORMAT_NOT_SUPPORTED;
   return VK_SUCCESS;
}

/* Both endpoints reproduce this public linear byte layout exactly. Its usage
 * omits COLOR_ATTACHMENT: Windows/KMD and other devices share its bytes,
 * whereas Vulkan rendering always targets a separate, private OPTIMAL image. */
static bool
yttrium_venus_create_shared_public_image(
   struct yttrium_venus *venus, struct yttrium_venus_resource *resource,
   uint32_t width, uint32_t height, enum pipe_format pipe_format,
   uint32_t resource_id, const VIOGPU_CREATE_ALLOCATION_EXCHANGE *allocation)
{
   VkFormat views[2];
   if (!yttrium_venus_shared_color_formats(pipe_format, views) ||
       !yttrium_venus_ensure_initialized(venus) || !venus->shared_image_supported)
      return false;
   const VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
      VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
   const VkImageCreateFlags flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
   VkExternalMemoryProperties external_memory;
   const VkResult legacy_result = yttrium_venus_query_shared_public_image(
      venus, width, height, views, usage, flags, false, &external_memory);
   VkResult result = legacy_result;
   bool drm = false;
   /* Unsupported capabilities may select the equivalent explicit LINEAR
    * representation. Never retry allocation, device or protocol failures. */
   if (result == VK_ERROR_FORMAT_NOT_SUPPORTED) {
      drm = true;
      result = yttrium_venus_query_shared_public_image(
         venus, width, height, views, usage, flags, true, &external_memory);
   }
   if (result != VK_SUCCESS) {
      YTTRIUM_WARN("yttrium: ERROR shared public image external query rejected owner=venus2 legacy_result=%d selected_result=%d extent=%ux%u features=0x%x action=fail-image\n",
                   legacy_result, result, width, height,
                   external_memory.externalMemoryFeatures);
      return false;
   }
   if (drm)
      YTTRIUM_LOG("yttrium: shared public image contract selected owner=venus2 component=shared-public-image reason=legacy-DMA_BUF-linear-unsupported action=use-DRM_FORMAT_MOD_LINEAR legacy_result=%d import_res_id=%u\n",
                   legacy_result, resource_id);
   const uint64_t linear_modifier = 0; /* DRM_FORMAT_MOD_LINEAR */
   const VkSubresourceLayout plane_layout = {
      .offset = allocation ? allocation->ScanoutOffset : 0,
      .rowPitch = allocation ? allocation->Stride : 0,
   };
   const VkImageDrmFormatModifierExplicitCreateInfoEXT explicit_modifier = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
      .drmFormatModifier = linear_modifier, .drmFormatModifierPlaneCount = 1,
      .pPlaneLayouts = &plane_layout,
   };
   const VkImageDrmFormatModifierListCreateInfoEXT modifier_list = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT,
      .drmFormatModifierCount = 1, .pDrmFormatModifiers = &linear_modifier,
   };
   const VkImageFormatListCreateInfo format_list = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO,
      .pNext = !drm ? NULL : resource_id ?
         (const void *)&explicit_modifier : (const void *)&modifier_list,
      .viewFormatCount = 2, .pViewFormats = views,
   };
   const VkExternalMemoryImageCreateInfo external_image = {
      .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
      .pNext = &format_list, .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
   };
   const VkImageCreateInfo create = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .pNext = &external_image,
      .flags = flags, .imageType = VK_IMAGE_TYPE_2D, .format = views[0],
      .extent = {width, height, 1}, .mipLevels = 1, .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = drm ? VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT : VK_IMAGE_TILING_LINEAR,
      .usage = usage, .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
   };
   yttrium_venus_init_object(venus, &resource->image_obj);
   resource->image = YTTRIUM_VENUS_HANDLE(VkImage, &resource->image_obj);
   result = vn_call_vkCreateImage(&venus->vn_ring, venus->device_handle,
                                 &create, NULL, &resource->image);
   if (result != VK_SUCCESS) {
      resource->image = VK_NULL_HANDLE;
      return false;
   }
   VkMemoryDedicatedRequirements dedicated = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS,
   };
   VkMemoryRequirements2 requirements = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, .pNext = &dedicated,
   };
   const VkImageMemoryRequirementsInfo2 requirements_query = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2, .image = resource->image,
   };
   vn_call_vkGetImageMemoryRequirements2(&venus->vn_ring, venus->device_handle,
                                        &requirements_query, &requirements);
   const VkMemoryRequirements *reqs = &requirements.memoryRequirements;
   VkMemoryResourcePropertiesMESA memory_properties = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_RESOURCE_PROPERTIES_MESA,
   };
   if (resource_id) {
      result = vn_call_vkGetMemoryResourcePropertiesMESA(
         &venus->vn_ring, venus->device_handle, resource_id, &memory_properties);
      if (result != VK_SUCCESS)
         goto discard_image;
   } else {
      memory_properties.memoryTypeBits = reqs->memoryTypeBits;
   }
   /* Runtime CPU blits read this public image, just as they read a native
    * display image. Preserve its readback policy instead of selecting a
    * device-local, write-combined backing for a CPU-readable resource.
    */
   const VkMemoryPropertyFlags required_memory =
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
      (resource->cpu_readback ? VK_MEMORY_PROPERTY_HOST_CACHED_BIT : 0);
   const VkMemoryPropertyFlags preferred_memory =
      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
      (resource->cpu_readback ? 0 : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
   const uint32_t memory_type = yttrium_venus_choose_memory_type(
      venus, reqs->memoryTypeBits & memory_properties.memoryTypeBits,
      required_memory, preferred_memory);
   if (!reqs->size || memory_type == UINT32_MAX ||
       (allocation && reqs->size > allocation->Size))
      goto discard_image;
   const VkImportMemoryResourceInfoMESA import = {
      .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_RESOURCE_INFO_MESA,
      .resourceId = resource_id,
   };
   const VkExportMemoryAllocateInfo export = yttrium_venus_export_memory_info();
   const VkMemoryDedicatedAllocateInfo dedicated_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
      .pNext = resource_id ? (const void *)&import : (const void *)&export,
      .image = resource->image,
   };
   const bool dedicated_allocation = dedicated.requiresDedicatedAllocation ||
      (external_memory.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT);
   const VkMemoryAllocateInfo memory_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .pNext = dedicated_allocation ? (const void *)&dedicated_info :
         resource_id ? (const void *)&import : (const void *)&export,
      .allocationSize = allocation ? allocation->Size : reqs->size,
      .memoryTypeIndex = memory_type,
   };
   yttrium_venus_init_object(venus, &resource->memory_obj);
   resource->memory = YTTRIUM_VENUS_HANDLE(VkDeviceMemory, &resource->memory_obj);
   if (yttrium_venus_allocate_memory(venus, &memory_info, reqs,
          &resource->memory, resource->memory_obj.id, "shared-public-image", true) != VK_SUCCESS)
      goto discard_image;
   result = vn_call_vkBindImageMemory(&venus->vn_ring, venus->device_handle,
                                     resource->image, resource->memory, 0);
   if (result != VK_SUCCESS) {
      yttrium_venus_discard_accepted_image(venus, resource, true);
      return false;
   }
   if (drm) {
      VkImageDrmFormatModifierPropertiesEXT modifier_properties = {
         .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT,
      };
      result = vn_call_vkGetImageDrmFormatModifierPropertiesEXT(
         &venus->vn_ring, venus->device_handle, resource->image, &modifier_properties);
      if (venus->failed || result != VK_SUCCESS ||
          modifier_properties.drmFormatModifier != linear_modifier) {
         YTTRIUM_WARN("yttrium: ERROR shared public modifier rejected owner=venus2 res_id=%u result=%d actual=0x%llx expected=0 action=fail-image\n",
                      resource_id, result,
                      (unsigned long long)modifier_properties.drmFormatModifier);
         yttrium_venus_discard_accepted_bound_image(venus, resource);
         return false;
      }
   }
   /* Memory-plane aspects describe DRM storage only. Image accesses and GPU
    * transitions continue to use COLOR for both linear representations. */
   const VkImageSubresource subresource = {
      .aspectMask = drm ? VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT : VK_IMAGE_ASPECT_COLOR_BIT,
   };
   VkSubresourceLayout layout = {0};
   if (!yttrium_venus_get_image_subresource_layout_checked(
          venus, resource, &subresource, &layout, "shared-public-image") ||
       layout.rowPitch < (uint64_t)width * 4 || layout.rowPitch > UINT32_MAX ||
       layout.offset > UINT32_MAX || layout.offset >= memory_info.allocationSize ||
       layout.size > memory_info.allocationSize - layout.offset ||
       (uint64_t)(height - 1) * layout.rowPitch + (uint64_t)width * 4 >
          memory_info.allocationSize - layout.offset ||
       (allocation && (layout.offset != allocation->ScanoutOffset ||
                       layout.rowPitch != allocation->Stride))) {
      YTTRIUM_WARN("yttrium: ERROR shared public plane rejected owner=venus2 res_id=%u offset=%llu pitch=%llu size=%llu action=fail-image\n",
                   resource_id, (unsigned long long)layout.offset,
                   (unsigned long long)layout.rowPitch, (unsigned long long)layout.size);
      yttrium_venus_discard_accepted_bound_image(venus, resource);
      return false;
   }
   resource->vk_format = views[0];
   resource->image_usage = usage;
   resource->width = width;
   resource->height = height;
   resource->depth = resource->levels = resource->layers = 1;
   resource->samples = VK_SAMPLE_COUNT_1_BIT;
   resource->layout = resource_id ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED;
   resource->allocation_size = memory_info.allocationSize;
   resource->image_offset = layout.offset;
   resource->image_size = layout.size;
   resource->image_row_pitch = layout.rowPitch;
   resource->image_array_pitch = layout.arrayPitch;
   resource->initialized = true;
   resource->contents_initialized = resource_id != 0;
   return true;

discard_image:
   YTTRIUM_WARN("yttrium: ERROR shared public memory rejected owner=venus2 res_id=%u required_size=%llu action=fail-image\n",
                resource_id, (unsigned long long)reqs->size);
   yttrium_venus_discard_accepted_image(venus, resource, false);
   return false;
}

/* Constructor rollback precedes publication and never owns render-batch work.
 * In particular, resource_fini would flush the ordered worker's active batch. */
static void
yttrium_venus_discard_unpublished_shared_image(
   struct yttrium_venus *venus, struct yttrium_venus_resource *resource)
{
   if (venus->failed || venus->retain_unretired_resources) {
      yttrium_venus2_retain_on_teardown(venus);
      return;
   }
   struct yttrium_venus_resource *images[] = {
      resource->display_endpoint, resource,
   };
   for (unsigned i = 0; i < ARRAY_SIZE(images); i++) {
      if (images[i] && images[i]->initialized) {
         vn_async_vkDestroyImage(&venus->vn_ring, venus->device_handle,
                                  images[i]->image, NULL);
         vn_async_vkFreeMemory(&venus->vn_ring, venus->device_handle,
                                images[i]->memory, NULL);
      }
   }
   if (venus->failed) {
      yttrium_venus2_retain_on_teardown(venus);
      return;
   }
   FREE(resource->display_endpoint);
   memset(resource, 0, sizeof(*resource));
}

static bool
yttrium_venus_create_shared_private_image(
   struct yttrium_venus *venus, struct yttrium_venus_resource *resource,
   uint32_t width, uint32_t height, enum pipe_format pipe_format, unsigned bind)
{
   bind &= ~(PIPE_BIND_SHARED | PIPE_BIND_DISPLAY_TARGET | PIPE_BIND_SCANOUT);
   return yttrium_venus2_create_texture_image_for_bind(venus, resource,
      PIPE_TEXTURE_2D, width, height, 1, 1, 1, pipe_format, 1, bind, NULL);
}

/* DMA_BUF does not carry a Vulkan image descriptor. Query each view against
 * the same explicit modifier rather than using generic OPTIMAL features. */
static VkResult
yttrium_venus_drm_modifiers(struct yttrium_venus *venus, VkFormat format,
                             VkDrmFormatModifierPropertiesEXT **out,
                             uint32_t *out_count)
{
   *out = NULL;
   *out_count = 0;
   VkDrmFormatModifierPropertiesListEXT list = {
      .sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT,
   };
   VkFormatProperties2 properties = {
      .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2, .pNext = &list,
   };
   vn_call_vkGetPhysicalDeviceFormatProperties2(
      &venus->vn_ring, venus->physical_device, format, &properties);
   if (venus->failed)
      return VK_ERROR_DEVICE_LOST;
   const uint32_t capacity = list.drmFormatModifierCount;
   if (!capacity || capacity > 256)
      return VK_ERROR_FORMAT_NOT_SUPPORTED;
   VkDrmFormatModifierPropertiesEXT *modifiers =
      CALLOC(capacity, sizeof(*modifiers));
   if (!modifiers)
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   list.pDrmFormatModifierProperties = modifiers;
   vn_call_vkGetPhysicalDeviceFormatProperties2(
      &venus->vn_ring, venus->physical_device, format, &properties);
   if (venus->failed || list.drmFormatModifierCount > capacity) {
      FREE(modifiers);
      return venus->failed ? VK_ERROR_DEVICE_LOST : VK_ERROR_UNKNOWN;
   }
   *out = modifiers;
   *out_count = list.drmFormatModifierCount;
   return VK_SUCCESS;
}

static VkResult
yttrium_venus_drm_view_supported(struct yttrium_venus *venus, VkFormat format,
                                  const struct yttrium_optimal_image_data *data,
                                  VkFormatFeatureFlags required)
{
   VkDrmFormatModifierPropertiesEXT *modifiers;
   uint32_t count;
   VkResult result = yttrium_venus_drm_modifiers(venus, format, &modifiers, &count);
   if (result != VK_SUCCESS)
      return result;
   result = VK_ERROR_FORMAT_NOT_SUPPORTED;
   for (uint32_t i = 0; i < count; i++) {
      if (modifiers[i].drmFormatModifier == data->drm_modifier &&
          modifiers[i].drmFormatModifierPlaneCount == data->drm_plane_count &&
          (modifiers[i].drmFormatModifierTilingFeatures & required) == required) {
         result = VK_SUCCESS;
         break;
      }
   }
   FREE(modifiers);
   return result;
}

static VkResult
yttrium_venus_query_optimal_shared_image(
   struct yttrium_venus *venus, uint32_t width, uint32_t height,
   const struct yttrium_optimal_image_data *data,
   VkExternalMemoryFeatureFlags *out_external_features,
   VkResult *out_query_result)
{
   *out_external_features = 0;
   const VkFormat views[2] = {data->view_formats[0], data->view_formats[1]};
   const uint32_t view_count = yttrium_optimal_image_view_count(data);
   const VkImageFormatListCreateInfo format_list = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO,
      .viewFormatCount = view_count, .pViewFormats = views,
   };
   const bool drm = data->version == YTTRIUM_OPTIMAL_IMAGE_DRM_VERSION;
   const VkPhysicalDeviceImageDrmFormatModifierInfoEXT modifier_query = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT,
      .pNext = &format_list, .drmFormatModifier = data->drm_modifier,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
   };
   const VkPhysicalDeviceExternalImageFormatInfo external_query = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,
      .pNext = drm ? (const void *)&modifier_query : (const void *)&format_list,
      .handleType = data->handle_types,
   };
   const VkPhysicalDeviceImageFormatInfo2 query = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
      .pNext = &external_query, .format = data->vk_format,
      .type = VK_IMAGE_TYPE_2D,
      .tiling = drm ? VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT : VK_IMAGE_TILING_OPTIMAL,
      .usage = data->vk_usage, .flags = data->vk_flags,
   };
   VkExternalImageFormatProperties external = {
      .sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES,
   };
   VkImageFormatProperties2 properties = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2, .pNext = &external,
   };
   const VkResult result = vn_call_vkGetPhysicalDeviceImageFormatProperties2(
      &venus->vn_ring, venus->physical_device, &query, &properties);
   *out_query_result = result;
   if (result != VK_SUCCESS)
      return result;
   const VkExternalMemoryProperties *memory = &external.externalMemoryProperties;
   *out_external_features = memory->externalMemoryFeatures;
   const VkExternalMemoryFeatureFlags required =
      VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT |
      VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT;
   if (!yttrium_venus_image_properties_fit(&properties.imageFormatProperties,
          width, height, 1, 1, 1, VK_SAMPLE_COUNT_1_BIT) ||
       (memory->externalMemoryFeatures & required) != required ||
       !(memory->compatibleHandleTypes & data->handle_types))
      return VK_ERROR_FORMAT_NOT_SUPPORTED;

   const VkFormatFeatureFlags common_features =
      VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT |
      ((data->vk_usage & VK_IMAGE_USAGE_SAMPLED_BIT) ?
         VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT : 0) |
      ((data->vk_usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) ?
         VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT : 0);
   for (unsigned i = 0; i < view_count; i++) {
      VkFormatProperties view_properties = {0};
      vn_call_vkGetPhysicalDeviceFormatProperties(
         &venus->vn_ring, venus->physical_device, views[i], &view_properties);
      if (venus->failed)
         return VK_ERROR_DEVICE_LOST;
      /* Preserve normal optimal-image filtering where the format supports it.
       * Integer formats do not require filtering to share sampled storage. */
      const VkFormatFeatureFlags filter_features =
         (data->vk_usage & VK_IMAGE_USAGE_SAMPLED_BIT) ?
            view_properties.optimalTilingFeatures &
               VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT : 0;
      /* Alternate sRGB views explicitly exclude inherited STORAGE usage.
       * Their matching UNORM backing retains native UAV support. */
      const VkFormatFeatureFlags required_format = common_features |
         filter_features |
         ((data->vk_usage & VK_IMAGE_USAGE_STORAGE_BIT) && i == 0 ?
            VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT : 0);
      if (drm) {
         VkResult view_result = yttrium_venus_drm_view_supported(
            venus, views[i], data, required_format);
         if (view_result != VK_SUCCESS)
            return view_result;
         continue;
      }
      if ((view_properties.optimalTilingFeatures & required_format) != required_format)
         return VK_ERROR_FORMAT_NOT_SUPPORTED;
   }
   return VK_SUCCESS;
}

static VkResult
yttrium_venus_select_shared_modifier(struct yttrium_venus *venus,
                                      uint32_t width, uint32_t height,
                                      struct yttrium_optimal_image_data *data,
                                      VkExternalMemoryFeatureFlags *features,
                                      VkResult *query_result)
{
   VkDrmFormatModifierPropertiesEXT *modifiers;
   uint32_t count;
   VkResult result = yttrium_venus_drm_modifiers(
      venus, data->vk_format, &modifiers, &count);
   if (result != VK_SUCCESS)
      return result;
   result = VK_ERROR_FORMAT_NOT_SUPPORTED;
   for (uint32_t i = 0; i < count; i++) {
      const VkDrmFormatModifierPropertiesEXT *modifier = &modifiers[i];
      /* A canonical render image must remain non-linear. Presentation owns
       * its separate LINEAR endpoint; it is not a sharing fallback. */
      if (!yttrium_optimal_image_modifier_valid(modifier->drmFormatModifier) ||
          !modifier->drmFormatModifierPlaneCount ||
          modifier->drmFormatModifierPlaneCount > YTTRIUM_OPTIMAL_IMAGE_MAX_DRM_PLANES)
         continue;
      data->drm_modifier = modifier->drmFormatModifier;
      data->drm_plane_count = modifier->drmFormatModifierPlaneCount;
      result = yttrium_venus_query_optimal_shared_image(
         venus, width, height, data, features, query_result);
      if (result != VK_ERROR_FORMAT_NOT_SUPPORTED)
         break;
   }
   FREE(modifiers);
   return result;
}

static bool
yttrium_venus_shared_modifier_layout(struct yttrium_venus *venus,
                                      struct yttrium_venus_resource *resource,
                                      struct yttrium_optimal_image_data *data,
                                      bool importing)
{
   VkImageDrmFormatModifierPropertiesEXT properties = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT,
   };
   VkResult result = vn_call_vkGetImageDrmFormatModifierPropertiesEXT(
      &venus->vn_ring, venus->device_handle, resource->image, &properties);
   if (venus->failed || result != VK_SUCCESS ||
       properties.drmFormatModifier != data->drm_modifier) {
      YTTRIUM_ERROR("yttrium: DMA_BUF shared modifier mismatch owner=venus2 result=%d selected=0x%llx actual=0x%llx action=fail-image\n",
                    result, (unsigned long long)data->drm_modifier,
                    (unsigned long long)properties.drmFormatModifier);
      return false;
   }
   for (uint32_t i = 0; i < data->drm_plane_count; i++) {
      const VkImageSubresource subresource = {
         .aspectMask = VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT << i,
      };
      VkSubresourceLayout layout = {0};
      if (!yttrium_venus_get_image_subresource_layout_checked(
             venus, resource, &subresource, &layout, "DMA_BUF-shared-image"))
         return false;
      if (!layout.rowPitch || layout.offset >= resource->allocation_size ||
          (importing && (layout.offset != data->drm_planes[i].offset ||
                         layout.rowPitch != data->drm_planes[i].row_pitch))) {
         YTTRIUM_ERROR("yttrium: DMA_BUF shared plane mismatch owner=venus2 plane=%u offset=%llu pitch=%llu expected_offset=%llu expected_pitch=%llu action=fail-image\n",
                       i, (unsigned long long)layout.offset,
                       (unsigned long long)layout.rowPitch,
                       (unsigned long long)data->drm_planes[i].offset,
                       (unsigned long long)data->drm_planes[i].row_pitch);
         return false;
      }
      if (!importing) {
         data->drm_planes[i].offset = layout.offset;
         data->drm_planes[i].row_pitch = layout.rowPitch;
      }
   }
   return true;
}

/* Keep discrete-device placement preferred even if a host-visible type has
 * a lower index. Explicit DMA_BUF metadata also permits device-local memory
 * exposed to the CPU; the image remains tiled, unmapped GPU storage. Legacy
 * OPAQUE_FD storage retains its qualified device-only allocation policy. */
static uint32_t
yttrium_venus_shared_memory_type(
   const VkPhysicalDeviceMemoryProperties *properties, uint32_t memory_type_bits,
   bool dma_buf)
{
   uint32_t host_visible_type = UINT32_MAX;
   for (uint32_t i = 0; i < properties->memoryTypeCount; i++) {
      const VkMemoryPropertyFlags flags = properties->memoryTypes[i].propertyFlags;
      if (!(memory_type_bits & (1u << i)) ||
          !(flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
         continue;
      if (!(flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
         return i;
      if (dma_buf && host_visible_type == UINT32_MAX)
         host_visible_type = i;
   }
   return host_visible_type;
}

/* Both endpoints recreate the creator's exact image descriptor. Version 4
 * uses DMA_BUF with explicit modifier planes; version 2 remains readable for
 * existing OPAQUE_FD allocations while processes transition to the new UMD. */
static bool
yttrium_venus_create_optimal_shared_storage(
   struct yttrium_venus *venus, struct yttrium_venus_resource *resource,
   uint32_t width, uint32_t height, uint32_t resource_id,
   struct yttrium_optimal_image_data *data)
{
   const bool drm = data->version == YTTRIUM_OPTIMAL_IMAGE_DRM_VERSION;
   if (!yttrium_venus_ensure_initialized(venus) || !venus->shared_image_supported ||
       (drm && !venus->image_drm_format_modifier) ||
       ((data->vk_usage & VK_IMAGE_USAGE_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT) &&
        !venus->attachment_feedback_loop_layout) ||
       ((data->vk_flags & VK_IMAGE_CREATE_MULTISAMPLED_RENDER_TO_SINGLE_SAMPLED_BIT_EXT) &&
        !venus->multisampled_render_to_single_sampled)) {
      YTTRIUM_WARN("yttrium: ERROR optimal shared image unsupported owner=venus2 res_id=%u reason=required-device-features-unavailable action=fail-image\n", resource_id);
      return false;
   }
   if (!venus->kmt_ctx || !venus->kmt_ctx->scheduled_present_mode) {
      YTTRIUM_WARN("yttrium: ERROR optimal shared image unsupported owner=venus2 res_id=%u reason=scheduled-submit-required-for-cross-device-ordering action=fail-image\n", resource_id);
      return false;
   }
   /* OPTIMAL external storage is an opaque driver/device layout. The current
    * renderer does not check these identities when attaching/importing an fd.
    * Query locally for both endpoints before accepting that implicit layout. */
   VkPhysicalDeviceIDProperties identity = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES,
   };
   VkPhysicalDeviceProperties2 device_properties = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &identity,
   };
   vn_call_vkGetPhysicalDeviceProperties2(&venus->vn_ring, venus->physical_device,
                                          &device_properties);
   if (venus->failed || !yttrium_shared_image_uuid_valid(identity.deviceUUID) ||
       !yttrium_shared_image_uuid_valid(identity.driverUUID)) {
      YTTRIUM_WARN("yttrium: ERROR optimal shared image identity unavailable owner=venus2 res_id=%u reason=missing-device-or-driver-UUID action=fail-image\n", resource_id);
      return false;
   }
   if (resource_id) {
      const bool same_device =
         !memcmp(data->device_uuid, identity.deviceUUID, VK_UUID_SIZE);
      const bool same_driver =
         !memcmp(data->driver_uuid, identity.driverUUID, VK_UUID_SIZE);
      if (!same_device || !same_driver) {
         YTTRIUM_WARN("yttrium: ERROR optimal shared image identity rejected owner=venus2 res_id=%u same_device=%u same_driver=%u reason=opaque-layout-UUID-mismatch action=fail-image\n",
                      resource_id, same_device, same_driver);
         return false;
      }
   } else {
      memcpy(data->device_uuid, identity.deviceUUID, VK_UUID_SIZE);
      memcpy(data->driver_uuid, identity.driverUUID, VK_UUID_SIZE);
   }
   VkExternalMemoryFeatureFlags external_features = 0;
   VkResult raw_query_result = VK_SUCCESS;
   VkResult result = drm && !resource_id ?
      yttrium_venus_select_shared_modifier(
         venus, width, height, data, &external_features, &raw_query_result) :
      yttrium_venus_query_optimal_shared_image(
         venus, width, height, data, &external_features, &raw_query_result);
   if (!resource_id && result == VK_ERROR_FORMAT_NOT_SUPPORTED &&
       (data->vk_usage & VK_IMAGE_USAGE_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT)) {
      /* The optional feedback bit is selected once against the exact external
       * image descriptor, then persisted for every alias. */
      data->vk_usage &= ~VK_IMAGE_USAGE_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT;
      result = drm ? yttrium_venus_select_shared_modifier(
         venus, width, height, data, &external_features, &raw_query_result) :
         yttrium_venus_query_optimal_shared_image(
            venus, width, height, data, &external_features, &raw_query_result);
      if (result == VK_SUCCESS)
         YTTRIUM_WARN("yttrium: WARNING optimal shared optional feedback unavailable owner=venus2 component=shared-image reason=external-image-descriptor-unsupported action=create-with-required-usage actual-feedback-draws=reject\n");
   }
   if (result != VK_SUCCESS) {
      YTTRIUM_WARN("yttrium: ERROR optimal shared external image query failed owner=venus2 res_id=%u result=%d capability_result=%d format=%u usage=0x%x flags=0x%x external_features=0x%x action=fail-image\n",
                   resource_id, raw_query_result, result, data->vk_format, data->vk_usage,
                   data->vk_flags, external_features);
      return false;
   }
   const VkFormat views[2] = {data->view_formats[0], data->view_formats[1]};
   const VkImageFormatListCreateInfo format_list = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO,
      .viewFormatCount = yttrium_optimal_image_view_count(data),
      .pViewFormats = views,
   };
   VkSubresourceLayout plane_layouts[YTTRIUM_OPTIMAL_IMAGE_MAX_DRM_PLANES] = {0};
   for (uint32_t i = 0; i < data->drm_plane_count; i++) {
      plane_layouts[i].offset = data->drm_planes[i].offset;
      plane_layouts[i].rowPitch = data->drm_planes[i].row_pitch;
      /* For these single-layer 2D images size/arrayPitch/depthPitch are zero
       * in the explicit import descriptor, as required by Vulkan. */
   }
   const VkImageDrmFormatModifierExplicitCreateInfoEXT explicit_modifier = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
      .drmFormatModifier = data->drm_modifier,
      .drmFormatModifierPlaneCount = data->drm_plane_count,
      .pPlaneLayouts = plane_layouts,
   };
   const VkImageDrmFormatModifierListCreateInfoEXT modifier_list = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT,
      .drmFormatModifierCount = 1, .pDrmFormatModifiers = &data->drm_modifier,
   };
   VkImageFormatListCreateInfo shared_format_list = format_list;
   shared_format_list.pNext = !drm ? NULL : resource_id ?
      (const void *)&explicit_modifier : (const void *)&modifier_list;
   const VkExternalMemoryImageCreateInfo external_image = {
      .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
      .pNext = &shared_format_list, .handleTypes = data->handle_types,
   };
   const VkImageCreateInfo create = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .pNext = &external_image,
      .flags = data->vk_flags, .imageType = VK_IMAGE_TYPE_2D,
      .format = data->vk_format, .extent = {width, height, 1},
      .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = drm ? VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT : VK_IMAGE_TILING_OPTIMAL,
      .usage = data->vk_usage,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
   };
   yttrium_venus_init_object(venus, &resource->image_obj);
   resource->image = YTTRIUM_VENUS_HANDLE(VkImage, &resource->image_obj);
   result = vn_call_vkCreateImage(&venus->vn_ring, venus->device_handle,
                                 &create, NULL, &resource->image);
   if (result != VK_SUCCESS) {
      YTTRIUM_WARN("yttrium: ERROR optimal shared image creation failed owner=venus2 res_id=%u result=%d action=fail-image\n", resource_id, result);
      resource->image = VK_NULL_HANDLE;
      return false;
   }
   VkMemoryDedicatedRequirements dedicated = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS,
   };
   VkMemoryRequirements2 requirements = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, .pNext = &dedicated,
   };
   const VkImageMemoryRequirementsInfo2 requirements_query = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2,
      .image = resource->image,
   };
   vn_call_vkGetImageMemoryRequirements2(&venus->vn_ring, venus->device_handle,
                                        &requirements_query, &requirements);
   const VkMemoryRequirements *reqs = &requirements.memoryRequirements;
   const bool requires_dedicated = dedicated.requiresDedicatedAllocation ||
      (external_features & VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT);
   if (resource_id) {
      /* OPAQUE needs the creator's memory type. DMA_BUF additionally exposes
       * the actual FD's compatibility mask; intersect it with image needs. */
      if (drm) {
         VkMemoryResourcePropertiesMESA properties = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_RESOURCE_PROPERTIES_MESA,
         };
         result = vn_call_vkGetMemoryResourcePropertiesMESA(
            &venus->vn_ring, venus->device_handle, resource_id, &properties);
         if (venus->failed || result != VK_SUCCESS)
            goto discard_image;
         requirements.memoryRequirements.memoryTypeBits &= properties.memoryTypeBits;
      }
      if (data->memory_type_index >= venus->memory_props.memoryTypeCount ||
          !(reqs->memoryTypeBits & (1u << data->memory_type_index)) ||
          (drm ? data->allocation.Size < reqs->size : data->allocation.Size != reqs->size) ||
          data->memory_alignment != reqs->alignment ||
          (requires_dedicated && !data->dedicated))
         goto discard_image;
   } else {
      data->memory_type_index = yttrium_venus_shared_memory_type(
         &venus->memory_props, reqs->memoryTypeBits, drm);
      if (data->memory_type_index == UINT32_MAX)
         YTTRIUM_WARN("yttrium: ERROR optimal shared memory unsupported owner=venus2 component=shared-image reason=%s image_bits=0x%x action=fail-image\n",
                      drm ? "no-compatible-device-local-memory" :
                         "no-compatible-non-host-visible-device-local-memory",
                      reqs->memoryTypeBits);
      data->memory_alignment = reqs->alignment;
      data->dedicated = drm || requires_dedicated || dedicated.prefersDedicatedAllocation;
   }
   if (venus->failed || !reqs->size || reqs->size > UINT32_MAX ||
       !reqs->alignment || data->memory_type_index == UINT32_MAX)
      goto discard_image;
   const VkMemoryPropertyFlags memory_flags =
      venus->memory_props.memoryTypes[data->memory_type_index].propertyFlags;
   if (!(memory_flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) ||
       (!drm && (memory_flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))) {
      YTTRIUM_WARN("yttrium: ERROR optimal shared memory unsupported owner=venus2 component=shared-image res_id=%u memory_type=%u property_flags=0x%x reason=%s action=fail-image\n",
                   resource_id, data->memory_type_index, memory_flags,
                   drm ? "canonical-shared-storage-requires-device-local-memory" :
                      "legacy-opaque-storage-requires-non-host-visible-device-local-memory");
      goto discard_image;
   }
   if (memory_flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
      YTTRIUM_WARN("yttrium: WARNING optimal shared placement fallback=DMA_BUF-host-visible-device-local owner=venus2 component=shared-image res_id=%u memory_type=%u property_flags=0x%x reason=%s action=use-unmapped-tiled-GPU-storage\n",
                   resource_id, data->memory_type_index, memory_flags,
                   resource_id ? "preserve-exporter-memory-type-for-import" :
                      "no-compatible-non-host-visible-device-local-memory");
   }
   const VkImportMemoryResourceInfoMESA import = {
      .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_RESOURCE_INFO_MESA,
      .resourceId = resource_id,
   };
   const VkExportMemoryAllocateInfo export = {
      .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
      .handleTypes = data->handle_types,
   };
   const VkMemoryDedicatedAllocateInfo dedicated_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
      .pNext = resource_id ? (const void *)&import : (const void *)&export,
      .image = resource->image,
   };
   const VkMemoryAllocateInfo memory_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .pNext = data->dedicated ? (const void *)&dedicated_info :
         resource_id ? (const void *)&import : (const void *)&export,
      .allocationSize = resource_id ? data->allocation.Size : reqs->size,
      .memoryTypeIndex = data->memory_type_index,
   };
   yttrium_venus_init_object(venus, &resource->memory_obj);
   resource->memory = YTTRIUM_VENUS_HANDLE(VkDeviceMemory, &resource->memory_obj);
   result = yttrium_venus_allocate_memory(venus, &memory_info, reqs,
      &resource->memory, resource->memory_obj.id, "optimal-shared-image", true);
   if (result != VK_SUCCESS)
      goto discard_image;
   result = vn_call_vkBindImageMemory(&venus->vn_ring, venus->device_handle,
                                     resource->image, resource->memory, 0);
   if (result != VK_SUCCESS) {
      YTTRIUM_WARN("yttrium: ERROR optimal shared image memory bind failed owner=venus2 res_id=%u result=%d action=fail-image\n", resource_id, result);
      yttrium_venus_discard_accepted_image(venus, resource, true);
      return false;
   }
   resource->vk_format = data->vk_format;
   resource->image_usage = data->vk_usage;
   resource->width = width;
   resource->height = height;
   resource->depth = resource->levels = resource->layers = 1;
   resource->samples = VK_SAMPLE_COUNT_1_BIT;
   resource->layout = resource_id ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED;
   resource->allocation_size = memory_info.allocationSize;
   resource->image_size = reqs->size;
   resource->image_offset = resource->image_row_pitch = resource->image_array_pitch = 0;
   resource->initialized = true;
   resource->buffer_backed = false;
   if (drm && !yttrium_venus_shared_modifier_layout(venus, resource, data, resource_id != 0)) {
      yttrium_venus_discard_accepted_bound_image(venus, resource);
      return false;
   }
   resource->contents_initialized = resource_id != 0;
   resource->initialized_aspects = resource_id ? VK_IMAGE_ASPECT_COLOR_BIT : 0;
   if (!resource_id && !yttrium_venus_initialize_shared_public_image(venus, resource)) {
      yttrium_venus_discard_unpublished_shared_image(venus, resource);
      return false;
   }
   resource->initialized_aspects = VK_IMAGE_ASPECT_COLOR_BIT;
   resource->external_image = (struct yttrium_venus_external_image) {
      .image = resource->image, .layout = &resource->layout, .enabled = true,
   };
   return true;

discard_image:
   YTTRIUM_WARN("yttrium: ERROR optimal shared image memory contract rejected owner=venus2 res_id=%u result=%d required_size=%llu shared_size=%llu required_alignment=%llu shared_alignment=%llu image_bits=0x%x memory_type=%u dedicated_required=%u dedicated_shared=%u action=fail-image\n",
                resource_id, result, (unsigned long long)reqs->size,
                (unsigned long long)data->allocation.Size,
                (unsigned long long)reqs->alignment,
                (unsigned long long)data->memory_alignment,
                reqs->memoryTypeBits,
                data->memory_type_index, requires_dedicated, data->dedicated);
   yttrium_venus_discard_accepted_image(venus, resource, false);
   return false;
}

bool
yttrium_venus2_create_optimal_shared_image(
   struct yttrium_venus *venus, struct yttrium_venus_resource *resource,
   uint32_t width, uint32_t height, enum pipe_format pipe_format,
   unsigned bind, struct yttrium_optimal_image_data *data,
   uint64_t *out_memory_id, uint64_t *out_allocation_size)
{
   if (out_memory_id) *out_memory_id = 0;
   if (out_allocation_size) *out_allocation_size = 0;
   if (!resource || resource->initialized || resource->cpu_readback || !data ||
       !width || width > 16384 || !height || height > 16384 ||
       !(bind & PIPE_BIND_SHARED) ||
       (bind & ~(PIPE_BIND_SHARED | PIPE_BIND_DISPLAY_TARGET |
                 PIPE_BIND_RENDER_TARGET | PIPE_BIND_SAMPLER_VIEW |
                 PIPE_BIND_SHADER_IMAGE)) ||
       !yttrium_venus_ensure_initialized(venus)) {
      YTTRIUM_WARN("yttrium: ERROR optimal shared image create rejected owner=venus2 reason=unsupported-resource-contract action=fail-allocation\n");
      return false;
   }
   uint32_t resource_format = 0;
   switch (pipe_format) {
#define YTTRIUM_SHARED_PIPE_FORMAT(name, dxgi, vk, alternate) \
   case PIPE_FORMAT_##name: resource_format = VIRGL_FORMAT_##name; break;
      YTTRIUM_SHARED_COLOR_FORMATS(YTTRIUM_SHARED_PIPE_FORMAT)
#undef YTTRIUM_SHARED_PIPE_FORMAT
   default:
      YTTRIUM_WARN("yttrium: ERROR optimal shared image create rejected owner=venus2 reason=unsupported-format format=%u action=fail-allocation\n", pipe_format);
      return false;
   }
   struct yttrium_shared_color_format format;
   if (!yttrium_shared_color_format(resource_format, &format))
      return false;
   struct yttrium_optimal_image_data descriptor = {
      .magic = YTTRIUM_MESA_SHARED_IMAGE_MAGIC,
      .version = YTTRIUM_OPTIMAL_IMAGE_DRM_VERSION,
      .bytes = YTTRIUM_OPTIMAL_IMAGE_DRM_BYTES,
      .dxgi_format = format.dxgi_format, .vk_format = format.vk_format,
      .vk_usage = yttrium_optimal_image_usage(bind),
      .handle_types = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
      .view_formats = {format.vk_format, format.alternate_view_format},
   };
   if (venus->attachment_feedback_loop_layout &&
       (bind & PIPE_BIND_RENDER_TARGET) && (bind & PIPE_BIND_SAMPLER_VIEW))
      descriptor.vk_usage |= VK_IMAGE_USAGE_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT;
   descriptor.vk_flags = yttrium_venus_image_create_flags(
      venus, VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT, descriptor.vk_usage,
      VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT);
   if (!yttrium_venus_create_optimal_shared_storage(
          venus, resource, width, height, 0, &descriptor))
      return false;
   *data = descriptor;
   if (out_memory_id) *out_memory_id = resource->memory_obj.id;
   if (out_allocation_size) *out_allocation_size = resource->allocation_size;
   return true;
}

bool
yttrium_venus2_import_optimal_shared_image(
   struct yttrium_venus *venus, struct yttrium_venus_resource *resource,
   uint32_t resource_id, const struct yttrium_optimal_image_data *data)
{
   if (!resource || resource->initialized || resource->cpu_readback ||
       !resource_id || !yttrium_mesa_optimal_image_valid(data)) {
      YTTRIUM_WARN("yttrium: ERROR optimal shared image import rejected owner=venus2 res_id=%u reason=invalid-optimal-metadata action=fail-import\n", resource_id);
      return false;
   }
   struct yttrium_optimal_image_data descriptor = *data;
   return yttrium_venus_create_optimal_shared_storage(venus, resource,
      data->allocation.ResourceOptions.width, data->allocation.ResourceOptions.height,
      resource_id, &descriptor);
}

bool
yttrium_venus2_create_presentable_image(
   struct yttrium_venus *venus, struct yttrium_venus_resource *resource,
   uint32_t width, uint32_t height, enum pipe_format pipe_format, unsigned bind,
   uint64_t *out_endpoint_memory_id, uint64_t *out_endpoint_allocation_size)
{
   if (out_endpoint_memory_id) *out_endpoint_memory_id = 0;
   if (out_endpoint_allocation_size) *out_endpoint_allocation_size = 0;
   const unsigned required = PIPE_BIND_RENDER_TARGET | PIPE_BIND_DISPLAY_TARGET;
   const unsigned allowed = required | PIPE_BIND_SAMPLER_VIEW;
   if (!resource || resource->initialized ||
       (pipe_format != PIPE_FORMAT_R8G8B8A8_UNORM &&
        pipe_format != PIPE_FORMAT_R8G8B8A8_SRGB &&
        pipe_format != PIPE_FORMAT_B8G8R8A8_UNORM &&
        pipe_format != PIPE_FORMAT_B8G8R8A8_SRGB) ||
       !width || width > 16384 || !height || height > 16384 ||
       (bind & required) != required || (bind & ~allowed) ||
       !yttrium_venus_ensure_initialized(venus) || !venus->kmt_ctx ||
       !venus->kmt_ctx->scheduled_present_mode) {
      YTTRIUM_WARN("yttrium: ERROR presentable image rejected owner=venus2 reason=unsupported-local-present-or-unscheduled-contract action=fail-resource\n");
      return false;
   }
   if (!yttrium_venus_create_shared_private_image(
          venus, resource, width, height, pipe_format, bind))
      return false;
   resource->display_endpoint = CALLOC_STRUCT(yttrium_venus_resource);
   struct yttrium_venus_resource *endpoint = resource->display_endpoint;
   if (endpoint)
      endpoint->cpu_readback = resource->cpu_readback;
   if (!endpoint ||
       !yttrium_venus_create_shared_public_image(
          venus, endpoint, width, height, pipe_format, 0, NULL) ||
       !yttrium_venus_initialize_shared_public_image(venus, endpoint)) {
      yttrium_venus_discard_unpublished_shared_image(venus, resource);
      return false;
   }
   /* The parent's pipe references also protect this internal endpoint record.
    * Initialization is a cold constructor operation, never a Present drain. */
   endpoint->owner = resource->owner;
   endpoint->initialized_aspects = VK_IMAGE_ASPECT_COLOR_BIT;
   endpoint->external_image = (struct yttrium_venus_external_image) {
      .image = endpoint->image, .layout = &endpoint->layout, .enabled = true,
   };
   resource->display_endpoint_state.public_current = true;
   if (out_endpoint_memory_id) *out_endpoint_memory_id = endpoint->memory_obj.id;
   if (out_endpoint_allocation_size)
      *out_endpoint_allocation_size = endpoint->allocation_size;
   return true;
}

bool
yttrium_venus2_import_display_endpoint(
   struct yttrium_venus *venus, struct yttrium_venus_resource *resource,
   uint32_t resource_id, const struct yttrium_display_endpoint_data *data)
{
   if (!resource || resource->initialized || !resource_id ||
       !yttrium_display_endpoint_valid(data) ||
       !yttrium_venus_ensure_initialized(venus) || !venus->kmt_ctx ||
       !venus->kmt_ctx->scheduled_present_mode) {
      YTTRIUM_WARN("yttrium: ERROR display endpoint import rejected owner=venus2 res_id=%u reason=invalid-v3-or-unscheduled-contract action=fail-import\n", resource_id);
      return false;
   }
   enum pipe_format logical_format;
   switch (data->format) {
   case 28: logical_format = PIPE_FORMAT_R8G8B8A8_UNORM; break;
   case 29: logical_format = PIPE_FORMAT_R8G8B8A8_SRGB; break;
   case 87: logical_format = PIPE_FORMAT_B8G8R8A8_UNORM; break;
   case 91: logical_format = PIPE_FORMAT_B8G8R8A8_SRGB; break;
   default:
      return false;
   }
   const VIOGPU_CREATE_ALLOCATION_EXCHANGE *a = &data->allocation;
   if (!yttrium_venus_create_shared_public_image(
          venus, resource, a->ResourceOptions.width, a->ResourceOptions.height,
          logical_format, resource_id, a))
      return false;
   resource->display_endpoint_imported = true;
   resource->initialized_aspects = VK_IMAGE_ASPECT_COLOR_BIT;
   resource->external_image = (struct yttrium_venus_external_image) {
      .image = resource->image, .layout = &resource->layout, .enabled = true,
   };
   return true;
}

bool
yttrium_venus2_create_color_attachment_image(struct yttrium_venus *venus,
                                            struct yttrium_venus_resource *resource,
                                            uint32_t width,
                                            uint32_t height,
                                            uint32_t levels,
                                            uint32_t layers,
                                            enum pipe_format pipe_format,
                                            uint64_t *out_allocation_size)
{
   const VkFormat vk_format = yttrium_venus2_pipe_format(pipe_format);

   if (out_allocation_size)
      *out_allocation_size = 0;

   if (!resource || vk_format == VK_FORMAT_UNDEFINED ||
       yttrium_venus_format_has_depth(vk_format))
      return false;

   if (resource->initialized) {
      if (resource->buffer_backed || !resource->memory_obj.id)
         return false;
      if (out_allocation_size)
         *out_allocation_size = resource->allocation_size;
      return true;
   }

   if (!yttrium_venus_ensure_initialized(venus))
      return false;

   VkImageUsageFlags usage =
      VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
      VK_IMAGE_USAGE_TRANSFER_DST_BIT |
      VK_IMAGE_USAGE_SAMPLED_BIT |
      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
   if (!yttrium_venus_select_feedback_loop_usage(
          venus, vk_format, VK_IMAGE_TYPE_2D, width, height, 1, levels,
          layers, 0, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL, &usage))
      return false;
   if (!yttrium_venus_create_image_tiled(venus, resource, VK_IMAGE_TYPE_2D,
                                         vk_format, width, height, 1,
                                         levels, layers, 0, usage,
                                         VK_SAMPLE_COUNT_1_BIT,
                                         VK_IMAGE_TILING_OPTIMAL,
                                         VK_IMAGE_LAYOUT_UNDEFINED))
      return false;

   VkMemoryRequirements reqs;
   memset(&reqs, 0, sizeof(reqs));
   vn_call_vkGetImageMemoryRequirements(&venus->vn_ring,
                                        venus->device_handle,
                                        resource->image, &reqs);

   const uint32_t memory_type_index =
      yttrium_venus_choose_memory_type(venus, reqs.memoryTypeBits, 0,
                                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
   if (memory_type_index == UINT32_MAX) {
      YTTRIUM_LOG("yttrium: Venus no memory type for color attachment image bits=0x%x %ux%u format=%u req_size=0x%llx\n",
                  reqs.memoryTypeBits, width, height, pipe_format,
                  (unsigned long long)reqs.size);
      vn_async_vkDestroyImage(&venus->vn_ring, venus->device_handle,
                              resource->image, NULL);
      memset(resource, 0, sizeof(*resource));
      return false;
   }

   yttrium_venus_init_object(venus, &resource->memory_obj);
   resource->memory =
      YTTRIUM_VENUS_HANDLE(VkDeviceMemory, &resource->memory_obj);

   const VkMemoryAllocateInfo memory_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = reqs.size,
      .memoryTypeIndex = memory_type_index,
   };
   if (yttrium_venus_allocate_memory(
          venus, &memory_info, &reqs, &resource->memory,
          resource->memory_obj.id, "color-attachment", false) != VK_SUCCESS) {
      yttrium_venus_discard_accepted_image(venus, resource, false);
      return false;
   }

   struct vn_ring_submit_command bind_submit;
   vn_submit_vkBindImageMemory(&venus->vn_ring, 0, venus->device_handle,
                               resource->image, resource->memory, 0,
                               &bind_submit);
   if (!yttrium_venus_async_submit_succeeded(
          venus, &bind_submit, "vkBindImageMemory(color-attachment)",
          resource->image_obj.id)) {
      yttrium_venus_discard_accepted_image(venus, resource, true);
      return false;
   }

   resource->layout = VK_IMAGE_LAYOUT_UNDEFINED;
   resource->allocation_size = memory_info.allocationSize;
   resource->image_offset = 0;
   resource->image_size = reqs.size;
   resource->image_row_pitch = 0;
   resource->image_array_pitch = 0;
   resource->initialized = true;
   resource->buffer_backed = false;

   if (out_allocation_size)
      *out_allocation_size = memory_info.allocationSize;

   return true;
}

static bool
yttrium_venus2_sampled_image_supported(struct yttrium_venus *venus,
                                       VkFormat vk_format,
                                       VkImageType image_type,
                                       uint32_t width,
                                       uint32_t height,
                                       uint32_t depth,
                                       uint32_t levels,
                                       uint32_t layers,
                                       VkSampleCountFlagBits samples,
                                       VkImageCreateFlags image_flags,
                                       VkImageUsageFlags usage,
                                       VkImageTiling tiling);

bool
yttrium_venus2_create_sampled_texture_image(struct yttrium_venus *venus,
                                           struct yttrium_venus_resource *resource,
                                           enum pipe_texture_target target,
                                           uint32_t width,
                                           uint32_t height,
                                           uint32_t depth,
                                           uint32_t levels,
                                           uint32_t layers,
                                           enum pipe_format pipe_format,
                                           uint64_t *out_allocation_size)
{
   const VkFormat vk_format = yttrium_venus2_pipe_format(pipe_format);

   if (out_allocation_size)
      *out_allocation_size = 0;

   if (!resource || vk_format == VK_FORMAT_UNDEFINED ||
       yttrium_venus_format_has_depth(vk_format))
      return false;

   if (resource->initialized) {
      if (resource->buffer_backed || !resource->memory_obj.id)
         return false;
      if (out_allocation_size)
         *out_allocation_size = resource->allocation_size;
      return true;
   }

   if (!yttrium_venus_ensure_initialized(venus))
      return false;

   VkImageType image_type;
   VkImageCreateFlags image_flags = 0;
   uint32_t image_height = MAX2(height, 1);
   uint32_t image_depth = 1;
   uint32_t image_layers = MAX2(layers, 1);
   switch (target) {
   case PIPE_TEXTURE_1D:
   case PIPE_TEXTURE_1D_ARRAY:
      image_type = VK_IMAGE_TYPE_1D;
      image_height = 1;
      break;
   case PIPE_TEXTURE_2D:
   case PIPE_TEXTURE_2D_ARRAY:
      image_type = VK_IMAGE_TYPE_2D;
      if (target == PIPE_TEXTURE_2D_ARRAY &&
          width == image_height && image_layers >= 6 && image_layers % 6 == 0)
         image_flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
      break;
   case PIPE_TEXTURE_CUBE:
   case PIPE_TEXTURE_CUBE_ARRAY:
      if (width != height || image_layers < 6 || image_layers % 6)
         return false;
      image_type = VK_IMAGE_TYPE_2D;
      image_flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
      break;
   case PIPE_TEXTURE_3D:
      image_type = VK_IMAGE_TYPE_3D;
      image_depth = MAX2(depth, 1);
      image_layers = 1;
      break;
   default:
      return false;
   }

   const VkImageUsageFlags usage =
      VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
      VK_IMAGE_USAGE_TRANSFER_DST_BIT |
      VK_IMAGE_USAGE_SAMPLED_BIT;
   VkImageTiling tiling = VK_IMAGE_TILING_OPTIMAL;
   if (!yttrium_venus2_sampled_image_supported(venus, vk_format, image_type,
                                                width, image_height, image_depth,
                                                levels, image_layers,
                                                VK_SAMPLE_COUNT_1_BIT,
                                                image_flags, usage, tiling)) {
      /* Some native formats, notably RGB32 on RADV, are sampleable only as
       * linear images.  Preserve the exact format and keep the resource on
       * the GPU instead of rejecting the draw or introducing a CPU path.
       */
      tiling = VK_IMAGE_TILING_LINEAR;
      if (!yttrium_venus2_sampled_image_supported(
             venus, vk_format, image_type, width, image_height, image_depth,
             levels, image_layers, VK_SAMPLE_COUNT_1_BIT, image_flags, usage,
             tiling))
         return false;

      YTTRIUM_LOG("yttrium: Venus sampled texture uses linear tiling "
                  "target=%u extent=%ux%ux%u levels=%u layers=%u "
                  "format=%u vk_format=%u\n",
                  target, width, image_height, image_depth, levels,
                  image_layers, pipe_format, vk_format);
   }
   if (!yttrium_venus_create_image_tiled(
          venus, resource, image_type, vk_format, width, image_height,
          image_depth, levels, image_layers, image_flags, usage,
          VK_SAMPLE_COUNT_1_BIT,
          tiling, VK_IMAGE_LAYOUT_UNDEFINED))
      return false;

   VkMemoryRequirements reqs;
   memset(&reqs, 0, sizeof(reqs));
   vn_call_vkGetImageMemoryRequirements(&venus->vn_ring,
                                        venus->device_handle,
                                        resource->image, &reqs);

   const uint32_t memory_type_index =
      yttrium_venus_choose_memory_type(venus, reqs.memoryTypeBits, 0,
                                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
   if (memory_type_index == UINT32_MAX) {
      YTTRIUM_LOG("yttrium: Venus no memory type for sampled texture image bits=0x%x target=%u %ux%ux%u format=%u req_size=0x%llx\n",
                  reqs.memoryTypeBits, target, width, image_height,
                  image_depth, pipe_format, (unsigned long long)reqs.size);
      vn_async_vkDestroyImage(&venus->vn_ring, venus->device_handle,
                              resource->image, NULL);
      memset(resource, 0, sizeof(*resource));
      return false;
   }

   yttrium_venus_init_object(venus, &resource->memory_obj);
   resource->memory =
      YTTRIUM_VENUS_HANDLE(VkDeviceMemory, &resource->memory_obj);

   const VkMemoryAllocateInfo memory_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = reqs.size,
      .memoryTypeIndex = memory_type_index,
   };
   if (yttrium_venus_allocate_memory(
          venus, &memory_info, &reqs, &resource->memory,
          resource->memory_obj.id, "sampled-texture", false) != VK_SUCCESS) {
      yttrium_venus_discard_accepted_image(venus, resource, false);
      return false;
   }

   struct vn_ring_submit_command bind_submit;
   vn_submit_vkBindImageMemory(&venus->vn_ring, 0, venus->device_handle,
                               resource->image, resource->memory, 0,
                               &bind_submit);
   if (!yttrium_venus_async_submit_succeeded(
          venus, &bind_submit, "vkBindImageMemory(sampled-texture)",
          resource->image_obj.id)) {
      yttrium_venus_discard_accepted_image(venus, resource, true);
      return false;
   }

   resource->layout = VK_IMAGE_LAYOUT_UNDEFINED;
   resource->allocation_size = memory_info.allocationSize;
   resource->image_offset = 0;
   resource->image_size = reqs.size;
   resource->image_row_pitch = 0;
   resource->image_array_pitch = 0;
   resource->initialized = true;
   resource->buffer_backed = false;

   if (out_allocation_size)
      *out_allocation_size = memory_info.allocationSize;

   return true;
}

static bool
yttrium_venus_texture_target_params(enum pipe_texture_target target,
                                    uint32_t height,
                                    uint32_t depth,
                                    uint32_t layers,
                                    VkImageType *image_type,
                                    uint32_t *image_height,
                                    uint32_t *image_depth,
                                    uint32_t *image_layers)
{
   switch (target) {
   case PIPE_TEXTURE_1D:
   case PIPE_TEXTURE_1D_ARRAY:
      *image_type = VK_IMAGE_TYPE_1D;
      *image_height = 1;
      *image_depth = 1;
      *image_layers = MAX2(layers, 1);
      return true;
   case PIPE_TEXTURE_2D:
   case PIPE_TEXTURE_2D_ARRAY:
   case PIPE_TEXTURE_RECT:
      *image_type = VK_IMAGE_TYPE_2D;
      *image_height = MAX2(height, 1);
      *image_depth = 1;
      *image_layers = MAX2(layers, 1);
      return true;
   case PIPE_TEXTURE_CUBE:
   case PIPE_TEXTURE_CUBE_ARRAY:
      *image_type = VK_IMAGE_TYPE_2D;
      *image_height = MAX2(height, 1);
      *image_depth = 1;
      *image_layers = MAX2(layers, 1);
      return true;
   case PIPE_TEXTURE_3D:
      *image_type = VK_IMAGE_TYPE_3D;
      *image_height = MAX2(height, 1);
      *image_depth = MAX2(depth, 1);
      *image_layers = 1;
      return true;
   default:
      return false;
   }
}

static VkImageCreateFlags
yttrium_venus_image_flags_for_target(enum pipe_texture_target target,
                                     uint32_t width,
                                     uint32_t height,
                                     uint32_t layers)
{
   switch (target) {
   case PIPE_TEXTURE_CUBE:
   case PIPE_TEXTURE_CUBE_ARRAY:
      if (width != height || layers < 6 || layers % 6)
         return 0;
      return VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
   case PIPE_TEXTURE_2D_ARRAY:
      if (width == height && layers >= 6 && layers % 6 == 0)
         return VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
      return 0;
   case PIPE_TEXTURE_3D:
      return VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT;
   default:
      return 0;
   }
}

static VkImageUsageFlags
yttrium_venus_image_usage_for_bind(VkFormat vk_format, unsigned bind)
{
   const bool depth_format = yttrium_venus_format_has_depth(vk_format);
   VkImageUsageFlags usage =
      VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
      VK_IMAGE_USAGE_TRANSFER_DST_BIT;

   if (bind & PIPE_BIND_SAMPLER_VIEW)
      usage |= VK_IMAGE_USAGE_SAMPLED_BIT;

   if (bind & PIPE_BIND_SHADER_IMAGE)
      usage |= VK_IMAGE_USAGE_STORAGE_BIT;

   if (vk_format == VK_FORMAT_R16_UINT &&
       (bind & PIPE_BIND_RENDER_TARGET))
      usage |= VK_IMAGE_USAGE_STORAGE_BIT;

   if (bind & PIPE_BIND_DEPTH_STENCIL) {
      if (!depth_format)
         return 0;
      usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
   }

   if (bind & PIPE_BIND_RENDER_TARGET) {
      if (depth_format)
         return 0;
      usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
   }

   return usage;
}

static bool
yttrium_venus2_sampled_image_supported(struct yttrium_venus *venus,
                                       VkFormat vk_format,
                                       VkImageType image_type,
                                       uint32_t width,
                                       uint32_t height,
                                       uint32_t depth,
                                       uint32_t levels,
                                       uint32_t layers,
                                       VkSampleCountFlagBits samples,
                                       VkImageCreateFlags image_flags,
                                       VkImageUsageFlags usage,
                                       VkImageTiling tiling)
{
   const VkFormatFeatureFlags required_features =
      VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
      VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
      VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
   VkFormatProperties2 format_props = {
      .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2,
   };
   const VkPhysicalDeviceImageFormatInfo2 image_info = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
      .format = vk_format,
      .type = image_type,
      .tiling = tiling,
      .usage = usage,
      .flags = yttrium_venus_image_create_flags(venus, image_flags, usage,
                                                 samples, tiling),
   };
   VkImageFormatProperties2 image_props = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2,
   };

   if (!yttrium_venus_ensure_initialized(venus))
      return false;

   vn_call_vkGetPhysicalDeviceFormatProperties2(
      &venus->vn_ring, venus->physical_device, vk_format, &format_props);
   const VkFormatFeatureFlags tiling_features =
      tiling == VK_IMAGE_TILING_LINEAR
         ? format_props.formatProperties.linearTilingFeatures
         : format_props.formatProperties.optimalTilingFeatures;
   if ((tiling_features & required_features) != required_features)
      return false;

   if (vn_call_vkGetPhysicalDeviceImageFormatProperties2(
          &venus->vn_ring, venus->physical_device, &image_info,
          &image_props) != VK_SUCCESS)
      return false;

   const VkImageFormatProperties *props = &image_props.imageFormatProperties;
   return width && height && depth && levels && layers &&
      width <= props->maxExtent.width &&
      height <= props->maxExtent.height &&
      depth <= props->maxExtent.depth &&
      levels <= props->maxMipLevels &&
      layers <= props->maxArrayLayers &&
      (props->sampleCounts & samples);
}

bool
yttrium_venus2_sampled_texture_format_supported(
   struct yttrium_venus *venus,
   enum pipe_format pipe_format,
   enum pipe_texture_target target)
{
   const VkFormat vk_format = yttrium_venus2_pipe_format(pipe_format);
   VkImageType image_type;
   uint32_t image_height;
   uint32_t image_depth;
   uint32_t image_layers;
   VkImageCreateFlags image_flags = 0;

   if (vk_format == VK_FORMAT_UNDEFINED ||
       yttrium_venus_format_has_depth(vk_format) ||
       !yttrium_venus_texture_target_params(target, 1, 1, 1,
                                            &image_type, &image_height,
                                            &image_depth, &image_layers) ||
       !image_height || !image_depth || !image_layers)
      return false;

   if (target == PIPE_TEXTURE_CUBE || target == PIPE_TEXTURE_CUBE_ARRAY)
      image_flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;

   return yttrium_venus2_sampled_image_supported(
      venus, vk_format, image_type, 1, image_height,
      target == PIPE_TEXTURE_3D ? 2 : image_depth,
      1, image_layers, VK_SAMPLE_COUNT_1_BIT, image_flags,
      VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
      VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_TILING_OPTIMAL);
}

bool
yttrium_venus2_create_texture_image_for_bind(struct yttrium_venus *venus,
                                            struct yttrium_venus_resource *resource,
                                            enum pipe_texture_target target,
                                            uint32_t width,
                                            uint32_t height,
                                            uint32_t depth,
                                            uint32_t levels,
                                            uint32_t layers,
                                            enum pipe_format pipe_format,
                                            unsigned sample_count,
                                            unsigned bind,
                                            uint64_t *out_allocation_size)
{
   VkFormat vk_format =
      yttrium_venus_pipe_format_for_bind(pipe_format, bind);

   if (out_allocation_size)
      *out_allocation_size = 0;

   if (!resource || vk_format == VK_FORMAT_UNDEFINED)
      return false;

   if (resource->initialized) {
      if (resource->buffer_backed || !resource->memory_obj.id)
         return false;
      if (out_allocation_size)
         *out_allocation_size = resource->allocation_size;
      return true;
   }

   if (!yttrium_venus_ensure_initialized(venus))
      return false;

   VkImageType image_type;
   uint32_t image_height;
   uint32_t image_depth;
   uint32_t image_layers;
   if (!yttrium_venus_texture_target_params(target, height, depth, layers,
                                            &image_type, &image_height,
                                            &image_depth, &image_layers))
      return false;

   VkImageUsageFlags usage =
      yttrium_venus_image_usage_for_bind(vk_format, bind);
   if (!usage)
      return false;

   VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
   if (!yttrium_venus_sample_count(sample_count, &samples))
      return false;
   if (samples != VK_SAMPLE_COUNT_1_BIT &&
       !(venus->framebuffer_sample_counts & samples))
      return false;

   VkImageCreateFlags image_flags =
      yttrium_venus_image_flags_for_target(target, width, image_height,
                                           image_layers);
   if ((target == PIPE_TEXTURE_CUBE || target == PIPE_TEXTURE_CUBE_ARRAY) &&
       !image_flags)
      return false;

   /* Shared single-sample colour images use linear tiling and ALIAS so
    * yttrium_venus2_import_texture_image_for_bind and the win32 WSI
    * DXGI_SHARED path can reproduce their memory layout.  Multisampled and
    * depth images retain optimal tiling.
    */
   const bool shared_colour = (bind & PIPE_BIND_SHARED) &&
                              !yttrium_venus_format_has_depth(vk_format) &&
                              samples == VK_SAMPLE_COUNT_1_BIT;

   const VkImageTiling tiling = shared_colour ? VK_IMAGE_TILING_LINEAR
                                             : VK_IMAGE_TILING_OPTIMAL;
   if (shared_colour)
      image_flags |= VK_IMAGE_CREATE_ALIAS_BIT;

   if (!yttrium_venus_select_depth_storage_format(
          venus, &vk_format, image_type, width, image_height, image_depth,
          levels, image_layers, image_flags, usage, samples, tiling,
          "create-texture-image"))
      return false;

   if (!yttrium_venus_select_feedback_loop_usage(
          venus, vk_format, image_type, width, image_height, image_depth,
          levels, image_layers, image_flags, samples, tiling, &usage))
      return false;

   if ((usage & VK_IMAGE_USAGE_SAMPLED_BIT) &&
       !yttrium_venus2_sampled_image_supported(
          venus, vk_format, image_type, width, image_height, image_depth,
          levels, image_layers, samples, image_flags, usage, tiling))
      return false;

   if (!yttrium_venus_create_image_tiled(
          venus, resource, image_type, vk_format, width, image_height,
          image_depth, levels, image_layers, image_flags, usage, samples,
          tiling, VK_IMAGE_LAYOUT_UNDEFINED))
      return false;

   VkMemoryRequirements reqs;
   memset(&reqs, 0, sizeof(reqs));
   vn_call_vkGetImageMemoryRequirements(&venus->vn_ring,
                                        venus->device_handle,
                                        resource->image, &reqs);
   /* Never allocate or bind zero-sized backing.  Changing tiling here would
    * break the shared image layout agreement with the importer.
    */
   if (!reqs.size) {
      YTTRIUM_WARN("yttrium: Venus zero memory requirement for image %ux%ux%u "
                   "format=%u vk_format=%u tiling=%u usage=0x%x "
                   "flags=0x%x samples=0x%x bind=0x%x; refusing allocation\n",
                   width, image_height, image_depth, pipe_format, vk_format,
                   tiling, usage, image_flags, samples, bind);

      vn_async_vkDestroyImage(&venus->vn_ring, venus->device_handle,
                              resource->image, NULL);
      memset(resource, 0, sizeof(*resource));
      return false;
   }

   const uint32_t memory_type_index =
      yttrium_venus_choose_memory_type(venus, reqs.memoryTypeBits, 0,
                                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
   if (memory_type_index == UINT32_MAX) {
      YTTRIUM_LOG("yttrium: Venus no memory type for bind texture image bits=0x%x target=%u %ux%ux%u levels=%u layers=%u format=%u bind=0x%x req_size=0x%llx usage=0x%x\n",
                  reqs.memoryTypeBits, target, width, image_height,
                  image_depth, MAX2(levels, 1), image_layers, pipe_format,
                  bind, (unsigned long long)reqs.size, usage);
      vn_async_vkDestroyImage(&venus->vn_ring, venus->device_handle,
                              resource->image, NULL);
      memset(resource, 0, sizeof(*resource));
      return false;
   }

   yttrium_venus_init_object(venus, &resource->memory_obj);
   resource->memory =
      YTTRIUM_VENUS_HANDLE(VkDeviceMemory, &resource->memory_obj);

   VkExportMemoryAllocateInfo export_info =
      yttrium_venus_export_memory_info();
   const VkMemoryAllocateInfo memory_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .pNext = (bind & PIPE_BIND_SHARED) ? &export_info : NULL,
      .allocationSize = reqs.size,
      .memoryTypeIndex = memory_type_index,
   };
   if (bind & PIPE_BIND_SHARED) {
      /* KMD may open the exported blob immediately after this returns.  Keep
       * allocate/bind synchronous so the host renderer has materialized it.
       */
      VkResult result = yttrium_venus_allocate_memory(
         venus, &memory_info, &reqs, &resource->memory,
         resource->memory_obj.id, "shared-bind-texture-image", true);
      if (result != VK_SUCCESS) {
         YTTRIUM_WARN("yttrium: Venus vkAllocateMemory shared bind texture image failed result=%d alloc_size=0x%llx target=%u type=%u bits=0x%x flags=0x%x usage=0x%x bind=0x%x\n",
                     result,
                     (unsigned long long)memory_info.allocationSize,
                     target, image_type, reqs.memoryTypeBits,
                     venus->memory_props.memoryTypes[memory_type_index].propertyFlags,
                     usage, bind);
         vn_async_vkDestroyImage(&venus->vn_ring, venus->device_handle,
                                 resource->image, NULL);
         memset(resource, 0, sizeof(*resource));
         return false;
      }

      result = vn_call_vkBindImageMemory(&venus->vn_ring,
                                         venus->device_handle,
                                         resource->image,
                                         resource->memory, 0);
      if (result != VK_SUCCESS) {
         YTTRIUM_WARN("yttrium: Venus vkBindImageMemory shared bind texture image failed result=%d memory_id=%llu image_id=%llu target=%u bind=0x%x\n",
                     result,
                     (unsigned long long)resource->memory_obj.id,
                     (unsigned long long)resource->image_obj.id,
                     target, bind);
         vn_async_vkFreeMemory(&venus->vn_ring, venus->device_handle,
                               resource->memory, NULL);
         vn_async_vkDestroyImage(&venus->vn_ring, venus->device_handle,
                                 resource->image, NULL);
         memset(resource, 0, sizeof(*resource));
         return false;
      }
   } else {
      if (yttrium_venus_allocate_memory(
             venus, &memory_info, &reqs, &resource->memory,
             resource->memory_obj.id, "bind-texture-image", false) != VK_SUCCESS) {
         yttrium_venus_discard_accepted_image(venus, resource, false);
         return false;
      }

      struct vn_ring_submit_command bind_submit;
      vn_submit_vkBindImageMemory(&venus->vn_ring, 0,
                                  venus->device_handle, resource->image,
                                  resource->memory, 0, &bind_submit);
      if (!yttrium_venus_async_submit_succeeded(
             venus, &bind_submit, "vkBindImageMemory(bind-texture-image)",
             resource->image_obj.id)) {
         yttrium_venus_discard_accepted_image(venus, resource, true);
         return false;
      }
   }

   resource->layout = VK_IMAGE_LAYOUT_UNDEFINED;
   resource->allocation_size = memory_info.allocationSize;
   resource->image_offset = 0;
   resource->image_size = reqs.size;
   resource->image_row_pitch = 0;
   resource->image_array_pitch = 0;
   resource->initialized = true;
   resource->buffer_backed = false;

   if (out_allocation_size)
      *out_allocation_size = memory_info.allocationSize;

   return true;
}

bool
yttrium_venus2_create_depth_stencil_image(struct yttrium_venus *venus,
                                         struct yttrium_venus_resource *resource,
                                         uint32_t width,
                                         uint32_t height,
                                         enum pipe_format pipe_format,
                                         uint64_t *out_allocation_size)
{
   VkFormat vk_format = yttrium_venus2_pipe_format(pipe_format);

   if (out_allocation_size)
      *out_allocation_size = 0;

   if (!resource || vk_format == VK_FORMAT_UNDEFINED ||
       !yttrium_venus_format_has_depth(vk_format))
      return false;

   if (resource->initialized) {
      if (resource->buffer_backed || !resource->memory_obj.id)
         return false;
      if (out_allocation_size)
         *out_allocation_size = resource->allocation_size;
      return true;
   }

   if (!yttrium_venus_ensure_initialized(venus))
      return false;

   const VkImageUsageFlags usage =
      VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
      VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
      VK_IMAGE_USAGE_TRANSFER_DST_BIT;
   if (!yttrium_venus_select_depth_storage_format(
          venus, &vk_format, VK_IMAGE_TYPE_2D, width, height, 1, 1, 1,
          0, usage, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL,
          "create-depth-stencil-image"))
      return false;
   if (!yttrium_venus_create_image_tiled(venus, resource, VK_IMAGE_TYPE_2D,
                                         vk_format, width, height, 1,
                                         1, 1, 0, usage,
                                         VK_SAMPLE_COUNT_1_BIT,
                                         VK_IMAGE_TILING_OPTIMAL,
                                         VK_IMAGE_LAYOUT_UNDEFINED))
      return false;

   VkMemoryRequirements reqs;
   memset(&reqs, 0, sizeof(reqs));
   vn_call_vkGetImageMemoryRequirements(&venus->vn_ring,
                                        venus->device_handle,
                                        resource->image, &reqs);

   const uint32_t memory_type_index =
      yttrium_venus_choose_memory_type(venus, reqs.memoryTypeBits, 0,
                                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
   if (memory_type_index == UINT32_MAX) {
      YTTRIUM_LOG("yttrium: Venus no memory type for depth image bits=0x%x %ux%u format=%u req_size=0x%llx\n",
                  reqs.memoryTypeBits, width, height, pipe_format,
                  (unsigned long long)reqs.size);
      vn_async_vkDestroyImage(&venus->vn_ring, venus->device_handle,
                              resource->image, NULL);
      memset(resource, 0, sizeof(*resource));
      return false;
   }

   yttrium_venus_init_object(venus, &resource->memory_obj);
   resource->memory =
      YTTRIUM_VENUS_HANDLE(VkDeviceMemory, &resource->memory_obj);

   const VkMemoryAllocateInfo memory_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = reqs.size,
      .memoryTypeIndex = memory_type_index,
   };
   if (yttrium_venus_allocate_memory(
          venus, &memory_info, &reqs, &resource->memory,
          resource->memory_obj.id, "depth-stencil", false) != VK_SUCCESS) {
      yttrium_venus_discard_accepted_image(venus, resource, false);
      return false;
   }

   struct vn_ring_submit_command bind_submit;
   vn_submit_vkBindImageMemory(&venus->vn_ring, 0, venus->device_handle,
                               resource->image, resource->memory, 0,
                               &bind_submit);
   if (!yttrium_venus_async_submit_succeeded(
          venus, &bind_submit, "vkBindImageMemory(depth-stencil)",
          resource->image_obj.id)) {
      yttrium_venus_discard_accepted_image(venus, resource, true);
      return false;
   }

   resource->layout = VK_IMAGE_LAYOUT_UNDEFINED;
   resource->allocation_size = memory_info.allocationSize;
   resource->initialized = true;
   resource->buffer_backed = false;

   if (out_allocation_size)
      *out_allocation_size = memory_info.allocationSize;

   return true;
}

bool
yttrium_venus2_import_display_image(struct yttrium_venus *venus,
                                   struct yttrium_venus_resource *resource,
                                   uint32_t resource_id,
                                   uint32_t width,
                                   uint32_t height,
                                   enum pipe_format pipe_format,
                                   uint64_t min_allocation_size,
                                   uint64_t *out_memory_id,
                                   uint64_t *out_allocation_size)
{
   VkResult result;
   const VkFormat vk_format = yttrium_venus2_pipe_format(pipe_format);

   if (out_memory_id)
      *out_memory_id = 0;
   if (out_allocation_size)
      *out_allocation_size = 0;

   if (!resource || !resource_id || vk_format == VK_FORMAT_UNDEFINED)
      return false;

   if (resource->initialized) {
      if (resource->buffer_backed || !resource->memory_obj.id)
         return false;
      if (out_memory_id)
         *out_memory_id = resource->memory_obj.id;
      if (out_allocation_size)
         *out_allocation_size = resource->allocation_size;
      return true;
   }

   if (!yttrium_venus_ensure_initialized(venus))
      return false;

   VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                              VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                              VK_IMAGE_USAGE_SAMPLED_BIT |
                              VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
   if (!yttrium_venus_select_feedback_loop_usage(
          venus, vk_format, VK_IMAGE_TYPE_2D, width, height, 1, 1, 1,
          0, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_LINEAR, &usage))
      return false;
   if (!yttrium_venus_create_image_tiled(
          venus, resource, VK_IMAGE_TYPE_2D, vk_format, width, height, 1,
          1, 1, 0, usage,
          VK_SAMPLE_COUNT_1_BIT,
          VK_IMAGE_TILING_LINEAR, VK_IMAGE_LAYOUT_PREINITIALIZED)) {
      usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
      if (!yttrium_venus_create_image_tiled(
             venus, resource, VK_IMAGE_TYPE_2D, vk_format, width, height, 1,
             1, 1, 0, usage,
             VK_SAMPLE_COUNT_1_BIT,
             VK_IMAGE_TILING_LINEAR, VK_IMAGE_LAYOUT_PREINITIALIZED))
         return false;
   }

   VkMemoryRequirements reqs;
   memset(&reqs, 0, sizeof(reqs));
   vn_call_vkGetImageMemoryRequirements(&venus->vn_ring,
                                        venus->device_handle,
                                        resource->image, &reqs);

   VkMemoryResourcePropertiesMESA resource_props = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_RESOURCE_PROPERTIES_MESA,
   };
   result = vn_call_vkGetMemoryResourcePropertiesMESA(&venus->vn_ring,
                                                      venus->device_handle,
                                                      resource_id,
                                                      &resource_props);
   if (result != VK_SUCCESS) {
      YTTRIUM_LOG("yttrium: Venus vkGetMemoryResourcePropertiesMESA failed result=%d res_id=%u\n",
                   result, resource_id);
      vn_async_vkDestroyImage(&venus->vn_ring, venus->device_handle,
                              resource->image, NULL);
      memset(resource, 0, sizeof(*resource));
      return false;
   }

   const uint32_t memory_type_bits =
      reqs.memoryTypeBits & resource_props.memoryTypeBits;
   const uint32_t memory_type_index =
      yttrium_venus_choose_memory_type(venus, memory_type_bits,
                                       0, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
   if (memory_type_index == UINT32_MAX) {
      YTTRIUM_LOG("yttrium: Venus no compatible memory type for imported display image res_id=%u image_bits=0x%x resource_bits=0x%x %ux%u format=%u req_size=0x%llx\n",
                   resource_id, reqs.memoryTypeBits,
                   resource_props.memoryTypeBits, width, height, pipe_format,
                   (unsigned long long)reqs.size);
      vn_async_vkDestroyImage(&venus->vn_ring, venus->device_handle,
                              resource->image, NULL);
      memset(resource, 0, sizeof(*resource));
      return false;
   }

   yttrium_venus_init_object(venus, &resource->memory_obj);
   resource->memory =
      YTTRIUM_VENUS_HANDLE(VkDeviceMemory, &resource->memory_obj);

   VkImportMemoryResourceInfoMESA import_info = {
      .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_RESOURCE_INFO_MESA,
      .resourceId = resource_id,
   };
   const VkMemoryAllocateInfo memory_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .pNext = &import_info,
      .allocationSize = MAX2(reqs.size, (VkDeviceSize)min_allocation_size),
      .memoryTypeIndex = memory_type_index,
   };
   if (yttrium_venus_allocate_memory(
          venus, &memory_info, &reqs, &resource->memory,
          resource->memory_obj.id, "import-display-image", false) != VK_SUCCESS) {
      yttrium_venus_discard_accepted_image(venus, resource, false);
      return false;
   }

   struct vn_ring_submit_command bind_submit;
   vn_submit_vkBindImageMemory(&venus->vn_ring, 0, venus->device_handle,
                               resource->image, resource->memory, 0,
                               &bind_submit);
   if (!yttrium_venus_async_submit_succeeded(
          venus, &bind_submit, "vkBindImageMemory(import-display-image)",
          resource->image_obj.id)) {
      yttrium_venus_discard_accepted_image(venus, resource, true);
      return false;
   }

   const VkImageSubresource subresource = {
      .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
      .mipLevel = 0,
      .arrayLayer = 0,
   };
   VkSubresourceLayout layout;
   memset(&layout, 0, sizeof(layout));
   if (!yttrium_venus_get_image_subresource_layout_checked(
          venus, resource, &subresource, &layout,
          "import-display-image")) {
      yttrium_venus_discard_accepted_bound_image(venus, resource);
      return false;
   }

   resource->layout = VK_IMAGE_LAYOUT_PREINITIALIZED;
   resource->allocation_size = memory_info.allocationSize;
   resource->image_offset = layout.offset;
   resource->image_size = layout.size;
   resource->image_row_pitch = layout.rowPitch;
   resource->image_array_pitch = layout.arrayPitch;
   resource->initialized = true;
   resource->buffer_backed = false;
   /* This image aliases an existing presented allocation.  DWM may import it
    * after the producer has already rendered the frame, so treating it as
    * uninitialized would clear the producer's contents before sampling.
    */
   resource->contents_initialized = true;

   if (out_memory_id)
      *out_memory_id = resource->memory_obj.id;
   if (out_allocation_size)
      *out_allocation_size = memory_info.allocationSize;

   return true;
}

bool
yttrium_venus2_import_texture_image_for_bind(
   struct yttrium_venus *venus,
   struct yttrium_venus_resource *resource,
   uint32_t resource_id,
   enum pipe_texture_target target,
   uint32_t width,
   uint32_t height,
   uint32_t depth,
   uint32_t levels,
   uint32_t layers,
   enum pipe_format pipe_format,
   unsigned sample_count,
   unsigned bind,
   uint64_t min_allocation_size,
   uint64_t *out_memory_id,
   uint64_t *out_allocation_size)
{
   VkResult result;
   VkFormat vk_format =
      yttrium_venus_pipe_format_for_bind(pipe_format, bind);

   if (out_memory_id)
      *out_memory_id = 0;
   if (out_allocation_size)
      *out_allocation_size = 0;

   if (!resource || !resource_id || vk_format == VK_FORMAT_UNDEFINED)
      return false;

   if (resource->initialized) {
      if (resource->buffer_backed || !resource->memory_obj.id)
         return false;
      if (out_memory_id)
         *out_memory_id = resource->memory_obj.id;
      if (out_allocation_size)
         *out_allocation_size = resource->allocation_size;
      return true;
   }

   if (!yttrium_venus_ensure_initialized(venus))
      return false;

   VkImageType image_type;
   uint32_t image_height;
   uint32_t image_depth;
   uint32_t image_layers;
   if (!yttrium_venus_texture_target_params(target, height, depth, layers,
                                            &image_type, &image_height,
                                            &image_depth, &image_layers))
      return false;

   VkImageUsageFlags usage =
      yttrium_venus_image_usage_for_bind(vk_format, bind);
   if (!usage)
      return false;

   VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
   if (!yttrium_venus_sample_count(sample_count, &samples))
      return false;
   if (samples != VK_SAMPLE_COUNT_1_BIT &&
       !(venus->framebuffer_sample_counts & samples))
      return false;

   VkImageCreateFlags image_flags =
      yttrium_venus_image_flags_for_target(target, width, image_height,
                                           image_layers);
   if ((target == PIPE_TEXTURE_CUBE || target == PIPE_TEXTURE_CUBE_ARRAY) &&
       !image_flags)
      return false;

   /* Match the shared colour image created by
    * yttrium_venus2_create_texture_image_for_bind.  Importing its linear
    * backing through an optimal-tiled image changes texel addressing.
    */
   const bool shared_colour = (bind & PIPE_BIND_SHARED) &&
                              !yttrium_venus_format_has_depth(vk_format) &&
                              samples == VK_SAMPLE_COUNT_1_BIT;
   const VkImageTiling tiling = shared_colour ? VK_IMAGE_TILING_LINEAR
                                             : VK_IMAGE_TILING_OPTIMAL;
   if (shared_colour)
      image_flags |= VK_IMAGE_CREATE_ALIAS_BIT;

   if (!yttrium_venus_select_depth_storage_format(
          venus, &vk_format, image_type, width, image_height, image_depth,
          levels, image_layers, image_flags, usage, samples, tiling,
          "import-texture-image"))
      return false;

   if (!yttrium_venus_select_feedback_loop_usage(
          venus, vk_format, image_type, width, image_height, image_depth,
          levels, image_layers, image_flags, samples, tiling, &usage))
      return false;

   if (!yttrium_venus_create_image_tiled(
          venus, resource, image_type, vk_format, width, image_height,
          image_depth, levels, image_layers, image_flags, usage, samples,
          tiling, VK_IMAGE_LAYOUT_UNDEFINED))
      return false;

   VkMemoryRequirements reqs;
   memset(&reqs, 0, sizeof(reqs));
   vn_call_vkGetImageMemoryRequirements(&venus->vn_ring,
                                        venus->device_handle,
                                        resource->image, &reqs);

   VkMemoryResourcePropertiesMESA resource_props = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_RESOURCE_PROPERTIES_MESA,
   };
   result = vn_call_vkGetMemoryResourcePropertiesMESA(&venus->vn_ring,
                                                      venus->device_handle,
                                                      resource_id,
                                                      &resource_props);
   if (result != VK_SUCCESS) {
      vn_async_vkDestroyImage(&venus->vn_ring, venus->device_handle,
                              resource->image, NULL);
      memset(resource, 0, sizeof(*resource));
      return false;
   }

   const uint32_t memory_type_bits =
      reqs.memoryTypeBits & resource_props.memoryTypeBits;
   const uint32_t memory_type_index =
      yttrium_venus_choose_memory_type(venus, memory_type_bits,
                                       0, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
   if (memory_type_index == UINT32_MAX) {
      vn_async_vkDestroyImage(&venus->vn_ring, venus->device_handle,
                              resource->image, NULL);
      memset(resource, 0, sizeof(*resource));
      return false;
   }

   yttrium_venus_init_object(venus, &resource->memory_obj);
   resource->memory =
      YTTRIUM_VENUS_HANDLE(VkDeviceMemory, &resource->memory_obj);

   VkImportMemoryResourceInfoMESA import_info = {
      .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_RESOURCE_INFO_MESA,
      .resourceId = resource_id,
   };
   const VkMemoryAllocateInfo memory_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .pNext = &import_info,
      .allocationSize = MAX2(reqs.size, (VkDeviceSize)min_allocation_size),
      .memoryTypeIndex = memory_type_index,
   };
   result = yttrium_venus_allocate_memory(
      venus, &memory_info, &reqs, &resource->memory,
      resource->memory_obj.id, "import-texture-image", true);
   if (result != VK_SUCCESS) {
      vn_async_vkDestroyImage(&venus->vn_ring, venus->device_handle,
                              resource->image, NULL);
      memset(resource, 0, sizeof(*resource));
      return false;
   }

   result = vn_call_vkBindImageMemory(&venus->vn_ring, venus->device_handle,
                                      resource->image, resource->memory, 0);
   if (result != VK_SUCCESS) {
      vn_async_vkFreeMemory(&venus->vn_ring, venus->device_handle,
                            resource->memory, NULL);
      vn_async_vkDestroyImage(&venus->vn_ring, venus->device_handle,
                              resource->image, NULL);
      memset(resource, 0, sizeof(*resource));
      return false;
   }

   resource->layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
   resource->allocation_size = memory_info.allocationSize;
   resource->image_offset = 0;
   resource->image_size = reqs.size;
   resource->image_row_pitch = 0;
   resource->image_array_pitch = 0;
   resource->initialized = true;
   resource->buffer_backed = false;
   resource->contents_initialized = true;

   if (out_memory_id)
      *out_memory_id = resource->memory_obj.id;
   if (out_allocation_size)
      *out_allocation_size = memory_info.allocationSize;

   return true;
}

bool
yttrium_venus2_create_display_buffer(struct yttrium_venus *venus,
                                    struct yttrium_venus_resource *resource,
                                    uint64_t allocation_size,
                                    uint64_t *out_memory_id)
{
   if (out_memory_id)
      *out_memory_id = 0;

   if (!resource)
      return false;

   if (resource->initialized) {
      if (!resource->buffer_backed || !resource->memory_obj.id)
         return false;
      if (out_memory_id)
         *out_memory_id = resource->memory_obj.id;
      return true;
   }

   if (!yttrium_venus_ensure_initialized(venus))
      return false;

   yttrium_venus_init_object(venus, &resource->buffer_obj);
   resource->buffer = YTTRIUM_VENUS_HANDLE(VkBuffer, &resource->buffer_obj);

   const VkDeviceSize buffer_size = MAX2((VkDeviceSize)allocation_size,
                                         (VkDeviceSize)1);
   const VkExternalMemoryBufferCreateInfo external_buffer_info =
      yttrium_venus_external_buffer_info();
   const VkBufferCreateInfo buffer_info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .pNext = &external_buffer_info,
      .size = buffer_size,
      .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
               VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
   };

   struct vn_ring_submit_command create_submit;
   vn_submit_vkCreateBuffer(&venus->vn_ring, 0, venus->device_handle,
                            &buffer_info, NULL, &resource->buffer,
                            &create_submit);
   if (!yttrium_venus_async_submit_succeeded(
          venus, &create_submit, "vkCreateBuffer(display-buffer)",
          resource->buffer_obj.id)) {
      memset(resource, 0, sizeof(*resource));
      return false;
   }

   VkMemoryRequirements reqs;
   memset(&reqs, 0, sizeof(reqs));
   vn_call_vkGetBufferMemoryRequirements(&venus->vn_ring,
                                         venus->device_handle,
                                         resource->buffer, &reqs);

   uint32_t memory_type_index = UINT32_MAX;
   if (resource->cpu_readback) {
      /*
       * Readback buffers are consumed by the CPU after the GPU copy.  A
       * merely coherent exported allocation is reported by virglrenderer as
       * write-combined, making sequential CPU reads pathologically slow.
       * Requiring CACHED as well as COHERENT gives the KMD a legitimate
       * write-back mapping and avoids any explicit invalidate requirement.
       */
      memory_type_index = yttrium_venus_choose_memory_type(
         venus, reqs.memoryTypeBits,
         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
            VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
         0);
      if (memory_type_index == UINT32_MAX) {
         YTTRIUM_WARN("yttrium: WARNING: readback cache fallback owner=venus2 reason=no_host_cached_coherent_memory fallback=host_visible_coherent bits=0x%x size=0x%llx\n",
                      reqs.memoryTypeBits,
                      (unsigned long long)buffer_size);
      }
   }
   if (memory_type_index == UINT32_MAX) {
      memory_type_index =
         yttrium_venus_choose_memory_type(
            venus, reqs.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
   }
   if (memory_type_index == UINT32_MAX) {
      YTTRIUM_LOG("yttrium: Venus no host-visible memory type for display buffer bits=0x%x size=0x%llx\n",
                   reqs.memoryTypeBits, (unsigned long long)buffer_size);
      vn_async_vkDestroyBuffer(&venus->vn_ring, venus->device_handle,
                               resource->buffer, NULL);
      memset(resource, 0, sizeof(*resource));
      return false;
   }

   yttrium_venus_init_object(venus, &resource->memory_obj);
   resource->memory =
      YTTRIUM_VENUS_HANDLE(VkDeviceMemory, &resource->memory_obj);

   VkExportMemoryAllocateInfo export_info =
      yttrium_venus_export_memory_info();
   const VkMemoryAllocateInfo memory_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .pNext = &export_info,
      .allocationSize = MAX2(reqs.size, buffer_size),
      .memoryTypeIndex = memory_type_index,
   };
   /* KMD may create/open the blob mapping immediately after this returns.  Keep
    * allocate/bind synchronous so Venus has exported a valid host object.
    */
   VkResult result =
      yttrium_venus_allocate_memory(
         venus, &memory_info, &reqs, &resource->memory,
         resource->memory_obj.id, "display-buffer", true);
   if (result != VK_SUCCESS) {
      YTTRIUM_LOG("yttrium: Venus vkAllocateMemory display buffer failed result=%d alloc_size=0x%llx type=%u bits=0x%x flags=0x%x\n",
                  result,
                  (unsigned long long)memory_info.allocationSize,
                  memory_type_index, reqs.memoryTypeBits,
                  venus->memory_props.memoryTypes[memory_type_index].propertyFlags);
      vn_async_vkDestroyBuffer(&venus->vn_ring, venus->device_handle,
                               resource->buffer, NULL);
      memset(resource, 0, sizeof(*resource));
      return false;
   }

   result = vn_call_vkBindBufferMemory(&venus->vn_ring,
                                       venus->device_handle,
                                       resource->buffer,
                                       resource->memory, 0);
   if (result != VK_SUCCESS) {
      YTTRIUM_LOG("yttrium: Venus vkBindBufferMemory display failed result=%d memory_id=%llu\n",
                  result,
                  (unsigned long long)resource->memory_obj.id);
      vn_async_vkFreeMemory(&venus->vn_ring, venus->device_handle,
                            resource->memory, NULL);
      vn_async_vkDestroyBuffer(&venus->vn_ring, venus->device_handle,
                               resource->buffer, NULL);
      memset(resource, 0, sizeof(*resource));
      return false;
   }

   resource->initialized = true;
   resource->buffer_backed = true;
   resource->buffer_usage = buffer_info.usage;
   resource->vk_format = VK_FORMAT_UNDEFINED;
   resource->allocation_size = memory_info.allocationSize;
   resource->image_size = buffer_size;

   if (out_memory_id)
      *out_memory_id = resource->memory_obj.id;

   return true;
}

bool
yttrium_venus2_transform_feedback_enabled(const struct yttrium_venus *venus)
{
   return venus && venus->initialized && venus->transform_feedback;
}

bool
yttrium_venus2_supports_multisampled_render_to_single_sampled(
   struct yttrium_venus *venus,
   uint32_t sample_count)
{
   VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;

   if (!yttrium_venus_ensure_initialized(venus))
      return false;
   if (!yttrium_venus_sample_count(sample_count, &samples))
      return false;

   return venus->multisampled_render_to_single_sampled &&
      samples != VK_SAMPLE_COUNT_1_BIT &&
      (venus->framebuffer_sample_counts & samples);
}

bool
yttrium_venus2_supports_forced_sample_interlock(
   struct yttrium_venus *venus,
   uint32_t sample_count)
{
   VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;

   if (!yttrium_venus_ensure_initialized(venus))
      return false;
   if (!yttrium_venus_sample_count(sample_count, &samples))
      return false;

   return venus->fragment_shader_pixel_interlock &&
      venus->fragment_stores_and_atomics &&
      samples != VK_SAMPLE_COUNT_1_BIT &&
      (venus->framebuffer_no_attachments_sample_counts & samples);
}

VkSampleCountFlags
yttrium_venus2_framebuffer_color_sample_counts(struct yttrium_venus *venus)
{
   return yttrium_venus_ensure_physical_device(venus) ?
      venus->framebuffer_sample_counts : 0;
}

static bool
yttrium_venus2_create_device_local_draw_mirror(
   struct yttrium_venus *venus,
   struct yttrium_venus_resource *resource,
   VkDeviceSize buffer_size,
   VkBufferUsageFlags source_usage)
{
   const VkBufferUsageFlags draw_usage =
      source_usage & (VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                      VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
   if (!draw_usage)
      return true;

   yttrium_venus_init_object(venus,
                              &resource->device_local_draw_buffer_obj);
   resource->device_local_draw_buffer =
      YTTRIUM_VENUS_HANDLE(
         VkBuffer, &resource->device_local_draw_buffer_obj);
   const VkBufferCreateInfo buffer_info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = buffer_size,
      .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | draw_usage,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
   };
   struct vn_ring_submit_command create_submit;
   vn_submit_vkCreateBuffer(&venus->vn_ring, 0, venus->device_handle,
                            &buffer_info, NULL,
                            &resource->device_local_draw_buffer,
                            &create_submit);
   if (!yttrium_venus_async_submit_succeeded(
          venus, &create_submit, "vkCreateBuffer(device-local-draw-mirror)",
          resource->device_local_draw_buffer_obj.id)) {
      memset(&resource->device_local_draw_buffer_obj, 0,
             sizeof(resource->device_local_draw_buffer_obj));
      resource->device_local_draw_buffer = VK_NULL_HANDLE;
      return false;
   }

   VkMemoryRequirements reqs;
   memset(&reqs, 0, sizeof(reqs));
   vn_call_vkGetBufferMemoryRequirements(
      &venus->vn_ring, venus->device_handle,
      resource->device_local_draw_buffer, &reqs);

   /* Prefer real local VRAM over a host-visible BAR memory type. */
   uint32_t memory_type_index = UINT32_MAX;
   for (uint32_t i = 0; i < venus->memory_props.memoryTypeCount; i++) {
      if (!(reqs.memoryTypeBits & (1u << i)))
         continue;

      const VkMemoryPropertyFlags flags =
         venus->memory_props.memoryTypes[i].propertyFlags;
      if ((flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) &&
          !(flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
         memory_type_index = i;
         break;
      }
   }
   if (memory_type_index == UINT32_MAX) {
      memory_type_index = yttrium_venus_choose_memory_type(
         venus, reqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
   }
   if (memory_type_index == UINT32_MAX) {
      YTTRIUM_WARN("yttrium: WARNING: device-local draw mirror unavailable owner=venus2 reason=no-device-local-memory-type action=use-host-visible-source bits=0x%x size=0x%llx usage=0x%x\n",
                   reqs.memoryTypeBits,
                   (unsigned long long)buffer_size, draw_usage);
      goto fail_buffer;
   }

   yttrium_venus_init_object(venus,
                              &resource->device_local_draw_memory_obj);
   resource->device_local_draw_memory =
      YTTRIUM_VENUS_HANDLE(
         VkDeviceMemory, &resource->device_local_draw_memory_obj);
   const VkMemoryAllocateInfo memory_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = MAX2(reqs.size, buffer_size),
      .memoryTypeIndex = memory_type_index,
   };
   VkResult result = yttrium_venus_allocate_memory(
      venus, &memory_info, &reqs, &resource->device_local_draw_memory,
      resource->device_local_draw_memory_obj.id, "device-local-draw-mirror", true);
   if (result != VK_SUCCESS) {
      YTTRIUM_WARN("yttrium: WARNING: device-local draw mirror unavailable owner=venus2 reason=vkAllocateMemory-failed result=%d action=use-host-visible-source type=%u flags=0x%x bits=0x%x size=0x%llx usage=0x%x\n",
                   result, memory_type_index,
                   venus->memory_props.memoryTypes[memory_type_index].propertyFlags,
                   reqs.memoryTypeBits,
                   (unsigned long long)memory_info.allocationSize, draw_usage);
      goto fail_buffer;
   }

   result = vn_call_vkBindBufferMemory(
      &venus->vn_ring, venus->device_handle,
      resource->device_local_draw_buffer,
      resource->device_local_draw_memory, 0);
   if (result != VK_SUCCESS) {
      YTTRIUM_WARN("yttrium: WARNING: device-local draw mirror unavailable owner=venus2 reason=vkBindBufferMemory-failed result=%d action=use-host-visible-source type=%u size=0x%llx usage=0x%x\n",
                   result, memory_type_index,
                   (unsigned long long)memory_info.allocationSize, draw_usage);
      goto fail_memory;
   }

   resource->device_local_draw_buffer_size = buffer_size;
   return true;

fail_memory:
   vn_async_vkFreeMemory(&venus->vn_ring, venus->device_handle,
                         resource->device_local_draw_memory, NULL);
fail_buffer:
   vn_async_vkDestroyBuffer(&venus->vn_ring, venus->device_handle,
                            resource->device_local_draw_buffer, NULL);
   memset(&resource->device_local_draw_buffer_obj, 0,
          sizeof(resource->device_local_draw_buffer_obj));
   memset(&resource->device_local_draw_memory_obj, 0,
          sizeof(resource->device_local_draw_memory_obj));
   resource->device_local_draw_buffer = VK_NULL_HANDLE;
   resource->device_local_draw_memory = VK_NULL_HANDLE;
   resource->device_local_draw_buffer_size = 0;
   return false;
}

bool
yttrium_venus2_create_bind_buffer(struct yttrium_venus *venus,
                                  struct yttrium_venus_resource *resource,
                                  uint64_t allocation_size,
                                  VkBufferUsageFlags usage,
                                  uint64_t *out_memory_id)
{
   const bool exportable = out_memory_id != NULL;

   if (out_memory_id)
      *out_memory_id = 0;

   if (!resource)
      return false;

   usage |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
            VK_BUFFER_USAGE_TRANSFER_DST_BIT;

   if (resource->initialized) {
      if (!resource->buffer_backed || !resource->memory_obj.id ||
          !resource->buffer || (resource->buffer_usage & usage) != usage)
         return false;
      if (out_memory_id)
         *out_memory_id = resource->memory_obj.id;
      return true;
   }

   if (!yttrium_venus_ensure_initialized(venus))
      return false;

   yttrium_venus_init_object(venus, &resource->buffer_obj);
   resource->buffer = YTTRIUM_VENUS_HANDLE(VkBuffer, &resource->buffer_obj);

   const VkDeviceSize buffer_size = MAX2((VkDeviceSize)allocation_size,
                                         (VkDeviceSize)1);
   const VkExternalMemoryBufferCreateInfo external_buffer_info =
      yttrium_venus_external_buffer_info();
   const VkBufferCreateInfo buffer_info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .pNext = &external_buffer_info,
      .size = buffer_size,
      .usage = usage,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
   };

   struct vn_ring_submit_command create_submit;
   vn_submit_vkCreateBuffer(&venus->vn_ring, 0, venus->device_handle,
                            &buffer_info, NULL, &resource->buffer,
                            &create_submit);
   if (!yttrium_venus_async_submit_succeeded(
          venus, &create_submit, "vkCreateBuffer(bind-buffer)",
          resource->buffer_obj.id)) {
      memset(resource, 0, sizeof(*resource));
      return false;
   }

   VkMemoryRequirements reqs;
   memset(&reqs, 0, sizeof(reqs));
   vn_call_vkGetBufferMemoryRequirements(&venus->vn_ring,
                                         venus->device_handle,
                                         resource->buffer, &reqs);

   /*
    * For exported buffers, prefer HOST_CACHED as well as HOST_COHERENT.
    * virglrenderer derives the
    * blob's map_info from these very bits - `(coherent && cached) ? CACHED :
    * WC` in vkr_device_memory.c - and the guest maps the blob with whatever it
    * reports.  Without HOST_CACHED we are handed write-combined memory, where
    * CPU reads are uncached: the draw-time index scan measured ~142 ns per
    * element against well under a nanosecond from cached memory.  Asking for a
    * cacheable memory type gets a write-back mapping legitimately, with the
    * host using a cached coherent view of the same pages - unlike overriding
    * the reported type in the KMD, which mismatches the guest and host
    * attributes and corrupts everything the host reads (tried 2026-08-01).
    * Preferred, not required, so this degrades to the previous behaviour when
    * no such memory type exists.
    */
   uint32_t memory_type_index = UINT32_MAX;
   if (exportable) {
      memory_type_index =
         yttrium_venus_choose_memory_type(
            venus, reqs.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
            VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
   } else {
      /*
       * A private immutable draw buffer is populated through Vulkan transfer
       * commands.  Prefer real VRAM and deliberately omit export metadata so
       * RADV does not place it on the global external-memory BO list.
       */
      for (uint32_t i = 0; i < venus->memory_props.memoryTypeCount; i++) {
         if (!(reqs.memoryTypeBits & (1u << i)))
            continue;

         const VkMemoryPropertyFlags flags =
            venus->memory_props.memoryTypes[i].propertyFlags;
         if ((flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) &&
             !(flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
            memory_type_index = i;
            break;
         }
      }
      if (memory_type_index == UINT32_MAX) {
         memory_type_index = yttrium_venus_choose_memory_type(
            venus, reqs.memoryTypeBits,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
      }
   }
   if (memory_type_index == UINT32_MAX) {
      YTTRIUM_WARN("yttrium: Venus no compatible memory type for bind buffer bits=0x%x size=0x%llx usage=0x%x\n",
                   reqs.memoryTypeBits, (unsigned long long)buffer_size,
                   usage);
      vn_async_vkDestroyBuffer(&venus->vn_ring, venus->device_handle,
                               resource->buffer, NULL);
      memset(resource, 0, sizeof(*resource));
      return false;
   }

   yttrium_venus_init_object(venus, &resource->memory_obj);
   resource->memory =
      YTTRIUM_VENUS_HANDLE(VkDeviceMemory, &resource->memory_obj);

   VkExportMemoryAllocateInfo export_info =
      yttrium_venus_export_memory_info();
   const VkMemoryAllocateInfo memory_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .pNext = exportable ? &export_info : NULL,
      .allocationSize = MAX2(reqs.size, buffer_size),
      .memoryTypeIndex = memory_type_index,
   };
   VkResult result =
      yttrium_venus_allocate_memory(
         venus, &memory_info, &reqs, &resource->memory,
         resource->memory_obj.id, "bind-buffer", true);
   if (result != VK_SUCCESS) {
      YTTRIUM_WARN("yttrium: Venus vkAllocateMemory bind buffer failed result=%d alloc_size=0x%llx type=%u bits=0x%x flags=0x%x usage=0x%x\n",
                   result,
                   (unsigned long long)memory_info.allocationSize,
                   memory_type_index, reqs.memoryTypeBits,
                   venus->memory_props.memoryTypes[memory_type_index].propertyFlags,
                   usage);
      vn_async_vkDestroyBuffer(&venus->vn_ring, venus->device_handle,
                               resource->buffer, NULL);
      memset(resource, 0, sizeof(*resource));
      return false;
   }

   result = vn_call_vkBindBufferMemory(&venus->vn_ring,
                                       venus->device_handle,
                                       resource->buffer,
                                       resource->memory, 0);
   if (result != VK_SUCCESS) {
      YTTRIUM_WARN("yttrium: Venus vkBindBufferMemory bind buffer failed result=%d memory_id=%llu usage=0x%x\n",
                   result,
                   (unsigned long long)resource->memory_obj.id,
                   usage);
      vn_async_vkFreeMemory(&venus->vn_ring, venus->device_handle,
                            resource->memory, NULL);
      vn_async_vkDestroyBuffer(&venus->vn_ring, venus->device_handle,
                               resource->buffer, NULL);
      memset(resource, 0, sizeof(*resource));
      return false;
   }

   resource->initialized = true;
   resource->buffer_backed = true;
   resource->buffer_usage = usage;
   resource->vk_format = VK_FORMAT_UNDEFINED;
   resource->allocation_size = memory_info.allocationSize;
   resource->image_size = buffer_size;

   if (exportable && venus->device_local_static_draw_buffers &&
       (usage & (VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                 VK_BUFFER_USAGE_INDEX_BUFFER_BIT))) {
      yttrium_venus2_create_device_local_draw_mirror(
         venus, resource, buffer_size, usage);
   }

   if (out_memory_id)
      *out_memory_id = resource->memory_obj.id;

   return true;
}

bool
yttrium_venus2_transform_feedback_draw_enabled(const struct yttrium_venus *venus)
{
   return yttrium_venus2_transform_feedback_enabled(venus) &&
          venus->transform_feedback_props.transformFeedbackDraw;
}

bool
yttrium_venus2_vertex_attribute_divisor_supported(struct yttrium_venus *venus,
                                                 uint32_t divisor)
{
   if (divisor == 1)
      return true;

   if (!yttrium_venus_ensure_initialized(venus))
      return false;

   if (divisor == UINT32_MAX)
      return venus->vertex_attribute_instance_rate_zero_divisor;

   return venus->vertex_attribute_instance_rate_divisor &&
          divisor <= venus->max_vertex_attrib_divisor;
}

bool
yttrium_venus2_depth_clamp_enabled(const struct yttrium_venus *venus)
{
   return venus && venus->initialized && venus->depth_clamp;
}

bool
yttrium_venus2_logic_op_enabled(const struct yttrium_venus *venus)
{
   return venus && venus->initialized && venus->logic_op;
}

uint32_t
yttrium_venus2_max_dual_source_render_targets(struct yttrium_venus *venus)
{
   if (!yttrium_venus_ensure_initialized(venus))
      return 0;

   return venus->max_dual_source_render_targets;
}

float
yttrium_venus2_max_sampler_anisotropy(struct yttrium_venus *venus)
{
   if (!yttrium_venus_ensure_initialized(venus))
      return 1.0f;

   return MAX2(venus->max_sampler_anisotropy, 1.0f);
}

float
yttrium_venus2_max_sampler_lod_bias(struct yttrium_venus *venus)
{
   if (!yttrium_venus_ensure_initialized(venus))
      return 0.0f;

   return MAX2(venus->max_sampler_lod_bias, 0.0f);
}

uint32_t
yttrium_venus2_max_transform_feedback_stride(const struct yttrium_venus *venus)
{
   return venus ?
          venus->transform_feedback_props.maxTransformFeedbackBufferDataStride :
          0;
}

uint32_t
yttrium_venus2_max_viewports(struct yttrium_venus *venus)
{
   if (!yttrium_venus_ensure_initialized(venus))
      return 1;

   return MAX2(venus->max_viewports, 1u);
}

uint32_t
yttrium_venus2_mipmap_precision_bits(const struct yttrium_venus *venus)
{
   return venus ? venus->mipmap_precision_bits : 0;
}

bool
yttrium_venus2_create_stream_output_buffer(struct yttrium_venus *venus,
                                          struct yttrium_venus_resource *resource,
                                          uint64_t allocation_size,
                                          uint64_t *out_memory_id)
{
   if (out_memory_id)
      *out_memory_id = 0;

   if (!resource)
      return false;

   if (resource->initialized) {
      const VkBufferUsageFlags required =
         VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT |
         VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
         VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_COUNTER_BUFFER_BIT_EXT;
      if (!resource->buffer_backed || !resource->memory_obj.id ||
          !resource->buffer ||
          (resource->buffer_usage & required) != required)
         return false;
      if (out_memory_id)
         *out_memory_id = resource->memory_obj.id;
      return true;
   }

   if (!yttrium_venus_ensure_initialized(venus) || !venus->transform_feedback)
      return false;

   yttrium_venus_init_object(venus, &resource->buffer_obj);
   resource->buffer = YTTRIUM_VENUS_HANDLE(VkBuffer, &resource->buffer_obj);

   const VkDeviceSize buffer_size = MAX2((VkDeviceSize)allocation_size,
                                         (VkDeviceSize)1);
   const VkBufferUsageFlags usage =
      VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
      VK_BUFFER_USAGE_TRANSFER_DST_BIT |
      VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
      VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT |
      VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_COUNTER_BUFFER_BIT_EXT;
   const VkBufferCreateInfo buffer_info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = buffer_size,
      .usage = usage,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
   };

   struct vn_ring_submit_command create_submit;
   vn_submit_vkCreateBuffer(&venus->vn_ring, 0, venus->device_handle,
                            &buffer_info, NULL, &resource->buffer,
                            &create_submit);
   if (!yttrium_venus_async_submit_succeeded(
          venus, &create_submit, "vkCreateBuffer(stream-output)",
          resource->buffer_obj.id)) {
      memset(resource, 0, sizeof(*resource));
      return false;
   }

   VkMemoryRequirements reqs;
   memset(&reqs, 0, sizeof(reqs));
   vn_call_vkGetBufferMemoryRequirements(&venus->vn_ring,
                                         venus->device_handle,
                                         resource->buffer, &reqs);

   const uint32_t memory_type_index =
      yttrium_venus_choose_memory_type(venus, reqs.memoryTypeBits,
                                       0,
                                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
   if (memory_type_index == UINT32_MAX) {
      YTTRIUM_LOG("yttrium: Venus no memory type for stream output buffer bits=0x%x size=0x%llx\n",
                  reqs.memoryTypeBits, (unsigned long long)buffer_size);
      vn_async_vkDestroyBuffer(&venus->vn_ring, venus->device_handle,
                               resource->buffer, NULL);
      memset(resource, 0, sizeof(*resource));
      return false;
   }

   yttrium_venus_init_object(venus, &resource->memory_obj);
   resource->memory =
      YTTRIUM_VENUS_HANDLE(VkDeviceMemory, &resource->memory_obj);

   const VkMemoryAllocateInfo memory_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = MAX2(reqs.size, buffer_size),
      .memoryTypeIndex = memory_type_index,
   };
   if (yttrium_venus_allocate_memory(
          venus, &memory_info, &reqs, &resource->memory,
          resource->memory_obj.id, "stream-output", false) != VK_SUCCESS) {
      yttrium_venus_discard_accepted_buffer(venus, resource, false);
      return false;
   }

   struct vn_ring_submit_command bind_submit;
   vn_submit_vkBindBufferMemory(&venus->vn_ring, 0, venus->device_handle,
                                resource->buffer, resource->memory, 0,
                                &bind_submit);
   if (!yttrium_venus_async_submit_succeeded(
          venus, &bind_submit, "vkBindBufferMemory(stream-output)",
          resource->buffer_obj.id)) {
      yttrium_venus_discard_accepted_buffer(venus, resource, true);
      return false;
   }

   resource->initialized = true;
   resource->buffer_backed = true;
   resource->buffer_usage = usage;
   resource->vk_format = VK_FORMAT_UNDEFINED;
   resource->allocation_size = memory_info.allocationSize;
   resource->image_size = buffer_size;

   if (out_memory_id)
      *out_memory_id = resource->memory_obj.id;

   return true;
}

bool
yttrium_venus2_create_sampled_buffer(struct yttrium_venus *venus,
                                    struct yttrium_venus_resource *resource,
                                    uint64_t allocation_size,
                                    enum pipe_format pipe_format,
                                    uint64_t *out_memory_id)
{
   const VkFormat vk_format = yttrium_venus2_pipe_format(pipe_format);

   if (out_memory_id)
      *out_memory_id = 0;

   if (!resource || vk_format == VK_FORMAT_UNDEFINED)
      return false;

   if (resource->initialized) {
      if (!resource->buffer_backed || !resource->memory_obj.id ||
          !resource->buffer ||
          !(resource->buffer_usage &
            VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT))
         return false;
      resource->vk_format = vk_format;
      if (out_memory_id)
         *out_memory_id = resource->memory_obj.id;
      return true;
   }

   if (!yttrium_venus_ensure_initialized(venus))
      return false;

   yttrium_venus_init_object(venus, &resource->buffer_obj);
   resource->buffer = YTTRIUM_VENUS_HANDLE(VkBuffer, &resource->buffer_obj);

   const VkDeviceSize buffer_size = MAX2((VkDeviceSize)allocation_size,
                                         (VkDeviceSize)1);
   const VkBufferUsageFlags usage =
      VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
      VK_BUFFER_USAGE_TRANSFER_DST_BIT |
      VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT |
      VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT;
   const VkBufferCreateInfo buffer_info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = buffer_size,
      .usage = usage,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
   };

   struct vn_ring_submit_command create_submit;
   vn_submit_vkCreateBuffer(&venus->vn_ring, 0, venus->device_handle,
                            &buffer_info, NULL, &resource->buffer,
                            &create_submit);
   if (!yttrium_venus_async_submit_succeeded(
          venus, &create_submit, "vkCreateBuffer(sampled-buffer)",
          resource->buffer_obj.id)) {
      memset(resource, 0, sizeof(*resource));
      return false;
   }

   VkMemoryRequirements reqs;
   memset(&reqs, 0, sizeof(reqs));
   vn_call_vkGetBufferMemoryRequirements(&venus->vn_ring,
                                         venus->device_handle,
                                         resource->buffer, &reqs);

   const uint32_t memory_type_index =
      yttrium_venus_choose_memory_type(venus, reqs.memoryTypeBits, 0,
                                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
   if (memory_type_index == UINT32_MAX) {
      YTTRIUM_LOG("yttrium: Venus no memory type for sampled buffer bits=0x%x size=0x%llx format=%u vk_format=%u\n",
                  reqs.memoryTypeBits, (unsigned long long)buffer_size,
                  pipe_format, vk_format);
      vn_async_vkDestroyBuffer(&venus->vn_ring, venus->device_handle,
                               resource->buffer, NULL);
      memset(resource, 0, sizeof(*resource));
      return false;
   }

   yttrium_venus_init_object(venus, &resource->memory_obj);
   resource->memory =
      YTTRIUM_VENUS_HANDLE(VkDeviceMemory, &resource->memory_obj);

   const VkMemoryAllocateInfo memory_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = MAX2(reqs.size, buffer_size),
      .memoryTypeIndex = memory_type_index,
   };
   if (yttrium_venus_allocate_memory(
          venus, &memory_info, &reqs, &resource->memory,
          resource->memory_obj.id, "sampled-buffer", false) != VK_SUCCESS) {
      yttrium_venus_discard_accepted_buffer(venus, resource, false);
      return false;
   }

   struct vn_ring_submit_command bind_submit;
   vn_submit_vkBindBufferMemory(&venus->vn_ring, 0, venus->device_handle,
                                resource->buffer, resource->memory, 0,
                                &bind_submit);
   if (!yttrium_venus_async_submit_succeeded(
          venus, &bind_submit, "vkBindBufferMemory(sampled-buffer)",
          resource->buffer_obj.id)) {
      yttrium_venus_discard_accepted_buffer(venus, resource, true);
      return false;
   }

   resource->initialized = true;
   resource->buffer_backed = true;
   resource->buffer_usage = usage;
   resource->vk_format = vk_format;
   resource->allocation_size = memory_info.allocationSize;
   resource->image_size = buffer_size;

   if (out_memory_id)
      *out_memory_id = resource->memory_obj.id;

   return true;
}

bool
yttrium_venus2_ensure_null_sampled_buffer(struct yttrium_venus *venus,
                                         enum pipe_format pipe_format,
                                         uint64_t min_size,
                                         struct yttrium_venus_resource **out_resource,
                                         uint32_t *out_resource_id)
{
   const uint64_t null_size = MAX2(min_size, 4096ull);

   if (!venus || !out_resource || !out_resource_id ||
       yttrium_venus2_pipe_format(pipe_format) == VK_FORMAT_UNDEFINED)
      return false;

   if (!venus->null_sampled_buffer.initialized ||
       venus->null_sampled_buffer.image_size < min_size) {
      if (venus->null_sampled_buffer.initialized)
         yttrium_venus2_resource_fini(venus, NULL,
                                     &venus->null_sampled_buffer, NULL);

      uint64_t memory_id = 0;
      if (!yttrium_venus2_create_sampled_buffer(
             venus, &venus->null_sampled_buffer, null_size, pipe_format,
             &memory_id)) {
         YTTRIUM_LOG("yttrium: Venus null sampled buffer create failed size=0x%llx format=%u\n",
                     (unsigned long long)null_size, pipe_format);
         return false;
      }
   } else {
      venus->null_sampled_buffer.vk_format =
         yttrium_venus2_pipe_format(pipe_format);
   }

   *out_resource = &venus->null_sampled_buffer;
   *out_resource_id = 0;
   return true;
}
