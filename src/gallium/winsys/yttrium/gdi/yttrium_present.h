/*
 * SPDX-FileCopyrightText: 2026 Ake Rehnman <ake.rehnman@gmail.com>
 * SPDX-License-Identifier: MPL-2.0
 */

#ifndef YTTRIUM_PRESENT_H
#define YTTRIUM_PRESENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pipe/p_state.h"

#ifdef __cplusplus
extern "C" {
#endif

struct pipe_context;
struct pipe_resource;
struct pipe_screen;
struct pipe_box;
struct yttrium_resource;

bool
yttrium_ensure_display_mapped(struct pipe_context *ctx,
                              struct yttrium_resource *res);

void
yttrium_format_indexed_path(char *dst,
                            size_t dst_size,
                            const char *path,
                            unsigned index);

void
yttrium_flush_frontbuffer(struct pipe_screen *pscreen,
                          struct pipe_context *ctx,
                          struct pipe_resource *resource,
                          unsigned level,
                          unsigned layer,
                          void *context_private,
                          unsigned nboxes,
                          struct pipe_box *boxes);

#ifdef __cplusplus
}
#endif

#endif /* YTTRIUM_PRESENT_H */
