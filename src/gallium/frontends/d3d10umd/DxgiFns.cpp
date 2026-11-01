/**************************************************************************
 *
 * Copyright 2012-2021 VMware, Inc.
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE COPYRIGHT HOLDERS, AUTHORS AND/OR ITS SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 **************************************************************************/

/*
 * DxgiFns.cpp --
 *    DXGI related functions.
 */

#include <stdint.h>
#include <windows.h>
#include <string.h>
#include <winerror.h>
#include <winnt.h>

#include "DxgiFns.h"

#include <d3dkmthk.h>

#include "Format.h"
#include "Shader.h"
#include "State.h"

#include "Debug.h"

#include "frontend/winsys_handle.h"
#include "pipe/p_defines.h"
#include "pipe/p_state.h"
#include "util/u_inlines.h"
#include "gallium/winsys/yttrium/gdi/yttrium_gdi_public.h"

static const char *
dxgi_screen_name(Device *device)
{
   if (!device || !device->screen || !device->screen->get_name) {
      return "<unknown>";
   }

   const char *name = device->screen->get_name(device->screen);
   return name ? name : "<unknown>";
}

static bool
dxgi_is_yttrium_screen(Device *device)
{
   return strcmp(dxgi_screen_name(device), "yttrium") == 0;
}

static void
dxgi_sync_yttrium_primary_identity(Device *device, Resource *resource)
{
   if (!dxgi_is_yttrium_screen(device) || !resource || !resource->resource) {
      return;
   }

   yttrium_gdi_resource_set_primary_target(resource->resource,
                                           resource->yttrium_primary);
}

static bool
dxgi_is_optimal_composition_resource(Device *device, Resource *resource)
{
   return dxgi_is_yttrium_screen(device) && resource && resource->resource &&
      (resource->resource->bind & PIPE_BIND_DISPLAY_TARGET) &&
      yttrium_gdi_resource_is_optimal_shared(resource->resource);
}

static D3DKMT_HANDLE
dxgi_get_d3dkmt_allocation(Device *device, Resource *resource)
{
   struct winsys_handle whandle;

   if (!device || !device->screen || !device->screen->resource_get_handle ||
       !resource || !resource->resource)
      return 0;

   memset(&whandle, 0, sizeof(whandle));
   whandle.type = WINSYS_HANDLE_TYPE_D3DKMT_ALLOC;
   if (!device->screen->resource_get_handle(device->screen, device->pipe,
                                            resource->resource, &whandle, 0))
      return 0;

   return (D3DKMT_HANDLE)(uintptr_t)whandle.handle;
}

static void
dxgi_flush_frontbuffer(Device *device,
                       struct pipe_resource *resource,
                       unsigned level,
                       unsigned layer,
                       void *context_private)
{
   if (!device || !device->pipe || !device->screen ||
       !device->screen->flush_frontbuffer || !resource) {
      return;
   }

   /* The Yttrium screen hook owns its exact asynchronous publication ticket. */
   if (!dxgi_is_yttrium_screen(device))
      yttrium_gdi_flush_labeled(device->pipe, NULL, 0,
                                "DXGI flush-frontbuffer pre-flush");
   device->screen->flush_frontbuffer(device->screen,
                                     device->pipe,
                                     resource,
                                     level,
                                     layer,
                                     context_private,
                                     0,
                                     NULL);
}

/*
 * ----------------------------------------------------------------------
 *
 * _Present --
 *
 *    This is turned into kernel callbacks rather than directly emitted
 *    as fifo packets.
 *
 * ----------------------------------------------------------------------
 */
