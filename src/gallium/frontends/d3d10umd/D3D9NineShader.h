/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright 2011 Joakim Sindholt <opensource@zhasha.com>
 * Copyright 2026 Ake Rehnman
 */

#pragma once

#include <windows.h>
#include <winddk_compat.h>
#include <d3d9.h>
#include <stdint.h>
#include <stdbool.h>

#include "pipe/p_context.h"
#include "pipe/p_defines.h"
#include "pipe/p_state.h"
#include "util/u_debug.h"
#include "util/u_memory.h"
#include "util/compiler.h"
#include "tgsi/tgsi_ureg.h"

#ifndef unreachable
#define unreachable(msg) UNREACHABLE(msg)
#endif

#ifndef PIPE_SHADER_VERTEX
#define PIPE_SHADER_VERTEX MESA_SHADER_VERTEX
#endif
#ifndef PIPE_SHADER_FRAGMENT
#define PIPE_SHADER_FRAGMENT MESA_SHADER_FRAGMENT
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define DBG_SHADER (1u << 22)

#define DBG(fmt, ...) do { if (0) debug_printf(fmt, ##__VA_ARGS__); } while (0)
#define WARN(fmt, ...) do { if (0) debug_printf(fmt, ##__VA_ARGS__); } while (0)
#define ERR(fmt, ...) do { if (0) debug_printf(fmt, ##__VA_ARGS__); } while (0)
#define DUMP(...) do { if (0) debug_printf(__VA_ARGS__); } while (0)

#define user_error(x) (!(x))
#define user_assert(x, r) \
   do { \
      if (user_error(x)) \
         return r; \
   } while (0)

struct nine_range
{
   struct nine_range *next;
   int16_t bgn;
   int16_t end;
};

struct nine_lconstf
{
   struct nine_range *ranges;
   float *data;
};

