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
 * DriverIncludes.h --
 *    Basic DDK includes for building the client driver.
 */

#ifndef DRIVER_INCLUDES_H
#define DRIVER_INCLUDES_H

#include "Debug.h"
#include "util/u_inlines.h"

#include <winddk_compat.h>

#define D3D10DDI_MINOR_HEADER_VERSION 2

/* Unfortunately WinDDK's d3d10umddi.h defines D3D10.x constants as global
 * const variables instead of preprocessor defines, causing LINK to fail due
 * to duplicate symbols. Include d3d10_1.h to avoid the issue.
 */
#include <d3d10_1.h>

#include <d3d10umddi.h>

#define D3D10UMD_DRIVER_BUFINFO_CB_SLOT 0
#define D3D10UMD_BUFINFO_RECORD_DWORDS 4
#define D3D10UMD_BUFINFO_SRV_RECORD_BASE 0
#define D3D10UMD_BUFINFO_UAV_RECORD_BASE PIPE_MAX_SHADER_SAMPLER_VIEWS
#define D3D10UMD_BUFINFO_RECORD_COUNT \
   (PIPE_MAX_SHADER_SAMPLER_VIEWS + PIPE_MAX_SHADER_IMAGES)
#define D3D10UMD_DRIVER_SAMPLE_INFO_RECORD D3D10UMD_BUFINFO_RECORD_COUNT
#define D3D10UMD_DRIVER_CB_RECORD_COUNT \
   (D3D10UMD_BUFINFO_RECORD_COUNT + 1)
#define D3D10UMD_BUFINFO_CB_DWORDS \
   (D3D10UMD_DRIVER_CB_RECORD_COUNT * D3D10UMD_BUFINFO_RECORD_DWORDS)
#define D3D10UMD_BUFINFO_CB_SIZE \
   (D3D10UMD_BUFINFO_CB_DWORDS * sizeof(uint32_t))

#endif   /* DRIVER_INCLUDES_H */
