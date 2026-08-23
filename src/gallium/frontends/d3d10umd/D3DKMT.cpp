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
 * D3DKMT.cpp --
 *    Implement kernel mode thunks, so that this can be loaded as a
 *    software DLL (D3D_DRIVER_TYPE_SOFTWARE).
 */


#include "DriverIncludes.h"
#include "gallium/winsys/yttrium/gdi/yttrium_gdi_public.h"
#include "virtio/wddm/viogpu_wddm_driver.h"

#ifndef STATUS_NOT_IMPLEMENTED
#define STATUS_NOT_IMPLEMENTED 0xC0000002
#endif

#ifndef STATUS_INVALID_PARAMETER
#define STATUS_INVALID_PARAMETER 0xC000000D
#endif

struct D3DKMTCallbacks {
   HMODULE gdi32;
   PFND3DKMT_QUERYADAPTERINFO QueryAdapterInfo;
   PFND3DKMT_GETDISPLAYMODELIST GetDisplayModeList;
   PFND3DKMT_SETDISPLAYMODE SetDisplayMode;
   PFND3DKMT_GETMULTISAMPLEMETHODLIST GetMultisampleMethodList;
   PFND3DKMT_GETRUNTIMEDATA GetRuntimeData;
   PFND3DKMT_ESCAPE Escape;
   PFND3DKMT_RENDER Render;
   PFND3DKMT_CREATECONTEXT CreateContext;
   PFND3DKMT_DESTROYCONTEXT DestroyContext;
   PFND3DKMT_CREATEALLOCATION CreateAllocation;
   PFND3DKMT_DESTROYALLOCATION DestroyAllocation;
   PFND3DKMT_LOCK Lock;
   PFND3DKMT_UNLOCK Unlock;
   PFND3DKMT_QUERYRESOURCEINFO QueryResourceInfo;
   PFND3DKMT_OPENRESOURCE OpenResource;
   PFND3DKMT_CREATEDEVICE CreateDevice;
   PFND3DKMT_DESTROYDEVICE DestroyDevice;
   PFND3DKMT_OPENADAPTERFROMHDC OpenAdapterFromHdc;
   PFND3DKMT_OPENADAPTERFROMGDIDISPLAYNAME OpenAdapterFromGdiDisplayName;
   PFND3DKMT_OPENADAPTERFROMDEVICENAME OpenAdapterFromDeviceName;
   PFND3DKMT_CLOSEADAPTER CloseAdapter;
   PFND3DKMT_PRESENT Present;
   PFND3DKMT_SIGNALSYNCHRONIZATIONOBJECT2 SignalSynchronizationObject2;
   PFND3DKMT_WAITFORSYNCHRONIZATIONOBJECT2 WaitForSynchronizationObject2;
   PFND3DKMT_WAITFORVERTICALBLANKEVENT WaitForVerticalBlankEvent;
   PFND3DKMT_GETDEVICESTATE GetDeviceState;
};

static D3DKMTCallbacks g_kmt;

static bool
D3DKMTIsKnownViogpuEscapeType(USHORT type)
{
   switch (type) {
   case VIOGPU_GET_DEVICE_ID:
   case VIOGPU_GET_CUSTOM_RESOLUTION:
   case VIOGPU_GET_CAPS:
   case VIOGPU_RES_INFO:
   case VIOGPU_RES_BUSY:
   case VIOGPU_RES_MAP_BLOB:
   case VIOGPU_RES_UNMAP_BLOB:
   case VIOGPU_RES_CREATE_BLOB:
   case VIOGPU_RES_SET_SCANOUT_BLOB:
   case VIOGPU_RES_ATTACH_WAIT:
   case VIOGPU_CTX_INIT:
   case VIOGPU_SUBMIT_CMD:
      return true;
   default:
      return false;
   }
}

static void
D3DKMTWarnInvalidViogpuEscape(CONST D3DKMT_ESCAPE *pData)
{
   static volatile LONG warning_count;
   LONG count = InterlockedIncrement(&warning_count);
   if (count > 8)
      return;

   const void *private_data = pData ? pData->pPrivateDriverData : nullptr;
   const UINT private_size = pData ? pData->PrivateDriverDataSize : 0;
   USHORT type = 0xffff;
   USHORT data_length = 0xffff;
   unsigned char first_bytes[16] = {0};

   if (private_data) {
      const unsigned char *bytes = (const unsigned char *)private_data;
      const UINT copy = private_size < sizeof(first_bytes) ?
         private_size : (UINT)sizeof(first_bytes);
      memcpy(first_bytes, bytes, copy);
      if (private_size >= sizeof(type))
         memcpy(&type, bytes, sizeof(type));
      if (private_size >= sizeof(type) + sizeof(data_length))
         memcpy(&data_length, bytes + sizeof(type), sizeof(data_length));
   }

   yttrium_gdi_trace_warnf(
      "yttrium: UMD invalid VIOGPU escape source=d3dkmt hAdapter=0x%lx hDevice=0x%lx hContext=0x%lx private=%p private_size=%u type=0x%x data_length=%u pid=%lu tid=%lu bytes=%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
      pData ? (unsigned long)pData->hAdapter : 0,
      pData ? (unsigned long)pData->hDevice : 0,
      pData ? (unsigned long)pData->hContext : 0,
      private_data, private_size, type, data_length,
      GetCurrentProcessId(), GetCurrentThreadId(),
      first_bytes[0], first_bytes[1], first_bytes[2], first_bytes[3],
      first_bytes[4], first_bytes[5], first_bytes[6], first_bytes[7],
      first_bytes[8], first_bytes[9], first_bytes[10], first_bytes[11],
      first_bytes[12], first_bytes[13], first_bytes[14], first_bytes[15]);
}