#define NINE_DECLUSAGE_POSITION         0
#define NINE_DECLUSAGE_BLENDWEIGHT      1
#define NINE_DECLUSAGE_BLENDINDICES     2
#define NINE_DECLUSAGE_NORMAL           3
#define NINE_DECLUSAGE_TEXCOORD         4
#define NINE_DECLUSAGE_TANGENT          5
#define NINE_DECLUSAGE_BINORMAL         6
#define NINE_DECLUSAGE_COLOR            7
#define NINE_DECLUSAGE_POSITIONT        8
#define NINE_DECLUSAGE_PSIZE            9
#define NINE_DECLUSAGE_TESSFACTOR       10
#define NINE_DECLUSAGE_DEPTH            11
#define NINE_DECLUSAGE_FOG              12
#define NINE_DECLUSAGE_SAMPLE           13
#define NINE_DECLUSAGE_NONE             14
#define NINE_DECLUSAGE_COUNT            (NINE_DECLUSAGE_NONE + 1)
#define NINE_DECLUSAGE_i(declusage, n) \
   (NINE_DECLUSAGE_##declusage + (n) * NINE_DECLUSAGE_COUNT)

#define NINE_MAX_CONST_F_PS3 224
#define NINE_MAX_CONST_F   256
#define NINE_MAX_CONST_I   16
#define NINE_MAX_CONST_B   16
#define NINE_MAX_CONST_SWVP_SPE_OFFSET 3564
#define NINE_MAX_CONST_VS_SPE_OFFSET \
   (NINE_MAX_CONST_F + (NINE_MAX_CONST_I + NINE_MAX_CONST_B / 4))
#define NINE_MAX_CONST_VS_SPE 11
#define NINE_MAX_CONST_ALL_VS \
   (NINE_MAX_CONST_VS_SPE_OFFSET + NINE_MAX_CONST_VS_SPE)
#define NINE_MAX_CONST_PS_SPE_OFFSET \
   (NINE_MAX_CONST_F_PS3 + (NINE_MAX_CONST_I + NINE_MAX_CONST_B / 4))
#define NINE_MAX_CONST_PS_SPE 15
#define NINE_MAX_CONST_ALL_PS \
   (NINE_MAX_CONST_PS_SPE_OFFSET + NINE_MAX_CONST_PS_SPE)
#define NINE_MAX_SAMPLERS_PS 16
#define NINE_MAX_SAMPLERS_VS 4
#define NINE_MAX_SAMPLERS 21

#define D3D9_NINE_POINT_SIZE_GENERIC 15

#define NINED3DSTT_1D_TOKEN (1u << D3DSP_TEXTURETYPE_SHIFT)
#define NINED3DSTT_TOKEN_TO_TYPE(token) \
   (((uint32_t)(token) & D3DSP_TEXTURETYPE_MASK) >> D3DSP_TEXTURETYPE_SHIFT)

enum nine_sm1_sampler_type {
   NINED3DSTT_UNKNOWN = NINED3DSTT_TOKEN_TO_TYPE(D3DSTT_UNKNOWN),
   NINED3DSTT_1D = NINED3DSTT_TOKEN_TO_TYPE(NINED3DSTT_1D_TOKEN),
   NINED3DSTT_2D = NINED3DSTT_TOKEN_TO_TYPE(D3DSTT_2D),
   NINED3DSTT_CUBE = NINED3DSTT_TOKEN_TO_TYPE(D3DSTT_CUBE),
   NINED3DSTT_VOLUME = NINED3DSTT_TOKEN_TO_TYPE(D3DSTT_VOLUME),
};

#define NINE_CONST_I_BASE(nconstf) ((nconstf) * 4 * sizeof(float))
#define NINE_CONST_B_BASE(nconstf) \
   ((nconstf) * 4 * sizeof(float) + NINE_MAX_CONST_I * 4 * sizeof(int))

struct NineDevice9
{
   struct pipe_screen *screen;
   struct pipe_screen *screen_sw;
   uint16_t max_vs_const_f;
   struct {
      bool always_output_pointsize;
      bool shader_emulate_features;
   } driver_caps;
};

struct NineVertexDeclaration9;

struct nine_vs_output_info
{
   BYTE output_semantic;
   int output_semantic_index;
   int mask;
   int output_index;
};

struct nine_shader_constant_combination
{
   struct nine_shader_constant_combination *next;
   int const_i[NINE_MAX_CONST_I][4];
   BOOL const_b[NINE_MAX_CONST_B];
};

struct nine_shader_info
{
   unsigned type;
   uint8_t version;
   const DWORD *byte_code;
   DWORD byte_size;
   void *cso;
   uint16_t input_map[PIPE_MAX_ATTRIBS];
   uint8_t num_inputs;
   bool position_t;
   bool force_position_t;
   bool point_size;
   bool uses_face;
   float point_size_min;
   float point_size_max;
   uint32_t sampler_ps1xtypes;
   uint64_t sampler_type_overrides;
   uint16_t sampler_type_override_mask;
   uint16_t sampler_mask;
   uint16_t sampler_mask_shadow;
   uint8_t sampler_targets[NINE_MAX_SAMPLERS];
   uint8_t rt_mask;
   uint8_t fog_enable;
   uint8_t runtime_fixed_function;
   uint8_t fog_mode;
   uint8_t zfog;
   uint8_t force_color_in_centroid;
   uint8_t color_flatshade;
   uint8_t projected;
   uint16_t fetch4;
   uint16_t fetch4_ati1;
   uint16_t fetch4_projected_fallback;
   uint8_t alpha_test_emulation;
   uint8_t clip_plane_emulation;
   bool emulate_features;
   unsigned const_i_base;
   unsigned const_b_base;
   unsigned const_used_size;
   bool int_slots_used[NINE_MAX_CONST_I];
   bool bool_slots_used[NINE_MAX_CONST_B];
   unsigned const_float_slots;
   unsigned const_int_slots;
   unsigned const_bool_slots;
   unsigned *const_ranges;
   struct nine_lconstf lconstf;
   uint8_t bumpenvmat_needed;
   struct {
      struct nine_shader_constant_combination *c_combination;
      bool (*int_const_added)[NINE_MAX_CONST_I];
      bool (*bool_const_added)[NINE_MAX_CONST_B];
   } add_constants_defs;
   bool swvp_on;
   bool process_vertices;
   struct NineVertexDeclaration9 *vdecl_out;
   struct pipe_stream_output_info so;
};

uint16_t nine_d3d9_to_nine_declusage(unsigned usage, unsigned index);

void *nine_create_shader_with_so_and_destroy(
   struct ureg_program *p, struct pipe_context *pipe,
   const struct pipe_stream_output_info *so);

HRESULT nine_translate_shader(struct NineDevice9 *device,
                              struct nine_shader_info *info,
                              struct pipe_context *pipe);

static inline void
NineVertexDeclaration9_FillStreamOutputInfo(
   struct NineVertexDeclaration9 *This,
   struct nine_vs_output_info *ShaderOutputsInfo,
   unsigned numOutputs,
   struct pipe_stream_output_info *so)
{
   (void)This;
   (void)ShaderOutputsInfo;
   (void)numOutputs;
   memset(so, 0, sizeof(*so));
}

static inline unsigned
pipe_comp_to_tgsi_opposite(BYTE flags)
{
   switch (flags) {
   case PIPE_FUNC_GREATER: return TGSI_OPCODE_SLE;
   case PIPE_FUNC_EQUAL: return TGSI_OPCODE_SNE;
   case PIPE_FUNC_GEQUAL: return TGSI_OPCODE_SLT;
   case PIPE_FUNC_LESS: return TGSI_OPCODE_SGE;
   case PIPE_FUNC_NOTEQUAL: return TGSI_OPCODE_SEQ;
   case PIPE_FUNC_LEQUAL: return TGSI_OPCODE_SGT;
   default: return TGSI_OPCODE_SGT;
   }
}

#ifdef __cplusplus
}
#endif
