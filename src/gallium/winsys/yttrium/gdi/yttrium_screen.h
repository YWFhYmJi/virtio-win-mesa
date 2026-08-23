/*
 * SPDX-FileCopyrightText: 2026 Ake Rehnman <ake.rehnman@gmail.com>
 * SPDX-License-Identifier: MPL-2.0
 */

#ifndef YTTRIUM_SCREEN_H
#define YTTRIUM_SCREEN_H

#include "yttrium_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

void
yttrium_init_screen_options(struct yttrium_screen *screen);

void
yttrium_log_screen_config_and_options(const struct yttrium_screen *screen);

void
yttrium_free_screen_options(struct yttrium_screen *screen);

const char *
yttrium_get_name(struct pipe_screen *screen);

const char *
yttrium_get_vendor(struct pipe_screen *screen);

const char *
yttrium_get_device_vendor(struct pipe_screen *screen);

void
yttrium_destroy_screen(struct pipe_screen *pscreen);

bool
yttrium_init_caps(struct yttrium_screen *screen);

void
yttrium_fence_reference(struct pipe_screen *screen,
                        struct pipe_fence_handle **ptr,
                        struct pipe_fence_handle *fence);

bool
yttrium_fence_finish(struct pipe_screen *screen,
                     struct pipe_context *ctx,
                     struct pipe_fence_handle *fence,
                     uint64_t timeout);

uint64_t
yttrium_get_timestamp(struct pipe_screen *screen);

#ifdef __cplusplus
}
#endif

#endif /* YTTRIUM_SCREEN_H */
