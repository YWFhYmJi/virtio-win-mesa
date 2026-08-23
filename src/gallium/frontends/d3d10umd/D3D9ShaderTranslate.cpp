/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright 2026 Ake Rehnman
 */

#include "D3D9ShaderTranslate.h"

#include "D3D9NineShader.h"

#include "util/u_memory.h"

bool
D3D9ShaderHasRuntimeFixedFunctionComment(const D3D9Object *shader)
{
   if (!shader || shader->kind != D3D9_OBJECT_SHADER ||
       shader->size < 4 * sizeof(UINT) || (shader->size % sizeof(UINT)))
      return false;

   const UINT *tokens = (const UINT *)shader->data;
   const UINT token_count = (UINT)(shader->size / sizeof(UINT));

   for (UINT i = 1; i < token_count;) {
      const UINT token = tokens[i];
      if (token == 0x0000ffff)
         return false;
      if ((token & D3DSI_OPCODE_MASK) != D3DSIO_COMMENT)
         return false;

      const UINT size =
         (token & D3DSI_COMMENTSIZE_MASK) >> D3DSI_COMMENTSIZE_SHIFT;
      if (i + 1 + size > token_count)
         return false;

      if (size >= 2 &&
          tokens[i + 1] == D3D9FourCC('T', 'E', 'X', 'T') &&
          tokens[i + 2] == D3D9FourCC('$', 'F', 'F', '\0'))
         return true;

      i += 1 + size;
   }

   return false;
}

HRESULT
D3D9TranslateVertexShaderForState(D3D9Device *device, D3D9Object *shader,
                                  bool fog_enable, bool position_t,
                                  uint8_t clip_plane_mask)
{
   if (!device || !device->pipe || !device->screen || !shader ||
       shader->kind != D3D9_OBJECT_SHADER ||
       shader->size < sizeof(UINT) || (shader->size % sizeof(UINT)))
      return E_INVALIDARG;

   const UINT *tokens = (const UINT *)shader->data;
   const UINT version = tokens[0];
   if ((version >> 16) != 0xfffe)
      return E_INVALIDARG;

   NineDevice9 nine = {};
   nine.screen = device->screen;
   nine.screen_sw = device->screen;
   nine.max_vs_const_f = NINE_MAX_CONST_F;

   nine_shader_info info = {};
   info.type = PIPE_SHADER_VERTEX;
   info.byte_code = (const DWORD *)tokens;
   info.const_i_base = NINE_CONST_I_BASE(nine.max_vs_const_f) / 16;
   info.const_b_base = NINE_CONST_B_BASE(nine.max_vs_const_f) / 16;
   info.fog_enable = fog_enable ? 1 : 0;
   info.runtime_fixed_function =
      D3D9ShaderHasRuntimeFixedFunctionComment(shader) ? 1 : 0;
   info.force_position_t = position_t;
   info.clip_plane_emulation = clip_plane_mask;
   info.point_size_min = 0.0f;
   info.point_size_max = 8192.0f;

   const HRESULT hr = nine_translate_shader(&nine, &info, device->pipe);
   if (FAILED(hr))
      return hr;

   if (shader->translated_vs_cso)
      device->pipe->delete_vs_state(device->pipe, shader->translated_vs_cso);
   free(shader->translated_vs_const_ranges);
   free(shader->translated_vs_lconstf_ranges);
   free(shader->translated_vs_lconstf_data);

   shader->translated_vs_cso = info.cso;
   shader->translated_vs_key = (fog_enable ? 1u : 0u) |
                               (position_t ? 1u << 1 : 0u) |
                               ((UINT)clip_plane_mask << 2);
   shader->translated_vs_const_used_size = info.const_used_size;
   shader->translated_vs_const_ranges = info.const_ranges;
   shader->translated_vs_lconstf_ranges = info.lconstf.ranges;
   shader->translated_vs_lconstf_data = info.lconstf.data;
   shader->translated_vs_outputs_point_size = info.point_size;
   memcpy(shader->translated_vs_input_map, info.input_map,
          sizeof(shader->translated_vs_input_map));
   shader->translated_vs_num_inputs = info.num_inputs;
   shader->translated_vs_sampler_mask =
      info.sampler_mask & ((1u << NINE_MAX_SAMPLERS_VS) - 1u);
   memcpy(shader->translated_vs_sampler_targets, info.sampler_targets,
          sizeof(shader->translated_vs_sampler_targets));
   info.const_ranges = NULL;
   info.lconstf.data = NULL;
   info.lconstf.ranges = NULL;

   D3D9Tracef("translated D3D9 VS with Nine shader=%p version=%u.%u "
              "size=%zu byte_size=%u fog=%u position_t=%u clip_planes=0x%02x "
              "point_size=%u cso=%p\n",
              shader, (version >> 8) & 0xff, version & 0xff, shader->size,
              info.byte_size, fog_enable ? 1u : 0u, position_t ? 1u : 0u,
              clip_plane_mask, info.point_size ? 1u : 0u,
              shader->translated_vs_cso);
   return S_OK;
}

HRESULT
D3D9TranslateVertexShader(D3D9Device *device, D3D9Object *shader)
{
   return D3D9TranslateVertexShaderForState(device, shader, false, false, 0);
}

