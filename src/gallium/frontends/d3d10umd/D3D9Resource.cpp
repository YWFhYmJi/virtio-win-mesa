/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright 2026 Ake Rehnman
 */
#include "D3D9Private.h"

#include "gallium/winsys/yttrium/gdi/yttrium_gdi_public.h"

#include "util/format/u_format.h"
#include "util/format_srgb.h"
#include "util/u_math.h"
#include "util/u_threaded_context.h"
#include "util/u_upload_mgr.h"
#include "virtio/virtio-gpu/virgl_hw.h"
#include "virtio/wddm/viogpu_wddm_driver.h"

#include <stdlib.h>

static bool
D3D9OrderedContextWorkerEnabled()
{
   static int enabled = -1;

   if (enabled < 0) {
      enabled = yttrium_gdi_debug_get_bool_option(
         "D3D10UMD_YTTRIUM_ORDERED_CONTEXT_WORKER", true) ? 1 : 0;
   }
   return enabled != 0;
}

static void
D3D9ReleaseWorkerBufferUpload(D3D9Device *device, D3D9SubResource *sub)
{
   if (!sub || !sub->worker_upload_buffer)
      return;

   if (device && device->pipe) {
      pipe_resource_release(device->pipe, sub->worker_upload_buffer);
      sub->worker_upload_buffer = NULL;
   } else {
      pipe_resource_reference(&sub->worker_upload_buffer, NULL);
   }
   sub->worker_upload_offset = 0;
   sub->worker_upload_source_offset = 0;
   sub->worker_upload_size = 0;
}

static bool
D3D9CaptureWorkerBufferUpload(D3D9Device *device, D3D9SubResource *sub)
{
   if (!device || !device->pipe || !device->pipe->stream_uploader || !sub ||
       !sub->transfer_map || sub->transfer_box.x < 0 ||
       sub->transfer_box.width <= 0)
      return false;

   const unsigned source_offset = (unsigned)sub->transfer_box.x;
   const unsigned size = (unsigned)sub->transfer_box.width;
   unsigned upload_offset = 0;
   struct pipe_resource *upload_buffer = NULL;
   u_upload_data_ref(device->pipe->stream_uploader, source_offset, size, 16,
                     sub->transfer_map, &upload_offset, &upload_buffer);
   if (!upload_buffer || upload_offset < source_offset)
      return false;

   D3D9ReleaseWorkerBufferUpload(device, sub);
   sub->worker_upload_buffer = upload_buffer;
   sub->worker_upload_offset = upload_offset;
   sub->worker_upload_source_offset = source_offset;
   sub->worker_upload_size = size;
   return true;
}

static bool
D3D9ResourcesMatchForManagedUpload(const D3D9Resource *src,
                                   const D3D9Resource *dst,
                                   UINT *src_base_subresource);
static bool
D3D9ResourcesMatchForVolumeUpload(const D3D9Resource *src,
                                  const D3D9Resource *dst,
                                  UINT *src_base_subresource);
static bool
D3D9ManagedUploadSourceClean(const D3D9Resource *src,
                             const D3D9Resource *dst);

static UINT
D3D9FormatBytesPerPixel(D3DDDIFORMAT format)
{
   switch (format) {
   case D3DDDIFMT_A8R8G8B8:
   case D3DDDIFMT_X8R8G8B8:
   case D3DDDIFMT_A8B8G8R8:
   case D3DDDIFMT_X8B8G8R8:
   case D3DDDIFMT_A2R10G10B10:
   case D3D9_FMT_A2B10G10R10:
   case D3DDDIFMT_D24X8:
   case D3DDDIFMT_D24S8:
   case D3D9_FMT_DF24:
   case D3D9_FMT_INTZ:
   case D3DDDIFMT_INDEX32:
   case D3D9_FMT_G16R16:
   case D3D9_FMT_G16R16F:
   case D3D9_FMT_R32F:
   case D3D9_FMT_Q8W8V8U8:
   case D3D9_FMT_V16U16:
   case D3D9_FMT_D32F_LOCKABLE:
      return 4;
   case D3D9_FMT_A16B16G16R16:
   case D3D9_FMT_A16B16G16R16F:
   case D3D9_FMT_G32R32F:
   case D3D9_FMT_Q16W16V16U16:
      return 8;
   case D3D9_FMT_A32B32G32R32F:
      return 16;
   case D3DDDIFMT_R5G6B5:
   case D3DDDIFMT_X1R5G5B5:
   case D3DDDIFMT_A1R5G5B5:
   case D3D9_FMT_G8R8:
   case D3DDDIFMT_D16:
   case D3D9_FMT_DF16:
   case D3DDDIFMT_INDEX16:
   case D3DDDIFMT_A4R4G4B4:
   case D3D9_FMT_X4R4G4B4:
   case D3D9_FMT_A8P8:
   case D3D9_FMT_A8L8:
   case D3D9_FMT_D16_LOCKABLE:
   case D3D9_FMT_L16:
   case D3D9_FMT_R16F:
   case D3D9_FMT_V8U8:
      return 2;
   case D3D9_FMT_R8:
   case D3DDDIFMT_A8:
   case D3D9_FMT_P8:
   case D3D9_FMT_L8:
      return 1;
   default:
      return 4;
   }
}

static bool
D3D9FormatIsBlockCompressed(D3DDDIFORMAT format)
{
   switch (format) {
   case D3D9_FMT_DXT1:
   case D3D9_FMT_DXT2:
   case D3D9_FMT_DXT3:
   case D3D9_FMT_DXT4:
   case D3D9_FMT_DXT5:
   case D3D9_FMT_ATI1:
      return true;
   default:
      return false;
   }
}

static bool
D3D9FormatsHaveSameMemoryLayout(D3DDDIFORMAT a, D3DDDIFORMAT b)
{
   if (a == b)
      return true;

   return (a == D3DDDIFMT_A8R8G8B8 && b == D3DDDIFMT_X8R8G8B8) ||
      (a == D3DDDIFMT_X8R8G8B8 && b == D3DDDIFMT_A8R8G8B8);
}

static UINT
D3D9FormatBlockSize(D3DDDIFORMAT format)
{
   switch (format) {
   case D3D9_FMT_DXT1:
   case D3D9_FMT_ATI1:
      return 8;
   case D3D9_FMT_DXT2:
   case D3D9_FMT_DXT3:
   case D3D9_FMT_DXT4:
   case D3D9_FMT_DXT5:
      return 16;
   default:
      return D3D9FormatBytesPerPixel(format);
   }
}

static UINT
D3D9FormatRowPitch(D3DDDIFORMAT format, UINT width)
{
   if (D3D9FormatIsBlockCompressed(format))
      return MAX2(1u, (width + 3u) / 4u) * D3D9FormatBlockSize(format);
   return width * D3D9FormatBytesPerPixel(format);
}

static UINT
D3D9AlignPitch(UINT pitch)
{
   return (pitch + 3u) & ~3u;
}

static UINT
D3D9FormatRowCount(D3DDDIFORMAT format, UINT height)
{
   if (D3D9FormatIsBlockCompressed(format))
      return MAX2(1u, (height + 3u) / 4u);
   return height ? height : 1;
}

static enum pipe_format
D3D9FormatToPipe(D3DDDIFORMAT format)
{
   switch (format) {
   case D3DDDIFMT_A8R8G8B8:
      return PIPE_FORMAT_B8G8R8A8_UNORM;
   case D3DDDIFMT_X8R8G8B8:
      return PIPE_FORMAT_B8G8R8X8_UNORM;
   case D3DDDIFMT_A8B8G8R8:
      return PIPE_FORMAT_R8G8B8A8_UNORM;
   case D3DDDIFMT_X8B8G8R8:
      return PIPE_FORMAT_R8G8B8X8_UNORM;
   case D3DDDIFMT_R5G6B5:
      return PIPE_FORMAT_B5G6R5_UNORM;
   case D3DDDIFMT_A1R5G5B5:
      return PIPE_FORMAT_B5G5R5A1_UNORM;
   case D3DDDIFMT_A2R10G10B10:
      return PIPE_FORMAT_B10G10R10A2_UNORM;
   case D3D9_FMT_A2B10G10R10:
      return PIPE_FORMAT_R10G10B10A2_UNORM;
   case D3DDDIFMT_A8:
      return PIPE_FORMAT_A8_UNORM;
   case D3D9_FMT_R8:
      return PIPE_FORMAT_R8_UNORM;
   case D3D9_FMT_P8:
   case D3D9_FMT_L8:
      return PIPE_FORMAT_R8_UNORM;
   case D3D9_FMT_A8P8:
   case D3D9_FMT_A8L8:
   case D3D9_FMT_G8R8:
      return PIPE_FORMAT_R8G8_UNORM;
   case D3D9_FMT_L16:
      return PIPE_FORMAT_R16_UNORM;
   case D3D9_FMT_G16R16:
      return PIPE_FORMAT_R16G16_UNORM;
   case D3D9_FMT_A16B16G16R16:
      return PIPE_FORMAT_R16G16B16A16_UNORM;
   case D3D9_FMT_V8U8:
      return PIPE_FORMAT_R8G8_SNORM;
   case D3D9_FMT_Q8W8V8U8:
      return PIPE_FORMAT_R8G8B8A8_SNORM;
   case D3D9_FMT_V16U16:
      return PIPE_FORMAT_R16G16_SNORM;
   case D3D9_FMT_Q16W16V16U16:
      return PIPE_FORMAT_R16G16B16A16_SNORM;
   case D3D9_FMT_R16F:
      return PIPE_FORMAT_R16_FLOAT;
   case D3D9_FMT_G16R16F:
      return PIPE_FORMAT_R16G16_FLOAT;
   case D3D9_FMT_A16B16G16R16F:
      return PIPE_FORMAT_R16G16B16A16_FLOAT;
   case D3D9_FMT_R32F:
      return PIPE_FORMAT_R32_FLOAT;
   case D3D9_FMT_G32R32F:
      return PIPE_FORMAT_R32G32_FLOAT;
   case D3D9_FMT_A32B32G32R32F:
      return PIPE_FORMAT_R32G32B32A32_FLOAT;
   case D3DDDIFMT_D16:
   case D3D9_FMT_D16_LOCKABLE:
   case D3D9_FMT_DF16:
      return PIPE_FORMAT_Z16_UNORM;
   case D3DDDIFMT_D24S8:
      return PIPE_FORMAT_Z24_UNORM_S8_UINT;
   case D3DDDIFMT_D24X8:
   case D3D9_FMT_DF24:
   case D3D9_FMT_INTZ:
      return PIPE_FORMAT_Z24X8_UNORM;
   case D3D9_FMT_DXT1:
      return PIPE_FORMAT_DXT1_RGBA;
   case D3D9_FMT_DXT2:
   case D3D9_FMT_DXT3:
      return PIPE_FORMAT_DXT3_RGBA;
   case D3D9_FMT_DXT4:
   case D3D9_FMT_DXT5:
      return PIPE_FORMAT_DXT5_RGBA;
   case D3D9_FMT_ATI1:
      return PIPE_FORMAT_RGTC1_UNORM;
   default:
      return PIPE_FORMAT_NONE;
   }
}

static D3DDDIFORMAT
D3D9PipeToFormat(enum pipe_format format)
{
   switch (format) {
   case PIPE_FORMAT_B8G8R8A8_UNORM:
      return D3DDDIFMT_A8R8G8B8;
   case PIPE_FORMAT_B8G8R8X8_UNORM:
      return D3DDDIFMT_X8R8G8B8;
   case PIPE_FORMAT_R8G8B8A8_UNORM:
      return D3DDDIFMT_A8B8G8R8;
   case PIPE_FORMAT_R8G8B8X8_UNORM:
      return D3DDDIFMT_X8B8G8R8;
   case PIPE_FORMAT_B5G6R5_UNORM:
      return D3DDDIFMT_R5G6B5;
   case PIPE_FORMAT_B5G5R5A1_UNORM:
      return D3DDDIFMT_A1R5G5B5;
   case PIPE_FORMAT_Z16_UNORM:
      return D3DDDIFMT_D16;
   case PIPE_FORMAT_Z24X8_UNORM:
      return D3DDDIFMT_D24X8;
   case PIPE_FORMAT_Z24_UNORM_S8_UINT:
      return D3DDDIFMT_D24S8;
   default:
      return D3DDDIFMT_UNKNOWN;
   }
}

static D3DDDIFORMAT
D3D9VirglToFormat(ULONG format)
{
   switch (format) {
   case VIRGL_FORMAT_B8G8R8A8_UNORM:
      return D3DDDIFMT_A8R8G8B8;
   case VIRGL_FORMAT_B8G8R8X8_UNORM:
      return D3DDDIFMT_X8R8G8B8;
   case VIRGL_FORMAT_R8G8B8A8_UNORM:
      return D3DDDIFMT_A8B8G8R8;
   case VIRGL_FORMAT_R8G8B8X8_UNORM:
      return D3DDDIFMT_X8B8G8R8;
   case VIRGL_FORMAT_B5G6R5_UNORM:
      return D3DDDIFMT_R5G6B5;
   case VIRGL_FORMAT_B5G5R5A1_UNORM:
      return D3DDDIFMT_A1R5G5B5;
   case VIRGL_FORMAT_Z16_UNORM:
      return D3DDDIFMT_D16;
   case VIRGL_FORMAT_Z24X8_UNORM:
      return D3DDDIFMT_D24X8;
   case VIRGL_FORMAT_Z24_UNORM_S8_UINT:
      return D3DDDIFMT_D24S8;
   default:
      return D3DDDIFMT_UNKNOWN;
   }
}

static enum pipe_format
D3D9ResourcePipeFormat(const D3DDDIARG_CREATERESOURCE *data)
{
   if (data->Flags.VertexBuffer || data->Flags.IndexBuffer)
      return PIPE_FORMAT_R8_UINT;
   return D3D9FormatToPipe(data->Format);
}

static const VIOGPU_CREATE_ALLOCATION_EXCHANGE *
D3D9OpenResourceAllocationPrivate(const D3DDDIARG_OPENRESOURCE *data)
{
   if (!data || !data->pOpenAllocationInfo || data->NumAllocations != 1 ||
       !data->pOpenAllocationInfo[0].pPrivateDriverData ||
       data->pOpenAllocationInfo[0].PrivateDriverDataSize <
          sizeof(VIOGPU_CREATE_ALLOCATION_EXCHANGE))
      return NULL;

   return (const VIOGPU_CREATE_ALLOCATION_EXCHANGE *)
      data->pOpenAllocationInfo[0].pPrivateDriverData;
}

static bool
D3D9OpenResourceIsRuntimeStandardPrimary(const D3DDDIARG_OPENRESOURCE *data,
                                         const VIOGPU_CREATE_ALLOCATION_EXCHANGE **out_alloc)
{
   const VIOGPU_CREATE_ALLOCATION_EXCHANGE *alloc =
      D3D9OpenResourceAllocationPrivate(data);
   if (!alloc)
      return false;

   if (out_alloc)
      *out_alloc = alloc;

   return data->Flags.Fullscreen && alloc->BlobMem == 0 &&
      (alloc->ResourceOptions.bind & VIRGL_BIND_DISPLAY_TARGET) != 0;
}

static bool
D3D9PoolNeedsReadback(D3DDDI_POOL pool);

static bool
D3D9ResourceIsColorTarget(const D3DDDIARG_CREATERESOURCE *data)
{
   return data->Flags.RenderTarget || data->Flags.Primary ||
      data->Flags.DiscardRenderTarget;
}

static bool
D3D9ResourceUsesDisplayTargetBind(const D3DDDIARG_CREATERESOURCE *data)
{
   if (data->Flags.Primary)
      return true;

   if (data->MultisampleType)
      return false;

   /* A swapchain backbuffer that will be presented/flipped must be an exported
    * LINEAR Venus display image.  Legacy D3DSWAPEFFECT_DISCARD backbuffers
    * arrive as non-Texture render targets (or DiscardRenderTarget) and already
    * hit this.  D3D9Ex FLIPEX backbuffers instead arrive as Texture render
    * targets marked SharedResource (cross-process, for DWM); without
    * DISPLAY_TARGET they were built as non-scannable OPTIMAL device-local images
    * and the kernel flip presented black.  Treat a shared render target as a
    * display target too (mirroring the unconditional D3D10_DDI_BIND_PRESENT ->
    * PIPE_BIND_DISPLAY_TARGET on the DXGI side), while still excluding
    * cube/volume targets and leaving plain (non-shared) render-to-texture
    * surfaces untouched so they keep OPTIMAL tiling. */
   return data->Flags.DiscardRenderTarget ||
      (data->Flags.RenderTarget && !data->Flags.CubeMap &&
       !data->Flags.Volume &&
       (data->Flags.SharedResource || !data->Flags.Texture));
}

static bool
D3D9ResourceCanBeColorCopyTarget(const D3DDDIARG_CREATERESOURCE *data)
{
   if (data->Flags.VertexBuffer || data->Flags.IndexBuffer)
      return false;

   switch (data->Format) {
   case D3DDDIFMT_A8R8G8B8:
   case D3DDDIFMT_X8R8G8B8:
   case D3DDDIFMT_A8B8G8R8:
   case D3DDDIFMT_X8B8G8R8:
   case D3DDDIFMT_R5G6B5:
   case D3DDDIFMT_A1R5G5B5:
   case D3DDDIFMT_X1R5G5B5:
   case D3DDDIFMT_A4R4G4B4:
   case D3D9_FMT_X4R4G4B4:
   case D3DDDIFMT_A2R10G10B10:
   case D3D9_FMT_A2B10G10R10:
   case D3D9_FMT_A16B16G16R16:
      return true;
   default:
      return false;
   }
}

static unsigned
D3D9PipeBind(const D3DDDIARG_CREATERESOURCE *data)
{
   const enum pipe_format format = D3D9ResourcePipeFormat(data);
   const bool depth = format == PIPE_FORMAT_Z16_UNORM ||
      format == PIPE_FORMAT_Z24X8_UNORM ||
      format == PIPE_FORMAT_Z24_UNORM_S8_UINT;

   if (data->Flags.VertexBuffer)
      return PIPE_BIND_VERTEX_BUFFER;
   if (data->Flags.IndexBuffer)
      return PIPE_BIND_INDEX_BUFFER;
   if (depth) {
      unsigned bind = PIPE_BIND_DEPTH_STENCIL | PIPE_BIND_SHARED;
      if (data->Flags.Texture || data->Flags.CubeMap || data->Flags.Volume)
         bind |= PIPE_BIND_SAMPLER_VIEW;
      return bind;
   }

   unsigned bind = 0;
   if (D3D9ResourceIsColorTarget(data)) {
      bind |= PIPE_BIND_RENDER_TARGET | PIPE_BIND_SHARED;
      if (D3D9ResourceUsesDisplayTargetBind(data))
         bind |= PIPE_BIND_DISPLAY_TARGET;
   }
   if (data->Flags.Texture || data->Flags.CubeMap || data->Flags.Volume)
      bind |= PIPE_BIND_SAMPLER_VIEW;
   if (data->Flags.AutogenMipmap &&
       D3D9ResourceCanBeColorCopyTarget(data))
      bind |= PIPE_BIND_RENDER_TARGET | PIPE_BIND_SHARED;
   if (D3D9PoolNeedsReadback(data->Pool) &&
       D3D9ResourceCanBeColorCopyTarget(data))
      bind |= PIPE_BIND_RENDER_TARGET | PIPE_BIND_SHARED;
   return bind;
}