HRESULT APIENTRY
_Present(DXGI_DDI_ARG_PRESENT *pPresentData)
{

   LOG_ENTRYPOINT();

   struct Device *device = CastDevice(pPresentData->hDevice);
   Resource *pSrcResource = CastResource(pPresentData->hSurfaceToPresent);
   const bool yttrium_screen = dxgi_is_yttrium_screen(device);
   Resource *pDstResource = CastResource(pPresentData->hDstResource);
   D3DKMT_HANDLE src_alloc = 0;
   D3DKMT_HANDLE dst_alloc = 0;
   HRESULT present_result = S_OK;
   const D3DKMT_PRESENT *dxgi_present_context =
      pPresentData->pDXGIContext ?
         (const D3DKMT_PRESENT *)pPresentData->pDXGIContext : NULL;


   if (yttrium_screen) {
      src_alloc = dxgi_get_d3dkmt_allocation(device, pSrcResource);
      dst_alloc = dxgi_get_d3dkmt_allocation(device, pDstResource);
      yttrium_gdi_trace_dxgi_present(
         YTTRIUM_GDI_TRACE_DXGI_PRESENT_BEGIN,
         (uint64_t)(uintptr_t)pPresentData->hSurfaceToPresent,
         (uint64_t)(uintptr_t)pPresentData->hDstResource,
         src_alloc,
         dst_alloc,
         (uint64_t)(uintptr_t)pPresentData->pDXGIContext,
         dxgi_present_context ?
            (uint64_t)(uintptr_t)dxgi_present_context->hWindow : 0,
         dxgi_present_context ? (uint64_t)dxgi_present_context->hSource : 0,
         dxgi_present_context ?
            (uint64_t)dxgi_present_context->hDestination : 0,
         pPresentData->SrcSubResourceIndex,
         pPresentData->DstSubResourceIndex,
         pPresentData->Flags.Value,
         pPresentData->FlipInterval,
         dxgi_present_context ? dxgi_present_context->PresentCount : 0,
         0,
         0);
   }

   /* A runtime/KMD blt writes the allocation, not canonical optimal storage.
    * UMD GPU Blt below is different: it writes the canonical pipe image and
    * publishes its endpoint. Do not allow a reverse CPU writer here. */
   if (yttrium_screen && pDstResource && pDstResource->resource &&
       (yttrium_gdi_resource_is_optimal_present(pDstResource->resource) ||
        yttrium_gdi_resource_is_optimal_shared(pDstResource->resource))) {
      yttrium_gdi_user_logf("yttrium: ERROR optimal image Present destination rejected owner=Present component=d3d10umd reason=external-write-bypasses-canonical-storage action=fail-present\n");
      return DXGI_DDI_ERR_UNSUPPORTED;
   }

   if ((dxgi_is_optimal_composition_resource(device, pSrcResource) ||
        (yttrium_screen && pSrcResource &&
         yttrium_gdi_resource_is_optimal_present(pSrcResource->resource))) &&
       (pPresentData->SrcSubResourceIndex || pPresentData->DstSubResourceIndex ||
        pPresentData->hDstResource)) {
      yttrium_gdi_user_logf("yttrium: ERROR optimal image Present rejected owner=Present component=d3d10umd reason=only-one-whole-surface-qualified action=fail-present\n");
      return DXGI_DDI_ERR_UNSUPPORTED;
   }

   /* dxgi_flush_frontbuffer publishes this Yttrium Present asynchronously. */
   if (!yttrium_screen)
      yttrium_gdi_flush_labeled(device->pipe, NULL, 0,
                                "DXGI Present pre-flush");

   {
      struct gdikmt_present_info present_info;
      void *present_context = pPresentData->pDXGIContext;

      memset(&present_info, 0, sizeof(present_info));
      if (yttrium_screen) {
         present_info.magic = GDIKMT_PRESENT_INFO_MAGIC;
         present_info.version = 3;
         present_info.dxgi_context = pPresentData->pDXGIContext;
         present_info.hDstAllocation = dst_alloc;
         present_info.status = S_OK;
         /* A runtime primary and a real window are not sufficient: windowed
          * flip-model swapchains use redirected primaries too.  Only a real
          * non-redirected flip may bypass Windows presentation and publish
          * through the fullscreen scanout escape. */
         present_info.application_scanout =
            pSrcResource && pSrcResource->yttrium_primary &&
            dxgi_present_context && dxgi_present_context->hWindow != NULL &&
            dxgi_present_context->Flags.Flip &&
            !dxgi_present_context->Flags.RedirectedFlip &&
            !dxgi_present_context->Flags.RedirectedBlt;
         present_context = &present_info;
         yttrium_gdi_trace_dxgi_present(
            YTTRIUM_GDI_TRACE_DXGI_PRESENT_FLUSH_FRONTBUFFER,
            (uint64_t)(uintptr_t)pPresentData->hSurfaceToPresent,
            (uint64_t)(uintptr_t)pPresentData->hDstResource,
            src_alloc,
            dst_alloc,
            (uint64_t)(uintptr_t)pPresentData->pDXGIContext,
            dxgi_present_context ?
               (uint64_t)(uintptr_t)dxgi_present_context->hWindow : 0,
            dxgi_present_context ? (uint64_t)dxgi_present_context->hSource : 0,
            dxgi_present_context ?
               (uint64_t)dxgi_present_context->hDestination : 0,
            pPresentData->SrcSubResourceIndex,
            pPresentData->DstSubResourceIndex,
            pPresentData->Flags.Value,
            pPresentData->FlipInterval,
            dxgi_present_context ? dxgi_present_context->PresentCount : 0,
            0,
            0);
      }

      dxgi_flush_frontbuffer(device,
                             pSrcResource->resource,
                             0,
                             0,
                             present_context);
      if (yttrium_screen)
         present_result = present_info.status;
   }

   if (yttrium_screen) {
      yttrium_gdi_trace_dxgi_present(
         YTTRIUM_GDI_TRACE_DXGI_PRESENT_END,
         (uint64_t)(uintptr_t)pPresentData->hSurfaceToPresent,
         (uint64_t)(uintptr_t)pPresentData->hDstResource,
         src_alloc,
         dst_alloc,
         (uint64_t)(uintptr_t)pPresentData->pDXGIContext,
         dxgi_present_context ?
            (uint64_t)(uintptr_t)dxgi_present_context->hWindow : 0,
         dxgi_present_context ? (uint64_t)dxgi_present_context->hSource : 0,
         dxgi_present_context ?
            (uint64_t)dxgi_present_context->hDestination : 0,
         pPresentData->SrcSubResourceIndex,
         pPresentData->DstSubResourceIndex,
         pPresentData->Flags.Value,
         pPresentData->FlipInterval,
         dxgi_present_context ? dxgi_present_context->PresentCount : 0,
         0,
         0);
   }

   return present_result;
}


