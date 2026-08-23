/*
 * SPDX-FileCopyrightText: 2026 Ake Rehnman <ake.rehnman@gmail.com>
 * SPDX-License-Identifier: MPL-2.0
 */

#include <stdbool.h>
#include <stdint.h>

#include "util/u_math.h"
#include "yttrium_venus2_private.h"

VkFormat
yttrium_venus2_pipe_format(enum pipe_format format)
{
   switch (format) {
   case PIPE_FORMAT_B8G8R8A8_UNORM:
   case PIPE_FORMAT_B8G8R8X8_UNORM:
      return VK_FORMAT_B8G8R8A8_UNORM;
   case PIPE_FORMAT_B8G8R8A8_SRGB:
      return VK_FORMAT_B8G8R8A8_SRGB;
   case PIPE_FORMAT_R8G8B8A8_UNORM:
   case PIPE_FORMAT_R8G8B8X8_UNORM:
      return VK_FORMAT_R8G8B8A8_UNORM;
   case PIPE_FORMAT_R8G8B8A8_SRGB:
      return VK_FORMAT_R8G8B8A8_SRGB;
   case PIPE_FORMAT_R10G10B10A2_UNORM:
      return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
   case PIPE_FORMAT_A8_UNORM:
      return VK_FORMAT_R8_UNORM;
   case PIPE_FORMAT_R9G9B9E5_FLOAT:
      return VK_FORMAT_E5B9G9R9_UFLOAT_PACK32;
   case PIPE_FORMAT_DXT1_RGB:
      return VK_FORMAT_BC1_RGB_UNORM_BLOCK;
   case PIPE_FORMAT_DXT1_RGBA:
      return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
   case PIPE_FORMAT_DXT1_SRGB:
      return VK_FORMAT_BC1_RGB_SRGB_BLOCK;
   case PIPE_FORMAT_DXT1_SRGBA:
      return VK_FORMAT_BC1_RGBA_SRGB_BLOCK;
   case PIPE_FORMAT_DXT3_RGBA:
      return VK_FORMAT_BC2_UNORM_BLOCK;
   case PIPE_FORMAT_DXT3_SRGBA:
      return VK_FORMAT_BC2_SRGB_BLOCK;
   case PIPE_FORMAT_DXT5_RGBA:
      return VK_FORMAT_BC3_UNORM_BLOCK;
   case PIPE_FORMAT_DXT5_SRGBA:
      return VK_FORMAT_BC3_SRGB_BLOCK;
   case PIPE_FORMAT_RGTC1_UNORM:
      return VK_FORMAT_BC4_UNORM_BLOCK;
   case PIPE_FORMAT_RGTC1_SNORM:
      return VK_FORMAT_BC4_SNORM_BLOCK;
   case PIPE_FORMAT_RGTC2_UNORM:
      return VK_FORMAT_BC5_UNORM_BLOCK;
   case PIPE_FORMAT_RGTC2_SNORM:
      return VK_FORMAT_BC5_SNORM_BLOCK;
   case PIPE_FORMAT_BPTC_RGB_UFLOAT:
      return VK_FORMAT_BC6H_UFLOAT_BLOCK;
   case PIPE_FORMAT_BPTC_RGB_FLOAT:
      return VK_FORMAT_BC6H_SFLOAT_BLOCK;
   case PIPE_FORMAT_BPTC_RGBA_UNORM:
      return VK_FORMAT_BC7_UNORM_BLOCK;
   case PIPE_FORMAT_BPTC_SRGBA:
      return VK_FORMAT_BC7_SRGB_BLOCK;
   case PIPE_FORMAT_R8_UINT:
      return VK_FORMAT_R8_UINT;
   case PIPE_FORMAT_R8G8_UINT:
      return VK_FORMAT_R8G8_UINT;
   case PIPE_FORMAT_R8G8B8_UINT:
      return VK_FORMAT_R8G8B8_UINT;
   case PIPE_FORMAT_R8G8B8A8_UINT:
      return VK_FORMAT_R8G8B8A8_UINT;
   case PIPE_FORMAT_R8_SINT:
      return VK_FORMAT_R8_SINT;
   case PIPE_FORMAT_R8G8_SINT:
      return VK_FORMAT_R8G8_SINT;
   case PIPE_FORMAT_R8G8B8_SINT:
      return VK_FORMAT_R8G8B8_SINT;
   case PIPE_FORMAT_R8G8B8A8_SINT:
      return VK_FORMAT_R8G8B8A8_SINT;
   case PIPE_FORMAT_R8_UNORM:
      return VK_FORMAT_R8_UNORM;
   case PIPE_FORMAT_R8G8_UNORM:
      return VK_FORMAT_R8G8_UNORM;
   case PIPE_FORMAT_R8G8B8_UNORM:
      return VK_FORMAT_R8G8B8_UNORM;
   case PIPE_FORMAT_R8_SNORM:
      return VK_FORMAT_R8_SNORM;
   case PIPE_FORMAT_R8G8_SNORM:
      return VK_FORMAT_R8G8_SNORM;
   case PIPE_FORMAT_R8G8B8_SNORM:
      return VK_FORMAT_R8G8B8_SNORM;
   case PIPE_FORMAT_R8G8B8A8_SNORM:
      return VK_FORMAT_R8G8B8A8_SNORM;
   case PIPE_FORMAT_B5G6R5_UNORM:
      return VK_FORMAT_B5G6R5_UNORM_PACK16;
   case PIPE_FORMAT_B5G5R5A1_UNORM:
      return VK_FORMAT_B5G5R5A1_UNORM_PACK16;
   case PIPE_FORMAT_R8_USCALED:
      return VK_FORMAT_R8_USCALED;
   case PIPE_FORMAT_R8G8_USCALED:
      return VK_FORMAT_R8G8_USCALED;
   case PIPE_FORMAT_R8G8B8_USCALED:
      return VK_FORMAT_R8G8B8_USCALED;
   case PIPE_FORMAT_R8G8B8A8_USCALED:
      return VK_FORMAT_R8G8B8A8_USCALED;
   case PIPE_FORMAT_R8_SSCALED:
      return VK_FORMAT_R8_SSCALED;
   case PIPE_FORMAT_R8G8_SSCALED:
      return VK_FORMAT_R8G8_SSCALED;
   case PIPE_FORMAT_R8G8B8_SSCALED:
      return VK_FORMAT_R8G8B8_SSCALED;
   case PIPE_FORMAT_R8G8B8A8_SSCALED:
      return VK_FORMAT_R8G8B8A8_SSCALED;
   case PIPE_FORMAT_R16_UINT:
      return VK_FORMAT_R16_UINT;
   case PIPE_FORMAT_R16G16_UINT:
      return VK_FORMAT_R16G16_UINT;
   case PIPE_FORMAT_R16G16B16_UINT:
      return VK_FORMAT_R16G16B16_UINT;
   case PIPE_FORMAT_R16G16B16A16_UINT:
      return VK_FORMAT_R16G16B16A16_UINT;
   case PIPE_FORMAT_R16_SINT:
      return VK_FORMAT_R16_SINT;
   case PIPE_FORMAT_R16G16_SINT:
      return VK_FORMAT_R16G16_SINT;
   case PIPE_FORMAT_R16G16B16_SINT:
      return VK_FORMAT_R16G16B16_SINT;
   case PIPE_FORMAT_R16G16B16A16_SINT:
      return VK_FORMAT_R16G16B16A16_SINT;
   case PIPE_FORMAT_R16_FLOAT:
      return VK_FORMAT_R16_SFLOAT;
   case PIPE_FORMAT_R16G16_FLOAT:
      return VK_FORMAT_R16G16_SFLOAT;
   case PIPE_FORMAT_R16G16B16_FLOAT:
      return VK_FORMAT_R16G16B16_SFLOAT;
   case PIPE_FORMAT_R16G16B16A16_FLOAT:
      return VK_FORMAT_R16G16B16A16_SFLOAT;
   case PIPE_FORMAT_R11G11B10_FLOAT:
      return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
   case PIPE_FORMAT_R16_UNORM:
      return VK_FORMAT_R16_UNORM;
   case PIPE_FORMAT_R16G16_UNORM:
      return VK_FORMAT_R16G16_UNORM;
   case PIPE_FORMAT_R16G16B16_UNORM:
      return VK_FORMAT_R16G16B16_UNORM;
   case PIPE_FORMAT_R16G16B16A16_UNORM:
      return VK_FORMAT_R16G16B16A16_UNORM;
   case PIPE_FORMAT_R16_SNORM:
      return VK_FORMAT_R16_SNORM;
   case PIPE_FORMAT_R16G16_SNORM:
      return VK_FORMAT_R16G16_SNORM;
   case PIPE_FORMAT_R16G16B16_SNORM:
      return VK_FORMAT_R16G16B16_SNORM;
   case PIPE_FORMAT_R16G16B16A16_SNORM:
      return VK_FORMAT_R16G16B16A16_SNORM;
   case PIPE_FORMAT_R16_USCALED:
      return VK_FORMAT_R16_USCALED;
   case PIPE_FORMAT_R16G16_USCALED:
      return VK_FORMAT_R16G16_USCALED;
   case PIPE_FORMAT_R16G16B16_USCALED:
      return VK_FORMAT_R16G16B16_USCALED;
   case PIPE_FORMAT_R16G16B16A16_USCALED:
      return VK_FORMAT_R16G16B16A16_USCALED;
   case PIPE_FORMAT_R16_SSCALED:
      return VK_FORMAT_R16_SSCALED;
   case PIPE_FORMAT_R16G16_SSCALED:
      return VK_FORMAT_R16G16_SSCALED;
   case PIPE_FORMAT_R16G16B16_SSCALED:
      return VK_FORMAT_R16G16B16_SSCALED;
   case PIPE_FORMAT_R16G16B16A16_SSCALED:
      return VK_FORMAT_R16G16B16A16_SSCALED;
   case PIPE_FORMAT_R32_UINT:
      return VK_FORMAT_R32_UINT;
   case PIPE_FORMAT_R32G32_UINT:
      return VK_FORMAT_R32G32_UINT;
   case PIPE_FORMAT_R32G32B32_UINT:
      return VK_FORMAT_R32G32B32_UINT;
   case PIPE_FORMAT_R32G32B32A32_UINT:
      return VK_FORMAT_R32G32B32A32_UINT;
   case PIPE_FORMAT_R32_SINT:
      return VK_FORMAT_R32_SINT;
   case PIPE_FORMAT_R32G32_SINT:
      return VK_FORMAT_R32G32_SINT;
   case PIPE_FORMAT_R32G32B32_SINT:
      return VK_FORMAT_R32G32B32_SINT;
   case PIPE_FORMAT_R32G32B32A32_SINT:
      return VK_FORMAT_R32G32B32A32_SINT;
   case PIPE_FORMAT_R32_FLOAT:
      return VK_FORMAT_R32_SFLOAT;
   case PIPE_FORMAT_R32G32_FLOAT:
      return VK_FORMAT_R32G32_SFLOAT;
   case PIPE_FORMAT_R32G32B32_FLOAT:
      return VK_FORMAT_R32G32B32_SFLOAT;
   case PIPE_FORMAT_R32G32B32A32_FLOAT:
      return VK_FORMAT_R32G32B32A32_SFLOAT;
   case PIPE_FORMAT_R32G32B32A32_UNORM:
      return VK_FORMAT_R32G32B32A32_SFLOAT;
   case PIPE_FORMAT_Z16_UNORM:
      return VK_FORMAT_D16_UNORM;
   case PIPE_FORMAT_Z16_UNORM_S8_UINT:
      return VK_FORMAT_D16_UNORM_S8_UINT;
   case PIPE_FORMAT_Z32_FLOAT:
      return VK_FORMAT_D32_SFLOAT;
   case PIPE_FORMAT_Z32_FLOAT_S8X24_UINT:
      return VK_FORMAT_D32_SFLOAT_S8_UINT;
   case PIPE_FORMAT_Z24X8_UNORM:
      return VK_FORMAT_X8_D24_UNORM_PACK32;
   case PIPE_FORMAT_Z24_UNORM_S8_UINT:
   case PIPE_FORMAT_S8_UINT_Z24_UNORM:
      return VK_FORMAT_D24_UNORM_S8_UINT;
   case PIPE_FORMAT_X24S8_UINT:
   case PIPE_FORMAT_S8X24_UINT:
   case PIPE_FORMAT_X32_S8X24_UINT:
   case PIPE_FORMAT_S8_UINT:
      return VK_FORMAT_S8_UINT;
   default:
      return VK_FORMAT_UNDEFINED;
   }
}