static UINT
D3D9PipeArraySize(const D3DDDIARG_CREATERESOURCE *data)
{
   if (!data || data->Flags.VertexBuffer || data->Flags.IndexBuffer ||
       data->Flags.Volume || data->Flags.Primary)
      return 1;

   if (data->Flags.CubeMap)
      return 6;

   const UINT mip_levels = data->MipLevels ? data->MipLevels : 1;
   if (data->SurfCount > mip_levels && data->SurfCount % mip_levels == 0)
      return data->SurfCount / mip_levels;

   return 1;
}

static enum pipe_texture_target
D3D9PipeTarget(const D3DDDIARG_CREATERESOURCE *data)
{
   if (data->Flags.VertexBuffer || data->Flags.IndexBuffer)
      return PIPE_BUFFER;
   if (data->Flags.CubeMap)
      return PIPE_TEXTURE_CUBE;
   if (data->Flags.Volume)
      return PIPE_TEXTURE_3D;
   if (D3D9PipeArraySize(data) > 1)
      return PIPE_TEXTURE_2D_ARRAY;
   return PIPE_TEXTURE_2D;
}

static enum pipe_resource_usage
D3D9PipeUsage(D3DDDI_POOL pool)
{
   switch (pool) {
   case D3DDDIPOOL_SYSTEMMEM:
   case D3D9_POOL_STAGINGMEM:
      return PIPE_USAGE_STAGING;
   default:
      return PIPE_USAGE_DEFAULT;
   }
}

static D3DKMT_HANDLE
D3D9GetPipeResourceAllocation(D3D9Device *device,
                              struct pipe_resource *pipe_resource)
{
   struct winsys_handle whandle;

   if (!device || !device->screen || !device->screen->resource_get_handle ||
       !pipe_resource)
      return 0;

   memset(&whandle, 0, sizeof(whandle));
   whandle.type = WINSYS_HANDLE_TYPE_D3DKMT_ALLOC;
   if (!device->screen->resource_get_handle(device->screen, device->pipe,
                                            pipe_resource, &whandle, 0))
      return 0;

   return (D3DKMT_HANDLE)(uintptr_t)whandle.handle;
}

static D3DKMT_HANDLE
D3D9GetPipeAllocation(D3D9Device *device, D3D9Resource *resource)
{
   if (!resource)
      return 0;

   return D3D9GetPipeResourceAllocation(device, resource->pipe_resource);
}

bool
D3D9UploadSubResource(D3D9Device *device, D3D9Resource *resource,
                      UINT subresource_index);

static bool
D3D9CopyTextureToCpuDestination(D3D9Device *device,
                                D3D9Resource *dst_resource,
                                D3D9SubResource *dst_sub,
                                UINT dst_subresource_index,
                                const RECT *dst_rect,
                                D3D9Resource *src_resource,
                                D3D9SubResource *src_sub,
                                UINT src_subresource_index,
                                const RECT *src_rect);

static bool
D3D9PoolNeedsReadback(D3DDDI_POOL pool)
{
   return pool != D3DDDIPOOL_SYSTEMMEM && pool != D3D9_POOL_STAGINGMEM;
}

static bool
D3D9EnsureCpuBackingSize(D3D9SubResource *sub, size_t size)
{
   if (!sub)
      return false;
   if (size < sub->size)
      size = sub->size;
   if (sub->data && sub->data_capacity >= size)
      return true;

   uint8_t *data = (uint8_t *)calloc(1, size ? size : 1);
   if (!data)
      return false;

   if (sub->data) {
      memcpy(data, sub->data, MIN2(sub->data_capacity, size));
      if (sub->owns_data)
         free(sub->data);
   }
   sub->data = data;
   sub->data_capacity = size;
   sub->owns_data = true;
   return true;
}

static bool
D3D9EnsureCpuBacking(D3D9SubResource *sub)
{
   return D3D9EnsureCpuBackingSize(sub, sub ? sub->size : 0);
}

static size_t
D3D9PackedMipChainSize(const D3D9Resource *resource)
{
   if (!resource || !resource->flags.Texture || resource->flags.CubeMap ||
       resource->flags.Volume || !resource->mip_levels)
      return 0;

   size_t size = 0;
   const UINT count = MIN2(resource->mip_levels, resource->surf_count);
   for (UINT i = 0; i < count; ++i)
      size += resource->surfaces[i].size;
   return size;
}

static bool
D3D9LockCoversFullSubResource(const D3D9SubResource *sub,
                              const struct pipe_box *box)
{
   return sub && box && box->x == 0 && box->y == 0 && box->z == 0 &&
      box->width == (int)sub->width && box->height == (int)sub->height &&
      box->depth == (int)(sub->depth ? sub->depth : 1);
}

static bool
D3D9DistributePackedMipLock(D3D9Resource *resource)
{
   if (!resource || !resource->flags.Texture || resource->flags.CubeMap ||
       resource->flags.Volume || resource->mip_levels <= 1 ||
       resource->surf_count < resource->mip_levels)
      return true;

   D3D9SubResource *base = &resource->surfaces[0];
   if (!base->data)
      return false;

   const size_t packed_size = D3D9PackedMipChainSize(resource);
   if (!packed_size || base->data_capacity < packed_size)
      return false;

   size_t offset = base->size;
   for (UINT i = 1; i < resource->mip_levels; ++i) {
      D3D9SubResource *sub = &resource->surfaces[i];
      if (!D3D9EnsureCpuBacking(sub))
         return false;

      memcpy(sub->data, base->data + offset, sub->size);
      sub->cpu_dirty = true;
      offset += sub->size;
   }

   return true;
}

static bool
D3D9HasContiguousMipData(const D3D9Resource *resource)
{
   if (!resource || !resource->flags.Texture || resource->flags.CubeMap ||
       resource->flags.Volume || resource->mip_levels <= 1 ||
       resource->surf_count < resource->mip_levels ||
       !resource->surfaces[0].data)
      return false;

   const uint8_t *base = resource->surfaces[0].data;
   size_t offset = resource->surfaces[0].size;
   for (UINT i = 1; i < resource->mip_levels; ++i) {
      const D3D9SubResource *sub = &resource->surfaces[i];
      if (sub->data != base + offset)
         return false;
      offset += sub->size;
   }
   return true;
}

static void
D3D9MarkMipChainCpuDirty(D3D9Resource *resource)
{
   if (!resource)
      return;

   const UINT count = MIN2(resource->mip_levels, resource->surf_count);
   for (UINT i = 0; i < count; ++i) {
      if (resource->surfaces[i].data)
         resource->surfaces[i].cpu_dirty = true;
   }
}

static bool
D3D9UploadDirtyTextureSubResources(D3D9Device *device, D3D9Resource *resource)
{
   if (!resource)
      return false;

   for (UINT i = 0; i < resource->surf_count; ++i) {
      if (!D3D9UploadSubResource(device, resource, i))
         return false;
   }

   return true;
}

static bool
D3D9NotifyLockCoversFullMipChain(const D3D9Resource *resource,
                                 const D3DDDIARG_LOCK *data)
{
   return resource && data && data->Flags.NotifyOnly &&
      resource->pool == D3DDDIPOOL_SYSTEMMEM &&
      resource->flags.Texture && !resource->flags.CubeMap &&
      !resource->flags.Volume && resource->mip_levels > 1 &&
      !data->Flags.RangeValid && !data->Flags.AreaValid &&
      !data->Flags.BoxValid;
}

static UINT
D3D9ResolveNotifyLockSubResource(D3D9Resource *resource,
                                 const D3DDDIARG_LOCK *data)
{
   if (!resource || !data || !data->Flags.NotifyOnly ||
       data->Flags.ReadOnly || data->SubResourceIndex != 0 ||
       data->Flags.RangeValid || data->Flags.AreaValid ||
       data->Flags.BoxValid || resource->pool != D3DDDIPOOL_SYSTEMMEM ||
       !resource->flags.Texture || resource->mip_levels <= 1)
      return data ? data->SubResourceIndex : 0;

   /* The D3D9 runtime reports full-surface managed mip locks as subresource 0. */
   UINT subresource_index = resource->notify_lock_mip_cursor;
   if (subresource_index >= resource->mip_levels)
      subresource_index = 0;

   resource->notify_lock_mip_cursor = subresource_index + 1;
   if (resource->notify_lock_mip_cursor >= resource->mip_levels)
      resource->notify_lock_mip_cursor = 0;

   resource->notify_lock_subresource = subresource_index;
   resource->notify_lock_subresource_valid = true;
   return subresource_index;
}

static UINT
D3D9ResolveNotifyUnlockSubResource(D3D9Resource *resource,
                                   const D3DDDIARG_UNLOCK *data)
{
   if (!resource || !data || !resource->notify_lock_subresource_valid)
      return data ? data->SubResourceIndex : 0;

   resource->notify_lock_subresource_valid = false;
   return resource->notify_lock_subresource;
}

static RECT
D3D9FullRect(const D3D9SubResource *sub)
{
   RECT rect;

   rect.left = 0;
   rect.top = 0;
   rect.right = sub ? (LONG)sub->width : 0;
   rect.bottom = sub ? (LONG)sub->height : 0;
   return rect;
}

static RECT
D3D9ClampRect(const D3D9SubResource *sub, const RECT *rect)
{
   RECT clamped = rect ? *rect : D3D9FullRect(sub);

   if (!sub)
      return clamped;
   if (clamped.right <= clamped.left || clamped.bottom <= clamped.top)
      clamped = D3D9FullRect(sub);

   if (clamped.left < 0)
      clamped.left = 0;
   if (clamped.top < 0)
      clamped.top = 0;
   if (clamped.right > (LONG)sub->width)
      clamped.right = (LONG)sub->width;
   if (clamped.bottom > (LONG)sub->height)
      clamped.bottom = (LONG)sub->height;
   return clamped;
}

static bool
D3D9IntersectRect(RECT *rect, const RECT *bounds)
{
   if (!rect || !bounds || rect->right <= rect->left ||
       rect->bottom <= rect->top || bounds->right <= bounds->left ||
       bounds->bottom <= bounds->top)
      return false;

   if (rect->left < bounds->left)
      rect->left = bounds->left;
   if (rect->top < bounds->top)
      rect->top = bounds->top;
   if (rect->right > bounds->right)
      rect->right = bounds->right;
   if (rect->bottom > bounds->bottom)
      rect->bottom = bounds->bottom;

   return rect->right > rect->left && rect->bottom > rect->top;
}

static RECT
D3D9ClipRect(const D3D9SubResource *sub, const RECT *rect)
{
   RECT clipped = rect ? *rect : D3D9FullRect(sub);
   const RECT bounds = D3D9FullRect(sub);

   if (!D3D9IntersectRect(&clipped, &bounds))
      clipped.left = clipped.top = clipped.right = clipped.bottom = 0;
   return clipped;
}

static bool
D3D9ClearRectForSubResource(D3D9Device *device, const D3D9SubResource *sub,
                            const RECT *rect, RECT *clear_rect)
{
   if (!sub || !clear_rect)
      return false;

   *clear_rect = D3D9ClipRect(sub, rect);
   if (clear_rect->right <= clear_rect->left ||
       clear_rect->bottom <= clear_rect->top)
      return false;

   if (device->viewport.Width && device->viewport.Height) {
      RECT viewport;
      viewport.left = (LONG)device->viewport.X;
      viewport.top = (LONG)device->viewport.Y;
      viewport.right = viewport.left + (LONG)device->viewport.Width;
      viewport.bottom = viewport.top + (LONG)device->viewport.Height;
      if (!D3D9IntersectRect(clear_rect, &viewport))
         return false;
   }

   if (device->render_states[D3DDDIRS_SCISSORTESTENABLE] &&
       !D3D9IntersectRect(clear_rect, &device->scissor_rect))
      return false;

   return true;
}

static void
D3D9InitPipeSurface(D3D9Resource *resource, UINT subresource_index,
                    struct pipe_surface *surface)
{
   memset(surface, 0, sizeof(*surface));
   pipe_surface_init(NULL, surface, resource->pipe_resource,
                     D3D9PipeMipLevel(resource, subresource_index), 0);
   surface->first_layer = D3D9PipeLayer(resource, subresource_index);
   surface->last_layer = surface->first_layer;
}

static void
D3D9ReleasePipeSurface(struct pipe_surface *surface)
{
   if (!surface)
      return;

   pipe_resource_reference(&surface->texture, NULL);
}

static void
D3D9PipeBoxFromRect(const RECT *rect, struct pipe_box *box)
{
   memset(box, 0, sizeof(*box));
   box->x = rect->left;
   box->y = rect->top;
   box->z = 0;
   box->width = rect->right - rect->left;
   box->height = rect->bottom - rect->top;
   box->depth = 1;
}

static void
D3D9PipeBoxFromD3DBox(const D3DDDIBOX *src, struct pipe_box *box)
{
   memset(box, 0, sizeof(*box));
   box->x = src->Left;
   box->y = src->Top;
   box->z = src->Front;
   box->width = src->Right - src->Left;
   box->height = src->Bottom - src->Top;
   box->depth = src->Back - src->Front;
}

static bool
D3D9TextureSubdata(D3D9Device *device, D3D9Resource *resource,
                   UINT subresource_index, unsigned usage,
                   const struct pipe_box *box, const void *data,
                   unsigned stride, uintptr_t layer_stride)
{
   if (!device || !device->pipe || !device->pipe->texture_subdata ||
       !resource || !resource->pipe_resource || !box || !data ||
       resource->pipe_resource->target == PIPE_BUFFER)
      return false;

   const UINT row_bytes = D3D9FormatRowPitch(resource->format, box->width);
   const UINT rows = D3D9FormatRowCount(resource->format, box->height);
   const uintptr_t tight_layer_stride = (uintptr_t)row_bytes * rows;
   const UINT depth = box->depth ? (UINT)box->depth : 1;
   if (!row_bytes || !rows || !tight_layer_stride)
      return false;

   if (stride < row_bytes || layer_stride < tight_layer_stride)
      return false;

   const void *upload_data = data;
   unsigned upload_stride = stride;
   uintptr_t upload_layer_stride = layer_stride;
   uint8_t *tight = NULL;

   if (stride != row_bytes || layer_stride != tight_layer_stride) {
      if (depth > SIZE_MAX / tight_layer_stride)
         return false;
      tight = (uint8_t *)malloc((size_t)tight_layer_stride * depth);
      if (!tight)
         return false;

      const uint8_t *src = (const uint8_t *)data;
      for (UINT z = 0; z < depth; ++z) {
         const uint8_t *src_slice = src + (size_t)z * layer_stride;
         uint8_t *dst_slice = tight + (size_t)z * tight_layer_stride;
         for (UINT y = 0; y < rows; ++y)
            memcpy(dst_slice + (size_t)y * row_bytes,
                   src_slice + (size_t)y * stride, row_bytes);
      }

      upload_data = tight;
      upload_stride = row_bytes;
      upload_layer_stride = tight_layer_stride;
   }

   device->pipe->texture_subdata(device->pipe, resource->pipe_resource,
                                 D3D9PipeMipLevel(resource,
                                                  subresource_index),
                                 usage, box, upload_data, upload_stride,
                                 upload_layer_stride);
   free(tight);
   return true;
}

static void
D3D9ColorToPipe(D3DCOLOR color, union pipe_color_union *pipe_color,
                bool srgb_write)
{
   const float red = ((color >> 16) & 0xff) / 255.0f;
   const float green = ((color >> 8) & 0xff) / 255.0f;
   const float blue = (color & 0xff) / 255.0f;

   if (srgb_write) {
      pipe_color->f[0] = util_format_linear_float_to_srgb_8unorm(red) /
         255.0f;
      pipe_color->f[1] = util_format_linear_float_to_srgb_8unorm(green) /
         255.0f;
      pipe_color->f[2] = util_format_linear_float_to_srgb_8unorm(blue) /
         255.0f;
   } else {
      pipe_color->f[0] = red;
      pipe_color->f[1] = green;
      pipe_color->f[2] = blue;
   }
   pipe_color->f[3] = ((color >> 24) & 0xff) / 255.0f;
}

static bool
D3D9IsYttriumScreen(struct pipe_screen *screen)
{
   if (!screen || !screen->get_name)
      return false;

   const char *name = screen->get_name(screen);
   return name && strcmp(name, "yttrium") == 0;
}

static bool
D3D9ClearColorRectUpload(struct pipe_context *pipe,
                         struct pipe_surface *surface,
                         const union pipe_color_union *clear_color,
                         unsigned x, unsigned y,
                         unsigned width, unsigned height)
{
   if (!pipe || !pipe->texture_subdata || !surface || !surface->texture ||
       !clear_color || !width || !height)
      return false;

   const enum pipe_format format = surface->format;
   const struct util_format_pack_description *pack =
      util_format_pack_description(format);
   if (!pack)
      return false;

   const bool pure_uint = util_format_is_pure_uint(format);
   const bool pure_sint = util_format_is_pure_sint(format);
   if ((pure_uint && !pack->pack_rgba_uint) ||
       (pure_sint && !pack->pack_rgba_sint) ||
       (!pure_uint && !pure_sint && !pack->pack_rgba_float))
      return false;

   const unsigned stride = util_format_get_stride(format, width);
   const uintptr_t layer_stride =
      (uintptr_t)util_format_get_2d_size(format, stride, height);
   const unsigned first_layer = surface->first_layer;
   const unsigned layer_count =
      surface->last_layer >= surface->first_layer ?
      surface->last_layer - surface->first_layer + 1 : 1;

   if (!stride || !layer_stride || layer_stride > SIZE_MAX / layer_count)
      return false;

   uint8_t *data = (uint8_t *)malloc((size_t)layer_stride * layer_count);
   if (!data)
      return false;

   uint8_t *row = (uint8_t *)malloc(stride);
   void *src = NULL;
   if (!row)
      goto fail;

   if (pure_uint) {
      uint32_t *rgba = (uint32_t *)malloc(width * 4 * sizeof(*rgba));
      if (!rgba)
         goto fail;
      for (unsigned i = 0; i < width; ++i) {
         rgba[i * 4 + 0] = clear_color->ui[0];
         rgba[i * 4 + 1] = clear_color->ui[1];
         rgba[i * 4 + 2] = clear_color->ui[2];
         rgba[i * 4 + 3] = clear_color->ui[3];
      }
      pack->pack_rgba_uint(row, 0, rgba, 0, width, 1);
      src = rgba;
   } else if (pure_sint) {
      int32_t *rgba = (int32_t *)malloc(width * 4 * sizeof(*rgba));
      if (!rgba)
         goto fail;
      for (unsigned i = 0; i < width; ++i) {
         rgba[i * 4 + 0] = clear_color->i[0];
         rgba[i * 4 + 1] = clear_color->i[1];
         rgba[i * 4 + 2] = clear_color->i[2];
         rgba[i * 4 + 3] = clear_color->i[3];
      }
      pack->pack_rgba_sint(row, 0, rgba, 0, width, 1);
      src = rgba;
   } else {
      float *rgba = (float *)malloc(width * 4 * sizeof(*rgba));
      if (!rgba)
         goto fail;
      for (unsigned i = 0; i < width; ++i) {
         rgba[i * 4 + 0] = clear_color->f[0];
         rgba[i * 4 + 1] = clear_color->f[1];
         rgba[i * 4 + 2] = clear_color->f[2];
         rgba[i * 4 + 3] = clear_color->f[3];
      }
      pack->pack_rgba_float(row, 0, rgba, 0, width, 1);
      src = rgba;
   }

   for (unsigned layer = 0; layer < layer_count; ++layer) {
      uint8_t *layer_data = data + (size_t)layer_stride * layer;
      for (unsigned row_index = 0; row_index < height; ++row_index)
         memcpy(layer_data + (size_t)stride * row_index, row, stride);
   }

   struct pipe_box box;
   box.x = x;
   box.y = y;
   box.z = first_layer;
   box.width = width;
   box.height = height;
   box.depth = layer_count;
   pipe->texture_subdata(pipe, surface->texture, surface->level, 0, &box,
                         data, stride, layer_stride);

   free(src);
   free(row);
   free(data);
   return true;

fail:
   free(src);
   free(row);
   free(data);
   return false;
}

