/*
 * SPDX-FileCopyrightText: 2026 Ake Rehnman <ake.rehnman@gmail.com>
 * SPDX-License-Identifier: MPL-2.0
 */

#include "yttrium_screen.h"

#include <string.h>

#include "compiler/glsl_types.h"
#include "util/u_math.h"
#include "util/u_memory.h"

#include "yttrium_gdi_public.h"
#include "yttrium_options.h"
#include "yttrium_resource.h"
#include "yttrium_shader.h"
#include "yttrium_trace.h"
#include "yttrium_venus.h"

static char *
yttrium_dup_option_string(const char *value)
{
   if (!value || !value[0])
      return NULL;

   const size_t len = strlen(value) + 1;
   char *copy = MALLOC(len);
   if (copy)
      memcpy(copy, value, len);
   return copy;
}

void
yttrium_init_screen_options(struct yttrium_screen *screen)
{
   screen->capture_textured_draw_path =
      yttrium_dup_option_string(
         yttrium_gdi_debug_get_option(
            "D3D10UMD_YTTRIUM_CAPTURE_TEXTURED_DRAW", NULL));
   int64_t capture_limit =
      yttrium_gdi_debug_get_num_option(
         "D3D10UMD_YTTRIUM_CAPTURE_TEXTURED_DRAW_LIMIT", 1);
   screen->capture_textured_draw_limit =
      capture_limit > 0 ? (unsigned)MIN2(capture_limit, 1024) : 0;

   screen->capture_skipped_draw_path =
      yttrium_dup_option_string(
         yttrium_gdi_debug_get_option(
            "D3D10UMD_YTTRIUM_CAPTURE_SKIPPED_DRAW", NULL));
   int64_t capture_skipped_limit =
      yttrium_gdi_debug_get_num_option(
         "D3D10UMD_YTTRIUM_CAPTURE_SKIPPED_DRAW_LIMIT", 0);
   screen->capture_skipped_draw_limit =
      capture_skipped_limit > 0 ?
         (unsigned)MIN2(capture_skipped_limit, 1024) : 0;
}

void
yttrium_log_screen_config_and_options(const struct yttrium_screen *screen)
{
   bool config_found = false;
   const char *config_path = NULL;
   unsigned config_entry_count = 0;

   yttrium_gdi_debug_get_config_status(NULL, &config_found, &config_path,
                                       &config_entry_count);
   YTTRIUM_LOG("yttrium: config file %s entries=%u\n",
                config_found && config_path ? config_path : "<none>",
                config_entry_count);

   YTTRIUM_LOG("yttrium: options present=KMD capture_textured_draw=%s capture_textured_draw_limit=%u capture_skipped_draw=%s capture_skipped_draw_limit=%u\n",
                screen->capture_textured_draw_path ?
                   screen->capture_textured_draw_path : "<none>",
                screen->capture_textured_draw_limit,
                screen->capture_skipped_draw_path ?
                   screen->capture_skipped_draw_path : "<none>",
                screen->capture_skipped_draw_limit);
}

void
yttrium_free_screen_options(struct yttrium_screen *screen)
{
   FREE(screen->capture_textured_draw_path);
   FREE(screen->capture_skipped_draw_path);
   screen->capture_textured_draw_path = NULL;
   screen->capture_skipped_draw_path = NULL;
}

const char *
yttrium_get_name(struct pipe_screen *screen)
{
   return "yttrium";
}

const char *
yttrium_get_vendor(struct pipe_screen *screen)
{
   return "Mesa";
}

const char *
yttrium_get_device_vendor(struct pipe_screen *screen)
{
   return "virtio-gpu";
}

bool
yttrium_gdi_screen_supports_logic_op(struct pipe_screen *pscreen)
{
   if (!pscreen || !pscreen->get_name ||
       strcmp(pscreen->get_name(pscreen), "yttrium") != 0)
      return false;

   struct yttrium_screen *screen = yttrium_screen(pscreen);
   return yttrium_venus_logic_op_enabled(screen->venus);
}

void
yttrium_destroy_screen(struct pipe_screen *pscreen)
{
   struct yttrium_screen *screen = yttrium_screen(pscreen);

   /* The ordered-upload pool owns live Venus resources and must release them
    * while the backend and KMD device are still available. */
   yttrium_resource_cleanup_screen(screen);
   yttrium_venus_destroy(screen->venus);
   if (screen->glsl_type_singleton_ref)
      glsl_type_singleton_decref();
   yttrium_trace_shutdown();
   yttrium_free_screen_options(screen);
   FREE(screen);
}

