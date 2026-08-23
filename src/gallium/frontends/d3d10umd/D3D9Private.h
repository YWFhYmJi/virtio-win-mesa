/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright 2026 Ake Rehnman
 */

#pragma once

#include "Debug.h"
#include "util/u_inlines.h"

#include <winddk_compat.h>

#undef D3D_UMD_INTERFACE_VERSION
#define D3D_UMD_INTERFACE_VERSION D3D_UMD_INTERFACE_VERSION_WIN7

#include <d3d9.h>
#include <d3dumddi.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "frontend/winsys_handle.h"
#include "pipe/p_context.h"
#include "pipe/p_defines.h"
#include "pipe/p_screen.h"
#include "gdikmt/gdikmt.h"
#include "gdikmt_d3dddi.h"
#include "gallium/winsys/yttrium/gdi/yttrium_gdi_public.h"

#ifndef D3DCAPS2_CANRENDERWINDOWED
#define D3DCAPS2_CANRENDERWINDOWED 0x00080000
#endif

#ifndef D3DDEVCAPS_FLOATTLVERTEX
#define D3DDEVCAPS_FLOATTLVERTEX 0x00000001
#endif

#ifndef D3DPRASTERCAPS_SUBPIXEL
#define D3DPRASTERCAPS_SUBPIXEL 0x00000020
#endif

#ifndef D3DPRASTERCAPS_STIPPLE
#define D3DPRASTERCAPS_STIPPLE 0x00000200
#endif

#ifndef D3DPRASTERCAPS_ZBIAS
#define D3DPRASTERCAPS_ZBIAS 0x00004000
#endif

#ifndef D3DPTEXTURECAPS_TRANSPARENCY
#define D3DPTEXTURECAPS_TRANSPARENCY 0x00000008
#endif

#define D3D9_TEXTURE_STAGE_COUNT (D3DVERTEXTEXTURESAMPLER3 + 1)
#define D3D9_TEXTURE_STAGE_STATE_COUNT 35
#define D3D9_MAX_USER_CLIP_PLANES 8

EXTERN_C struct pipe_screen *
d3d10_create_screen(gdikmt_device *device);

struct D3D9Adapter
{
   HANDLE hRTAdapter;
   D3DDDI_ADAPTERCALLBACKS callbacks;
   struct gdikmt_device *msaa_device;
   struct yttrium_venus *msaa_venus;
   UINT ms_types;
};

struct D3D9Resource;

struct D3D9Device
{
   HANDLE hRTDevice;
   D3DDDI_DEVICECALLBACKS callbacks;
   struct pipe_screen *screen;
   struct pipe_context *pipe;
   struct gdikmt_device_d3dddi gdi_device;
   UINT render_states[256];
   UINT texture_stage_states[D3D9_TEXTURE_STAGE_COUNT][D3D9_TEXTURE_STAGE_STATE_COUNT];
   UINT texture_transform_count;
   BOOL texture_transform_count_pending;
   BOOL texture_transform_count_consumed;
   D3DMATRIX transforms[512];
   D3DDDIARG_VIEWPORTINFO viewport;
   D3DDDIARG_ZRANGE zrange;
   D3DDDIARG_WINFO winfo;
   RECT scissor_rect;
   D3DDDIARG_SETMATERIAL material;
   FLOAT clip_planes[D3D9_MAX_USER_CLIP_PLANES][4];
   HANDLE pixel_shader;
   HANDLE vertex_shader_func;
   HANDLE vertex_shader_decl;
   FLOAT vs_float_constants[256][4];
   INT vs_int_constants[16][4];
   BOOL vs_bool_constants[256];
   FLOAT ps_float_constants[224][4];
   INT ps_int_constants[16][4];
   BOOL ps_bool_constants[256];
   HANDLE textures[D3D9_TEXTURE_STAGE_COUNT];
   struct pipe_resource *null_textures[4];
   D3D9Resource *last_systemmem_texture;
   D3D9Resource *last_systemmem_buffer;
   HANDLE render_targets[4];
   UINT render_target_subresources[4];
   HANDLE depth_stencil;
   HANDLE index_buffer;
   const void *index_sysmem;
   UINT index_stride;
   struct {
      HANDLE vertex_buffer;
      const void *sysmem;
      UINT offset;
      UINT stride;
      UINT divider;
   } streams[16];
   struct {
      UINT flags;
      HANDLE resource;
      PALETTEENTRY entries[256];
   } palettes[256];
   struct {
      BOOL enabled;
      D3DDDI_LIGHT data;
   } lights[16];
};