/*
 * ----------------------------------------------------------------------
 *
 * _GetGammaCaps --
 *
 *    Return gamma capabilities.
 *
 * ----------------------------------------------------------------------
 */

HRESULT APIENTRY
_GetGammaCaps( DXGI_DDI_ARG_GET_GAMMA_CONTROL_CAPS *GetCaps )
{
   LOG_ENTRYPOINT();

   DXGI_GAMMA_CONTROL_CAPABILITIES *pCaps;

   pCaps = GetCaps->pGammaCapabilities;

   pCaps->ScaleAndOffsetSupported = false;
   pCaps->MinConvertedValue = 0.0;
   pCaps->MaxConvertedValue = 1.0;
   pCaps->NumGammaControlPoints = 17;

   for (UINT i = 0; i < pCaps->NumGammaControlPoints; i++) {
      pCaps->ControlPointPositions[i] = (float)i / (float)(pCaps->NumGammaControlPoints - 1);
   }

   return S_OK;
}


/*
 * ----------------------------------------------------------------------
 *
 * _SetDisplayMode --
 *
 *    Set the resource that is used to scan out to the display.
 *
 * ----------------------------------------------------------------------
 */

HRESULT APIENTRY
_SetDisplayMode( DXGI_DDI_ARG_SETDISPLAYMODE *SetDisplayMode )
{
   LOG_ENTRYPOINT();
   
   Device* device = CastDevice(SetDisplayMode->hDevice);
   Resource *res = CastResource(SetDisplayMode->hResource);

   if (!res || !res->resource) {
      yttrium_gdi_trace_warnf("yttrium: dxgi SetDisplayMode failed owner=d3d10umd "
                   "resource=%p subresource=%u reason=invalid-resource\n",
                   (void *)SetDisplayMode->hResource,
                   SetDisplayMode->SubResourceIndex);
      return E_INVALIDARG;
   }

   if (dxgi_is_yttrium_screen(device) &&
       yttrium_gdi_resource_is_optimal_shared(res->resource)) {
      yttrium_gdi_user_logf("yttrium: ERROR optimal shared mode set rejected owner=SetDisplayMode component=d3d10umd reason=opaque-storage-is-not-scanout action=fail-mode-set\n");
      return DXGI_DDI_ERR_UNSUPPORTED;
   }


   if(!device->screen->resource_get_handle) {
      LOG_UNSUPPORTED_ENTRYPOINT();
      return DXGI_DDI_ERR_UNSUPPORTED;
   }

   struct winsys_handle handle;
   memset(&handle, 0, sizeof(handle));
   handle.type = WINSYS_HANDLE_TYPE_D3DKMT_ALLOC;
   if(!device->screen->resource_get_handle(device->screen, NULL, res->resource, &handle, 0)) {
      LOG_UNSUPPORTED_ENTRYPOINT();
      return E_FAIL;
   };

   const auto kmt_handle =
      static_cast<D3DKMT_HANDLE>(reinterpret_cast<uintptr_t>(handle.handle));
   NTSTATUS status =
      device->device.base.setDisplayMode(&device->device.base, kmt_handle);

   return (HRESULT)status;
}


