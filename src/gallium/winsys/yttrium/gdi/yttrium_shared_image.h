/*
 * SPDX-FileCopyrightText: 2026 Ake Rehnman <ake.rehnman@gmail.com>
 * SPDX-License-Identifier: MPL-2.0
 */
#ifndef YTTRIUM_SHARED_IMAGE_H
#define YTTRIUM_SHARED_IMAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <vulkan/vulkan_core.h>
#include "virtio/virtio-gpu/virgl_hw.h"
#include "virtio/wddm/viogpu_wddm_driver.h"

/* The runtime preserves these pointer-free trailers. KMD consumes the
 * unchanged 72-byte allocation prefix; Mesa validates versioned GPU storage
 * and display-endpoint contracts. Version 1 is recognized only for rejection. */
#define YTTRIUM_MESA_SHARED_IMAGE_MAGIC 0x314d3359u
#define YTTRIUM_DISPLAY_ENDPOINT_ROLE_LINEAR_SCANOUT 1u
#define YTTRIUM_OPTIMAL_IMAGE_V2_BYTES 168u
#define YTTRIUM_OPTIMAL_IMAGE_DRM_VERSION 4u
#define YTTRIUM_OPTIMAL_IMAGE_DRM_BYTES 248u
#define YTTRIUM_OPTIMAL_IMAGE_MAX_DRM_PLANES 4u
/* ResourceOptions.bind is the Windows PIPE_BIND ABI. Its shader-image bit
 * shares a value with virgl's unrelated query-buffer bit. */
#define YTTRIUM_MESA_SHARED_IMAGE_SHADER_IMAGE_BIND (1u << 15)
#define YTTRIUM_MESA_SHARED_IMAGE_BIND_MASK \
   (VIRGL_BIND_RENDER_TARGET | VIRGL_BIND_SAMPLER_VIEW | \
    YTTRIUM_MESA_SHARED_IMAGE_SHADER_IMAGE_BIND | \
    VIRGL_BIND_DISPLAY_TARGET | VIRGL_BIND_SHARED)

/* Preserve typed color identity across the shared-image ABI. Vulkan qualifies
 * the requested usage and exact external-memory descriptor on each device. */
