/*
 * SPDX-FileCopyrightText: 2026 Ake Rehnman <ake.rehnman@gmail.com>
 * SPDX-License-Identifier: MPL-2.0
 */

#include <windows.h>
#include <winternl.h>

#include "util/u_memory.h"

#include "yttrium_gdi_public.h"
#include "yttrium_context.h"
#include "yttrium_internal.h"
#include "yttrium_options.h"
#include "yttrium_present.h"
#include "yttrium_resource.h"
#include "yttrium_screen.h"
#include "yttrium_trace.h"
#include "yttrium_venus.h"

#include <d3dkmthk.h>

#define VIRTGPU_DRM_CAPSET_VENUS 4

struct pipe_screen *
yttrium_gdi_screen_create(struct gdikmt_device *device)
{
   struct yttrium_screen *screen;
   NTSTATUS status;

   if (!device)
      return NULL;

   screen = CALLOC_STRUCT(yttrium_screen);
   if (!screen)
      return NULL;

   screen->device = device;
   yttrium_init_screen_options(screen);
   bool etw_log =
      yttrium_gdi_debug_get_bool_option("D3D10UMD_YTTRIUM_ETW_LOG", false);
   yttrium_trace_init(etw_log, false);

   status = device->queryAdapterInfo(device, KMTQAITYPE_UMDRIVERPRIVATE,
                                     &screen->adapter_info,
                                     sizeof(screen->adapter_info));
   if (!NT_SUCCESS(status)) {
      YTTRIUM_ERROR("yttrium: D3DKMTQueryAdapterInfo failed status=0x%lx\n",
                    status);
      yttrium_trace_shutdown();
      yttrium_free_screen_options(screen);
      FREE(screen);
      return NULL;
   }

   YTTRIUM_LOG("yttrium: adapter flags 3d=%u blob=%u host_visible=%u ctx_init=%u capset_fix=%u capsets=0x%llx\n",
                screen->adapter_info.Flags.Supports3d,
                screen->adapter_info.Flags.has_resource_blob,
                screen->adapter_info.Flags.has_host_visible,
                screen->adapter_info.Flags.has_context_init,
                screen->adapter_info.Flags.has_capset_query_fix,
                (unsigned long long)screen->adapter_info.SupportedCapsetIDs);
   yttrium_log_screen_config_and_options(screen);

   if (screen->adapter_info.IamVioGPU != VIOGPU_IAM ||
       !screen->adapter_info.Flags.Supports3d ||
       !screen->adapter_info.Flags.has_resource_blob) {
      YTTRIUM_ERROR("yttrium: adapter is not a blob-capable viogpu 3D adapter\n");
      yttrium_trace_shutdown();
      yttrium_free_screen_options(screen);
      FREE(screen);
      return NULL;
   }

   if (!(screen->adapter_info.SupportedCapsetIDs &
         (1ull << VIRTGPU_DRM_CAPSET_VENUS))) {
      YTTRIUM_ERROR("yttrium: Venus capset is not available on this adapter\n");
      yttrium_trace_shutdown();
      yttrium_free_screen_options(screen);
      FREE(screen);
      return NULL;
   }

   screen->venus = yttrium_venus_create(device);
   if (!screen->venus) {
      YTTRIUM_ERROR("yttrium: failed to create Venus transport state\n");
      yttrium_trace_shutdown();
      yttrium_free_screen_options(screen);
      FREE(screen);
      return NULL;
   }

   yttrium_trace_screen_create(screen->adapter_info.Flags.Supports3d,
                               screen->adapter_info.Flags.has_resource_blob,
                               screen->adapter_info.Flags.has_host_visible,
                               0,
                               0,
                               0,
                               0,
                               0);

   screen->base.destroy = yttrium_destroy_screen;
   screen->base.get_name = yttrium_get_name;
   screen->base.get_vendor = yttrium_get_vendor;
   screen->base.get_device_vendor = yttrium_get_device_vendor;
   screen->base.get_timestamp = yttrium_get_timestamp;
   screen->base.context_create = yttrium_context_create;
   screen->base.is_format_supported = yttrium_is_format_supported;
   screen->base.resource_create = yttrium_resource_create;
   screen->base.resource_from_handle = yttrium_resource_from_handle;
   screen->base.resource_destroy = yttrium_resource_destroy;
   screen->base.resource_get_handle = yttrium_resource_get_handle;
   screen->base.flush_frontbuffer = yttrium_flush_frontbuffer;
   screen->base.fence_reference = yttrium_fence_reference;
   screen->base.fence_finish = yttrium_fence_finish;

   yttrium_resource_init_screen(screen);
   yttrium_init_caps(screen);

   YTTRIUM_LOG("d3d10umd: using yttrium Gallium driver\n");
   return &screen->base;
}
