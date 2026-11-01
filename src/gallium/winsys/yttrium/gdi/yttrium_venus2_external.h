/*
 * SPDX-FileCopyrightText: 2026 Ake Rehnman <ake.rehnman@gmail.com>
 * SPDX-License-Identifier: MPL-2.0
 */
#ifndef YTTRIUM_VENUS2_EXTERNAL_H
#define YTTRIUM_VENUS2_EXTERNAL_H

#include <stdbool.h>
#include <string.h>
#include <vulkan/vulkan.h>

/* Shared public images return to GENERAL/EXTERNAL between batches. The
 * intrusive list lives until command-buffer end or recording cancellation;
 * existing batch resource references keep its members alive. */
struct yttrium_venus_external_image {
   struct yttrium_venus_external_image *next;
   VkImage image;
   VkImageLayout *layout;
   bool enabled;
   bool acquired;
};

static inline VkImageMemoryBarrier
yttrium_venus_external_barrier(VkImage image, uint32_t queue_family,
                              bool acquire)
{
   VkImageMemoryBarrier barrier;
   memset(&barrier, 0, sizeof(barrier));
   barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
   barrier.srcAccessMask = acquire ? 0u : VK_ACCESS_MEMORY_READ_BIT |
                                        VK_ACCESS_MEMORY_WRITE_BIT;
   barrier.dstAccessMask = acquire ? VK_ACCESS_MEMORY_READ_BIT |
                                    VK_ACCESS_MEMORY_WRITE_BIT : 0u;
   barrier.oldLayout = barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
   barrier.srcQueueFamilyIndex = acquire ? VK_QUEUE_FAMILY_EXTERNAL : queue_family;
   barrier.dstQueueFamilyIndex = acquire ? queue_family : VK_QUEUE_FAMILY_EXTERNAL;
   barrier.image = image;
   barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
   barrier.subresourceRange.levelCount = barrier.subresourceRange.layerCount = 1;
   return barrier;
}

static inline bool
yttrium_venus_external_acquire(struct yttrium_venus_external_image **head,
                              struct yttrium_venus_external_image *image,
                              uint32_t queue_family,
                              VkImageMemoryBarrier *barrier)
{
   if (!image->enabled || image->acquired)
      return false;
   *barrier = yttrium_venus_external_barrier(image->image, queue_family, true);
   image->acquired = true;
   image->next = *head;
   *head = image;
   return true;
}

/* Cancellation emits no barriers: an unsubmitted recording never took the
 * image away from its external owner. Success also returns ownership/layout
 * to GENERAL before this list is cleared, including across repeated batches. */
static inline struct yttrium_venus_external_image *
yttrium_venus_external_pop(struct yttrium_venus_external_image **head)
{
   struct yttrium_venus_external_image *image = *head;
   if (!image)
      return NULL;
   *head = image->next;
   image->next = NULL;
   image->acquired = false;
   *image->layout = VK_IMAGE_LAYOUT_GENERAL;
   return image;
}

static inline void
yttrium_venus_external_cancel(struct yttrium_venus_external_image **head)
{
   while (yttrium_venus_external_pop(head))
      ;
}

#endif