bool
yttrium_init_caps(struct yttrium_screen *screen)
{
   struct pipe_caps caps;
   memset(&caps, 0, sizeof(caps));

   caps.graphics = true;
   caps.device_reset_status_query = true;
   caps.npot_textures = true;
   caps.blend_equation_separate = true;
   caps.mixed_framebuffer_sizes = true;
   caps.max_render_targets = 1;
   caps.max_dual_source_render_targets =
      yttrium_venus_max_dual_source_render_targets(screen->venus);
   caps.max_texture_anisotropy =
      yttrium_venus_max_sampler_anisotropy(screen->venus);
   caps.anisotropic_filter = caps.max_texture_anisotropy > 1.0f;
   caps.max_texture_lod_bias =
      yttrium_venus_max_sampler_lod_bias(screen->venus);
   caps.max_texture_2d_size = 16384;
   caps.max_texture_3d_levels = 1;
   caps.max_texture_cube_levels = 1;
   caps.max_texture_array_layers = 1;
   caps.constant_buffer_offset_alignment = 16;
   caps.prefer_real_buffer_in_constbuf0 = true;
   caps.min_map_buffer_alignment = 64;
   caps.texture_buffer_offset_alignment = 16;
   caps.linear_image_pitch_alignment = 1;
   caps.linear_image_base_address_alignment = 1;
   caps.max_vertex_buffers = PIPE_MAX_ATTRIBS;
   caps.max_vertex_attrib_stride = 2048;
   caps.max_vertex_element_src_offset = 2047;
   caps.max_viewports = yttrium_venus_max_viewports(screen->venus);
   caps.fs_position_is_sysval = true;

   if (yttrium_shader_caps_enabled())
      caps.nir_samplers_as_deref = true;

   *(struct pipe_caps *)&screen->base.caps = caps;

   yttrium_trace_debug_stringf(
      "yttrium: shader gates caps=%u compile=%u module=%u pipeline_draw=%u",
      yttrium_shader_caps_enabled(),
      yttrium_shader_compile_enabled(),
      yttrium_shader_module_enabled(),
      yttrium_shader_pipeline_draw_enabled());

   if (yttrium_shader_caps_enabled()) {
      for (unsigned stage = 0; stage < MESA_SHADER_STAGES; stage++) {
         struct pipe_shader_caps *shader_caps =
            (struct pipe_shader_caps *)&screen->base.shader_caps[stage];

         if (stage != MESA_SHADER_VERTEX &&
             stage != MESA_SHADER_TESS_CTRL &&
             stage != MESA_SHADER_TESS_EVAL &&
             stage != MESA_SHADER_FRAGMENT)
            continue;

         shader_caps->max_instructions = 16384;
         shader_caps->max_alu_instructions = 16384;
         shader_caps->max_tex_instructions = 16384;
         shader_caps->max_tex_indirections = 16384;
         shader_caps->max_control_flow_depth = 32;
         shader_caps->max_inputs = PIPE_MAX_SHADER_INPUTS;
         shader_caps->max_outputs = PIPE_MAX_SHADER_OUTPUTS;
         shader_caps->max_const_buffer0_size = 64 * 1024;
         shader_caps->max_const_buffers = PIPE_MAX_CONSTANT_BUFFERS;
         shader_caps->max_temps = 256;
         shader_caps->max_texture_samplers = PIPE_MAX_SAMPLERS;
         shader_caps->max_sampler_views = PIPE_MAX_SHADER_SAMPLER_VIEWS;
         shader_caps->supported_irs =
            (1 << PIPE_SHADER_IR_TGSI) | (1 << PIPE_SHADER_IR_NIR);
         shader_caps->integers = true;
         shader_caps->tgsi_any_inout_decl_range = true;
      }

      yttrium_screen_init_shader_compiler(screen);
   }
   return true;
}

void
yttrium_fence_reference(struct pipe_screen *screen,
                        struct pipe_fence_handle **ptr,
                        struct pipe_fence_handle *fence)
{
   struct pipe_reference *old_ref = *ptr ? (struct pipe_reference *)*ptr : NULL;
   struct pipe_reference *new_ref = fence ? (struct pipe_reference *)fence : NULL;

   if (pipe_reference(old_ref, new_ref))
      FREE(*ptr);
   *ptr = fence;
}

bool
yttrium_fence_finish(struct pipe_screen *screen,
                     struct pipe_context *ctx,
                     struct pipe_fence_handle *fence,
                     uint64_t timeout)
{
   return true;
}

uint64_t
yttrium_get_timestamp(struct pipe_screen *screen)
{
   return 0;
}