struct D3D9SubResource
{
   UINT width;
   UINT height;
   UINT depth;
   UINT pitch;
   UINT slice_pitch;
   size_t size;
   uint8_t *data;
   size_t data_capacity;
   bool owns_data;
   D3DKMT_HANDLE allocation;
   bool cpu_dirty;
   struct pipe_transfer *transfer;
   struct pipe_resource *readback_resource;
   bool transfer_on_readback;
   bool transfer_write;
   struct pipe_box transfer_box;
   void *transfer_map;
   struct pipe_resource *worker_upload_buffer;
   unsigned worker_upload_offset;
   unsigned worker_upload_source_offset;
   unsigned worker_upload_size;
   bool cpu_locked;
   bool cpu_lock_write;
   bool cpu_lock_packed_mips;
   bool notify_only_locked;
   bool notify_only_write;
   bool notify_only_full_mip_chain;
};

struct D3D9Resource
{
   HANDLE runtime_handle;
   D3DDDIFORMAT format;
   D3DDDI_POOL pool;
   D3DDDI_RESOURCEFLAGS flags;
   UINT priority;
   UINT surf_count;
   UINT mip_levels;
   struct pipe_resource *pipe_resource;
   struct pipe_resource *primary_display_resource;
   D3D9SubResource *surfaces;
   UINT notify_lock_mip_cursor;
   UINT notify_lock_subresource;
   bool notify_lock_subresource_valid;
   bool managed_upload_complete;
   bool autogen_mipmap_dirty;
   struct pipe_resource *managed_default_pipe_resource;
   struct pipe_resource *managed_source_pipe_resource;
   D3D9Resource *managed_default_resource;
   D3D9Resource *managed_source_resource;
   UINT managed_source_base_level;
   bool runtime_standard_primary;
};

static inline UINT
D3D9PipeMipLevel(const D3D9Resource *resource, UINT subresource_index)
{
   const UINT mip_levels = resource && resource->mip_levels ?
      resource->mip_levels : 1;
   return subresource_index % mip_levels;
}

static inline UINT
D3D9PipeLayer(const D3D9Resource *resource, UINT subresource_index)
{
   if (!resource)
      return 0;

   if (!resource->flags.CubeMap &&
       (!resource->pipe_resource || resource->pipe_resource->array_size <= 1))
      return 0;

   const UINT mip_levels = resource->mip_levels ? resource->mip_levels : 1;
   return subresource_index / mip_levels;
}

static inline void
D3D9MarkAutogenMipmapsDirty(D3D9Resource *resource, UINT subresource_index)
{
   if (!resource || !resource->flags.AutogenMipmap ||
       D3D9PipeMipLevel(resource, subresource_index) != 0)
      return;

   resource->autogen_mipmap_dirty = true;
}

enum D3D9ObjectKind
{
   D3D9_OBJECT_SHADER,
   D3D9_OBJECT_VERTEX_DECL,
   D3D9_OBJECT_QUERY,
};

struct nine_range;