static unsigned
D3D9PipeMapUsage(D3DDDI_LOCKFLAGS flags)
{
   unsigned usage = 0;

   if (flags.ReadOnly)
      usage |= PIPE_MAP_READ;
   else if (flags.WriteOnly)
      usage |= PIPE_MAP_WRITE;
   else
      usage |= PIPE_MAP_READ | PIPE_MAP_WRITE;

   if (flags.Discard)
      usage |= PIPE_MAP_DISCARD_RANGE;
   if (flags.NoOverwrite)
      usage |= PIPE_MAP_UNSYNCHRONIZED;

   return usage;
}

static bool
D3D9ClearColorPipe(D3D9Device *device, HANDLE resource_handle,
                   UINT subresource_index, const RECT *rect,
                   D3DCOLOR color, bool srgb_write, bool prefer_upload)
{
   if (!device || !device->pipe || !device->pipe->clear_render_target)
      return false;

   D3D9Resource *resource = D3D9CastResource(resource_handle);
   D3D9SubResource *sub = D3D9GetSubResource(resource_handle,
                                             subresource_index);
   if (!resource || !resource->pipe_resource || !sub)
      return false;

   const RECT dst = D3D9ClipRect(sub, rect);
   if (dst.right <= dst.left || dst.bottom <= dst.top)
      return true;

   struct pipe_surface surface;
   union pipe_color_union clear_color;

   D3D9InitPipeSurface(resource, subresource_index, &surface);
   D3D9ColorToPipe(color, &clear_color, srgb_write);
   const unsigned width = (unsigned)(dst.right - dst.left);
   const unsigned height = (unsigned)(dst.bottom - dst.top);
   const bool full_clear = dst.left == 0 && dst.top == 0 &&
      width == sub->width && height == sub->height;
   const bool yttrium = D3D9IsYttriumScreen(device->pipe->screen);
   const bool upload_full_clear = prefer_upload && yttrium &&
      !(resource->pipe_resource->bind & PIPE_BIND_DISPLAY_TARGET);
   const bool needs_upload = !full_clear ||
      upload_full_clear ||
      (yttrium && D3D9PipeMipLevel(resource, subresource_index) != 0);

   if (needs_upload &&
       D3D9ClearColorRectUpload(device->pipe, &surface, &clear_color,
                                (unsigned)dst.left, (unsigned)dst.top,
                                width, height)) {
      D3D9ReleasePipeSurface(&surface);
      sub->cpu_dirty = false;
      D3D9MarkAutogenMipmapsDirty(resource, subresource_index);
      return true;
   }

   if (needs_upload && yttrium) {
      static volatile LONG logged;
      D3D9WarnOncef(&logged, "unsupported color clear upload format=%u "
                    "sub=%u rect=%ld,%ld-%ld,%ld\n",
                    surface.format, subresource_index, dst.left, dst.top,
                    dst.right, dst.bottom);
      D3D9ReleasePipeSurface(&surface);
      return false;
   }

   device->pipe->clear_render_target(device->pipe, &surface, &clear_color,
                                     (unsigned)dst.left, (unsigned)dst.top,
                                     width, height, true);
   D3D9ReleasePipeSurface(&surface);
   sub->cpu_dirty = false;
   D3D9MarkAutogenMipmapsDirty(resource, subresource_index);
   return true;
}

static bool
D3D9ClearDepthPipe(D3D9Device *device, HANDLE resource_handle,
                   UINT subresource_index, const RECT *rect,
                   bool clear_depth, float depth,
                   bool clear_stencil, UINT stencil)
{
   if (!device || !device->pipe || !device->pipe->clear_depth_stencil)
      return false;

   D3D9Resource *resource = D3D9CastResource(resource_handle);
   D3D9SubResource *sub = D3D9GetSubResource(resource_handle,
                                             subresource_index);
   if (!resource || !resource->pipe_resource || !sub)
      return false;

   const RECT dst = D3D9ClipRect(sub, rect);
   if (dst.right <= dst.left || dst.bottom <= dst.top)
      return true;

   struct pipe_surface surface;
   unsigned clear_flags = 0;

   if (clear_depth)
      clear_flags |= PIPE_CLEAR_DEPTH;
   if (clear_stencil)
      clear_flags |= PIPE_CLEAR_STENCIL;

   D3D9InitPipeSurface(resource, subresource_index, &surface);

   const unsigned width = (unsigned)(dst.right - dst.left);
   const unsigned height = (unsigned)(dst.bottom - dst.top);
   device->pipe->clear_depth_stencil(device->pipe, &surface, clear_flags,
                                     depth, stencil,
                                     (unsigned)dst.left, (unsigned)dst.top,
                                     width, height, true);
   D3D9ReleasePipeSurface(&surface);
   sub->cpu_dirty = false;
   return true;
}

static HRESULT
D3D9CopyBuffer(D3D9Device *device, HANDLE dst_resource_handle,
               HANDLE src_resource_handle, UINT dst_offset,
               UINT src_offset, UINT size)
{
   D3D9Resource *dst_resource = D3D9CastResource(dst_resource_handle);
   D3D9Resource *src_resource = D3D9CastResource(src_resource_handle);
   if (!device || !dst_resource || !src_resource ||
       !dst_resource->surf_count || !src_resource->surf_count)
      return E_INVALIDARG;

   D3D9SubResource *dst = &dst_resource->surfaces[0];
   D3D9SubResource *src = &src_resource->surfaces[0];
   if (dst_offset > dst->size || src_offset > src->size ||
       size > dst->size - dst_offset || size > src->size - src_offset)
      return E_INVALIDARG;
   if (!size)
      return S_OK;

   if (dst_resource->pipe_resource && src_resource->pipe_resource &&
       dst_resource->pipe_resource->target == PIPE_BUFFER &&
       src_resource->pipe_resource->target == PIPE_BUFFER &&
       device->pipe && device->pipe->resource_copy_region) {
      if (src->cpu_dirty &&
          !D3D9UploadSubResource(device, src_resource, 0))
         return E_NOTIMPL;

      struct pipe_box box;
      memset(&box, 0, sizeof(box));
      box.x = src_offset;
      box.width = size;
      box.height = 1;
      box.depth = 1;
      device->pipe->resource_copy_region(device->pipe,
                                         dst_resource->pipe_resource, 0,
                                         dst_offset, 0, 0,
                                         src_resource->pipe_resource, 0,
                                         &box);
      dst->cpu_dirty = false;
      return S_OK;
   }

   D3D9Warnf("unsupported buffer blit without pipe copy dst=%p src=%p\n",
             dst_resource, src_resource);
   return E_NOTIMPL;
}

static bool
D3D9CopyCpuSourceToTexture(D3D9Device *device, D3D9Resource *dst_resource,
                           D3D9SubResource *dst_sub,
                           UINT dst_subresource_index,
                           const RECT *dst_rect,
                           D3D9Resource *src_resource,
                           D3D9SubResource *src_sub,
                           const RECT *src_rect)
{
   if (!device || !device->pipe || !device->pipe->texture_subdata ||
       !dst_resource || !dst_sub || !src_resource || !src_sub ||
       !dst_resource->pipe_resource || !src_sub->data ||
       dst_resource->pipe_resource->target == PIPE_BUFFER ||
       dst_resource->format != src_resource->format)
      return false;

   if (src_resource->pipe_resource && !src_sub->cpu_dirty &&
       D3D9PoolNeedsReadback(src_resource->pool))
      return false;

   const UINT bpp = D3D9FormatBytesPerPixel(src_resource->format);
   if (!bpp)
      return false;

   const RECT dst = D3D9ClampRect(dst_sub, dst_rect);
   const RECT src = D3D9ClampRect(src_sub, src_rect);
   const LONG dst_width = dst.right - dst.left;
   const LONG dst_height = dst.bottom - dst.top;
   const LONG src_width = src.right - src.left;
   const LONG src_height = src.bottom - src.top;
   if (dst_width <= 0 || dst_height <= 0 ||
       src_width <= 0 || src_height <= 0)
      return true;

   /* texture_subdata is a direct upload, not a stretch operation.  Let the
    * upload-plus-blit path below handle differently sized rectangles instead
    * of silently cropping them to the smaller extent.
    */
   if (dst_width != src_width || dst_height != src_height)
      return false;

   struct pipe_box box;
   memset(&box, 0, sizeof(box));
   box.x = dst.left;
   box.y = dst.top;
   box.z = D3D9PipeLayer(dst_resource, dst_subresource_index);
   box.width = dst_width;
   box.height = dst_height;
   box.depth = 1;

   const uint8_t *src_data = src_sub->data +
      (size_t)src.top * src_sub->pitch + (size_t)src.left * bpp;
   if (!D3D9TextureSubdata(device, dst_resource, dst_subresource_index,
                           PIPE_MAP_WRITE | PIPE_MAP_DISCARD_RANGE,
                           &box, src_data, src_sub->pitch,
                           src_sub->slice_pitch))
      return false;
   dst_sub->cpu_dirty = false;
   D3D9MarkAutogenMipmapsDirty(dst_resource, dst_subresource_index);
   return true;
}

static void
D3D9InitBlitInfo(struct pipe_blit_info *info,
                 struct pipe_resource *dst_resource, UINT dst_level,
                 UINT dst_layer,
                 enum pipe_format dst_format, const RECT *dst_rect,
                 struct pipe_resource *src_resource, UINT src_level,
                 UINT src_layer,
                 enum pipe_format src_format, const RECT *src_rect)
{
   memset(info, 0, sizeof(*info));

   info->dst.resource = dst_resource;
   info->dst.level = dst_level;
   info->dst.format = dst_format;
   info->dst.box.x = dst_rect->left;
   info->dst.box.y = dst_rect->top;
   info->dst.box.z = dst_layer;
   info->dst.box.width = dst_rect->right - dst_rect->left;
   info->dst.box.height = dst_rect->bottom - dst_rect->top;
   info->dst.box.depth = 1;
   info->src.resource = src_resource;
   info->src.level = src_level;
   info->src.format = src_format;
   info->src.box.x = src_rect->left;
   info->src.box.y = src_rect->top;
   info->src.box.z = src_layer;
   info->src.box.width = src_rect->right - src_rect->left;
   info->src.box.height = src_rect->bottom - src_rect->top;
   info->src.box.depth = 1;
   info->mask = util_format_get_mask(dst_format) &
                util_format_get_mask(src_format);
   /* The renderer path rejects combined Z/S multisample resolves.  Resolve
    * depth here; app-visible D24S8 MSAA StretchRect support is not exposed.
    */
   if ((info->mask & (PIPE_MASK_Z | PIPE_MASK_S)) ==
       (PIPE_MASK_Z | PIPE_MASK_S) &&
       (src_resource->nr_samples > 1 || dst_resource->nr_samples > 1))
      info->mask = PIPE_MASK_Z;
   info->filter = PIPE_TEX_FILTER_NEAREST;
}

static bool
D3D9BlitCpuOnlySourceToTexture(D3D9Device *device,
                               D3D9Resource *dst_resource,
                               D3D9SubResource *dst_sub,
                               UINT dst_subresource_index,
                               const RECT *dst_rect,
                               D3D9Resource *src_resource,
                               D3D9SubResource *src_sub,
                               const RECT *src_rect,
                               enum pipe_tex_filter filter)
{
   if (!device || !device->screen || !device->pipe ||
       !device->pipe->texture_subdata || !device->pipe->blit ||
       !dst_resource || !dst_sub || !dst_resource->pipe_resource ||
       dst_resource->pipe_resource->target == PIPE_BUFFER ||
       !src_resource || src_resource->pipe_resource || !src_sub ||
       !src_sub->data)
      return false;

   const RECT dst = D3D9ClampRect(dst_sub, dst_rect);
   const RECT src = D3D9ClampRect(src_sub, src_rect);
   const LONG dst_width = dst.right - dst.left;
   const LONG dst_height = dst.bottom - dst.top;
   const LONG src_width = src.right - src.left;
   const LONG src_height = src.bottom - src.top;
   if (dst_width <= 0 || dst_height <= 0 ||
       src_width <= 0 || src_height <= 0)
      return true;

   const enum pipe_format src_format =
      D3D9FormatToPipe(src_resource->format);
   const UINT bpp = D3D9FormatBytesPerPixel(src_resource->format);
   const enum pipe_format dst_format =
      dst_resource->pipe_resource->format;
   if (src_format == PIPE_FORMAT_NONE || !bpp ||
       D3D9FormatIsBlockCompressed(src_resource->format) ||
       src_resource->format != dst_resource->format ||
       src_format != dst_format ||
       util_format_is_depth_or_stencil(src_format) ||
       util_format_is_depth_or_stencil(dst_format) ||
       dst_resource->pipe_resource->nr_samples > 1)
      return false;

   const size_t max_size = ~(size_t)0;
   const size_t width = (size_t)src_width;
   const size_t height = (size_t)src_height;
   const size_t bytes_per_pixel = (size_t)bpp;
   const size_t pitch = (size_t)src_sub->pitch;
   if (width > max_size / bytes_per_pixel)
      return false;

   const size_t row_bytes = width * bytes_per_pixel;
   if (!pitch || row_bytes > pitch ||
       (size_t)src.top > max_size / pitch)
      return false;

   size_t data_offset = (size_t)src.top * pitch;
   if ((size_t)src.left > (max_size - data_offset) / bytes_per_pixel)
      return false;
   data_offset += (size_t)src.left * bytes_per_pixel;

   if (data_offset > max_size - row_bytes)
      return false;
   const size_t remaining = max_size - data_offset - row_bytes;
   if (height - 1 > remaining / pitch)
      return false;

   const size_t required = data_offset + (height - 1) * pitch + row_bytes;
   if (required > src_sub->data_capacity)
      return false;

   struct pipe_resource templ;
   memset(&templ, 0, sizeof(templ));
   templ.target = PIPE_TEXTURE_2D;
   templ.format = src_format;
   templ.width0 = src_width;
   templ.height0 = src_height;
   templ.depth0 = 1;
   templ.array_size = 1;
   templ.last_level = 0;
   templ.nr_samples = 1;
   templ.nr_storage_samples = 1;
   templ.bind = PIPE_BIND_SAMPLER_VIEW;
   templ.usage = PIPE_USAGE_DEFAULT;

   struct pipe_resource *upload =
      device->screen->resource_create(device->screen, &templ);
   if (!upload)
      return false;

   struct pipe_box upload_box;
   memset(&upload_box, 0, sizeof(upload_box));
   upload_box.width = src_width;
   upload_box.height = src_height;
   upload_box.depth = 1;
   D3D9Resource upload_resource;
   memset(&upload_resource, 0, sizeof(upload_resource));
   upload_resource.format = src_resource->format;
   upload_resource.mip_levels = 1;
   upload_resource.pipe_resource = upload;
   const uint8_t *src_data = src_sub->data + data_offset;
   if (!D3D9TextureSubdata(device, &upload_resource, 0,
                           PIPE_MAP_WRITE |
                              PIPE_MAP_DISCARD_WHOLE_RESOURCE,
                           &upload_box, src_data, src_sub->pitch,
                           src_sub->slice_pitch)) {
      pipe_resource_reference(&upload, NULL);
      return false;
   }

   RECT upload_rect;
   upload_rect.left = 0;
   upload_rect.top = 0;
   upload_rect.right = src_width;
   upload_rect.bottom = src_height;
   struct pipe_blit_info info;
   D3D9InitBlitInfo(&info, dst_resource->pipe_resource,
                    D3D9PipeMipLevel(dst_resource,
                                     dst_subresource_index),
                    D3D9PipeLayer(dst_resource,
                                  dst_subresource_index),
                    dst_format, &dst,
                    upload, 0, 0, src_format, &upload_rect);
   info.filter = filter;
   device->pipe->blit(device->pipe, &info);
   pipe_resource_reference(&upload, NULL);

   dst_sub->cpu_dirty = false;
   D3D9MarkAutogenMipmapsDirty(dst_resource, dst_subresource_index);
   return true;
}

static bool
D3D9ResolveThenConvertPipe(D3D9Device *device, D3D9Resource *dst_resource,
                           D3D9SubResource *dst_sub,
                           UINT dst_subresource_index, const RECT *dst,
                           D3D9Resource *src_resource,
                           UINT src_subresource_index, const RECT *src)
{
   if (!device || !device->screen || !device->pipe || !device->pipe->blit ||
       !dst_resource || !dst_sub || !src_resource ||
       !dst_resource->pipe_resource || !src_resource->pipe_resource)
      return false;

   struct pipe_resource *dst_pipe = dst_resource->pipe_resource;
   struct pipe_resource *src_pipe = src_resource->pipe_resource;
   if (src_pipe->nr_samples <= 1 || dst_pipe->nr_samples != 1 ||
       src_pipe->format == dst_pipe->format ||
       src_pipe->target == PIPE_BUFFER || dst_pipe->target == PIPE_BUFFER)
      return false;

   const LONG dst_width = dst->right - dst->left;
   const LONG dst_height = dst->bottom - dst->top;
   const LONG src_width = src->right - src->left;
   const LONG src_height = src->bottom - src->top;
   if (dst_width <= 0 || dst_height <= 0 ||
       dst_width != src_width || dst_height != src_height)
      return false;

   struct pipe_resource templ;
   memset(&templ, 0, sizeof(templ));
   templ.target = PIPE_TEXTURE_2D;
   templ.format = src_pipe->format;
   templ.width0 = src_width;
   templ.height0 = src_height;
   templ.depth0 = 1;
   templ.array_size = 1;
   templ.last_level = 0;
   templ.nr_samples = 1;
   templ.nr_storage_samples = 1;
   templ.bind = PIPE_BIND_RENDER_TARGET | PIPE_BIND_SAMPLER_VIEW;
   templ.usage = PIPE_USAGE_DEFAULT;

   struct pipe_resource *resolved =
      device->screen->resource_create(device->screen, &templ);
   if (!resolved)
      return false;

   RECT resolved_rect;
   resolved_rect.left = 0;
   resolved_rect.top = 0;
   resolved_rect.right = src_width;
   resolved_rect.bottom = src_height;

   struct pipe_blit_info info;
   D3D9InitBlitInfo(&info, resolved, 0, 0, resolved->format, &resolved_rect,
                    src_pipe, D3D9PipeMipLevel(src_resource,
                                               src_subresource_index),
                    D3D9PipeLayer(src_resource, src_subresource_index),
                    src_pipe->format, src);
   device->pipe->blit(device->pipe, &info);

   D3D9InitBlitInfo(&info, dst_pipe,
                    D3D9PipeMipLevel(dst_resource, dst_subresource_index),
                    D3D9PipeLayer(dst_resource, dst_subresource_index),
                    dst_pipe->format, dst, resolved, 0, 0,
                    resolved->format, &resolved_rect);
   device->pipe->blit(device->pipe, &info);

   pipe_resource_reference(&resolved, NULL);
   dst_sub->cpu_dirty = false;
   D3D9MarkAutogenMipmapsDirty(dst_resource, dst_subresource_index);
   return true;
}