static void
D3DKMTCheckViogpuEscape(CONST D3DKMT_ESCAPE *pData)
{
   const UINT header_size = sizeof(USHORT) + sizeof(USHORT);
   if (!pData) {
      D3DKMTWarnInvalidViogpuEscape(pData);
      return;
   }

   if (pData->Type != D3DKMT_ESCAPE_DRIVERPRIVATE)
      return;

   if (!pData->pPrivateDriverData ||
       pData->PrivateDriverDataSize < header_size) {
      D3DKMTWarnInvalidViogpuEscape(pData);
      return;
   }

   const VIOGPU_ESCAPE *escape =
      (const VIOGPU_ESCAPE *)pData->pPrivateDriverData;
   if (!D3DKMTIsKnownViogpuEscapeType(escape->Type))
      D3DKMTWarnInvalidViogpuEscape(pData);
}

static FARPROC
LoadKmtProc(const char *name)
{
   if (!g_kmt.gdi32)
      g_kmt.gdi32 = LoadLibraryA("gdi32.dll");
   return g_kmt.gdi32 ? GetProcAddress(g_kmt.gdi32, name) : nullptr;
}

static void
LoadKmtCallbacks()
{
   if (g_kmt.QueryAdapterInfo)
      return;

   g_kmt.QueryAdapterInfo =
      (PFND3DKMT_QUERYADAPTERINFO)LoadKmtProc("D3DKMTQueryAdapterInfo");
   g_kmt.GetDisplayModeList =
      (PFND3DKMT_GETDISPLAYMODELIST)LoadKmtProc("D3DKMTGetDisplayModeList");
   g_kmt.SetDisplayMode =
      (PFND3DKMT_SETDISPLAYMODE)LoadKmtProc("D3DKMTSetDisplayMode");
   g_kmt.GetMultisampleMethodList =
      (PFND3DKMT_GETMULTISAMPLEMETHODLIST)
         LoadKmtProc("D3DKMTGetMultisampleMethodList");
   g_kmt.GetRuntimeData =
      (PFND3DKMT_GETRUNTIMEDATA)LoadKmtProc("D3DKMTGetRuntimeData");
   g_kmt.Escape = (PFND3DKMT_ESCAPE)LoadKmtProc("D3DKMTEscape");
   g_kmt.Render = (PFND3DKMT_RENDER)LoadKmtProc("D3DKMTRender");
   g_kmt.CreateContext =
      (PFND3DKMT_CREATECONTEXT)LoadKmtProc("D3DKMTCreateContext");
   g_kmt.DestroyContext =
      (PFND3DKMT_DESTROYCONTEXT)LoadKmtProc("D3DKMTDestroyContext");
   g_kmt.CreateAllocation =
      (PFND3DKMT_CREATEALLOCATION)LoadKmtProc("D3DKMTCreateAllocation");
   g_kmt.DestroyAllocation =
      (PFND3DKMT_DESTROYALLOCATION)LoadKmtProc("D3DKMTDestroyAllocation");
   g_kmt.Lock = (PFND3DKMT_LOCK)LoadKmtProc("D3DKMTLock");
   g_kmt.Unlock = (PFND3DKMT_UNLOCK)LoadKmtProc("D3DKMTUnlock");
   g_kmt.QueryResourceInfo =
      (PFND3DKMT_QUERYRESOURCEINFO)LoadKmtProc("D3DKMTQueryResourceInfo");
   g_kmt.OpenResource =
      (PFND3DKMT_OPENRESOURCE)LoadKmtProc("D3DKMTOpenResource");
   g_kmt.CreateDevice =
      (PFND3DKMT_CREATEDEVICE)LoadKmtProc("D3DKMTCreateDevice");
   g_kmt.DestroyDevice =
      (PFND3DKMT_DESTROYDEVICE)LoadKmtProc("D3DKMTDestroyDevice");
   g_kmt.OpenAdapterFromHdc =
      (PFND3DKMT_OPENADAPTERFROMHDC)LoadKmtProc("D3DKMTOpenAdapterFromHdc");
   g_kmt.OpenAdapterFromGdiDisplayName =
      (PFND3DKMT_OPENADAPTERFROMGDIDISPLAYNAME)
         LoadKmtProc("D3DKMTOpenAdapterFromGdiDisplayName");
   g_kmt.OpenAdapterFromDeviceName =
      (PFND3DKMT_OPENADAPTERFROMDEVICENAME)
         LoadKmtProc("D3DKMTOpenAdapterFromDeviceName");
   g_kmt.CloseAdapter =
      (PFND3DKMT_CLOSEADAPTER)LoadKmtProc("D3DKMTCloseAdapter");
   g_kmt.Present = (PFND3DKMT_PRESENT)LoadKmtProc("D3DKMTPresent");
   g_kmt.SignalSynchronizationObject2 =
      (PFND3DKMT_SIGNALSYNCHRONIZATIONOBJECT2)
         LoadKmtProc("D3DKMTSignalSynchronizationObject2");
   g_kmt.WaitForSynchronizationObject2 =
      (PFND3DKMT_WAITFORSYNCHRONIZATIONOBJECT2)
         LoadKmtProc("D3DKMTWaitForSynchronizationObject2");
   g_kmt.WaitForVerticalBlankEvent =
      (PFND3DKMT_WAITFORVERTICALBLANKEVENT)
         LoadKmtProc("D3DKMTWaitForVerticalBlankEvent");
   g_kmt.GetDeviceState =
      (PFND3DKMT_GETDEVICESTATE)LoadKmtProc("D3DKMTGetDeviceState");
}