struct D3D9Object
{
   D3D9ObjectKind kind;
   void *translated_vs_cso;
   UINT translated_vs_key;
   unsigned translated_vs_const_used_size;
   unsigned *translated_vs_const_ranges;
   struct nine_range *translated_vs_lconstf_ranges;
   float *translated_vs_lconstf_data;
   bool translated_vs_outputs_point_size;
   uint16_t translated_vs_input_map[PIPE_MAX_ATTRIBS];
   uint8_t translated_vs_num_inputs;
   uint16_t translated_vs_sampler_mask;
   uint8_t translated_vs_sampler_targets[4];
   void *translated_ps_cso;
   UINT translated_ps_key;
   uint64_t translated_ps_sampler_type_overrides;
   uint16_t translated_ps_sampler_type_override_mask;
   uint16_t translated_ps_fetch4;
   uint16_t translated_ps_fetch4_ati1;
   uint16_t translated_ps_fetch4_projected_fallback;
   unsigned translated_ps_const_used_size;
   unsigned *translated_ps_const_ranges;
   struct nine_range *translated_ps_lconstf_ranges;
   float *translated_ps_lconstf_data;
   bool translated_ps_uses_face;
   uint16_t translated_ps_input_map[PIPE_MAX_ATTRIBS];
   uint8_t translated_ps_num_inputs;
   uint16_t translated_ps_sampler_mask;
   uint8_t translated_ps_sampler_targets[16];
   size_t size;
   D3DDDIQUERYTYPE query_type;
   D3DDDI_ISSUEQUERYFLAGS query_flags;
   uint8_t data[1];
};

static constexpr D3DDDIFORMAT
D3D9FourCC(char a, char b, char c, char d)
{
   return (D3DDDIFORMAT)((uint32_t)(uint8_t)a |
                         ((uint32_t)(uint8_t)b << 8) |
                         ((uint32_t)(uint8_t)c << 16) |
                         ((uint32_t)(uint8_t)d << 24));
}

