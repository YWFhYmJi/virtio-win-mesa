# Copyright 2026
# SPDX-License-Identifier: MIT

yttrium_gdi_c_args = []
if host_machine.system() == 'windows'
  yttrium_gdi_c_args += cc.get_supported_arguments('-fms-extensions')
endif

libyttriumgdi = static_library(
  'yttriumgdi',
  [
    'yttrium_capture.c',
    'yttrium_context.c',
    'yttrium_draw.c',
    'yttrium_gdi_winsys.c',
    'yttrium_options.c',
    'yttrium_pipeline.c',
    'yttrium_present.c',
    'yttrium_resource.c',
    'yttrium_screen.c',
    'yttrium_shader.c',
    'yttrium_state.c',
    'yttrium_trace.c',
    'yttrium_venus.c',
    'yttrium_venus2.c',
    'yttrium_venus2_batch.c',
    'yttrium_venus2_diag.c',
    'yttrium_venus2_format.c',
    'yttrium_venus2_impl.c',
    'yttrium_venus2_policy.c',
    'yttrium_venus2_resource.c',
    'yttrium_venus2_ring.c',
    'yttrium_venus2_transport.c',
    '../../../drivers/zink/nir_to_spirv/nir_to_spirv.c',
    '../../../drivers/zink/nir_to_spirv/spirv_builder.c',
  ],
  include_directories : [
    include_directories('.'),
    include_directories('../../../drivers/zink/nir_to_spirv'),
    inc_include, inc_src, inc_mesa, inc_gallium, inc_gallium_aux,
    inc_gallium_drivers, inc_virtio, inc_winddk
  ],
  dependencies : [idep_mesautil, idep_nir],
  c_args : yttrium_gdi_c_args,
  gnu_symbol_visibility : 'hidden',
)

winsys_yttrium_gdi = declare_dependency(
  link_with : [libyttriumgdi],
)

if with_tests
  test(
    'yttrium_venus2_ring_seqno',
    executable(
      'yttrium_venus2_ring_seqno_test',
      files('tests/yttrium_venus2_ring_seqno_test.c'),
      include_directories : include_directories('.'),
    ),
    suite : ['yttrium'],
  )
endif