EXTERN_C NTSTATUS APIENTRY
D3DKMTCreateAllocation(D3DKMT_CREATEALLOCATION *pData)
{
   LoadKmtCallbacks();
   return g_kmt.CreateAllocation ? g_kmt.CreateAllocation(pData) :
                                   STATUS_NOT_IMPLEMENTED;
}


EXTERN_C NTSTATUS APIENTRY
D3DKMTCreateAllocation2(D3DKMT_CREATEALLOCATION *pData)
{
   LoadKmtCallbacks();
   return g_kmt.CreateAllocation ? g_kmt.CreateAllocation(pData) :
                                   STATUS_NOT_IMPLEMENTED;
}


EXTERN_C NTSTATUS APIENTRY
D3DKMTQueryResourceInfo(D3DKMT_QUERYRESOURCEINFO *pData)
{
   LoadKmtCallbacks();
   return g_kmt.QueryResourceInfo ? g_kmt.QueryResourceInfo(pData) :
                                    STATUS_NOT_IMPLEMENTED;
}


EXTERN_C NTSTATUS APIENTRY
D3DKMTOpenResource(D3DKMT_OPENRESOURCE *pData)
{
   LoadKmtCallbacks();
   return g_kmt.OpenResource ? g_kmt.OpenResource(pData) :
                               STATUS_NOT_IMPLEMENTED;
}


EXTERN_C NTSTATUS APIENTRY
D3DKMTOpenResource2(D3DKMT_OPENRESOURCE *pData)
{
   LOG_UNSUPPORTED_ENTRYPOINT();
   return STATUS_NOT_IMPLEMENTED;
}


EXTERN_C NTSTATUS APIENTRY
D3DKMTDestroyAllocation(CONST D3DKMT_DESTROYALLOCATION *pData)
{
   LoadKmtCallbacks();
   return g_kmt.DestroyAllocation ? g_kmt.DestroyAllocation(pData) :
                                    STATUS_NOT_IMPLEMENTED;
}


EXTERN_C NTSTATUS APIENTRY
D3DKMTSetAllocationPriority(CONST D3DKMT_SETALLOCATIONPRIORITY *pData)
{
   LOG_UNSUPPORTED_ENTRYPOINT();
   return STATUS_NOT_IMPLEMENTED;
}


EXTERN_C NTSTATUS APIENTRY
D3DKMTQueryAllocationResidency(CONST D3DKMT_QUERYALLOCATIONRESIDENCY *pData)
{
   LOG_UNSUPPORTED_ENTRYPOINT();
   return STATUS_NOT_IMPLEMENTED;
}


EXTERN_C NTSTATUS APIENTRY
D3DKMTCreateDevice(D3DKMT_CREATEDEVICE *pData)
{
   LoadKmtCallbacks();
   return g_kmt.CreateDevice ? g_kmt.CreateDevice(pData) :
                               STATUS_NOT_IMPLEMENTED;
}


EXTERN_C NTSTATUS APIENTRY
D3DKMTDestroyDevice(CONST D3DKMT_DESTROYDEVICE *pData)
{
   LoadKmtCallbacks();
   return g_kmt.DestroyDevice ? g_kmt.DestroyDevice(pData) :
                                STATUS_NOT_IMPLEMENTED;
}