static bool
D3D9CopyRectPipe(D3D9Device *device, HANDLE dst_resource_handle,
                 UINT dst_subresource_index, const RECT *dst_rect,
                 HANDLE src_resource_handle, UINT src_subresource_index,
                 const RECT *src_rect, enum pipe_tex_filter filter)
{
   if (!device || !device->pipe || !device->pipe->resource_copy_region)
      return false;

   D3D9Resource *dst_resource = D3D9CastResource(dst_resource_handle);
   D3D9Resource *src_resource = D3D9CastResource(src_resource_handle);
   D3D9SubResource *dst_sub = D3D9GetSubResource(dst_resource_handle,
                                                 dst_subresource_index);
   D3D9SubResource *src_sub = D3D9GetSubResource(src_resource_handle,
                                                 src_subresource_index);
   if (!dst_resource || !src_resource || !src_sub || !dst_sub)
      return false;

   if (D3D9CopyTextureToCpuDestination(device, dst_resource, dst_sub,
                                       dst_subresource_index, dst_rect,
                                       src_resource, src_sub,
                                       src_subresource_index, src_rect))
      return true;

   if (D3D9CopyCpuSourceToTexture(device, dst_resource, dst_sub,
                                  dst_subresource_index, dst_rect,
                                  src_resource, src_sub, src_rect))
      return true;

   if (D3D9BlitCpuOnlySourceToTexture(device, dst_resource, dst_sub,
                                      dst_subresource_index, dst_rect,
                                      src_resource, src_sub, src_rect,
                                      filter))
      return true;

   if (!dst_resource->pipe_resource || !src_resource->pipe_resource)
      return false;

   if (src_sub->cpu_dirty &&
       !D3D9UploadSubResource(device, src_resource, src_subresource_index))
      return false;

   const RECT dst = D3D9ClampRect(dst_sub, dst_rect);
   const RECT src = D3D9ClampRect(src_sub, src_rect);
   const LONG dst_width = dst.right - dst.left;
   const LONG dst_height = dst.bottom - dst.top;
   const LONG src_width = src.right - src.left;
   const LONG src_height = src.bottom - src.top;
   if (dst_width <= 0 || dst_height <= 0 || src_width <= 0 ||
       src_height <= 0)
      return true;

   if (src_resource == device->last_systemmem_texture &&
       dst_resource->managed_upload_complete &&
       D3D9ManagedUploadSourceClean(src_resource, dst_resource))
      return true;

   if (D3D9ResolveThenConvertPipe(device, dst_resource, dst_sub,
                                  dst_subresource_index, &dst,
                                  src_resource, src_subresource_index, &src))
      return true;

   const bool dst_depth_or_stencil =
      util_format_is_depth_or_stencil(dst_resource->pipe_resource->format);
   const bool src_depth_or_stencil =
      util_format_is_depth_or_stencil(src_resource->pipe_resource->format);
   if (dst_depth_or_stencil != src_depth_or_stencil)
      return false;

   if ((dst_depth_or_stencil ||
        dst_width != src_width || dst_height != src_height ||
        dst_resource->pipe_resource->format !=
        src_resource->pipe_resource->format ||
        dst_resource->pipe_resource->nr_samples !=
        src_resource->pipe_resource->nr_samples) &&
       device->pipe->blit) {
      struct pipe_blit_info info;
      D3D9InitBlitInfo(&info, dst_resource->pipe_resource,
                       D3D9PipeMipLevel(dst_resource, dst_subresource_index),
                       D3D9PipeLayer(dst_resource, dst_subresource_index),
                       dst_resource->pipe_resource->format, &dst,
                       src_resource->pipe_resource,
                       D3D9PipeMipLevel(src_resource, src_subresource_index),
                       D3D9PipeLayer(src_resource, src_subresource_index),
                       src_resource->pipe_resource->format, &src);
      info.filter = filter;
      device->pipe->blit(device->pipe, &info);
      dst_sub->cpu_dirty = false;
      D3D9MarkAutogenMipmapsDirty(dst_resource, dst_subresource_index);
      return true;
   }

   const LONG width = dst.right - dst.left < src.right - src.left ?
      dst.right - dst.left : src.right - src.left;
   const LONG height = dst.bottom - dst.top < src.bottom - src.top ?
      dst.bottom - dst.top : src.bottom - src.top;
   if (width <= 0 || height <= 0)
      return true;

   struct pipe_box src_box;
   D3D9PipeBoxFromRect(&src, &src_box);
   src_box.z = D3D9PipeLayer(src_resource, src_subresource_index);
   src_box.width = width;
   src_box.height = height;

   device->pipe->resource_copy_region(device->pipe,
                                      dst_resource->pipe_resource,
                                      D3D9PipeMipLevel(dst_resource,
                                                       dst_subresource_index),
                                      (unsigned)dst.left,
                                      (unsigned)dst.top,
                                      D3D9PipeLayer(dst_resource,
                                                   dst_subresource_index),
                                      src_resource->pipe_resource,
                                      D3D9PipeMipLevel(src_resource,
                                                       src_subresource_index),
                                      &src_box);
   dst_sub->cpu_dirty = false;
   D3D9MarkAutogenMipmapsDirty(dst_resource, dst_subresource_index);
   return true;
}

static bool
D3D9CopyVolumePipe(D3D9Device *device, HANDLE dst_resource_handle,
                   HANDLE src_resource_handle, const D3DDDIBOX *src_box,
                   UINT dst_x, UINT dst_y, UINT dst_z)
{
   if (!device || !device->pipe || !device->pipe->resource_copy_region)
      return false;

   D3D9Resource *dst_resource = D3D9CastResource(dst_resource_handle);
   D3D9Resource *src_resource = D3D9CastResource(src_resource_handle);
   if (!dst_resource || !src_resource || !dst_resource->pipe_resource ||
       !src_resource->pipe_resource || !dst_resource->surf_count ||
       !src_resource->surf_count)
      return false;

   D3D9SubResource *dst = &dst_resource->surfaces[0];
   D3D9SubResource *src = &src_resource->surfaces[0];
   if (src_box->Right <= src_box->Left || src_box->Bottom <= src_box->Top ||
       src_box->Back <= src_box->Front)
      return true;

   UINT width = src_box->Right - src_box->Left;
   UINT height = src_box->Bottom - src_box->Top;
   UINT depth = src_box->Back - src_box->Front;
   if (src_box->Right > src->width || src_box->Bottom > src->height ||
       src_box->Back > src->depth || dst_x > dst->width ||
       dst_y > dst->height || dst_z > dst->depth)
      return false;
   width = MIN2(width, dst->width - dst_x);
   height = MIN2(height, dst->height - dst_y);
   depth = MIN2(depth, dst->depth - dst_z);
   if (!width || !height || !depth)
      return true;

   if (src->cpu_dirty &&
       !D3D9UploadSubResource(device, src_resource, 0))
      return false;

   struct pipe_box box;
   D3D9PipeBoxFromD3DBox(src_box, &box);
   box.width = width;
   box.height = height;
   box.depth = depth;
   device->pipe->resource_copy_region(device->pipe,
                                      dst_resource->pipe_resource, 0,
                                      dst_x, dst_y, dst_z,
                                      src_resource->pipe_resource, 0,
                                      &box);
   dst->cpu_dirty = false;
   D3D9MarkAutogenMipmapsDirty(dst_resource, 0);
   return true;
}

static struct pipe_resource *
D3D9GetReadbackResource(D3D9Device *device, D3D9Resource *resource,
                        D3D9SubResource *sub)
{
   if (sub->readback_resource)
      return sub->readback_resource;

   if (!device || !device->screen || !resource || !resource->pipe_resource)
      return NULL;

   struct pipe_resource templ;
   memset(&templ, 0, sizeof(templ));
   templ.target = resource->pipe_resource->target;
   templ.format = resource->pipe_resource->format;
   templ.width0 = sub->width;
   templ.height0 = sub->height;
   templ.depth0 = sub->depth ? sub->depth : 1;
   templ.array_size = 1;
   templ.last_level = 0;
   templ.nr_samples = 1;
   templ.nr_storage_samples = 1;
   templ.usage = PIPE_USAGE_STAGING;

   sub->readback_resource = device->screen->resource_create(device->screen,
                                                            &templ);
   D3D9Tracef("ReadbackResource resource=%p pipe=%p readback=%p "
              "format=%u size=%ux%ux%u\n",
              resource, resource->pipe_resource, sub->readback_resource,
              templ.format, templ.width0, templ.height0, templ.depth0);
   return sub->readback_resource;
}

static void *
D3D9ReadbackTextureLock(D3D9Device *device, D3D9Resource *resource,
                        D3D9SubResource *sub, UINT subresource_index,
                        unsigned usage, const struct pipe_box *box,
                        D3DDDIARG_LOCK *data)
{
   if (!device || !device->pipe || !device->pipe->resource_copy_region ||
       !device->pipe->texture_map || !resource || !resource->pipe_resource ||
       !sub || !box || !data)
      return NULL;

   struct pipe_resource *readback =
      D3D9GetReadbackResource(device, resource, sub);
   if (!readback)
      return NULL;

   struct pipe_box full_box;
   memset(&full_box, 0, sizeof(full_box));
   full_box.width = sub->width;
   full_box.height = sub->height;
   full_box.depth = sub->depth ? sub->depth : 1;

   device->pipe->resource_copy_region(device->pipe, readback, 0,
                                      0, 0, 0,
                                      resource->pipe_resource,
                                      D3D9PipeMipLevel(resource,
                                                       subresource_index),
                                      &full_box);
   yttrium_gdi_flush_labeled(device->pipe, NULL, 0,
                             "D3D9 resource readback");

   void *map = device->pipe->texture_map(device->pipe, readback, 0,
                                         usage, box, &sub->transfer);
   if (!map || !sub->transfer) {
      sub->transfer = NULL;
      return NULL;
   }

   sub->transfer_on_readback = true;
   sub->transfer_write = (usage & PIPE_MAP_WRITE) != 0;
   sub->transfer_box = *box;
   data->Pitch = sub->transfer->stride;
   data->SlicePitch = sub->transfer->layer_stride;
   D3D9Tracef("ReadbackTextureLock resource=%p sub=%u box=%d,%d,%d "
              "%dx%dx%d usage=0x%x\n",
              resource, subresource_index, box->x, box->y, box->z,
              box->width, box->height, box->depth, usage);
   return map;
}

static void
D3D9EndReadbackLock(D3D9Device *device, D3D9SubResource *sub)
{
   if (!device || !device->pipe || !sub || !sub->transfer)
      return;

   if (device->pipe->texture_unmap)
      device->pipe->texture_unmap(device->pipe, sub->transfer);
   sub->transfer = NULL;
   sub->transfer_on_readback = false;
   sub->transfer_write = false;
   memset(&sub->transfer_box, 0, sizeof(sub->transfer_box));
}

static bool
D3D9CopyTextureToCpuDestination(D3D9Device *device,
                                D3D9Resource *dst_resource,
                                D3D9SubResource *dst_sub,
                                UINT dst_subresource_index,
                                const RECT *dst_rect,
                                D3D9Resource *src_resource,
                                D3D9SubResource *src_sub,
                                UINT src_subresource_index,
                                const RECT *src_rect)
{
   if (!device || !device->pipe || !device->pipe->texture_map ||
       !device->pipe->texture_unmap || !dst_resource || !dst_sub ||
       !src_resource || !src_sub || D3D9PoolNeedsReadback(dst_resource->pool))
      return false;

   if (!src_resource->pipe_resource ||
       src_resource->pipe_resource->target == PIPE_BUFFER ||
       src_resource->format != dst_resource->format || src_sub->transfer ||
       dst_sub->transfer || dst_sub->cpu_locked)
      return false;

   if (!D3D9EnsureCpuBacking(dst_sub))
      return false;

   if (src_sub->cpu_dirty &&
       !D3D9UploadSubResource(device, src_resource, src_subresource_index))
      return false;

   const RECT dst = D3D9ClampRect(dst_sub, dst_rect);
   const RECT src = D3D9ClampRect(src_sub, src_rect);
   const LONG width = dst.right - dst.left < src.right - src.left ?
      dst.right - dst.left : src.right - src.left;
   const LONG height = dst.bottom - dst.top < src.bottom - src.top ?
      dst.bottom - dst.top : src.bottom - src.top;
   if (width <= 0 || height <= 0)
      return true;

   const UINT bpp = D3D9FormatBytesPerPixel(src_resource->format);
   if (!bpp)
      return false;

   struct pipe_box src_box;
   D3D9PipeBoxFromRect(&src, &src_box);
   src_box.width = width;
   src_box.height = height;

   D3DDDIARG_LOCK src_lock;
   memset(&src_lock, 0, sizeof(src_lock));
   void *src_map = D3D9ReadbackTextureLock(device, src_resource, src_sub,
                                           src_subresource_index,
                                           PIPE_MAP_READ, &src_box,
                                           &src_lock);
   if (!src_map)
      return false;

   const size_t row_bytes = (size_t)width * bpp;
   const unsigned src_stride = src_sub->transfer->stride;
   const unsigned dst_stride = dst_sub->pitch;
   const uint8_t *src_bytes = (const uint8_t *)src_map;
   uint8_t *dst_bytes = dst_sub->data +
      (size_t)dst.top * dst_sub->pitch + (size_t)dst.left * bpp;
   for (LONG y = 0; y < height; ++y) {
      memcpy(dst_bytes + (size_t)y * dst_stride,
             src_bytes + (size_t)y * src_stride, row_bytes);
   }

   D3D9EndReadbackLock(device, src_sub);
   dst_sub->cpu_dirty = false;
   D3D9Tracef("CopyTextureToCpuDestination dst=%p/%u src=%p/%u box=%ldx%ld\n",
              dst_resource, dst_subresource_index,
              src_resource, src_subresource_index, width, height);
   return true;
}

bool
D3D9UploadSubResource(D3D9Device *device, D3D9Resource *resource,
                      UINT subresource_index)
{
   if (!device || !device->pipe || !resource || !resource->pipe_resource ||
       subresource_index >= resource->surf_count)
      return false;

   D3D9SubResource *sub = &resource->surfaces[subresource_index];
   if (!sub->cpu_dirty || !sub->data)
      return true;

   if (resource->pipe_resource->target == PIPE_BUFFER) {
      if (!device->pipe->buffer_subdata) {
         D3D9Warnf("unsupported CPU-sourced buffer upload without pipe "
                   "buffer_subdata resource=%p sub=%u\n",
                   resource, subresource_index);
         return false;
      }
   } else if (!device->pipe->texture_subdata) {
      D3D9Warnf("unsupported CPU-sourced texture upload without pipe "
                "texture_subdata resource=%p sub=%u\n",
                resource, subresource_index);
      return false;
   }

   if (resource->pipe_resource->target != PIPE_BUFFER &&
       (!sub->width || !sub->height))
      return false;

   D3D9Tracef("UploadSubResource resource=%p sub=%u allocation=0x%lx "
              "size=%ux%ux%u\n",
              resource, subresource_index, (unsigned long)sub->allocation,
              sub->width, sub->height, sub->depth);

   struct pipe_box box;
   memset(&box, 0, sizeof(box));
   box.z = D3D9PipeLayer(resource, subresource_index);
   box.width = sub->width;
   box.height = sub->height;
   box.depth = sub->depth ? sub->depth : 1;

   if (resource->pipe_resource->target == PIPE_BUFFER &&
       device->pipe->buffer_subdata) {
      device->pipe->buffer_subdata(device->pipe, resource->pipe_resource,
                                   PIPE_MAP_WRITE | PIPE_MAP_DISCARD_RANGE,
                                   0, (unsigned)sub->size, sub->data);
   } else {
      if (!D3D9TextureSubdata(device, resource, subresource_index,
                              PIPE_MAP_WRITE | PIPE_MAP_DISCARD_RANGE,
                              &box, sub->data, sub->pitch,
                              sub->slice_pitch))
         return false;
   }
   sub->cpu_dirty = false;
   D3D9MarkAutogenMipmapsDirty(resource, subresource_index);
   if (D3D9PoolNeedsReadback(resource->pool)) {
      if (sub->owns_data)
         free(sub->data);
      sub->data = NULL;
      sub->data_capacity = 0;
      sub->owns_data = false;
   }
   return true;
}

static bool
D3D9ResourceIsTexture(const D3D9Resource *resource)
{
   return resource &&
      (resource->flags.Texture || resource->flags.CubeMap ||
       resource->flags.Volume);
}

static UINT
D3D9ManagedUploadSrcSubResource(const D3D9Resource *src,
                                const D3D9Resource *dst,
                                UINT src_base_subresource,
                                UINT dst_subresource)
{
   if (src && dst && src->flags.CubeMap && dst->flags.CubeMap &&
       src->mip_levels && dst->mip_levels) {
      const UINT face = dst_subresource / dst->mip_levels;
      const UINT mip = dst_subresource % dst->mip_levels;
      return face * src->mip_levels + src_base_subresource + mip;
   }

   return src_base_subresource + dst_subresource;
}

static bool
D3D9ResourcesMatchForManagedUpload(const D3D9Resource *src,
                                   const D3D9Resource *dst,
                                   UINT *src_base_subresource)
{
   if (src_base_subresource)
      *src_base_subresource = 0;

   if (!D3D9ResourceIsTexture(src) || !D3D9ResourceIsTexture(dst) ||
       !dst->pipe_resource || src->format != dst->format ||
       src->flags.CubeMap != dst->flags.CubeMap ||
       src->flags.Volume != dst->flags.Volume)
      return false;

   if (src->surf_count == dst->surf_count &&
       src->mip_levels == dst->mip_levels) {
      for (UINT i = 0; i < src->surf_count; ++i) {
         if (src->surfaces[i].width != dst->surfaces[i].width ||
             src->surfaces[i].height != dst->surfaces[i].height ||
             src->surfaces[i].depth != dst->surfaces[i].depth)
            return false;
      }
      return true;
   }

   if (src->flags.CubeMap && dst->flags.CubeMap &&
       src->mip_levels >= dst->mip_levels &&
       src->surf_count >= src->mip_levels * 6 &&
       dst->surf_count == dst->mip_levels * 6) {
      const UINT max_base = src->mip_levels - dst->mip_levels;
      for (UINT base = 1; base <= max_base; ++base) {
         bool match = true;
         for (UINT i = 0; i < dst->surf_count; ++i) {
            const UINT src_index =
               D3D9ManagedUploadSrcSubResource(src, dst, base, i);
            const D3D9SubResource *src_sub = &src->surfaces[src_index];
            const D3D9SubResource *dst_sub = &dst->surfaces[i];

            if (src_sub->width != dst_sub->width ||
                src_sub->height != dst_sub->height ||
                src_sub->depth != dst_sub->depth) {
               match = false;
               break;
            }
         }
         if (match) {
            if (src_base_subresource)
               *src_base_subresource = base;
            return true;
         }
      }
   }

   if (!src->flags.Texture || dst->flags.CubeMap || dst->flags.Volume ||
       !src->mip_levels || !dst->mip_levels ||
       dst->surf_count != dst->mip_levels ||
       src->surf_count < dst->surf_count)
      return false;

   for (UINT base = 1; base + dst->surf_count <= src->surf_count; ++base) {
      bool match = true;
      for (UINT i = 0; i < dst->surf_count; ++i) {
         const D3D9SubResource *src_sub = &src->surfaces[base + i];
         const D3D9SubResource *dst_sub = &dst->surfaces[i];

         if (src_sub->width != dst_sub->width ||
             src_sub->height != dst_sub->height ||
             src_sub->depth != dst_sub->depth) {
            match = false;
            break;
         }
      }
      if (match) {
         if (src_base_subresource)
            *src_base_subresource = base;
         return true;
      }
   }

   return false;
}