static constexpr D3DDDIFORMAT D3D9_FMT_X4R4G4B4 = (D3DDDIFORMAT)30;
static constexpr D3DDDIFORMAT D3D9_FMT_A2B10G10R10 = (D3DDDIFORMAT)31;
static constexpr D3DDDIFORMAT D3D9_FMT_G16R16 = (D3DDDIFORMAT)34;
static constexpr D3DDDIFORMAT D3D9_FMT_A16B16G16R16 = (D3DDDIFORMAT)36;
static constexpr D3DDDIFORMAT D3D9_FMT_A8P8 = (D3DDDIFORMAT)40;
static constexpr D3DDDIFORMAT D3D9_FMT_P8 = (D3DDDIFORMAT)41;
static constexpr D3DDDIFORMAT D3D9_FMT_L8 = (D3DDDIFORMAT)50;
static constexpr D3DDDIFORMAT D3D9_FMT_A8L8 = (D3DDDIFORMAT)51;
static constexpr D3DDDIFORMAT D3D9_FMT_G8R8 = (D3DDDIFORMAT)91;
static constexpr D3DDDIFORMAT D3D9_FMT_R8 = (D3DDDIFORMAT)92;
static constexpr D3DDDIFORMAT D3D9_FMT_D16_LOCKABLE = (D3DDDIFORMAT)70;
static constexpr D3DDDIFORMAT D3D9_FMT_L16 = (D3DDDIFORMAT)81;
static constexpr D3DDDIFORMAT D3D9_FMT_D32F_LOCKABLE = (D3DDDIFORMAT)82;
static constexpr D3DDDIFORMAT D3D9_FMT_Q16W16V16U16 = (D3DDDIFORMAT)110;
static constexpr D3DDDIFORMAT D3D9_FMT_R16F = (D3DDDIFORMAT)111;
static constexpr D3DDDIFORMAT D3D9_FMT_G16R16F = (D3DDDIFORMAT)112;
static constexpr D3DDDIFORMAT D3D9_FMT_A16B16G16R16F = (D3DDDIFORMAT)113;
static constexpr D3DDDIFORMAT D3D9_FMT_R32F = (D3DDDIFORMAT)114;
static constexpr D3DDDIFORMAT D3D9_FMT_G32R32F = (D3DDDIFORMAT)115;
static constexpr D3DDDIFORMAT D3D9_FMT_A32B32G32R32F = (D3DDDIFORMAT)116;
static constexpr D3DDDIFORMAT D3D9_FMT_V8U8 = (D3DDDIFORMAT)60;
static constexpr D3DDDIFORMAT D3D9_FMT_Q8W8V8U8 = (D3DDDIFORMAT)63;
static constexpr D3DDDIFORMAT D3D9_FMT_V16U16 = (D3DDDIFORMAT)64;
static constexpr D3DDDIFORMAT D3D9_FMT_DXT1 = D3D9FourCC('D', 'X', 'T', '1');
static constexpr D3DDDIFORMAT D3D9_FMT_DXT2 = D3D9FourCC('D', 'X', 'T', '2');
static constexpr D3DDDIFORMAT D3D9_FMT_DXT3 = D3D9FourCC('D', 'X', 'T', '3');
static constexpr D3DDDIFORMAT D3D9_FMT_DXT4 = D3D9FourCC('D', 'X', 'T', '4');
static constexpr D3DDDIFORMAT D3D9_FMT_DXT5 = D3D9FourCC('D', 'X', 'T', '5');
static constexpr D3DDDIFORMAT D3D9_FMT_ATI1 = D3D9FourCC('A', 'T', 'I', '1');
static constexpr D3DDDIFORMAT D3D9_FMT_DF16 = D3D9FourCC('D', 'F', '1', '6');
static constexpr D3DDDIFORMAT D3D9_FMT_DF24 = D3D9FourCC('D', 'F', '2', '4');
static constexpr D3DDDIFORMAT D3D9_FMT_INTZ = D3D9FourCC('I', 'N', 'T', 'Z');
static constexpr D3DDDIFORMAT D3D9_FMT_AYUV = D3D9FourCC('A', 'Y', 'U', 'V');
static constexpr D3DDDIFORMAT D3D9_FMT_YUY2 = D3D9FourCC('Y', 'U', 'Y', '2');
static constexpr D3DDDI_POOL D3D9_POOL_STAGINGMEM = (D3DDDI_POOL)5;

static inline bool
D3D9ResourceUsesCpuBufferStorage(const D3D9Resource *resource)
{
   return resource &&
      (resource->flags.VertexBuffer || resource->flags.IndexBuffer) &&
      (resource->pool == D3DDDIPOOL_SYSTEMMEM ||
       resource->pool == D3D9_POOL_STAGINGMEM ||
       resource->flags.MightDrawFromLocked);
}

void D3D9Tracef(const char *format, ...);
void D3D9Warnf(const char *format, ...);
void D3D9WarnOncef(volatile LONG *logged, const char *format, ...);
D3D9Resource *D3D9CastResource(HANDLE resource);
D3D9SubResource *D3D9GetSubResource(HANDLE resource_handle,
                                    UINT subresource_index);
bool D3D9UploadSubResource(D3D9Device *device, D3D9Resource *resource,
                           UINT subresource_index);
D3D9Object *D3D9CreateObject(D3D9ObjectKind kind, size_t payload_size,
                             const void *payload);
bool D3D9CheckConstantRange(UINT reg, UINT count, UINT max_count);
void D3D9SetIdentityMatrix(D3DMATRIX *matrix);
void D3D9MatrixMultiply(D3DMATRIX *dst, const D3DMATRIX *a,
                        const D3DMATRIX *b);
void D3D9FillDeviceFuncs(D3DDDI_DEVICEFUNCS *funcs);

HRESULT APIENTRY D3D9SetPriority(HANDLE hDevice,
                                 const D3DDDIARG_SETPRIORITY *data);
HRESULT APIENTRY D3D9VolBlt(HANDLE hDevice,
                            const D3DDDIARG_VOLUMEBLT *data);