EXTERN_C NTSTATUS APIENTRY
D3DKMTCreateContext(D3DKMT_CREATECONTEXT *pData)
{
   LoadKmtCallbacks();
   return g_kmt.CreateContext ? g_kmt.CreateContext(pData) :
                                STATUS_NOT_IMPLEMENTED;
}


EXTERN_C NTSTATUS APIENTRY
D3DKMTDestroyContext(CONST D3DKMT_DESTROYCONTEXT *pData)
{
   LoadKmtCallbacks();
   return g_kmt.DestroyContext ? g_kmt.DestroyContext(pData) :
                                 STATUS_NOT_IMPLEMENTED;
}


EXTERN_C NTSTATUS APIENTRY
D3DKMTCreateSynchronizationObject(D3DKMT_CREATESYNCHRONIZATIONOBJECT *pData)
{
   LOG_UNSUPPORTED_ENTRYPOINT();
   return STATUS_NOT_IMPLEMENTED;
}


EXTERN_C NTSTATUS APIENTRY
D3DKMTCreateSynchronizationObject2(D3DKMT_CREATESYNCHRONIZATIONOBJECT2 *pData)
{
   LOG_UNSUPPORTED_ENTRYPOINT();
   return STATUS_NOT_IMPLEMENTED;
}


EXTERN_C NTSTATUS APIENTRY
D3DKMTOpenSynchronizationObject(D3DKMT_OPENSYNCHRONIZATIONOBJECT *pData)
{
   LOG_UNSUPPORTED_ENTRYPOINT();
   return STATUS_NOT_IMPLEMENTED;
}


EXTERN_C NTSTATUS APIENTRY
D3DKMTDestroySynchronizationObject(CONST D3DKMT_DESTROYSYNCHRONIZATIONOBJECT *pData)
{
   LOG_UNSUPPORTED_ENTRYPOINT();
   return STATUS_NOT_IMPLEMENTED;
}


EXTERN_C NTSTATUS APIENTRY
D3DKMTWaitForSynchronizationObject(CONST D3DKMT_WAITFORSYNCHRONIZATIONOBJECT *pData)
{
   LOG_UNSUPPORTED_ENTRYPOINT();
   return STATUS_NOT_IMPLEMENTED;
}


EXTERN_C NTSTATUS APIENTRY
D3DKMTWaitForSynchronizationObject2(CONST D3DKMT_WAITFORSYNCHRONIZATIONOBJECT2 *pData)
{
   LoadKmtCallbacks();
   return g_kmt.WaitForSynchronizationObject2 ?
             g_kmt.WaitForSynchronizationObject2(pData) :
             STATUS_NOT_IMPLEMENTED;
}


EXTERN_C NTSTATUS APIENTRY
D3DKMTSignalSynchronizationObject(CONST D3DKMT_SIGNALSYNCHRONIZATIONOBJECT *pData)
{
   LOG_UNSUPPORTED_ENTRYPOINT();
   return STATUS_NOT_IMPLEMENTED;
}


EXTERN_C NTSTATUS APIENTRY
D3DKMTSignalSynchronizationObject2(CONST D3DKMT_SIGNALSYNCHRONIZATIONOBJECT2 *pData)
{
   LoadKmtCallbacks();
   return g_kmt.SignalSynchronizationObject2 ?
             g_kmt.SignalSynchronizationObject2(pData) :
             STATUS_NOT_IMPLEMENTED;
}


EXTERN_C NTSTATUS APIENTRY
D3DKMTLock(D3DKMT_LOCK *pData)
{
   LoadKmtCallbacks();
   return g_kmt.Lock ? g_kmt.Lock(pData) : STATUS_NOT_IMPLEMENTED;
}


EXTERN_C NTSTATUS APIENTRY
D3DKMTUnlock(CONST D3DKMT_UNLOCK *pData)
{
   LoadKmtCallbacks();
   return g_kmt.Unlock ? g_kmt.Unlock(pData) : STATUS_NOT_IMPLEMENTED;
}


EXTERN_C NTSTATUS APIENTRY
D3DKMTGetDisplayModeList(D3DKMT_GETDISPLAYMODELIST *pData)
{
   LoadKmtCallbacks();
   return g_kmt.GetDisplayModeList ?
             g_kmt.GetDisplayModeList(pData) :
             STATUS_NOT_IMPLEMENTED;
}


EXTERN_C NTSTATUS APIENTRY
D3DKMTSetDisplayMode(CONST D3DKMT_SETDISPLAYMODE *pData)
{
   LoadKmtCallbacks();
   return g_kmt.SetDisplayMode ?
             g_kmt.SetDisplayMode(pData) :
             STATUS_NOT_IMPLEMENTED;
}


EXTERN_C NTSTATUS APIENTRY
D3DKMTGetMultisampleMethodList(D3DKMT_GETMULTISAMPLEMETHODLIST *pData)
{
   LoadKmtCallbacks();
   return g_kmt.GetMultisampleMethodList ?
             g_kmt.GetMultisampleMethodList(pData) :
             STATUS_NOT_IMPLEMENTED;
}