static bool
D3D9ResourcesMatchForVolumeUpload(const D3D9Resource *src,
                                  const D3D9Resource *dst,
                                  UINT *src_base_subresource)
{
   if (src_base_subresource)
      *src_base_subresource = 0;

   if (!src || !dst || !src->flags.Volume || !dst->flags.Volume ||
       !src->mip_levels || !dst->mip_levels ||
       !D3D9FormatsHaveSameMemoryLayout(src->format, dst->format) ||
       src->surf_count < dst->surf_count)
      return false;

   for (UINT base = 0; base + dst->surf_count <= src->surf_count; ++base) {
      bool match = true;

      for (UINT i = 0; i < dst->surf_count; ++i) {
         const D3D9SubResource *src_sub = &src->surfaces[base + i];
         const D3D9SubResource *dst_sub = &dst->surfaces[i];

         if (src_sub->width != dst_sub->width ||
             src_sub->height != dst_sub->height ||
             src_sub->depth != dst_sub->depth) {
            match = false;
            break;
         }
      }

      if (match) {
         if (src_base_subresource)
            *src_base_subresource = base;
         return true;
      }
   }

   return false;
}

static bool
D3D9ResourcesMatchForManagedBufferUpload(const D3D9Resource *src,
                                         const D3D9Resource *dst)
{
   if (!src || !dst || !src->surf_count || !dst->surf_count ||
       src->pool != D3DDDIPOOL_SYSTEMMEM ||
       dst->pool == D3DDDIPOOL_SYSTEMMEM)
      return false;

   if (src->flags.VertexBuffer != dst->flags.VertexBuffer ||
       src->flags.IndexBuffer != dst->flags.IndexBuffer ||
       !(src->flags.VertexBuffer || src->flags.IndexBuffer))
      return false;

   return src->surfaces[0].size == dst->surfaces[0].size;
}

static bool
D3D9ManagedUploadSourceClean(const D3D9Resource *src, const D3D9Resource *dst)
{
   UINT src_base_subresource = 0;
   if (!D3D9ResourcesMatchForManagedUpload(src, dst, &src_base_subresource))
      return false;

   for (UINT i = 0; i < dst->surf_count; ++i) {
      const UINT src_index =
         D3D9ManagedUploadSrcSubResource(src, dst, src_base_subresource, i);
      if (src->surfaces[src_index].cpu_dirty)
         return false;
   }

   return true;
}

static bool
D3D9UploadManagedTextureData(D3D9Device *device, D3D9Resource *dst,
                             D3D9Resource *src)
{
   if (!device || !device->pipe || !device->pipe->texture_subdata ||
       !dst || !src)
      return false;

   UINT src_base_subresource = 0;
   if (!D3D9ResourcesMatchForManagedUpload(src, dst, &src_base_subresource))
      return false;

   if (!src_base_subresource && src->surf_count == dst->surf_count &&
       src->mip_levels == dst->mip_levels) {
      if (src->managed_default_resource &&
          src->managed_default_resource != dst &&
          src->managed_default_resource->managed_source_resource == src)
         src->managed_default_resource->managed_source_resource = NULL;
      if (dst->managed_source_resource &&
          dst->managed_source_resource != src &&
          dst->managed_source_resource->managed_default_resource == dst)
         dst->managed_source_resource->managed_default_resource = NULL;
      src->managed_default_resource = dst;
      dst->managed_source_resource = src;
      pipe_resource_reference(&src->managed_default_pipe_resource,
                              dst->pipe_resource);
   } else if (src->managed_default_pipe_resource) {
      pipe_resource_reference(&dst->managed_source_pipe_resource,
                              src->managed_default_pipe_resource);
      dst->managed_source_base_level =
         D3D9PipeMipLevel(src, src_base_subresource);
   }

   bool uploaded = false;
   for (UINT i = 0; i < dst->surf_count; ++i) {
      const UINT src_index =
         D3D9ManagedUploadSrcSubResource(src, dst, src_base_subresource, i);
      D3D9SubResource *src_sub = &src->surfaces[src_index];
      D3D9SubResource *dst_sub = &dst->surfaces[i];
      if (!src_sub->data)
         continue;

      struct pipe_box box;
      memset(&box, 0, sizeof(box));
      box.z = D3D9PipeLayer(dst, i);
      box.width = dst_sub->width;
      box.height = dst_sub->height;
      box.depth = dst_sub->depth ? dst_sub->depth : 1;

      if (!D3D9TextureSubdata(device, dst, i,
                              PIPE_MAP_WRITE | PIPE_MAP_DISCARD_RANGE,
                              &box, src_sub->data, src_sub->pitch,
                              src_sub->slice_pitch))
         return false;
      dst_sub->cpu_dirty = false;
      src_sub->cpu_dirty = false;
      D3D9MarkAutogenMipmapsDirty(dst, i);
      uploaded = true;
   }

   dst->managed_upload_complete = uploaded;
   return uploaded;
}

static bool
D3D9UploadVolumeTextureData(D3D9Device *device, D3D9Resource *dst,
                            D3D9Resource *src)
{
   if (!device || !device->pipe || !device->pipe->texture_subdata ||
       !dst || !src || !dst->pipe_resource)
      return false;

   if (D3D9PoolNeedsReadback(src->pool))
      return false;

   UINT src_base_subresource = 0;
   if (!D3D9ResourcesMatchForVolumeUpload(src, dst, &src_base_subresource))
      return false;

   for (UINT i = 0; i < dst->surf_count; ++i) {
      D3D9SubResource *src_sub = &src->surfaces[src_base_subresource + i];
      D3D9SubResource *dst_sub = &dst->surfaces[i];
      if (!src_sub->data)
         return false;

      struct pipe_box box;
      memset(&box, 0, sizeof(box));
      box.width = dst_sub->width;
      box.height = dst_sub->height;
      box.depth = dst_sub->depth ? dst_sub->depth : 1;

      if (!D3D9TextureSubdata(device, dst, i,
                              PIPE_MAP_WRITE | PIPE_MAP_DISCARD_RANGE,
                              &box, src_sub->data, src_sub->pitch,
                              src_sub->slice_pitch))
         return false;

      dst_sub->cpu_dirty = false;
      src_sub->cpu_dirty = false;
      D3D9MarkAutogenMipmapsDirty(dst, i);
   }

   return true;
}

static HRESULT
D3D9FlushFrontbuffer(D3D9Device *device, D3D9Resource *src_resource,
                     UINT src_subresource_index, D3DKMT_HANDLE dst_allocation,
                     bool application_scanout)
{
   if (!device || !device->pipe || !device->screen ||
       !device->screen->flush_frontbuffer || !src_resource ||
       !src_resource->pipe_resource)
      return E_INVALIDARG;

   struct gdikmt_present_info present_info;
   memset(&present_info, 0, sizeof(present_info));
   present_info.magic = GDIKMT_PRESENT_INFO_MAGIC;
   present_info.version = 3;
   present_info.hDstAllocation = dst_allocation;
   present_info.status = S_OK;
   present_info.application_scanout = application_scanout;

   struct pipe_resource *flush_resource = src_resource->pipe_resource;
   unsigned flush_level = D3D9PipeMipLevel(src_resource,
                                           src_subresource_index);
   struct pipe_resource *resolved = NULL;

   if (flush_resource->nr_samples > 1 && device->pipe->blit) {
      D3D9SubResource *src_sub =
         D3D9GetSubResource(src_resource, src_subresource_index);
      if (!src_sub)
         return E_INVALIDARG;

      if (src_resource->primary_display_resource) {
         pipe_resource_reference(&resolved,
                                 src_resource->primary_display_resource);
      } else {
         struct pipe_resource templ;
         memset(&templ, 0, sizeof(templ));
         templ.target = PIPE_TEXTURE_2D;
         templ.format = flush_resource->format;
         templ.width0 = src_sub->width;
         templ.height0 = src_sub->height;
         templ.depth0 = 1;
         templ.array_size = 1;
         templ.last_level = 0;
         templ.nr_samples = 1;
         templ.nr_storage_samples = 1;
         templ.bind = PIPE_BIND_RENDER_TARGET | PIPE_BIND_DISPLAY_TARGET |
                      PIPE_BIND_SHARED;
         templ.usage = PIPE_USAGE_DEFAULT;

         resolved = device->screen->resource_create(device->screen, &templ);
         if (!resolved)
            return E_OUTOFMEMORY;
      }

      RECT rect = D3D9FullRect(src_sub);
      struct pipe_blit_info info;
      D3D9InitBlitInfo(&info, resolved, 0, 0, resolved->format, &rect,
                       flush_resource, flush_level,
                       D3D9PipeLayer(src_resource, src_subresource_index),
                       flush_resource->format, &rect);
      device->pipe->blit(device->pipe, &info);
      flush_resource = resolved;
      flush_level = 0;
   }

   /* The Yttrium screen hook owns its exact asynchronous publication ticket. */
   if (!D3D9IsYttriumScreen(device->screen))
      yttrium_gdi_flush_labeled(device->pipe, NULL, 0,
                                "D3D9 present pre-flush");
   device->screen->flush_frontbuffer(device->screen,
                                     device->pipe,
                                     flush_resource,
                                     flush_level,
                                     0,
                                     &present_info,
                                     0,
                                     NULL);
   pipe_resource_reference(&resolved, NULL);
   return present_info.status;
}

static size_t
D3D9ResourceDimensionSize(const D3DDDIARG_CREATERESOURCE *data,
                          const D3DDDI_SURFACEINFO *surface)
{
   const UINT depth = surface->Depth ? surface->Depth : 1;
   if (data->Flags.VertexBuffer || data->Flags.IndexBuffer)
      return surface->Width;

   const UINT pitch =
      D3D9AlignPitch(D3D9FormatRowPitch(data->Format, surface->Width));
   return (size_t)pitch * D3D9FormatRowCount(data->Format,
                                             surface->Height) * depth;
}

static UINT
D3D9ResourcePitch(const D3DDDIARG_CREATERESOURCE *data,
                  const D3DDDI_SURFACEINFO *surface)
{
   if (data->Flags.VertexBuffer || data->Flags.IndexBuffer)
      return surface->Width;
   return D3D9AlignPitch(D3D9FormatRowPitch(data->Format, surface->Width));
}

static UINT
D3D9ResourceSlicePitch(const D3DDDIARG_CREATERESOURCE *data,
                       const D3DDDI_SURFACEINFO *surface, UINT pitch)
{
   if (data->Flags.VertexBuffer || data->Flags.IndexBuffer)
      return surface->Width;
   return pitch * D3D9FormatRowCount(data->Format, surface->Height);
}

static void
D3D9FreeResource(D3D9Resource *resource)
{
   if (!resource)
      return;

   pipe_resource_reference(&resource->pipe_resource, NULL);
   pipe_resource_reference(&resource->primary_display_resource, NULL);
   pipe_resource_reference(&resource->managed_default_pipe_resource, NULL);
   pipe_resource_reference(&resource->managed_source_pipe_resource, NULL);
   for (UINT i = 0; i < resource->surf_count; ++i) {
      pipe_resource_reference(&resource->surfaces[i].readback_resource, NULL);
      pipe_resource_reference(
         &resource->surfaces[i].worker_upload_buffer, NULL);
      if (resource->surfaces[i].owns_data)
         free(resource->surfaces[i].data);
   }
   free(resource->surfaces);
   free(resource);
}

static HRESULT
D3D9DeallocateResource(D3D9Device *device, D3D9Resource *resource)
{
   if (!device || !resource)
      return E_INVALIDARG;

   bool has_allocation = false;
   for (UINT i = 0; i < resource->surf_count; ++i)
      has_allocation |= resource->surfaces[i].allocation != 0;
   if (!has_allocation)
      return S_OK;

   if (!device->callbacks.pfnDeallocateCb) {
      D3D9Warnf("ERROR: D3D9 resource deallocation unavailable "
                "owner=d3d9-resource reason=missing-pfnDeallocateCb "
                "action=retain-resource resource=%p runtime=%p\n",
                resource, resource->runtime_handle);
      return E_NOTIMPL;
   }

   if (resource->runtime_handle) {
      D3DDDICB_DEALLOCATE deallocate;
      memset(&deallocate, 0, sizeof(deallocate));
      deallocate.hResource = resource->runtime_handle;
      HRESULT hr = device->callbacks.pfnDeallocateCb(device->hRTDevice,
                                                     &deallocate);
      D3D9Tracef("DeallocateCb resource=%p runtime=%p hr=0x%08lx\n",
                 resource, resource->runtime_handle, hr);
      if (FAILED(hr)) {
         D3D9Warnf("ERROR: D3D9 resource deallocation failed "
                   "owner=d3d9-resource reason=pfnDeallocateCb-failed "
                   "action=retain-resource resource=%p runtime=%p "
                   "status=0x%08lx\n",
                   resource, resource->runtime_handle, hr);
         return hr;
      }

      resource->runtime_handle = NULL;
      for (UINT i = 0; i < resource->surf_count; ++i)
         resource->surfaces[i].allocation = 0;
      return S_OK;
   }

   for (UINT i = 0; i < resource->surf_count; ++i) {
      if (!resource->surfaces[i].allocation)
         continue;

      D3DKMT_HANDLE allocation = resource->surfaces[i].allocation;
      D3DDDICB_DEALLOCATE deallocate;
      memset(&deallocate, 0, sizeof(deallocate));
      deallocate.NumAllocations = 1;
      deallocate.HandleList = &allocation;
      HRESULT hr = device->callbacks.pfnDeallocateCb(device->hRTDevice,
                                                     &deallocate);
      D3D9Tracef("DeallocateCb allocation=0x%lx hr=0x%08lx\n",
                 (unsigned long)allocation, hr);
      if (FAILED(hr)) {
         D3D9Warnf("ERROR: D3D9 allocation deallocation failed "
                   "owner=d3d9-resource reason=pfnDeallocateCb-failed "
                   "action=retain-resource resource=%p subresource=%u "
                   "allocation=0x%lx status=0x%08lx\n",
                   resource, i, (unsigned long)allocation, hr);
         return hr;
      }
      for (UINT j = i; j < resource->surf_count; ++j) {
         if (resource->surfaces[j].allocation == allocation)
            resource->surfaces[j].allocation = 0;
      }
   }

   return S_OK;
}

static HRESULT
D3D9CopyInitialData(const D3DDDIARG_CREATERESOURCE *data,
                    D3D9SubResource *dst,
                    const D3DDDI_SURFACEINFO *src)
{
   if (!src->pSysMem)
      return S_OK;

   if (!D3D9PoolNeedsReadback(data->Pool)) {
      dst->data = (uint8_t *)(uintptr_t)src->pSysMem;
      dst->data_capacity = dst->size;
      dst->owns_data = false;
      dst->cpu_dirty = true;
      return S_OK;
   }

   dst->data = (uint8_t *)malloc(dst->size);
   if (!dst->data)
      return E_OUTOFMEMORY;
   dst->owns_data = true;
   dst->data_capacity = dst->size;

   if (data->Flags.VertexBuffer || data->Flags.IndexBuffer) {
      memcpy(dst->data, src->pSysMem, dst->size);
      dst->cpu_dirty = true;
      return S_OK;
   }

   const UINT row_bytes = D3D9FormatRowPitch(data->Format, src->Width);
   const UINT rows = D3D9FormatRowCount(data->Format, src->Height);
   const UINT depth = src->Depth ? src->Depth : 1;
   const UINT src_pitch = src->SysMemPitch ? src->SysMemPitch : row_bytes;
   const UINT src_slice_pitch =
      src->SysMemSlicePitch ? src->SysMemSlicePitch : src_pitch * rows;

   for (UINT z = 0; z < depth; ++z) {
      const uint8_t *src_slice = (const uint8_t *)src->pSysMem +
         (size_t)z * src_slice_pitch;
      uint8_t *dst_slice = dst->data + (size_t)z * dst->slice_pitch;
      for (UINT y = 0; y < rows; ++y)
         memcpy(dst_slice + (size_t)y * dst->pitch,
                src_slice + (size_t)y * src_pitch,
                row_bytes);
   }

   dst->cpu_dirty = true;
   return S_OK;
}


HRESULT APIENTRY
D3D9SetPriority(HANDLE hDevice, const D3DDDIARG_SETPRIORITY *data)
{
   (void)hDevice;
   if (!data || !data->hResource)
      return E_INVALIDARG;

   D3D9Resource *resource = D3D9CastResource(data->hResource);
   resource->priority = data->Priority;
   return S_OK;
}

HRESULT APIENTRY
D3D9VolBlt(HANDLE hDevice, const D3DDDIARG_VOLUMEBLT *data)
{
   D3D9Device *device = (D3D9Device *)hDevice;
   if (!device || !data)
      return E_INVALIDARG;

   D3D9Resource *dst_resource = D3D9CastResource(data->hDstResource);
   D3D9Resource *src_resource = D3D9CastResource(data->hSrcResource);
   if (D3D9UploadVolumeTextureData(device, dst_resource, src_resource))
      return S_OK;

   if (D3D9CopyVolumePipe(device, data->hDstResource, data->hSrcResource,
                          &data->SrcBox, data->DstX, data->DstY,
                          data->DstZ))
      return S_OK;

   D3D9Warnf("unsupported volume blit without pipe copy dst=%p src=%p\n",
             data->hDstResource, data->hSrcResource);
   return E_NOTIMPL;
}

HRESULT APIENTRY
D3D9BufBlt(HANDLE hDevice, const D3DDDIARG_BUFFERBLT *data)
{
   if (!data)
      return E_INVALIDARG;

   return D3D9CopyBuffer((D3D9Device *)hDevice, data->hDstResource,
                         data->hSrcResource, data->Offset,
                         data->SrcRange.Offset, data->SrcRange.Size);
}

HRESULT APIENTRY
D3D9TexBlt(HANDLE hDevice, const D3DDDIARG_TEXBLT *data)
{
   D3D9Device *device = (D3D9Device *)hDevice;
   if (!device || !data)
      return E_INVALIDARG;

   D3D9Resource *dst_resource = D3D9CastResource(data->hDstResource);
   D3D9Resource *src_resource = D3D9CastResource(data->hSrcResource);
   if (src_resource && dst_resource &&
       src_resource->pool == D3DDDIPOOL_SYSTEMMEM &&
       (src_resource->flags.Texture || src_resource->flags.CubeMap) &&
       !src_resource->flags.Volume && src_resource->mip_levels > 1 &&
       data->CubeMapFace == 0 && data->DstPoint.x == 0 &&
       data->DstPoint.y == 0 && data->SrcRect.left == 0 &&
       data->SrcRect.top == 0 &&
       data->SrcRect.right == (LONG)src_resource->surfaces[0].width &&
       data->SrcRect.bottom == (LONG)src_resource->surfaces[0].height &&
       D3D9ResourcesMatchForManagedUpload(src_resource, dst_resource, NULL)) {
      return D3D9UploadManagedTextureData(device, dst_resource,
                                          src_resource) ? S_OK : E_NOTIMPL;
   }

   RECT dst_rect;
   dst_rect.left = data->DstPoint.x;
   dst_rect.top = data->DstPoint.y;
   dst_rect.right = dst_rect.left + (data->SrcRect.right - data->SrcRect.left);
   dst_rect.bottom = dst_rect.top + (data->SrcRect.bottom - data->SrcRect.top);

   if (D3D9CopyRectPipe(device, data->hDstResource, data->CubeMapFace,
                        &dst_rect, data->hSrcResource, data->CubeMapFace,
                        &data->SrcRect, PIPE_TEX_FILTER_NEAREST))
      return S_OK;

   D3D9Warnf("unsupported texture blit without pipe copy dst=%p src=%p "
             "sub=%u\n",
             data->hDstResource, data->hSrcResource, data->CubeMapFace);
   return E_NOTIMPL;
}