#define YTTRIUM_SHARED_COLOR_FORMATS(M) \
   M(R32G32B32A32_FLOAT, 2, VK_FORMAT_R32G32B32A32_SFLOAT, VK_FORMAT_UNDEFINED) \
   M(R32G32B32A32_UINT, 3, VK_FORMAT_R32G32B32A32_UINT, VK_FORMAT_UNDEFINED) \
   M(R32G32B32A32_SINT, 4, VK_FORMAT_R32G32B32A32_SINT, VK_FORMAT_UNDEFINED) \
   M(R16G16B16A16_FLOAT, 10, VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_UNDEFINED) \
   M(R16G16B16A16_UNORM, 11, VK_FORMAT_R16G16B16A16_UNORM, VK_FORMAT_UNDEFINED) \
   M(R16G16B16A16_UINT, 12, VK_FORMAT_R16G16B16A16_UINT, VK_FORMAT_UNDEFINED) \
   M(R16G16B16A16_SNORM, 13, VK_FORMAT_R16G16B16A16_SNORM, VK_FORMAT_UNDEFINED) \
   M(R16G16B16A16_SINT, 14, VK_FORMAT_R16G16B16A16_SINT, VK_FORMAT_UNDEFINED) \
   M(R32G32_FLOAT, 16, VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_UNDEFINED) \
   M(R32G32_UINT, 17, VK_FORMAT_R32G32_UINT, VK_FORMAT_UNDEFINED) \
   M(R32G32_SINT, 18, VK_FORMAT_R32G32_SINT, VK_FORMAT_UNDEFINED) \
   M(R10G10B10A2_UNORM, 24, VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_FORMAT_UNDEFINED) \
   M(R10G10B10A2_UINT, 25, VK_FORMAT_A2B10G10R10_UINT_PACK32, VK_FORMAT_UNDEFINED) \
   M(R11G11B10_FLOAT, 26, VK_FORMAT_B10G11R11_UFLOAT_PACK32, VK_FORMAT_UNDEFINED) \
   M(R8G8B8A8_UNORM, 28, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB) \
   M(R8G8B8A8_SRGB, 29, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB) \
   M(R8G8B8A8_UINT, 30, VK_FORMAT_R8G8B8A8_UINT, VK_FORMAT_UNDEFINED) \
   M(R8G8B8A8_SNORM, 31, VK_FORMAT_R8G8B8A8_SNORM, VK_FORMAT_UNDEFINED) \
   M(R8G8B8A8_SINT, 32, VK_FORMAT_R8G8B8A8_SINT, VK_FORMAT_UNDEFINED) \
   M(R16G16_FLOAT, 34, VK_FORMAT_R16G16_SFLOAT, VK_FORMAT_UNDEFINED) \
   M(R16G16_UNORM, 35, VK_FORMAT_R16G16_UNORM, VK_FORMAT_UNDEFINED) \
   M(R16G16_UINT, 36, VK_FORMAT_R16G16_UINT, VK_FORMAT_UNDEFINED) \
   M(R16G16_SNORM, 37, VK_FORMAT_R16G16_SNORM, VK_FORMAT_UNDEFINED) \
   M(R16G16_SINT, 38, VK_FORMAT_R16G16_SINT, VK_FORMAT_UNDEFINED) \
   M(R32_FLOAT, 41, VK_FORMAT_R32_SFLOAT, VK_FORMAT_UNDEFINED) \
   M(R32_UINT, 42, VK_FORMAT_R32_UINT, VK_FORMAT_UNDEFINED) \
   M(R32_SINT, 43, VK_FORMAT_R32_SINT, VK_FORMAT_UNDEFINED) \
   M(R8G8_UNORM, 49, VK_FORMAT_R8G8_UNORM, VK_FORMAT_UNDEFINED) \
   M(R8G8_UINT, 50, VK_FORMAT_R8G8_UINT, VK_FORMAT_UNDEFINED) \
   M(R8G8_SNORM, 51, VK_FORMAT_R8G8_SNORM, VK_FORMAT_UNDEFINED) \
   M(R8G8_SINT, 52, VK_FORMAT_R8G8_SINT, VK_FORMAT_UNDEFINED) \
   M(R16_FLOAT, 54, VK_FORMAT_R16_SFLOAT, VK_FORMAT_UNDEFINED) \
   M(R16_UNORM, 56, VK_FORMAT_R16_UNORM, VK_FORMAT_UNDEFINED) \
   M(R16_UINT, 57, VK_FORMAT_R16_UINT, VK_FORMAT_UNDEFINED) \
   M(R16_SNORM, 58, VK_FORMAT_R16_SNORM, VK_FORMAT_UNDEFINED) \
   M(R16_SINT, 59, VK_FORMAT_R16_SINT, VK_FORMAT_UNDEFINED) \
   M(R8_UNORM, 61, VK_FORMAT_R8_UNORM, VK_FORMAT_UNDEFINED) \
   M(R8_UINT, 62, VK_FORMAT_R8_UINT, VK_FORMAT_UNDEFINED) \
   M(R8_SNORM, 63, VK_FORMAT_R8_SNORM, VK_FORMAT_UNDEFINED) \
   M(R8_SINT, 64, VK_FORMAT_R8_SINT, VK_FORMAT_UNDEFINED) \
   M(A8_UNORM, 65, VK_FORMAT_R8_UNORM, VK_FORMAT_UNDEFINED) \
   M(B8G8R8A8_UNORM, 87, VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_SRGB) \
   M(B8G8R8X8_UNORM, 88, VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_SRGB) \
   M(B8G8R8A8_SRGB, 91, VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_SRGB) \
   M(B8G8R8X8_SRGB, 93, VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_SRGB)

struct yttrium_shared_color_format {
   uint32_t dxgi_format;
   uint32_t vk_format;
   uint32_t alternate_view_format;
};

static inline bool
yttrium_shared_color_format(uint32_t resource_format,
                             struct yttrium_shared_color_format *format)
{
   switch (resource_format) {
#define YTTRIUM_SHARED_COLOR_CASE(name, dxgi, vk, alternate) \
   case VIRGL_FORMAT_##name: \
      format->dxgi_format = dxgi; \
      format->vk_format = vk; \
      format->alternate_view_format = alternate; \
      return true;
      YTTRIUM_SHARED_COLOR_FORMATS(YTTRIUM_SHARED_COLOR_CASE)
#undef YTTRIUM_SHARED_COLOR_CASE
   default:
      return false;
   }
}

static inline uint32_t
yttrium_optimal_image_usage(uint32_t bind)
{
   return VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
      ((bind & VIRGL_BIND_SAMPLER_VIEW) ? VK_IMAGE_USAGE_SAMPLED_BIT : 0) |
      ((bind & VIRGL_BIND_RENDER_TARGET) ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT : 0) |
      ((bind & YTTRIUM_MESA_SHARED_IMAGE_SHADER_IMAGE_BIND) ?
         VK_IMAGE_USAGE_STORAGE_BIT : 0);
}