EXTERN_C NTSTATUS APIENTRY
D3DKMTPresent(D3DKMT_PRESENT *pData)
{
   LoadKmtCallbacks();
   return g_kmt.Present ? g_kmt.Present(pData) : STATUS_NOT_IMPLEMENTED;
}


EXTERN_C NTSTATUS APIENTRY
D3DKMTRender(D3DKMT_RENDER *pData)
{
   LoadKmtCallbacks();
   return g_kmt.Render ? g_kmt.Render(pData) : STATUS_NOT_IMPLEMENTED;
}


EXTERN_C NTSTATUS APIENTRY
D3DKMTGetRuntimeData(CONST D3DKMT_GETRUNTIMEDATA *pData)
{
   LoadKmtCallbacks();
   return g_kmt.GetRuntimeData ?
             g_kmt.GetRuntimeData(pData) :
             STATUS_NOT_IMPLEMENTED;
}


EXTERN_C NTSTATUS APIENTRY
D3DKMTQueryAdapterInfo(CONST D3DKMT_QUERYADAPTERINFO *pData)
{
   LOG_ENTRYPOINT();

   if (!pData)
      return STATUS_INVALID_PARAMETER;

   switch (pData->Type) {
   case KMTQAITYPE_UMDRIVERPRIVATE:
      {
         LoadKmtCallbacks();
         if (g_kmt.QueryAdapterInfo) {
            NTSTATUS Status = g_kmt.QueryAdapterInfo(pData);
            if (NT_SUCCESS(Status))
               return Status;
            DebugPrintf("%s: forwarded UMDRIVERPRIVATE query failed 0x%08lx\n",
                        __func__, Status);
            return Status;
         }
         return STATUS_NOT_IMPLEMENTED;
      }
      break;
   case KMTQAITYPE_UMDRIVERNAME:
      {
         D3DKMT_UMDFILENAMEINFO *pResult =
               (D3DKMT_UMDFILENAMEINFO *)pData->pPrivateDriverData;
         if (pResult->Version != KMTUMDVERSION_DX9 &&
             pResult->Version != KMTUMDVERSION_DX10 &&
             pResult->Version != KMTUMDVERSION_DX11) {
            DebugPrintf("%s: unsupported UMD version (%u)\n",
                        __func__, pResult->Version);
            return STATUS_INVALID_PARAMETER;
         }
         HMODULE hModule = 0;
         BOOL bRet;
         DWORD dwRet;
         bRet = GetModuleHandleEx(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                                  (LPCTSTR)D3DKMTQueryAdapterInfo,
                                  &hModule);
         assert(bRet);
         dwRet = GetModuleFileNameW(hModule, pResult->UmdFileName, MAX_PATH);
         assert(dwRet);
         return STATUS_SUCCESS;
      }
      break;
   case KMTQAITYPE_CURRENTDISPLAYMODE:
      {
         LoadKmtCallbacks();
         return g_kmt.QueryAdapterInfo ?
                   g_kmt.QueryAdapterInfo(pData) :
                   STATUS_NOT_IMPLEMENTED;
      }
      break;
   case KMTQAITYPE_MODELIST:
      {
         LoadKmtCallbacks();
         return g_kmt.QueryAdapterInfo ?
                   g_kmt.QueryAdapterInfo(pData) :
                   STATUS_NOT_IMPLEMENTED;
      }
      break;
   case KMTQAITYPE_GETSEGMENTSIZE:
      {
         D3DKMT_SEGMENTSIZEINFO *pResult =
               (D3DKMT_SEGMENTSIZEINFO *)pData->pPrivateDriverData;
         pResult->DedicatedVideoMemorySize = 0;
         pResult->DedicatedSystemMemorySize = 0;
         pResult->SharedSystemMemorySize = 3ULL*1024ULL*1024ULL*1024ULL;
         return STATUS_SUCCESS;
      }
      break;
   case KMTQAITYPE_CHECKDRIVERUPDATESTATUS:
      {
         BOOL *pResult = (BOOL *)pData->pPrivateDriverData;
         *pResult = false;
         return STATUS_SUCCESS;
      }
   case KMTQAITYPE_DRIVERVERSION:
      {
         D3DKMT_DRIVERVERSION *pResult = (D3DKMT_DRIVERVERSION *)pData->pPrivateDriverData;
         *pResult = KMT_DRIVERVERSION_WDDM_1_0;
         return STATUS_SUCCESS;
      }
   case KMTQAITYPE_XBOX:
      {
         BOOL *pResult = (BOOL *)pData->pPrivateDriverData;
         *pResult = false;
         return STATUS_SUCCESS;
      }
   case KMTQAITYPE_PHYSICALADAPTERCOUNT:
      {
         UINT *pResult = (UINT *)pData->pPrivateDriverData;
         *pResult = 1;
         return STATUS_SUCCESS;
      }
   case KMTQAITYPE_PHYSICALADAPTERDEVICEIDS:
      ZeroMemory(pData->pPrivateDriverData, pData->PrivateDriverDataSize);
      return STATUS_SUCCESS;
   default:
      DebugPrintf("%s: unsupported query type (Type=%u, PrivateDriverDataSize=%u)\n",
                  __func__, pData->Type, pData->PrivateDriverDataSize);
      ZeroMemory(pData->pPrivateDriverData, pData->PrivateDriverDataSize);
      return STATUS_NOT_IMPLEMENTED;
   }
}


