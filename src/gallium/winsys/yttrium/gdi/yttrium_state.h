/*
 * SPDX-FileCopyrightText: 2026 Ake Rehnman <ake.rehnman@gmail.com>
 * SPDX-License-Identifier: MPL-2.0
 */

#ifndef YTTRIUM_STATE_H
#define YTTRIUM_STATE_H

#include "compiler/shader_enums.h"
#include "pipe/p_context.h"
#include "pipe/p_state.h"

#ifdef __cplusplus
extern "C" {
#endif

void *
yttrium_create_vertex_elements_state(
   struct pipe_context *ctx,
   unsigned num_elements,
   const struct pipe_vertex_element *elements);

void
yttrium_bind_vertex_elements_state(struct pipe_context *ctx, void *state);

void
yttrium_delete_vertex_elements_state(struct pipe_context *ctx, void *state);

void
yttrium_set_vertex_buffers(struct pipe_context *ctx,
                           unsigned count,
                           const struct pipe_vertex_buffer *buffers);

void
yttrium_set_framebuffer_state(struct pipe_context *ctx,
                              const struct pipe_framebuffer_state *state);

void *
yttrium_create_rasterizer_state(struct pipe_context *ctx,
                                const struct pipe_rasterizer_state *state);

void
yttrium_bind_rasterizer_state(struct pipe_context *ctx, void *state);

void
yttrium_delete_rasterizer_state(struct pipe_context *ctx, void *state);

void *
yttrium_create_blend_state(struct pipe_context *ctx,
                           const struct pipe_blend_state *state);

void
yttrium_bind_blend_state(struct pipe_context *ctx, void *state);

void
yttrium_delete_blend_state(struct pipe_context *ctx, void *state);

void *
yttrium_create_dsa_state(
   struct pipe_context *ctx,
   const struct pipe_depth_stencil_alpha_state *state);

void
yttrium_bind_dsa_state(struct pipe_context *ctx, void *state);

void
yttrium_delete_dsa_state(struct pipe_context *ctx, void *state);

void
yttrium_set_stencil_ref(struct pipe_context *ctx,
                        const struct pipe_stencil_ref state);

void
yttrium_set_blend_color(struct pipe_context *ctx,
                        const struct pipe_blend_color *state);

void
yttrium_set_sample_mask(struct pipe_context *ctx, unsigned sample_mask);

void
yttrium_set_viewport_states(struct pipe_context *ctx,
                            unsigned start_slot,
                            unsigned num_viewports,
                            const struct pipe_viewport_state *states);

void
yttrium_set_scissor_states(struct pipe_context *ctx,
                           unsigned start_slot,
                           unsigned num_scissors,
                           const struct pipe_scissor_state *states);

void
yttrium_set_constant_buffer(struct pipe_context *ctx,
                            mesa_shader_stage shader,
                            uint index,
                            const struct pipe_constant_buffer *cb);

void *
yttrium_create_sampler_state(struct pipe_context *ctx,
                             const struct pipe_sampler_state *state);

void
yttrium_bind_sampler_states(struct pipe_context *ctx,
                            mesa_shader_stage shader,
                            unsigned start_slot,
                            unsigned num_samplers,
                            void **samplers);

void
yttrium_delete_sampler_state(struct pipe_context *ctx, void *state);

struct pipe_sampler_view *
yttrium_create_sampler_view(struct pipe_context *ctx,
                            struct pipe_resource *texture,
                            const struct pipe_sampler_view *state);

void
yttrium_set_sampler_views(struct pipe_context *ctx,
                          mesa_shader_stage shader,
                          unsigned start_slot,
                          unsigned num_views,
                          unsigned unbind_num_trailing_slots,
                          struct pipe_sampler_view **views);

void
yttrium_set_shader_images(struct pipe_context *ctx,
                          mesa_shader_stage shader,
                          unsigned start_slot,
                          unsigned count,
                          unsigned unbind_num_trailing_slots,
                          const struct pipe_image_view *images);

void
yttrium_sampler_view_destroy(struct pipe_context *ctx,
                             struct pipe_sampler_view *view);

#ifdef __cplusplus
}
#endif

#endif /* YTTRIUM_STATE_H */