VkFormat
yttrium_venus_pipe_format_for_bind(enum pipe_format format, unsigned bind)
{
   if (bind & PIPE_BIND_SAMPLER_VIEW) {
      switch (format) {
      case PIPE_FORMAT_Z24X8_UNORM:
      case PIPE_FORMAT_X8Z24_UNORM:
         return VK_FORMAT_D32_SFLOAT;
      case PIPE_FORMAT_Z24_UNORM_S8_UINT:
      case PIPE_FORMAT_S8_UINT_Z24_UNORM:
         return VK_FORMAT_D32_SFLOAT_S8_UINT;
      default:
         break;
      }
   }

   return yttrium_venus2_pipe_format(format);
}

VkImageAspectFlags
yttrium_venus_format_aspects(VkFormat format)
{
   switch (format) {
   case VK_FORMAT_D16_UNORM:
   case VK_FORMAT_D32_SFLOAT:
   case VK_FORMAT_X8_D24_UNORM_PACK32:
      return VK_IMAGE_ASPECT_DEPTH_BIT;
   case VK_FORMAT_S8_UINT:
      return VK_IMAGE_ASPECT_STENCIL_BIT;
   case VK_FORMAT_D16_UNORM_S8_UINT:
   case VK_FORMAT_D24_UNORM_S8_UINT:
   case VK_FORMAT_D32_SFLOAT_S8_UINT:
      return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
   default:
      return VK_IMAGE_ASPECT_COLOR_BIT;
   }
}

