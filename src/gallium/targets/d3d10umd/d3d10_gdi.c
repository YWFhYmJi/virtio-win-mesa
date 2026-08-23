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
 *
 **************************************************************************/


#include "gdikmt/gdikmt.h"
#include "util/u_debug.h"
#include "target-helpers/inline_debug_helper.h"
#include "llvmpipe/lp_public.h"
#include "softpipe/sp_public.h"
#include "sw/gdi/gdi_sw_winsys.h"
#include "virgl/gdi/virgl_gdi_public.h"
#ifdef GALLIUM_YTTRIUM
#include "yttrium/gdi/yttrium_gdi_public.h"
#include "yttrium/gdi/yttrium_trace.h"
#endif

#include "winddk_compat.h"
#include <d3dkmthk.h>
#include <stdlib.h>

extern struct pipe_screen *
d3d10_create_screen(struct gdikmt_device* device);

#if defined(GALLIUM_YTTRIUM) && defined(_MSC_VER)
static void
d3d10_disable_crt_error_dialogs(void)
{
   _set_error_mode(_OUT_TO_STDERR);
   _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
}
#endif

static HDC
d3d10_gdi_acquire_hdc(void *winsys_drawable_handle) {
   D3DKMT_PRESENT *pPresentInfo = (D3DKMT_PRESENT *)winsys_drawable_handle;

   HWND hWnd = pPresentInfo->hWindow;
   return GetDC(hWnd);
}

static void
d3d10_gdi_release_hdc(void *winsys_drawable_handle, HDC hDC) {
   D3DKMT_PRESENT *pPresentInfo = (D3DKMT_PRESENT *)winsys_drawable_handle;

   HWND hWnd = pPresentInfo->hWindow;
   ReleaseDC(hWnd, hDC);
}

static void
d3d10_trace_screen_driver_selection(struct gdikmt_device *device,
                                    const char *default_driver,
                                    const char *driver)
{
#ifdef GALLIUM_YTTRIUM
   const char *log_default_driver = default_driver ? default_driver : "(null)";
   const char *log_driver = driver ? driver : "(null)";
   bool config_loaded = false;
   bool config_found = false;
   const char *config_path = NULL;
   unsigned config_entries = 0;
   const char *env_driver = debug_get_option("GALLIUM_DRIVER", NULL);
   const char *log_env_driver = env_driver ? env_driver : "(null)";
   bool etw_log =
      yttrium_gdi_debug_get_bool_option("D3D10UMD_YTTRIUM_ETW_LOG", true);
   DWORD pid = GetCurrentProcessId();
   DWORD tid = GetCurrentThreadId();

   yttrium_gdi_debug_get_config_status(&config_loaded, &config_found,
                                       &config_path, &config_entries);
   const char *log_config_path = config_path ? config_path : "(null)";

   yttrium_gdi_user_logf("d3d10umd: create_screen select pid=%lu tid=%lu default_driver=%s selected_driver=%s env_driver=%s config_loaded=%u config_found=%u config_entries=%u config_path=%s device=%p\n",
                         (unsigned long)pid,
                         (unsigned long)tid,
                         log_default_driver,
                         log_driver,
                         log_env_driver,
                         config_loaded ? 1 : 0,
                         config_found ? 1 : 0,
                         config_entries,
                         log_config_path,
                         device);

   yttrium_trace_init(etw_log, false);
   yttrium_trace_debug_stringf("d3d10umd: create_screen select pid=%lu tid=%lu default_driver=%s selected_driver=%s env_driver=%s config_loaded=%u config_found=%u config_entries=%u config_path=%s device=%p",
                               (unsigned long)pid,
                               (unsigned long)tid,
                               log_default_driver,
                               log_driver,
                               log_env_driver,
                               config_loaded ? 1 : 0,
                               config_found ? 1 : 0,
                               config_entries,
                               log_config_path,
                               device);
   yttrium_trace_shutdown();
#else
   (void)device;
   (void)default_driver;
   (void)driver;
#endif
}

struct pipe_screen *
d3d10_create_screen(struct gdikmt_device* device)
{
   const char *default_driver;
   const char *driver;
   struct pipe_screen *screen = NULL;
   struct sw_winsys *winsys;

#if defined(GALLIUM_YTTRIUM) && defined(_MSC_VER)
   d3d10_disable_crt_error_dialogs();
#endif

#ifdef GALLIUM_YTTRIUM
   default_driver = "yttrium";
#elif defined(GALLIUM_VIRGL)
   default_driver = "virgl";
#elif defined(GALLIUM_LLVMPIPE)
   default_driver = "llvmpipe";
#else
   default_driver = "softpipe";
#endif

   driver = debug_get_option("GALLIUM_DRIVER", default_driver);
   d3d10_trace_screen_driver_selection(device, default_driver, driver);

#ifdef GALLIUM_VIRGL
   if (strcmp(driver, "virgl") == 0) {
#ifdef GALLIUM_YTTRIUM
      yttrium_gdi_user_logf("d3d10umd: create_screen using virgl device=%p\n",
                            device);
#endif
      return virgl_gdi_screen_create(device);
   }
#endif

#ifdef GALLIUM_YTTRIUM
   if (strcmp(driver, "yttrium") == 0) {
      yttrium_gdi_user_logf("d3d10umd: create_screen using yttrium device=%p\n",
                            device);
      return yttrium_gdi_screen_create(device);
   }
#endif

   winsys = gdi_create_sw_winsys(d3d10_gdi_acquire_hdc, d3d10_gdi_release_hdc);
   if(!winsys)
      goto no_winsys;

#ifdef GALLIUM_LLVMPIPE
   if (strcmp(driver, "llvmpipe") == 0) {
      screen = llvmpipe_create_screen( winsys );
   }
#else
   (void)driver;
#endif

#ifdef GALLIUM_SOFTPIPE
   if (screen == NULL) {
      screen = softpipe_create_screen( winsys );
   }
#endif

   if (screen == NULL)
      goto no_screen;

   return debug_screen_wrap( screen );

no_screen:
   winsys->destroy(winsys);
no_winsys:
   return NULL;
}