/*
 * ----------------------------------------------------------------------
 *
 * _SetResourcePriority --
 *
 * ----------------------------------------------------------------------
 */

HRESULT APIENTRY
_SetResourcePriority( DXGI_DDI_ARG_SETRESOURCEPRIORITY *SetResourcePriority )
{
   LOG_ENTRYPOINT();

   /* ignore */

   return S_OK;
}


/*
 * ----------------------------------------------------------------------
 *
 * _QueryResourceResidency --
 *
 * ----------------------------------------------------------------------
 */

HRESULT APIENTRY
_QueryResourceResidency( DXGI_DDI_ARG_QUERYRESOURCERESIDENCY *QueryResourceResidency )
{
   LOG_ENTRYPOINT();

   for (UINT i = 0; i < QueryResourceResidency->Resources; ++i) {
      QueryResourceResidency->pStatus[i] = DXGI_DDI_RESIDENCY_FULLY_RESIDENT;
   }

   return S_OK;
}


/*
 * ----------------------------------------------------------------------
 *
 * _RotateResourceIdentities --
 *
 *    Rotate a list of resources by recreating their views with
 *    the updated rotations.
 *
 * ----------------------------------------------------------------------
 */

HRESULT APIENTRY
_RotateResourceIdentities(DXGI_DDI_ARG_ROTATE_RESOURCE_IDENTITIES *RotateResourceIdentities )
{
   LOG_ENTRYPOINT();

   Device *device = CastDevice(RotateResourceIdentities->hDevice);


   if (RotateResourceIdentities->Resources <= 1) {
      return S_OK;
   }
   UINT NumResources = RotateResourceIdentities->Resources;
   const DXGI_DDI_HRESOURCE *hResources = RotateResourceIdentities->pResources;

   if (dxgi_is_yttrium_screen(device)) {
      if (NumResources > DXGI_MAX_SWAP_CHAIN_BUFFERS) {
         yttrium_gdi_user_logf(
            "yttrium: ERROR: DXGI runtime-handle rotation rejected "
            "owner=d3d10umd-dxgi component=RotateResourceIdentities "
            "reason=resource-count-exceeds-DXGI-limit action=abort "
            "count=%u limit=%u\n",
            NumResources, DXGI_MAX_SWAP_CHAIN_BUFFERS);
         return E_INVALIDARG;
      }

      struct pipe_resource *resources[DXGI_MAX_SWAP_CHAIN_BUFFERS];

      for (UINT i = 0; i < NumResources; ++i)
         resources[i] = CastPipeResource(hResources[i]);

      const bool handles_rotated =
         yttrium_gdi_resource_rotate_runtime_handles(resources, NumResources);
      if (!handles_rotated)
         return E_FAIL;
   }

   bool framebuffer_rotated = false;
   for (UINT binding = 0; binding < device->fb.nr_cbufs; ++binding) {
      struct pipe_resource **texture = &device->fb.cbufs[binding].texture;
      for (UINT i = 0; i < NumResources; ++i) {
         if (*texture != CastPipeResource(hResources[i]))
            continue;

         pipe_resource_reference(
            texture, CastPipeResource(hResources[(i + 1) % NumResources]));
         framebuffer_rotated = true;
         break;
      }
   }

   if (device->fb.zsbuf.texture) {
      for (UINT i = 0; i < NumResources; ++i) {
         if (device->fb.zsbuf.texture != CastPipeResource(hResources[i]))
            continue;

         pipe_resource_reference(
            &device->fb.zsbuf.texture,
            CastPipeResource(hResources[(i + 1) % NumResources]));
         framebuffer_rotated = true;
         break;
      }
   }

   struct pipe_resource *firstResource = CastPipeResource(hResources[0]);

   for (UINT i = 0; i < (NumResources - 1); ++i) {
      Resource* resource = CastResource(hResources[i]);
      resource->resource = CastPipeResource(hResources[i + 1]);
   }

   Resource *lastResource = CastResource(hResources[NumResources - 1]);
   lastResource->resource = firstResource;

   if (framebuffer_rotated)
      device->pipe->set_framebuffer_state(device->pipe, &device->fb);

   if (dxgi_is_yttrium_screen(device)) {
      for (UINT i = 0; i < NumResources; ++i) {
         dxgi_sync_yttrium_primary_identity(device,
                                            CastResource(hResources[i]));
      }
   }

   device->shader_resource_views_dirty = true;


   return S_OK;
}


