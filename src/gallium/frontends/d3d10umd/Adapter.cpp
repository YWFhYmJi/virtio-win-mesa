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
 * Adpater.cpp --
 *    Driver entry point.
 */


#include "DriverIncludes.h"
#include "Device.h"
#include "State.h"

#include "Debug.h"

#include "gallium/winsys/yttrium/gdi/yttrium_gdi_public.h"

#include "util/u_memory.h"


static HRESULT APIENTRY CloseAdapter(D3D10DDI_HADAPTER hAdapter);

static unsigned long numAdapters = 0;
#if 0
static unsigned long memdbg_no = 0;
#endif

#if SUPPORT_D3D11_1
static bool
ExposeD3D11_1()
{
   return yttrium_gdi_debug_get_bool_option(
      "D3D10UMD_YTTRIUM_ENABLE_FL11_1", true);
}
#endif

/*
 * ----------------------------------------------------------------------
 *
 * OpenAdapterCommon --
 *
 *    Common code for OpenAdapter10 and OpenAdapter10_2
 *
 * ----------------------------------------------------------------------
 */


static HRESULT
OpenAdapterCommon(__inout D3D10DDIARG_OPENADAPTER *pOpenData)   // IN
{
#if 0
   if (numAdapters == 0) {
      memdbg_no = debug_memory_begin();
   }
#endif
   ++numAdapters;

   Adapter *pAdaptor = (Adapter *)calloc(sizeof *pAdaptor, 1);
   if (!pAdaptor) {
      --numAdapters;
      return E_OUTOFMEMORY;
   }

   pOpenData->hAdapter.pDrvPrivate = pAdaptor;

   pOpenData->pAdapterFuncs->pfnCalcPrivateDeviceSize = CalcPrivateDeviceSize;
   pOpenData->pAdapterFuncs->pfnCreateDevice = CreateDevice;
   pOpenData->pAdapterFuncs->pfnCloseAdapter = CloseAdapter;

   pAdaptor->AdapterCallbacks = *pOpenData->pAdapterCallbacks;
   pAdaptor->hRTAdapter = pOpenData->hRTAdapter.handle;

   return S_OK;
}


/*
 * ----------------------------------------------------------------------
 *
 * OpenAdapter10 --
 *
 *    The OpenAdapter10 function creates a graphics adapter object
 *    that is referenced in subsequent calls.
 *
 * ----------------------------------------------------------------------
 */


EXTERN_C HRESULT APIENTRY
OpenAdapter10(__inout D3D10DDIARG_OPENADAPTER *pOpenData)   // IN
{
   LOG_ENTRYPOINT();

   /*
    * This is checked here and not on the common code because MSDN docs
    * state that it should be ignored on OpenAdapter10_2.
    */
   switch (pOpenData->Interface) {
   case D3D10_0_DDI_INTERFACE_VERSION:
   case D3D10_0_x_DDI_INTERFACE_VERSION:
   case D3D10_0_7_DDI_INTERFACE_VERSION:
#if SUPPORT_D3D10_1
   case D3D10_1_DDI_INTERFACE_VERSION:
   case D3D10_1_x_DDI_INTERFACE_VERSION:
   case D3D10_1_7_DDI_INTERFACE_VERSION:
#endif
#if SUPPORT_D3D11
   case D3D11_0_DDI_INTERFACE_VERSION:
   case D3D11_0_7_DDI_INTERFACE_VERSION:
      break;
#if SUPPORT_D3D11_1
   case D3D11_1_DDI_INTERFACE_VERSION:
      if (!ExposeD3D11_1())
         return E_FAIL;
      break;
#endif
#if SUPPORT_D3D_WDDM1_3
   case D3DWDDM1_3_DDI_INTERFACE_VERSION:
      if (!ExposeD3D11_1())
         return E_FAIL;
      break;
#endif
#endif
      break;
   default:
      if (0) {
         DebugPrintf("%s: unsupported interface version 0x%08x\n",
                     __func__, pOpenData->Interface);
      }
      return E_FAIL;
   }

   return OpenAdapterCommon(pOpenData);
}


static const UINT64
SupportedDDIInterfaceVersionsBase[] = {
   D3D10_0_DDI_SUPPORTED,
   D3D10_0_x_DDI_SUPPORTED,
   D3D10_0_7_DDI_SUPPORTED,
#if SUPPORT_D3D10_1
   D3D10_1_DDI_SUPPORTED,
   D3D10_1_x_DDI_SUPPORTED,
   D3D10_1_7_DDI_SUPPORTED,
#endif
#if SUPPORT_D3D11
   D3D11_0_DDI_SUPPORTED,
   D3D11_0_7_DDI_SUPPORTED,
#endif
};

#if SUPPORT_D3D11_1
static const UINT64
SupportedDDIInterfaceVersions11_1[] = {
#if SUPPORT_D3D11_1
   D3D11_1_DDI_SUPPORTED,
#endif
#if SUPPORT_D3D_WDDM1_3
   D3DWDDM1_3_DDI_SUPPORTED,
#endif
};
#endif


/*
 * ----------------------------------------------------------------------
 *
 * GetSupportedVersions --
 *
 *    Return a list of interface versions supported by the graphics
 *    adapter.
 *
 * ----------------------------------------------------------------------
 */