HRESULT APIENTRY
D3D9SetRenderTarget(HANDLE hDevice, const D3DDDIARG_SETRENDERTARGET *data)
{
   D3D9Tracef("SetRenderTarget hDevice=%p data=%p rt=%u resource=%p sub=%u\n",
              hDevice, data, data ? data->RenderTargetIndex : 0,
              data ? data->hRenderTarget : NULL,
              data ? data->SubResourceIndex : 0);

   D3D9Device *device = (D3D9Device *)hDevice;
   if (!device || !data ||
       data->RenderTargetIndex >= ARRAYSIZE(device->render_targets))
      return E_INVALIDARG;

   device->render_targets[data->RenderTargetIndex] = data->hRenderTarget;
   device->render_target_subresources[data->RenderTargetIndex] =
      data->SubResourceIndex;
   if (data->RenderTargetIndex == 0) {
      D3D9SubResource *sub =
         D3D9GetSubResource(data->hRenderTarget, data->SubResourceIndex);
      if (sub) {
         device->viewport.X = 0;
         device->viewport.Y = 0;
         device->viewport.Width = sub->width;
         device->viewport.Height = sub->height;
      }
   }
   return S_OK;
}

HRESULT APIENTRY
D3D9SetDepthStencil(HANDLE hDevice, const D3DDDIARG_SETDEPTHSTENCIL *data)
{
   D3D9Tracef("SetDepthStencil hDevice=%p data=%p resource=%p\n",
              hDevice, data, data ? data->hZBuffer : NULL);

   D3D9Device *device = (D3D9Device *)hDevice;
   if (!device || !data)
      return E_INVALIDARG;

   device->depth_stencil = data->hZBuffer;
   return S_OK;
}

HRESULT APIENTRY
D3D9CreateResource(HANDLE hDevice, D3DDDIARG_CREATERESOURCE *data)
{
   D3D9Tracef("CreateResource hDevice=%p data=%p resource=%p format=%u pool=%u "
              "surfaces=%u levels=%u flags=0x%08x rt=%u primary=%u "
              "discard_rt=%u texture=%u vb=%u ib=%u z=%u vidpn=%u "
              "multisample=%u\n",
              hDevice, data, data ? data->hResource : NULL,
              data ? data->Format : 0, data ? data->Pool : 0,
              data ? data->SurfCount : 0, data ? data->MipLevels : 0,
              data ? data->Flags.Value : 0,
              data ? data->Flags.RenderTarget : 0,
              data ? data->Flags.Primary : 0,
              data ? data->Flags.DiscardRenderTarget : 0,
              data ? data->Flags.Texture : 0,
              data ? data->Flags.VertexBuffer : 0,
              data ? data->Flags.IndexBuffer : 0,
              data ? data->Flags.ZBuffer : 0,
              data ? data->VidPnSourceId : 0,
              data ? data->MultisampleType : 0);

   if (!hDevice || !data || !data->pSurfList || !data->SurfCount)
      return E_INVALIDARG;

   D3D9Device *device = (D3D9Device *)hDevice;
   D3D9Resource *resource = (D3D9Resource *)calloc(1, sizeof(*resource));
   bool runtime_owns_private_allocation = false;
   if (!resource)
      return E_OUTOFMEMORY;

   resource->runtime_handle = data->hResource;
   resource->format = data->Format;
   resource->pool = data->Pool;
   resource->flags = data->Flags;
   resource->surf_count = data->SurfCount;
   resource->mip_levels = data->MipLevels;
   resource->surfaces = (D3D9SubResource *)calloc(data->SurfCount,
                                                  sizeof(*resource->surfaces));
   if (!resource->surfaces) {
      free(resource);
      return E_OUTOFMEMORY;
   }

   for (UINT i = 0; i < data->SurfCount; ++i) {
      const D3DDDI_SURFACEINFO *surface = &data->pSurfList[i];
      D3D9SubResource *sub = &resource->surfaces[i];
      D3D9Tracef("CreateResource surface[%u] %ux%ux%u sysmem=%p "
                 "pitch=%u slice_pitch=%u\n",
                 i, surface->Width, surface->Height, surface->Depth,
                 surface->pSysMem, surface->SysMemPitch,
                 surface->SysMemSlicePitch);
      sub->width = surface->Width;
      sub->height = surface->Height ? surface->Height : 1;
      sub->depth = surface->Depth ? surface->Depth : 1;
      sub->pitch = D3D9ResourcePitch(data, surface);
      sub->slice_pitch = D3D9ResourceSlicePitch(data, surface, sub->pitch);
      sub->size = D3D9ResourceDimensionSize(data, surface);

      if (!sub->size)
         sub->size = 1;

      HRESULT hr = D3D9CopyInitialData(data, sub, surface);
      if (FAILED(hr)) {
         D3D9FreeResource(resource);
         return hr;
      }
   }

   const bool cpu_only_plain =
      !D3D9PoolNeedsReadback(data->Pool) &&
      !(data->Flags.VertexBuffer || data->Flags.IndexBuffer ||
        data->Flags.Texture || data->Flags.CubeMap || data->Flags.Volume ||
        D3D9ResourceIsColorTarget(data) || data->Flags.ZBuffer);

   if (cpu_only_plain) {
      D3D9Tracef("CreateResource CPU-only plain resource=%p format=%u "
                 "pool=%u size=%ux%u\n",
                 resource, data->Format, data->Pool,
                 resource->surfaces[0].width, resource->surfaces[0].height);
   } else if (device->screen) {
      const enum pipe_format pipe_format = D3D9ResourcePipeFormat(data);
      if (pipe_format != PIPE_FORMAT_NONE) {
         struct pipe_resource templ;
         memset(&templ, 0, sizeof(templ));

         templ.target = D3D9PipeTarget(data);
         templ.format = pipe_format;
         templ.width0 = resource->surfaces[0].width;
         templ.height0 = templ.target == PIPE_BUFFER ?
            1 : resource->surfaces[0].height;
         templ.depth0 = templ.target == PIPE_BUFFER ?
            1 : resource->surfaces[0].depth;
         templ.array_size = D3D9PipeArraySize(data);
         templ.last_level = resource->mip_levels ? resource->mip_levels - 1 : 0;
         if (data->Flags.AutogenMipmap && templ.target != PIPE_BUFFER) {
            const UINT max_dimension =
               MAX3(templ.width0, templ.height0, templ.depth0);
            templ.last_level = max_dimension ? util_logbase2(max_dimension) : 0;
         }
         const bool yttrium = D3D9IsYttriumScreen(device->screen);
         const bool msaa_primary =
            yttrium && data->Flags.Primary && data->MultisampleType &&
            data->SurfCount > 1;
         templ.nr_samples = data->MultisampleType ? data->MultisampleType : 1;
         if (data->Flags.Primary && data->MultisampleType && !msaa_primary)
            templ.nr_samples = 1;
         templ.nr_storage_samples = templ.nr_samples;
         templ.bind = D3D9PipeBind(data);
         templ.usage = D3D9PipeUsage(data->Pool);
         if (yttrium && data->Flags.DiscardRenderTarget)
            templ.flags |= YTTRIUM_GDI_RESOURCE_FLAG_CPU_READBACK;
         if (msaa_primary)
            templ.bind &= ~PIPE_BIND_DISPLAY_TARGET;
         else if (yttrium && data->Flags.Primary)
            templ.flags |= PIPE_RESOURCE_FLAG_FRONTEND_PRIV;

         HANDLE old_hRTResource = device->gdi_device.hRTResource;
         boolean old_hRTResourceIsD3D9 =
            device->gdi_device.hRTResourceIsD3D9;
         UINT old_allocationVidPn = device->gdi_device.allocationVidPn;
         boolean old_isPrimary = device->gdi_device.isPrimary;
         device->gdi_device.hRTResource = resource->runtime_handle;
         device->gdi_device.hRTResourceIsD3D9 = true;
         device->gdi_device.allocationVidPn =
            data->Flags.Primary ? data->VidPnSourceId : 0;
         device->gdi_device.isPrimary = data->Flags.Primary;

         D3DKMT_HANDLE display_allocation = 0;
         if (msaa_primary) {
            struct pipe_resource display_templ = templ;
            display_templ.nr_samples = 1;
            display_templ.nr_storage_samples = 1;
            display_templ.bind = D3D9PipeBind(data);
            display_templ.flags |= PIPE_RESOURCE_FLAG_FRONTEND_PRIV;
            resource->primary_display_resource =
               device->screen->resource_create(device->screen,
                                               &display_templ);
            if (!resource->primary_display_resource) {
               device->gdi_device.hRTResource = old_hRTResource;
               device->gdi_device.hRTResourceIsD3D9 =
                  old_hRTResourceIsD3D9;
               device->gdi_device.allocationVidPn = old_allocationVidPn;
               device->gdi_device.isPrimary = old_isPrimary;
               D3D9FreeResource(resource);
               return E_OUTOFMEMORY;
            }
            display_allocation =
               D3D9GetPipeResourceAllocation(
                  device, resource->primary_display_resource);
            if (!display_allocation) {
               D3D9Warnf("unsupported D3D9 MSAA primary display resource "
                         "has no D3DKMT allocation resource=%p pipe=%p\n",
                         resource, resource->primary_display_resource);
               device->gdi_device.hRTResource = old_hRTResource;
               device->gdi_device.hRTResourceIsD3D9 =
                  old_hRTResourceIsD3D9;
               device->gdi_device.allocationVidPn = old_allocationVidPn;
               device->gdi_device.isPrimary = old_isPrimary;
               D3D9FreeResource(resource);
               return E_OUTOFMEMORY;
            }

            device->gdi_device.allocationVidPn = 0;
            device->gdi_device.isPrimary = false;
            device->gdi_device.hRTResource = NULL;
            device->gdi_device.hRTResourceIsD3D9 = false;
         }

         resource->pipe_resource =
            device->screen->resource_create(device->screen, &templ);
         device->gdi_device.hRTResource = old_hRTResource;
         device->gdi_device.hRTResourceIsD3D9 = old_hRTResourceIsD3D9;
         device->gdi_device.allocationVidPn = old_allocationVidPn;
         device->gdi_device.isPrimary = old_isPrimary;

         if (!resource->pipe_resource) {
            D3D9Tracef("CreateResource pipe resource failed resource=%p "
                       "format=%u pipe_format=%u bind=0x%x size=%ux%u\n",
                       resource, data->Format, pipe_format, templ.bind,
                       templ.width0, templ.height0);
            D3D9FreeResource(resource);
            return E_OUTOFMEMORY;
         }

         /*
          * A multisampled primary has two Gallium resources: the runtime-
          * associated single-sample display image and a private multisample
          * render image.  The D3D9 runtime tracks the latter allocation as
          * part of the primary resource even though it was allocated through
          * the standalone HandleList path, and retires it during Reset before
          * DestroyResource drops the Gallium reference.  Transfer allocation
          * ownership to the runtime once creation can no longer fail; Venus
          * object teardown remains owned by the pipe resource.
          */
         runtime_owns_private_allocation = msaa_primary;

         const D3DKMT_HANDLE allocation =
            D3D9GetPipeAllocation(device, resource);
         for (UINT i = 0; i < resource->surf_count; ++i) {
            if (msaa_primary && i == 0)
               resource->surfaces[i].allocation = display_allocation;
            else
               resource->surfaces[i].allocation = allocation;
         }
         if (!allocation && D3D9ResourceIsColorTarget(data)) {
            D3D9Warnf("unsupported pipe resource has no D3DKMT allocation "
                      "resource=%p pipe=%p format=%u bind=0x%x\n",
                      resource, resource->pipe_resource, templ.format,
                      templ.bind);
         } else if (!allocation) {
            D3D9Tracef("CreateResource pipe resource has no D3DKMT "
                       "allocation resource=%p pipe=%p format=%u bind=0x%x\n",
                       resource, resource->pipe_resource, templ.format,
                       templ.bind);
         }
         D3D9Tracef("CreateResource pipe=%p allocation=0x%lx target=%u "
                    "format=%u bind=0x%x size=%ux%u levels=%u "
                    "display_pipe=%p display_allocation=0x%lx\n",
                    resource->pipe_resource, (unsigned long)allocation,
                    templ.target, templ.format, templ.bind, templ.width0,
                    templ.height0, templ.last_level + 1,
                    resource->primary_display_resource,
                    (unsigned long)display_allocation);

         for (UINT i = 0; i < resource->surf_count; ++i) {
            if (!D3D9UploadSubResource(device, resource, i)) {
               D3D9FreeResource(resource);
               return E_NOTIMPL;
            }
         }

         if (data->Pool != D3DDDIPOOL_SYSTEMMEM &&
             data->Pool != D3D9_POOL_STAGINGMEM)
            D3D9UploadManagedTextureData(device, resource,
                                         device->last_systemmem_texture);
         if ((data->Flags.VertexBuffer || data->Flags.IndexBuffer) &&
             data->Pool != D3DDDIPOOL_SYSTEMMEM &&
             D3D9ResourcesMatchForManagedBufferUpload(
                device->last_systemmem_buffer, resource)) {
            resource->managed_source_resource =
               device->last_systemmem_buffer;
            device->last_systemmem_buffer->managed_default_resource =
               resource;
         }
      } else {
         D3D9Warnf("unsupported CPU-only resource for unsupported "
                   "format=%u pool=%u flags=0x%08x resource=%p\n",
                   data->Format, data->Pool, data->Flags.Value, resource);
      }
   } else {
      D3D9Warnf("unsupported CPU-only resource without Yttrium screen "
                "format=%u pool=%u flags=0x%08x resource=%p\n",
                data->Format, data->Pool, data->Flags.Value, resource);
   }
   if (runtime_owns_private_allocation)
      yttrium_gdi_resource_set_allocation_ownership(resource->pipe_resource,
                                                    false);

   data->hResource = resource;
   if (D3D9ResourceIsColorTarget(data) && !device->render_targets[0]) {
      device->render_targets[0] = resource;
      device->render_target_subresources[0] = 0;
      D3D9Tracef("CreateResource default RT0 resource=%p\n", resource);
   }
   if (data->Flags.ZBuffer && !device->depth_stencil) {
      device->depth_stencil = resource;
      D3D9Tracef("CreateResource default depth resource=%p\n", resource);
   }
   if (data->Pool == D3DDDIPOOL_SYSTEMMEM && D3D9ResourceIsTexture(resource))
      device->last_systemmem_texture = resource;
   if (data->Pool == D3DDDIPOOL_SYSTEMMEM &&
       (data->Flags.VertexBuffer || data->Flags.IndexBuffer))
      device->last_systemmem_buffer = resource;
   D3D9Tracef("CreateResource success resource=%p runtime=%p\n",
              resource, resource->runtime_handle);
   return S_OK;
}