/*
 * ----------------------------------------------------------------------
 *
 * _Blt --
 *
 *    Do a blt between two subresources. Apply MSAA resolve, format
 *    conversion and stretching.
 *
 * ----------------------------------------------------------------------
 */

HRESULT APIENTRY
_Blt(DXGI_DDI_ARG_BLT *Blt)
{
   LOG_ENTRYPOINT();

   Device *device = CastDevice(Blt->hDevice);
   Resource *dst = CastResource(Blt->hDstResource);
   Resource *src = CastResource(Blt->hSrcResource);


   if (!device || !device->pipe || !dst || !src || !dst->resource ||
       !src->resource) {
      return E_INVALIDARG;
   }

   if (dxgi_is_optimal_composition_resource(device, src) ||
       dxgi_is_optimal_composition_resource(device, dst)) {
      yttrium_gdi_user_logf("yttrium: ERROR optimal composition Blt rejected owner=DXGI-Blt component=d3d10umd reason=legacy-and-reverse-composition-blits-not-qualified action=fail-blt\n");
      return DXGI_DDI_ERR_UNSUPPORTED;
   }

   struct pipe_context *pipe = device->pipe;
   struct pipe_resource *dst_resource = dst->resource;
   struct pipe_resource *src_resource = src->resource;
   dxgi_sync_yttrium_primary_identity(device, dst);

   unsigned dst_level = Blt->DstSubresource % (dst_resource->last_level + 1);
   unsigned dst_layer = Blt->DstSubresource / (dst_resource->last_level + 1);
   unsigned src_level = Blt->SrcSubresource % (src_resource->last_level + 1);
   unsigned src_layer = Blt->SrcSubresource / (src_resource->last_level + 1);

   struct pipe_box src_box = {};
   src_box.x = 0;
   src_box.y = 0;
   src_box.z = src_layer;
   src_box.width = Blt->DstRight > Blt->DstLeft ?
      Blt->DstRight - Blt->DstLeft : src_resource->width0;
   src_box.height = Blt->DstBottom > Blt->DstTop ?
      Blt->DstBottom - Blt->DstTop : src_resource->height0;
   src_box.depth = 1;

   pipe->resource_copy_region(pipe,
                              dst_resource,
                              dst_level,
                              Blt->DstLeft,
                              Blt->DstTop,
                              dst_layer,
                              src_resource,
                              src_level,
                              &src_box);

   if (Blt->Flags.Present) {
      dxgi_flush_frontbuffer(device, dst_resource, dst_level, dst_layer, NULL);
   }


   return S_OK;
}

