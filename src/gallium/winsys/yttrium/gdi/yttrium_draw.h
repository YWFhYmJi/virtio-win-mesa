/*
 * SPDX-FileCopyrightText: 2026 Ake Rehnman <ake.rehnman@gmail.com>
 * SPDX-License-Identifier: MPL-2.0
 */

#ifndef YTTRIUM_DRAW_H
#define YTTRIUM_DRAW_H

#ifdef __cplusplus
extern "C" {
#endif

struct pipe_context;
struct pipe_draw_info;
struct pipe_draw_indirect_info;
struct pipe_draw_start_count_bias;

/* Diagnostic only: lets the RT stats sampler count frames.  See
 * yttrium_report_rt_stats.
 */
void
yttrium_rt_stats_frame_advance(void);

void
yttrium_draw_vbo(struct pipe_context *ctx,
                 const struct pipe_draw_info *info,
                 unsigned drawid_offset,
                 const struct pipe_draw_indirect_info *indirect,
                 const struct pipe_draw_start_count_bias *draws,
                 unsigned num_draws);

#ifdef __cplusplus
}
#endif

#endif /* YTTRIUM_DRAW_H */
