/*
 * SPDX-FileCopyrightText: 2026 Ake Rehnman <ake.rehnman@gmail.com>
 * SPDX-License-Identifier: MPL-2.0
 */

#ifndef YTTRIUM_CONTEXT_H
#define YTTRIUM_CONTEXT_H

#include <stdbool.h>
#include <stdint.h>

#include "pipe/p_context.h"
#include "pipe/p_screen.h"
#include "pipe/p_state.h"

#ifdef __cplusplus
extern "C" {
#endif

void
yttrium_flush(struct pipe_context *ctx,
              struct pipe_fence_handle **fence,
              unsigned flags);

void
yttrium_resource_release(struct pipe_context *ctx,
                         struct pipe_resource *resource);

struct pipe_query *
yttrium_create_query(struct pipe_context *ctx, unsigned query_type,
                     unsigned index);

void
yttrium_destroy_query(struct pipe_context *ctx, struct pipe_query *query);

bool
yttrium_begin_query(struct pipe_context *ctx, struct pipe_query *query);

bool
yttrium_end_query(struct pipe_context *ctx, struct pipe_query *query);

bool
yttrium_get_query_result(struct pipe_context *ctx,
                         struct pipe_query *query,
                         bool wait,
                         union pipe_query_result *result);

void
yttrium_set_active_query_state(struct pipe_context *ctx, bool enable);

void
yttrium_record_draw_queries(struct pipe_context *ctx,
                            const struct pipe_draw_info *info,
                            const struct pipe_draw_indirect_info *indirect,
                            const struct pipe_draw_start_count_bias *draws,
                            unsigned num_draws);

struct pipe_context *
yttrium_context_create(struct pipe_screen *pscreen, void *priv, unsigned flags);

#ifdef __cplusplus
}
#endif

#endif /* YTTRIUM_CONTEXT_H */