#pragma pack(push, 1)
struct yttrium_shared_image_data {
   VIOGPU_CREATE_ALLOCATION_EXCHANGE allocation;
   uint32_t magic;
   uint32_t version;
   uint32_t bytes;
   uint32_t format;
   uint32_t resource_flags;
   uint32_t resource_layout;
   uint32_t initial_state;
   uint32_t heap_flags;
   uint64_t heap_alignment;
   uint32_t creation_node_mask;
   uint32_t visible_node_mask;
   uint32_t reserved[4];
};

/* Version 2 shares one OPTIMAL image without a Windows-visible linear plane.
 * Both endpoints recreate the exact Vulkan image and dedicated-memory contract.
 * Image type, tiling, levels, layers and samples are fixed by this version.
 * RGBA8/BGRA8 storage declares UNORM and sRGB views. Logical A8 uses one R8
 * view, with VK_FORMAT_UNDEFINED in the unused second view-format slot.
 * Version 4 preserves the first 168 bytes and appends the exported DMA_BUF
 * image's DRM modifier and plane layouts. It uses explicit DRM-modifier tiling
 * and MUTABLE_FORMAT without ALIAS. Plane layouts describe the modifier's
 * memory planes, including auxiliary planes, rather than format color planes.
 * Parsers copy either wire version into a zero-initialized full-size object. */
struct yttrium_optimal_image_data {
   VIOGPU_CREATE_ALLOCATION_EXCHANGE allocation;
   uint32_t magic;
   uint32_t version;
   uint32_t bytes;
   uint32_t dxgi_format;
   uint32_t vk_format;
   uint32_t vk_usage;
   uint32_t vk_flags;
   uint32_t handle_types;
   uint32_t memory_type_index;
   uint32_t dedicated;
   uint64_t memory_alignment;
   uint32_t view_formats[2];
   uint32_t reserved[2];
   uint8_t device_uuid[VK_UUID_SIZE];
   uint8_t driver_uuid[VK_UUID_SIZE];
   uint64_t drm_modifier;
   uint32_t drm_plane_count;
   uint32_t drm_reserved;
   struct {
      uint64_t offset;
      uint64_t row_pitch;
   } drm_planes[YTTRIUM_OPTIMAL_IMAGE_MAX_DRM_PLANES];
};

/* Version 3 describes only the runtime-visible LINEAR presentation endpoint.
 * The creator's canonical OPTIMAL render storage is private and absent from
 * this record. Importers consume this endpoint without private render history.
 * The format field retains logical DXGI RGBA8/BGRA8 UNORM or sRGB semantics.
 * The prefix and physical LINEAR image use the matching channel-order UNORM
 * format, with mutable UNORM/sRGB views, DMA_BUF external memory, and
 * SAMPLED | TRANSFER_SRC | TRANSFER_DST usage (never COLOR_ATTACHMENT).
 */
struct yttrium_display_endpoint_data {
   VIOGPU_CREATE_ALLOCATION_EXCHANGE allocation;
   uint32_t magic;
   uint32_t version;
   uint32_t bytes;
   uint32_t format;
   uint32_t role;
   uint32_t reserved[11];
};
#pragma pack(pop)

typedef char yttrium_shared_prefix_size[
   (sizeof(VIOGPU_CREATE_ALLOCATION_EXCHANGE) == 72) ? 1 : -1];
typedef char yttrium_shared_trailer_offset[
   (offsetof(struct yttrium_shared_image_data, magic) == 72) ? 1 : -1];
typedef char yttrium_shared_wire_size[
   (sizeof(struct yttrium_shared_image_data) == 136) ? 1 : -1];
typedef char yttrium_optimal_trailer_offset[
   (offsetof(struct yttrium_optimal_image_data, magic) == 72) ? 1 : -1];
typedef char yttrium_optimal_v2_wire_size[
   (offsetof(struct yttrium_optimal_image_data, drm_modifier) ==
      YTTRIUM_OPTIMAL_IMAGE_V2_BYTES) ? 1 : -1];
typedef char yttrium_optimal_wire_size[
   (sizeof(struct yttrium_optimal_image_data) ==
      YTTRIUM_OPTIMAL_IMAGE_DRM_BYTES) ? 1 : -1];