EXTERN_C NTSTATUS APIENTRY
D3DKMTOpenAdapterFromHdc(D3DKMT_OPENADAPTERFROMHDC *pData)
{
   LoadKmtCallbacks();
   return g_kmt.OpenAdapterFromHdc ? g_kmt.OpenAdapterFromHdc(pData) :
                                     STATUS_NOT_IMPLEMENTED;
}


EXTERN_C NTSTATUS APIENTRY
D3DKMTOpenAdapterFromGdiDisplayName(D3DKMT_OPENADAPTERFROMGDIDISPLAYNAME *pData)
{
   LoadKmtCallbacks();
   return g_kmt.OpenAdapterFromGdiDisplayName ?
             g_kmt.OpenAdapterFromGdiDisplayName(pData) :
             STATUS_NOT_IMPLEMENTED;
}


EXTERN_C NTSTATUS APIENTRY
D3DKMTOpenAdapterFromDeviceName(D3DKMT_OPENADAPTERFROMDEVICENAME *pData)
{
   LoadKmtCallbacks();
   return g_kmt.OpenAdapterFromDeviceName ?
             g_kmt.OpenAdapterFromDeviceName(pData) :
             STATUS_NOT_IMPLEMENTED;
}


EXTERN_C NTSTATUS APIENTRY
D3DKMTCloseAdapter(CONST D3DKMT_CLOSEADAPTER *pData)
{
   LoadKmtCallbacks();
   return g_kmt.CloseAdapter ? g_kmt.CloseAdapter(pData) :
                               STATUS_NOT_IMPLEMENTED;
}


EXTERN_C NTSTATUS APIENTRY
D3DKMTGetSharedPrimaryHandle(D3DKMT_GETSHAREDPRIMARYHANDLE *pData)
{
   LOG_UNSUPPORTED_ENTRYPOINT();
   return STATUS_NOT_IMPLEMENTED;
}


EXTERN_C NTSTATUS APIENTRY
D3DKMTEscape(CONST D3DKMT_ESCAPE *pData)
{
   D3DKMTCheckViogpuEscape(pData);
   LoadKmtCallbacks();
   return g_kmt.Escape ? g_kmt.Escape(pData) : STATUS_NOT_IMPLEMENTED;
}


EXTERN_C NTSTATUS APIENTRY
D3DKMTSetVidPnSourceOwner(CONST D3DKMT_SETVIDPNSOURCEOWNER *pData)
{
   LOG_UNSUPPORTED_ENTRYPOINT();
   return STATUS_NOT_IMPLEMENTED;
}


EXTERN_C NTSTATUS APIENTRY
D3DKMTSetVidPnSourceOwner1(CONST D3DKMT_SETVIDPNSOURCEOWNER1 *pData)
{
   LOG_UNSUPPORTED_ENTRYPOINT();
   return STATUS_NOT_IMPLEMENTED;
}


EXTERN_C NTSTATUS APIENTRY
D3DKMTGetPresentHistory(D3DKMT_GETPRESENTHISTORY *pData)
{
   LOG_UNSUPPORTED_ENTRYPOINT();
   return STATUS_NOT_IMPLEMENTED;
}


EXTERN_C NTSTATUS APIENTRY
D3DKMTGetPresentQueueEvent(D3DKMT_HANDLE hAdapter, HANDLE *pData)
{
   LOG_UNSUPPORTED_ENTRYPOINT();
   return STATUS_NOT_IMPLEMENTED;
}


EXTERN_C NTSTATUS APIENTRY
D3DKMTCreateOverlay(D3DKMT_CREATEOVERLAY *pData)
{
   LOG_UNSUPPORTED_ENTRYPOINT();
   return STATUS_NOT_IMPLEMENTED;
}


EXTERN_C NTSTATUS APIENTRY
D3DKMTUpdateOverlay(CONST D3DKMT_UPDATEOVERLAY *pData)
{
   LOG_UNSUPPORTED_ENTRYPOINT();
   return STATUS_NOT_IMPLEMENTED;
}


EXTERN_C NTSTATUS APIENTRY
D3DKMTFlipOverlay(CONST D3DKMT_FLIPOVERLAY *pData)
{
   LOG_UNSUPPORTED_ENTRYPOINT();
   return STATUS_NOT_IMPLEMENTED;
}