HRESULT APIENTRY
D3D9OpenResource(HANDLE hDevice, D3DDDIARG_OPENRESOURCE *data)
{
   D3D9Device *device = (D3D9Device *)hDevice;
   D3D9Tracef("OpenResource hDevice=%p data=%p resource=%p km=0x%lx "
              "allocations=%u private=%u flags=0x%08x\n",
              hDevice, data, data ? data->hResource : NULL,
              data ? (unsigned long)data->hKMResource : 0,
              data ? data->NumAllocations : 0,
              data ? data->PrivateDriverDataSize : 0,
              data ? data->Flags.Value : 0);

   if (!device || !data || !data->hKMResource || !data->NumAllocations ||
       !device->screen || !device->screen->resource_from_handle)
      return E_INVALIDARG;

   const VIOGPU_CREATE_ALLOCATION_EXCHANGE *standard_primary_alloc = NULL;
   if (D3D9OpenResourceIsRuntimeStandardPrimary(data,
                                                &standard_primary_alloc)) {
      const D3DDDIFORMAT d3d_format =
         D3D9VirglToFormat(standard_primary_alloc->ResourceOptions.format);
      if (d3d_format == D3DDDIFMT_UNKNOWN)
         return E_NOTIMPL;

      const UINT mip_levels =
         standard_primary_alloc->ResourceOptions.last_level + 1;
      const UINT array_size =
         standard_primary_alloc->ResourceOptions.array_size ?
         standard_primary_alloc->ResourceOptions.array_size : 1;
      const UINT surf_count = mip_levels * array_size;
      D3D9Resource *resource =
         (D3D9Resource *)calloc(1, sizeof(*resource));
      if (!resource)
         return E_OUTOFMEMORY;

      resource->surfaces =
         (D3D9SubResource *)calloc(surf_count, sizeof(*resource->surfaces));
      if (!resource->surfaces) {
         free(resource);
         return E_OUTOFMEMORY;
      }

      resource->runtime_handle = data->hResource;
      resource->format = d3d_format;
      resource->pool = D3DDDIPOOL_VIDEOMEMORY;
      resource->surf_count = surf_count;
      resource->mip_levels = mip_levels;
      resource->flags.RenderTarget =
         (standard_primary_alloc->ResourceOptions.bind &
          VIRGL_BIND_RENDER_TARGET) != 0;
      resource->flags.Primary = 1;
      resource->runtime_standard_primary = true;

      const D3DKMT_HANDLE allocation =
         data->pOpenAllocationInfo[0].hAllocation;
      for (UINT layer = 0; layer < array_size; ++layer) {
         for (UINT level = 0; level < mip_levels; ++level) {
            const UINT index = layer * mip_levels + level;
            D3D9SubResource *sub = &resource->surfaces[index];
            sub->width =
               MAX2(standard_primary_alloc->ResourceOptions.width >> level,
                    1u);
            sub->height =
               MAX2(standard_primary_alloc->ResourceOptions.height >> level,
                    1u);
            sub->depth =
               MAX2(standard_primary_alloc->ResourceOptions.depth >> level,
                    1u);
            sub->pitch = level || !standard_primary_alloc->Stride ?
               D3D9AlignPitch(D3D9FormatRowPitch(d3d_format, sub->width)) :
               standard_primary_alloc->Stride;
            sub->slice_pitch =
               sub->pitch * D3D9FormatRowCount(d3d_format, sub->height);
            sub->size = (size_t)sub->slice_pitch * sub->depth;
            sub->allocation = allocation;
         }
      }

      data->hResource = resource;
      D3D9Warnf("WARNING: D3D9 opened runtime standard-primary fullscreen "
                "without Gallium backing resource=%p km=0x%lx "
                "allocation=0x%lx %ux%u format=%u bind=0x%lx blob_mem=0x%lx "
                "owner=d3d9_runtime reason=standard_primary_not_yttrium\n",
                resource, (unsigned long)data->hKMResource,
                (unsigned long)allocation,
                standard_primary_alloc->ResourceOptions.width,
                standard_primary_alloc->ResourceOptions.height,
                d3d_format,
                (unsigned long)standard_primary_alloc->ResourceOptions.bind,
                (unsigned long)standard_primary_alloc->BlobMem);
      return S_OK;
   }

   struct winsys_handle whandle;
   memset(&whandle, 0, sizeof(whandle));
   whandle.type = WINSYS_HANDLE_TYPE_WIN32_HANDLE;
   whandle.handle = (HANDLE)(uintptr_t)data->hKMResource;

   HANDLE old_hRTResource = device->gdi_device.hRTResource;
   boolean old_hRTResourceIsD3D9 =
      device->gdi_device.hRTResourceIsD3D9;
   const D3DDDIARG_OPENRESOURCE *old_d3d9_open =
      device->gdi_device.pD3D9OpenResource;
   device->gdi_device.hRTResource = data->hResource;
   device->gdi_device.hRTResourceIsD3D9 = true;
   device->gdi_device.pD3D9OpenResource = data;
   struct pipe_resource *pipe_resource =
      device->screen->resource_from_handle(device->screen, NULL, &whandle, 0);
   device->gdi_device.pD3D9OpenResource = old_d3d9_open;
   device->gdi_device.hRTResource = old_hRTResource;
   device->gdi_device.hRTResourceIsD3D9 = old_hRTResourceIsD3D9;
   if (!pipe_resource)
      return E_OUTOFMEMORY;

   const D3DDDIFORMAT d3d_format = D3D9PipeToFormat(pipe_resource->format);
   if (d3d_format == D3DDDIFMT_UNKNOWN ||
       pipe_resource->target == PIPE_BUFFER) {
      pipe_resource_reference(&pipe_resource, NULL);
      return E_NOTIMPL;
   }

   const UINT mip_levels = pipe_resource->last_level + 1;
   const UINT array_size = pipe_resource->array_size ?
      pipe_resource->array_size : 1;
   const UINT surf_count = mip_levels * array_size;
   D3D9Resource *resource = (D3D9Resource *)calloc(1, sizeof(*resource));
   if (!resource) {
      pipe_resource_reference(&pipe_resource, NULL);
      return E_OUTOFMEMORY;
   }

   resource->surfaces = (D3D9SubResource *)calloc(surf_count,
                                                  sizeof(*resource->surfaces));
   if (!resource->surfaces) {
      free(resource);
      pipe_resource_reference(&pipe_resource, NULL);
      return E_OUTOFMEMORY;
   }

   resource->runtime_handle = data->hResource;
   resource->format = d3d_format;
   resource->pool = D3DDDIPOOL_VIDEOMEMORY;
   resource->surf_count = surf_count;
   resource->mip_levels = mip_levels;
   resource->pipe_resource = pipe_resource;
   resource->flags.RenderTarget =
      (pipe_resource->bind & PIPE_BIND_RENDER_TARGET) != 0;
   resource->flags.ZBuffer =
      (pipe_resource->bind & PIPE_BIND_DEPTH_STENCIL) != 0;
   resource->flags.Primary =
      (pipe_resource->bind & PIPE_BIND_DISPLAY_TARGET) != 0;

   D3DKMT_HANDLE allocation = D3D9GetPipeAllocation(device, resource);
   if (!allocation && data->pOpenAllocationInfo && data->NumAllocations)
      allocation = data->pOpenAllocationInfo[0].hAllocation;
   for (UINT layer = 0; layer < array_size; ++layer) {
      for (UINT level = 0; level < mip_levels; ++level) {
         const UINT index = layer * mip_levels + level;
         D3D9SubResource *sub = &resource->surfaces[index];
         sub->width = MAX2(pipe_resource->width0 >> level, 1u);
         sub->height = MAX2(pipe_resource->height0 >> level, 1u);
         sub->depth = MAX2(pipe_resource->depth0 >> level, 1u);
         sub->pitch = D3D9AlignPitch(D3D9FormatRowPitch(d3d_format,
                                                        sub->width));
         sub->slice_pitch =
            sub->pitch * D3D9FormatRowCount(d3d_format, sub->height);
         sub->size = (size_t)sub->slice_pitch * sub->depth;
         sub->allocation = allocation;
      }
   }

   data->hResource = resource;
   D3D9Tracef("OpenResource success resource=%p runtime=%p pipe=%p "
              "allocation=0x%lx target=%u format=%u bind=0x%x size=%ux%u "
              "levels=%u layers=%u\n",
              resource, resource->runtime_handle, resource->pipe_resource,
              (unsigned long)allocation, pipe_resource->target,
              pipe_resource->format, pipe_resource->bind,
              pipe_resource->width0, pipe_resource->height0,
              mip_levels, array_size);
   return S_OK;
}

HRESULT APIENTRY
D3D9Lock(HANDLE hDevice, D3DDDIARG_LOCK *data)
{
   D3D9Device *device = (D3D9Device *)hDevice;
   D3D9Tracef("Lock resource=%p sub=%u flags=0x%08x\n",
              data ? data->hResource : NULL,
              data ? data->SubResourceIndex : 0,
              data ? data->Flags.Value : 0);
   if (!device || !data || !data->hResource)
      return E_INVALIDARG;

   D3D9Resource *resource = D3D9CastResource(data->hResource);
   UINT subresource_index = D3D9ResolveNotifyLockSubResource(resource, data);
   if (subresource_index >= resource->surf_count) {
      D3D9Tracef("Lock invalid subresource resource=%p sub=%u count=%u\n",
                 resource, subresource_index, resource->surf_count);
      return E_INVALIDARG;
   }

   D3D9SubResource *sub = &resource->surfaces[subresource_index];
   const bool buffer_resource =
      (resource->pipe_resource &&
       resource->pipe_resource->target == PIPE_BUFFER) ||
      resource->flags.VertexBuffer || resource->flags.IndexBuffer;
   if (resource->flags.VertexBuffer && D3D9OrderedContextWorkerEnabled())
      D3D9ReleaseWorkerBufferUpload(device, sub);

   if (data->Flags.NotifyOnly) {
      if (buffer_resource && D3D9EnsureCpuBacking(sub)) {
         UINT offset = 0;
         UINT size = (UINT)sub->size;
         if (data->Flags.RangeValid) {
            offset = data->Range.Offset;
            if (offset > sub->size)
               return E_INVALIDARG;
            size = data->Range.Size ? data->Range.Size :
               (UINT)(sub->size - offset);
         }
         if (offset > sub->size || size > sub->size - offset)
            return E_INVALIDARG;

         data->pSurfData = sub->data + offset;
         data->Pitch = 0;
         data->SlicePitch = 0;
         sub->notify_only_locked = true;
         sub->notify_only_write = !data->Flags.ReadOnly;
         sub->notify_only_full_mip_chain = false;
         D3D9Tracef("Lock notify-only maps CPU buffer backing resource=%p "
                    "sub=%u flags=0x%08x offset=%u size=%u write=%u\n",
                    resource, subresource_index, data->Flags.Value,
                    offset, size, sub->notify_only_write ? 1 : 0);
         return S_OK;
      }

      if (!D3D9PoolNeedsReadback(resource->pool) &&
          !(resource->flags.VertexBuffer || resource->flags.IndexBuffer)) {
         struct pipe_box box;
         memset(&box, 0, sizeof(box));
         if (data->Flags.BoxValid)
            D3D9PipeBoxFromD3DBox(&data->Box, &box);
         else if (data->Flags.AreaValid)
            D3D9PipeBoxFromRect(&data->Area, &box);
         else {
            box.width = sub->width;
            box.height = sub->height;
            box.depth = sub->depth ? sub->depth : 1;
         }

         if (box.width < 0 || box.height < 0 || box.depth < 0 ||
             box.x > sub->width || box.y > sub->height ||
             box.z > sub->depth ||
             (UINT)box.width > sub->width - box.x ||
             (UINT)box.height > sub->height - box.y ||
             (UINT)box.depth > sub->depth - box.z)
            return E_INVALIDARG;

         const UINT bpp = D3D9FormatBytesPerPixel(resource->format);
         if (bpp && D3D9EnsureCpuBacking(sub)) {
            data->Pitch = sub->pitch;
            data->SlicePitch = sub->slice_pitch;
            data->pSurfData = sub->data + (size_t)box.z * sub->slice_pitch +
               (size_t)box.y * sub->pitch + (size_t)box.x * bpp;
            sub->notify_only_locked = true;
            sub->notify_only_write = !data->Flags.ReadOnly;
            sub->notify_only_full_mip_chain =
               D3D9NotifyLockCoversFullMipChain(resource, data);
            D3D9Tracef("Lock notify-only maps CPU backing resource=%p "
                       "sub=%u flags=0x%08x box=%d,%d,%d %dx%dx%d "
                       "pitch=%u write=%u\n",
                       resource, subresource_index, data->Flags.Value,
                       box.x, box.y, box.z, box.width, box.height, box.depth,
                       data->Pitch, sub->notify_only_write ? 1 : 0);
            return S_OK;
         }
      }

      sub->notify_only_locked = true;
      sub->notify_only_write = !data->Flags.ReadOnly;
      sub->notify_only_full_mip_chain =
         D3D9NotifyLockCoversFullMipChain(resource, data);
      data->pSurfData = NULL;
      data->Pitch = 0;
      data->SlicePitch = 0;
      D3D9Tracef("Lock notify-only resource=%p sub=%u flags=0x%08x write=%u\n",
                 resource, subresource_index, data->Flags.Value,
                 sub->notify_only_write ? 1 : 0);
      return S_OK;
   }

   if (sub->transfer || sub->cpu_locked) {
      if (sub->transfer && resource->pipe_resource &&
         resource->pipe_resource->target == PIPE_BUFFER &&
         data->Flags.Discard) {
         if (!device->pipe || !device->pipe->buffer_unmap)
            return E_NOTIMPL;
         D3D9Tracef("Lock discards active buffer map resource=%p sub=%u\n",
                    resource, subresource_index);
         device->pipe->buffer_unmap(device->pipe, sub->transfer);
         sub->transfer = NULL;
         sub->transfer_map = NULL;
         sub->transfer_on_readback = false;
         sub->transfer_write = false;
         memset(&sub->transfer_box, 0, sizeof(sub->transfer_box));
      } else {
         D3D9Tracef("Lock rejected active map resource=%p sub=%u "
                    "transfer=%p cpu_locked=%u flags=0x%08x\n",
                    resource, subresource_index, sub->transfer,
                    sub->cpu_locked ? 1 : 0, data->Flags.Value);
         return E_INVALIDARG;
      }
   }

   struct pipe_box box;
   memset(&box, 0, sizeof(box));
   const unsigned usage = D3D9PipeMapUsage(data->Flags);
   void *map = NULL;

   if (buffer_resource) {
      if (!resource->pipe_resource || !device->pipe) {
         D3D9Warnf("unsupported buffer lock without pipe resource "
                   "resource=%p sub=%u flags=0x%08x\n",
                   resource, subresource_index, data->Flags.Value);
         return E_NOTIMPL;
      }
      if (!device->pipe->buffer_map)
         return E_NOTIMPL;

      box.x = data->Flags.RangeValid ? data->Range.Offset : 0;
      box.width = data->Flags.RangeValid && data->Range.Size ?
         data->Range.Size : (int)(sub->size - box.x);
      box.height = 1;
      box.depth = 1;
      if (box.x > sub->size || box.width < 0 ||
          (size_t)box.width > sub->size - box.x)
         return E_INVALIDARG;

      if (D3D9ResourceUsesCpuBufferStorage(resource)) {
         if (!D3D9EnsureCpuBacking(sub))
            return E_OUTOFMEMORY;

         data->pSurfData = sub->data + box.x;
         data->Pitch = 0;
         data->SlicePitch = 0;
         sub->cpu_locked = true;
         sub->cpu_lock_write = (usage & PIPE_MAP_WRITE) != 0;
         sub->cpu_lock_packed_mips = false;
         D3D9Tracef("Lock maps CPU buffer backing resource=%p sub=%u "
                    "flags=0x%08x offset=%d size=%d write=%u\n",
                    resource, subresource_index, data->Flags.Value,
                    box.x, box.width, sub->cpu_lock_write ? 1 : 0);
         return S_OK;
      }

      map = device->pipe->buffer_map(device->pipe, resource->pipe_resource,
                                     D3D9PipeMipLevel(resource,
                                                      subresource_index),
                                     usage, &box, &sub->transfer);
      data->Pitch = 0;
      data->SlicePitch = 0;
      if (sub->transfer) {
         sub->transfer_write = (usage & PIPE_MAP_WRITE) != 0;
         sub->transfer_box = box;
         sub->transfer_map = map;
      }
   } else {
      if (data->Flags.BoxValid) {
         D3D9PipeBoxFromD3DBox(&data->Box, &box);
      } else if (data->Flags.AreaValid) {
         D3D9PipeBoxFromRect(&data->Area, &box);
      } else {
         box.width = sub->width;
         box.height = sub->height;
         box.depth = sub->depth ? sub->depth : 1;
      }
      if (box.width < 0 || box.height < 0 || box.depth < 0 ||
          box.x > sub->width || box.y > sub->height || box.z > sub->depth ||
          (UINT)box.width > sub->width - box.x ||
          (UINT)box.height > sub->height - box.y ||
          (UINT)box.depth > sub->depth - box.z)
         return E_INVALIDARG;

      if (!D3D9PoolNeedsReadback(resource->pool)) {
         size_t cpu_backing_size = sub->size;
         const bool packed_mip_lock =
            resource->pool == D3DDDIPOOL_SYSTEMMEM &&
            subresource_index == 0 &&
            (usage & PIPE_MAP_WRITE) &&
            D3D9LockCoversFullSubResource(sub, &box);
         if (packed_mip_lock) {
            const size_t packed_size = D3D9PackedMipChainSize(resource);
            if (packed_size)
               cpu_backing_size = packed_size;
         }

         if (!D3D9EnsureCpuBackingSize(sub, cpu_backing_size))
            return E_OUTOFMEMORY;

         const UINT bpp = D3D9FormatBytesPerPixel(resource->format);
         data->Pitch = sub->pitch;
         data->SlicePitch = sub->slice_pitch;
         data->pSurfData = sub->data + (size_t)box.z * sub->slice_pitch +
            (size_t)box.y * sub->pitch + (size_t)box.x * bpp;
         sub->cpu_locked = true;
         sub->cpu_lock_write = (usage & PIPE_MAP_WRITE) != 0;
         sub->cpu_lock_packed_mips =
            packed_mip_lock && cpu_backing_size > sub->size;
         D3D9Tracef("Lock maps CPU backing resource=%p sub=%u flags=0x%08x "
                    "box=%d,%d,%d %dx%dx%d pitch=%u\n",
                    resource, subresource_index, data->Flags.Value,
                    box.x, box.y, box.z, box.width, box.height, box.depth,
                    data->Pitch);
         return S_OK;
      }

      if (!resource->pipe_resource || !device->pipe) {
         D3D9Warnf("unsupported lock without pipe resource resource=%p "
                   "sub=%u flags=0x%08x\n",
                   resource, subresource_index, data->Flags.Value);
         return E_NOTIMPL;
      }

      if (sub->cpu_dirty &&
          !D3D9UploadSubResource(device, resource, subresource_index))
         return E_NOTIMPL;

      if ((usage & PIPE_MAP_READ) && D3D9PoolNeedsReadback(resource->pool)) {
         map = D3D9ReadbackTextureLock(device, resource, sub,
                                       subresource_index, usage, &box,
                                       data);
      } else {
         if (!device->pipe->texture_map)
            return E_NOTIMPL;

         map = device->pipe->texture_map(device->pipe,
                                         resource->pipe_resource,
                                         D3D9PipeMipLevel(
                                            resource, subresource_index),
                                         usage, &box,
                                         &sub->transfer);
         if (sub->transfer) {
            data->Pitch = sub->transfer->stride;
            data->SlicePitch = sub->transfer->layer_stride;
            sub->transfer_write = (usage & PIPE_MAP_WRITE) != 0;
            sub->transfer_box = box;
         }
      }
   }

   if (!map || !sub->transfer)
      return E_FAIL;

   D3D9Tracef("Lock maps pipe resource resource=%p sub=%u flags=0x%08x "
              "box=%d,%d,%d %dx%dx%d\n",
              resource, subresource_index, data->Flags.Value,
              box.x, box.y, box.z, box.width, box.height, box.depth);
   data->pSurfData = map;
   return S_OK;
}

HRESULT APIENTRY
D3D9Unlock(HANDLE hDevice, const D3DDDIARG_UNLOCK *data)
{
   D3D9Device *device = (D3D9Device *)hDevice;
   D3D9Tracef("Unlock resource=%p sub=%u\n",
              data ? data->hResource : NULL,
              data ? data->SubResourceIndex : 0);
   if (!device || !data || !data->hResource)
      return E_INVALIDARG;

   D3D9Resource *resource = D3D9CastResource(data->hResource);
   UINT subresource_index = D3D9ResolveNotifyUnlockSubResource(resource, data);
   if (subresource_index >= resource->surf_count) {
      D3D9Tracef("Unlock invalid subresource resource=%p sub=%u count=%u\n",
                 resource, subresource_index, resource->surf_count);
      return E_INVALIDARG;
   }

   D3D9SubResource *sub = &resource->surfaces[subresource_index];
   if (data->Flags.NotifyOnly || sub->notify_only_locked) {
      if (sub->notify_only_write && !resource->managed_default_pipe_resource) {
         const bool full_mip_chain =
            sub->notify_only_full_mip_chain ||
            (resource->pool == D3DDDIPOOL_SYSTEMMEM &&
             subresource_index == 0 && D3D9HasContiguousMipData(resource));
         if (full_mip_chain)
            D3D9MarkMipChainCpuDirty(resource);
         else
            sub->cpu_dirty = true;
         if (resource->pipe_resource &&
             !(full_mip_chain ?
               D3D9UploadDirtyTextureSubResources(device, resource) :
               D3D9UploadSubResource(device, resource, subresource_index)))
            return E_NOTIMPL;
      }
      sub->notify_only_locked = false;
      sub->notify_only_write = false;
      sub->notify_only_full_mip_chain = false;
      D3D9Tracef("Unlock completes notify-only lock resource=%p sub=%u "
                 "flags=0x%08x dirty=%u\n",
                 resource, subresource_index, data->Flags.Value,
                 sub->cpu_dirty ? 1 : 0);
      return S_OK;
   }

   if (sub->cpu_locked) {
      HRESULT hr = S_OK;
      if (sub->cpu_lock_write) {
         if (sub->cpu_lock_packed_mips &&
             !D3D9DistributePackedMipLock(resource))
            hr = E_NOTIMPL;
         sub->cpu_dirty = true;
      }
      sub->cpu_locked = false;
      sub->cpu_lock_write = false;
      sub->cpu_lock_packed_mips = false;
      if (FAILED(hr))
         return hr;
      D3D9Tracef("Unlock CPU backing resource=%p sub=%u dirty=%u\n",
                 resource, subresource_index, sub->cpu_dirty ? 1 : 0);
      return S_OK;
   }

   if (!sub->transfer) {
      D3D9Tracef("Unlock without active transfer resource=%p sub=%u\n",
                 resource, subresource_index);
      return E_INVALIDARG;
   }

   bool worker_upload_ok = true;
   if (sub->transfer_on_readback) {
      if (!device->pipe->texture_unmap)
         return E_NOTIMPL;

      device->pipe->texture_unmap(device->pipe, sub->transfer);
      sub->transfer = NULL;

      if (sub->transfer_write) {
         if (!device->pipe->resource_copy_region)
            return E_NOTIMPL;

         device->pipe->resource_copy_region(device->pipe,
                                            resource->pipe_resource,
                                            D3D9PipeMipLevel(
                                               resource,
                                               subresource_index),
                                            sub->transfer_box.x,
                                            sub->transfer_box.y,
                                            sub->transfer_box.z,
                                            sub->readback_resource, 0,
                                            &sub->transfer_box);
      }
   } else if (resource->pipe_resource &&
       resource->pipe_resource->target == PIPE_BUFFER) {
      if (!device->pipe->buffer_unmap)
         return E_NOTIMPL;
      if (sub->transfer_write && resource->flags.VertexBuffer &&
          D3D9OrderedContextWorkerEnabled())
         worker_upload_ok = D3D9CaptureWorkerBufferUpload(device, sub);
      device->pipe->buffer_unmap(device->pipe, sub->transfer);
      sub->transfer = NULL;
   } else {
      if (!device->pipe->texture_unmap)
         return E_NOTIMPL;
      device->pipe->texture_unmap(device->pipe, sub->transfer);
      sub->transfer = NULL;
   }
   sub->transfer_on_readback = false;
   sub->transfer_map = NULL;
   if (sub->transfer_write)
      D3D9MarkAutogenMipmapsDirty(resource, subresource_index);
   sub->transfer_write = false;
   memset(&sub->transfer_box, 0, sizeof(sub->transfer_box));
   sub->cpu_dirty = false;
   D3D9Tracef("Unlock pipe resource=%p sub=%u\n",
              resource, subresource_index);
   return worker_upload_ok ? S_OK : E_OUTOFMEMORY;
}