typedef char yttrium_display_endpoint_trailer_offset[
   (offsetof(struct yttrium_display_endpoint_data, magic) == 72) ? 1 : -1];
typedef char yttrium_display_endpoint_role_offset[
   (offsetof(struct yttrium_display_endpoint_data, role) == 88) ? 1 : -1];
typedef char yttrium_display_endpoint_wire_size[
   (sizeof(struct yttrium_display_endpoint_data) == 136) ? 1 : -1];

static inline bool
yttrium_shared_image_wire_valid(uint32_t resource_magic, uint32_t resource_bytes,
                               uint32_t allocation_magic, uint32_t allocation_bytes)
{
   return resource_magic == YTTRIUM_MESA_SHARED_IMAGE_MAGIC &&
      allocation_magic == resource_magic &&
      resource_bytes == sizeof(VIOGPU_CREATE_RESOURCE_EXCHANGE) &&
      (allocation_bytes == sizeof(struct yttrium_shared_image_data) ||
       allocation_bytes == YTTRIUM_OPTIMAL_IMAGE_V2_BYTES ||
       allocation_bytes == YTTRIUM_OPTIMAL_IMAGE_DRM_BYTES);
}

static inline bool
yttrium_shared_image_uuid_valid(const uint8_t uuid[VK_UUID_SIZE])
{
   for (unsigned i = 0; i < VK_UUID_SIZE; i++) {
      if (uuid[i])
         return true;
   }
   return false;
}

/* DRM_FORMAT_MOD_INVALID is the vendor-NONE reserved 56-bit payload from
 * drm_fourcc.h, not UINT64_MAX. Neither sentinel describes importable storage. */
static inline bool
yttrium_optimal_image_modifier_valid(uint64_t modifier)
{
   return modifier && modifier != UINT64_C(0x00ffffffffffffff) &&
      modifier != UINT64_MAX;
}

static inline uint32_t
yttrium_optimal_image_view_count(const struct yttrium_optimal_image_data *data)
{
   return data->view_formats[1] == VK_FORMAT_UNDEFINED ? 1 : 2;
}

static inline bool
yttrium_mesa_optimal_image_valid(const struct yttrium_optimal_image_data *data)
{
   if (!data)
      return false;
   const VIOGPU_CREATE_ALLOCATION_EXCHANGE *a = &data->allocation;
   const uint32_t format = a->ResourceOptions.format;
   struct yttrium_shared_color_format color;
   if (!yttrium_shared_color_format(format, &color))
      return false;
   const uint32_t required_usage = yttrium_optimal_image_usage(a->ResourceOptions.bind);
   const uint32_t optional_usage =
      (a->ResourceOptions.bind & VIRGL_BIND_RENDER_TARGET) &&
      (a->ResourceOptions.bind & VIRGL_BIND_SAMPLER_VIEW) ?
         VK_IMAGE_USAGE_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT : 0;
   const bool drm = data->version == YTTRIUM_OPTIMAL_IMAGE_DRM_VERSION;
   const uint32_t expected_bytes = drm ? YTTRIUM_OPTIMAL_IMAGE_DRM_BYTES :
      YTTRIUM_OPTIMAL_IMAGE_V2_BYTES;
   const uint32_t required_flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT |
      (drm ? 0 : VK_IMAGE_CREATE_ALIAS_BIT);
   const uint32_t handle_type = drm ? VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT :
      VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
   if (data->magic != YTTRIUM_MESA_SHARED_IMAGE_MAGIC ||
       (data->version != 2 && !drm) || data->bytes != expected_bytes ||
       data->dxgi_format != color.dxgi_format ||
       data->vk_format != color.vk_format ||
       data->view_formats[0] != color.vk_format ||
       data->view_formats[1] != color.alternate_view_format ||
       (data->vk_usage & required_usage) != required_usage ||
       (data->vk_usage & ~(required_usage | optional_usage)) ||
       (data->vk_flags & required_flags) != required_flags ||
       (data->vk_flags & ~(required_flags | VK_IMAGE_CREATE_MULTISAMPLED_RENDER_TO_SINGLE_SAMPLED_BIT_EXT)) ||
       data->handle_types != handle_type ||
       data->memory_type_index >= 32 || data->dedicated > 1 ||
       !data->memory_alignment ||
       (data->memory_alignment & (data->memory_alignment - 1)) ||
       data->reserved[0] || data->reserved[1] ||
       !yttrium_shared_image_uuid_valid(data->device_uuid) ||
       !yttrium_shared_image_uuid_valid(data->driver_uuid))
      return false;
   if (data->drm_reserved)
      return false;
   if (drm) {
      if (!yttrium_optimal_image_modifier_valid(data->drm_modifier) ||
          !data->drm_plane_count ||
          data->drm_plane_count > YTTRIUM_OPTIMAL_IMAGE_MAX_DRM_PLANES)
         return false;
   } else if (data->drm_modifier || data->drm_plane_count) {
      return false;
   }
   for (unsigned i = 0; i < YTTRIUM_OPTIMAL_IMAGE_MAX_DRM_PLANES; i++) {
      if (i < data->drm_plane_count) {
         /* Tiled and auxiliary planes have modifier-specific extents. Check
          * only representable bounds here; Vulkan verifies the exact layout. */
         if (!data->drm_planes[i].row_pitch ||
             data->drm_planes[i].offset >= a->Size)
            return false;
      } else if (data->drm_planes[i].offset || data->drm_planes[i].row_pitch) {
         return false;
      }
   }
   /* DISPLAY_TARGET denotes composed-window sharing, not linear scanout. */
   return a->ResourceOptions.target == 2 &&
      (a->ResourceOptions.bind & VIRGL_BIND_SHARED) &&
      !(a->ResourceOptions.bind & ~YTTRIUM_MESA_SHARED_IMAGE_BIND_MASK) &&
      a->ResourceOptions.width && a->ResourceOptions.width <= 16384 &&
      a->ResourceOptions.height && a->ResourceOptions.height <= 16384 &&
      a->ResourceOptions.depth == 1 && a->ResourceOptions.array_size == 1 &&
      !a->ResourceOptions.last_level && a->ResourceOptions.nr_samples <= 1 &&
      !a->ResourceOptions.flags && a->Size && a->Size <= UINT32_MAX &&
      a->BlobId && a->BlobMem == 2 && a->BlobFlags == 2 &&
      !a->Stride && !a->ScanoutOffset;
}