EXTERN_C NTSTATUS APIENTRY
D3DKMTDestroyOverlay(CONST D3DKMT_DESTROYOVERLAY *pData)
{
   LOG_UNSUPPORTED_ENTRYPOINT();
   return STATUS_NOT_IMPLEMENTED;
}


EXTERN_C NTSTATUS APIENTRY
D3DKMTWaitForVerticalBlankEvent(CONST D3DKMT_WAITFORVERTICALBLANKEVENT *pData)
{
   LoadKmtCallbacks();
   return g_kmt.WaitForVerticalBlankEvent ?
             g_kmt.WaitForVerticalBlankEvent(pData) :
             STATUS_NOT_IMPLEMENTED;
}


EXTERN_C NTSTATUS APIENTRY
D3DKMTSetGammaRamp(CONST D3DKMT_SETGAMMARAMP *pData)
{
   LOG_UNSUPPORTED_ENTRYPOINT();
   return STATUS_NOT_IMPLEMENTED;
}


EXTERN_C NTSTATUS APIENTRY
D3DKMTGetDeviceState(D3DKMT_GETDEVICESTATE *pData)
{
   LoadKmtCallbacks();
   return g_kmt.GetDeviceState ?
             g_kmt.GetDeviceState(pData) :
             STATUS_NOT_IMPLEMENTED;
}


EXTERN_C NTSTATUS APIENTRY
D3DKMTCreateDCFromMemory(D3DKMT_CREATEDCFROMMEMORY *pData)
{
   LOG_UNSUPPORTED_ENTRYPOINT();
   return STATUS_NOT_IMPLEMENTED;
}


EXTERN_C NTSTATUS APIENTRY
D3DKMTDestroyDCFromMemory(CONST D3DKMT_DESTROYDCFROMMEMORY *pData)
{
   LOG_UNSUPPORTED_ENTRYPOINT();
   return STATUS_NOT_IMPLEMENTED;
}


EXTERN_C NTSTATUS APIENTRY
D3DKMTSetContextSchedulingPriority(CONST D3DKMT_SETCONTEXTSCHEDULINGPRIORITY *pData)
{
   LOG_UNSUPPORTED_ENTRYPOINT();
   return STATUS_NOT_IMPLEMENTED;
}


EXTERN_C NTSTATUS APIENTRY
D3DKMTGetContextSchedulingPriority(D3DKMT_GETCONTEXTSCHEDULINGPRIORITY *pData)
{
   LOG_UNSUPPORTED_ENTRYPOINT();
   return STATUS_NOT_IMPLEMENTED;
}


EXTERN_C NTSTATUS APIENTRY
D3DKMTSetProcessSchedulingPriorityClass(HANDLE hProcess, D3DKMT_SCHEDULINGPRIORITYCLASS Priority)
{
   LOG_UNSUPPORTED_ENTRYPOINT();
   return STATUS_NOT_IMPLEMENTED;
}


EXTERN_C NTSTATUS APIENTRY
D3DKMTGetProcessSchedulingPriorityClass(HANDLE hProcess, D3DKMT_SCHEDULINGPRIORITYCLASS *pPriority)
{
   LOG_UNSUPPORTED_ENTRYPOINT();
   return STATUS_NOT_IMPLEMENTED;
}


EXTERN_C NTSTATUS APIENTRY
D3DKMTReleaseProcessVidPnSourceOwners(HANDLE hProcess)
{
   LOG_UNSUPPORTED_ENTRYPOINT();
   return STATUS_NOT_IMPLEMENTED;
}


EXTERN_C NTSTATUS APIENTRY
D3DKMTGetScanLine(D3DKMT_GETSCANLINE *pData)
{
   LOG_UNSUPPORTED_ENTRYPOINT();
   return STATUS_NOT_IMPLEMENTED;
}


EXTERN_C NTSTATUS APIENTRY
D3DKMTChangeSurfacePointer(CONST D3DKMT_CHANGESURFACEPOINTER *pData)
{
   LOG_UNSUPPORTED_ENTRYPOINT();
   return STATUS_NOT_IMPLEMENTED;
}


EXTERN_C NTSTATUS APIENTRY
D3DKMTSetQueuedLimit(CONST D3DKMT_SETQUEUEDLIMIT *pData)
{
   LOG_UNSUPPORTED_ENTRYPOINT();
   return STATUS_NOT_IMPLEMENTED;
}


EXTERN_C NTSTATUS APIENTRY
D3DKMTPollDisplayChildren(CONST D3DKMT_POLLDISPLAYCHILDREN *pData)
{
   LOG_UNSUPPORTED_ENTRYPOINT();
   return STATUS_NOT_IMPLEMENTED;
}


EXTERN_C NTSTATUS APIENTRY
D3DKMTInvalidateActiveVidPn(CONST D3DKMT_INVALIDATEACTIVEVIDPN *pData)
{
   LOG_UNSUPPORTED_ENTRYPOINT();
   return STATUS_NOT_IMPLEMENTED;
}