#if SUPPORT_D3D11_1
HRESULT APIENTRY
_ResolveSharedResource(DXGI_DDI_ARG_RESOLVESHAREDRESOURCE *Resolve)
{
   LOG_ENTRYPOINT();

   if (!Resolve)
      return E_INVALIDARG;

   Device *device = CastDevice(Resolve->hDevice);
   Resource *resource = CastResource(Resolve->hResource);
   if (!device || !device->pipe || !resource || !resource->resource)
      return E_INVALIDARG;

   if (dxgi_is_yttrium_screen(device)) {
      if (!yttrium_gdi_flush_handoff(device->pipe,
                                      "DXGI resolve shared resource"))
         return DXGI_ERROR_DEVICE_REMOVED;
   }
   return S_OK;
}

HRESULT APIENTRY
_Blt1(DXGI_DDI_ARG_BLT1 *Blt)
{
   LOG_ENTRYPOINT();

   if (!Blt)
      return E_INVALIDARG;

   Device *device = CastDevice(Blt->hDevice);
   Resource *dst = CastResource(Blt->hDstResource);
   Resource *src = CastResource(Blt->hSrcResource);


   if (!device || !device->pipe || !dst || !src || !dst->resource ||
       !src->resource) {
      return E_INVALIDARG;
   }

   if (dxgi_is_optimal_composition_resource(device, src) ||
       dxgi_is_optimal_composition_resource(device, dst)) {
      yttrium_gdi_user_logf("yttrium: ERROR optimal composition Blt rejected owner=DXGI-Blt component=d3d10umd reason=legacy-and-reverse-composition-blits-not-qualified action=fail-blt\n");
      return DXGI_DDI_ERR_UNSUPPORTED;
   }

   struct pipe_context *pipe = device->pipe;
   struct pipe_resource *dst_resource = dst->resource;
   struct pipe_resource *src_resource = src->resource;
   dxgi_sync_yttrium_primary_identity(device, dst);

   unsigned dst_level = Blt->DstSubresource % (dst_resource->last_level + 1);
   unsigned dst_layer = Blt->DstSubresource / (dst_resource->last_level + 1);
   unsigned src_level = Blt->SrcSubresource % (src_resource->last_level + 1);
   unsigned src_layer = Blt->SrcSubresource / (src_resource->last_level + 1);

   unsigned width = Blt->SrcRight > Blt->SrcLeft ?
      Blt->SrcRight - Blt->SrcLeft :
      (Blt->DstRight > Blt->DstLeft ? Blt->DstRight - Blt->DstLeft :
       src_resource->width0);
   unsigned height = Blt->SrcBottom > Blt->SrcTop ?
      Blt->SrcBottom - Blt->SrcTop :
      (Blt->DstBottom > Blt->DstTop ? Blt->DstBottom - Blt->DstTop :
       src_resource->height0);

   struct pipe_box src_box = {};
   src_box.x = Blt->SrcLeft;
   src_box.y = Blt->SrcTop;
   src_box.z = src_layer;
   src_box.width = width;
   src_box.height = height;
   src_box.depth = 1;

   pipe->resource_copy_region(pipe,
                              dst_resource,
                              dst_level,
                              Blt->DstLeft,
                              Blt->DstTop,
                              dst_layer,
                              src_resource,
                              src_level,
                              &src_box);

   if (Blt->Flags.Present) {
      dxgi_flush_frontbuffer(device, dst_resource, dst_level, dst_layer, NULL);
   }


   return S_OK;
}