static inline uint32_t
yttrium_display_endpoint_resource_format(uint32_t dxgi_format)
{
   switch (dxgi_format) {
   case 28:
   case 29:
      return VIRGL_FORMAT_R8G8B8A8_UNORM;
   case 87:
   case 91:
      return VIRGL_FORMAT_B8G8R8A8_UNORM;
   default:
      return 0;
   }
}

static inline bool
yttrium_display_endpoint_valid(const struct yttrium_display_endpoint_data *data)
{
   if (!data)
      return false;
   const VIOGPU_CREATE_ALLOCATION_EXCHANGE *a = &data->allocation;
   const uint32_t resource_format =
      yttrium_display_endpoint_resource_format(data->format);
   const uint32_t required_bind = VIRGL_BIND_RENDER_TARGET |
      VIRGL_BIND_DISPLAY_TARGET;
   if (data->magic != YTTRIUM_MESA_SHARED_IMAGE_MAGIC || data->version != 3 ||
       data->bytes != sizeof(*data) || !resource_format ||
       data->role != YTTRIUM_DISPLAY_ENDPOINT_ROLE_LINEAR_SCANOUT)
      return false;
   for (unsigned i = 0; i < 11; i++) {
      if (data->reserved[i])
         return false;
   }
   if (a->ResourceOptions.target != 2 ||
       a->ResourceOptions.format != resource_format ||
       (a->ResourceOptions.bind & required_bind) != required_bind ||
       (a->ResourceOptions.bind & ~(required_bind | VIRGL_BIND_SAMPLER_VIEW)) ||
       !a->ResourceOptions.width || a->ResourceOptions.width > 16384 ||
       !a->ResourceOptions.height || a->ResourceOptions.height > 16384 ||
       a->ResourceOptions.depth != 1 || a->ResourceOptions.array_size != 1 ||
       a->ResourceOptions.last_level || a->ResourceOptions.nr_samples > 1 ||
       a->ResourceOptions.flags || !a->Size || a->Size > UINT32_MAX ||
       !a->BlobId || a->BlobMem != 2 || a->BlobFlags != 3 ||
       a->Stride < a->ResourceOptions.width * 4 || (a->Stride & 3) ||
       (a->ScanoutOffset & 3) || a->ScanoutOffset >= a->Size)
      return false;
   return (uint64_t)a->ScanoutOffset +
      (uint64_t)a->Stride * (a->ResourceOptions.height - 1) +
      (uint64_t)a->ResourceOptions.width * 4 <= a->Size;
}

#endif