static HRESULT APIENTRY
GetSupportedVersions(D3D10DDI_HADAPTER hAdapter,
                     UINT32 *puEntries,
                     UINT64 *pSupportedDDIInterfaceVersions)
{
   LOG_ENTRYPOINT();

   UINT32 entry_count = ARRAYSIZE(SupportedDDIInterfaceVersionsBase);
#if SUPPORT_D3D11_1
   if (ExposeD3D11_1())
      entry_count += ARRAYSIZE(SupportedDDIInterfaceVersions11_1);
#endif

   if (pSupportedDDIInterfaceVersions &&
       *puEntries < entry_count) {
      return E_OUTOFMEMORY;
   }

   *puEntries = entry_count;

   if (pSupportedDDIInterfaceVersions) {
      memcpy(pSupportedDDIInterfaceVersions,
             SupportedDDIInterfaceVersionsBase,
             sizeof SupportedDDIInterfaceVersionsBase);
#if SUPPORT_D3D11_1
      if (ExposeD3D11_1()) {
         memcpy(pSupportedDDIInterfaceVersions +
                   ARRAYSIZE(SupportedDDIInterfaceVersionsBase),
                SupportedDDIInterfaceVersions11_1,
                sizeof SupportedDDIInterfaceVersions11_1);
      }
#endif
   }

   return S_OK;
}


/*
 * ----------------------------------------------------------------------
 *
 * GetCaps --
 *
 *    Return the capabilities of the graphics adapter.
 *
 * ----------------------------------------------------------------------
 */

static HRESULT APIENTRY
GetCaps(D3D10DDI_HADAPTER hAdapter,
        const D3D10_2DDIARG_GETCAPS *pData)
{
   LOG_ENTRYPOINT();
   memset(pData->pData, 0, pData->DataSize);
   switch (pData->Type) {
#if SUPPORT_D3D11
   case D3D11DDICAPS_3DPIPELINESUPPORT:
      if (pData->DataSize >= sizeof(D3D11DDI_3DPIPELINESUPPORT_CAPS)) {
         D3D11DDI_3DPIPELINESUPPORT_CAPS *caps =
            (D3D11DDI_3DPIPELINESUPPORT_CAPS *)pData->pData;
         UINT pipeline_caps =
            D3D11DDI_ENCODE_3DPIPELINESUPPORT_CAP(
               D3D11DDI_3DPIPELINELEVEL_10_0) |
            D3D11DDI_ENCODE_3DPIPELINESUPPORT_CAP(
               D3D11DDI_3DPIPELINELEVEL_10_1) |
            D3D11DDI_ENCODE_3DPIPELINESUPPORT_CAP(
               D3D11DDI_3DPIPELINELEVEL_11_0);
#if SUPPORT_D3D11_1
         if (ExposeD3D11_1()) {
            pipeline_caps |= D3D11DDI_ENCODE_3DPIPELINESUPPORT_CAP(
               D3D11_1DDI_3DPIPELINELEVEL_11_1);
         }
#endif
         caps->Caps = pipeline_caps;
      }
      break;
   case D3D11DDICAPS_THREADING:
      break;
   case D3D11DDICAPS_SHADER:
      if (pData->DataSize >= sizeof(D3D11DDI_SHADER_CAPS)) {
         D3D11DDI_SHADER_CAPS *caps =
            (D3D11DDI_SHADER_CAPS *)pData->pData;
         caps->Caps =
            D3D11DDICAPS_SHADER_COMPUTE_PLUS_RAW_AND_STRUCTURED_BUFFERS_IN_SHADER_4_X;
      }
      break;
#if SUPPORT_D3D11_1
   case D3D11_1DDICAPS_D3D11_OPTIONS:
      if (pData->DataSize >= sizeof(D3D11_1DDI_D3D11_OPTIONS_DATA)) {
         D3D11_1DDI_D3D11_OPTIONS_DATA *caps =
            (D3D11_1DDI_D3D11_OPTIONS_DATA *)pData->pData;
         caps->OutputMergerLogicOp = TRUE;
      }
      break;
#endif
#if SUPPORT_D3D_WDDM1_3
   case D3DWDDM1_3DDICAPS_D3D11_OPTIONS1:
      if (pData->DataSize >= sizeof(D3DWDDM1_3DDI_D3D11_OPTIONS_DATA1)) {
         D3DWDDM1_3DDI_D3D11_OPTIONS_DATA1 *caps =
            (D3DWDDM1_3DDI_D3D11_OPTIONS_DATA1 *)pData->pData;
         caps->TiledResourcesSupportFlags = 0;
      }
      break;
#endif
#endif
   default:
      break;
   }
   return S_OK;
}


/*
 * ----------------------------------------------------------------------
 *
 * OpenAdapter10_2 --
 *
 *    The OpenAdapter10_2 function creates a graphics adapter object
 *    that is referenced in subsequent calls.
 *
 * ----------------------------------------------------------------------
 */


EXTERN_C HRESULT APIENTRY
OpenAdapter10_2(__inout D3D10DDIARG_OPENADAPTER *pOpenData)   // IN
{
   LOG_ENTRYPOINT();

   HRESULT hr = OpenAdapterCommon(pOpenData);

   if (SUCCEEDED(hr)) {
      pOpenData->pAdapterFuncs_2->pfnGetSupportedVersions = GetSupportedVersions;
      pOpenData->pAdapterFuncs_2->pfnGetCaps = GetCaps;
   }

   return hr;
}


/*
 * ----------------------------------------------------------------------
 *
 * CloseAdapter --
 *
 *    The CloseAdapter function releases resources for a
 *    graphics adapter object.
 *
 * ----------------------------------------------------------------------
 */

HRESULT APIENTRY
CloseAdapter(D3D10DDI_HADAPTER hAdapter)  // IN
{
   LOG_ENTRYPOINT();

   Adapter *pAdapter = CastAdapter(hAdapter);
   free(pAdapter);

   --numAdapters;
#if 0
   if (numAdapters == 0) {
      debug_memory_end(memdbg_no);
   }
#endif

   return S_OK;
}