HRESULT APIENTRY
_OfferResources(DXGI_DDI_ARG_OFFERRESOURCES *Offer)
{
   LOG_ENTRYPOINT();

   if (!Offer)
      return E_INVALIDARG;

   return S_OK;
}

HRESULT APIENTRY
_ReclaimResources(DXGI_DDI_ARG_RECLAIMRESOURCES *Reclaim)
{
   LOG_ENTRYPOINT();

   if (!Reclaim)
      return E_INVALIDARG;

   if (Reclaim->pDiscarded) {
      for (UINT i = 0; i < Reclaim->Resources; ++i)
         Reclaim->pDiscarded[i] = FALSE;
   }

   return S_OK;
}

HRESULT APIENTRY
_GetMultiplaneOverlayCaps(DXGI_DDI_ARG_GETMULTIPLANEOVERLAYCAPS *Caps)
{
   LOG_ENTRYPOINT();

   if (!Caps)
      return E_INVALIDARG;

   memset(&Caps->MultiplaneOverlayCaps, 0, sizeof(Caps->MultiplaneOverlayCaps));
   return S_OK;
}

HRESULT APIENTRY
_GetMultiplaneOverlayGroupCaps(DXGI_DDI_ARG_GETMULTIPLANEOVERLAYGROUPCAPS *Caps)
{
   LOG_ENTRYPOINT();

   if (!Caps)
      return E_INVALIDARG;

   memset(&Caps->MultiplaneOverlayGroupCaps, 0, sizeof(Caps->MultiplaneOverlayGroupCaps));
   return S_OK;
}

HRESULT APIENTRY
_PresentMultiplaneOverlay(DXGI_DDI_ARG_PRESENTMULTIPLANEOVERLAY *Present)
{
   LOG_ENTRYPOINT();

   return E_NOTIMPL;
}