bool
yttrium_venus_format_has_depth(VkFormat format)
{
   return (yttrium_venus_format_aspects(format) &
           VK_IMAGE_ASPECT_DEPTH_BIT) != 0;
}

bool
yttrium_venus_format_has_stencil(VkFormat format)
{
   return (yttrium_venus_format_aspects(format) &
            VK_IMAGE_ASPECT_STENCIL_BIT) != 0;
}

VkImageAspectFlags
yttrium_venus_initialized_aspects(
   const struct yttrium_venus_resource *resource)
{
   if (!resource)
      return 0;

   const VkImageAspectFlags format_aspects =
      yttrium_venus_format_aspects(resource->vk_format);
   if (resource->initialized_aspects)
      return resource->initialized_aspects & format_aspects;

   return resource->contents_initialized ? format_aspects : 0;
}

void
yttrium_venus_mark_aspects_initialized(struct yttrium_venus_resource *resource,
                                       VkImageAspectFlags aspects)
{
   if (!resource)
      return;

   resource->initialized_aspects |=
      aspects & yttrium_venus_format_aspects(resource->vk_format);
   if (resource->initialized_aspects)
      resource->contents_initialized = true;
}

static uint8_t
yttrium_venus_float_to_unorm8(float value)
{
   value = CLAMP(value, 0.0f, 1.0f);
   return (uint8_t)(value * 255.0f + 0.5f);
}