HRESULT APIENTRY
D3D9LockAsync(HANDLE hDevice, D3DDDIARG_LOCKASYNC *data)
{
   if (!data)
      return E_INVALIDARG;

   D3DDDIARG_LOCK lock;
   memset(&lock, 0, sizeof(lock));
   lock.hResource = data->hResource;
   lock.SubResourceIndex = data->SubResourceIndex;
   lock.Flags.WriteOnly = TRUE;
   lock.Flags.NoOverwrite = data->Flags.NoOverwrite;
   lock.Flags.Discard = data->Flags.Discard;
   lock.Flags.RangeValid = data->Flags.RangeValid;
   lock.Flags.AreaValid = data->Flags.AreaValid;
   lock.Flags.BoxValid = data->Flags.BoxValid;
   lock.Flags.NotifyOnly = data->Flags.NotifyOnly;
   if (data->Flags.BoxValid)
      lock.Box = data->Box;
   else if (data->Flags.AreaValid)
      lock.Area = data->Area;
   else
      lock.Range = data->Range;

   HRESULT hr = D3D9Lock(hDevice, &lock);
   if (FAILED(hr))
      return hr;

   data->hCookie = NULL;
   data->pSurfData = lock.pSurfData;
   data->Pitch = lock.Pitch;
   data->SlicePitch = lock.SlicePitch;
   return S_OK;
}

HRESULT APIENTRY
D3D9UnlockAsync(HANDLE hDevice, const D3DDDIARG_UNLOCKASYNC *data)
{
   if (!data)
      return E_INVALIDARG;

   D3DDDIARG_UNLOCK unlock;
   memset(&unlock, 0, sizeof(unlock));
   unlock.hResource = data->hResource;
   unlock.SubResourceIndex = data->SubResourceIndex;
   return D3D9Unlock(hDevice, &unlock);
}

HRESULT APIENTRY
D3D9GetPitch(HANDLE hDevice, D3DDDIARG_GETPITCH *data)
{
   (void)hDevice;
   D3D9Tracef("GetPitch resource=%p sub=%u\n",
              data ? data->hResource : NULL,
              data ? data->SubResourceIndex : 0);
   if (!data || !data->hResource)
      return E_INVALIDARG;

   D3D9SubResource *sub =
      D3D9GetSubResource(data->hResource, data->SubResourceIndex);
   if (!sub)
      return E_INVALIDARG;

   data->Pitch = sub->pitch;
   return S_OK;
}

HRESULT APIENTRY
D3D9ResolveSharedResource(HANDLE hDevice,
                          const D3DDDIARG_RESOLVESHAREDRESOURCE *data)
{
   D3D9Device *device = (D3D9Device *)hDevice;
   D3D9Tracef("ResolveSharedResource resource=%p\n",
              data ? data->hResource : NULL);
   if (!device || !data || !data->hResource)
      return E_INVALIDARG;

   D3D9Resource *resource = D3D9CastResource(data->hResource);
   if (!resource)
      return E_INVALIDARG;

   if (resource->pipe_resource && device->pipe)
      yttrium_gdi_flush_labeled(device->pipe, NULL, 0,
                                "D3D9 resolve shared resource");
   return S_OK;
}

HRESULT APIENTRY
D3D9QueryResourceResidency(HANDLE hDevice,
                           const D3DDDIARG_QUERYRESOURCERESIDENCY *data)
{
   (void)hDevice;
   if (!data || (!data->pHandleList && data->NumResources))
      return E_INVALIDARG;

   for (UINT i = 0; i < data->NumResources; ++i) {
      D3D9Resource *resource = D3D9CastResource(data->pHandleList[i]);
      if (!resource)
         return E_INVALIDARG;
   }

   return S_OK;
}

HRESULT APIENTRY
D3D9GetCaptureAllocationHandle(HANDLE hDevice,
                               D3DDDIARG_GETCAPTUREALLOCATIONHANDLE *data)
{
   (void)hDevice;
   D3D9Tracef("GetCaptureAllocationHandle resource=%p\n",
              data ? data->hResource : NULL);
   if (!data || !data->hResource)
      return E_INVALIDARG;

   D3D9SubResource *sub = D3D9GetSubResource(data->hResource, 0);
   if (!sub || !sub->allocation)
      return E_INVALIDARG;

   data->hAllocation = sub->allocation;
   return S_OK;
}

HRESULT APIENTRY
D3D9CaptureToSysMem(HANDLE hDevice, const D3DDDIARG_CAPTURETOSYSMEM *data)
{
   D3D9Device *device = (D3D9Device *)hDevice;
   D3D9Tracef("CaptureToSysMem src=%p dst=%p/%u\n",
              data ? data->hSrcResource : NULL,
              data ? data->hDstResource : NULL,
              data ? data->DstSubResourceIndex : 0);
   if (!device || !data || !data->hSrcResource || !data->hDstResource)
      return E_INVALIDARG;

   HRESULT hr = D3D9CopyRectPipe(device, data->hDstResource,
                                 data->DstSubResourceIndex,
                                 &data->DstRect, data->hSrcResource, 0,
                                 &data->SrcRect,
                                 PIPE_TEX_FILTER_NEAREST) ? S_OK : E_NOTIMPL;
   D3D9Tracef("CaptureToSysMem hr=0x%08lx\n", hr);
   return hr;
}

HRESULT APIENTRY
D3D9Blt(HANDLE hDevice, const D3DDDIARG_BLT *data)
{
   D3D9Device *device = (D3D9Device *)hDevice;
   D3D9Tracef("Blt data=%p dst=%p/%u src=%p/%u\n", data,
              data ? data->hDstResource : NULL,
              data ? data->DstSubResourceIndex : 0,
              data ? data->hSrcResource : NULL,
              data ? data->SrcSubResourceIndex : 0);
   if (!device || !data)
      return E_INVALIDARG;

   if (data->Flags.SrcColorKey || data->Flags.DstColorKey ||
       data->Flags.MirrorLeftRight ||
       data->Flags.MirrorUpDown || data->Flags.LinearToSrgb ||
       data->Flags.Rotate) {
      static volatile LONG logged;
      D3D9WarnOncef(
         &logged,
         "unsupported D3D9 blit owner=d3d9-blt "
         "reason=colorkey-transform-not-implemented "
         "action=reject flags=0x%08x color_key=0x%08x\n",
         data->Flags.Value, data->ColorKey);
      return E_NOTIMPL;
   }

   HRESULT hr = D3D9CopyRectPipe(device, data->hDstResource,
                                 data->DstSubResourceIndex, &data->DstRect,
                                 data->hSrcResource,
                                 data->SrcSubResourceIndex,
                                 &data->SrcRect,
                                 data->Flags.Linear ? PIPE_TEX_FILTER_LINEAR :
                                                      PIPE_TEX_FILTER_NEAREST) ?
      S_OK : E_NOTIMPL;
   D3D9Tracef("Blt hr=0x%08lx\n", hr);
   return hr;
}

HRESULT APIENTRY
D3D9ColorFill(HANDLE hDevice, const D3DDDIARG_COLORFILL *data)
{
   D3D9Device *device = (D3D9Device *)hDevice;
   D3D9Tracef("ColorFill data=%p resource=%p sub=%u color=0x%08x\n", data,
              data ? data->hResource : NULL,
              data ? data->SubResourceIndex : 0, data ? data->Color : 0);
   if (!device || !data)
      return E_INVALIDARG;

   const bool cleared =
      D3D9ClearColorPipe(device, data->hResource, data->SubResourceIndex,
                         &data->DstRect, data->Color, false, true);
   HRESULT hr = cleared ? S_OK : E_NOTIMPL;
   D3D9Tracef("ColorFill hr=0x%08lx\n", hr);
   return hr;
}

HRESULT APIENTRY
D3D9DepthFill(HANDLE hDevice, const D3DDDIARG_DEPTHFILL *data)
{
   D3D9Device *device = (D3D9Device *)hDevice;
   D3D9Tracef("DepthFill data=%p resource=%p sub=%u depth=%u\n", data,
              data ? data->hResource : NULL,
              data ? data->SubResourceIndex : 0, data ? data->Depth : 0);
   if (!device || !data)
      return E_INVALIDARG;

   const float depth = data->Depth / 16777215.0f;
   HRESULT hr = D3D9ClearDepthPipe(device, data->hResource,
                                   data->SubResourceIndex, &data->DstRect,
                                   true, depth, false, 0) ?
      S_OK : E_NOTIMPL;
   D3D9Tracef("DepthFill hr=0x%08lx\n", hr);
   return hr;
}

HRESULT APIENTRY
D3D9Present(HANDLE hDevice, const D3DDDIARG_PRESENT *data)
{
   D3D9Device *device = (D3D9Device *)hDevice;
   D3D9Tracef("Present data=%p src=%p/%u dst=%p/%u flags=0x%08x\n", data,
              data ? data->hSrcResource : NULL,
              data ? data->SrcSubResourceIndex : 0,
              data ? data->hDstResource : NULL,
              data ? data->DstSubResourceIndex : 0,
              data ? data->Flags.Value : 0);

   if (!device || !data)
      return E_INVALIDARG;

   HANDLE src_handle = data->hSrcResource ? data->hSrcResource :
      device->render_targets[0];
   UINT src_subresource_index = data->hSrcResource ?
      data->SrcSubResourceIndex : device->render_target_subresources[0];
   D3D9SubResource *src =
      D3D9GetSubResource(src_handle, src_subresource_index);
   D3D9SubResource *dst =
      D3D9GetSubResource(data->hDstResource, data->DstSubResourceIndex);
   D3D9Resource *src_resource = D3D9CastResource(src_handle);

   if (!src || !src_resource)
      return E_INVALIDARG;

   if (!src_resource->pipe_resource && src_resource->runtime_standard_primary) {
      D3D9Warnf("WARNING: D3D9 acknowledged runtime standard-primary "
                "Present without Gallium backing resource=%p "
                "allocation=0x%lx flags=0x%08x "
                "owner=d3d9_runtime reason=fullscreen_ownership_transition\n",
                src_resource, (unsigned long)src->allocation,
                src_resource->flags.Value);
      return S_OK;
   }

   if (!src_resource->pipe_resource) {
      D3D9Warnf("unsupported D3D9Present from resource without Gallium "
                "backing resource=%p allocation=0x%lx flags=0x%08x "
                "owner=d3d9 reason=no_gallium_backing_present\n",
                src_resource, (unsigned long)src->allocation,
                src_resource->flags.Value);
      return E_INVALIDARG;
   }

   if (src->cpu_dirty &&
       !D3D9UploadSubResource(device, src_resource, src_subresource_index))
      return E_NOTIMPL;
   D3D9Tracef("Present flush_frontbuffer src=0x%lx dst=0x%lx pipe=%p\n",
              (unsigned long)(src ? src->allocation : 0),
              (unsigned long)(dst ? dst->allocation : 0),
              src_resource->pipe_resource);
   HRESULT hr = D3D9FlushFrontbuffer(device, src_resource,
                                     src_subresource_index,
                                     dst ? dst->allocation : 0,
                                     src_resource->flags.Primary != 0 &&
                                        data->Flags.Flip != 0);
   D3D9Tracef("Present hr=0x%08lx\n", hr);
   return hr;
}

HRESULT APIENTRY
D3D9Clear(HANDLE hDevice, const D3DDDIARG_CLEAR *data, UINT rect_count,
          const RECT *rects)
{
   D3D9Tracef("Clear hDevice=%p data=%p flags=0x%08x rects=%u color=0x%08x "
              "depth=%f stencil=%u\n",
              hDevice, data, data ? data->Flags : 0, rect_count,
              data ? data->FillColor : 0, data ? data->FillDepth : 0.0f,
              data ? data->FillStencil : 0);

   D3D9Device *device = (D3D9Device *)hDevice;
   if (!device || !data || (rect_count && !rects))
      return E_INVALIDARG;

   const bool clear_color = (data->Flags & D3DCLEAR_TARGET) != 0;
   const bool clear_depth = (data->Flags & D3DCLEAR_ZBUFFER) != 0;
   const bool clear_stencil = (data->Flags & D3DCLEAR_STENCIL) != 0;
   if (!clear_color && !clear_depth && !clear_stencil)
      return S_OK;

   const UINT count = rect_count ? rect_count : 1;
   HRESULT first_failure = S_OK;

   for (UINT i = 0; i < count; ++i) {
      const RECT *rect = rect_count ? &rects[i] : NULL;

      if (clear_color) {
         for (UINT rt = 0; rt < ARRAYSIZE(device->render_targets); ++rt) {
            if (!device->render_targets[rt])
               continue;

            RECT clear_rect;
            D3D9SubResource *sub =
               D3D9GetSubResource(device->render_targets[rt],
                                  device->render_target_subresources[rt]);
            if (!D3D9ClearRectForSubResource(device, sub, rect, &clear_rect))
               continue;

            if (!D3D9ClearColorPipe(device, device->render_targets[rt],
                                    device->render_target_subresources[rt],
                                    &clear_rect, data->FillColor,
                                    device->render_states[
                                       D3DDDIRS_SRGBWRITEENABLE] != 0,
                                    false) &&
                SUCCEEDED(first_failure))
               first_failure = E_NOTIMPL;
         }
      }

      if ((clear_depth || clear_stencil) && device->depth_stencil) {
         RECT clear_rect;
         D3D9SubResource *sub = D3D9GetSubResource(device->depth_stencil, 0);
         if (!D3D9ClearRectForSubResource(device, sub, rect, &clear_rect))
            continue;

         if (!D3D9ClearDepthPipe(device, device->depth_stencil, 0,
                                 &clear_rect,
                                 clear_depth, data->FillDepth,
                                 clear_stencil, data->FillStencil) &&
             SUCCEEDED(first_failure))
            first_failure = E_NOTIMPL;
      }
   }

   return first_failure;
}

HRESULT APIENTRY
D3D9DestroyResource(HANDLE hDevice, HANDLE resource)
{
   D3D9Device *device = (D3D9Device *)hDevice;
   D3D9Resource *d3d9_resource = D3D9CastResource(resource);

   /*
    * Venus batches retain pipe_resource references until their GPU work
    * completes.  A D3D9 allocation created under data->hResource must be
    * deallocated before DestroyResource returns, because the runtime handle is
    * no longer valid afterwards.  Retire batches which reference resources
    * owned by this wrapper before dropping its final frontend references.
    */
   if (device && device->pipe && device->pipe->flush_resource &&
       d3d9_resource) {
      bool runtime_allocation = false;
      if (d3d9_resource->pipe_resource) {
         runtime_allocation |= yttrium_gdi_resource_has_runtime_allocation(
            d3d9_resource->pipe_resource);
         device->pipe->flush_resource(device->pipe,
                                      d3d9_resource->pipe_resource);
      }
      if (d3d9_resource->primary_display_resource) {
         runtime_allocation |= yttrium_gdi_resource_has_runtime_allocation(
            d3d9_resource->primary_display_resource);
         device->pipe->flush_resource(
            device->pipe, d3d9_resource->primary_display_resource);
      }
      if (runtime_allocation) {
         struct pipe_context *driver_pipe =
            threaded_context_unwrap_sync(device->pipe);
         if (d3d9_resource->pipe_resource) {
            yttrium_gdi_pipeline_invalidate_resource(
               driver_pipe, d3d9_resource->pipe_resource);
         }
         if (d3d9_resource->primary_display_resource) {
            yttrium_gdi_pipeline_invalidate_resource(
               driver_pipe, d3d9_resource->primary_display_resource);
         }
      }
   }

   if (d3d9_resource && !d3d9_resource->pipe_resource) {
      const HRESULT hr = D3D9DeallocateResource(device, d3d9_resource);
      if (FAILED(hr))
         return hr;
   }

   if (device && device->last_systemmem_texture == d3d9_resource)
      device->last_systemmem_texture = NULL;
   if (device && device->last_systemmem_buffer == d3d9_resource)
      device->last_systemmem_buffer = NULL;
   if (device) {
      for (UINT i = 0; i < ARRAYSIZE(device->render_targets); ++i) {
         if (device->render_targets[i] == resource) {
            device->render_targets[i] = NULL;
            device->render_target_subresources[i] = 0;
         }
      }
      if (device->depth_stencil == resource)
         device->depth_stencil = NULL;
   }
   if (d3d9_resource && d3d9_resource->managed_source_resource &&
       d3d9_resource->managed_source_resource->managed_default_resource ==
       d3d9_resource) {
      d3d9_resource->managed_source_resource->managed_default_resource = NULL;
      if (d3d9_resource->managed_source_resource->
             managed_default_pipe_resource == d3d9_resource->pipe_resource) {
         /*
          * A managed SYSTEMMEM wrapper retains the uploaded default-pool
          * resource so later draws can reuse it.  Drop that hidden Gallium
          * reference while the default resource's D3D9 runtime handle is
          * still valid; otherwise the allocation survives this callback and
          * pfnDeallocateCb rejects its eventual destruction.
          */
         pipe_resource_reference(
            &d3d9_resource->managed_source_resource->
                managed_default_pipe_resource,
            NULL);
      }
   }
   if (d3d9_resource && d3d9_resource->managed_default_resource &&
       d3d9_resource->managed_default_resource->managed_source_resource ==
       d3d9_resource)
      d3d9_resource->managed_default_resource->managed_source_resource = NULL;
   if (d3d9_resource && device && device->pipe) {
      for (UINT i = 0; i < d3d9_resource->surf_count; ++i)
         D3D9ReleaseWorkerBufferUpload(
            device, &d3d9_resource->surfaces[i]);
   }
   D3D9FreeResource(d3d9_resource);
   return S_OK;
}