EXTERN_C NTSTATUS APIENTRY
D3DKMTCheckOcclusion(CONST D3DKMT_CHECKOCCLUSION *pData)
{
   LOG_UNSUPPORTED_ENTRYPOINT();
   return STATUS_NOT_IMPLEMENTED;
}


EXTERN_C NTSTATUS APIENTRY
D3DKMTWaitForIdle(CONST D3DKMT_WAITFORIDLE *pData)
{
   LOG_UNSUPPORTED_ENTRYPOINT();
   return STATUS_NOT_IMPLEMENTED;
}


EXTERN_C NTSTATUS APIENTRY
D3DKMTCheckMonitorPowerState(CONST D3DKMT_CHECKMONITORPOWERSTATE *pData)
{
   LOG_UNSUPPORTED_ENTRYPOINT();
   return STATUS_NOT_IMPLEMENTED;
}


EXTERN_C BOOLEAN APIENTRY
D3DKMTCheckExclusiveOwnership(VOID)
{
   LOG_UNSUPPORTED_ENTRYPOINT();
   return false;
}


EXTERN_C NTSTATUS APIENTRY
D3DKMTCheckVidPnExclusiveOwnership(CONST D3DKMT_CHECKVIDPNEXCLUSIVEOWNERSHIP *pData)
{
   LOG_UNSUPPORTED_ENTRYPOINT();
   return STATUS_NOT_IMPLEMENTED;
}


EXTERN_C NTSTATUS APIENTRY
D3DKMTSetDisplayPrivateDriverFormat(CONST D3DKMT_SETDISPLAYPRIVATEDRIVERFORMAT *pData)
{
   LOG_UNSUPPORTED_ENTRYPOINT();
   return STATUS_NOT_IMPLEMENTED;
}


EXTERN_C NTSTATUS APIENTRY
D3DKMTSharedPrimaryLockNotification(CONST D3DKMT_SHAREDPRIMARYLOCKNOTIFICATION *pData)
{
   LOG_UNSUPPORTED_ENTRYPOINT();
   return STATUS_NOT_IMPLEMENTED;
}


EXTERN_C NTSTATUS APIENTRY
D3DKMTSharedPrimaryUnLockNotification(CONST D3DKMT_SHAREDPRIMARYUNLOCKNOTIFICATION *pData)
{
   LOG_UNSUPPORTED_ENTRYPOINT();
   return STATUS_NOT_IMPLEMENTED;
}


EXTERN_C NTSTATUS APIENTRY
D3DKMTCreateKeyedMutex(D3DKMT_CREATEKEYEDMUTEX *pData)
{
   LOG_UNSUPPORTED_ENTRYPOINT();
   return STATUS_NOT_IMPLEMENTED;
}


EXTERN_C NTSTATUS APIENTRY
D3DKMTOpenKeyedMutex(D3DKMT_OPENKEYEDMUTEX *pData)
{
   LOG_UNSUPPORTED_ENTRYPOINT();
   return STATUS_NOT_IMPLEMENTED;
}


EXTERN_C NTSTATUS APIENTRY
D3DKMTDestroyKeyedMutex(CONST D3DKMT_DESTROYKEYEDMUTEX *pData)
{
   LOG_UNSUPPORTED_ENTRYPOINT();
   return STATUS_NOT_IMPLEMENTED;
}


EXTERN_C NTSTATUS APIENTRY
D3DKMTAcquireKeyedMutex(D3DKMT_ACQUIREKEYEDMUTEX *pData)
{
   LOG_UNSUPPORTED_ENTRYPOINT();
   return STATUS_NOT_IMPLEMENTED;
}


EXTERN_C NTSTATUS APIENTRY
D3DKMTReleaseKeyedMutex(D3DKMT_RELEASEKEYEDMUTEX *pData)
{
   LOG_UNSUPPORTED_ENTRYPOINT();
   return STATUS_NOT_IMPLEMENTED;
}


EXTERN_C NTSTATUS APIENTRY
D3DKMTConfigureSharedResource(CONST D3DKMT_CONFIGURESHAREDRESOURCE *pData)
{
   LOG_UNSUPPORTED_ENTRYPOINT();
   return STATUS_NOT_IMPLEMENTED;
}


EXTERN_C NTSTATUS APIENTRY
D3DKMTGetOverlayState(D3DKMT_GETOVERLAYSTATE *pData)
{
   LOG_UNSUPPORTED_ENTRYPOINT();
   return STATUS_NOT_IMPLEMENTED;
}


EXTERN_C NTSTATUS APIENTRY
D3DKMTCheckSharedResourceAccess(CONST D3DKMT_CHECKSHAREDRESOURCEACCESS *pData)
{
   LOG_UNSUPPORTED_ENTRYPOINT();
   return STATUS_NOT_IMPLEMENTED;
}