bool
yttrium_venus_clear_pattern(enum pipe_format format,
                            const union pipe_color_union *color,
                            uint32_t *pattern)
{
   const uint8_t r = yttrium_venus_float_to_unorm8(color->f[0]);
   const uint8_t g = yttrium_venus_float_to_unorm8(color->f[1]);
   const uint8_t b = yttrium_venus_float_to_unorm8(color->f[2]);
   const uint8_t a = yttrium_venus_float_to_unorm8(color->f[3]);

   switch (format) {
   case PIPE_FORMAT_B8G8R8A8_UNORM:
   case PIPE_FORMAT_B8G8R8A8_SRGB:
      *pattern = (uint32_t)b | ((uint32_t)g << 8) |
                 ((uint32_t)r << 16) | ((uint32_t)a << 24);
      return true;
   case PIPE_FORMAT_B8G8R8X8_UNORM:
      *pattern = (uint32_t)b | ((uint32_t)g << 8) |
                 ((uint32_t)r << 16) | 0xff000000u;
      return true;
   case PIPE_FORMAT_R8G8B8A8_UNORM:
   case PIPE_FORMAT_R8G8B8A8_SRGB:
      *pattern = (uint32_t)r | ((uint32_t)g << 8) |
                 ((uint32_t)b << 16) | ((uint32_t)a << 24);
      return true;
   case PIPE_FORMAT_R8G8B8X8_UNORM:
      *pattern = (uint32_t)r | ((uint32_t)g << 8) |
                 ((uint32_t)b << 16) | 0xff000000u;
      return true;
   default:
      return false;
   }
}