HRESULT APIENTRY D3D9BufBlt(HANDLE hDevice,
                            const D3DDDIARG_BUFFERBLT *data);
HRESULT APIENTRY D3D9TexBlt(HANDLE hDevice, const D3DDDIARG_TEXBLT *data);
HRESULT APIENTRY D3D9SetRenderTarget(HANDLE hDevice,
                                     const D3DDDIARG_SETRENDERTARGET *data);
HRESULT APIENTRY D3D9SetDepthStencil(HANDLE hDevice,
                                     const D3DDDIARG_SETDEPTHSTENCIL *data);
HRESULT APIENTRY D3D9CreateResource(HANDLE hDevice,
                                    D3DDDIARG_CREATERESOURCE *data);
HRESULT APIENTRY D3D9OpenResource(HANDLE hDevice,
                                  D3DDDIARG_OPENRESOURCE *data);
HRESULT APIENTRY D3D9Lock(HANDLE hDevice, D3DDDIARG_LOCK *data);
HRESULT APIENTRY D3D9Unlock(HANDLE hDevice, const D3DDDIARG_UNLOCK *data);
HRESULT APIENTRY D3D9LockAsync(HANDLE hDevice, D3DDDIARG_LOCKASYNC *data);
HRESULT APIENTRY D3D9UnlockAsync(HANDLE hDevice,
                                 const D3DDDIARG_UNLOCKASYNC *data);
HRESULT APIENTRY D3D9GetPitch(HANDLE hDevice, D3DDDIARG_GETPITCH *data);
HRESULT APIENTRY D3D9ResolveSharedResource(
   HANDLE hDevice, const D3DDDIARG_RESOLVESHAREDRESOURCE *data);
HRESULT APIENTRY D3D9QueryResourceResidency(
   HANDLE hDevice, const D3DDDIARG_QUERYRESOURCERESIDENCY *data);
HRESULT APIENTRY D3D9GetCaptureAllocationHandle(
   HANDLE hDevice, D3DDDIARG_GETCAPTUREALLOCATIONHANDLE *data);
HRESULT APIENTRY D3D9CaptureToSysMem(HANDLE hDevice,
                                     const D3DDDIARG_CAPTURETOSYSMEM *data);
HRESULT APIENTRY D3D9Blt(HANDLE hDevice, const D3DDDIARG_BLT *data);
HRESULT APIENTRY D3D9ColorFill(HANDLE hDevice,
                               const D3DDDIARG_COLORFILL *data);
HRESULT APIENTRY D3D9DepthFill(HANDLE hDevice,
                               const D3DDDIARG_DEPTHFILL *data);
HRESULT APIENTRY D3D9Present(HANDLE hDevice, const D3DDDIARG_PRESENT *data);
HRESULT APIENTRY D3D9Clear(HANDLE hDevice, const D3DDDIARG_CLEAR *data,
                           UINT rect_count, const RECT *rects);
HRESULT APIENTRY D3D9DestroyResource(HANDLE hDevice, HANDLE resource);
HRESULT APIENTRY D3D9DrawPrimitive(HANDLE hDevice,
                                   const D3DDDIARG_DRAWPRIMITIVE *data,
                                   const UINT *indices);
HRESULT APIENTRY D3D9DrawIndexedPrimitive(
   HANDLE hDevice, const D3DDDIARG_DRAWINDEXEDPRIMITIVE *data);
HRESULT APIENTRY D3D9DrawPrimitive2(HANDLE hDevice,
                                    const D3DDDIARG_DRAWPRIMITIVE2 *data);
HRESULT APIENTRY D3D9DrawIndexedPrimitive2(
   HANDLE hDevice, const D3DDDIARG_DRAWINDEXEDPRIMITIVE2 *data,
   UINT index_size, const void *indices, const UINT *index_remap);
void D3D9DestroyDrawState(D3D9Device *device);