HRESULT
D3D9TranslatePixelShaderForState(D3D9Device *device, D3D9Object *shader,
                                 bool fog_enable, UINT fog_mode, bool zfog,
                                 uint8_t projected,
                                 uint32_t sampler_ps1xtypes,
                                 uint64_t sampler_type_overrides,
                                 uint16_t sampler_type_override_mask,
                                 uint16_t fetch4,
                                 uint16_t fetch4_ati1,
                                 uint16_t fetch4_projected_fallback)
{
   if (!device || !device->pipe || !device->screen || !shader ||
       shader->kind != D3D9_OBJECT_SHADER ||
       shader->size < sizeof(UINT) || (shader->size % sizeof(UINT)))
      return E_INVALIDARG;

   const UINT *tokens = (const UINT *)shader->data;
   const UINT version = tokens[0];
   if ((version >> 16) != 0xffff)
      return E_INVALIDARG;

   NineDevice9 nine = {};
   nine.screen = device->screen;
   nine.screen_sw = device->screen;
   nine.max_vs_const_f = NINE_MAX_CONST_F;

   nine_shader_info info = {};
   info.type = PIPE_SHADER_FRAGMENT;
   info.byte_code = (const DWORD *)tokens;
   info.const_i_base = NINE_CONST_I_BASE(NINE_MAX_CONST_F_PS3) / 16;
   info.const_b_base = NINE_CONST_B_BASE(NINE_MAX_CONST_F_PS3) / 16;
   info.fog_enable = fog_enable ? 1 : 0;
   info.fog_mode = fog_enable ? (uint8_t)fog_mode : D3DFOG_NONE;
   info.zfog = zfog ? 1 : 0;
   info.projected = projected;
   info.color_flatshade =
      device->render_states[D3DRS_SHADEMODE] == D3DSHADE_FLAT ? 1 : 0;
   info.sampler_ps1xtypes = sampler_ps1xtypes;
   info.sampler_type_overrides = sampler_type_overrides;
   info.sampler_type_override_mask = sampler_type_override_mask;
   info.fetch4 = fetch4;
   info.fetch4_ati1 = fetch4_ati1;
   info.fetch4_projected_fallback = fetch4_projected_fallback;
   info.alpha_test_emulation = PIPE_FUNC_ALWAYS;

   const HRESULT hr = nine_translate_shader(&nine, &info, device->pipe);
   if (FAILED(hr))
      return hr;

   if (shader->translated_ps_cso)
      device->pipe->delete_fs_state(device->pipe, shader->translated_ps_cso);
   free(shader->translated_ps_const_ranges);
   free(shader->translated_ps_lconstf_ranges);
   free(shader->translated_ps_lconstf_data);

   shader->translated_ps_cso = info.cso;
   shader->translated_ps_key =
      (fog_enable ? 1u : 0u) |
      ((fog_enable ? fog_mode : D3DFOG_NONE) << 1) |
      ((fog_enable && zfog ? 1u : 0u) << 3) |
      ((UINT)projected << 4) |
      (info.color_flatshade ? 1u << 12 : 0u) |
      (sampler_ps1xtypes << 13);
   shader->translated_ps_sampler_type_overrides = sampler_type_overrides;
   shader->translated_ps_sampler_type_override_mask = sampler_type_override_mask;
   shader->translated_ps_fetch4 = fetch4;
   shader->translated_ps_fetch4_ati1 = fetch4_ati1;
   shader->translated_ps_fetch4_projected_fallback = fetch4_projected_fallback;
   shader->translated_ps_const_used_size = info.const_used_size;
   shader->translated_ps_const_ranges = info.const_ranges;
   shader->translated_ps_lconstf_ranges = info.lconstf.ranges;
   shader->translated_ps_lconstf_data = info.lconstf.data;
   shader->translated_ps_uses_face = info.uses_face;
   memcpy(shader->translated_ps_input_map, info.input_map,
          sizeof(shader->translated_ps_input_map));
   shader->translated_ps_num_inputs = info.num_inputs;
   shader->translated_ps_sampler_mask = info.sampler_mask;
   memcpy(shader->translated_ps_sampler_targets, info.sampler_targets,
          sizeof(shader->translated_ps_sampler_targets));
   info.const_ranges = NULL;
   info.lconstf.data = NULL;
   info.lconstf.ranges = NULL;

   D3D9Tracef("translated D3D9 PS with Nine shader=%p version=%u.%u "
              "size=%zu byte_size=%u fog=%u mode=%u zfog=%u "
              "projected=0x%02x flatshade=%u ps1x_samplers=0x%04x "
              "sampler_overrides=0x%016llx/0x%04x fetch4=0x%04x "
              "ati1=0x%04x projected_fallback=0x%04x cso=%p\n",
              shader, (version >> 8) & 0xff, version & 0xff, shader->size,
              info.byte_size, fog_enable ? 1u : 0u, fog_mode,
              zfog ? 1u : 0u, projected, info.color_flatshade ? 1u : 0u,
              sampler_ps1xtypes,
              (unsigned long long)sampler_type_overrides,
              sampler_type_override_mask, fetch4, fetch4_ati1,
              fetch4_projected_fallback, shader->translated_ps_cso);
   return S_OK;
}

HRESULT
D3D9TranslatePixelShader(D3D9Device *device, D3D9Object *shader)
{
   return D3D9TranslatePixelShaderForState(device, shader, false,
                                          D3DFOG_NONE, true, 0, 0, 0, 0, 0,
                                          0, 0);
}
