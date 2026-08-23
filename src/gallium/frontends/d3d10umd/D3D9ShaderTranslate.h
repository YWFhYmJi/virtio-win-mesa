/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright 2026 Ake Rehnman
 */

#pragma once

#include "D3D9Private.h"

bool D3D9ShaderHasRuntimeFixedFunctionComment(const D3D9Object *shader);

HRESULT D3D9TranslateVertexShader(D3D9Device *device, D3D9Object *shader);
HRESULT D3D9TranslatePixelShader(D3D9Device *device, D3D9Object *shader);
HRESULT D3D9TranslateVertexShaderForState(D3D9Device *device,
                                          D3D9Object *shader,
                                          bool fog_enable,
                                          bool position_t,
                                          uint8_t clip_plane_mask);
HRESULT D3D9TranslatePixelShaderForState(D3D9Device *device,
                                         D3D9Object *shader,
                                         bool fog_enable,
                                         UINT fog_mode,
                                         bool zfog,
                                         uint8_t projected,
                                         uint32_t sampler_ps1xtypes,
                                         uint64_t sampler_type_overrides,
                                         uint16_t sampler_type_override_mask,
                                         uint16_t fetch4,
                                         uint16_t fetch4_ati1,
                                         uint16_t fetch4_projected_fallback);