HRESULT APIENTRY
_Present1(DXGI_DDI_ARG_PRESENT1 *Present)
{
   LOG_ENTRYPOINT();

   if (!Present || !Present->phSurfacesToPresent ||
       !Present->SurfacesToPresent) {
      return E_INVALIDARG;
   }

   Device *device = CastDevice(Present->hDevice);
   const UINT selected_source = Present->SurfacesToPresent - 1;
   if (dxgi_is_yttrium_screen(device)) {
      Resource *destination = CastResource(Present->hDstResource);
      if (destination && destination->resource &&
          (yttrium_gdi_resource_is_optimal_present(destination->resource) ||
           yttrium_gdi_resource_is_optimal_shared(destination->resource))) {
         yttrium_gdi_user_logf("yttrium: ERROR optimal image Present1 destination rejected owner=Present1 component=d3d10umd reason=external-write-bypasses-canonical-storage action=fail-present\n");
         return DXGI_DDI_ERR_UNSUPPORTED;
      }
      bool optimal_batch = false;
      if (Present->SurfacesToPresent > 1) {
         for (UINT i = 0; i < Present->SurfacesToPresent; ++i) {
            Resource *resource = CastResource(Present->phSurfacesToPresent[i].hSurface);
            if (resource && resource->resource &&
                (yttrium_gdi_resource_is_optimal_shared(resource->resource) ||
                 yttrium_gdi_resource_is_optimal_present(resource->resource))) {
               optimal_batch = true;
               break;
            }
         }
      }
      /* Present1 releases every source in one ownership batch. Canonical
       * optimal-shared images need no per-resource publication copy; the
       * ordered Present flush releases all acquired external images. Mixed
       * optimal batches must first qualify every role's publication. Legacy-
       * only arrays retain their existing publication policy. */
      for (UINT i = 0; i < Present->SurfacesToPresent; ++i) {
         Resource *resource = CastResource(Present->phSurfacesToPresent[i].hSurface);
         const bool shared = resource && resource->resource &&
            yttrium_gdi_resource_is_optimal_shared(resource->resource);
         const bool endpoint = resource && resource->resource &&
            yttrium_gdi_resource_is_optimal_present(resource->resource);
         const char *reason = NULL;
         HRESULT failure = DXGI_DDI_ERR_UNSUPPORTED;
         if (!resource || !resource->resource) {
            reason = "invalid-source-resource";
            failure = E_INVALIDARG;
         } else if (optimal_batch && !shared) {
            reason = "multi-source-requires-canonical-optimal-shared";
         } else if ((shared || endpoint) &&
                    Present->phSurfacesToPresent[i].SubResourceIndex) {
            reason = "source-subresource-not-qualified";
         } else if ((shared || endpoint) && Present->hDstResource) {
            reason = "explicit-destination-not-qualified";
         } else if ((shared || endpoint) && Present->DstSubResourceIndex) {
            reason = "destination-subresource-not-qualified";
         }
         if (reason) {
            yttrium_gdi_user_logf("yttrium: ERROR optimal image Present1 rejected owner=Present1 component=d3d10umd reason=%s sources=%u selected_index=%u source_index=%u resource=%p source_subresource=%u shared=%u endpoint=%u destination=%p destination_subresource=%u flags=0x%x flip_interval=%u dxgi_context=%p action=fail-present\n",
                                 reason, Present->SurfacesToPresent,
                                 selected_source, i,
                                 (void *)Present->phSurfacesToPresent[i].hSurface,
                                 Present->phSurfacesToPresent[i].SubResourceIndex,
                                 shared, endpoint, (void *)Present->hDstResource,
                                 Present->DstSubResourceIndex, Present->Flags.Value,
                                 Present->FlipInterval, Present->pDXGIContext);
            return failure;
         }
      }
   }

   /* DXGI carries the whole ownership batch in its opaque context. Its single
    * Present callback must translate the LAST source, not issue one callback
    * per surface. Keep that context and the presentation flags unchanged. */
   DXGI_DDI_ARG_PRESENT legacy = {};
   legacy.hDevice = Present->hDevice;
   legacy.hSurfaceToPresent = Present->phSurfacesToPresent[selected_source].hSurface;
   legacy.SrcSubResourceIndex =
      Present->phSurfacesToPresent[selected_source].SubResourceIndex;
   legacy.hDstResource = Present->hDstResource;
   legacy.DstSubResourceIndex = Present->DstSubResourceIndex;
   legacy.pDXGIContext = Present->pDXGIContext;
   legacy.Flags = Present->Flags;
   legacy.FlipInterval = Present->FlipInterval;

   return _Present(&legacy);
}

HRESULT APIENTRY
_CheckPresentDurationSupport(DXGI_DDI_ARG_CHECKPRESENTDURATIONSUPPORT *Support)
{
   LOG_ENTRYPOINT();

   if (!Support)
      return E_INVALIDARG;

   Support->ClosestSmallerDuration = 0;
   Support->ClosestLargerDuration = 0;
   return S_OK;
}

HRESULT APIENTRY
_DxgiReserved(void *Data)
{
   LOG_ENTRYPOINT();

   return E_NOTIMPL;
}
#endif
